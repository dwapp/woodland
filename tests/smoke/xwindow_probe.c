// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-xwindow-control-unstable-v1 的最小客户端探针。
 *
 * 用法：
 *   xwindow_probe --bind
 *   xwindow_probe --destroy
 *   xwindow_probe --move bare   <wid> <dx> <dy>
 *   xwindow_probe --move mapped <wid> <dx> <dy>
 *
 * 每次只发一条请求（--bind 一条都不发），冒烟脚本在两次调用之间用 wctl
 * 检查合成器状态。"bare" 是没分配角色、没 attach buffer 的裸 wl_surface
 * （场景里没有节点，协议应回 done(1)）；"mapped" 会真建一个 xdg toplevel
 * 并 attach shm buffer，作为合法 anchor。
 *
 * 收到 wl_callback.done 后把结果打印成 "callback:<0|1>"，断言交给脚本。
 *
 * 两个必须照抄的细节（见 AGENTS.md 冒烟陷阱）：
 *   1) registry 回调里发出的 bind 要第二次 roundtrip 才会被 flush；
 *   2) 收 done 事件走 prepare_read / poll / read_events / dispatch_pending，
 *      只调 dispatch_pending 不会读 socket。
 * 退出前显式销毁所有 proxy，ASan 会把残留的 wl_proxy 判成泄漏。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "treeland-xwindow-control-unstable-v1-client-protocol.h"
#include "xwindow-xdg-shell-client-protocol.h"

#define ANCHOR_SIZE 64

enum run_mode {
    MODE_BIND,
    MODE_DESTROY,
    MODE_MOVE,
};

struct probe_state {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct treeland_xwindow_control_v1 *control;
    bool found_control;
};

static struct wl_display *display;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static bool configured;
static bool callback_received;
static uint32_t callback_result;

/* ==================== registry / xdg ==================== */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct probe_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, treeland_xwindow_control_v1_interface.name) == 0) {
        state->found_control = true;
        state->control = wl_registry_bind(registry, name,
            &treeland_xwindow_control_v1_interface, 1);
    }
    (void)version;
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                           uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w,
                               int32_t h, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)w;
    (void)h;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void callback_done(void *data, struct wl_callback *callback, uint32_t result)
{
    (void)data;
    callback_received = true;
    callback_result = result;
    /* destructor 事件之后本地 proxy 由客户端负责释放（同 tests/unit 里的 sync） */
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener callback_listener = {
    .done = callback_done,
};

/* ==================== 工具 ==================== */

static int dispatch_until(bool *flag, int timeout_ms, const char *what);

static long long now_ms(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int report_error(const char *where)
{
    int err = wl_display_get_error(display);
    const struct wl_interface *iface = NULL;
    uint32_t object_id = 0, code = 0;
    /* wl_display_get_protocol_error() 是 3 个参数；get_error() 给的是 EPROTO */
    if (err == EPROTO) {
        code = wl_display_get_protocol_error(display, &iface, &object_id);
    }
    fprintf(stderr, "%s: display error %d (EPROTO=%d), protocol error %u on %s (id %u)\n",
            where, err, EPROTO, code, iface ? iface->name : "-", object_id);
    return 1;
}

static struct wl_buffer *create_buffer(struct wl_shm *shm)
{
    int stride = ANCHOR_SIZE * 4;
    int size = stride * ANCHOR_SIZE;
    int fd = memfd_create("xwindow-probe", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int i = 0; i < ANCHOR_SIZE * ANCHOR_SIZE; i++) {
        pixels[i] = 0xFF224466;
    }
    munmap(pixels, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, ANCHOR_SIZE, ANCHOR_SIZE, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

/* 建一个真正 mapped 的 xdg toplevel surface，作为合法 anchor */
static struct wl_surface *create_mapped_anchor(struct probe_state *state,
                                               struct wl_buffer **buffer_out)
{
    struct wl_surface *surface = wl_compositor_create_surface(state->compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(state->wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "xwindow-probe-anchor");

    wl_surface_commit(surface);
    /* 别裸等 wl_display_dispatch()：它阻塞且无超时，合成器没回 configure
     * 时整个冒烟会一直挂到 meson 的测试超时才被杀。 */
    if (dispatch_until(&configured, 3000, "xdg_surface.configure") != 0) {
        return NULL;
    }
    struct wl_buffer *buffer = create_buffer(state->shm);
    if (!buffer) {
        return NULL;
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, ANCHOR_SIZE, ANCHOR_SIZE);
    wl_surface_commit(surface);
    /* 让合成器把 map / commit 处理完，anchor 才有 scene buffer 节点 */
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        return NULL;
    }
    *buffer_out = buffer;
    return surface;
}

/* 等某个事件回调把 flag 置上。configure 和 wl_callback.done 共用这一套：
 * 只调 dispatch_pending 不会读 socket，必须 prepare_read / poll /
 * read_events / dispatch_pending；且一律带超时，别让冒烟脚本挂到 meson
 * 的 120s 超时才被杀掉。 */
static int dispatch_until(bool *flag, int timeout_ms, const char *what)
{
    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
    long long deadline = now_ms() + timeout_ms;
    while (!*flag && now_ms() < deadline) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                return report_error("dispatch pending");
            }
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return report_error("flush");
        }
        int left = (int)(deadline - now_ms());
        int ret = poll(&pfd, 1, left > 0 ? left : 0);
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
                return report_error("read events");
            }
        } else {
            wl_display_cancel_read(display);
        }
        /* 返回分发了几个事件，判错只看 < 0 */
        if (wl_display_dispatch_pending(display) < 0) {
            return report_error("dispatch");
        }
    }
    if (!*flag) {
        fprintf(stderr, "timed out waiting for %s\n", what);
        return 1;
    }
    return 0;
}

static int wait_callback(int timeout_ms)
{
    if (dispatch_until(&callback_received, timeout_ms, "wl_callback.done") != 0) {
        return 1;
    }
    printf("callback:%u\n", callback_result);
    return 0;
}

/* ==================== 主流程 ==================== */

int main(int argc, char **argv)
{
    enum run_mode mode;
    bool mapped_anchor = false;
    uint32_t wid = 0;
    int dx = 0, dy = 0;

    if (argc == 2 && strcmp(argv[1], "--bind") == 0) {
        mode = MODE_BIND;
    } else if (argc == 2 && strcmp(argv[1], "--destroy") == 0) {
        mode = MODE_DESTROY;
    } else if (argc == 6 && strcmp(argv[1], "--move") == 0 &&
               (strcmp(argv[2], "bare") == 0 || strcmp(argv[2], "mapped") == 0)) {
        mode = MODE_MOVE;
        mapped_anchor = strcmp(argv[2], "mapped") == 0;
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[3], &end, 0); /* 允许 0x... 形式的 wid */
        if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX) {
            fprintf(stderr, "invalid wid: %s\n", argv[3]);
            return 2;
        }
        wid = (uint32_t)parsed;
        dx = atoi(argv[4]);
        dy = atoi(argv[5]);
    } else {
        fprintf(stderr,
                "usage: %s --bind | --destroy | --move <bare|mapped> <wid> <dx> <dy>\n",
                argv[0]);
        return 2;
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }

    int rc = 1;
    bool manager_destroyed = false;
    struct wl_surface *anchor = NULL;
    struct wl_buffer *buffer = NULL;
    struct probe_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);

    /* 两次 roundtrip：第一次拿 global，第二次才把排队中的 bind flush 出去 */
    if (wl_display_roundtrip(display) < 0) {
        rc = report_error("registry roundtrip");
        goto cleanup;
    }
    if (!state.found_control || !state.control) {
        fprintf(stderr, "treeland_xwindow_control_v1 global not advertised\n");
        rc = 1;
        goto cleanup;
    }
    if (state.wm_base) {
        xdg_wm_base_add_listener(state.wm_base, &wm_base_listener, NULL);
    }
    if (wl_display_roundtrip(display) < 0) {
        rc = report_error("bind roundtrip");
        goto cleanup;
    }
    if (mode == MODE_MOVE && (mapped_anchor && (!state.compositor || !state.shm ||
                                                !state.wm_base))) {
        fprintf(stderr, "missing compositor / shm / xdg_wm_base for mapped anchor\n");
        rc = 1;
        goto cleanup;
    }
    if (mode == MODE_MOVE && !mapped_anchor && !state.compositor) {
        fprintf(stderr, "missing wl_compositor\n");
        rc = 1;
        goto cleanup;
    }

    if (mode == MODE_BIND) {
        rc = 0;
        printf("bind ok\n");
        goto cleanup;
    }
    if (mode == MODE_DESTROY) {
        /* destructor 请求同时销毁本地 proxy，别再 destroy 一次 */
        treeland_xwindow_control_v1_destroy(state.control);
        manager_destroyed = true;
        if (wl_display_roundtrip(display) < 0) {
            rc = report_error("destroy roundtrip");
            goto cleanup;
        }
        rc = 0;
        printf("destroy ok\n");
        goto cleanup;
    }

    if (mapped_anchor) {
        anchor = create_mapped_anchor(&state, &buffer);
    } else {
        anchor = wl_compositor_create_surface(state.compositor);
    }
    if (!anchor) {
        fprintf(stderr, "cannot create anchor wl_surface\n");
        rc = 1;
        goto cleanup;
    }

    struct wl_callback *callback =
        treeland_xwindow_control_v1_set_xwindow_position_relative(
            state.control, wid, anchor, wl_fixed_from_int(dx), wl_fixed_from_int(dy));
    if (!callback) {
        fprintf(stderr, "set_xwindow_position_relative returned no callback\n");
        rc = 1;
        goto cleanup;
    }
    wl_callback_add_listener(callback, &callback_listener, NULL);
    if (wl_display_flush(display) < 0) {
        rc = report_error("request flush");
        goto cleanup;
    }
    rc = wait_callback(3000);
    if (rc == 0 && wl_display_get_error(display) != 0) {
        rc = report_error("after callback");
    }

cleanup:
    if (buffer) {
        wl_buffer_destroy(buffer);
    }
    if (xdg_toplevel) {
        xdg_toplevel_destroy(xdg_toplevel);
    }
    if (xdg_surface) {
        xdg_surface_destroy(xdg_surface);
    }
    if (anchor) {
        wl_surface_destroy(anchor);
    }
    if (state.compositor) {
        wl_proxy_destroy((struct wl_proxy *)state.compositor);
    }
    if (state.shm) {
        wl_proxy_destroy((struct wl_proxy *)state.shm);
    }
    if (state.wm_base) {
        wl_proxy_destroy((struct wl_proxy *)state.wm_base);
    }
    if (state.control && !manager_destroyed) {
        wl_proxy_destroy((struct wl_proxy *)state.control);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return rc;
}
