// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>

#include "config.h"
#if WOODLAND_HAVE_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include "cursor.h"
#include "output.h"
#include "placement.h"
#include "protocol.h"
#include "seat.h"
#include "session_lock.h"
#include "tearing-control-v1-client-protocol.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

#include "../../src/protocol/protocol_internal.h"

struct client_state {
    struct wp_tearing_control_manager_v1 *tearing_manager;
    struct wl_compositor *compositor;
    bool saw_tearing_manager;
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, "wp_tearing_control_manager_v1") == 0) {
        state->saw_tearing_manager = true;
        state->tearing_manager = wl_registry_bind(
            registry, name, &wp_tearing_control_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_compositor") == 0) {
        state->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    }
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
    setenv("WLR_BACKENDS", "headless", 1);
#if !WOODLAND_HAVE_EFFECTS
    setenv("WLR_RENDERER", "pixman", 1);
#endif

    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    server.event_loop = wl_display_get_event_loop(server.display);
    assert(server.event_loop != NULL);

    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend != NULL);

#if WOODLAND_HAVE_EFFECTS
    server.renderer = fx_renderer_create(server.backend);
    if (server.renderer == NULL) {
        setenv("WLR_RENDERER_FORCE_SOFTWARE", "1", 1);
        server.renderer = fx_renderer_create(server.backend);
    }
    if (server.renderer == NULL) {
        fprintf(stderr, "no GLES2 device for fx_renderer, skip\n");
        return 77;
    }
#else
    server.renderer = wlr_renderer_autocreate(server.backend);
#endif
    assert(server.renderer != NULL);
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);
    wlr_subcompositor_create(server.display);
    wlr_renderer_init_wl_shm(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    server.config = woodland_config_create_default();
    assert(server.config != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_workspace_init(&server));
    assert(woodland_session_lock_init(&server));
    assert(woodland_shell_init(&server));
    assert(woodland_protocol_init(&server));

    struct wlr_output *wlr_out = wlr_headless_add_output(server.backend, 800, 600);
    assert(wlr_out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, wlr_out);

    struct woodland_output *output = NULL;
    struct woodland_output *o;
    wl_list_for_each(o, &server.outputs, link) {
        if (o->wlr_output == wlr_out) {
            output = o;
            break;
        }
    }
    assert(output != NULL);

    /* ==================== Client 1: 正常流程 ==================== */
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

    assert(state.saw_tearing_manager);
    assert(state.tearing_manager != NULL);
    assert(state.compositor != NULL);

    /* 1. 创建 client surface 并申请 tearing control */
    struct wl_surface *client_surface = wl_compositor_create_surface(state.compositor);
    assert(client_surface != NULL);
    struct wp_tearing_control_v1 *tearing_control =
        wp_tearing_control_manager_v1_get_tearing_control(state.tearing_manager,
                                                         client_surface);
    assert(tearing_control != NULL);
    pump_roundtrip(&server, client_display);

    /* 2. 初始提示应为 VSYNC */
    assert(server.proto != NULL && server.proto->tearing != NULL);
    struct woodland_tearing_controller *controller =
        wl_container_of(server.proto->tearing->controllers.next, controller, link);
    assert(controller != NULL);
    assert(controller->tearing_control != NULL);
    assert(controller->tearing_control->current ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);

    /* 3. 设置 ASYNC 提示并在 commit 后生效 */
    wp_tearing_control_v1_set_presentation_hint(
        tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    wl_surface_commit(client_surface);
    pump_roundtrip(&server, client_display);

    assert(controller->tearing_control->current ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    assert(wlr_tearing_control_manager_v1_surface_hint_from_surface(
               server.proto->tearing->manager, controller->tearing_control->surface) ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);

    /* 4. 将该 surface 关联到模拟视图并验证 view->tearing_hint 与 output_get_tearing_allowance */
    struct woodland_view view = {0};
    view.server = &server;
    view.mapped = true;
    view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    assert(view.scene_tree != NULL);
    view.scene_tree->node.x = 100;
    view.scene_tree->node.y = 100;
    view.tearing_hint = WOODLAND_TEARING_ASYNC;
    wl_list_insert(&server.views, &view.link);

    /* 无焦点时不允许撕裂 */
    assert(!woodland_output_get_tearing_allowance(output));

    /* 默认配置 allow_tearing = false，即使 hint 为 ASYNC 也不允许撕裂 */
    server.focused_view = &view;
    assert(!woodland_output_get_tearing_allowance(output));

    /* 开启 allow_tearing 后允许撕裂 */
    server.config->general.allow_tearing = true;
    assert(woodland_output_get_tearing_allowance(output));

    /* 配置关掉 allow_tearing 时不允许撕裂 */
    server.config->general.allow_tearing = false;
    assert(!woodland_output_get_tearing_allowance(output));
    server.config->general.allow_tearing = true;
    assert(woodland_output_get_tearing_allowance(output));
    /* 会话锁上锁时不允许撕裂 */
    server.session_lock->locked = true;
    assert(!woodland_output_get_tearing_allowance(output));
    server.session_lock->locked = false;
    assert(woodland_output_get_tearing_allowance(output));

    /* 5. 将提示切回 VSYNC 并 commit */
    wp_tearing_control_v1_set_presentation_hint(
        tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);
    wl_surface_commit(client_surface);
    pump_roundtrip(&server, client_display);

    assert(controller->tearing_control->current ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);
    view.tearing_hint = WOODLAND_TEARING_VSYNC;
    assert(!woodland_output_get_tearing_allowance(output));

    /* 清理 client 1 代理对象与 view */
    wl_list_remove(&view.link);
    wlr_scene_node_destroy(&view.scene_tree->node);
    server.focused_view = NULL;

    wp_tearing_control_v1_destroy(tearing_control);
    wl_surface_destroy(client_surface);
    wp_tearing_control_manager_v1_destroy(state.tearing_manager);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(reg);
    pump_roundtrip(&server, client_display);

    wl_display_disconnect(client_display);
    wl_client_destroy(client);

    /* ==================== Client 2: 重复创建抛出协议错误 ==================== */
    int fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds2) == 0);
    struct wl_client *client2 = wl_client_create(server.display, fds2[0]);
    assert(client2 != NULL);
    struct wl_display *client2_display = wl_display_connect_to_fd(fds2[1]);
    assert(client2_display != NULL);

    struct client_state state2 = {0};
    struct wl_registry *reg2 = wl_display_get_registry(client2_display);
    wl_registry_add_listener(reg2, &registry_listener, &state2);
    pump_roundtrip(&server, client2_display);

    assert(state2.tearing_manager != NULL);
    assert(state2.compositor != NULL);

    struct wl_surface *client2_surface = wl_compositor_create_surface(state2.compositor);
    assert(client2_surface != NULL);
    struct wp_tearing_control_v1 *tc1 =
        wp_tearing_control_manager_v1_get_tearing_control(state2.tearing_manager, client2_surface);
    assert(tc1 != NULL);
    pump_roundtrip(&server, client2_display);

    /* 重复对同一个 surface 调用 get_tearing_control 应抛出协议错误 */
    struct wp_tearing_control_v1 *tc2 =
        wp_tearing_control_manager_v1_get_tearing_control(state2.tearing_manager, client2_surface);
    (void)tc2;
    wl_display_flush(client2_display);
    wl_event_loop_dispatch(server.event_loop, 0);
    wl_display_flush_clients(server.display);
    wl_display_dispatch(client2_display);

    uint32_t object_id = 0;
    const struct wl_interface *err_iface = NULL;
    int disp_err = wl_display_get_error(client2_display);
    assert(disp_err != 0);
    uint32_t proto_err =
        wl_display_get_protocol_error(client2_display, &err_iface, &object_id);
    assert(proto_err == WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS);
    assert(err_iface == &wp_tearing_control_manager_v1_interface);
    wp_tearing_control_v1_destroy(tc2);
    wp_tearing_control_v1_destroy(tc1);
    wl_surface_destroy(client2_surface);
    wp_tearing_control_manager_v1_destroy(state2.tearing_manager);
    wl_compositor_destroy(state2.compositor);
    wl_registry_destroy(reg2);

    wl_display_disconnect(client2_display);
    wl_event_loop_dispatch(server.event_loop, 0);

    /* ==================== 逆序清理服务器资源 ==================== */
    woodland_protocol_finish(&server);
    assert(server.proto == NULL);
    /* 幂等 finish */
    woodland_protocol_finish(&server);

    woodland_shell_finish(&server);
    woodland_session_lock_finish(&server);
    woodland_workspace_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

    wlr_output_destroy(wlr_out);
    woodland_config_destroy(server.config);

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
