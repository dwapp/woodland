// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_compositor.h>

#include "cursor.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "util/json.h"
#include "woodland.h"
#include "workspace.h"

#ifdef TEST_HAS_SECURITY_CONTEXT_CLIENT
#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

struct client_state {
    bool saw_sec_ctx_manager;
    bool saw_layer_shell;
    bool saw_screencopy;
    bool saw_compositor;
    bool saw_shm;
    struct wp_security_context_manager_v1 *sec_ctx_manager;
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, "wp_security_context_manager_v1") == 0) {
        state->saw_sec_ctx_manager = true;
        if (!state->sec_ctx_manager) {
            state->sec_ctx_manager = wl_registry_bind(
                registry, name, &wp_security_context_manager_v1_interface, 1);
        }
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        state->saw_layer_shell = true;
    } else if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0) {
        state->saw_screencopy = true;
    } else if (strcmp(interface, "wl_compositor") == 0) {
        state->saw_compositor = true;
    } else if (strcmp(interface, "wl_shm") == 0) {
        state->saw_shm = true;
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
#endif

static void test_pure_functions(void)
{
    /* 1. 白名单放行的基础与增强协议 */
    assert(woodland_security_context_is_allowed("wl_compositor"));
    assert(woodland_security_context_is_allowed("wl_subcompositor"));
    assert(woodland_security_context_is_allowed("wl_shm"));
    assert(woodland_security_context_is_allowed("wl_seat"));
    assert(woodland_security_context_is_allowed("xdg_wm_base"));
    assert(woodland_security_context_is_allowed("wl_fixes"));
    assert(woodland_security_context_is_allowed("wl_output"));
    assert(woodland_security_context_is_allowed("wl_drm"));
    assert(woodland_security_context_is_allowed("zwp_linux_dmabuf_v1"));
    assert(woodland_security_context_is_allowed("wp_presentation"));
    assert(woodland_security_context_is_allowed("wp_viewporter"));
    assert(woodland_security_context_is_allowed("wp_single_pixel_buffer_manager_v1"));
    assert(woodland_security_context_is_allowed("wp_fractional_scale_manager_v1"));
    assert(woodland_security_context_is_allowed("wp_tearing_control_manager_v1"));
    assert(woodland_security_context_is_allowed("wp_alpha_modifier_v1"));
    assert(woodland_security_context_is_allowed("wp_color_manager_v1"));
    assert(woodland_security_context_is_allowed("wp_color_representation_manager_v1"));
    assert(woodland_security_context_is_allowed("wp_linux_drm_syncobj_manager_v1"));
    assert(woodland_security_context_is_allowed("zxdg_output_manager_v1"));
    assert(woodland_security_context_is_allowed("zwp_primary_selection_device_manager_v1"));
    assert(woodland_security_context_is_allowed("zwp_text_input_manager_v3"));
    assert(woodland_security_context_is_allowed("zwp_pointer_gestures_v1"));
    assert(woodland_security_context_is_allowed("wp_cursor_shape_manager_v1"));
    assert(woodland_security_context_is_allowed("zwp_relative_pointer_manager_v1"));
    assert(woodland_security_context_is_allowed("zwp_tablet_manager_v2"));
    assert(woodland_security_context_is_allowed("zwp_pointer_constraints_v1"));
    assert(woodland_security_context_is_allowed("xdg_activation_v1"));
    assert(woodland_security_context_is_allowed("org_kde_kwin_server_decoration_manager"));
    assert(woodland_security_context_is_allowed("zxdg_decoration_manager_v1"));
    assert(woodland_security_context_is_allowed("treeland_decoration_manager_v1"));
    assert(woodland_security_context_is_allowed("ext_background_effect_manager_v1"));
    assert(woodland_security_context_is_allowed("treeland_appearance_v1"));
    assert(woodland_security_context_is_allowed("zxdg_importer_v1"));
    assert(woodland_security_context_is_allowed("zxdg_importer_v2"));
    assert(woodland_security_context_is_allowed("xdg_toplevel_icon_manager_v1"));
    assert(woodland_security_context_is_allowed("xdg_wm_dialog_v1"));
    assert(woodland_security_context_is_allowed("zxdg_exporter_v1"));
    assert(woodland_security_context_is_allowed("zxdg_exporter_v2"));
    assert(woodland_security_context_is_allowed("zwp_idle_inhibit_manager_v1"));
    assert(woodland_security_context_is_allowed("xdg_toplevel_tag_manager_v1"));
    assert(woodland_security_context_is_allowed("zxdg_toplevel_tag_manager_v1"));

    /* 2. 严格禁止沙箱客户端访问 security_context 自身（防嵌套沙箱） */
    assert(!woodland_security_context_is_allowed("wp_security_context_manager_v1"));
    assert(!woodland_security_context_is_allowed("security_context_manager_v1"));
    assert(woodland_security_context_is_privileged("wp_security_context_manager_v1"));

    /* 3. 特权协议全部禁止 */
    assert(!woodland_security_context_is_allowed("zwlr_screencopy_manager_v1"));
    assert(woodland_security_context_is_privileged("zwlr_screencopy_manager_v1"));

    assert(!woodland_security_context_is_allowed("ext_image_copy_capture_manager_v1"));
    assert(woodland_security_context_is_privileged("ext_image_copy_capture_manager_v1"));

    assert(!woodland_security_context_is_allowed("zwlr_data_control_manager_v1"));
    assert(woodland_security_context_is_privileged("zwlr_data_control_manager_v1"));

    assert(!woodland_security_context_is_allowed("ext_data_control_manager_v1"));
    assert(woodland_security_context_is_privileged("ext_data_control_manager_v1"));

    assert(!woodland_security_context_is_allowed("ext_session_lock_manager_v1"));
    assert(woodland_security_context_is_privileged("ext_session_lock_manager_v1"));

    assert(!woodland_security_context_is_allowed("zwlr_layer_shell_v1"));
    assert(woodland_security_context_is_privileged("zwlr_layer_shell_v1"));

    assert(!woodland_security_context_is_allowed("zwlr_output_manager_v1"));
    assert(woodland_security_context_is_privileged("zwlr_output_manager_v1"));
    assert(!woodland_security_context_is_allowed("zwlr_output_management_v1"));
    assert(woodland_security_context_is_privileged("zwlr_output_management_v1"));

    assert(!woodland_security_context_is_allowed("zwlr_output_power_manager_v1"));
    assert(woodland_security_context_is_privileged("zwlr_output_power_manager_v1"));

    assert(!woodland_security_context_is_allowed("zwlr_gamma_control_manager_v1"));
    assert(woodland_security_context_is_privileged("zwlr_gamma_control_manager_v1"));

    assert(!woodland_security_context_is_allowed("wp_drm_lease_device_v1"));
    assert(woodland_security_context_is_privileged("wp_drm_lease_device_v1"));

    assert(!woodland_security_context_is_allowed("treeland_wallpaper_manager_v1"));
    assert(woodland_security_context_is_privileged("treeland_wallpaper_manager_v1"));

    assert(!woodland_security_context_is_allowed("treeland_compositor_action_v1"));
    assert(woodland_security_context_is_privileged("treeland_compositor_action_v1"));

    assert(!woodland_security_context_is_allowed("treeland_shortcut_manager_v3"));
    assert(woodland_security_context_is_privileged("treeland_shortcut_manager_v3"));

    assert(!woodland_security_context_is_allowed("treeland_foreign_toplevel_manager_v3"));
    assert(woodland_security_context_is_privileged("treeland_foreign_toplevel_manager_v3"));

    assert(!woodland_security_context_is_allowed("treeland_appearance_manager_v1"));
    assert(woodland_security_context_is_privileged("treeland_appearance_manager_v1"));

    assert(!woodland_security_context_is_allowed("ext_foreign_toplevel_list_v1"));
    assert(woodland_security_context_is_privileged("ext_foreign_toplevel_list_v1"));

    assert(!woodland_security_context_is_allowed("ext_workspace_manager_v1"));
    assert(woodland_security_context_is_privileged("ext_workspace_manager_v1"));
    assert(!woodland_security_context_is_allowed("zwp_keyboard_shortcuts_inhibit_manager_v1"));
    assert(woodland_security_context_is_privileged("zwp_keyboard_shortcuts_inhibit_manager_v1"));
    assert(!woodland_security_context_is_allowed("ext_idle_notifier_v1"));
    assert(woodland_security_context_is_privileged("ext_idle_notifier_v1"));

    /* 4. 未知协议默认拒绝 */
    assert(!woodland_security_context_is_allowed("org_unknown_protocol_v1"));
    assert(!woodland_security_context_is_privileged("org_unknown_protocol_v1"));

    /* 5. 空指针保护 */
    assert(!woodland_security_context_is_allowed(NULL));
    assert(!woodland_security_context_is_privileged(NULL));
}

static void test_status_null(void)
{
    assert(woodland_protocol_client_security_context(NULL, NULL) == NULL);
    assert(woodland_protocol_view_security_context(NULL, NULL) == NULL);

    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_security_context_status(NULL, &json);
    const char *data = woodland_json_data(&json);
    assert(strstr(data, "\"available\":false") != NULL);
    woodland_json_finish(&json);
}

int main(void)
{
    test_pure_functions();
    test_status_null();

#ifdef TEST_HAS_SECURITY_CONTEXT_CLIENT
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
    assert(woodland_workspace_init(&server));
    assert(woodland_protocol_init(&server));

    wlr_renderer_init_wl_display(server.renderer, server.display);
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);
    /* 初始状态检查 */
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_security_context_status(&server, &json);
    const char *status_str = woodland_json_data(&json);
    assert(strstr(status_str, "\"available\":true") != NULL);
    assert(strstr(status_str, "\"commit_count\":0") != NULL);
    woodland_json_finish(&json);

    /* 1. 普通非沙箱客户端连接 */
    int parent_fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, parent_fds) == 0);
    struct wl_client *parent_server_client = wl_client_create(server.display, parent_fds[0]);
    assert(parent_server_client != NULL);
    struct wl_display *parent_client_disp = wl_display_connect_to_fd(parent_fds[1]);
    assert(parent_client_disp != NULL);

    /* 普通客户端没有 security context */
    assert(woodland_protocol_client_security_context(&server, parent_server_client) == NULL);

    struct client_state parent_state = {0};
    struct wl_registry *parent_reg = wl_display_get_registry(parent_client_disp);
    wl_registry_add_listener(parent_reg, &registry_listener, &parent_state);
    pump_roundtrip(&server, parent_client_disp);

    /* 普通客户端能够看见 wp_security_context_manager_v1 */
    assert(parent_state.saw_sec_ctx_manager == true);
    assert(parent_state.sec_ctx_manager != NULL);

    /* 2. 创建监听 socket，通过 wp_security_context_manager_v1 注册沙箱上下文 */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listen_fd >= 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "woodland-test-sec-ctx-%d", getpid());
    socklen_t addr_len = sizeof(sa_family_t) + 1 + strlen(addr.sun_path + 1);
    assert(bind(listen_fd, (struct sockaddr *)&addr, addr_len) == 0);
    assert(listen(listen_fd, 1) == 0);

    int close_fds[2];
    assert(pipe2(close_fds, O_CLOEXEC) == 0);

    struct wp_security_context_v1 *sec_ctx =
        wp_security_context_manager_v1_create_listener(parent_state.sec_ctx_manager,
                                                       listen_fd, close_fds[0]);
    assert(sec_ctx != NULL);
    wp_security_context_v1_set_sandbox_engine(sec_ctx, "org.flatpak");
    wp_security_context_v1_set_app_id(sec_ctx, "org.example.App");
    wp_security_context_v1_set_instance_id(sec_ctx, "inst-42");
    wp_security_context_v1_commit(sec_ctx);
    wp_security_context_v1_destroy(sec_ctx);
    pump_roundtrip(&server, parent_client_disp);

    /* 3. 沙箱客户端连接该监听 socket 接入 */
    int client_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(client_fd >= 0);
    assert(connect(client_fd, (struct sockaddr *)&addr, addr_len) == 0);
    /* 让服务端在事件循环中完成 accept 并建好 wl_client */
    wl_event_loop_dispatch(server.event_loop, 0);

    struct wl_display *sandboxed_client_disp = wl_display_connect_to_fd(client_fd);
    assert(sandboxed_client_disp != NULL);
    struct client_state sandboxed_state = {0};
    struct wl_registry *sandboxed_reg = wl_display_get_registry(sandboxed_client_disp);
    wl_registry_add_listener(sandboxed_reg, &registry_listener, &sandboxed_state);
    pump_roundtrip(&server, sandboxed_client_disp);

    /* 4. 断言沙箱客户端过滤行为：
     *   - wp_security_context_manager_v1 必须被隐藏（防嵌套沙箱）
     *   - 特权接口（layer-shell / screencopy 等）必须被过滤
     *   - 基础接口（wl_compositor / wl_shm 等）正常放行
     */
    assert(sandboxed_state.saw_sec_ctx_manager == false);
    assert(sandboxed_state.saw_layer_shell == false);
    assert(sandboxed_state.saw_screencopy == false);
    assert(sandboxed_state.saw_compositor == true);
    assert(sandboxed_state.saw_shm == true);

    /* 5. 校验服务端状态与客户端状态查询 */
    woodland_json_init(&json);
    woodland_protocol_security_context_status(&server, &json);
    status_str = woodland_json_data(&json);
    assert(strstr(status_str, "\"available\":true") != NULL);
    assert(strstr(status_str, "\"commit_count\":1") != NULL);
    assert(strstr(status_str, "\"last_engine\":\"org.flatpak\"") != NULL);
    assert(strstr(status_str, "\"last_app_id\":\"org.example.App\"") != NULL);
    assert(strstr(status_str, "\"last_instance_id\":\"inst-42\"") != NULL);
    woodland_json_finish(&json);

    /* 遍历服务端 client 列表，找到沙箱客户端并验证上下文数据 */
    struct wl_list *clients = wl_display_get_client_list(server.display);
    bool found_sandboxed_client = false;
    struct wl_client *c;
    wl_client_for_each(c, clients) {
        const struct wlr_security_context_v1_state *st =
            woodland_protocol_client_security_context(&server, c);
        if (st) {
            found_sandboxed_client = true;
            assert(strcmp(st->sandbox_engine, "org.flatpak") == 0);
            assert(strcmp(st->app_id, "org.example.App") == 0);
            assert(strcmp(st->instance_id, "inst-42") == 0);
        }
    }
    assert(found_sandboxed_client == true);

    /* 6. 收尾清理 */
    wl_registry_destroy(sandboxed_reg);
    wl_display_disconnect(sandboxed_client_disp);

    close(close_fds[1]); // 触发 close_fd 挂断

    wp_security_context_manager_v1_destroy(parent_state.sec_ctx_manager);
    wl_registry_destroy(parent_reg);
    pump_roundtrip(&server, parent_client_disp);
    wl_display_disconnect(parent_client_disp);

    /* 逆序清理 */
    woodland_protocol_finish(&server);
    assert(server.proto == NULL);
    woodland_workspace_finish(&server);
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
#endif

    return 0;
}
