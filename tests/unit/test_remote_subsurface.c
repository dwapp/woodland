// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_subcompositor.h>

#include "config.h"
#if WOODLAND_HAVE_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include "scene.h"

#include "cursor.h"
#include "ime.h"
#include "layer.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "session_lock.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

#include "test-remote-subsurface-client-protocol.h"
#include "test-remote-subsurface-xdg-shell-client-protocol.h"

struct client_state {
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct treeland_remote_subsurface_manager_v1 *remote_mgr;
};

struct export_record {
    char token[129];
    uint32_t rejected_count;
    uint32_t rejected_reason;
    char rejected_token[129];
};

struct remote_record {
    uint32_t invalid_sibling_count;
    char sibling_token[129];
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)version;
    struct client_state *st = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        uint32_t bind_ver = version < 5 ? version : 5;
        st->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, bind_ver);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        st->subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        st->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        uint32_t bind_ver = version < 3 ? version : 3;
        st->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, bind_ver);
    } else if (strcmp(interface, "treeland_remote_subsurface_manager_v1") == 0) {
        st->remote_mgr =
            wl_registry_bind(reg, name, &treeland_remote_subsurface_manager_v1_interface, 1);
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
}

static const struct xdg_surface_listener surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void on_surface_token(void *data, struct treeland_exported_surface_v1 *exported,
                             const char *token)
{
    (void)exported;
    struct export_record *rec = data;
    snprintf(rec->token, sizeof(rec->token), "%s", token);
}

static void on_parent_rejected(void *data, struct treeland_exported_surface_v1 *exported,
                               uint32_t reason, const char *parent_token)
{
    (void)exported;
    struct export_record *rec = data;
    rec->rejected_count++;
    rec->rejected_reason = reason;
    snprintf(rec->rejected_token, sizeof(rec->rejected_token), "%s", parent_token);
}

static const struct treeland_exported_surface_v1_listener exported_listener = {
    .surface_token = on_surface_token,
    .parent_rejected = on_parent_rejected,
};

static void on_invalid_sibling(void *data, struct treeland_remote_subsurface_v1 *remote,
                               const char *sibling_token)
{
    (void)remote;
    struct remote_record *rec = data;
    rec->invalid_sibling_count++;
    snprintf(rec->sibling_token, sizeof(rec->sibling_token), "%s", sibling_token);
}

static const struct treeland_remote_subsurface_v1_listener remote_listener = {
    .invalid_sibling = on_invalid_sibling,
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

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("test-remote-shm", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

static void test_null_defaults(void)
{
    assert(woodland_remote_subsurface_parent(NULL) == NULL);
    assert(woodland_surface_get_root_surface(NULL) == NULL);

    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_remote_subsurface_status(NULL, &json);
    assert(strcmp(woodland_json_data(&json), "{\"available\":false}") == 0);
    woodland_json_finish(&json);
}

int main(void)
{
    test_null_defaults();

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
#else
    server.renderer = wlr_renderer_autocreate(server.backend);
#endif
    assert(server.renderer != NULL);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
#if WOODLAND_HAVE_EFFECTS
    if (server.allocator == NULL) {
        fprintf(stderr, "no dmabuf allocator for fx_renderer, skip\n");
        wlr_renderer_destroy(server.renderer);
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 77;
    }
#endif
    assert(server.allocator != NULL);

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);
    wlr_subcompositor_create(server.display);
    wlr_renderer_init_wl_shm(server.renderer, server.display);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    server.config = woodland_config_create_default();
    assert(server.config != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_ime_init(&server));
    assert(woodland_session_lock_init(&server));
    assert(woodland_workspace_init(&server));
    assert(woodland_layer_init(&server));
    assert(woodland_shell_init(&server));
    assert(woodland_protocol_init(&server));

    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);
    wlr_output_layout_add(server.output_layout, out, 0, 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *client = wl_client_create(server.display, fds[0]);
    assert(client != NULL);
    struct wl_display *client_display = wl_display_connect_to_fd(fds[1]);
    assert(client_display != NULL);

    struct client_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(client_display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    pump_roundtrip(&server, client_display);

    assert(state.compositor != NULL);
    assert(state.subcompositor != NULL);
    assert(state.shm != NULL);
    assert(state.wm_base != NULL);
    assert(state.remote_mgr != NULL);
    xdg_wm_base_add_listener(state.wm_base, &wm_base_listener, NULL);

    /* 1. 创建并 map 顶层父窗口 (100x100) */
    struct wl_surface *parent_surf = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *parent_xdg = xdg_wm_base_get_xdg_surface(state.wm_base, parent_surf);
    xdg_surface_add_listener(parent_xdg, &surface_listener, NULL);
    struct xdg_toplevel *parent_top = xdg_surface_get_toplevel(parent_xdg);
    xdg_toplevel_add_listener(parent_top, &toplevel_listener, NULL);
    wl_surface_commit(parent_surf);
    pump_roundtrip(&server, client_display);

    struct wl_buffer *parent_buf = create_shm_buffer(state.shm, 100, 100);
    wl_surface_attach(parent_surf, parent_buf, 0, 0);
    wl_surface_commit(parent_surf);
    pump_roundtrip(&server, client_display);

    assert(!wl_list_empty(&server.views));
    struct woodland_view *parent_view = wl_container_of(server.views.next, parent_view, link);
    assert(parent_view != NULL && parent_view->mapped);
    wlr_scene_node_set_position(&parent_view->scene_tree->node, 200, 150);

    /* 2. 导出父表面与两个子表面 */
    struct export_record parent_exp_rec = {0};
    struct export_record child1_exp_rec = {0};
    struct export_record child2_exp_rec = {0};

    struct treeland_exported_surface_v1 *parent_exp =
        treeland_remote_subsurface_manager_v1_export_surface(state.remote_mgr, parent_surf);
    treeland_exported_surface_v1_add_listener(parent_exp, &exported_listener, &parent_exp_rec);

    struct wl_surface *child1_surf = wl_compositor_create_surface(state.compositor);
    struct treeland_exported_surface_v1 *child1_exp =
        treeland_remote_subsurface_manager_v1_export_surface(state.remote_mgr, child1_surf);
    treeland_exported_surface_v1_add_listener(child1_exp, &exported_listener, &child1_exp_rec);

    struct wl_surface *child2_surf = wl_compositor_create_surface(state.compositor);
    struct treeland_exported_surface_v1 *child2_exp =
        treeland_remote_subsurface_manager_v1_export_surface(state.remote_mgr, child2_surf);
    treeland_exported_surface_v1_add_listener(child2_exp, &exported_listener, &child2_exp_rec);

    pump_roundtrip(&server, client_display);
    assert(strlen(parent_exp_rec.token) == 36);
    assert(strlen(child1_exp_rec.token) == 36);
    assert(strlen(child2_exp_rec.token) == 36);
    assert(strcmp(parent_exp_rec.token, child1_exp_rec.token) != 0);

    /* 3. 测试无效 parent_token 拒绝 */
    struct treeland_remote_subsurface_v1 *invalid_remote =
        treeland_exported_surface_v1_create_remote_subsurface(child1_exp, "missing-parent");
    pump_roundtrip(&server, client_display);
    assert(child1_exp_rec.rejected_count == 1);
    assert(child1_exp_rec.rejected_reason ==
           TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_INVALID_TOKEN);
    assert(strcmp(child1_exp_rec.rejected_token, "missing-parent") == 0);
    treeland_remote_subsurface_v1_destroy(invalid_remote);

    /* 4. 测试自环拒绝 (cyclic) */
    struct treeland_remote_subsurface_v1 *self_remote =
        treeland_exported_surface_v1_create_remote_subsurface(child1_exp, child1_exp_rec.token);
    pump_roundtrip(&server, client_display);
    assert(child1_exp_rec.rejected_count == 2);
    assert(child1_exp_rec.rejected_reason ==
           TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_CYCLIC);
    treeland_remote_subsurface_v1_destroy(self_remote);

    /* 5. 建立合法父子关系 child1 -> parent，并检验祖先环拒绝 parent -> child1 */
    struct remote_record remote1_rec = {0};
    struct treeland_remote_subsurface_v1 *remote1 =
        treeland_exported_surface_v1_create_remote_subsurface(child1_exp, parent_exp_rec.token);
    treeland_remote_subsurface_v1_add_listener(remote1, &remote_listener, &remote1_rec);
    treeland_remote_subsurface_v1_set_position(remote1, -12, 34);
    treeland_remote_subsurface_v1_place_below(remote1, "");
    pump_roundtrip(&server, client_display);

    struct treeland_remote_subsurface_v1 *cycle_remote =
        treeland_exported_surface_v1_create_remote_subsurface(parent_exp, child1_exp_rec.token);
    pump_roundtrip(&server, client_display);
    assert(parent_exp_rec.rejected_count == 1);
    assert(parent_exp_rec.rejected_reason ==
           TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_CYCLIC);
    treeland_remote_subsurface_v1_destroy(cycle_remote);

    /* 6. 挂载 child1 buffer 触发 map，验证场景位置与输入命中反查到父视图 */
    struct wl_buffer *child1_buf = create_shm_buffer(state.shm, 40, 40);
    wl_surface_attach(child1_surf, child1_buf, 0, 0);
    wl_surface_commit(child1_surf);
    pump_roundtrip(&server, client_display);

    /* child1 相对 parent (200, 150) 偏移 (-12, 34) => 布局坐标 (188, 184)..(228, 224)
     * 取 (190, 190)（在 parent (200,150) 左边缘之外，仅命中 child1） */
    double sx = 0, sy = 0;
    struct wlr_surface *hit_surf = NULL;
    struct woodland_view *hit_view = woodland_view_at(&server, 190.0, 190.0, &hit_surf, &sx, &sy);
    assert(hit_view == parent_view);
    assert(hit_surf != NULL);
    assert(woodland_remote_subsurface_parent(hit_surf) == woodland_view_surface(parent_view));
    assert(woodland_surface_get_root_surface(hit_surf) == woodland_view_surface(parent_view));

    /* 7. 验证 invalid_sibling 与 sibling Z 序重排 */
    treeland_remote_subsurface_v1_place_above(remote1, "missing-sibling");
    pump_roundtrip(&server, client_display);
    assert(remote1_rec.invalid_sibling_count == 1);
    assert(strcmp(remote1_rec.sibling_token, "missing-sibling") == 0);

    struct remote_record remote2_rec = {0};
    struct treeland_remote_subsurface_v1 *remote2 =
        treeland_exported_surface_v1_create_remote_subsurface(child2_exp, parent_exp_rec.token);
    treeland_remote_subsurface_v1_add_listener(remote2, &remote_listener, &remote2_rec);
    treeland_remote_subsurface_v1_place_above(remote2, child1_exp_rec.token);
    treeland_remote_subsurface_v1_place_below(remote1, parent_exp_rec.token);
    treeland_remote_subsurface_v1_place_above(remote2, parent_exp_rec.token);
    treeland_remote_subsurface_v1_place_above(remote2, "");
    pump_roundtrip(&server, client_display);

    /* 8. 同时存在原生 wl_subsurface 与 remote_subsurface 时不崩溃 */
    struct wl_surface *native_sub_surf = wl_compositor_create_surface(state.compositor);
    struct wl_subsurface *native_sub =
        wl_subcompositor_get_subsurface(state.subcompositor, native_sub_surf, parent_surf);
    struct wl_buffer *native_buf = create_shm_buffer(state.shm, 20, 20);
    wl_surface_attach(native_sub_surf, native_buf, 0, 0);
    wl_surface_commit(native_sub_surf);
    wl_surface_commit(parent_surf);
    pump_roundtrip(&server, client_display);

    /* 9. 验证 wl_surface.offset 在 remote_subsurface 上被忽略 */
    wl_surface_offset(child1_surf, 50, 60);
    wl_surface_commit(child1_surf);
    pump_roundtrip(&server, client_display);
    assert(hit_surf->current.dx == 0 && hit_surf->current.dy == 0);

    /* 10. 验证状态 JSON 输出 */
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_remote_subsurface_status(&server, &json);
    const char *status = woodland_json_data(&json);
    assert(strstr(status, "\"available\":true") != NULL);
    assert(strstr(status, "\"active_exports\":3") != NULL);
    assert(strstr(status, "\"active_subsurfaces\":2") != NULL);
    assert(strstr(status, "\"create_count\":2") != NULL);
    assert(strstr(status, "\"reject_count\":3") != NULL);
    assert(strstr(status, "\"invalid_sibling_count\":1") != NULL);
    woodland_json_finish(&json);

    /* 11. 验证销毁 exported_surface 仅撤销 token，不破坏既有父子关系，且可重新导出 */
    char old_parent_token[129];
    snprintf(old_parent_token, sizeof(old_parent_token), "%s", parent_exp_rec.token);
    treeland_exported_surface_v1_destroy(parent_exp);
    pump_roundtrip(&server, client_display);

    /* 既有 child1 -> parent 关系依然有效 */
    assert(woodland_remote_subsurface_parent(hit_surf) == woodland_view_surface(parent_view));
    assert(woodland_view_at(&server, 190.0, 190.0, &hit_surf, &sx, &sy) == parent_view);

    /* 旧 token 已被撤销，新绑定应被 INVALID_TOKEN 拒绝 */
    treeland_remote_subsurface_v1_destroy(remote2);
    pump_roundtrip(&server, client_display);
    struct treeland_remote_subsurface_v1 *revoked_remote =
        treeland_exported_surface_v1_create_remote_subsurface(child2_exp, old_parent_token);
    pump_roundtrip(&server, client_display);
    assert(child2_exp_rec.rejected_count == 1);
    assert(child2_exp_rec.rejected_reason ==
           TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_INVALID_TOKEN);
    treeland_remote_subsurface_v1_destroy(revoked_remote);

    /* 重新导出同一 parent_surf 可获得新 token */
    struct export_record parent_reexp_rec = {0};
    parent_exp =
        treeland_remote_subsurface_manager_v1_export_surface(state.remote_mgr, parent_surf);
    treeland_exported_surface_v1_add_listener(parent_exp, &exported_listener, &parent_reexp_rec);
    pump_roundtrip(&server, client_display);
    assert(strlen(parent_reexp_rec.token) == 36);
    assert(strcmp(parent_reexp_rec.token, old_parent_token) != 0);

    /* 12. 清理资源 */
    wl_subsurface_destroy(native_sub);
    wl_surface_destroy(native_sub_surf);
    wl_buffer_destroy(native_buf);

    treeland_remote_subsurface_v1_destroy(remote1);
    treeland_exported_surface_v1_destroy(child2_exp);
    treeland_exported_surface_v1_destroy(child1_exp);
    wl_surface_destroy(child2_surf);
    wl_surface_destroy(child1_surf);
    wl_buffer_destroy(child1_buf);
    treeland_exported_surface_v1_destroy(parent_exp);
    treeland_remote_subsurface_manager_v1_destroy(state.remote_mgr);

    xdg_toplevel_destroy(parent_top);
    xdg_surface_destroy(parent_xdg);
    wl_surface_destroy(parent_surf);
    wl_buffer_destroy(parent_buf);
    xdg_wm_base_destroy(state.wm_base);
    wl_subcompositor_destroy(state.subcompositor);
    wl_shm_destroy(state.shm);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(registry);
    pump_roundtrip(&server, client_display);
    wl_display_disconnect(client_display);
    wl_display_destroy_clients(server.display);

    woodland_protocol_finish(&server);
    woodland_shell_finish(&server);
    woodland_layer_finish(&server);
    woodland_workspace_finish(&server);
    woodland_session_lock_finish(&server);
    woodland_ime_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);
    woodland_config_destroy(server.config);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    printf("remote_subsurface unit tests passed\n");
    return 0;
}
