// SPDX-License-Identifier: GPL-2.0-only
/*
 * woodland-shortcutd —— treeland_shortcut_manager_v3 的客户端实现。
 *
 * 合成器侧（src/protocol/shortcut.c）提供的是"注册表 + 执行器"：客户端
 * acquire 拿独占，把带名字的 key 绑定注册进去，命中后合成器要么执行
 * action，要么回一个 activated 事件。协议本身不带任何默认值，所以非 DDE
 * 环境下没人注册 = 没有快捷键。这个进程就是那个"没人"：
 *
 *   - 读 $XDG_CONFIG_HOME/woodland/shortcuts.toml（或 -c 指定的文件），
 *     没有文件就用内置默认键位（Super+Return 开终端之类）；
 *   - acquire 之后把每条绑定 bind_key 登进去；
 *   - action=notify 的绑定命中时自己 fork/exec 命令，其余交给合成器。
 *
 * 为什么必须由合成器按 XDG_CURRENT_DESKTOP 决定要不要拉它：协议规定**同时
 * 只有一个客户端**能持有 manager（第二个 acquire 直接 occupied）。DDE 会话
 * 里这个位置属于 dde-daemon，我们先抢到就等于把 DDE 的全局快捷键全废掉。
 * 详见 src/server/server.c:start_shortcut_client()。
 *
 * 退出码约定（脚本/冒烟测试要能区分"没连上"和"协议对端拒绝"）：
 *   0  正常退出（收到 SIGTERM / 合成器断开 / 配置里 enabled=false）
 *   1  配置或运行期错误
 *   2  命令行用法错误
 *   3  连不上 Wayland display
 *   4  合成器没暴露 treeland_shortcut_manager_v3
 *   5  协议错误（最常见的是 occupied：别的客户端已经持有 manager）
 *
 * 调试：--verbose 会把每条注册、每次命中、每次 reload 都打到 stderr；
 * SIGHUP 重新读配置（先 unbind 旧的再 bind 新的，不放弃独占）。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "shortcutd_config.h"
#include "shortcutd_spawn.h"

#include "treeland-shortcut-manager-unstable-v3-client-protocol.h"

/*
 * 配置模型里的常量是协议枚举的本地副本（为了不让配置模块依赖生成的协议头）。
 * 这里做编译期对齐：上游改枚举值时先炸在这里，而不是在用户按下键位之后。
 */
_Static_assert(WSD_FLAG_PRESS == TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_KEY_PRESS,
               "keybind_flag 与协议不一致");
_Static_assert(WSD_FLAG_RELEASE == TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_KEY_RELEASE,
               "keybind_flag 与协议不一致");
_Static_assert(WSD_FLAG_REPEAT == TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_REPEAT,
               "keybind_flag 与协议不一致");
_Static_assert((uint32_t)WSD_ACTION_NOTIFY == (uint32_t)TREELAND_SHORTCUT_MANAGER_V3_ACTION_NOTIFY,
               "action 枚举与协议不一致");
_Static_assert((uint32_t)WSD_ACTION_WORKSPACE_12 == (uint32_t)TREELAND_SHORTCUT_MANAGER_V3_ACTION_WORKSPACE_12,
               "action 枚举与协议不一致");
_Static_assert((uint32_t)WSD_ACTION_TILE_LEFT == (uint32_t)TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_LEFT,
               "action 枚举与协议不一致");
_Static_assert((uint32_t)WSD_ACTION_MAX == (uint32_t)TREELAND_SHORTCUT_MANAGER_V3_ACTION_SHUTDOWN_MENU,
               "action 枚举与协议不一致");

#define SD_EXIT_OK 0
#define SD_EXIT_ERROR 1
#define SD_EXIT_USAGE 2
#define SD_EXIT_NO_DISPLAY 3
#define SD_EXIT_NO_PROTOCOL 4
#define SD_EXIT_PROTOCOL 5

/* 协议是 experimental，版本号写死 1（与合成器侧 SC_VERSION 一致） */
#define SD_MANAGER_VERSION 1

struct sd_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct treeland_shortcut_manager_v3 *manager;
    struct wsd_config config;
    const char *config_path; /* NULL 表示走默认路径 */
    bool acquired;
    bool verbose;
    bool running;
    volatile sig_atomic_t pending_signal;
    unsigned registered;
    unsigned activations;
    unsigned failures;
};

/* ==================== 日志 ==================== */

static bool g_verbose;

static void sd_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sd_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("woodland-shortcutd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void sd_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sd_info(const char *fmt, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fputs("woodland-shortcutd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ==================== 协议回调 ==================== */

static const struct wsd_binding *find_binding(const struct sd_state *st, const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < st->config.count; i++) {
        if (strcmp(st->config.bindings[i].name, name) == 0) {
            return &st->config.bindings[i];
        }
    }
    return NULL;
}

static const char *bind_error_name(uint32_t error)
{
    switch (error) {
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_NAME_CONFLICT:
        return "name_conflict";
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_DUPLICATE_BINDING:
        return "duplicate_binding";
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT:
        return "invalid_argument";
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INTERNAL_ERROR:
        return "internal_error";
    default:
        return "unknown";
    }
}

static void manager_handle_activated(void *data,
                                     struct treeland_shortcut_manager_v3 *manager,
                                     const char *name, uint32_t flags)
{
    struct sd_state *st = data;
    (void)manager;
    st->activations++;

    const struct wsd_binding *binding = find_binding(st, name);
    if (!binding) {
        /* 合成器里还留着上一版配置注册的绑定（reload 之间按下） */
        sd_warn("activated \"%s\" 不在当前配置里，忽略", name ? name : "");
        return;
    }
    if (!binding->command) {
        /* action 类的绑定由合成器自己执行，不该走 notify 这条回包 */
        sd_info("activated \"%s\" (合成器动作 %s)", binding->name,
                wsd_action_name(binding->action) ? wsd_action_name(binding->action) : "?");
        return;
    }
    if (wsd_spawn_command(binding->command)) {
        sd_info("activated \"%s\" (flags=%u) → %s", binding->name, flags, binding->command);
    } else {
        sd_warn("activated \"%s\" 但启动失败：%s", binding->name, binding->command);
    }
}

static void manager_handle_bind_failure(void *data,
                                        struct treeland_shortcut_manager_v3 *manager,
                                        const char *name, uint32_t error)
{
    struct sd_state *st = data;
    (void)manager;
    st->failures++;
    const struct wsd_binding *binding = find_binding(st, name);
    sd_warn("合成器拒绝了绑定 \"%s\"（%s）：%s", name ? name : "", bind_error_name(error),
            binding ? binding->key : "配置里已经没有这条");
}

static const struct treeland_shortcut_manager_v3_listener manager_listener = {
    .activated = manager_handle_activated,
    .bind_failure = manager_handle_bind_failure,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version)
{
    struct sd_state *st = data;
    if (strcmp(interface, treeland_shortcut_manager_v3_interface.name) != 0) {
        return;
    }
    if (st->manager) {
        return;
    }
    uint32_t bind_version = version < SD_MANAGER_VERSION ? version : SD_MANAGER_VERSION;
    st->manager = wl_registry_bind(registry, name,
                                   &treeland_shortcut_manager_v3_interface, bind_version);
    if (st->manager) {
        treeland_shortcut_manager_v3_add_listener(st->manager, &manager_listener, st);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ==================== 配置 ==================== */

/*
 * 把配置文件读进 cfg（调用方传入空配置）。文件不存在时填内置默认键位。
 * 返回 false 表示有硬错误（已经往 stderr 写过原因）。
 */
static bool sd_load_config(struct sd_state *st, struct wsd_config *cfg)
{
    char pathbuf[PATH_MAX];
    const char *path = st->config_path;
    if (!path) {
        path = wsd_config_default_path(pathbuf, sizeof(pathbuf));
    }

    char err[256] = {0};
    enum wsd_load_result result = wsd_config_load_file(cfg, path, err, sizeof(err));
    if (result == WSD_LOAD_MISSING) {
        fprintf(stderr,
                "woodland-shortcutd: 没有 %s，使用内置默认键位"
                "（--print-default-config 可以导出成配置文件）\n",
                path);
        wsd_config_load_defaults(cfg);
    } else if (result == WSD_LOAD_ERROR) {
        sd_warn("%s", err);
        return false;
    }
    if (cfg->count == 0) {
        sd_warn("%s 里没有可用的绑定", path);
    }
    return true;
}

/* ==================== 注册 ==================== */

static void sd_bind_all(struct sd_state *st)
{
    st->registered = 0;
    for (size_t i = 0; i < st->config.count; i++) {
        const struct wsd_binding *binding = &st->config.bindings[i];
        treeland_shortcut_manager_v3_bind_key(st->manager, binding->name, binding->key,
                                              binding->flags, binding->action);
        st->registered++;
        sd_info("bind %-24s %-8s %s", binding->key,
                binding->command ? "command" : "action",
                binding->command ? binding->command
                                 : (wsd_action_name(binding->action) ? wsd_action_name(binding->action)
                                                                    : "?"));
    }
}

static void sd_unbind_all(struct sd_state *st)
{
    for (size_t i = 0; i < st->config.count; i++) {
        treeland_shortcut_manager_v3_unbind(st->manager, st->config.bindings[i].name);
    }
}

/**
 * SIGHUP：重新读配置。先读成功再动绑定——读失败就保持现状，用户改错了
 * 不该把手里能用的键位一起扔掉。
 */
static void sd_handle_reload(struct sd_state *st)
{
    struct wsd_config next;
    wsd_config_init(&next);
    if (!sd_load_config(st, &next)) {
        wsd_config_clear(&next);
        sd_warn("重载失败，保留当前 %zu 条绑定", st->config.count);
        return;
    }
    if (!next.enabled) {
        wsd_config_clear(&next);
        sd_warn("配置里 [general] enabled = false，退出");
        st->running = false;
        return;
    }

    sd_unbind_all(st);
    wsd_config_clear(&st->config);
    st->config = next;
    sd_bind_all(st);
    if (wl_display_flush(st->display) < 0 && errno != EAGAIN) {
        sd_warn("flush failed: %s", strerror(errno));
    }
    sd_info("重载完成：%u 条绑定", st->registered);
}

/* ==================== 主循环 ==================== */

static int sd_display_failed(struct sd_state *st)
{
    int err = wl_display_get_error(st->display);
    if (err == EPROTO) {
        sd_warn("协议错误：合成器关闭了连接。如果这里跑着 DDE，"
                "treeland_shortcut_manager_v3 可能已经被 dde-daemon 之类持有（occupied）");
        return SD_EXIT_PROTOCOL;
    }
    sd_warn("与合成器的连接断开：%s", strerror(err));
    return SD_EXIT_ERROR;
}

/*
 * 事件循环。手写 poll + prepare_read 而不是直接 wl_display_dispatch()：
 * SIGHUP 要能打断阻塞（sa_flags 不设 SA_RESTART，poll 返回 EINTR），而且
 * 只有在确认可读时 read_events，不会出现"用户空间还有事件、socket 却读不到"
 * 的假死。
 */
static int sd_run(struct sd_state *st)
{
    int fd = wl_display_get_fd(st->display);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    while (st->running) {
        if (wl_display_dispatch_pending(st->display) < 0) {
            return sd_display_failed(st);
        }
        if (wl_display_flush(st->display) < 0 && errno != EAGAIN) {
            return sd_display_failed(st);
        }

        sig_atomic_t sig = st->pending_signal;
        if (sig) {
            st->pending_signal = 0;
            if (sig == SIGHUP) {
                sd_handle_reload(st);
            } else {
                sd_info("收到信号 %d，退出", (int)sig);
                st->running = false;
            }
            continue;
        }

        if (wl_display_prepare_read(st->display) != 0) {
            /* 内存里还有排队事件：先让上面的 dispatch_pending 吃掉 */
            continue;
        }

        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            wl_display_cancel_read(st->display);
            if (errno == EINTR) {
                continue; /* 交给循环头的 pending_signal 处理 */
            }
            sd_warn("poll failed: %s", strerror(errno));
            return SD_EXIT_ERROR;
        }
        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(st->display) < 0) {
                return sd_display_failed(st);
            }
            continue;
        }

        wl_display_cancel_read(st->display);
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            sd_info("合成器断开连接");
            return SD_EXIT_OK;
        }
    }
    return SD_EXIT_OK;
}

/* ==================== 入口 ==================== */

static void print_actions(FILE *out)
{
    fputs("actions:\n", out);
    for (uint32_t i = 0; i <= WSD_ACTION_MAX; i++) {
        const char *name = wsd_action_name(i);
        if (name) {
            fprintf(out, "  %s\n", name);
        }
    }
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "\n"
           "在合成器里注册全局快捷键（treeland_shortcut_manager_v3）。\n"
           "非 DDE 会话下由 woodland 自动启动；DDE 会话里由 dde-daemon 占用，不要手动跑。\n"
           "\n"
           "options:\n"
           "  -c, --config PATH        配置文件（默认 $XDG_CONFIG_HOME/woodland/shortcuts.toml）\n"
           "  -n, --dry-run            只解析配置并打印，不连接合成器\n"
           "      --print-default-config  打印内置默认键位（可直接当配置文件用）\n"
           "  -v, --verbose            打印每条绑定与每次命中\n"
           "  -h, --help               显示本帮助\n"
           "\n"
           "环境变量:\n"
           "  WOODLAND_SHORTCUTD       合成器据此找本程序；设为 off/0 则合成器不启动它\n"
           "\n",
           name);
    print_actions(stdout);
}

/* signal handler 里只能碰 sig_atomic_t；进程内只有一个 wsd 实例，用静态指针带过去 */
static struct sd_state *g_state_for_signal;

static void handle_signal(int sig)
{
    /* 只记信号，真正的处理在主循环里（handler 里不能干重活） */
    if (g_state_for_signal) {
        g_state_for_signal->pending_signal = sig;
    }
}

static void install_signal_handlers(struct sd_state *st)
{
    g_state_for_signal = st;
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    /* 不设 SA_RESTART：poll 要靠 EINTR 回到主循环顶部 */
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char *argv[])
{
    static const struct option long_options[] = {
        {"config", required_argument, NULL, 'c'},
        {"dry-run", no_argument, NULL, 'n'},
        {"print-default-config", no_argument, NULL, 1000},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    struct sd_state st = {.running = true};
    wsd_config_init(&st.config);
    bool dry_run = false;

    int c;
    while ((c = getopt_long(argc, argv, "c:nvh", long_options, NULL)) != -1) {
        switch (c) {
        case 'c':
            st.config_path = optarg;
            break;
        case 'n':
            dry_run = true;
            break;
        case 'v':
            g_verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            return SD_EXIT_OK;
        case 1000:
            wsd_print_sample_config(stdout);
            return SD_EXIT_OK;
        default:
            usage(argv[0]);
            return SD_EXIT_USAGE;
        }
    }
    if (optind < argc) {
        sd_warn("多余的参数：%s", argv[optind]);
        usage(argv[0]);
        return SD_EXIT_USAGE;
    }

    if (!sd_load_config(&st, &st.config)) {
        wsd_config_clear(&st.config);
        return SD_EXIT_ERROR;
    }
    if (!st.config.enabled) {
        sd_info("配置里 [general] enabled = false，不注册任何快捷键");
        wsd_config_clear(&st.config);
        return SD_EXIT_OK;
    }
    if (dry_run) {
        wsd_config_dump(&st.config, stdout);
        wsd_config_clear(&st.config);
        return SD_EXIT_OK;
    }
    if (st.config.count == 0) {
        /*
         * 一条都没有就别去 acquire 了：占了独占位置却什么都不注册，
         * 后面想接管（比如用户临时起 dde-daemon）的客户端会拿到 occupied。
         */
        sd_warn("没有可注册的快捷键，退出");
        wsd_config_clear(&st.config);
        return SD_EXIT_OK;
    }

    st.display = wl_display_connect(NULL);
    if (!st.display) {
        sd_warn("连不上 Wayland display（WAYLAND_DISPLAY=%s）",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        wsd_config_clear(&st.config);
        return SD_EXIT_NO_DISPLAY;
    }
    install_signal_handlers(&st);

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    /* 第一次 roundtrip 只拿 global 列表，bind 才算真正生效 */
    if (wl_display_roundtrip(st.display) < 0) {
        sd_warn("拿 global 列表失败");
        wl_display_disconnect(st.display);
        wsd_config_clear(&st.config);
        return SD_EXIT_ERROR;
    }
    if (!st.manager) {
        sd_warn("合成器没有暴露 treeland_shortcut_manager_v3（不是 woodland/treeland？）");
        wl_display_disconnect(st.display);
        wsd_config_clear(&st.config);
        return SD_EXIT_NO_PROTOCOL;
    }

    treeland_shortcut_manager_v3_acquire(st.manager);
    st.acquired = true;
    sd_bind_all(&st);
    /* 第二遍 roundtrip：把 acquire/bind_key 真正发出去，顺便收 bind_failure */
    if (wl_display_roundtrip(st.display) < 0) {
        sd_warn("注册绑定失败：合成器断开（如果这里跑着 DDE，多半是 occupied）");
        wl_display_disconnect(st.display);
        wsd_config_clear(&st.config);
        return SD_EXIT_PROTOCOL;
    }
    fprintf(stderr, "woodland-shortcutd: 已注册 %u 条快捷键（%u 条被拒绝）\n", st.registered,
            st.failures);

    int rc = sd_run(&st);

    wsd_config_clear(&st.config);
    /*
     * 显式销毁两个 proxy 再断连接：destroy 让合成器立刻放开独占（不必等
     * socket 关闭），而 wl_registry 不销毁就是一处 wl_proxy_marshal 的泄漏
     * ——ASan 构建下这条退出路径会被 LeakSanitizer 判红。
     */
    if (st.manager) {
        treeland_shortcut_manager_v3_destroy(st.manager);
        st.manager = NULL;
    }
    if (st.registry) {
        wl_registry_destroy(st.registry);
        st.registry = NULL;
    }
    wl_display_flush(st.display);
    wl_display_disconnect(st.display);
    return rc;
}
