// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-appearance / -manager 的最小客户端探针。
 *
 * 它自己不做断言——断言交给冒烟脚本（wctl get_tree 的 appearance 段 + 探针
 * 打印的事件行）。探针只负责把"一个真实的控制中心/应用会做什么"做出来：
 *
 *   --observe <毫秒>    bind treeland_appearance_v1，把收到的每条事件按
 *                       "<event> <值...>" 打印到 stdout，撑满这么多毫秒再退出。
 *                       用来验证两件事：bind 之后立刻收到 11 项当前值；
 *                       别人 set_* 时能收到广播。
 *   --set <请求> [参数]  bind manager 发一条 set_*，roundtrip 一次确认连接没
 *                       被协议错误杀掉。请求名用协议里的原名（set_cursor_theme），
 *                       脚本不必记一套缩写。
 *   --set-invalid-scheme <n>  set_color_scheme 的枚举外取值（协议要求静默忽略）
 *
 * 为什么不在这里建窗口：SSD 几何的断言用 deco_probe 起的真实顶层窗口，
 * 一个进程一个场景更好定位（见 AGENTS.md 冒烟陷阱 5）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client.h>

#include "treeland-appearance-manager-unstable-v1-client-protocol.h"
#include "treeland-appearance-unstable-v1-client-protocol.h"

struct probe_state {
    struct treeland_appearance_v1 *observer;
    struct treeland_appearance_manager_v1 *manager;
    uint32_t event_count;
    bool found_observer;
    bool found_manager;
};

/* ==================== 事件打印（脚本按这些行断言） ==================== */

static void ev_cursor_theme(void *data, struct treeland_appearance_v1 *obj, const char *name)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("cursor_theme %s\n", name ? name : "");
}

static void ev_cursor_size(void *data, struct treeland_appearance_v1 *obj, uint32_t width,
                           uint32_t height)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("cursor_size %u %u\n", width, height);
}

static void ev_font(void *data, struct treeland_appearance_v1 *obj, const char *font_name)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("font %s\n", font_name ? font_name : "");
}

static void ev_monospace_font(void *data, struct treeland_appearance_v1 *obj,
                              const char *font_name)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("monospace_font %s\n", font_name ? font_name : "");
}

static void ev_font_size(void *data, struct treeland_appearance_v1 *obj, uint32_t size)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("font_size %u\n", size);
}

static void ev_icon_theme(void *data, struct treeland_appearance_v1 *obj, const char *theme_name)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("icon_theme %s\n", theme_name ? theme_name : "");
}

static void ev_accent_color(void *data, struct treeland_appearance_v1 *obj, uint32_t r, uint32_t g,
                           uint32_t b)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("accent_color %u %u %u\n", r, g, b);
}

static void ev_window_opacity(void *data, struct treeland_appearance_v1 *obj, wl_fixed_t opacity)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("window_opacity %.3f\n", wl_fixed_to_double(opacity));
}

static void ev_color_scheme(void *data, struct treeland_appearance_v1 *obj, uint32_t scheme)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("color_scheme %u\n", scheme);
}

static void ev_titlebar_height(void *data, struct treeland_appearance_v1 *obj, uint32_t height)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("titlebar_height %u\n", height);
}

static void ev_corner_radius(void *data, struct treeland_appearance_v1 *obj, uint32_t radius)
{
    (void)obj;
    ((struct probe_state *)data)->event_count++;
    printf("corner_radius %u\n", radius);
}

static const struct treeland_appearance_v1_listener observer_listener = {
    .cursor_theme = ev_cursor_theme,
    .cursor_size = ev_cursor_size,
    .font = ev_font,
    .monospace_font = ev_monospace_font,
    .font_size = ev_font_size,
    .icon_theme = ev_icon_theme,
    .accent_color = ev_accent_color,
    .window_opacity = ev_window_opacity,
    .color_scheme = ev_color_scheme,
    .titlebar_height = ev_titlebar_height,
    .corner_radius = ev_corner_radius,
};

/* ==================== registry ==================== */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct probe_state *state = data;
    (void)version;
    if (strcmp(interface, treeland_appearance_v1_interface.name) == 0) {
        state->found_observer = true;
        /* 协议版本写死 1：experimental，上游升版要显式跟 */
        state->observer =
            wl_registry_bind(registry, name, &treeland_appearance_v1_interface, 1);
        treeland_appearance_v1_add_listener(state->observer, &observer_listener, state);
    } else if (strcmp(interface, treeland_appearance_manager_v1_interface.name) == 0) {
        state->found_manager = true;
        state->manager = wl_registry_bind(registry, name,
                                          &treeland_appearance_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ==================== 工具 ==================== */

static int report_error(struct wl_display *display, const char *where)
{
    int err = wl_display_get_error(display);
    const struct wl_interface *iface = NULL;
    uint32_t protocol_code = 0;
    uint32_t object_id = 0;
    /* wl_display_get_error() 给的是 errno（协议错误恒为 EPROTO=71）；
     * 真正的协议错误码要问 wl_display_get_protocol_error()（3 个参数） */
    if (err == EPROTO) {
        protocol_code = wl_display_get_protocol_error(display, &iface, &object_id);
    }
    fprintf(stderr, "%s: display error %d (EPROTO=%d), protocol error %u on %s (id %u)\n", where,
            err, EPROTO, protocol_code, iface ? iface->name : "-", object_id);
    return 1;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

/* ==================== 模式 ==================== */

static int run_observe(struct wl_display *display, struct probe_state *state, int duration_ms)
{
    /*
     * 两次 roundtrip，不能省。
     *
     * 第一次只拿到 global 列表：registry 回调里发出的 bind 请求被排进出站缓冲，
     * 但同一批事件里那个 sync 回调已经把 done 置上了，dispatch 循环立刻退出，
     * 缓冲里剩下的 bind 根本没人 flush —— 合成器看不到 bind，也就不会推初始值。
     * 第二次 roundtrip 开头的那次 flush 才把 bind 发出去，它同时把 bind 时
     * 推送的 11 条事件收回来。
     */
    if (wl_display_roundtrip(display) < 0) {
        return report_error(display, "registry roundtrip");
    }
    if (!state->found_observer) {
        fprintf(stderr, "treeland_appearance_v1 global not advertised\n");
        return 1;
    }
    if (wl_display_roundtrip(display) < 0) {
        return report_error(display, "initial push roundtrip");
    }
    printf("ready\n");
    fflush(stdout);

    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
    long long deadline = now_ms() + duration_ms;
    while (now_ms() < deadline) {
        long long left = deadline - now_ms();
        int timeout = left > 0 ? (int)left : 0;

        /*
         * 收广播必须走 prepare_read / poll / read_events / dispatch_pending
         * 这一套。只调 dispatch_pending 是不行的：它只分发已经躺进内存队列的
         * 事件，socket 里的字节一个都不会被读走 —— roundtrip 收得到初始值
         * （它是自己读的），后续广播却永远进不来。
         */
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                return report_error(display, "dispatch pending");
            }
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return report_error(display, "flush");
        }

        int ret = poll(&pfd, 1, timeout);
        if (ret < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 1;
        }
        if (ret == 0) {
            wl_display_cancel_read(display);
            break;
        }
        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(display) < 0) {
                return report_error(display, "read events");
            }
        } else {
            wl_display_cancel_read(display);
        }
        /* dispatch_pending 返回分发了几个事件，判错只看 < 0 */
        if (wl_display_dispatch_pending(display) < 0) {
            return report_error(display, "dispatch");
        }
    }
    fflush(stdout);
    return 0;
}

static int run_set(struct wl_display *display, struct probe_state *state, int argc, char **argv)
{
    /* 两次 roundtrip：第一次拿 global，第二次才把 registry 回调里排队的 bind
     * 真正 flush 出去（原因见 run_observe 的注释） */
    if (wl_display_roundtrip(display) < 0) {
        return report_error(display, "registry roundtrip");
    }
    if (!state->found_manager) {
        fprintf(stderr, "treeland_appearance_manager_v1 global not advertised\n");
        return 1;
    }
    if (wl_display_roundtrip(display) < 0) {
        return report_error(display, "bind roundtrip");
    }
    const char *request = argv[0];
    uint32_t a = 0, b = 0, c = 0;
    bool ok = true;

    if (strcmp(request, "set_cursor_theme") == 0 && argc == 2) {
        treeland_appearance_manager_v1_set_cursor_theme(state->manager, argv[1]);
    } else if (strcmp(request, "set_cursor_size") == 0 && argc == 3) {
        ok = parse_u32(argv[1], &a) && parse_u32(argv[2], &b);
        if (ok) {
            treeland_appearance_manager_v1_set_cursor_size(state->manager, a, b);
        }
    } else if (strcmp(request, "set_font") == 0 && argc == 2) {
        treeland_appearance_manager_v1_set_font(state->manager, argv[1]);
    } else if (strcmp(request, "set_monospace_font") == 0 && argc == 2) {
        treeland_appearance_manager_v1_set_monospace_font(state->manager, argv[1]);
    } else if (strcmp(request, "set_font_size") == 0 && argc == 2) {
        ok = parse_u32(argv[1], &a);
        if (ok) {
            treeland_appearance_manager_v1_set_font_size(state->manager, a);
        }
    } else if (strcmp(request, "set_icon_theme") == 0 && argc == 2) {
        treeland_appearance_manager_v1_set_icon_theme(state->manager, argv[1]);
    } else if (strcmp(request, "set_accent_color") == 0 && argc == 4) {
        ok = parse_u32(argv[1], &a) && parse_u32(argv[2], &b) && parse_u32(argv[3], &c);
        if (ok) {
            treeland_appearance_manager_v1_set_accent_color(state->manager, a, b, c);
        }
    } else if (strcmp(request, "set_window_opacity") == 0 && argc == 2) {
        char *end = NULL;
        double value = strtod(argv[1], &end);
        if (!end || *end != '\0') {
            ok = false;
        } else {
            treeland_appearance_manager_v1_set_window_opacity(
                state->manager, wl_fixed_from_double(value));
        }
    } else if (strcmp(request, "set_color_scheme") == 0 && argc == 2) {
        ok = parse_u32(argv[1], &a);
        if (ok) {
            treeland_appearance_manager_v1_set_color_scheme(state->manager, a);
        }
    } else if (strcmp(request, "set_titlebar_height") == 0 && argc == 2) {
        ok = parse_u32(argv[1], &a);
        if (ok) {
            treeland_appearance_manager_v1_set_titlebar_height(state->manager, a);
        }
    } else if (strcmp(request, "set_corner_radius") == 0 && argc == 2) {
        ok = parse_u32(argv[1], &a);
        if (ok) {
            treeland_appearance_manager_v1_set_corner_radius(state->manager, a);
        }
    } else {
        fprintf(stderr, "unknown or misused request: %s\n", request);
        return 2;
    }
    if (!ok) {
        fprintf(stderr, "bad argument for %s\n", request);
        return 2;
    }

    if (wl_display_roundtrip(display) < 0) {
        return report_error(display, "request roundtrip");
    }
    if (wl_display_get_error(display) != 0) {
        return report_error(display, "after request");
    }
    printf("set %s ok\n", request);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s --observe <ms> | --set <request> [args...]\n"
                "  requests: set_cursor_theme <name> | set_cursor_size <w> <h> |\n"
                "            set_font <family> | set_monospace_font <family> |\n"
                "            set_font_size <pt*10> | set_icon_theme <name> |\n"
                "            set_accent_color <r> <g> <b> | set_window_opacity <0..1> |\n"
                "            set_color_scheme <0|1> | set_titlebar_height <px> |\n"
                "            set_corner_radius <px>\n",
                argv[0]);
        return 2;
    }

    bool observe = strcmp(argv[1], "--observe") == 0;
    bool set = strcmp(argv[1], "--set") == 0;
    if (!observe && !set) {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 2;
    }

    int duration_ms = 500;
    if (observe) {
        uint32_t parsed = 0;
        if (!parse_u32(argv[2], &parsed)) {
            fprintf(stderr, "invalid duration: %s\n", argv[2]);
            return 2;
        }
        duration_ms = (int)parsed;
    } else if (argc < 3) {
        fprintf(stderr, "--set needs a request name\n");
        return 2;
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }

    struct probe_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);

    int rc;
    if (observe) {
        rc = run_observe(display, &state, duration_ms);
    } else {
        rc = run_set(display, &state, argc - 2, argv + 2);
    }

    /*
     * LSan 会把没销毁的 proxy 当成泄漏：destructor 请求会同时销毁本地
     * proxy，其余用 wl_proxy_destroy 只做本地释放（不再多发一条请求）。
     */
    if (state.observer) {
        treeland_appearance_v1_destroy(state.observer);
    }
    if (state.manager) {
        wl_proxy_destroy((struct wl_proxy *)state.manager);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return rc;
}
