// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>

#include "capture.h"
#include "config.h"
#include "cursor.h"
#include "ime.h"
#include "layer.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "session_lock.h"
#include "view.h"
#include "util/global_filter.h"
#include "util/json.h"
#include "woodland.h"
#include "workspace.h"
#if WOODLAND_HAVE_XWAYLAND
#include "xwayland.h"
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>
#endif
#include "../../src/protocol/protocol_internal.h"

#ifdef TEST_HAS_SECURITY_CONTEXT_CLIENT
#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

#define MAX_SEEN_GLOBALS 128

struct client_state {
    char seen[MAX_SEEN_GLOBALS][64];
    size_t seen_count;
    struct wp_security_context_manager_v1 *sec_ctx_manager;
};

static bool state_saw(const struct client_state *state, const char *iface)
{
    for (size_t i = 0; i < state->seen_count; i++) {
        if (strcmp(state->seen[i], iface) == 0) {
            return true;
        }
    }
    return false;
}

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct client_state *state = data;
    if (state->seen_count < MAX_SEEN_GLOBALS) {
        snprintf(state->seen[state->seen_count], sizeof(state->seen[0]), "%s", interface);
        state->seen_count++;
    }
    if (strcmp(interface, "wp_security_context_manager_v1") == 0) {
        if (!state->sec_ctx_manager) {
            state->sec_ctx_manager = wl_registry_bind(
                registry, name, &wp_security_context_manager_v1_interface, 1);
        }
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

static void pump_roundtrip(struct wl_event_loop *loop, struct wl_display *server_disp,
                           struct wl_display *client_disp)
{
    bool done = false;
    struct wl_callback *cb = wl_display_sync(client_disp);
    assert(cb != NULL);
    wl_callback_add_listener(cb, &sync_listener, &done);
    while (!done) {
        assert(wl_display_get_error(client_disp) == 0);
        wl_display_flush(client_disp);
        wl_event_loop_dispatch(loop, 0);
        wl_display_flush_clients(server_disp);
        wl_display_dispatch(client_disp);
    }
}

static bool block_interface_filter(const struct wl_client *client,
                                   const struct wl_global *global,
                                   void *data)
{
    (void)client;
    const char *blocked = data;
    const struct wl_interface *iface = wl_global_get_interface(global);
    if (!iface || !iface->name) {
        return false;
    }
    return strcmp(iface->name, blocked) != 0;
}

static bool allow_all_filter(const struct wl_client *client,
                             const struct wl_global *global,
                             void *data)
{
    (void)client;
    (void)global;
    (void)data;
    return true;
}

static void test_global_filter_dispatcher(void)
{
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    struct wl_display *disp = wl_display_create();
    assert(disp != NULL);
    struct wl_event_loop *loop = wl_display_get_event_loop(disp);
    assert(loop != NULL);

    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    assert(backend != NULL);
    struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
    assert(renderer != NULL);
    assert(wlr_renderer_init_wl_display(renderer, disp));
    struct wlr_compositor *compositor = wlr_compositor_create(disp, 5, renderer);
    assert(compositor != NULL);
    assert(wlr_subcompositor_create(disp) != NULL);

    /* 参数校验 */
    assert(!woodland_global_filter_add(NULL, "a", allow_all_filter, NULL));
    assert(!woodland_global_filter_add(disp, NULL, allow_all_filter, NULL));
    assert(!woodland_global_filter_add(disp, "", allow_all_filter, NULL));
    assert(!woodland_global_filter_add(disp, "a", NULL, NULL));
    assert(woodland_global_filter_count(disp) == 0);

    /* 注册两个独立 checker：filter_a 挡 wl_compositor，filter_b 挡 wl_shm */
    assert(woodland_global_filter_add(disp, "filter_a", block_interface_filter, "wl_compositor"));
    assert(woodland_global_filter_add(disp, "filter_b", block_interface_filter, "wl_shm"));
    assert(woodland_global_filter_count(disp) == 2);
    assert(woodland_global_filter_has(disp, "filter_a"));
    assert(woodland_global_filter_has(disp, "filter_b"));

    /* 同名重复注册必须拒绝且不覆盖已有条目 */
    assert(!woodland_global_filter_add(disp, "filter_a", allow_all_filter, NULL));
    assert(woodland_global_filter_count(disp) == 2);

    /* 客户端 1：两者皆被挡，wl_subcompositor 正常放行 */
    int fds1[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds1) == 0);
    assert(wl_client_create(disp, fds1[0]) != NULL);
    struct wl_display *c1 = wl_display_connect_to_fd(fds1[1]);
    assert(c1 != NULL);
    struct client_state s1 = {0};
    struct wl_registry *r1 = wl_display_get_registry(c1);
    wl_registry_add_listener(r1, &registry_listener, &s1);
    pump_roundtrip(loop, disp, c1);
    assert(!state_saw(&s1, "wl_compositor"));
    assert(!state_saw(&s1, "wl_shm"));
    assert(state_saw(&s1, "wl_subcompositor"));
    wl_registry_destroy(r1);
    wl_display_disconnect(c1);

    /* 撤掉 filter_a：wl_compositor 恢复，wl_shm 仍被 filter_b 拦截 */
    woodland_global_filter_remove(disp, "filter_a");
    assert(woodland_global_filter_count(disp) == 1);
    assert(!woodland_global_filter_has(disp, "filter_a"));
    assert(woodland_global_filter_has(disp, "filter_b"));

    int fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds2) == 0);
    assert(wl_client_create(disp, fds2[0]) != NULL);
    struct wl_display *c2 = wl_display_connect_to_fd(fds2[1]);
    assert(c2 != NULL);
    struct client_state s2 = {0};
    struct wl_registry *r2 = wl_display_get_registry(c2);
    wl_registry_add_listener(r2, &registry_listener, &s2);
    pump_roundtrip(loop, disp, c2);
    assert(state_saw(&s2, "wl_compositor"));
    assert(!state_saw(&s2, "wl_shm"));
    wl_registry_destroy(r2);
    wl_display_disconnect(c2);

    /* 撤掉 filter_b：全部放行且注册表归零 */
    woodland_global_filter_remove(disp, "filter_b");
    assert(woodland_global_filter_count(disp) == 0);

    int fds3[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds3) == 0);
    assert(wl_client_create(disp, fds3[0]) != NULL);
    struct wl_display *c3 = wl_display_connect_to_fd(fds3[1]);
    assert(c3 != NULL);
    struct client_state s3 = {0};
    struct wl_registry *r3 = wl_display_get_registry(c3);
    wl_registry_add_listener(r3, &registry_listener, &s3);
    pump_roundtrip(loop, disp, c3);
    assert(state_saw(&s3, "wl_compositor"));
    assert(state_saw(&s3, "wl_shm"));
    wl_registry_destroy(r3);
    wl_display_disconnect(c3);

    /* 测试异常未 remove 直接销毁 display：destroy listener 自动回收无泄漏 */
    assert(woodland_global_filter_add(disp, "leftover", allow_all_filter, NULL));
    assert(woodland_global_filter_count(disp) == 1);
    wl_display_destroy_clients(disp);

    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(disp);
}
#endif

static void test_pure_functions(void)
{
    /* 1. 白名单放行的协议（含用户指定默认放行的 zwp_keyboard_shortcuts_inhibit_manager_v1） */
    static const char *const expected_allowed[] = {
        "wl_shm",
        "wl_compositor",
        "wl_subcompositor",
        "wl_data_device_manager",
        "wl_seat",
        "xdg_wm_base",
        "wl_fixes",
        "wl_output",
        "zwp_linux_dmabuf_v1",
        "wp_presentation",
        "wp_viewporter",
        "wp_single_pixel_buffer_manager_v1",
        "wp_fractional_scale_manager_v1",
        "wp_tearing_control_manager_v1",
        "wp_alpha_modifier_v1",
        "wp_linux_drm_syncobj_manager_v1",
        "zxdg_output_manager_v1",
        "zwp_primary_selection_device_manager_v1",
        "zwp_text_input_manager_v3",
        "zwp_pointer_gestures_v1",
        "wp_cursor_shape_manager_v1",
        "zwp_relative_pointer_manager_v1",
        "zwp_pointer_constraints_v1",
        "zwp_keyboard_shortcuts_inhibit_manager_v1",
        "xdg_activation_v1",
        "zxdg_decoration_manager_v1",
        "xdg_wm_dialog_v1",
        "xdg_toplevel_tag_manager_v1",
        "zwp_idle_inhibit_manager_v1",
        "treeland_decoration_manager_v1",
        "ext_background_effect_manager_v1",
        "treeland_appearance_v1",
    };

    for (size_t i = 0; i < sizeof(expected_allowed) / sizeof(expected_allowed[0]); i++) {
        assert(woodland_security_context_is_allowed(expected_allowed[i]));
        assert(!woodland_security_context_is_privileged(expected_allowed[i]));
    }

    /* 2. 特权协议黑名单（含 xwayland_shell_v1，及默认屏蔽的列表/工作区等） */
    static const char *const expected_privileged[] = {
        "wp_security_context_manager_v1",
        "zwlr_screencopy_manager_v1",
        "ext_image_copy_capture_manager_v1",
        "ext_output_image_capture_source_manager_v1",
        "ext_foreign_toplevel_image_capture_source_manager_v1",
        "zwlr_data_control_manager_v1",
        "ext_data_control_manager_v1",
        "ext_session_lock_manager_v1",
        "zwlr_layer_shell_v1",
        "zwlr_output_manager_v1",
        "zwlr_output_power_manager_v1",
        "zwlr_gamma_control_manager_v1",
        "wp_drm_lease_device_v1",
        "zwp_input_method_manager_v2",
        "zwp_virtual_keyboard_manager_v1",
        "ext_idle_notifier_v1",
        "ext_workspace_manager_v1",
        "zwlr_foreign_toplevel_manager_v1",
        "ext_foreign_toplevel_list_v1",
        "xwayland_shell_v1",
        "treeland_wallpaper_manager_v1",
        "treeland_wallpaper_shell_v1",
        "treeland_wallpaper_notifier_v1",
        "treeland_compositor_action_v1",
        "treeland_shortcut_manager_v3",
        "treeland_foreign_toplevel_manager_v3",
        "treeland_appearance_manager_v1",
    };

    for (size_t i = 0; i < sizeof(expected_privileged) / sizeof(expected_privileged[0]); i++) {
        assert(!woodland_security_context_is_allowed(expected_privileged[i]));
        assert(woodland_security_context_is_privileged(expected_privileged[i]));
    }

    /* 3. 未知或 woodland 未暴露的协议一律默认拒绝（deny-by-default） */
    assert(!woodland_security_context_is_allowed("wp_color_manager_v1"));
    assert(!woodland_security_context_is_allowed("zxdg_importer_v1"));
    assert(!woodland_security_context_is_allowed("zwp_tablet_manager_v2"));
    assert(!woodland_security_context_is_allowed("org_unknown_protocol_v1"));
    assert(!woodland_security_context_is_privileged("org_unknown_protocol_v1"));

    /* 4. 空指针保护 */
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

#ifdef TEST_HAS_SECURITY_CONTEXT_CLIENT
static void test_abnormal_display_destroy(void)
{
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    struct woodland_proto proto = {.server = &server};
    server.proto = &proto;

    assert(woodland_proto_security_context_init(&proto));
    assert(proto.security_context != NULL);
    assert(woodland_global_filter_has(server.display, "security_context"));

    /* 故意跳过 woodland_proto_security_context_finish() 直接销毁 display：
     * 验证 handle_manager_destroy() 摘除 commit/destroy 监听器并避免 wlroots abort，
     * 且将 proto.security_context 置 NULL 保证后续幂等调用安全。 */
    wl_display_destroy(server.display);
    server.display = NULL;
    assert(proto.security_context == NULL);

    woodland_proto_security_context_finish(&proto);
}

static void test_server_integration(void)
{
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

    assert(wlr_renderer_init_wl_display(server.renderer, server.display));
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);
    assert(wlr_subcompositor_create(server.display) != NULL);
    assert(wlr_data_device_manager_create(server.display) != NULL);
    assert(wlr_data_control_manager_v1_create(server.display) != NULL);
    assert(wlr_ext_data_control_manager_v1_create(server.display, 1) != NULL);
    assert(wlr_single_pixel_buffer_manager_v1_create(server.display) != NULL);
    assert(wlr_fractional_scale_manager_v1_create(server.display, 1) != NULL);
    assert(wlr_alpha_modifier_v1_create(server.display) != NULL);
    assert(wlr_fixes_create(server.display, 1) != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_ime_init(&server));
    assert(woodland_session_lock_init(&server));
    assert(woodland_shell_init(&server));
    assert(woodland_workspace_init(&server));
    assert(woodland_layer_init(&server));
    assert(woodland_protocol_init(&server));
    assert(woodland_capture_init(&server));
    server.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.display, 1);
    assert(server.cursor_shape_manager != NULL);

    bool xwl_ok = false;
#if WOODLAND_HAVE_XWAYLAND
    xwl_ok = woodland_xwayland_init(&server);
#endif

    /* 验证 global_filter 多 checker 注册：security_context 与 xwayland_shell 共存且拒绝重复注册 */
    assert(woodland_global_filter_has(server.display, "security_context"));
    assert(!woodland_global_filter_add(server.display, "security_context",
                                       allow_all_filter, NULL));
    if (xwl_ok) {
        assert(woodland_global_filter_has(server.display, "xwayland_shell"));
        assert(woodland_global_filter_count(server.display) == 2);
    } else {
        assert(woodland_global_filter_count(server.display) == 1);
    }

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

    assert(woodland_protocol_client_security_context(&server, parent_server_client) == NULL);

    struct client_state parent_state = {0};
    struct wl_registry *parent_reg = wl_display_get_registry(parent_client_disp);
    wl_registry_add_listener(parent_reg, &registry_listener, &parent_state);
    pump_roundtrip(server.event_loop, server.display, parent_client_disp);

    /* 显式断言普通客户端能看到所有特权与非特权全局（xwayland_shell_v1 除外） */
    assert(parent_state.sec_ctx_manager != NULL);
    static const char *const must_see_normal[] = {
        "wp_security_context_manager_v1",
        "zwlr_layer_shell_v1",
        "zwlr_screencopy_manager_v1",
        "ext_image_copy_capture_manager_v1",
        "zwlr_data_control_manager_v1",
        "ext_data_control_manager_v1",
        "ext_session_lock_manager_v1",
        "zwp_input_method_manager_v2",
        "zwp_virtual_keyboard_manager_v1",
        "zwlr_output_manager_v1",
        "zwlr_output_power_manager_v1",
        "zwlr_gamma_control_manager_v1",
        "ext_idle_notifier_v1",
        "ext_workspace_manager_v1",
        "zwlr_foreign_toplevel_manager_v1",
        "ext_foreign_toplevel_list_v1",
        "treeland_foreign_toplevel_manager_v3",
        "treeland_compositor_action_v1",
        "treeland_shortcut_manager_v3",
        "treeland_appearance_manager_v1",
        "treeland_wallpaper_manager_v1",
        "treeland_wallpaper_shell_v1",
        "treeland_wallpaper_notifier_v1",
        "zwp_keyboard_shortcuts_inhibit_manager_v1",
        "wl_compositor",
        "wl_shm",
        "xdg_wm_base",
    };
    for (size_t i = 0; i < sizeof(must_see_normal) / sizeof(must_see_normal[0]); i++) {
        assert(state_saw(&parent_state, must_see_normal[i]));
    }
#if WOODLAND_HAVE_XWAYLAND
    if (xwl_ok) {
        assert(!state_saw(&parent_state, "xwayland_shell_v1"));

        /* Xwayland 自身客户端必须能看见 xwayland_shell_v1 */
        int xwl_fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, xwl_fds) == 0);
        struct wl_client *xwl_mock_client = wl_client_create(server.display, xwl_fds[0]);
        assert(xwl_mock_client != NULL);
        struct wl_display *xwl_disp = wl_display_connect_to_fd(xwl_fds[1]);
        assert(xwl_disp != NULL);

        struct wl_client *saved_xwl_client = server.xwayland->server->client;
        server.xwayland->server->client = xwl_mock_client;

        struct client_state xwl_state = {0};
        struct wl_registry *xwl_reg = wl_display_get_registry(xwl_disp);
        wl_registry_add_listener(xwl_reg, &registry_listener, &xwl_state);
        pump_roundtrip(server.event_loop, server.display, xwl_disp);
        assert(state_saw(&xwl_state, "xwayland_shell_v1"));
        assert(state_saw(&xwl_state, "wl_compositor"));

        server.xwayland->server->client = saved_xwl_client;
        if (xwl_state.sec_ctx_manager) {
            wp_security_context_manager_v1_destroy(xwl_state.sec_ctx_manager);
        }
        wl_registry_destroy(xwl_reg);
        wl_display_disconnect(xwl_disp);
        wl_client_destroy(xwl_mock_client);
    }
#else
    (void)xwl_ok;
#endif

    /* 不变量断言：合成器实际暴露的每一个 global 都必须落在 allowed 或 privileged 之一且互斥 */
    assert(parent_state.seen_count > 0);
    for (size_t i = 0; i < parent_state.seen_count; i++) {
        bool allowed = woodland_security_context_is_allowed(parent_state.seen[i]);
        bool privileged = woodland_security_context_is_privileged(parent_state.seen[i]);
        assert(allowed != privileged);
    }

    /* 2. 创建监听 socket，通过 wp_security_context_manager_v1 注册沙箱上下文 */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listen_fd >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "woodland-test-sec-ctx-%d", getpid());
    socklen_t addr_len = (socklen_t)(sizeof(sa_family_t) + 1 + strlen(addr.sun_path + 1));
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
    pump_roundtrip(server.event_loop, server.display, parent_client_disp);

    /* 3. 沙箱客户端连接该监听 socket 接入 */
    int client_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(client_fd >= 0);
    assert(connect(client_fd, (struct sockaddr *)&addr, addr_len) == 0);
    wl_event_loop_dispatch(server.event_loop, 0);

    struct wl_display *sandboxed_client_disp = wl_display_connect_to_fd(client_fd);
    assert(sandboxed_client_disp != NULL);

    struct client_state sandboxed_state = {0};
    struct wl_registry *sandboxed_reg = wl_display_get_registry(sandboxed_client_disp);
    wl_registry_add_listener(sandboxed_reg, &registry_listener, &sandboxed_state);
    pump_roundtrip(server.event_loop, server.display, sandboxed_client_disp);

    /* 4. 断言沙箱客户端过滤行为：
     *   - 沙箱客户端见到的每一个 global 都必须在允许白名单内且非特权；
     *   - 基础协议与 zwp_keyboard_shortcuts_inhibit_manager_v1 正常放行；
     *   - wp_security_context_manager_v1 与各类特权协议全部被屏蔽。
     */
    assert(sandboxed_state.seen_count > 0);
    for (size_t i = 0; i < sandboxed_state.seen_count; i++) {
        assert(woodland_security_context_is_allowed(sandboxed_state.seen[i]));
        assert(!woodland_security_context_is_privileged(sandboxed_state.seen[i]));
    }
    assert(state_saw(&sandboxed_state, "wl_compositor"));
    assert(state_saw(&sandboxed_state, "wl_shm"));
    assert(state_saw(&sandboxed_state, "xdg_wm_base"));
    assert(state_saw(&sandboxed_state, "zwp_keyboard_shortcuts_inhibit_manager_v1"));
    assert(!state_saw(&sandboxed_state, "wp_security_context_manager_v1"));
    assert(!state_saw(&sandboxed_state, "zwlr_layer_shell_v1"));
    assert(!state_saw(&sandboxed_state, "zwlr_screencopy_manager_v1"));
    assert(!state_saw(&sandboxed_state, "ext_foreign_toplevel_list_v1"));
    assert(!state_saw(&sandboxed_state, "ext_workspace_manager_v1"));
    assert(!state_saw(&sandboxed_state, "xwayland_shell_v1"));

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
    close(close_fds[1]);

    wp_security_context_manager_v1_destroy(parent_state.sec_ctx_manager);
    wl_registry_destroy(parent_reg);
    pump_roundtrip(server.event_loop, server.display, parent_client_disp);
    wl_display_disconnect(parent_client_disp);
    wl_display_destroy_clients(server.display);

#if WOODLAND_HAVE_XWAYLAND
    woodland_xwayland_finish(&server);
#endif
    woodland_protocol_finish(&server);
    assert(server.proto == NULL);
    assert(!woodland_global_filter_has(server.display, "security_context"));
    assert(woodland_global_filter_count(server.display) == 0);

    woodland_capture_finish(&server);
    woodland_shell_finish(&server);
    woodland_layer_finish(&server);
    woodland_workspace_finish(&server);
    woodland_session_lock_finish(&server);
    woodland_ime_finish(&server);
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
}
#endif

int main(void)
{
    test_pure_functions();
    test_status_null();

#ifdef TEST_HAS_SECURITY_CONTEXT_CLIENT
    test_global_filter_dispatcher();
    test_abnormal_display_destroy();
    test_server_integration();
#endif

    return 0;
}
