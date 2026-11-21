// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <pixman.h>
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
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "config.h"
#if WOODLAND_HAVE_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include "scene.h"

#include "cursor.h"
#include "effect.h"
#include "layer.h"
#include "output.h"
#include "seat.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

#include "test-popup-xdg-shell-client-protocol.h"

struct client_state {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)version;
    struct client_state *st = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        st->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        st->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        uint32_t bind_ver = version < 3 ? version : 3;
        st->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, bind_ver);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

struct popup_record {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    bool configured;
    bool repositioned;
    uint32_t reposition_token;
};

static void popup_configure(void *data, struct xdg_popup *popup,
                            int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)popup;
    struct popup_record *rec = data;
    rec->x = x;
    rec->y = y;
    rec->width = width;
    rec->height = height;
    rec->configured = true;
}

static void popup_done(void *data, struct xdg_popup *popup) {
    (void)data; (void)popup;
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
    (void)popup;
    struct popup_record *rec = data;
    rec->repositioned = true;
    rec->reposition_token = token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void sync_callback(void *data, struct wl_callback *cb, uint32_t serial) {
    bool *done = data;
    *done = true;
    wl_callback_destroy(cb);
    (void)serial;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_callback,
};

static void pump_roundtrip(struct woodland_server *server, struct wl_display *client_disp) {
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

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int width, int height) {
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("test-shm", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

#if WOODLAND_HAVE_EFFECTS
static struct wlr_scene_blur *find_tree_blur(struct wlr_scene_tree *tree) {
    if (!tree) {
        return NULL;
    }
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->type == WLR_SCENE_NODE_BLUR) {
            return wlr_scene_blur_from_node(node);
        }
    }
    return NULL;
}
#endif

int main(void) {
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
    assert(woodland_workspace_init(&server));
    assert(woodland_shell_init(&server));

    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);
    wlr_output_layout_add(server.output_layout, out, 0, 0);

    /* 设置工作区可用区域为 800x600 */
    struct woodland_output *wo = wl_container_of(server.outputs.next, wo, link);
    wo->usable_area = (struct wlr_box){.x = 0, .y = 0, .width = 800, .height = 600};
    wo->usable_area_valid = true;

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
    assert(state.shm != NULL);
    assert(state.wm_base != NULL);
    xdg_wm_base_add_listener(state.wm_base, &wm_base_listener, NULL);

    /* 创建顶层窗口 */
    struct wl_surface *top_surf = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *top_xdg_surf = xdg_wm_base_get_xdg_surface(state.wm_base, top_surf);
    xdg_surface_add_listener(top_xdg_surf, &surface_listener, NULL);
    struct xdg_toplevel *top_toplevel = xdg_surface_get_toplevel(top_xdg_surf);
    xdg_toplevel_add_listener(top_toplevel, &toplevel_listener, NULL);

    /* 初始 commit 获取 configure */
    wl_surface_commit(top_surf);
    pump_roundtrip(&server, client_display);

    /* 挂载 buffer 触发 map */
    struct wl_buffer *top_buf = create_shm_buffer(state.shm, 200, 200);
    wl_surface_attach(top_surf, top_buf, 0, 0);
    wl_surface_commit(top_surf);
    pump_roundtrip(&server, client_display);

    assert(!wl_list_empty(&server.views));
    struct woodland_view *view = wl_container_of(server.views.next, view, link);
    assert(view != NULL);
    /* 把窗口定位在 (600, 450)，贴近 800x600 的右下角 */
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, 600, 450);
    }

    /* 创建第二个顶层窗口，初始时排在视图列表最前端 */
    struct wl_surface *top_surf2 = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *top_xdg_surf2 = xdg_wm_base_get_xdg_surface(state.wm_base, top_surf2);
    xdg_surface_add_listener(top_xdg_surf2, &surface_listener, NULL);
    struct xdg_toplevel *top_toplevel2 = xdg_surface_get_toplevel(top_xdg_surf2);
    xdg_toplevel_add_listener(top_toplevel2, &toplevel_listener, NULL);
    wl_surface_commit(top_surf2);
    pump_roundtrip(&server, client_display);
    struct wl_buffer *top_buf2 = create_shm_buffer(state.shm, 200, 200);
    wl_surface_attach(top_surf2, top_buf2, 0, 0);
    wl_surface_commit(top_surf2);
    pump_roundtrip(&server, client_display);

    struct woodland_view *topmost = wl_container_of(server.views.next, topmost, link);
    assert(topmost != view); // 确认新建窗口处于上层

    /* ============================================================
     * 测试 1：直接 popup（xdg_toplevel 的子级）避让屏幕边缘，
     * 且触发弹出时所属窗口自动提升至最顶层，确保 popup 在普通窗口之上。
     * Anchor 在窗口 (100, 100, 50, 50)，大小 250x200，向右下展开。
     * 若不避让：layout 终点为 (600+150+250=1000, 450+150+200=800)，越界。
     * ============================================================ */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(state.wm_base);
    xdg_positioner_set_size(pos, 250, 200);
    xdg_positioner_set_anchor_rect(pos, 100, 100, 50, 50);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    struct wl_surface *pop_surf = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *pop_xdg_surf = xdg_wm_base_get_xdg_surface(state.wm_base, pop_surf);
    xdg_surface_add_listener(pop_xdg_surf, &surface_listener, NULL);
    struct xdg_popup *pop = xdg_surface_get_popup(pop_xdg_surf, top_xdg_surf, pos);

    struct popup_record pop_rec = {0};
    xdg_popup_add_listener(pop, &popup_listener, &pop_rec);

    wl_surface_commit(pop_surf);
    pump_roundtrip(&server, client_display);

    /* 验证 view 已被提升至栈顶，确保其 popup 处于普通窗口之上 */
    topmost = wl_container_of(server.views.next, topmost, link);
    assert(topmost == view);

    assert(pop_rec.configured);
    int pop_layout_x = 600 + pop_rec.x;
    int pop_layout_y = 450 + pop_rec.y;
    assert(pop_layout_x >= 0);
    assert(pop_layout_x + pop_rec.width <= 800);
    assert(pop_layout_y >= 0);
    assert(pop_layout_y + pop_rec.height <= 600);

    /* commit pop_surf 挂载 buffer 确认其 committed 几何生效 */
    struct wl_buffer *pop_buf = create_shm_buffer(state.shm, pop_rec.width, pop_rec.height);
    wl_surface_attach(pop_surf, pop_buf, 0, 0);
    wl_surface_commit(pop_surf);
    pump_roundtrip(&server, client_display);

    /* ============================================================
     * 测试 2：嵌套 popup（子菜单）避让屏幕边缘
     * Anchor 设在 popup1 的右侧 (230, 20, 20, 20)，大小 200x150，向右展开。
     * ============================================================ */
    struct xdg_positioner *nested_pos = xdg_wm_base_create_positioner(state.wm_base);
    xdg_positioner_set_size(nested_pos, 200, 150);
    xdg_positioner_set_anchor_rect(nested_pos, 230, 20, 20, 20);
    xdg_positioner_set_anchor(nested_pos, XDG_POSITIONER_ANCHOR_RIGHT);
    xdg_positioner_set_gravity(nested_pos, XDG_POSITIONER_GRAVITY_RIGHT);
    xdg_positioner_set_constraint_adjustment(nested_pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    struct wl_surface *nested_surf = wl_compositor_create_surface(state.compositor);
    struct xdg_surface *nested_xdg_surf = xdg_wm_base_get_xdg_surface(state.wm_base, nested_surf);
    xdg_surface_add_listener(nested_xdg_surf, &surface_listener, NULL);
    struct xdg_popup *nested_pop = xdg_surface_get_popup(nested_xdg_surf, pop_xdg_surf, nested_pos);

    struct popup_record nested_rec = {0};
    xdg_popup_add_listener(nested_pop, &popup_listener, &nested_rec);

    wl_surface_commit(nested_surf);
    pump_roundtrip(&server, client_display);

    assert(nested_rec.configured);
    int nested_layout_x = 600 + pop_rec.x + nested_rec.x;
    int nested_layout_y = 450 + pop_rec.y + nested_rec.y;
    assert(nested_layout_x >= 0);
    assert(nested_layout_x + nested_rec.width <= 800);
    assert(nested_layout_y >= 0);
    assert(nested_layout_y + nested_rec.height <= 600);

    /* ============================================================
     * 测试 3：动态 reposition 重新避让
     * ============================================================ */
    struct xdg_positioner *repo_pos = xdg_wm_base_create_positioner(state.wm_base);
    xdg_positioner_set_size(repo_pos, 220, 160);
    xdg_positioner_set_anchor_rect(repo_pos, 240, 30, 10, 10);
    xdg_positioner_set_anchor(repo_pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
    xdg_positioner_set_gravity(repo_pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(repo_pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    nested_rec.repositioned = false;
    xdg_popup_reposition(nested_pop, repo_pos, 42);
    pump_roundtrip(&server, client_display);

    assert(nested_rec.repositioned);
    assert(nested_rec.reposition_token == 42);
    nested_layout_x = 600 + pop_rec.x + nested_rec.x;
    nested_layout_y = 450 + pop_rec.y + nested_rec.y;
    assert(nested_layout_x >= 0);
    assert(nested_layout_x + nested_rec.width <= 800);
    assert(nested_layout_y >= 0);
    assert(nested_layout_y + nested_rec.height <= 600);

    /* 为嵌套 popup 挂载 ARGB8888 buffer，使其具备有效尺寸与透明通道 */
    struct wl_buffer *nested_buf = create_shm_buffer(state.shm, nested_rec.width, nested_rec.height);
    wl_surface_attach(nested_surf, nested_buf, 0, 0);
    wl_surface_commit(nested_surf);
    pump_roundtrip(&server, client_display);

#if WOODLAND_HAVE_EFFECTS
    /* ============================================================
     * 测试 4：Popup 模糊（显式区域与父级自动继承，强制 live blur）
     * ============================================================ */
    assert(!wl_list_empty(&view->xdg_toplevel->base->popups));
    struct wlr_xdg_popup *srv_pop =
        wl_container_of(view->xdg_toplevel->base->popups.next, srv_pop, link);
    assert(!wl_list_empty(&srv_pop->base->popups));
    struct wlr_xdg_popup *srv_nested =
        wl_container_of(srv_pop->base->popups.next, srv_nested, link);

    server.config->effects.blur = true;
    server.config->effects.blur_optimized = true;

    /* 测试 A：显式设置 popup 模糊区域，不污染父窗口，且强制 live blur */
    pixman_region32_t blur_reg;
    pixman_region32_init_rect(&blur_reg, 0, 0, pop_rec.width, pop_rec.height);
    assert(woodland_effect_set_popup_blur_region(srv_pop, &blur_reg));
    assert(!view->effect.blur);

    struct wlr_scene_blur *pop_blur = find_tree_blur(srv_pop->base->data);
    assert(pop_blur != NULL);
    assert(pop_blur->node.enabled);
    assert(pop_blur->width == pop_rec.width);
    assert(pop_blur->height == pop_rec.height);
    assert(!pop_blur->should_only_blur_bottom_layer);

    struct wlr_scene_blur *nested_blur = find_tree_blur(srv_nested->base->data);
    assert(nested_blur != NULL);
    assert(nested_blur->node.enabled);
    assert(!nested_blur->should_only_blur_bottom_layer);

    woodland_effect_clear_popup_blur_region(srv_pop);
    assert(!pop_blur->node.enabled);
    assert(!nested_blur->node.enabled);

    /* 测试 B：从父窗口自动继承模糊到透明子 popup 与嵌套子 popup */
    pixman_region32_clear(&blur_reg);
    pixman_region32_init_rect(&blur_reg, 0, 0, 200, 200);
    woodland_effect_set_blur_region(view, &blur_reg, 200, 200);
    assert(view->effect.blur);
    assert(pop_blur->node.enabled);
    assert(!pop_blur->should_only_blur_bottom_layer);
    assert(nested_blur->node.enabled);
    assert(!nested_blur->should_only_blur_bottom_layer);

    woodland_effect_set_blur_region(view, NULL, 0, 0);
    assert(!view->effect.blur);
    assert(!pop_blur->node.enabled);
    assert(!nested_blur->node.enabled);
    pixman_region32_fini(&blur_reg);
#endif

    /* 清理客户端资源 */
    xdg_positioner_destroy(repo_pos);
    xdg_positioner_destroy(nested_pos);
    xdg_positioner_destroy(pos);
    xdg_popup_destroy(nested_pop);
    xdg_surface_destroy(nested_xdg_surf);
    wl_surface_destroy(nested_surf);
    wl_buffer_destroy(nested_buf);
    xdg_popup_destroy(pop);
    xdg_surface_destroy(pop_xdg_surf);
    wl_surface_destroy(pop_surf);
    wl_buffer_destroy(pop_buf);
    xdg_toplevel_destroy(top_toplevel2);
    xdg_surface_destroy(top_xdg_surf2);
    wl_surface_destroy(top_surf2);
    wl_buffer_destroy(top_buf2);
    xdg_toplevel_destroy(top_toplevel);
    xdg_surface_destroy(top_xdg_surf);
    wl_surface_destroy(top_surf);
    wl_buffer_destroy(top_buf);
    xdg_wm_base_destroy(state.wm_base);
    wl_shm_destroy(state.shm);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(client_display);

    return 0;
}
