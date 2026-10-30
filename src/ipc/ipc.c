// SPDX-License-Identifier: GPL-2.0-only
/*
 * 调试接口的传输层：unix socket 上的行分隔 JSON。
 *
 * 这一层只管字节怎么进出：非阻塞 accept、按行切分、输出排队。
 * 命令说什么、JSON 长什么样全在 commands.c —— 两个方向的边界就是
 * woodland_ipc_dispatch()：进来一个字符串，出去一段 JSON。
 *
 * 实现上刻意"笨"：一行命令一行回应，没有长度头，`nc -U $WOODLAND_IPC`
 * 就能手敲。代价是要自己处理"对端连上却不读"，见下面 client_send。
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "ipc.h"
#include "util/json.h"
#include "woodland.h"

/* 单行命令上限：正常命令几十字节，留足余量给"手抖粘贴一堆东西" */
#define IPC_MAX_LINE 8192
/* 单连接待发数据的硬上限。正常响应远小于它；超过说明对端连上就不读，
 * 与其让队列无限涨，不如断开这个连接（调试接口丢一条连接是可接受的） */
#define IPC_MAX_PENDING (4 * 1024 * 1024)

struct woodland_ipc_client {
    struct wl_list link;
    struct woodland_server *server;
    int fd;
    struct wl_event_source *source;
    size_t len;
    char buf[IPC_MAX_LINE];
    /* 输出队列：out[out_off..out_len) 是还没发出去的字节 */
    char *out;
    size_t out_len;
    size_t out_cap;
    size_t out_off;
};

/* 尽量把队列里的字节写出去。
 * 返回 false 表示真错误（EPIPE/EBADF 之类），连接必须断开；
 * EAGAIN 不算错误——对端只是暂时不收，等 WRITABLE 再来。 */
static bool client_flush(struct woodland_ipc_client *client) {
    while (client->out_off < client->out_len) {
        ssize_t n = write(client->fd, client->out + client->out_off,
                          client->out_len - client->out_off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        client->out_off += (size_t)n;
    }
    /* 发完就复位，别让缓冲只涨不落 */
    client->out_off = 0;
    client->out_len = 0;
    return true;
}

/* 事件掩码二选一：有积压就只关心可写（顺带就是反压——不再读新命令），
 * 队列空了才回去读。两个都挂着会让"读进来又发不出去"无限套娃。 */
static void client_update_events(struct woodland_ipc_client *client) {
    if (!client->source) {
        return;
    }
    uint32_t mask = (client->out_off < client->out_len) ? WL_EVENT_WRITABLE
                                                        : WL_EVENT_READABLE;
    wl_event_source_fd_update(client->source, mask);
}

/* 排队并发出去。返回 false 表示连接必须断开。 */
static bool client_send(struct woodland_ipc_client *client,
                        const char *data, size_t len) {
    /* 回收已经发掉的头部 */
    if (client->out_off > 0) {
        memmove(client->out, client->out + client->out_off,
                client->out_len - client->out_off);
        client->out_len -= client->out_off;
        client->out_off = 0;
    }
    if (len > IPC_MAX_PENDING || client->out_len > IPC_MAX_PENDING - len) {
        wlr_log(WLR_ERROR, "ipc client is not reading, dropping it");
        return false;
    }
    if (client->out_len + len > client->out_cap) {
        size_t cap = client->out_cap ? client->out_cap : 4096;
        while (cap < client->out_len + len) {
            cap *= 2;
        }
        char *grown = realloc(client->out, cap);
        if (!grown) {
            return false;
        }
        client->out = grown;
        client->out_cap = cap;
    }
    memcpy(client->out + client->out_len, data, len);
    client->out_len += len;
    return client_flush(client);
}

static void client_destroy(struct woodland_ipc_client *client) {
    if (!client) {
        return;
    }
    if (client->source) {
        wl_event_source_remove(client->source);
        client->source = NULL;
    }
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    wl_list_remove(&client->link);
    free(client->out);
    free(client);
}

/* 处理一行命令，返回 false 表示连接不可用需要断开 */
static bool client_handle_line(struct woodland_ipc_client *client, char *line) {
    /* 去掉尾部空白（含 telnet/nc 带来的 \r） */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '
                       || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    char *start = line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start == '\0') {
        return true;   /* 空行当心跳，不回应 */
    }

    struct woodland_json out;
    woodland_json_init(&out);
    woodland_ipc_dispatch(client->server, start, &out);
    woodland_json_raw(&out, "\n");
    bool ok = client_send(client, woodland_json_data(&out),
                          woodland_json_len(&out));
    woodland_json_finish(&out);
    return ok;
}

/* 消费缓冲区里所有完整的行。返回 false 表示连接不可用。
 * 发出积压时会提前收手，剩下的半截留在 buf 里等下一次。 */
static bool client_process_lines(struct woodland_ipc_client *client) {
    size_t consumed = 0;
    while (consumed < client->len) {
        char *nl = memchr(client->buf + consumed, '\n',
                          client->len - consumed);
        if (!nl) {
            break;
        }
        *nl = '\0';
        size_t line_off = consumed;
        consumed = (size_t)(nl - client->buf) + 1;
        if (!client_handle_line(client, client->buf + line_off)) {
            return false;
        }
        if (client->out_off < client->out_len) {
            /* 上一条回应还没发完，先别解析下一条：一次只压一条响应，
             * 内存才有上界 */
            break;
        }
    }

    if (consumed > 0) {
        memmove(client->buf, client->buf + consumed, client->len - consumed);
        client->len -= consumed;
        client->buf[client->len] = '\0';
    }
    return true;
}

static int client_event(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct woodland_ipc_client *client = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        client_destroy(client);
        return 0;
    }

    if (mask & WL_EVENT_WRITABLE) {
        if (!client_flush(client)) {
            client_destroy(client);
            return 0;
        }
        if (client->out_off < client->out_len) {
            return 0;   /* 对端还是不收，继续等 */
        }
        /* 队列排空：把因为积压而中断的解析接上 */
        if (!client_process_lines(client)) {
            client_destroy(client);
            return 0;
        }
        client_update_events(client);
        return 0;
    }

    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    ssize_t n = read(client->fd, client->buf + client->len,
                     sizeof(client->buf) - client->len - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            return 0;
        }
        client_destroy(client);
        return 0;
    }
    client->len += (size_t)n;
    client->buf[client->len] = '\0';

    if (!client_process_lines(client)) {
        client_destroy(client);
        return 0;
    }

    if (client->len >= sizeof(client->buf) - 1) {
        /* 一直没等到换行：丢掉这一行，别让半条命令无限攒着 */
        static const char too_long[] =
            "{\"ok\":false,\"error\":\"line too long\"}\n";
        client->len = 0;
        client->buf[0] = '\0';
        if (!client_send(client, too_long, sizeof(too_long) - 1)) {
            client_destroy(client);
            return 0;
        }
    }

    client_update_events(client);
    return 0;
}

static int ipc_accept(int fd, uint32_t mask, void *data) {
    struct woodland_server *server = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            wlr_log(WLR_ERROR, "ipc accept failed: %s", strerror(errno));
        }
        return 0;
    }

    /* 关键：accept 出来的 fd 不会继承监听 socket 的 O_NONBLOCK，
     * 不显式设上就退化成阻塞写，对端不读时卡死事件循环 */
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        wlr_log(WLR_ERROR, "ipc: cannot set O_NONBLOCK: %s", strerror(errno));
        close(client_fd);
        return 0;
    }

    struct woodland_ipc_client *client = calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return 0;
    }
    client->server = server;
    client->fd = client_fd;
    client->source = wl_event_loop_add_fd(server->event_loop, client_fd,
                                         WL_EVENT_READABLE, client_event,
                                         client);
    if (!client->source) {
        close(client_fd);
        free(client);
        return 0;
    }
    wl_list_insert(&server->ipc_clients, &client->link);
    return 0;
}

const char *woodland_ipc_path(const struct woodland_server *server) {
    return (server && server->ipc_path) ? server->ipc_path : "";
}

bool woodland_ipc_init(struct woodland_server *server,
                       const char *wayland_socket) {
    if (!server || !server->event_loop) {
        return false;
    }
    wl_list_init(&server->ipc_clients);

    /* 与 wayland socket 同目录同名规则：一个合成器一个调试 socket，
     * 多实例（嵌套跑测试）不会互相踩 */
    char path[108];   /* sizeof(sockaddr_un.sun_path) */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int written;
    if (runtime && runtime[0]) {
        written = snprintf(path, sizeof(path), "%s/woodland-%s.sock", runtime,
                           wayland_socket ? wayland_socket : "wayland");
    } else {
        written = snprintf(path, sizeof(path), "/tmp/woodland-%d.sock",
                           (int)getpid());
    }
    if (written < 0 || (size_t)written >= sizeof(path)) {
        wlr_log(WLR_ERROR, "ipc socket path too long");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "ipc socket() failed: %s", strerror(errno));
        return false;
    }
    /* 非阻塞监听：poll 报可读到真正 accept 之间连接可能已被对端 RST，
     * 阻塞 accept 会在这个窗口里停住整个事件循环 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        wlr_log(WLR_ERROR, "ipc: cannot set O_NONBLOCK: %s", strerror(errno));
        close(fd);
        return false;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, (size_t)written + 1);

    /* 上一轮崩溃留下的 socket 文件会挡住 bind */
    unlink(path);
    /* 只允许本用户连接：调试接口能读到全部窗口标题 */
    mode_t old_umask = umask(0077);
    bool bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    umask(old_umask);
    if (!bound) {
        wlr_log(WLR_ERROR, "ipc bind %s failed: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, 8) < 0) {
        wlr_log(WLR_ERROR, "ipc listen failed: %s", strerror(errno));
        close(fd);
        unlink(path);
        return false;
    }

    server->ipc_fd = fd;
    server->ipc_path = strdup(path);
    server->ipc_source = wl_event_loop_add_fd(server->event_loop, fd,
                                             WL_EVENT_READABLE, ipc_accept,
                                             server);
    if (!server->ipc_source) {
        close(fd);
        server->ipc_fd = -1;
        free(server->ipc_path);
        server->ipc_path = NULL;
        unlink(path);
        return false;
    }

    setenv("WOODLAND_IPC", path, true);
    /* 命令清单不再硬编码在这条日志里：socket 上问一句 help 就是全量文档 */
    wlr_log(WLR_INFO, "debug IPC listening on %s (send 'help' for the "
                      "command list)", path);
    return true;
}

void woodland_ipc_finish(struct woodland_server *server) {
    if (!server) {
        return;
    }
    struct woodland_ipc_client *client;
    struct woodland_ipc_client *tmp;
    wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
        client_destroy(client);
    }
    if (server->ipc_source) {
        wl_event_source_remove(server->ipc_source);
        server->ipc_source = NULL;
    }
    if (server->ipc_fd >= 0) {
        close(server->ipc_fd);
        server->ipc_fd = -1;
    }
    if (server->ipc_path) {
        unlink(server->ipc_path);
        free(server->ipc_path);
        server->ipc_path = NULL;
    }
}
