// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

#include "cursor.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "woodland.h"

/*
 * linux-drm-syncobj-v1 接线的确定性验证。
 *
 * 关键点：协议全局只有在 renderer / backend 都支持 timeline 且能拿到 DRM
 * fd 时才会暴露。这里强制 headless + pixman——pixman 没有 timeline 特性、
 * get_drm_fd 返回 -1，所以全局必须不存在。反之若不 pin 后端，测试机会
 * 继承外层 WAYLAND_DISPLAY，wlr_backend_autocreate 会选 wayland 后端并
 * 在 teardown 时走进 wlroots 0.20.2 的 wlr_touch_finish 崩溃路径（CI 红）。
 */
struct client_state {
    bool saw_syncobj;
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, "wp_linux_drm_syncobj_manager_v1") == 0) {
        state->saw_syncobj = true;
    }
    (void)registry;
    (void)name;
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

static void sync_callback(void *data, struct wl_callback *cb, uint32_t serial)
{
    bool *done = data;
    *done = true;
    wl_callback_destroy(cb);
    (void)serial;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_callback,
};

static void pump_roundtrip(struct woodland_server *server, struct wl_display *client_disp)
{
    bool done = false;
    struct wl_callback *cb = wl_display_sync(client_disp);
    assert(cb != NULL);
    wl_callback_add_listener(cb, &sync_listener, &done);
    while (!done) {
        assert(wl_display_get_error(client_disp) == 0);
        wl_display_flush(client_disp);
        wl_event_loop_dispatch(server->event_loop, 0);
        wl_display_flush_clients(server->display);
        wl_display_dispatch(client_disp);
    }
}

int main(void)
{
    /* pin 后端与渲染器：headless + pixman 下 timeline 特性确定不成立，
     * 既避免外部 WAYLAND_DISPLAY 干扰，也让下面的断言确定可复现 */
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    server.event_loop = wl_display_get_event_loop(server.display);
    assert(server.event_loop != NULL);

    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend != NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer != NULL);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_protocol_init(&server));

    /* pixman + headless：renderer/backend 不满足 timeline 门控，全局不暴露 */
    assert(server.renderer->features.timeline == false);
    assert(wlr_renderer_get_drm_fd(server.renderer) < 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *client = wl_client_create(server.display, fds[0]);
    assert(client != NULL);
    struct wl_display *client_display = wl_display_connect_to_fd(fds[1]);
    assert(client_display != NULL);

    struct client_state state = {0};
    struct wl_registry *reg = wl_display_get_registry(client_display);
    wl_registry_add_listener(reg, &registry_listener, &state);
    pump_roundtrip(&server, client_display);

    /* 门控条件不满足时，客户端在 registry 里必须看不到 syncobj 全局 */
    assert(state.saw_syncobj == false);

    wl_registry_destroy(reg);
    pump_roundtrip(&server, client_display);
    wl_display_disconnect(client_display);
    wl_client_destroy(client);

    /* create 对非法 fd 的安全防护：返回 NULL（wlroots 内部 check_syncobj_eventfd 兜底） */
    struct wlr_linux_drm_syncobj_manager_v1 *dummy =
        wlr_linux_drm_syncobj_manager_v1_create(server.display, 1, -1);
    assert(dummy == NULL);

    /* 逆序清理，二次 finish 保证幂等 */
    woodland_protocol_finish(&server);
    assert(server.proto == NULL);
    woodland_protocol_finish(&server);

    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_allocator_destroy(server.allocator);
    server.allocator = NULL;
    wlr_renderer_destroy(server.renderer);
    server.renderer = NULL;
    wlr_backend_destroy(server.backend);
    server.backend = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;

    return 0;
}
