// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-foreign-toplevel-manager-unstable-v3 的客户端探针。
 *
 * 它同时扮演两个角色：一个普通 xdg 窗口客户端，和一个 Dock（bind manager、
 * 收 handle、发 activate/close/maximize…）。这样单进程就能覆盖"窗口出现 →
 * handle 出现 → 属性快照 → 请求回灌 → closed"的完整链路，不需要第二个
 * 可执行文件配合。
 *
 * 断言仍然交给脚本（run_ftl_smoke.sh）：探针只把观察到的协议事件按稳定格式
 * 打出来，脚本用 grep 断言这几行、用 wctl 断言合成器内部状态。分成两半是
 * 有意的——协议"说了什么"和合成器"实际是什么"必须能对照，只看一边会漏掉
 * "发出去了但没人照做"这类 bug。
 *
 * 用法：
 *   ftl_probe [--late-bind] [--hold 毫秒] [操作...]
 *
 * 操作按顺序在 READY 之后执行：
 *   show [identifier]             scope.show（不带 id 时用自己窗口的 identifier）
 *   tooltip-empty                 用一块没有内容的 surface 调 show_tooltip_surface
 *                                 （协议：什么都不显示，不是错误）
 *   max / unmax                    set/unset_maximized
 *   min / unmin                    set/unset_minimized
 *   full / unfull                  set/unset_fullscreen（不带 output）
 *   icon <x> <y> <w> <h>           set_icon_geometry
 *   activate                       activate(wl_seat)
 *   close                          close（窗口应收到 xdg_toplevel.close）
 *   stop                           manager.stop，等 finished
 *   dup-context                    get_dock_preview_context 两次：第二次必须
 *                                  报 context_already_exists(1)
 *
 * --late-bind：先把窗口 map 上去，再 bind manager。覆盖"bind 时补快照"
 * 这条路径（客户端晚于窗口出现时绑定，是 Dock 重启后的常态）。
 *
 * 退出码：0 正常；1 环境/协商失败；3 收到了预期内的协议错误。
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "treeland-foreign-toplevel-manager-unstable-v3-client-protocol.h"
/* 前缀带 ftl-：同一个 build 目录里 custom_target 的输出名不能重复，
 * deco_probe 已经占了 xdg-shell-client-protocol.h */
#include "ftl-xdg-shell-client-protocol.h"

#define WIDTH 400
#define HEIGHT 300
#define DEFAULT_HOLD_MS 1500

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_output *output;
static struct treeland_foreign_toplevel_manager_v3 *manager;
static struct treeland_foreign_toplevel_handle_v3 *handle;
static struct treeland_dock_preview_context_v3 *dock_ctx;

/* manager 的全局名：--late-bind 要在窗口 map 之后再 bind 它 */
static uint32_t manager_name;
static uint32_t manager_version;
static int late_bind;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
/* tooltip 用的一块独立 surface（没有提交内容，用来走"客户端违约"分支） */
static struct wl_surface *bare_surface;
static struct wl_buffer *buffer;
static int configured;

/* 收到的属性（done 时打一行快照） */
static uint32_t identifier;
static int have_identifier;
static char title[256];
static char app_id[256];
static uint32_t pid;
static uint32_t state[8];
static size_t state_len;
static uint32_t outputs;
static uint32_t done_count;
static int closed;

static const struct xdg_wm_base_listener wm_base_listener;
static const struct treeland_foreign_toplevel_manager_v3_listener manager_listener;
static const struct treeland_foreign_toplevel_handle_v3_listener handle_listener;

/* ==================== 工具 ==================== */

static const char *state_name(uint32_t value)
{
    switch (value) {
    case TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_MAXIMIZED:
        return "maximized";
    case TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_MINIMIZED:
        return "minimized";
    case TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_ACTIVATED:
        return "activated";
    case TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_FULLSCREEN:
        return "fullscreen";
    case TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_ATTENTION:
        return "attention";
    default:
        return "unknown";
    }
}

/* 把状态数组打成 "activated+maximized" 这样的稳定字符串（空集写 "-"） */
static void state_string(char *buf, size_t len)
{
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < state_len; i++) {
        const char *name = state_name(state[i]);
        int n = snprintf(buf + used, used < len ? len - used : 0, "%s%s", used ? "+" : "", name);
        if (n < 0) {
            return;
        }
        used += (size_t)n;
        if (used >= len) {
            return;
        }
    }
    if (used == 0) {
        snprintf(buf, len, "-");
    }
}

static int pump(int ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (wl_display_prepare_read(display) == 0) {
            wl_display_flush(display);
            struct pollfd pfd = {
                .fd = wl_display_get_fd(display),
                .events = POLLIN,
            };
            if (poll(&pfd, 1, 20) > 0) {
                if (wl_display_read_events(display) != 0) {
                    return -1;
                }
            } else {
                wl_display_cancel_read(display);
            }
        }
        /* 返回值是"分发了几个事件"，不是 0/1：只看是否 < 0 */
        if (wl_display_dispatch_pending(display) < 0) {
            return -1;
        }
        if (wl_display_get_error(display) != 0) {
            return -1;
        }
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&tick, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed =
            (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms) {
            return 0;
        }
    }
}

static void print_protocol_error(void)
{
    const struct wl_interface *iface = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
    (void)id;
    printf("PROTOCOL_ERROR %u %s\n", code, iface ? iface->name : "?");
    fflush(stdout);
}

/* ==================== handle 事件 ==================== */

static void handle_pid(void *data, struct treeland_foreign_toplevel_handle_v3 *h, uint32_t value)
{
    (void)data;
    (void)h;
    pid = value;
}

static void handle_title(void *data, struct treeland_foreign_toplevel_handle_v3 *h, const char *value)
{
    (void)data;
    (void)h;
    snprintf(title, sizeof(title), "%s", value ? value : "");
}

static void handle_app_id(void *data, struct treeland_foreign_toplevel_handle_v3 *h, const char *value)
{
    (void)data;
    (void)h;
    snprintf(app_id, sizeof(app_id), "%s", value ? value : "");
}

static void handle_identifier(void *data, struct treeland_foreign_toplevel_handle_v3 *h,
                              uint32_t value)
{
    (void)data;
    (void)h;
    identifier = value;
    have_identifier = 1;
}

static void handle_output_enter(void *data, struct treeland_foreign_toplevel_handle_v3 *h,
                                struct wl_output *o)
{
    (void)data;
    (void)h;
    (void)o;
    outputs++;
}

static void handle_output_leave(void *data, struct treeland_foreign_toplevel_handle_v3 *h,
                                struct wl_output *o)
{
    (void)data;
    (void)h;
    (void)o;
    if (outputs > 0) {
        outputs--;
    }
}

static void handle_state(void *data, struct treeland_foreign_toplevel_handle_v3 *h,
                         struct wl_array *values)
{
    (void)data;
    (void)h;
    state_len = 0;
    if (values && values->size % sizeof(uint32_t) == 0) {
        const uint32_t *vals = values->data;
        size_t n = values->size / sizeof(uint32_t);
        for (size_t i = 0; i < n && state_len < sizeof(state) / sizeof(state[0]); i++) {
            state[state_len++] = vals[i];
        }
    }
}

static void handle_done(void *data, struct treeland_foreign_toplevel_handle_v3 *h)
{
    (void)data;
    (void)h;
    char states[128];
    state_string(states, sizeof(states));
    done_count++;
    /* 一行一条快照：脚本按最后一次 PROPS 断言最终状态 */
    printf("PROPS ident=%u title=%s app=%s pid=%u state=%s outputs=%u done=%u\n",
           have_identifier ? identifier : 0, title, app_id, pid, states, outputs, done_count);
    fflush(stdout);
}

static void handle_closed(void *data, struct treeland_foreign_toplevel_handle_v3 *h)
{
    (void)data;
    (void)h;
    closed = 1;
    printf("CLOSED\n");
    fflush(stdout);
}

static void handle_parent(void *data, struct treeland_foreign_toplevel_handle_v3 *h,
                          struct treeland_foreign_toplevel_handle_v3 *parent)
{
    (void)data;
    (void)h;
    printf("PARENT %s\n", parent ? "set" : "null");
    fflush(stdout);
}

static const struct treeland_foreign_toplevel_handle_v3_listener handle_listener = {
    .pid = handle_pid,
    .title = handle_title,
    .app_id = handle_app_id,
    .identifier = handle_identifier,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

/* ==================== manager 事件 ==================== */

static void manager_toplevel(void *data, struct treeland_foreign_toplevel_manager_v3 *m,
                             struct treeland_foreign_toplevel_handle_v3 *new_handle)
{
    (void)data;
    (void)m;
    printf("TOPLEVEL\n");
    fflush(stdout);
    if (!handle) {
        handle = new_handle;
        treeland_foreign_toplevel_handle_v3_add_listener(handle, &handle_listener, NULL);
    }
}

static void manager_finished(void *data, struct treeland_foreign_toplevel_manager_v3 *m)
{
    (void)data;
    (void)m;
    printf("FINISHED\n");
    fflush(stdout);
}

static const struct treeland_foreign_toplevel_manager_v3_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

/* ==================== xdg / registry ==================== */

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)data;
    (void)t;
    printf("CLOSE\n");
    fflush(stdout);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
    (void)data;
    (void)t;
    (void)w;
    (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *c)
{
    (void)data;
    (void)t;
    (void)c;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        /* v1 就够：activate 只要一个 wl_seat 对象，用不上它的任何事件 */
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        /*
         * 绑 wl_output 是为了让 output_enter 有对象可发：协议规定 enter 的
         * 参数必须是客户端自己 bind 的 wl_output，没绑过就一条都收不到
         * （合成器那边也会因此暂时跳过，等客户端绑上再补）。
         */
        output = wl_registry_bind(reg, name, &wl_output_interface, version < 3 ? version : 3);
    } else if (strcmp(interface, treeland_foreign_toplevel_manager_v3_interface.name) == 0) {
        manager_name = name;
        manager_version = version < 1 ? version : 1;
        if (!late_bind) {
            manager = wl_registry_bind(reg, name,
                                       &treeland_foreign_toplevel_manager_v3_interface,
                                       manager_version);
            treeland_foreign_toplevel_manager_v3_add_listener(manager, &manager_listener, NULL);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ==================== buffer ==================== */

static struct wl_buffer *create_buffer(void)
{
    int stride = WIDTH * 4;
    int size = stride * HEIGHT;
    int fd = memfd_create("ftl-probe", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    /* 纯色即可：窗口只要有一块真 buffer 就能 map，内容不影响任何断言 */
    uint32_t *pixels = data;
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        pixels[i] = 0xff202020;
    }
    munmap(data, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ==================== 操作 ==================== */

static int wait_ready(int need_handle, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (configured && (!need_handle || done_count > 0)) {
            return 0;
        }
        if (pump(50) != 0) {
            print_protocol_error();
            return -1;
        }
        if (closed) {
            return -1;
        }
        waited += 50;
    }
    return -1;
}

static void do_show(uint32_t ident, int use_own)
{
    uint32_t ids[1];
    ids[0] = use_own ? identifier : ident;
    struct wl_array array;
    wl_array_init(&array);
    uint32_t *slot = wl_array_add(&array, sizeof(ids));
    if (slot) {
        memcpy(slot, ids, sizeof(ids));
    }
    treeland_dock_preview_context_v3_show(dock_ctx, &array, 10, 20,
                                          TREELAND_DOCK_PREVIEW_CONTEXT_V3_DIRECTION_BOTTOM);
    wl_array_release(&array);
    pump(100);
}

int main(int argc, char *argv[])
{
    int hold_ms = DEFAULT_HOLD_MS;
    int want_dup_context = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--late-bind") == 0) {
            late_bind = 1;
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        }
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to compositor\n");
        return 1;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    /* 第二轮不是保险：registry 回调里的 bind 是在第一轮分发时才排进发送队列的，
     * 第一轮等的那次 sync 早于 bind 发出，服务端当时还没看见它 */
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "missing wl_compositor / wl_shm / xdg_wm_base\n");
        return 1;
    }
    if (!manager && !manager_name) {
        fprintf(stderr, "treeland_foreign_toplevel_manager_v3 not advertised\n");
        return 77;
    }

    /* 建窗口并 map 上去 */
    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "ftl-probe");
    xdg_toplevel_set_app_id(toplevel, "ftl.probe");
    wl_surface_commit(surface);
    if (wait_ready(0, 3000) != 0) {
        fprintf(stderr, "no xdg_surface configure in 3s\n");
        return 1;
    }
    buffer = create_buffer();
    if (!buffer) {
        fprintf(stderr, "cannot create shm buffer\n");
        return 1;
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);

    /*
     * --late-bind：窗口先 map（服务端已把它放进视图链表），再 bind manager。
     * 这里必须 roundtrip 一次——不然 bind 和 map 会挤在同一批请求里，
     * "补快照"那条路径就没被覆盖到。
     */
    if (late_bind) {
        wl_display_roundtrip(display);
        manager = wl_registry_bind(registry, manager_name,
                                   &treeland_foreign_toplevel_manager_v3_interface,
                                   manager_version);
        treeland_foreign_toplevel_manager_v3_add_listener(manager, &manager_listener, NULL);
    }

    if (!manager) {
        fprintf(stderr, "manager was never bound\n");
        return 1;
    }
    if (wait_ready(1, 3000) != 0) {
        fprintf(stderr, "no toplevel handle in 3s (done=%u)\n", done_count);
        return 1;
    }
    printf("READY\n");
    fflush(stdout);

    /*
     * dup-context：同一个 surface 上的第二个 context 必须被
     * context_already_exists 打掉。单独立项是因为它会把连接打断，
     * 放到操作列表里就没法继续跑别的用例。
     */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "dup-context") == 0) {
            want_dup_context = 1;
        }
    }
    if (want_dup_context) {
        struct treeland_dock_preview_context_v3 *first =
            treeland_foreign_toplevel_manager_v3_get_dock_preview_context(manager, surface);
        (void)first;
        treeland_foreign_toplevel_manager_v3_get_dock_preview_context(manager, surface);
        wl_display_flush(display);
        if (pump(1000) == 0) {
            fprintf(stderr, "FAIL: duplicate dock preview context was accepted\n");
            return 1;
        }
        print_protocol_error();
        return 3;
    }

    /* 需要 dock context 的操作先建一个（锚点用窗口自己的 surface） */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "show") == 0 || strcmp(argv[i], "tooltip-empty") == 0) {
            dock_ctx =
                treeland_foreign_toplevel_manager_v3_get_dock_preview_context(manager, surface);
            break;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--late-bind") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "dup-context") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "show") == 0) {
            /* 带 id 就是查一个不存在的 identifier（协议要求静默忽略），
             * 不带就是查自己的窗口（真实预览路径） */
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                do_show((uint32_t)atoi(argv[++i]), 0);
            } else {
                do_show(0, 1);
            }
        } else if (strcmp(argv[i], "tooltip-empty") == 0) {
            /* 没有提交过内容的 surface：协议规定"什么都不显示"，不是错误 */
            bare_surface = wl_compositor_create_surface(compositor);
            treeland_dock_preview_context_v3_show_tooltip_surface(
                dock_ctx, bare_surface, 3, 4, TREELAND_DOCK_PREVIEW_CONTEXT_V3_DIRECTION_LEFT);
            wl_display_flush(display);
            pump(150);
            printf("TOOLTIP_EMPTY_SENT\n");
            fflush(stdout);
        } else if (strcmp(argv[i], "max") == 0) {
            treeland_foreign_toplevel_handle_v3_set_maximized(handle);
        } else if (strcmp(argv[i], "unmax") == 0) {
            treeland_foreign_toplevel_handle_v3_unset_maximized(handle);
        } else if (strcmp(argv[i], "min") == 0) {
            treeland_foreign_toplevel_handle_v3_set_minimized(handle);
        } else if (strcmp(argv[i], "unmin") == 0) {
            treeland_foreign_toplevel_handle_v3_unset_minimized(handle);
        } else if (strcmp(argv[i], "full") == 0) {
            treeland_foreign_toplevel_handle_v3_set_fullscreen(handle, NULL);
        } else if (strcmp(argv[i], "unfull") == 0) {
            treeland_foreign_toplevel_handle_v3_unset_fullscreen(handle);
        } else if (strcmp(argv[i], "icon") == 0 && i + 4 < argc) {
            int x = atoi(argv[++i]);
            int y = atoi(argv[++i]);
            int w = atoi(argv[++i]);
            int h = atoi(argv[++i]);
            treeland_foreign_toplevel_handle_v3_set_icon_geometry(handle, surface, x, y, w, h);
        } else if (strcmp(argv[i], "activate") == 0) {
            if (seat) {
                treeland_foreign_toplevel_handle_v3_activate(handle, seat);
            }
        } else if (strcmp(argv[i], "close") == 0) {
            treeland_foreign_toplevel_handle_v3_close(handle);
        } else if (strcmp(argv[i], "stop") == 0) {
            treeland_foreign_toplevel_manager_v3_stop(manager);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 1;
        }
        wl_display_flush(display);
        /* 每个操作后给服务端一个处理窗口：状态事件是异步回来的 */
        if (pump(120) != 0) {
            print_protocol_error();
            return 3;
        }
    }

    /* 所有操作都发出去了，且每个都留过处理窗口：脚本可以开始查状态了 */
    printf("OPS_DONE\n");
    fflush(stdout);

    if (pump(hold_ms) != 0) {
        print_protocol_error();
        return 3;
    }
    printf("DONE closed=%d done_count=%u\n", closed, done_count);
    fflush(stdout);

    /* 退出前把本进程建的对象全放掉：ASan 构建会把泄漏算进冒烟结果 */
    if (dock_ctx) {
        treeland_dock_preview_context_v3_destroy(dock_ctx);
    }
    if (bare_surface) {
        wl_surface_destroy(bare_surface);
    }
    if (handle) {
        treeland_foreign_toplevel_handle_v3_destroy(handle);
    }
    if (manager) {
        treeland_foreign_toplevel_manager_v3_destroy(manager);
    }
    wl_buffer_destroy(buffer);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(wm_base);
    /* 这三个没有可靠的析构请求（wl_shm/wl_seat 的 release 要求高版本、
     * wl_output 的 release 是 v3+），本地释放即可，退出时不必再发请求 */
    wl_proxy_destroy((struct wl_proxy *)shm);
    if (seat) {
        wl_proxy_destroy((struct wl_proxy *)seat);
    }
    if (output) {
        wl_proxy_destroy((struct wl_proxy *)output);
    }
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
