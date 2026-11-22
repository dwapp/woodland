// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xwayland 基础支持。
 *
 * 分两条路径：
 *   - managed（普通顶层窗口）：建 woodland_view，走与 xdg 完全相同的
 *     SSD 装饰 / 放置 / 聚焦 / 最大化 / 贴边吸附逻辑；
 *   - unmanaged（override-redirect：菜单、提示、拖拽影子）：单独一棵
 *     scene 树，按 X 侧坐标定位，无装饰、不参与焦点与窗口列表。
 *
 * 时序（与 labwc 一致）：new_surface → map_request（拿得到尺寸，可放置并
 * configure）→ associate（wlr_surface 可用，挂 surface 事件）→ map（真正上屏）。
 */
#include "util/listener.h"
#include "util/global_filter.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <pixman.h>
#include "cursor.h"
#include "output.h"
#include "placement.h"
#include "woodland_config.h"

#include "scene.h"
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/xwayland.h>

#include "effect.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "xwayland.h"
#include "xwayland_outputs.h"
#include "xwayland_scale.h"
#include "focus.h"

/* ==================== unmanaged（override-redirect） ==================== */

struct woodland_unmanaged {
    struct wl_list link;               // server->xwayland_unmanaged
    struct woodland_server *server;
    struct wlr_xwayland_surface *xsurface;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_surface *scene_surface;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener set_override_redirect;
    struct wl_listener request_configure;
    struct wl_listener grab_focus;
};

static void unmanaged_set_geometry(struct woodland_unmanaged *unmanaged);
static void unmanaged_map(struct wl_listener *listener, void *data);
static void unmanaged_unmap(struct wl_listener *listener, void *data);
static void unmanaged_update_filter_mode(struct woodland_unmanaged *unmanaged);
static void unmanaged_set_override_redirect(struct wl_listener *listener, void *data);
static void xwl_new_view(struct woodland_server *server, struct wlr_xwayland_surface *xsurface);
static void xwl_new_unmanaged(struct woodland_server *server, struct wlr_xwayland_surface *xsurface);
static void xwl_destroy(struct wl_listener *listener, void *data);

/* ==================== native_resolution：X 坐标 <-> 布局坐标 ==================== */

/*
 * 命中测试回调：native_resolution 下 surface 被缩回逻辑尺寸显示，scene 给的
 * 节点内坐标是逻辑单位，而 surface 本地坐标是 X 像素。按"surface 尺寸 / 显示
 * 尺寸"把它放大回去；返回的 sx/sy 会原样交给 wlr_seat 做指针进入/移动，
 * 所以 X11 客户端收到的是它自己的像素坐标。
 */
static bool xwl_point_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (!scene_surface || !scene_surface->surface) {
        return false;
    }
    struct wlr_surface *surface = scene_surface->surface;
    if (buffer->dst_width > 0 && buffer->dst_height > 0) {
        *sx *= (double)surface->current.width / buffer->dst_width;
        *sy *= (double)surface->current.height / buffer->dst_height;
    }
    return wlr_surface_point_accepts_input(surface, *sx, *sy);
}

/*
 * 把 surface 缩回 region 对应的逻辑尺寸。必须在 wlr_scene_surface 自己处理完
 * commit 之后调用（它每次 commit 都会把显示尺寸重置成 surface 的像素尺寸）。
 * region 为 NULL（未启用 native_resolution）时什么也不做。
 * 返回显示尺寸（dst_width / dst_height）是否发生了变化。
 */
static bool xwl_rescale_surface(struct wlr_scene_surface *scene_surface,
                                const struct woodland_xwl_region *region) {
    if (!region || !scene_surface || !scene_surface->buffer || !scene_surface->surface) {
        return false;
    }
    struct wlr_surface *surface = scene_surface->surface;
    int width = surface->current.width;
    int height = surface->current.height;
    if (width <= 0 || height <= 0) {
        return false;
    }
    int dst_w = woodland_xwl_to_layout_width(region, width);
    int dst_h = woodland_xwl_to_layout_height(region, height);
    if (dst_w < 1) {
        dst_w = 1;
    }
    if (dst_h < 1) {
        dst_h = 1;
    }
    bool changed = (scene_surface->buffer->dst_width != dst_w ||
                    scene_surface->buffer->dst_height != dst_h);

    pixman_region32_t opaque;
    pixman_region32_init(&opaque);
    if (region->scale != 1.0 && region->scale > 0.0) {
        wlr_region_scale(&opaque, &surface->opaque_region, (float)(1.0 / region->scale));
    } else {
        pixman_region32_copy(&opaque, &surface->opaque_region);
    }
    wlr_scene_buffer_set_opaque_region(scene_surface->buffer, &opaque);
    pixman_region32_fini(&opaque);

    wlr_scene_buffer_set_dest_size(scene_surface->buffer, dst_w, dst_h);
    scene_surface->buffer->point_accepts_input = xwl_point_accepts_input;
    return changed;
}

static bool xwl_native(const struct woodland_server *server) {
    return server && server->xwl_map.enabled;
}

/* 受管理视图：按它所在输出把 surface 缩回逻辑尺寸（未启用时空操作） */
static bool xwl_rescale_view(struct woodland_view *view) {
    struct woodland_xwl_region region;
    if (view->scene_surface && woodland_xwl_view_region(view, &region)) {
        return xwl_rescale_surface(view->scene_surface, &region);
    }
    return false;
}

/* override-redirect 窗口：位置按 X 坐标换回布局坐标，并缩回逻辑尺寸 */
static void unmanaged_place(struct woodland_unmanaged *unmanaged, int x, int y) {
    if (xwl_native(unmanaged->server)) {
        struct woodland_xwl_region region;
        woodland_xwl_map_region_at_x(&unmanaged->server->xwl_map, x, y, &region);
        wlr_scene_node_set_position(&unmanaged->scene_tree->node,
                                    woodland_xwl_to_layout_x(&region, x),
                                    woodland_xwl_to_layout_y(&region, y));
        xwl_rescale_surface(unmanaged->scene_surface, &region);
    } else {
        wlr_scene_node_set_position(&unmanaged->scene_tree->node, x, y);
    }
}

static void unmanaged_set_geometry(struct woodland_unmanaged *unmanaged) {
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;
    unmanaged_place(unmanaged, xsurface->x, xsurface->y);
    unmanaged_update_filter_mode(unmanaged);
}
static enum wlr_scale_filter_mode xwl_get_scale_filter_mode(struct woodland_server *server,
                                                            float output_scale)
{
    enum woodland_scale_filter sf = WOODLAND_SCALE_FILTER_SMART;
    if (server && server->config) {
        sf = server->config->xwayland.scale_filter;
    }

    switch (sf) {
    case WOODLAND_SCALE_FILTER_NEAREST:
        return (output_scale > 1.0f) ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR;
    case WOODLAND_SCALE_FILTER_BILINEAR:
        return WLR_SCALE_FILTER_BILINEAR;
    case WOODLAND_SCALE_FILTER_SMART:
    default:
        if (output_scale > 1.0f && floorf(output_scale) == output_scale) {
            return WLR_SCALE_FILTER_NEAREST;
        }
        return WLR_SCALE_FILTER_BILINEAR;
    }
}

static void unmanaged_update_filter_mode(struct woodland_unmanaged *unmanaged) {
    if (!unmanaged || !unmanaged->scene_surface || !unmanaged->server) {
        return;
    }
    struct woodland_output *wo = woodland_output_at(unmanaged->server,
        unmanaged->scene_tree->node.x, unmanaged->scene_tree->node.y);
    float scale = (wo && wo->wlr_output && wo->wlr_output->scale > 0.0f) ? wo->wlr_output->scale : 1.0f;
    enum wlr_scale_filter_mode mode = xwl_get_scale_filter_mode(unmanaged->server, scale);
    if (unmanaged->scene_surface->buffer) {
        wlr_scene_buffer_set_filter_mode(unmanaged->scene_surface->buffer, mode);
    }
}

static void unmanaged_commit(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, commit);
    (void)data;
    if (!xwl_native(unmanaged->server)) {
        return;
    }
    /* 注册在 wlr_scene_surface 的 commit 监听之后，此时它已重置过显示尺寸 */
    struct woodland_xwl_region region;
    woodland_xwl_map_region_at_x(&unmanaged->server->xwl_map, unmanaged->xsurface->x,
                                 unmanaged->xsurface->y, &region);
    xwl_rescale_surface(unmanaged->scene_surface, &region);
}

/* 挂上 wlr_surface：建 scene surface + 接 map/unmap */
static void unmanaged_attach_surface(struct woodland_unmanaged *unmanaged,
                                     struct wlr_surface *surface) {
    if (!surface || unmanaged->scene_surface) {
        return;
    }
    unmanaged->scene_surface = wlr_scene_surface_create(unmanaged->scene_tree,
                                                        surface);
    woodland_listener_connect(&unmanaged->map, &surface->events.map,
                          unmanaged_map, "xwayland.map");
    woodland_listener_connect(&unmanaged->unmap, &surface->events.unmap,
                          unmanaged_unmap, "xwayland.unmap");
    woodland_listener_connect(&unmanaged->commit, &surface->events.commit,
                          unmanaged_commit, "xwayland.unmanaged_commit");
    /* set_geometry 里已经会刷一次滤波（buffer 这时通常还没有，是空操作），
     * 真正的第一次生效在 map 之后 */
    if (surface->mapped) {
        unmanaged_map(&unmanaged->map, NULL);
    }
}

static void unmanaged_associate(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, associate);
    unmanaged_attach_surface(unmanaged, unmanaged->xsurface->surface);
    (void)data;
}

static void unmanaged_dissociate(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, dissociate);
    /* surface 正在销毁：先摘监听，scene surface 随 surface 自动释放 */
    if (unmanaged->scene_surface) {
        wl_list_remove(&unmanaged->map.link);
        wl_list_remove(&unmanaged->unmap.link);
        wl_list_remove(&unmanaged->commit.link);
        unmanaged->scene_surface = NULL;
    }
    (void)data;
}

static void unmanaged_map(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, map);
    unmanaged_set_geometry(unmanaged);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&unmanaged->scene_tree->node);
    if (unmanaged->xsurface &&
        wlr_xwayland_surface_override_redirect_wants_focus(unmanaged->xsurface)) {
        woodland_seat_focus_surface(unmanaged->server, unmanaged->xsurface->surface);
    }
    (void)data;
}

static void unmanaged_unmap(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, unmap);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, false);
    struct woodland_server *server = unmanaged->server;
    if (server && server->seat && unmanaged->xsurface &&
        server->seat->keyboard_state.focused_surface == unmanaged->xsurface->surface) {
        struct woodland_unmanaged *u;
        bool focused_next = false;
        wl_list_for_each_reverse(u, &server->xwayland_unmanaged, link) {
            if (u != unmanaged && u->xsurface && u->xsurface->surface &&
                u->xsurface->surface->mapped &&
                wlr_xwayland_surface_override_redirect_wants_focus(u->xsurface)) {
                woodland_seat_focus_surface(server, u->xsurface->surface);
                focused_next = true;
                break;
            }
        }
        if (!focused_next) {
            if (server->focused_view && server->focused_view->mapped) {
                woodland_focus_view(server->focused_view, false);
            } else {
                woodland_focus_fallback(server);
            }
        }
    }
    (void)data;
}

/* override-redirect 窗口自己决定几何，跟随 X 侧的 ConfigureNotify 即可 */
static void unmanaged_set_geometry_event(struct wl_listener *listener,
                                         void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, set_geometry);
    unmanaged_set_geometry(unmanaged);
    (void)data;
}

static void unmanaged_destroy(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, destroy);
    wl_list_remove(&unmanaged->associate.link);
    wl_list_remove(&unmanaged->dissociate.link);
    wl_list_remove(&unmanaged->destroy.link);
    wl_list_remove(&unmanaged->set_geometry.link);
    wl_list_remove(&unmanaged->request_configure.link);
    wl_list_remove(&unmanaged->grab_focus.link);
    if (unmanaged->scene_surface) {
        wl_list_remove(&unmanaged->map.link);
        wl_list_remove(&unmanaged->unmap.link);
        wl_list_remove(&unmanaged->commit.link);
    }
    wl_list_remove(&unmanaged->link);
    wl_list_remove(&unmanaged->set_override_redirect.link);
    wlr_scene_node_destroy(&unmanaged->scene_tree->node);
    free(unmanaged);
    (void)data;
}
static void unmanaged_grab_focus(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, grab_focus);
    (void)data;
    if (unmanaged->xsurface && unmanaged->xsurface->surface) {
        woodland_seat_focus_surface(unmanaged->server, unmanaged->xsurface->surface);
    }
}

static void unmanaged_request_configure(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, request_configure);
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;
    struct wlr_xwayland_surface_configure_event *ev = data;
    int16_t x = (ev->mask & XCB_CONFIG_WINDOW_X) ? ev->x : xsurface->x;
    int16_t y = (ev->mask & XCB_CONFIG_WINDOW_Y) ? ev->y : xsurface->y;
    uint16_t width = (ev->mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : xsurface->width;
    uint16_t height = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : xsurface->height;
    wlr_xwayland_surface_configure(xsurface, x, y, width, height);
    unmanaged_place(unmanaged, x, y);
    unmanaged_update_filter_mode(unmanaged);
}

static void unmanaged_set_override_redirect(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, set_override_redirect);
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;
    (void)data;
    if (!xsurface || xsurface->override_redirect) {
        return;
    }
    struct woodland_server *server = unmanaged->server;
    bool mapped = xsurface->surface && xsurface->surface->mapped;
    if (mapped) {
        unmanaged_unmap(&unmanaged->unmap, NULL);
    }
    unmanaged_destroy(&unmanaged->destroy, NULL);
    xwl_new_view(server, xsurface);
}


static void xwl_new_unmanaged(struct woodland_server *server,
                              struct wlr_xwayland_surface *xsurface) {
    struct woodland_unmanaged *unmanaged = calloc(1, sizeof(*unmanaged));
    if (!unmanaged) {
        wlr_log(WLR_ERROR, "failed to allocate unmanaged X11 window");
        return;
    }
    unmanaged->server = server;
    unmanaged->xsurface = xsurface;
    unmanaged->scene_tree =
        wlr_scene_tree_create(server->xwayland_unmanaged_tree);

    woodland_listener_connect(&unmanaged->associate, &xsurface->events.associate,
                          unmanaged_associate, "xwayland.associate");
    woodland_listener_connect(&unmanaged->dissociate, &xsurface->events.dissociate,
                          unmanaged_dissociate, "xwayland.dissociate");
    woodland_listener_connect(&unmanaged->destroy, &xsurface->events.destroy,
                          unmanaged_destroy, "xwayland.destroy");
    woodland_listener_connect(&unmanaged->set_geometry, &xsurface->events.set_geometry,
                          unmanaged_set_geometry_event, "xwayland.set_geometry");
    woodland_listener_connect(&unmanaged->set_override_redirect, &xsurface->events.set_override_redirect,
                          unmanaged_set_override_redirect, "xwayland.set_override_redirect");

    woodland_listener_connect(&unmanaged->request_configure, &xsurface->events.request_configure,
                          unmanaged_request_configure, "xwayland.unmanaged_request_configure");
    woodland_listener_connect(&unmanaged->grab_focus, &xsurface->events.grab_focus,
                          unmanaged_grab_focus, "xwayland.unmanaged_grab_focus");
    wl_list_insert(&server->xwayland_unmanaged, &unmanaged->link);

    /* 多数情况下 surface 在 associate 里才出现；已存在时直接挂 */
    unmanaged_attach_surface(unmanaged, xsurface->surface);
}
bool woodland_xwayland_initial_position_hint(const struct wlr_xwayland_surface *xsurface,
                                            int16_t *x, int16_t *y) {
    if (!xsurface || !xsurface->size_hints) {
        return false;
    }
    if (!(xsurface->size_hints->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))) {
        return false;
    }
    if (x) *x = xsurface->x;
    if (y) *y = xsurface->y;
    return true;
}


/* ==================== managed 顶层窗口 ==================== */

/*
 * 放置并回 configure。
 *
 * X11 客户端必须收到 configure 才会上屏，所以"首次放置"和"ack 尺寸"
 * 合并成一次：先按 SSD 边距算出内容原点，再用同一个尺寸 configure。
 */
static void xwl_place_and_configure(struct woodland_view *view) {
    if (!view->placed) {
        int16_t x, y;
        if (woodland_xwayland_initial_position_hint(view->xwayland_surface, &x, &y)) {
            int lx = x, ly = y;
            if (xwl_native(view->server)) {
                /* 位置提示是 X 坐标，换回布局坐标 */
                struct woodland_xwl_region region;
                woodland_xwl_map_region_at_x(&view->server->xwl_map, x, y, &region);
                lx = woodland_xwl_to_layout_x(&region, x);
                ly = woodland_xwl_to_layout_y(&region, y);
            }
            wlr_scene_node_set_position(&view->scene_tree->node, lx, ly);
            view->placed = true;
            woodland_view_update_outputs(view);
        } else {
            woodland_view_place_initial(view);
            view->placed = true;
        }
    }
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    woodland_view_request_size(view, content.width, content.height);
}

static void xwl_map_request(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map_request);
    xwl_place_and_configure(view);
    (void)data;
}

static void xwl_map(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map);
    if (view->mapped) {
        return;
    }
    bool take_focus = view->focused_before_map;
    view->focused_before_map = false;
    woodland_view_map(view);
    if (take_focus) {
        struct woodland_server *server = view->server;
        struct wlr_surface *surface = woodland_view_surface(view);
        if (server && server->seat && surface && server->focused_view == view) {
            if (server->seat->keyboard_state.focused_surface != surface) {
                woodland_seat_focus_surface(server, surface);
            }
        }
    }
    (void)data;
}

static void xwl_unmap(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    woodland_view_unmap(view);
    (void)data;
}

static void xwl_commit(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, commit);
    /* 注册在 wlr_scene_surface 的 commit 监听之后：它刚把显示尺寸重置成像素尺寸 */
    xwl_rescale_view(view);
    woodland_ssd_update(view);
    if (!view->ssd) {
        woodland_effect_update(view);
    }
    (void)data;
}
static void xwl_update_deco(struct woodland_view *view);


static void xwl_associate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, associate);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface || !xsurface->surface) {
        return;
    }

    if (!view->scene_surface) {
        view->scene_surface = wlr_scene_surface_create(view->scene_tree,
                                                       xsurface->surface);
        woodland_xwayland_view_update_scale_filter(view);
    }

    woodland_listener_connect(&view->map, &xsurface->surface->events.map,
                          xwl_map, "xwayland.map");
    woodland_listener_connect(&view->unmap, &xsurface->surface->events.unmap,
                          xwl_unmap, "xwayland.unmap");
    woodland_listener_connect(&view->commit, &xsurface->surface->events.commit,
                          xwl_commit, "xwayland.commit");

    /*
     * EWMH: 窗口映射前可能已经设置了 _NET_WM_STATE（fullscreen/maximized）。
     * 在 associate 阶段定下初始全屏/最大化状态（对齐 labwc）。
     */
    if (xsurface->fullscreen) {
        woodland_view_set_fullscreen(view, true, NULL);
    } else if (xsurface->maximized_horz || xsurface->maximized_vert) {
        woodland_view_set_maximized(view, true);
    }

    /* 属性在 map_request 之后才读完，这里定下装饰策略：
     * _MOTIF_WM_HINTS 请求无边框的窗口不画 SSD（同 labwc want_deco） */
    xwl_update_deco(view);

    /* map_request 里还不知道父窗口，这里补上（供后续 remap 居中用） */
    if (!view->parent && xsurface->parent && xsurface->parent->data) {
        woodland_view_set_parent(view, xsurface->parent->data);
    }

    /* 若 surface 挂接监听器前已由 wlroots 完成 map，补调 xwl_map 确保视图上屏 */
    if (xsurface->surface->mapped) {
        xwl_map(&view->map, NULL);
    }

    (void)data;
}

static void xwl_dissociate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, dissociate);
    /* surface 正在销毁或解绑：先摘监听（幂等保护） */
    if (view->scene_surface) {
        wl_list_remove(&view->map.link);
        wl_list_remove(&view->unmap.link);
        wl_list_remove(&view->commit.link);
        view->scene_surface = NULL;
    }
    if (view->mapped) {
        woodland_view_unmap(view);
    }
    (void)data;
}

static void xwl_update_deco(struct woodland_view *view) {
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface) {
        return;
    }
    bool borderless =
        xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    if (borderless == view->deco_csd) {
        return;
    }

    struct woodland_ssd_margins old_m, new_m;
    woodland_ssd_get_margins(view, &old_m);

    view->deco_csd = borderless;
    if (view->mapped) {
        if (borderless) {
            woodland_ssd_destroy(view);
            woodland_effect_update(view);
        } else {
            woodland_ssd_create(view);
            woodland_effect_update(view);
        }
    }

    woodland_ssd_get_margins(view, &new_m);

    /* 外框左上角不动：平移内容原点补偿边距变化 */
    wlr_scene_node_set_position(&view->scene_tree->node,
                                view->scene_tree->node.x + (old_m.left - new_m.left),
                                view->scene_tree->node.y + (old_m.top - new_m.top));

    struct wlr_box content;
    woodland_view_content_box(view, &content);
    woodland_view_request_size(view, content.width, content.height);
}

static void xwl_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->associate.link);
    xwl_dissociate(&view->dissociate, NULL);
    wl_list_remove(&view->dissociate.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->map_request.link);
    wl_list_remove(&view->request_configure.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->request_activate.link);
    wl_list_remove(&view->request_close.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_class.link);
    wl_list_remove(&view->set_parent.link);
    wl_list_remove(&view->set_geometry.link);
    wl_list_remove(&view->set_override_redirect.link);
    wl_list_remove(&view->set_window_type.link);
    wl_list_remove(&view->request_skip_taskbar.link);
    wl_list_remove(&view->set_decorations.link);
    wl_list_remove(&view->focus_in.link);
    wl_list_remove(&view->grab_focus.link);
    if (view->xwayland_surface) {
        view->xwayland_surface->data = NULL;
    }
    woodland_view_destroy(view);
    (void)data;
}

static void xwl_request_configure(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!view || !view->xwayland_surface) {
        return;
    }
    struct woodland_xwl_region region;
    bool native = woodland_xwl_view_region(view, &region);

    /* 最大化/全屏下由 WM 决定几何；仅当客户端请求了几何时才回发 ConfigureNotify 驳回请求。
     * 例外：全屏游戏切到较低的 RandR 模拟分辨率时允许缩到 <= 输出尺寸，好让
     * Xwayland 通过 wp_viewport 把它拉伸铺满屏幕。 */
    if (view->maximized || view->fullscreen) {
        if (event->mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                           XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)) {
            int x = view->scene_tree ? view->scene_tree->node.x : 0;
            int y = view->scene_tree ? view->scene_tree->node.y : 0;
            if (native) {
                x = woodland_xwl_to_x(&region, x);
                y = woodland_xwl_to_y(&region, y);
            }
            if (x < INT16_MIN) {
                x = INT16_MIN;
            } else if (x > INT16_MAX) {
                x = INT16_MAX;
            }
            if (y < INT16_MIN) {
                y = INT16_MIN;
            } else if (y > INT16_MAX) {
                y = INT16_MAX;
            }

            uint16_t cfg_w = view->xwayland_surface->width;
            uint16_t cfg_h = view->xwayland_surface->height;
            if (view->fullscreen &&
                (event->mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))) {
                int req_w = (event->mask & XCB_CONFIG_WINDOW_WIDTH)
                                ? event->width
                                : view->xwayland_surface->width;
                int req_h = (event->mask & XCB_CONFIG_WINDOW_HEIGHT)
                                ? event->height
                                : view->xwayland_surface->height;
                int full_w = native && region.x.width > 0
                                 ? region.x.width
                                 : view->xwayland_surface->width;
                int full_h = native && region.x.height > 0
                                 ? region.x.height
                                 : view->xwayland_surface->height;
                if (!native && view->server && view->server->output_layout) {
                    struct woodland_output *wo = woodland_output_for_view(view);
                    if (wo && wo->wlr_output) {
                        struct wlr_box obox = {0};
                        wlr_output_layout_get_box(view->server->output_layout, wo->wlr_output,
                                                  &obox);
                        if (obox.width > 0 && obox.height > 0) {
                            full_w = obox.width;
                            full_h = obox.height;
                        }
                    }
                }
                if (req_w > 0 && req_h > 0 && req_w <= full_w && req_h <= full_h) {
                    cfg_w = (uint16_t)req_w;
                    cfg_h = (uint16_t)req_h;
                }
            }
            if (view->xwayland_surface->xwm) {
                wlr_xwayland_surface_configure(view->xwayland_surface,
                                               (int16_t)x, (int16_t)y,
                                               cfg_w, cfg_h);
            } else {
                view->xwayland_surface->x = (int16_t)x;
                view->xwayland_surface->y = (int16_t)y;
                view->xwayland_surface->width = cfg_w;
                view->xwayland_surface->height = cfg_h;
            }
        }
        return;
    }

    /* 浮动状态：同步客户端的位置请求（native_resolution 下它给的是 X 坐标） */
    if (event->mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) {
        int x = view->scene_tree->node.x;
        int y = view->scene_tree->node.y;
        if (native) {
            struct woodland_xwl_region at;
            woodland_xwl_map_region_at_x(&view->server->xwl_map,
                (event->mask & XCB_CONFIG_WINDOW_X) ? event->x : woodland_xwl_to_x(&region, x),
                (event->mask & XCB_CONFIG_WINDOW_Y) ? event->y : woodland_xwl_to_y(&region, y),
                &at);
            if (event->mask & XCB_CONFIG_WINDOW_X) {
                x = woodland_xwl_to_layout_x(&at, event->x);
            }
            if (event->mask & XCB_CONFIG_WINDOW_Y) {
                y = woodland_xwl_to_layout_y(&at, event->y);
            }
        } else {
            if (event->mask & XCB_CONFIG_WINDOW_X) {
                x = event->x;
            }
            if (event->mask & XCB_CONFIG_WINDOW_Y) {
                y = event->y;
            }
        }
        wlr_scene_node_set_position(&view->scene_tree->node, (int16_t)x, (int16_t)y);
        view->placed = true;
        woodland_view_update_outputs(view);
        native = woodland_xwl_view_region(view, &region);
    }

    /* 调整尺寸并 configure。woodland_view_request_size 收逻辑尺寸，
     * 而 X 客户端给的是 X 像素，先换算 */
    int width = (event->mask & XCB_CONFIG_WINDOW_WIDTH) ? event->width : view->xwayland_surface->width;
    int height = (event->mask & XCB_CONFIG_WINDOW_HEIGHT) ? event->height : view->xwayland_surface->height;
    if (native) {
        width = woodland_xwl_to_layout_width(&region, width);
        height = woodland_xwl_to_layout_height(&region, height);
    }
    woodland_view_request_size(view, width, height);
}
static void xwl_request_move(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xwl_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

static void xwl_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_maximize);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    woodland_view_set_maximized(view,
        xsurface->maximized_horz || xsurface->maximized_vert);
    (void)data;
}

static void xwl_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_fullscreen);
    woodland_view_set_fullscreen(view, view->xwayland_surface->fullscreen, NULL);
    (void)data;
}

static void xwl_request_minimize(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_minimize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_minimize);
    woodland_view_set_minimized(view, event->minimize);
}

static void xwl_request_activate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_activate);
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_focus_view(view, true);
    (void)data;
}

static void xwl_request_close(struct wl_listener *listener, void *data) {
    /* 客户端自己的 WM_DELETE_WINDOW 请求：由 wlroots 转发，无需处理 */
    (void)listener;
    (void)data;
}

static void xwl_set_title(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    woodland_view_set_title(view, view->xwayland_surface->title);
    (void)data;
}

static void xwl_set_class(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_class);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    /* X11 的 app_id 近似取 WM_CLASS 的 instance（对齐 labwc 的 app_id 语义） */
    const char *app_id = xsurface->instance ? xsurface->instance
                                            : xsurface->class;
    woodland_view_set_app_id(view, app_id);
    (void)data;
}

static void xwl_set_parent(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xwayland_surface *parent = view->xwayland_surface->parent;
    woodland_view_set_parent(view, parent ? parent->data : NULL);
    (void)data;
}

static void xwl_set_geometry(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_geometry);
    /* 位置由 WM 决定；尺寸变化后同步 SSD（客户端自己 resize 的情况） */
    xwl_rescale_view(view);
    if (view->mapped) {
        woodland_ssd_update(view);
    }
    (void)data;
}
static void xwl_set_override_redirect(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_override_redirect);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface || !xsurface->override_redirect) {
        return;
    }
    struct woodland_server *server = view->server;
    bool mapped = xsurface->surface && xsurface->surface->mapped;
    if (mapped) {
        woodland_view_unmap(view);
    }
    /*
     * surface 还活着，之后不会再有 dissociate 事件：手动摘掉挂在 wlr_surface
     * 上的 map/unmap/commit，否则 free(view) 之后它们仍会被回调（UAF）。
     */
    xwl_dissociate(&view->dissociate, NULL);
    xwl_destroy(&view->destroy, xsurface);
    xwl_new_unmanaged(server, xsurface);
}

static void xwl_set_window_type(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_window_type);
    (void)data;
    xwl_update_deco(view);
    woodland_view_update_taskbar_presence(view);
}

static void xwl_set_decorations(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_decorations);
    (void)data;
    xwl_update_deco(view);
}

static void xwl_request_skip_taskbar(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_skip_taskbar);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface) {
        return;
    }
    woodland_view_update_taskbar_presence(view);
}

static void xwl_focus_in(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, focus_in);
    (void)data;
    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface) {
        view->focused_before_map = true;
        return;
    }
    struct woodland_server *server = view->server;
    if (!server || !server->seat) {
        return;
    }

    /* 先走完整聚焦链记账（工作区 / 模态重定向 / activated / trace） */
    woodland_focus_view(view, false);
    if (server->focused_view != view) {
        return; /* 被拒绝：别的工作区、模态重定向拦截、不可聚焦 */
    }

    /* GLOBAL 输入模型客户端自己 SetInputFocus 上来，在此认账并更新键盘表面 */
    if (server->seat->keyboard_state.focused_surface != surface) {
        woodland_seat_focus_surface(server, surface);
        woodland_pointer_constraint_update(server);
    }
}

static void xwl_grab_focus(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, grab_focus);
    (void)data;
    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface) {
        return;
    }
    struct woodland_server *server = view->server;
    if (!server || !server->seat) {
        return;
    }

    woodland_focus_view(view, false);
    if (server->seat->keyboard_state.focused_surface != surface) {
        woodland_seat_focus_surface(server, surface);
        woodland_pointer_constraint_update(server);
    }
}

static void xwl_new_view(struct woodland_server *server,
                         struct wlr_xwayland_surface *xsurface) {
    struct woodland_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_view");
        return;
    }
    view->server = server;
    view->id = ++server->next_view_id;
    view->type = WOODLAND_VIEW_XWAYLAND;
    view->xwayland_surface = xsurface;
    view->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    if (!view->scene_tree) {
        wlr_log(WLR_ERROR, "failed to create scene tree for X11 view");
        free(view);
        return;
    }
    view->scene_tree->node.data = view;
    xsurface->data = view;

    woodland_listener_connect(&view->associate, &xsurface->events.associate,
                          xwl_associate, "xwayland.associate");
    woodland_listener_connect(&view->dissociate, &xsurface->events.dissociate,
                          xwl_dissociate, "xwayland.dissociate");
    woodland_listener_connect(&view->destroy, &xsurface->events.destroy,
                          xwl_destroy, "xwayland.destroy");

    woodland_listener_connect(&view->map_request, &xsurface->events.map_request,
                          xwl_map_request, "xwayland.map_request");
    woodland_listener_connect(&view->request_configure, &xsurface->events.request_configure,
                          xwl_request_configure, "xwayland.request_configure");
    woodland_listener_connect(&view->request_move, &xsurface->events.request_move,
                          xwl_request_move, "xwayland.request_move");
    woodland_listener_connect(&view->request_resize, &xsurface->events.request_resize,
                          xwl_request_resize, "xwayland.request_resize");
    woodland_listener_connect(&view->request_maximize, &xsurface->events.request_maximize,
                          xwl_request_maximize, "xwayland.request_maximize");
    woodland_listener_connect(&view->request_fullscreen, &xsurface->events.request_fullscreen,
                          xwl_request_fullscreen, "xwayland.request_fullscreen");
    woodland_listener_connect(&view->request_minimize, &xsurface->events.request_minimize,
                          xwl_request_minimize, "xwayland.request_minimize");
    woodland_listener_connect(&view->request_activate, &xsurface->events.request_activate,
                          xwl_request_activate, "xwayland.request_activate");
    woodland_listener_connect(&view->request_close, &xsurface->events.request_close,
                          xwl_request_close, "xwayland.request_close");
    woodland_listener_connect(&view->set_title, &xsurface->events.set_title,
                          xwl_set_title, "xwayland.set_title");
    woodland_listener_connect(&view->set_class, &xsurface->events.set_class,
                          xwl_set_class, "xwayland.set_class");
    woodland_listener_connect(&view->set_parent, &xsurface->events.set_parent,
                          xwl_set_parent, "xwayland.set_parent");
    woodland_listener_connect(&view->set_geometry, &xsurface->events.set_geometry,
                          xwl_set_geometry, "xwayland.set_geometry");
    woodland_listener_connect(&view->set_override_redirect, &xsurface->events.set_override_redirect,
                          xwl_set_override_redirect, "xwayland.set_override_redirect");
    woodland_listener_connect(&view->set_window_type, &xsurface->events.set_window_type,
                          xwl_set_window_type, "xwayland.set_window_type");
    woodland_listener_connect(&view->request_skip_taskbar, &xsurface->events.request_skip_taskbar,
                          xwl_request_skip_taskbar, "xwayland.request_skip_taskbar");
    woodland_listener_connect(&view->set_decorations, &xsurface->events.set_decorations,
                          xwl_set_decorations, "xwayland.set_decorations");
    woodland_listener_connect(&view->focus_in, &xsurface->events.focus_in,
                          xwl_focus_in, "xwayland.focus_in");
    woodland_listener_connect(&view->grab_focus, &xsurface->events.grab_focus,
                          xwl_grab_focus, "xwayland.grab_focus");

    woodland_view_set_title(view, xsurface->title);
    woodland_view_set_app_id(view, xsurface->instance
                                      ? xsurface->instance : xsurface->class);

    if (xsurface->surface) {
        xwl_associate(&view->associate, NULL);
    }
}


static void xwl_new_surface(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    if (xsurface->override_redirect) {
        xwl_new_unmanaged(server, xsurface);
        return;
    }

    xwl_new_view(server, xsurface);
}
struct woodland_xwl_xsettings {
    xcb_connection_t *conn;
    xcb_window_t window;
    xcb_atom_t selection_atom;
    xcb_atom_t settings_atom;
    xcb_atom_t manager_atom;
    uint32_t serial;
    struct woodland_xsettings_values last;
    int last_dpi;
};

static bool xwl_resolve_xsettings_atoms(const char *display_name, xcb_atom_t *sel,
                                        xcb_atom_t *settings, xcb_atom_t *mgr) {
    *sel = XCB_ATOM_NONE;
    *settings = XCB_ATOM_NONE;
    *mgr = XCB_ATOM_NONE;
    if (!display_name) {
        return false;
    }
    /* 用独立短连接同步取 atom，避免在 XWM 连接上等 reply 时把事件读空 */
    xcb_connection_t *c = xcb_connect(display_name, NULL);
    if (!c || xcb_connection_has_error(c) != 0) {
        if (c) {
            xcb_disconnect(c);
        }
        return false;
    }
    static const char *const names[3] = {"_XSETTINGS_S0", "_XSETTINGS_SETTINGS", "MANAGER"};
    xcb_intern_atom_cookie_t cookies[3];
    for (int i = 0; i < 3; i++) {
        cookies[i] = xcb_intern_atom(c, 0, (uint16_t)strlen(names[i]), names[i]);
    }
    xcb_atom_t resolved[3] = {XCB_ATOM_NONE, XCB_ATOM_NONE, XCB_ATOM_NONE};
    for (int i = 0; i < 3; i++) {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookies[i], NULL);
        if (reply) {
            resolved[i] = reply->atom;
            free(reply);
        }
    }
    xcb_disconnect(c);
    *sel = resolved[0];
    *settings = resolved[1];
    *mgr = resolved[2];
    return *sel != XCB_ATOM_NONE && *settings != XCB_ATOM_NONE && *mgr != XCB_ATOM_NONE;
}

static void xwl_publish_xsettings(struct woodland_server *server) {
    if (!server || !server->xwayland || !server->xwl_xsettings) {
        return;
    }
    if (server->config && !server->config->xwayland.xsettings) {
        return;
    }
    struct woodland_xwl_xsettings *xs = server->xwl_xsettings;
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!conn || conn != xs->conn || xcb_connection_has_error(conn) != 0) {
        return;
    }

    double scale = server->xwl_map.scale > 1.0 ? server->xwl_map.scale : 1.0;
    const char *cursor_theme = NULL;
    int cursor_size = 24;
    if (server->xcursor_manager) {
        cursor_theme = server->xcursor_manager->name;
        if (server->xcursor_manager->size > 0) {
            cursor_size = (int)server->xcursor_manager->size;
        }
    } else if (server->config) {
        cursor_theme = server->config->cursor.theme;
        if (server->config->cursor.size > 0) {
            cursor_size = server->config->cursor.size;
        }
    }

    bool flushed_needed = false;
    if (xs->window != XCB_NONE && xs->settings_atom != XCB_ATOM_NONE) {
        struct woodland_xsettings_values values;
        woodland_xsettings_for(scale, cursor_theme, cursor_size, &values);
        if (xs->serial == 0 || memcmp(&values, &xs->last, sizeof(values)) != 0) {
            uint8_t buf[512];
            uint32_t next_serial = xs->serial + 1;
            size_t len = woodland_xsettings_encode(next_serial, &values, buf, sizeof(buf));
            if (len > 0) {
                xs->serial = next_serial;
                xs->last = values;
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, xs->window,
                                    xs->settings_atom, xs->settings_atom, 8,
                                    (uint32_t)len, buf);
                flushed_needed = true;
            }
        }
    }

    int dpi = (int)lround(96.0 * scale);
    if (dpi != xs->last_dpi) {
        xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(conn));
        if (screens.rem != 0) {
            char res[64];
            int n = snprintf(res, sizeof(res), "Xft.dpi:\t%d\n", dpi);
            if (n > 0) {
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screens.data->root,
                                    XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8,
                                    (uint32_t)n, res);
                xs->last_dpi = dpi;
                flushed_needed = true;
            }
        }
    }

    if (flushed_needed) {
        xcb_flush(conn);
    }
}

static void xwl_start_xsettings(struct woodland_server *server) {
    if (!server || !server->xwayland) {
        return;
    }
    if (server->config && !server->config->xwayland.xsettings) {
        return;
    }
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!conn || xcb_connection_has_error(conn) != 0) {
        return;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(conn));
    if (screens.rem == 0) {
        return;
    }
    if (!server->xwl_xsettings) {
        server->xwl_xsettings = calloc(1, sizeof(*server->xwl_xsettings));
        if (!server->xwl_xsettings) {
            return;
        }
    } else {
        memset(server->xwl_xsettings, 0, sizeof(*server->xwl_xsettings));
    }
    struct woodland_xwl_xsettings *xs = server->xwl_xsettings;
    xs->conn = conn;
    xs->window = XCB_NONE;

    xcb_atom_t sel = XCB_ATOM_NONE, settings = XCB_ATOM_NONE, mgr = XCB_ATOM_NONE;
    if (!xwl_resolve_xsettings_atoms(server->xwayland->display_name, &sel, &settings, &mgr)) {
        wlr_log(WLR_INFO, "xwayland: XSETTINGS atoms unavailable, falling back to RESOURCE_MANAGER");
        xwl_publish_xsettings(server);
        return;
    }

    xcb_window_t root = screens.data->root;
    xs->selection_atom = sel;
    xs->settings_atom = settings;
    xs->manager_atom = mgr;
    xs->window = xcb_generate_id(conn);
    xcb_create_window(conn, 0, xs->window, root, -1, -1, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
    /* 先写属性再宣称 selection 所有权，收到 MANAGER 的客户端立即就能读到 */
    xwl_publish_xsettings(server);
    xcb_set_selection_owner(conn, xs->window, sel, XCB_CURRENT_TIME);

    xcb_client_message_event_t msg = {0};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = root;
    msg.type = mgr;
    msg.data.data32[0] = XCB_CURRENT_TIME;
    msg.data.data32[1] = sel;
    msg.data.data32[2] = xs->window;
    xcb_send_event(conn, 0, root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&msg);
    xcb_flush(conn);
}

void woodland_xwayland_view_update_scale_filter(struct woodland_view *view) {
    if (!view || view->type != WOODLAND_VIEW_XWAYLAND || !view->scene_surface) {
        return;
    }
    struct woodland_output *wo = woodland_output_for_view(view);
    float scale = (wo && wo->wlr_output && wo->wlr_output->scale > 0.0f) ? wo->wlr_output->scale : 1.0f;
    enum wlr_scale_filter_mode mode = xwl_get_scale_filter_mode(view->server, scale);
    if (view->scene_surface->buffer && view->scene_surface->buffer->filter_mode != mode) {
        wlr_scene_buffer_set_filter_mode(view->scene_surface->buffer, mode);
    }
    /* 视图换了输出（缩放不同）时显示尺寸要跟着变，并同步 SSD / 特效外框 */
    if (xwl_rescale_view(view) && view->mapped) {
        if (view->ssd) {
            woodland_ssd_update(view);
        } else {
            woodland_effect_update(view);
        }
    }
    /* 拖动结束或 wctl move 挪动窗口后，把最终布局坐标同步给 X11 侧，
     * 防止应用内右键菜单 / 下拉列表仍按旧位置弹出。 */
    if (view->mapped && view->server && !view->server->grabbed_view &&
        view->xwayland_surface && view->xwayland_surface->xwm && view->scene_tree) {
        int xx = view->scene_tree->node.x;
        int xy = view->scene_tree->node.y;
        struct woodland_xwl_region region;
        if (woodland_xwl_view_region(view, &region)) {
            xx = woodland_xwl_to_x(&region, xx);
            xy = woodland_xwl_to_y(&region, xy);
        }
        if (xx < INT16_MIN) {
            xx = INT16_MIN;
        } else if (xx > INT16_MAX) {
            xx = INT16_MAX;
        }
        if (xy < INT16_MIN) {
            xy = INT16_MIN;
        } else if (xy > INT16_MAX) {
            xy = INT16_MAX;
        }
        if (xx != view->xwayland_surface->x || xy != view->xwayland_surface->y) {
            wlr_xwayland_surface_configure(view->xwayland_surface,
                                           (int16_t)xx, (int16_t)xy,
                                           view->xwayland_surface->width,
                                           view->xwayland_surface->height);
            woodland_xwayland_flush(view->server);
        }
    }
}

/*
 * 输出布局 / 缩放变了：重算 X 屏幕并告诉 Xwayland，再把窗口摆回对应位置。
 * 逻辑尺寸在这一刻是真值（用户眼里的窗口大小没变），X 尺寸才该跟着变，
 * 所以受管理视图用当前显示尺寸（上一次缩放后的逻辑尺寸）重新 configure。
 */
static void xwl_remap_windows(struct woodland_server *server) {
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->type != WOODLAND_VIEW_XWAYLAND || !view->xwayland_surface ||
            !view->scene_surface || !view->scene_surface->buffer) {
            continue;
        }
        int width = view->scene_surface->buffer->dst_width;
        int height = view->scene_surface->buffer->dst_height;
        if (width <= 0 || height <= 0) {
            continue;
        }
        woodland_view_request_size(view, width, height);
    }
    woodland_xwayland_flush(server);
}

void woodland_xwayland_refresh_unmanaged_filters(struct woodland_server *server) {
    if (!server) {
        return;
    }
    if (server->xwl_outputs && woodland_xwl_outputs_update(server) && server->views.next) {
        xwl_remap_windows(server);
    }
    if (server->xwayland && wlr_xwayland_get_xwm_connection(server->xwayland)) {
        if (!server->config || server->config->xwayland.xsettings) {
            if (!server->xwl_xsettings) {
                xwl_start_xsettings(server);
            } else {
                xwl_publish_xsettings(server);
            }
        }
    }
    if (!server->xwayland_unmanaged.next) {
        return;
    }
    struct woodland_unmanaged *u;
    wl_list_for_each(u, &server->xwayland_unmanaged, link) {
        if (xwl_native(server)) {
            unmanaged_set_geometry(u);
        } else {
            unmanaged_update_filter_mode(u);
        }
    }
}

void woodland_xwayland_update_cursor(struct woodland_server *server) {
    if (!server || !server->xwayland || !server->xcursor_manager) {
        return;
    }
    struct wlr_xcursor *xcursor =
        wlr_xcursor_manager_get_xcursor(server->xcursor_manager, "default", 1);
    if (!xcursor) {
        xcursor = wlr_xcursor_manager_get_xcursor(server->xcursor_manager, "left_ptr", 1);
    }
    if (xcursor && xcursor->images && xcursor->images[0]) {
        struct wlr_xcursor_image *image = xcursor->images[0];
        struct wlr_buffer *cursor_buffer = wlr_xcursor_image_get_buffer(image);
        if (cursor_buffer) {
            wlr_xwayland_set_cursor(server->xwayland, cursor_buffer,
                                    image->hotspot_x, image->hotspot_y);
        }
    }
    xwl_publish_xsettings(server);
}

static void spawn_script_async(const char *path) {
    wlr_log(WLR_INFO, "running xinitrc script: %s", path);
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "failed to fork for xinitrc");
        return;
    }
    if (pid == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        signal(SIGPIPE, SIG_DFL);
        setsid();

        pid_t grand_pid = fork();
        if (grand_pid < 0) {
            _exit(1);
        }
        if (grand_pid == 0) {
            execl("/bin/sh", "sh", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

static void run_xinitrc(void) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[1024];

    /* 1. $XDG_CONFIG_HOME/woodland/xinitrc or ~/.config/woodland/xinitrc */
    if (config_home && config_home[0]) {
        snprintf(path, sizeof(path), "%s/woodland/xinitrc", config_home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    } else if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/woodland/xinitrc", home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    }

    /* 2. $XDG_CONFIG_DIRS/woodland/xinitrc or /etc/xdg/woodland/xinitrc */
    const char *config_dirs = getenv("XDG_CONFIG_DIRS");
    if (!config_dirs || !config_dirs[0]) {
        config_dirs = "/etc/xdg";
    }
    char dirs_copy[1024];
    strncpy(dirs_copy, config_dirs, sizeof(dirs_copy) - 1);
    dirs_copy[sizeof(dirs_copy) - 1] = '\0';
    char *saveptr = NULL;
    char *dir = strtok_r(dirs_copy, ":", &saveptr);
    while (dir) {
        snprintf(path, sizeof(path), "%s/woodland/xinitrc", dir);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    /* 3. Fallback: labwc xinitrc */
    if (config_home && config_home[0]) {
        snprintf(path, sizeof(path), "%s/labwc/xinitrc", config_home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    } else if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/labwc/xinitrc", home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    }
}

/* ==================== 模块入口 ==================== */
static void xwl_ready(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, xwayland_ready);
    wlr_log(WLR_INFO, "xwayland ready on DISPLAY=%s",
            server->xwayland ? server->xwayland->display_name : "(null)");
    /* 先于 xinitrc 发布 XSETTINGS / Xft.dpi，让用户 xinitrc 中的 xrdb 有机会覆盖 */
    xwl_start_xsettings(server);
    run_xinitrc();
    woodland_xwayland_update_cursor(server);
    (void)data;
}
static bool xwl_global_filter(const struct wl_client *client,
                              const struct wl_global *global,
                              void *data) {
    struct woodland_server *server = data;
    struct wl_client *xwayland_client = (server && server->xwayland && server->xwayland->server)
        ? server->xwayland->server->client
        : NULL;
    /* native_resolution：Xwayland 只看专用 xdg-output，其余客户端只看 wlroots 的 */
    if (!woodland_xwl_outputs_advertise(server, client, global, xwayland_client)) {
        return false;
    }
    const struct wl_interface *iface = wl_global_get_interface(global);
    if (!iface || !iface->name || strcmp(iface->name, "xwayland_shell_v1") != 0) {
        return true;
    }
    return client == xwayland_client;
}


bool woodland_xwayland_init(struct woodland_server *server) {
    wl_list_init(&server->xwayland_unmanaged);

    server->xwayland = wlr_xwayland_create(server->display, server->compositor,
                                           true);
    if (!server->xwayland) {
        wlr_log(WLR_ERROR, "failed to create xwayland");
        return false;
    }
    if (!woodland_global_filter_add(server->display, "xwayland_shell",
                                    xwl_global_filter, server)) {
        wlr_log(WLR_ERROR, "failed to register xwayland global filter");
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
        return false;
    }
    if (server->seat) {
        wlr_xwayland_set_seat(server->xwayland, server->seat);
    }

    /* 原生分辨率：无条件建专用 xdg-output global（server->config 在 server_init
     * 末尾才加载，这里若读不到配置先按默认 true 建好；稍后 cfg_apply 再同步开关）。 */
    memset(&server->xwl_map, 0, sizeof(server->xwl_map));
    server->xwl_map.scale = 1.0;
    server->xwl_map.enabled = !server->config || server->config->xwayland.native_resolution;
    server->xwl_outputs = woodland_xwl_outputs_create(server);
    if (!server->xwl_outputs) {
        wlr_log(WLR_ERROR, "xwayland: native_resolution unavailable, using logical size");
        server->xwl_map.enabled = false;
    } else if (server->xwl_map.enabled) {
        wlr_log(WLR_INFO, "xwayland: native_resolution enabled");
    }

    server->xwayland_unmanaged_tree =
        wlr_scene_tree_create(&server->scene->tree);

    woodland_listener_connect(&server->new_xwayland_surface, &server->xwayland->events.new_surface,
                          xwl_new_surface, "xwayland.new_surface");

    woodland_listener_connect(&server->xwayland_ready, &server->xwayland->events.ready,
                          xwl_ready, "xwayland.ready");
    server->xwayland_cursor_hook = woodland_xwayland_update_cursor;
    server->xwayland_scale_filter_hook = woodland_xwayland_view_update_scale_filter;
    server->xwayland_refresh_hook = woodland_xwayland_refresh_unmanaged_filters;
    server->xwayland_restack_hook = woodland_xwayland_restack_view;
    server->xwayland_flush_hook = woodland_xwayland_flush;
    server->xwindow_move_hook = woodland_xwayland_move_by_wid;

    wlr_log(WLR_INFO, "xwayland initialised, DISPLAY=%s",
            server->xwayland->display_name);
    return true;
}

void woodland_xwayland_finish(struct woodland_server *server) {
    if (!server->xwayland) {
        return;
    }
    woodland_global_filter_remove(server->display, "xwayland_shell");
    woodland_xwl_outputs_destroy(server->xwl_outputs);
    server->xwl_outputs = NULL;
    free(server->xwl_xsettings);
    server->xwl_xsettings = NULL;
    server->xwl_map.enabled = false;
    wl_list_remove(&server->new_xwayland_surface.link);
    wl_list_remove(&server->xwayland_ready.link);
    server->xwayland_cursor_hook = NULL;
    server->xwayland_scale_filter_hook = NULL;
    server->xwayland_refresh_hook = NULL;
    server->xwayland_restack_hook = NULL;
    server->xwayland_flush_hook = NULL;
    server->xwindow_move_hook = NULL;
    wlr_xwayland_destroy(server->xwayland);
    server->xwayland = NULL;
}
void woodland_xwayland_flush(struct woodland_server *server) {
    if (!server || !server->xwayland) {
        return;
    }
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (conn) {
        xcb_flush(conn);
    }
}

/*
 * treeland-xwindow-control-v1 的摆位入口（挂在 server->xwindow_move_hook 上）。
 *
 * X11 窗口的坐标有两份真相：wlr_scene 节点（画在哪）和 X server 里的窗口
 * 几何（X11 客户端以为自己在哪）。只改前者，下一次 set_geometry 会把节点
 * 拉回旧坐标；只改后者，画面要等 X11 回 ConfigureNotify 才动。两边一起写，
 * 再 flush 一次 xcb 让 configure 立刻出栈。
 *
 * 受管理视图的内容原点就是 scene_tree 原点（SSD 画在负坐标上）；协议说的
 * "窗口左上角"取的是 X11 窗口自身的几何原点，与 X11 侧的 x/y 一致。
 */
bool woodland_xwayland_move_by_wid(struct woodland_server *server, uint32_t wid,
                                   int x, int y, uint32_t *view_id,
                                   bool *unmanaged)
{
    if (view_id) {
        *view_id = 0;
    }
    if (unmanaged) {
        *unmanaged = false;
    }
    if (!server) {
        return false;
    }
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
        /* 协议族调用前已验过；这里是纵深防御，别让别的调用方截断成错坐标 */
        wlr_log(WLR_DEBUG, "xwindow-control: target (%d,%d) outside X11 int16 range", x, y);
        return false;
    }

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (view->type != WOODLAND_VIEW_XWAYLAND || !xsurface ||
            xsurface->window_id != (xcb_window_t)wid) {
            continue;
        }
        if (!view->mapped || !view->scene_tree) {
            wlr_log(WLR_DEBUG, "xwindow-control: wid=%u exists but is not mapped", wid);
            return false;
        }
        /* 协议只改位置：宽高用 X11 窗口自己的几何，别用 content_box——
         * 带 SSD / transform 时两者不等，会附带触发一次 resize。 */
        int xx = x, xy = y;
        if (xwl_native(server)) {
            struct woodland_output *wo = woodland_output_at(server, x, y);
            struct woodland_xwl_region region;
            woodland_xwl_map_region(&server->xwl_map, wo ? wo->wlr_output : NULL, x, y, &region);
            xx = woodland_xwl_to_x(&region, x);
            xy = woodland_xwl_to_y(&region, y);
        }
        if (xx < INT16_MIN || xx > INT16_MAX || xy < INT16_MIN || xy > INT16_MAX) {
            wlr_log(WLR_DEBUG, "xwindow-control: scaled target (%d,%d) outside X11 int16 range",
                    xx, xy);
            return false;
        }
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
        if (xsurface->xwm) {
            wlr_xwayland_surface_configure(xsurface, (int16_t)xx, (int16_t)xy,
                                           xsurface->width, xsurface->height);
        } else {
            xsurface->x = (int16_t)xx;
            xsurface->y = (int16_t)xy;
        }
        view->placed = true;
        woodland_view_update_outputs(view);
        woodland_xwayland_flush(server);
        if (view_id) {
            *view_id = view->id;
        }
        return true;
    }

    struct woodland_unmanaged *u;
    wl_list_for_each(u, &server->xwayland_unmanaged, link) {
        struct wlr_xwayland_surface *xsurface = u->xsurface;
        if (!xsurface || xsurface->window_id != (xcb_window_t)wid) {
            continue;
        }
        if (!u->scene_tree || !xsurface->surface || !xsurface->surface->mapped) {
            wlr_log(WLR_DEBUG, "xwindow-control: wid=%u exists but is not mapped", wid);
            return false;
        }
        int xx = x, xy = y;
        struct woodland_xwl_region region;
        bool native = xwl_native(server);
        if (native) {
            struct woodland_output *wo = woodland_output_at(server, x, y);
            woodland_xwl_map_region(&server->xwl_map, wo ? wo->wlr_output : NULL, x, y, &region);
            xx = woodland_xwl_to_x(&region, x);
            xy = woodland_xwl_to_y(&region, y);
        }
        if (xx < INT16_MIN || xx > INT16_MAX || xy < INT16_MIN || xy > INT16_MAX) {
            wlr_log(WLR_DEBUG, "xwindow-control: scaled target (%d,%d) outside X11 int16 range",
                    xx, xy);
            return false;
        }
        wlr_scene_node_set_position(&u->scene_tree->node, x, y);
        if (native) {
            xwl_rescale_surface(u->scene_surface, &region);
        }
        if (xsurface->xwm) {
            wlr_xwayland_surface_configure(xsurface, (int16_t)xx, (int16_t)xy,
                                           xsurface->width, xsurface->height);
        } else {
            xsurface->x = (int16_t)xx;
            xsurface->y = (int16_t)xy;
        }
        woodland_xwayland_flush(server);
        if (unmanaged) {
            *unmanaged = true;
        }
        return true;
    }
    return false;
}

void woodland_xwayland_restack_view(struct woodland_view *view) {
    if (!view || view->type != WOODLAND_VIEW_XWAYLAND || !view->xwayland_surface) {
        return;
    }
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (xsurface->override_redirect || !xsurface->xwm) {
        return;
    }
    wlr_xwayland_surface_restack(xsurface, NULL, XCB_STACK_MODE_ABOVE);
}
