// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-compositor-action 的最小客户端探针。
 *
 * 用法： action_probe --trigger <uint32> | --destroy
 *
 * 每次只发一条请求：冒烟脚本在每两条之间用 wctl 检查合成器状态，所以
 * 不能把整个动作序列塞进一个进程里。trigger 是 fire-and-forget，协议没有
 * event，也没有错误枚举——能断言的只有"连接没被杀、没收到协议错误"。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "treeland-compositor-action-unstable-v1-client-protocol.h"

struct probe_state {
    struct treeland_compositor_action_v1 *action;
    bool found;
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct probe_state *state = data;
    if (strcmp(interface, treeland_compositor_action_v1_interface.name) == 0) {
        state->found = true;
        state->action = wl_registry_bind(registry, name,
            &treeland_compositor_action_v1_interface, 1);
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

static int report_error(struct wl_display *display, const char *where)
{
    int err = wl_display_get_error(display);
    const struct wl_interface *iface = NULL;
    uint32_t protocol_code = 0;
    uint32_t object_id = 0;
    /*
     * wl_display_get_error() 返回的是 libwayland 的 errno（协议错误恒为
     * EPROTO=71）；真正的协议错误码要问 wl_display_get_protocol_error()。
     */
    if (err == EPROTO) {
        protocol_code = wl_display_get_protocol_error(display, &iface, &object_id);
    }
    fprintf(stderr, "%s: display error %d (EPROTO=%d), protocol error %u on %s (id %u)\n",
            where, err, EPROTO, protocol_code, iface ? iface->name : "-", object_id);
    return 1;
}

int main(int argc, char **argv)
{
    bool do_trigger = false;
    bool do_destroy = false;
    uint32_t value = 0;

    if (argc == 3 && strcmp(argv[1], "--trigger") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX) {
            fprintf(stderr, "invalid action value: %s\n", argv[2]);
            return 2;
        }
        value = (uint32_t)parsed;
        do_trigger = true;
    } else if (argc == 2 && strcmp(argv[1], "--destroy") == 0) {
        do_destroy = true;
    } else {
        fprintf(stderr, "usage: %s --trigger <uint32> | --destroy\n", argv[0]);
        return 2;
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }

    int rc = 0;
    bool action_proxy_destroyed = false;
    struct probe_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);

    /*
     * roundtrip 的返回值是"分发了几个事件"，不是 0/1：判错只看 < 0。
     * 把它当布尔会在收到第一个事件后立刻误报协议错误（见 AGENTS.md 陷阱 6）。
     */
    if (wl_display_roundtrip(display) < 0) {
        rc = report_error(display, "registry roundtrip");
        goto cleanup;
    }
    if (!state.found || !state.action) {
        fprintf(stderr, "treeland_compositor_action_v1 global not advertised\n");
        rc = 1;
        goto cleanup;
    }

    if (do_trigger) {
        treeland_compositor_action_v1_trigger(state.action, value);
    }
    if (do_destroy) {
        /* destructor 请求同时销毁本地 proxy（WL_MARSHAL_FLAG_DESTROY） */
        treeland_compositor_action_v1_destroy(state.action);
        action_proxy_destroyed = true;
    }
    if (wl_display_roundtrip(display) < 0) {
        rc = report_error(display, "request roundtrip");
        goto cleanup;
    }
    if (wl_display_get_error(display) != 0) {
        rc = report_error(display, "after roundtrip");
        goto cleanup;
    }
    printf("action_probe: %s ok\n", do_trigger ? "trigger" : "destroy");

cleanup:
    /*
     * LSan 会把没销毁的 proxy 当成泄漏。trigger 模式不再补发一条 destroy
     * 请求（脚本按"每次探针只发一条请求"计数），只在本地释放即可。
     */
    if (state.action && !action_proxy_destroyed) {
        wl_proxy_destroy((struct wl_proxy *)state.action);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return rc;
}
