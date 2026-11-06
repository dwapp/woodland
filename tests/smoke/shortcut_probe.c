// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-shortcut-manager-unstable-v3 的最小客户端探针。
 *
 * 用法：shortcut_probe <mode> [args...]
 *
 * 每次进程只跑一个场景：冒烟脚本在两步之间用 wctl 查合成器状态，所以不能
 * 把整个序列塞进一个进程。探针自己断言「收到了什么事件 / 什么协议错误」，
 * 合成器侧的状态（binding_count、capture_pending、last_outcome、trace）
 * 交给脚本断言。
 *
 * 模式：
 *   bind                           bind + destroy
 *   acquire                        bind + acquire + destroy
 *   bind-key N K F A               acquire + bind_key + unbind
 *   bind-failure N K F A E         acquire + bind_key，期望 bind_failure(E)
 *   name-conflict                  同名 bind_key 两次，期望 name_conflict
 *   duplicate-binding              同 (finger,direction,action) swipe 两次
 *   upsert                         同 key+action 不同名，期望无 failure（更新 flags）
 *   gesture                        swipe + hold 都注册成功（只登记不触发）
 *   not-acquired                   不 acquire 就 bind_key，期望协议错误 2
 *   occupied                       两个 manager 都 acquire，期望协议错误 1
 *   capture                        map 一个 toplevel 后发起 capture，打印结果
 *   capture-busy                   连发两次 capture，打印两次结果
 *   capture-destroy                capture 后 destroy 掉对象（取消，无终端事件）
 *   capture-not-active             bare surface（不 map）发起 capture，期望 failed(3)
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "treeland-shortcut-manager-unstable-v3-client-protocol.h"
#include "shortcut-xdg-shell-client-protocol.h"

#define MAX_CAPTURES 4

struct probe_state {
    struct wl_display *display;
    struct wl_registry *registry;
    uint32_t manager_global_name;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct treeland_shortcut_manager_v3 *manager[2];
    bool manager_destroyed[2];
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct treeland_shortcut_capture_v3 *caps[MAX_CAPTURES];
    bool cap_destroyed[MAX_CAPTURES];
    int ncaps;

    int failures;
    uint32_t failure_error;
    char failure_name[128];
    int activated;
    int captured_count;
    int failed_count;
    int terminal_before;
    uint32_t reasons[MAX_CAPTURES];
    char captured_key[128];
};

static void manager_handle_activated(void *data,
                                     struct treeland_shortcut_manager_v3 *manager,
                                     const char *name, uint32_t flags)
{
    struct probe_state *st = data;
    (void)manager;
    (void)name;
    (void)flags;
    st->activated++;
}

static void manager_handle_bind_failure(void *data,
                                        struct treeland_shortcut_manager_v3 *manager,
                                        const char *name, uint32_t error)
{
    struct probe_state *st = data;
    (void)manager;
    st->failures++;
    st->failure_error = error;
    snprintf(st->failure_name, sizeof(st->failure_name), "%s", name ? name : "");
}

static const struct treeland_shortcut_manager_v3_listener manager_listener = {
    .activated = manager_handle_activated,
    .bind_failure = manager_handle_bind_failure,
};

static void capture_handle_captured(void *data,
                                   struct treeland_shortcut_capture_v3 *capture,
                                   const char *key)
{
    struct probe_state *st = data;
    (void)capture;
    st->captured_count++;
    snprintf(st->captured_key, sizeof(st->captured_key), "%s", key ? key : "");
}

static void capture_handle_failed(void *data,
                                  struct treeland_shortcut_capture_v3 *capture,
                                  uint32_t reason)
{
    struct probe_state *st = data;
    (void)capture;
    st->failed_count++;
    if (st->failed_count <= MAX_CAPTURES) {
        st->reasons[st->failed_count - 1] = reason;
    }
}

static const struct treeland_shortcut_capture_v3_listener capture_listener = {
    .captured = capture_handle_captured,
    .failed = capture_handle_failed,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface,
                                    uint32_t version)
{
    struct probe_state *st = data;
    if (strcmp(interface, treeland_shortcut_manager_v3_interface.name) == 0) {
        st->manager_global_name = name;
        st->manager[0] = wl_registry_bind(registry, name,
            &treeland_shortcut_manager_v3_interface, 1);
        treeland_shortcut_manager_v3_add_listener(st->manager[0],
                                                  &manager_listener, st);
    } else if (strcmp(interface, "wl_compositor") == 0) {
        st->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
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

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                   uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static int report_error(struct wl_display *display, const char *where)
{
    int err = wl_display_get_error(display);
    const struct wl_interface *iface = NULL;
    uint32_t object_id = 0;
    uint32_t code = 0;
    /* wl_display_get_error() 给的是 EPROTO；错误码要问三参数版本 */
    if (err == EPROTO) {
        code = wl_display_get_protocol_error(display, &iface, &object_id);
    }
    fprintf(stderr, "%s: display error %d (EPROTO=%d), protocol error %u on %s (id %u)\n",
            where, err, EPROTO, code, iface ? iface->name : "-", object_id);
    return 1;
}

/** map 一个普通 toplevel，让 woodland 给它键盘焦点（capture 的 not_active 判定） */
static bool make_toplevel(struct probe_state *st)
{
    if (!st->compositor || !st->wm_base) {
        fprintf(stderr, "missing wl_compositor / xdg_wm_base\n");
        return false;
    }
    xdg_wm_base_add_listener(st->wm_base, &wm_base_listener, st);
    st->surface = wl_compositor_create_surface(st->compositor);
    st->xdg_surface = xdg_wm_base_get_xdg_surface(st->wm_base, st->surface);
    xdg_surface_add_listener(st->xdg_surface, &xdg_surface_listener, st);
    st->toplevel = xdg_surface_get_toplevel(st->xdg_surface);
    wl_surface_commit(st->surface);
    for (int i = 0; i < 6; i++) {
        if (wl_display_roundtrip(st->display) < 0) {
            return false;
        }
    }
    return true;
}

static struct treeland_shortcut_capture_v3 *new_capture(struct probe_state *st)
{
    if (st->ncaps >= MAX_CAPTURES) {
        return NULL;
    }
    struct treeland_shortcut_capture_v3 *cap =
        treeland_shortcut_manager_v3_capture_next_shortcut(st->manager[0],
                                                           st->surface, NULL);
    if (!cap) {
        return NULL;
    }
    treeland_shortcut_capture_v3_add_listener(cap, &capture_listener, st);
    st->caps[st->ncaps] = cap;
    st->ncaps++;
    return cap;
}

static void cleanup(struct probe_state *st)
{
    /* destructor 请求自带 WL_MARSHAL_FLAG_DESTROY，已经销毁本地 proxy */
    for (int i = 0; i < st->ncaps; i++) {
        if (!st->cap_destroyed[i] && st->caps[i]) {
            wl_proxy_destroy((struct wl_proxy *)st->caps[i]);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (st->manager[i] && !st->manager_destroyed[i]) {
            wl_proxy_destroy((struct wl_proxy *)st->manager[i]);
        }
    }
    if (st->toplevel) {
        wl_proxy_destroy((struct wl_proxy *)st->toplevel);
    }
    if (st->xdg_surface) {
        wl_proxy_destroy((struct wl_proxy *)st->xdg_surface);
    }
    if (st->surface) {
        wl_proxy_destroy((struct wl_proxy *)st->surface);
    }
    if (st->wm_base) {
        wl_proxy_destroy((struct wl_proxy *)st->wm_base);
    }
    if (st->compositor) {
        wl_proxy_destroy((struct wl_proxy *)st->compositor);
    }
    if (st->registry) {
        wl_registry_destroy(st->registry);
    }
    if (st->display) {
        wl_display_disconnect(st->display);
    }
}

static int expect_no_failure(struct probe_state *st, const char *mode)
{
    if (st->failures != 0) {
        fprintf(stderr, "%s: unexpected bind_failure(%s, %u)\n", mode,
                st->failure_name, st->failure_error);
        return 1;
    }
    return 0;
}

static int expect_failure(struct probe_state *st, const char *mode, uint32_t error)
{
    if (st->failures != 1 || st->failure_error != error) {
        fprintf(stderr, "%s: expected bind_failure(%u) once, got %d failure(s), "
                "last error %u (%s)\n", mode, error, st->failures,
                st->failure_error, st->failure_name);
        return 1;
    }
    return 0;
}

static int expect_protocol_error(struct probe_state *st, const char *mode,
                                 uint32_t error)
{
    /* roundtrip 的返回值是事件数，判错只看 display 状态 */
    wl_display_roundtrip(st->display);
    int err = wl_display_get_error(st->display);
    const struct wl_interface *iface = NULL;
    uint32_t object_id = 0;
    uint32_t code = 0;
    if (err == EPROTO) {
        code = wl_display_get_protocol_error(st->display, &iface, &object_id);
    }
    if (err != EPROTO || code != error) {
        fprintf(stderr, "%s: expected protocol error %u, got display %d code %u on %s\n",
                mode, error, err, code, iface ? iface->name : "-");
        return 1;
    }
    return 0;
}

static uint32_t parse_u32(const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) {
        fprintf(stderr, "invalid uint32: %s\n", text);
        exit(2);
    }
    return (uint32_t)value;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mode> [args...]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];

    struct probe_state st = {0};
    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }
    int rc = 0;
    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    if (wl_display_roundtrip(st.display) < 0) {
        rc = report_error(st.display, "registry roundtrip");
        goto done;
    }
    if (!st.manager[0]) {
        fprintf(stderr, "treeland_shortcut_manager_v3 global not advertised\n");
        rc = 1;
        goto done;
    }

    if (strcmp(mode, "bind") == 0) {
        treeland_shortcut_manager_v3_destroy(st.manager[0]);
        st.manager_destroyed[0] = true;
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "bind destroy");
            goto done;
        }
    } else if (strcmp(mode, "acquire") == 0) {
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_destroy(st.manager[0]);
        st.manager_destroyed[0] = true;
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "acquire destroy");
            goto done;
        }
    } else if (strcmp(mode, "bind-key") == 0 && argc == 6) {
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], argv[2], argv[3],
                                             parse_u32(argv[4]), parse_u32(argv[5]));
        treeland_shortcut_manager_v3_unbind(st.manager[0], argv[2]);
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "bind-key roundtrip");
            goto done;
        }
        rc = expect_no_failure(&st, mode);
    } else if (strcmp(mode, "bind-failure") == 0 && argc == 7) {
        uint32_t expected = parse_u32(argv[6]);
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], argv[2], argv[3],
                                             parse_u32(argv[4]), parse_u32(argv[5]));
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "bind-failure roundtrip");
            goto done;
        }
        rc = expect_failure(&st, mode, expected);
    } else if (strcmp(mode, "name-conflict") == 0) {
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], "dup", "Ctrl+Alt+T", 1, 0);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], "dup", "Ctrl+Alt+U", 1, 0);
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "name-conflict roundtrip");
            goto done;
        }
        rc = expect_failure(&st, mode, 1 /* name_conflict */);
    } else if (strcmp(mode, "duplicate-binding") == 0) {
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_swipe_gesture(st.manager[0], "swipe-a", 3, 1, 1);
        treeland_shortcut_manager_v3_bind_swipe_gesture(st.manager[0], "swipe-b", 3, 1, 1);
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "duplicate-binding roundtrip");
            goto done;
        }
        rc = expect_failure(&st, mode, 2 /* duplicate_binding */);
    } else if (strcmp(mode, "upsert") == 0) {
        /*
         * 绑定不随客户端断开而消失（协议：destroy 只释放独占），所以别用
         * name-conflict 留下的 "Ctrl+Alt+T" —— 那会变成跨进程的 upsert。
         */
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], "first", "Ctrl+Alt+U", 1, 0);
        treeland_shortcut_manager_v3_bind_key(st.manager[0], "second", "Ctrl+Alt+U", 2, 0);
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "upsert roundtrip");
            goto done;
        }
        rc = expect_no_failure(&st, mode);
    } else if (strcmp(mode, "gesture") == 0) {
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_bind_swipe_gesture(st.manager[0], "swipe", 4, 3, 2);
        treeland_shortcut_manager_v3_bind_hold_gesture(st.manager[0], "hold", 3, 3);
        if (wl_display_roundtrip(st.display) < 0) {
            rc = report_error(st.display, "gesture roundtrip");
            goto done;
        }
        rc = expect_no_failure(&st, mode);
    } else if (strcmp(mode, "not-acquired") == 0) {
        treeland_shortcut_manager_v3_bind_key(st.manager[0], "no-owner", "Ctrl+A", 1, 0);
        rc = expect_protocol_error(&st, mode, 2 /* not_acquired */);
        goto done;
    } else if (strcmp(mode, "occupied") == 0) {
        /* 同一 client 的第二个 manager：owner 已存在，第二次 acquire 必须 occupied */
        st.manager[1] = wl_registry_bind(st.registry, st.manager_global_name,
            &treeland_shortcut_manager_v3_interface, 1);
        if (!st.manager[1]) {
            fprintf(stderr, "cannot bind a second shortcut manager\n");
            rc = 1;
            goto done;
        }
        treeland_shortcut_manager_v3_add_listener(st.manager[1],
                                                 &manager_listener, &st);
        treeland_shortcut_manager_v3_acquire(st.manager[0]);
        treeland_shortcut_manager_v3_acquire(st.manager[1]);
        rc = expect_protocol_error(&st, mode, 1 /* occupied */);
        goto done;
    } else if (strcmp(mode, "capture") == 0 ||
               strcmp(mode, "capture-busy") == 0 ||
               strcmp(mode, "capture-destroy") == 0 ||
               strcmp(mode, "capture-not-active") == 0) {
        bool focused = strcmp(mode, "capture-not-active") != 0;
        if (focused) {
            if (!make_toplevel(&st)) {
                rc = 1;
                goto done;
            }
        } else {
            st.surface = wl_compositor_create_surface(st.compositor);
            wl_surface_commit(st.surface);
            wl_display_roundtrip(st.display);
        }
        if (!new_capture(&st)) {
            fprintf(stderr, "capture_next_shortcut failed to create a proxy\n");
            rc = 1;
            goto done;
        }
        if (strcmp(mode, "capture-busy") == 0 && !new_capture(&st)) {
            fprintf(stderr, "second capture_next_shortcut failed\n");
            rc = 1;
            goto done;
        }
        if (strcmp(mode, "capture-destroy") == 0) {
            /*
             * 先收一次「立即 failed(not_active)」这类终端事件，记住基线，
             * 再发 destroy：断言的是「destroy 之后没有新增终端事件」，
             * 而不是「一条终端事件都没有」——headless 下常常本来就没有
             * 键盘焦点，第一条会立刻失败，这不属于 destroy 的语义。
             */
            wl_display_roundtrip(st.display);
            st.terminal_before = st.failed_count + st.captured_count;
            treeland_shortcut_capture_v3_destroy(st.caps[0]);
            st.cap_destroyed[0] = true;
        }
        wl_display_roundtrip(st.display);
        if (wl_display_get_error(st.display) != 0) {
            rc = report_error(st.display, mode);
            goto done;
        }
        if (strcmp(mode, "capture-not-active") == 0 &&
            (st.failed_count != 1 || st.reasons[0] != 3)) {
            fprintf(stderr, "%s: expected failed(not_active=3), got %d failed, reason %u\n",
                    mode, st.failed_count, st.reasons[0]);
            rc = 1;
            goto done;
        }
        if (strcmp(mode, "capture-destroy") == 0 &&
            st.failed_count + st.captured_count != st.terminal_before) {
            fprintf(stderr, "%s: destroy emitted a terminal event after cancellation\n", mode);
            rc = 1;
            goto done;
        }
        printf("shortcut_probe: mode=%s captured=%d failed=%d reason=%u,%u key=%s\n",
               mode, st.captured_count, st.failed_count,
               st.reasons[0], st.reasons[1], st.captured_key);
    } else {
        fprintf(stderr, "unknown mode or wrong argument count: %s\n", mode);
        rc = 2;
        goto done;
    }

    if (rc == 0 && wl_display_get_error(st.display) != 0) {
        rc = report_error(st.display, mode);
    }

done:
    cleanup(&st);
    if (rc == 0) {
        printf("shortcut_probe: %s ok (failures=%d activated=%d)\n",
               mode, st.failures, st.activated);
    }
    return rc;
}
