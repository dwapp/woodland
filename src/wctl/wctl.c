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
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

#define WCTL_MAX_REPLY 65536

static void usage(const char *name) {
    printf("Usage: %s [-s socket] [-t timeout_ms] [command]\n"
           "\n"
           "commands:\n"
           "  get_tree    windows, outputs, workspace, focus  (default)\n"
           "  get_focus   just the focus state\n"
           "  self_check  invariant checks\n"
           "  dump_scene  the wlr_scene tree\n"
           "  ping        liveness probe\n"
           "\n"
           "aliases: tree, focus, check, scene\n"
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

int main(int argc, char *argv[]) {
    const char *socket_path = NULL;
    int timeout_ms = 2000;

    int opt;
    while ((opt = getopt(argc, argv, "s:t:h")) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 't':
            timeout_ms = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    const char *cmd = normalize(optind < argc ? argv[optind] : NULL);

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

    int fd = connect_with_retry(path, timeout_ms);
    if (fd < 0) {
        return 2;
    }

    char request[64];
    int request_len = snprintf(request, sizeof(request), "%s\n", cmd);
    if (write(fd, request, (size_t)request_len) != request_len) {
        fprintf(stderr, "wctl: write failed: %s\n", strerror(errno));
        close(fd);
        return 2;
    }

    /* 协议是一行一条回应，读到换行为止 */
    static char reply[WCTL_MAX_REPLY];
    size_t len = 0;
    while (len < sizeof(reply) - 1) {
        ssize_t n = read(fd, reply + len, sizeof(reply) - 1 - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "wctl: read failed: %s\n", strerror(errno));
            close(fd);
            return 2;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        if (memchr(reply, '\n', len)) {
            break;
        }
    }
    close(fd);
    reply[len] = '\0';

    if (len == 0) {
        fprintf(stderr, "wctl: empty reply (compositor gone?)\n");
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
