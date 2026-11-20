// SPDX-License-Identifier: GPL-2.0-only
/*
 * wctl：和合成器调试接口对话的小客户端。
 *
 * 就做三件事：找到 socket、发一行命令、把回应打到 stdout。
 * 输出是原始 JSON（一行），所以 `wctl get_tree | jq .` 直接可用；
 * 也正因为不做美化，脚本里 `wctl get_focus | grep -o '"view":[0-9]*'`
 * 这种土办法照样能用。
 *
 * 找不到 socket 时的寻找顺序：
 *   1. -s/--socket
 *   2. $WOODLAND_IPC（合成器启动时导出）
 *   3. $XDG_RUNTIME_DIR 下最新的 woodland-*.sock
 */
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

/* dump_scene 允许 2000 个节点，正常一棵树也能到几百 KB，
 * 缓冲区要能装下最坏情况，否则只能拿到半截 JSON */
#define WCTL_MAX_REPLY (1024 * 1024)

static void usage(const char *name) {
    printf("Usage: %s [-s socket] [-t timeout_ms] [command]\n"
           "\n"
           "commands:\n"
           "  get_tree    windows, outputs, workspace, focus  (default)\n"
           "  get_focus   just the focus state\n"
           "  self_check  invariant checks\n"
           "  dump_scene  the wlr_scene tree\n"
           "  ping        liveness probe\n"
           "  get_config  active runtime configuration\n"
           "  reload_config reload configuration from file\n"
           "  pixel <x> <y>  sample pixel color & scene attribution\n"
           "  screenshot [--region x,y,w,h] [--expect #rrggbb] [--tolerance N]\n"
           "  wait <condition> [key=val...] [timeout=MS]\n"
           "\n"
           "write commands (only when woodland runs with --debug-ctl; see `help`):\n"
           "  focus_view <id> | close <id> | move <id> <x> <y> | resize <id> <w> <h>\n"
           "  maximize|fullscreen|minimize <id> <0|1> | workspace <name>\n"
           "  pointer_move <x> <y> | pointer_button <left|right|middle> <press|release|click>\n"
           "  inject_key <linux_keycode> <press|release|tap>\n"
           "\n"
           "aliases: tree, focus, check, scene, config, reload\n"
           "\n"
           "options:\n"
           "  -s, --socket PATH   debug socket (default: $WOODLAND_IPC)\n"
           "  -t, --timeout MS    connect/read timeout in ms (default 2000)\n",
           name);
}

/* 别名归一：手敲时懒得记下划线/命令全名 */
static const char *normalize(const char *cmd) {
    if (!cmd) {
        return "get_tree";
    }
    if (strcmp(cmd, "tree") == 0 || strcmp(cmd, "get-tree") == 0) {
        return "get_tree";
    }
    if (strcmp(cmd, "focus") == 0 || strcmp(cmd, "get-focus") == 0) {
        return "get_focus";
    }
    if (strcmp(cmd, "check") == 0 || strcmp(cmd, "self-check") == 0) {
        return "self_check";
    }
    if (strcmp(cmd, "scene") == 0 || strcmp(cmd, "dump-scene") == 0) {
        return "dump_scene";
    }
    if (strcmp(cmd, "config") == 0 || strcmp(cmd, "get-config") == 0) {
        return "get_config";
    }
    if (strcmp(cmd, "reload") == 0 || strcmp(cmd, "reload-config") == 0) {
        return "reload_config";
    }
    return cmd;
}

/* $XDG_RUNTIME_DIR 下 mtime 最新的 woodland-*.sock */
static bool find_latest(char *out, size_t out_len) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0]) {
        return false;
    }
    DIR *dir = opendir(runtime);
    if (!dir) {
        return false;
    }
    char best[512] = {0};
    struct timespec best_time = {0, 0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "woodland-", 9) != 0) {
            continue;
        }
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 5, ".sock") != 0) {
            continue;
        }
        char path[512];
        int written = snprintf(path, sizeof(path), "%s/%s", runtime,
                               entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;   /* 路径拼不下：不是我们要找的 socket */
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (st.st_mtim.tv_sec > best_time.tv_sec
            || (st.st_mtim.tv_sec == best_time.tv_sec
                && st.st_mtim.tv_nsec > best_time.tv_nsec)) {
            best_time = st.st_mtim;
            snprintf(best, sizeof(best), "%s", path);
        }
    }
    closedir(dir);
    if (!best[0]) {
        return false;
    }
    snprintf(out, out_len, "%s", best);
    return true;
}

/* 连不上的常见原因是合成器刚起来还没 bind，所以重试到超时为止 */
static int connect_with_retry(const char *path, int timeout_ms) {
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "wctl: socket path too long\n");
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int waited = 0;
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("wctl: socket");
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        if (waited >= timeout_ms) {
            fprintf(stderr, "wctl: cannot connect to %s: %s\n", path,
                    strerror(errno));
            return -1;
        }
        /* nanosleep 而不是 usleep：后者在 _POSIX_C_SOURCE=200809L 下不再声明 */
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
        nanosleep(&delay, NULL);
        waited += 50;
    }
}

static bool append_escaped_str(char *buf, size_t cap, int *pos, const char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '"' || s[i] == '\\') {
            if (*pos + 2 >= (int)cap) {
                return false;
            }
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = s[i];
        } else {
            if (*pos + 1 >= (int)cap) {
                return false;
            }
            buf[(*pos)++] = s[i];
        }
    }
    return true;
}

static bool append_arg(char *buf, size_t cap, int *pos, const char *arg,
                       bool *bad_newline) {
    if (strpbrk(arg, "\r\n") != NULL) {
        *bad_newline = true;
        return false;
    }
    if (*pos < 0 || *pos + 2 >= (int)cap) {
        return false;
    }
    buf[(*pos)++] = ' ';

    bool needs_quote = (arg[0] == '\0' || strpbrk(arg, " \t\"\\") != NULL);
    if (!needs_quote) {
        int more = snprintf(buf + *pos, cap - (size_t)*pos, "%s", arg);
        if (more < 0 || *pos + more >= (int)cap) {
            return false;
        }
        *pos += more;
        return true;
    }

    const char *eq = strchr(arg, '=');
    bool key_eq_val = false;
    if (eq && eq > arg) {
        key_eq_val = true;
        for (const char *p = arg; p < eq; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"' || *p == '\\') {
                key_eq_val = false;
                break;
            }
        }
    }

    if (key_eq_val) {
        const char *val = eq + 1;
        bool val_needs_quote = (val[0] == '\0' || strpbrk(val, " \t\"\\") != NULL);
        if (!val_needs_quote) {
            int more = snprintf(buf + *pos, cap - (size_t)*pos, "%s", arg);
            if (more < 0 || *pos + more >= (int)cap) {
                return false;
            }
            *pos += more;
            return true;
        }
        size_t klen = (size_t)(eq - arg) + 1;
        if (klen >= cap || *pos + (int)klen + 2 >= (int)cap) {
            return false;
        }
        memcpy(buf + *pos, arg, klen);
        *pos += (int)klen;
        buf[(*pos)++] = '"';
        if (!append_escaped_str(buf, cap, pos, val)) {
            return false;
        }
        if (*pos + 1 >= (int)cap) {
            return false;
        }
        buf[(*pos)++] = '"';
        return true;
    }

    if (*pos + 2 >= (int)cap) {
        return false;
    }
    buf[(*pos)++] = '"';
    if (!append_escaped_str(buf, cap, pos, arg)) {
        return false;
    }
    if (*pos + 1 >= (int)cap) {
        return false;
    }
    buf[(*pos)++] = '"';
    return true;
}

int main(int argc, char *argv[]) {
    const char *socket_path = NULL;
    int timeout_ms = 2000;
    bool timeout_explicit = false;

    static const struct option long_opts[] = {
        {"socket", required_argument, NULL, 's'},
        {"timeout", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    /* '+' 阻止 GNU getopt 重排参数：子命令的 --region / -5 必须原样传给服务端 */
    while ((opt = getopt_long(argc, argv, "+s:t:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 't':
            timeout_ms = atoi(optarg);
            if (timeout_ms <= 0) {
                fprintf(stderr, "wctl: invalid timeout '%s'\n", optarg);
                return 2;
            }
            timeout_explicit = true;
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    const char *cmd = normalize(optind < argc ? argv[optind] : NULL);

    bool is_wait = strcmp(cmd, "wait") == 0;
    bool has_wait_timeout = false;
    int wait_timeout_ms = timeout_explicit ? timeout_ms : 5000;
    if (is_wait) {
        for (int i = optind + 1; i < argc; i++) {
            if (strncmp(argv[i], "timeout=", 8) == 0) {
                has_wait_timeout = true;
                char *end = NULL;
                long v = strtol(argv[i] + 8, &end, 10);
                if (argv[i][8] != '\0' && end && *end == '\0' && v > 0 && v <= 60000) {
                    wait_timeout_ms = (int)v;
                }
            }
        }
    }

    int connect_timeout_ms = timeout_ms;
    int read_timeout_ms = timeout_ms;
    if (is_wait) {
        connect_timeout_ms = timeout_explicit
                                 ? timeout_ms
                                 : (wait_timeout_ms > timeout_ms ? wait_timeout_ms : timeout_ms);
        read_timeout_ms = wait_timeout_ms + 1000;
    }

    char path[256] = {0};
    if (socket_path) {
        snprintf(path, sizeof(path), "%s", socket_path);
    } else if (getenv("WOODLAND_IPC") && getenv("WOODLAND_IPC")[0]) {
        snprintf(path, sizeof(path), "%s", getenv("WOODLAND_IPC"));
    } else if (!find_latest(path, sizeof(path))) {
        fprintf(stderr, "wctl: no debug socket found "
                        "(set WOODLAND_IPC or pass -s)\n");
        return 2;
    }

    int fd = connect_with_retry(path, connect_timeout_ms);
    if (fd < 0) {
        return 2;
    }

    /* -t 对 connect 之外也要生效：对端连上但不说话时，阻塞 read 会一直等下去，
     * 这个工具就不能用在超时/就绪探测里了 */
    struct timeval tv;
    tv.tv_sec = read_timeout_ms / 1000;
    tv.tv_usec = (read_timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 1000;   /* -t 小于 1ms 时给个下限，0 表示"永不超时" */
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 命令名后面的参数原样拼进同一行（写命令：move 3 100 200）。
     * 容量按"够用且有上限"定：超出就拒发，不发被截断的半条命令 */
    char request[1024];
    bool bad_newline = strpbrk(cmd, "\r\n") != NULL;
    int request_len = snprintf(request, sizeof(request), "%s", cmd);
    bool arg_ok = !bad_newline && request_len >= 0 && (size_t)request_len < sizeof(request);
    for (int i = optind + 1; arg_ok && i < argc; i++) {
        arg_ok = append_arg(request, sizeof(request), &request_len, argv[i], &bad_newline);
    }
    if (arg_ok && is_wait && !has_wait_timeout) {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "timeout=%d", wait_timeout_ms);
        arg_ok = append_arg(request, sizeof(request), &request_len, tbuf, &bad_newline);
    }
    if (arg_ok && request_len >= 0 && (size_t)request_len + 2 <= sizeof(request)) {
        request[request_len++] = '\n';
        request[request_len] = '\0';
    } else {
        request_len = (int)sizeof(request);   /* 触发下面的错误分支 */
    }
    if (bad_newline) {
        fprintf(stderr, "wctl: command arguments cannot contain newlines\n");
        close(fd);
        return 2;
    }
    /* snprintf 返回"本来要写多少"，截断时它会大于缓冲：直接拿去 write
     * 会从栈上读出去，把后面的内存发给对端 */
    if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
        fprintf(stderr, "wctl: command too long (%zu bytes max)\n",
                sizeof(request) - 2);
        close(fd);
        return 2;
    }
    if (write(fd, request, (size_t)request_len) != request_len) {
        fprintf(stderr, "wctl: write failed: %s\n", strerror(errno));
        close(fd);
        return 2;
    }

    /* 协议是一行一条回应，读到换行为止 */
    static char reply[WCTL_MAX_REPLY];
    size_t len = 0;
    bool got_newline = false;
    while (len < sizeof(reply) - 1) {
        ssize_t n = read(fd, reply + len, sizeof(reply) - 1 - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "wctl: timed out after %d ms waiting for reply\n",
                        read_timeout_ms);
            } else {
                fprintf(stderr, "wctl: read failed: %s\n", strerror(errno));
            }
            close(fd);
            return 2;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        if (memchr(reply, '\n', len)) {
            got_newline = true;
            break;
        }
    }
    close(fd);

    /* 缓冲区塞满还没见到换行：接下来打印的只是前半截 JSON，
     * 而且很可能连 "ok":true 都撞得上，不能当成功 */
    if (!got_newline && len >= sizeof(reply) - 1) {
        fprintf(stderr, "wctl: reply exceeds %d bytes, refusing to print "
                        "truncated JSON (try a narrower command)\n",
                WCTL_MAX_REPLY);
        return 2;
    }
    reply[len] = '\0';

    if (len == 0) {
        fprintf(stderr, "wctl: empty reply (compositor gone?)\n");
        return 2;
    }
    if (!got_newline) {
        fprintf(stderr, "wctl: incomplete reply (%zu bytes, no newline)\n", len);
        return 2;
    }
    /* 去掉行尾换行，输出保持单行 */
    while (len > 0 && (reply[len - 1] == '\n' || reply[len - 1] == '\r')) {
        reply[--len] = '\0';
    }
    printf("%s\n", reply);

    /*
     * 退出码让 shell 能直接判断：ok=false / 有错误 → 1。
     * 不能拿 "{\"ok\":true" 定长比较——那样会把结尾的 NUL 也拿去比，
     * 后面紧跟的逗号对不上，永远返回 1。
     */
    return (reply[0] == '{' && strstr(reply, "\"ok\":true") != NULL) ? 0 : 1;
}
