// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "capture.h"
#include "layer.h"
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"

/* ==================== 双后端访问器 ==================== */

struct wlr_surface *woodland_view_surface(const struct woodland_view *view) {
    if (!view) {
        return NULL;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        return xsurface ? xsurface->surface : NULL;
    }
#endif
    return view->xdg_toplevel ? view->xdg_toplevel->base->surface : NULL;
}

void woodland_view_content_box(const struct woodland_view *view,
                               struct wlr_box *box) {
    *box = (struct wlr_box){0};
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        /* X11 窗口的 surface 就是窗口本身，几何原点恒为 0,0 */
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (xsurface) {
            box->width = xsurface->width;
            box->height = xsurface->height;
        }
        return;
    }
#endif
    *box = view->xdg_toplevel->base->geometry;
}

void woodland_view_request_size(struct woodland_view *view, int width,
                                int height) {
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (!xsurface) {
            return;
        }
        /* X11 没有"尺寸由客户端决定"的语义，必须给出具体值。
         * 位置用布局坐标直接当 root 坐标发出去：单输出（布局原点 0,0）下等价，
         * 多输出时 X11 客户端看到的是同一坐标系，暂不做输出内部偏移换算。 */
        if (width < 1) {
            width = 1;
        }
        if (height < 1) {
            height = 1;
        }
        wlr_xwayland_surface_configure(xsurface,
            (int16_t)view->scene_tree->node.x,
            (int16_t)view->scene_tree->node.y,
            (uint16_t)width, (uint16_t)height);
        return;
    }
#endif
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
}

void woodland_view_set_activated(struct woodland_view *view, bool activated) {
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_activate(view->xwayland_surface, activated);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
}

void woodland_view_close(struct woodland_view *view) {
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_close(view->xwayland_surface);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

struct woodland_view *woodland_view_from_surface(struct wlr_surface *surface) {
    if (!surface) {
        return NULL;
    }
    struct wlr_xdg_toplevel *toplevel =
        wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (toplevel) {
        struct wlr_scene_tree *tree = toplevel->base->data;
        return tree ? tree->node.data : NULL;
    }
#if WOODLAND_HAVE_XWAYLAND
    struct wlr_xwayland_surface *xsurface =
        wlr_xwayland_surface_try_from_wlr_surface(surface);
    return xsurface ? xsurface->data : NULL;
#else
    return NULL;
#endif
}

/* ==================== view 聚焦与命中 ==================== */

struct woodland_view *woodland_view_at(struct woodland_server *server,
                                       double lx, double ly,
                                       struct wlr_surface **surface,
                                       double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree->node.data;
}

/* ==================== toplevel 视图事件 ==================== */

/*
 * 服务端最终采用的装饰模式。
 *
 * 只看策略位 deco_csd，**不能**看 view->ssd 是否存在：SSD 对象在 map 时才创建，
 * 而 zxdg 协商发生在 initial commit（早于 map），那时 ssd 必为 NULL，
 * 拿它判断会给"请求 server-side"的客户端回一个 client-side configure，
 * 客户端于是自己再画一条标题栏 —— 叠加我们的 SSD 就是双标题栏（foot 即此例）。
 */
static enum wlr_xdg_toplevel_decoration_v1_mode
view_decoration_mode(const struct woodland_view *view) {
    return view->deco_csd
        ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

void woodland_view_map(struct woodland_view *view) {
    if (view->mapped) {
        return;
    }
    struct woodland_server *server = view->server;
    view->mapped = true;
    /* 有已映射父视图则 inherit 其工作区，否则进当前活动工作区 */
    if (view->parent && view->parent->mapped) {
        view->workspace = view->parent->workspace;
    } else {
        view->workspace = server->active_workspace;
    }
    wlr_scene_node_reparent(&view->scene_tree->node,
                            view->workspace->tree);
    wl_list_insert(&server->views, &view->link);
    /* 首次 map 时创建 SSD（未 map 前不显示空装饰，同 labwc handle_map） */
    if (!view->ssd && !view->deco_csd) {
        woodland_ssd_create(view);
    }
    /* 只在首次 map 放置；remap 保留用户摆好的位置 */
    if (!view->placed) {
        view->placed = true;
        woodland_view_place_initial(view);
    }
    woodland_focus_view(view, true);
}

void woodland_view_unmap(struct woodland_view *view) {
    if (view == view->server->grabbed_view) {
        woodland_cursor_reset_mode(view->server);
    }
    /* SSD 即将销毁：清掉指向它的指针状态，避免悬空 */
    if (view->server->hover_view == view) {
        view->server->hover_view = NULL;
        view->server->hover_part = WOODLAND_SSD_NONE;
    }
    if (view->server->pressed_view == view) {
        view->server->pressed_view = NULL;
        view->server->pressed_part = WOODLAND_SSD_NONE;
    }
    /* 焦点：先按正常卸载路径复位（改 activated / SSD 激活色 / foreign-toplevel，
     * 并清掉键盘焦点），再摘链表。只把 focused_view 置空会留下"句柄还是
     * 激活态"的残影——没有键盘设备时 focus_current() 反查不到旧视图，
     * 回退时就没人负责把它变暗。摘链表放在后面，回退会自然跳过它。 */
    bool was_focused = (view->server->focused_view == view);
    if (was_focused) {
        woodland_focus_none(view->server);
        /* 没有 seat 时 focus_none 会提前返回，指针得自己清 */
        view->server->focused_view = NULL;
    }
    /* 只在确实在链表里时摘除：X11 窗口可能没 map 过就 unmap */
    if (view->mapped) {
        wl_list_remove(&view->link);
        view->mapped = false;
    }
    /* unmap 时销毁 SSD（避免残留空装饰）；remap 时按 deco_csd 重建 */
    if (view->ssd) {
        woodland_ssd_destroy(view);
    }
    /* 焦点回退：此时视图已不在链表里，回退不会再选中它 */
    if (was_focused) {
        woodland_focus_fallback(view->server);
    }
}

void woodland_view_destroy(struct woodland_view *view) {
    /* 销毁中的视图不能作为回退目标，也不能被 focus_current 反查到：
     * 先清焦点（只改状态，不挑新目标），等视图摘出链表后再回退 */
    bool was_focused = (view->server->focused_view == view);
    if (was_focused) {
        woodland_focus_none(view->server);
        /* 没有 seat 时 focus_none 会提前返回，指针得自己清 */
        view->server->focused_view = NULL;
    }
    if (view->mapped) {
        wl_list_remove(&view->link);
        view->mapped = false;
    }
    /* foreign-toplevel handle 与 listener */
    if (view->toplevel_handle) {
        wl_list_remove(&view->ft_request_activate.link);
        wl_list_remove(&view->ft_request_maximize.link);
        wl_list_remove(&view->ft_request_fullscreen.link);
        wl_list_remove(&view->ft_request_minimize.link);
        wl_list_remove(&view->ft_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(view->toplevel_handle);
        view->toplevel_handle = NULL;
    }
    /* ext-foreign-toplevel-list 句柄 */
    if (view->ext_toplevel_handle) {
        wl_list_remove(&view->ext_handle_destroy.link);
        wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_toplevel_handle);
        view->ext_toplevel_handle = NULL;
    }
    /* 按窗口捕获源：源随 scene 节点释放，这里只摘监听 */
    woodland_capture_view_finish(view);
    /* SSD 在 scene 树销毁时级联释放，这里只清指针 */
    if (view->ssd) {
        woodland_ssd_destroy(view);
    }
    if (view->server->hover_view == view) {
        view->server->hover_view = NULL;
        view->server->hover_part = WOODLAND_SSD_NONE;
    }
    if (view->server->pressed_view == view) {
        view->server->pressed_view = NULL;
        view->server->pressed_part = WOODLAND_SSD_NONE;
    }
    if (was_focused) {
        woodland_focus_fallback(view->server);
    }
    free(view->title);
    free(view->app_id);
    free(view);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map);
    woodland_view_map(view);
    (void)data;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    woodland_view_unmap(view);
    (void)data;
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, commit);
    struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
    if (xdg->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        /* initial commit：此时 surface 已 initialized，可以 set_mode 完成 zxdg 协商 */
        if (view->deco) {
            wlr_xdg_toplevel_decoration_v1_set_mode(view->deco,
                view_decoration_mode(view));
        }
    }
    /* 内容尺寸/状态变化后同步 SSD 几何（标题栏/边框/按钮） */
    woodland_ssd_update(view);
    (void)data;
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);
    wl_list_remove(&view->set_parent.link);
    /* zxdg_decoration 监听器（协商过才注册） */
    if (view->deco_registered) {
        wl_list_remove(&view->deco_destroy.link);
        wl_list_remove(&view->deco_request_mode.link);
        view->deco_registered = false;
    }
    woodland_view_destroy(view);
    (void)data;
}

/* ==================== 交互式 move/resize ==================== */

static void view_unsnap_for_drag(struct woodland_view *view);

void woodland_view_begin_interactive(struct woodland_view *view,
                                     enum woodland_cursor_mode mode,
                                     uint32_t edges) {
    struct woodland_server *server = view->server;
    server->grabbed_view = view;
    server->cursor_mode = mode;
    server->drag_start_x = server->cursor->x;
    server->drag_start_y = server->cursor->y;

    if (mode == WOODLAND_CURSOR_MOVE) {
        /* 拖动最大化/贴边的窗口：先恢复原尺寸再跟随光标（labwc 同款），
         * 否则窗口只能被整体平移，用户没法把它"拉下来" */
        view_unsnap_for_drag(view);
        server->grab_x = server->cursor->x - view->scene_tree->node.x;
        server->grab_y = server->cursor->y - view->scene_tree->node.y;
    } else {
        /* 手动缩放即放弃贴边状态（尺寸由用户接管） */
        view->snapped_edges = 0;
        view->pending_snap_edges = 0;
        struct wlr_box geo_box;
        woodland_view_content_box(view, &geo_box);
        double border_x = (view->scene_tree->node.x + geo_box.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (view->scene_tree->node.y + geo_box.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += view->scene_tree->node.x;
        server->grab_geobox.y += view->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

/* ==================== 窗口状态处理 ==================== */

static void view_save_geometry(struct woodland_view *view) {
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    view->saved_geometry.x = view->scene_tree->node.x;
    view->saved_geometry.y = view->scene_tree->node.y;
    view->saved_geometry.width = content.width;
    view->saved_geometry.height = content.height;
}

static void view_apply_geometry(struct woodland_view *view,
                                const struct wlr_box *box) {
    /* 先落位置再改尺寸：X11 的 configure 会把当前位置一并发给客户端 */
    wlr_scene_node_set_position(&view->scene_tree->node, box->x, box->y);
    woodland_view_request_size(view, box->width, box->height);
}

/* ---- 状态标志的协议侧同步（xdg 走 configure，X11 走 _NET_WM_STATE） ---- */

static void view_apply_maximized_state(struct woodland_view *view,
                                       bool maximized) {
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface,
                                               maximized, maximized);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
}

static void view_apply_fullscreen_state(struct woodland_view *view,
                                        bool fullscreen) {
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_fullscreen(view->xwayland_surface,
                                                fullscreen);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
}

/* X11 的 configure 已由 woodland_view_request_size 直接发出，无需再排程 */
static void view_schedule_configure(struct woodland_view *view) {
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        return;
    }
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

void woodland_view_set_maximized(struct woodland_view *view, bool maximized) {
    if (maximized == view->maximized) {
        return;
    }
    struct woodland_server *server = view->server;
    if (maximized) {
        /* 先退出 fullscreen（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->fullscreen) {
            view->fullscreen = false;
            view_apply_geometry(view, &view->saved_geometry);
            view_apply_fullscreen_state(view, false);
        }
        view_save_geometry(view);
        view->maximized = true;
        /* 最大化取"这个窗口所在输出"的工作区，多输出下才不会被拽到主屏 */
        struct wlr_box box = woodland_view_maximized_geometry_on(
            server, woodland_output_for_view(view));
        /* 最大化保留标题栏，内容让出 titlebar 高度 */
        woodland_view_apply_outer_geometry(view, &box);
        view_apply_maximized_state(view, true);
    } else {
        view->maximized = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        view_apply_maximized_state(view, false);
    }
    view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
    }
}

void woodland_view_set_fullscreen(struct woodland_view *view, bool fullscreen,
                                  struct wlr_output *output) {
    if (fullscreen == view->fullscreen) {
        return;
    }
    struct woodland_server *server = view->server;
    if (fullscreen) {
        /* 先退出 maximize（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->maximized) {
            view->maximized = false;
            view_apply_geometry(view, &view->saved_geometry);
            view_apply_maximized_state(view, false);
        }
        view_save_geometry(view);
        view->fullscreen = true;
        if (!output) {
            /* 无指定输出：落到窗口当前所在的输出（多输出下不能固定主屏） */
            struct woodland_output *wo = woodland_output_for_view(view);
            if (!wo) {
                wo = woodland_output_first(server);
            }
            output = wo ? wo->wlr_output : NULL;
        }
        struct wlr_box box = woodland_view_fullscreen_geometry(server, output);
        woodland_view_apply_outer_geometry(view, &box);
        view_apply_fullscreen_state(view, true);
    } else {
        view->fullscreen = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        view_apply_fullscreen_state(view, false);
    }
    view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
    }
}

void woodland_view_set_minimized(struct woodland_view *view, bool minimized) {
    if (minimized == view->minimized) {
        return;
    }
    view->minimized = minimized;
    wlr_scene_node_set_enabled(&view->scene_tree->node, !minimized);
    if (minimized) {
        /* 最小化掉焦点视图：交给焦点回退（挑同工作区下一个窗口），
         * 否则键盘焦点会悬空，用户必须先点一下才能继续输入 */
        struct woodland_view *focused = woodland_focus_current(view->server);
        if (focused == view) {
            woodland_focus_fallback(view->server);
        }
    }
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_minimized(
            view->toplevel_handle, minimized);
    }
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_maximize);
    woodland_view_set_maximized(view, view->xdg_toplevel->requested.maximized);
    (void)data;
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_fullscreen);
    woodland_view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen,
                                 view->xdg_toplevel->requested.fullscreen_output);
    (void)data;
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_minimize);
    woodland_view_set_minimized(view, view->xdg_toplevel->requested.minimized);
    (void)data;
}

/* 客户端/合成器侧销毁句柄时清指针，避免视图销毁时二次释放 */
static void ext_toplevel_handle_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, ext_handle_destroy);
    wl_list_remove(&view->ext_handle_destroy.link);
    view->ext_toplevel_handle = NULL;
    (void)data;
}

/* ext-foreign-toplevel-list 的句柄状态：只有 title/app_id 两项 */
static void ext_toplevel_sync_state(struct woodland_view *view) {
    if (!view->ext_toplevel_handle) {
        return;
    }
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = view->title,
        .app_id = view->app_id,
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(
        view->ext_toplevel_handle, &state);
}

void woodland_view_set_title(struct woodland_view *view, const char *title) {
    char *copy = title ? strdup(title) : NULL;
    if (title && !copy) {
        return;
    }
    free(view->title);
    view->title = copy;
    /* wlroots 的 set_title 不接受 NULL（内部 strdup），空标题就不下发 */
    if (view->toplevel_handle && view->title) {
        wlr_foreign_toplevel_handle_v1_set_title(view->toplevel_handle,
                                                 view->title);
    }
    ext_toplevel_sync_state(view);
    /* 标题变化时刷新 SSD 标题文本 */
    if (view->ssd) {
        woodland_ssd_update(view);
    }
}

void woodland_view_set_app_id(struct woodland_view *view, const char *app_id) {
    char *copy = app_id ? strdup(app_id) : NULL;
    if (app_id && !copy) {
        return;
    }
    free(view->app_id);
    view->app_id = copy;
    if (view->toplevel_handle && view->app_id) {
        wlr_foreign_toplevel_handle_v1_set_app_id(view->toplevel_handle,
                                                  view->app_id);
    }
    ext_toplevel_sync_state(view);
}

void woodland_view_set_parent(struct woodland_view *view,
                              struct woodland_view *parent) {
    view->parent = parent;
    /* 同步 foreign-toplevel parent */
    if (view->toplevel_handle) {
        struct wlr_foreign_toplevel_handle_v1 *parent_handle =
            (parent && parent->toplevel_handle)
                ? parent->toplevel_handle : NULL;
        wlr_foreign_toplevel_handle_v1_set_parent(
            view->toplevel_handle, parent_handle);
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    woodland_view_set_title(view, view->xdg_toplevel->title);
    (void)data;
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_app_id);
    woodland_view_set_app_id(view, view->xdg_toplevel->app_id);
    (void)data;
}

static void xdg_toplevel_set_parent(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xdg_toplevel *parent = view->xdg_toplevel->parent;
    struct woodland_view *parent_view = NULL;
    if (parent && parent->base->data) {
        struct wlr_scene_tree *parent_tree = parent->base->data;
        parent_view = parent_tree->node.data;
    }
    woodland_view_set_parent(view, parent_view);
    (void)data;
}

/* ==================== foreign-toplevel 请求 ==================== */

static void ft_request_activate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_activate);
    /* 若最小化则先恢复 */
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_focus_view(view, true);
    (void)data;
}

static void ft_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    woodland_view_set_maximized(view, event->maximized);
}

static void ft_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    woodland_view_set_fullscreen(view, event->fullscreen, event->output);
}

static void ft_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    woodland_view_set_minimized(view, event->minimized);
}

static void ft_request_close(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_close);
    woodland_view_close(view);
    (void)data;
}

/* ==================== 交互式 motion 处理 ==================== */

void woodland_cursor_reset_mode(struct woodland_server *server) {
    server->cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
}

/* ==================== SSD 按钮 hover/pressed ==================== */

/*
 * 更新悬停的按钮。
 *
 * 指针每帧只会悬停在一个 SSD 部件上，所以这里维护"唯一的 hover 目标"：
 * 换了视图先清旧的，避免两个窗口同时亮着。
 */
static void ssd_update_hover(struct woodland_server *server,
                             struct woodland_view *view,
                             enum woodland_ssd_part part) {
    if (server->hover_view == view && server->hover_part == part) {
        return;
    }
    if (server->hover_view && server->hover_view != view) {
        woodland_ssd_set_hover(server->hover_view, WOODLAND_SSD_NONE);
    }
    server->hover_view = view;
    server->hover_part = part;
    if (view) {
        woodland_ssd_set_hover(view, part);
    }
}

/* 清掉按下态（不执行动作） */
static void ssd_clear_pressed(struct woodland_server *server) {
    if (server->pressed_view) {
        woodland_ssd_set_pressed(server->pressed_view, WOODLAND_SSD_NONE);
    }
    server->pressed_view = NULL;
    server->pressed_part = WOODLAND_SSD_NONE;
}

/* 吸附候选边上限：每个视图贡献 2 条边（左右或上下） */
#define WOODLAND_SNAP_MAX_EDGES 64

/*
 * 收集吸附候选线（纯数据，交给 woodland_snap_axis 决策）。
 *
 * x_edges=true 收集竖直边（外框左右边），此时只考虑纵向范围与
 * [span_start, span_start+span_len) 有交叠的视图——否则隔着半屏的窗口
 * 也会把我们的窗口吸过去。
 */
static int collect_snap_edges(struct woodland_server *server,
                              struct woodland_view *exclude, bool x_edges,
                              int span_start, int span_len,
                              int *edges, int max_edges) {
    int n = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link) {
        if (it == exclude || !it->mapped || it->minimized || it->fullscreen
            || it->workspace != exclude->workspace) {
            continue;
        }
        struct woodland_ssd_margins m;
        woodland_ssd_get_margins(it, &m);
        struct wlr_box content;
        woodland_view_content_box(it, &content);
        struct wlr_box box = {
            .x = it->scene_tree->node.x - m.left,
            .y = it->scene_tree->node.y - m.top,
            .width = content.width + m.left + m.right,
            .height = content.height + m.top + m.bottom,
        };
        int other_start = x_edges ? box.y : box.x;
        int other_len = x_edges ? box.height : box.width;
        if (other_start + other_len <= span_start
            || span_start + span_len <= other_start) {
            continue;
        }
        if (n + 2 > max_edges) {
            break;
        }
        edges[n++] = x_edges ? box.x : box.y;
        edges[n++] = x_edges ? box.x + box.width : box.y + box.height;
    }
    return n;
}

/* ==================== 贴边铺满（snap to edge） ==================== */

/*
 * 拖动已最大化/已贴边的窗口：先恢复原尺寸，再把标题栏摆到光标下方。
 *
 * 直接把窗口平移到光标处是错的——最大化窗口一旦被拖，用户期望它缩回
 * 原来的大小并"挂在"光标上（labwc / 多数桌面的行为）。
 */
static void view_unsnap_for_drag(struct woodland_view *view) {
    if (!view->maximized && view->snapped_edges == 0) {
        return;
    }
    struct woodland_server *server = view->server;

    struct wlr_box restore;   /* 内容框 */
    if (view->maximized) {
        restore = view->saved_geometry;
        /* 直接落标志 + 通知客户端，位置稍后统一按光标摆 */
        view->maximized = false;
        view_apply_maximized_state(view, false);
        if (view->toplevel_handle) {
            wlr_foreign_toplevel_handle_v1_set_maximized(
                view->toplevel_handle, false);
        }
    } else {
        struct woodland_ssd_margins m;
        woodland_ssd_get_margins(view, &m);
        struct wlr_box outer = view->snap_saved_geometry;
        restore = (struct wlr_box){
            .x = outer.x + m.left,
            .y = outer.y + m.top,
            .width = outer.width - m.left - m.right,
            .height = outer.height - m.top - m.bottom,
        };
    }
    view->snapped_edges = 0;
    view->pending_snap_edges = 0;
    woodland_view_set_tiled(view, 0);

    struct woodland_ssd_margins m;
    woodland_ssd_get_margins(view, &m);
    struct wlr_box target = {
        .x = (int)(server->cursor->x - restore.width / 2.0),
        .y = (int)(server->cursor->y - m.top / 2.0),
        .width = restore.width,
        .height = restore.height,
    };
    view_apply_geometry(view, &target);
    woodland_view_constrain(view);
    view_schedule_configure(view);
}

/* 释放鼠标时把"拖到边上的窗口"铺满（左/右半屏、上边最大化、四角 1/4） */
static void view_apply_snap(struct woodland_view *view) {
    uint32_t edges = view->pending_snap_edges;
    view->pending_snap_edges = 0;
    if (woodland_view_snap_to(view, edges)) {
        view_schedule_configure(view);
    }
}

/*
 * 拖动移动：外框跟随光标，靠近工作区边缘或其它窗口边缘时吸附，
 * 并记录"松手即铺满"的边。
 *
 * 先按 x 吸附（用当前的纵向跨度判断邻窗是否并排），再按吸附后的 x
 * 算 y，避免两次吸附互相打脸。吸附基准是**窗口所在输出**的工作区，
 * 多输出下光标在副屏时不会被主屏的边吸走。
 */
static void process_cursor_move(struct woodland_server *server) {
    struct woodland_view *view = server->grabbed_view;
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);
    struct wlr_box geo;
    woodland_view_content_box(view, &geo);

    /* grab_x/y 是光标相对内容原点的偏移，换算成外框原点 */
    int x = (int)(server->cursor->x - server->grab_x) - margins.left;
    int y = (int)(server->cursor->y - server->grab_y) - margins.top;
    int outer_w = geo.width + margins.left + margins.right;
    int outer_h = geo.height + margins.top + margins.bottom;

    view->pending_snap_edges = 0;

    if (!view->maximized && !view->fullscreen) {
        struct woodland_output *wo = woodland_output_at(server,
            server->cursor->x, server->cursor->y);
        if (!wo) {
            wo = woodland_output_for_view(view);
        }
        struct wlr_box area;
        woodland_output_workarea(wo, &area);

        int edges[WOODLAND_SNAP_MAX_EDGES];
        int n = collect_snap_edges(server, view, true, y, outer_h,
                                   edges, WOODLAND_SNAP_MAX_EDGES);
        x = woodland_snap_axis(x, outer_w, area.x, area.x + area.width,
                               edges, n, WOODLAND_SNAP_DISTANCE);

        n = collect_snap_edges(server, view, false, x, outer_w,
                               edges, WOODLAND_SNAP_MAX_EDGES);
        y = woodland_snap_axis(y, outer_h, area.y, area.y + area.height,
                               edges, n, WOODLAND_SNAP_DISTANCE);

        /*
         * 贴边铺满的判定：只有真的拖动了（超过阈值距离）才算，
         * 否则"按住标题栏不动就松手"会把刚开在角落的窗口变成半屏。
         */
        double dx = server->cursor->x - server->drag_start_x;
        double dy = server->cursor->y - server->drag_start_y;
        if ((dx < 0 ? -dx : dx) > WOODLAND_SNAP_DISTANCE
                || (dy < 0 ? -dy : dy) > WOODLAND_SNAP_DISTANCE) {
            struct wlr_box outer = { x, y, outer_w, outer_h };
            view->pending_snap_edges = woodland_snap_edges_for(
                &outer, &area, WOODLAND_SNAP_DISTANCE);
        }
    }

    wlr_scene_node_set_position(&view->scene_tree->node,
        x + margins.left, y + margins.top);
}

static void process_cursor_resize(struct woodland_server *server) {
    struct woodland_view *view = server->grabbed_view;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    int width = new_right - new_left;
    int height = new_bottom - new_top;

    /* 客户端 hint 的尺寸上下限（xdg min/max size、X11 WM_NORMAL_HINTS）：
     * 不裁的话能把终端缩到 1px，或者把固定尺寸的对话框拉大 */
    int min_w, min_h, max_w, max_h;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (width < min_w) {
        width = min_w;
    }
    if (max_w > 0 && width > max_w) {
        width = max_w;
    }
    if (height < min_h) {
        height = min_h;
    }
    if (max_h > 0 && height > max_h) {
        height = max_h;
    }
    /* 拖左/上边时对边固定，尺寸变化反向调整原点 */
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = new_right - width;
    } else {
        new_right = new_left + width;
    }
    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = new_bottom - height;
    } else {
        new_bottom = new_top + height;
    }

    struct wlr_box geo_box;
    woodland_view_content_box(view, &geo_box);
    wlr_scene_node_set_position(&view->scene_tree->node,
        new_left - geo_box.x, new_top - geo_box.y);

    woodland_view_request_size(view, new_right - new_left,
                               new_bottom - new_top);
}

void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time) {
    if (server->cursor_mode == WOODLAND_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == WOODLAND_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct woodland_view *view = woodland_view_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    /* 未命中 surface 时，检查是否悬停在某视图的 SSD 区域（标题栏/边框） */
    enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
    uint32_t edges = 0;
    struct woodland_view *ssd_view = NULL;
    if (!view) {
        struct woodland_view *v;
        wl_list_for_each(v, &server->views, link) {
            enum woodland_ssd_part part = woodland_ssd_at(v,
                server->cursor->x, server->cursor->y, &edges);
            if (part != WOODLAND_SSD_NONE) {
                ssd_part = part;
                ssd_view = v;
                break;
            }
        }
    }

    if (ssd_view) {
        /* 悬停 SSD：按区域切换光标，不给 surface 指针焦点 */
        const char *cursor_name = "default";
        if (ssd_part == WOODLAND_SSD_TITLEBAR) {
            cursor_name = "default";
        } else if (ssd_part >= WOODLAND_SSD_BORDER_LEFT
                && ssd_part <= WOODLAND_SSD_BORDER_BOTTOM) {
            cursor_name = "se-resize";
        }
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
            cursor_name);
        wlr_seat_pointer_clear_focus(seat);
        /* 只有按钮才亮；标题栏/边框传 NONE（顺带清掉上一个按钮的高亮） */
        bool on_button = ssd_part == WOODLAND_SSD_BUTTON_CLOSE
            || ssd_part == WOODLAND_SSD_BUTTON_MAXIMIZE
            || ssd_part == WOODLAND_SSD_BUTTON_MINIMIZE;
        ssd_update_hover(server, on_button ? ssd_view : NULL,
                         on_button ? ssd_part : WOODLAND_SSD_NONE);
        return;
    }

    /* 离开 SSD 区域（含移到客户端内容上）：清掉按钮高亮 */
    ssd_update_hover(server, NULL, WOODLAND_SSD_NONE);

    if (!view) {
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

/* ==================== new_toplevel ==================== */

/* foreign-toplevel 与 ext-foreign-toplevel-list 句柄：两个后端共用 */
void woodland_view_init_handles(struct woodland_view *view) {
    struct woodland_server *server = view->server;

    if (server->foreign_toplevel_manager) {
        view->toplevel_handle = wlr_foreign_toplevel_handle_v1_create(
            server->foreign_toplevel_manager);
        if (view->toplevel_handle) {
            view->toplevel_handle->data = view;
            view->ft_request_activate.notify = ft_request_activate;
            wl_signal_add(&view->toplevel_handle->events.request_activate,
                          &view->ft_request_activate);
            view->ft_request_maximize.notify = ft_request_maximize;
            wl_signal_add(&view->toplevel_handle->events.request_maximize,
                          &view->ft_request_maximize);
            view->ft_request_fullscreen.notify = ft_request_fullscreen;
            wl_signal_add(&view->toplevel_handle->events.request_fullscreen,
                          &view->ft_request_fullscreen);
            view->ft_request_minimize.notify = ft_request_minimize;
            wl_signal_add(&view->toplevel_handle->events.request_minimize,
                          &view->ft_request_minimize);
            view->ft_request_close.notify = ft_request_close;
            wl_signal_add(&view->toplevel_handle->events.request_close,
                          &view->ft_request_close);
        }
    }

    /* ext-foreign-toplevel-list-v1 句柄：截图/任务栏类客户端（lswt、grim -T）用。
     * handle->data 指回 view，capture 模块据它建「按窗口取源」。 */
    if (server->ext_toplevel_list) {
        struct wlr_ext_foreign_toplevel_handle_v1_state state = {
            .title = view->title,
            .app_id = view->app_id,
        };
        view->ext_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
            server->ext_toplevel_list, &state);
        if (view->ext_toplevel_handle) {
            view->ext_toplevel_handle->data = view;
            view->ext_handle_destroy.notify = ext_toplevel_handle_destroy;
            wl_signal_add(&view->ext_toplevel_handle->events.destroy,
                          &view->ext_handle_destroy);
        }
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct woodland_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_view");
        return;
    }
    view->server = server;
    view->type = WOODLAND_VIEW_XDG;
    view->xdg_toplevel = xdg_toplevel;
    view->scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
    view->scene_tree->node.data = view;
    xdg_toplevel->base->data = view->scene_tree;

    view->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);

    view->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);

    view->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
    view->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
    view->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
    view->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);
    view->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
    view->set_app_id.notify = xdg_toplevel_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);
    view->set_parent.notify = xdg_toplevel_set_parent;
    wl_signal_add(&xdg_toplevel->events.set_parent, &view->set_parent);

    woodland_view_init_handles(view);
}

/* ==================== popup ==================== */

struct woodland_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct woodland_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
    (void)data;
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct woodland_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    (void)data;
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_popup *xdg_popup = data;

    /* layer-shell 的 popup 分两步：先经 xdg_wm_base.get_popup 创建
     * （此时 parent 尚未关联，为 NULL），再经 zwlr_layer_surface_v1.get_popup
     * 关联到 layer surface。父为 NULL 的 popup 交给 layer.c 的
     * layer_surface->events.new_popup 处理，这里跳过。 */
    if (xdg_popup->parent == NULL) {
        return;
    }

    struct woodland_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_popup");
        return;
    }
    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (parent == NULL) {
        free(popup);
        return;
    }
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ==================== zxdg_decoration ==================== */

/* 应用客户端请求的装饰模式（不直接 set_mode，避免未 initialized 时 assert） */
static void deco_apply_mode(struct woodland_view *view,
                            enum wlr_xdg_toplevel_decoration_v1_mode requested) {
    switch (requested) {
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        view->deco_csd = false;
        if (view->mapped && !view->ssd) {
            woodland_ssd_create(view);
        }
        break;
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        view->deco_csd = true;
        if (view->ssd) {
            woodland_ssd_destroy(view);
        }
        break;
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE:
    default:
        /* 未指定：默认 SSD（见设计决策） */
        view->deco_csd = false;
        if (view->mapped && !view->ssd) {
            woodland_ssd_create(view);
        }
        break;
    }
}

/* 客户端 set_mode/unset_mode 时触发：更新状态，set_mode 延迟到 initial commit */
static void deco_request_mode(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, deco_request_mode);
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;

    deco_apply_mode(view, deco->requested_mode);

    /* 仅在 surface 已 initialized 时才能 schedule configure；回给客户端的模式
     * 必须是"我们实际会采用的模式"，否则客户端会重复画装饰 */
    if (deco->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco,
            view_decoration_mode(view));
    }
}

static void deco_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, deco_destroy);
    if (view->deco_registered) {
        wl_list_remove(&view->deco_destroy.link);
        wl_list_remove(&view->deco_request_mode.link);
        view->deco_registered = false;
    }
    view->deco = NULL;
    (void)data;
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_toplevel_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct wlr_xdg_surface *xdg_surface = deco->toplevel->base;
    if (!xdg_surface || !xdg_surface->data) {
        return;
    }
    struct wlr_scene_tree *tree = xdg_surface->data;
    struct woodland_view *view = tree->node.data;
    if (!view) {
        return;
    }

    /* 注册 mode 请求与销毁监听 */
    view->deco = deco;
    view->deco_request_mode.notify = deco_request_mode;
    wl_signal_add(&deco->events.request_mode, &view->deco_request_mode);
    view->deco_destroy.notify = deco_destroy;
    wl_signal_add(&deco->events.destroy, &view->deco_destroy);
    view->deco_registered = true;

    /* 立即按客户端当前请求模式调整（未 initialized 时 set_mode 延迟到 initial commit） */
    deco_apply_mode(view, deco->requested_mode);
    if (xdg_surface->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco,
            view_decoration_mode(view));
    }
}

/* ==================== init/finish ==================== */

bool woodland_shell_init(struct woodland_server *server) {
    wl_list_init(&server->views);

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        return false;
    }

    server->foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(server->display);

    /* ext-foreign-toplevel-list-v1：只读的窗口列表，配合
     * ext-foreign-toplevel-image-capture-source-v1 做按窗口截图 */
    server->ext_toplevel_list =
        wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);

    server->xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server->display);

    /* xdg-dialog-v1：模态对话框标志。GTK4 / Qt 的模态对话框只通过这个
     * 扩展声明，没有它就只能按 parent 关系猜，焦点分配会不准 */
    server->xdg_dialog_manager =
        wlr_xdg_wm_dialog_v1_create(server->display, 1);

    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
    if (server->xdg_decoration_manager) {
        server->new_toplevel_decoration.notify = server_new_toplevel_decoration;
        wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
                      &server->new_toplevel_decoration);
    }

    return true;
}

void woodland_shell_finish(struct woodland_server *server) {
    if (server->xdg_shell) {
        wl_list_remove(&server->new_xdg_toplevel.link);
        wl_list_remove(&server->new_xdg_popup.link);
    }
    if (server->xdg_decoration_manager) {
        wl_list_remove(&server->new_toplevel_decoration.link);
    }
}

/* ==================== 光标事件处理 ==================== */

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    woodland_cursor_process_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
        event->x, event->y);
    woodland_cursor_process_motion(server, event->time_msec);
}

/*
 * 释放 SSD 按钮：只有指针仍停在按下的那个按钮上才执行动作。
 *
 * 动作放在释放而不是按下，是为了让"按下态"有意义（按住时能看到凹陷），
 * 同时支持按住后拖离按钮取消（与常见工具包的行为一致）。
 */
static void ssd_button_release(struct woodland_server *server) {
    struct woodland_view *view = server->pressed_view;
    enum woodland_ssd_part part = server->pressed_part;
    ssd_clear_pressed(server);

    if (!view || !view->mapped) {
        return;
    }
    uint32_t edges = 0;
    if (woodland_ssd_at(view, server->cursor->x, server->cursor->y, &edges)
            != part) {
        return;
    }

    switch (part) {
    case WOODLAND_SSD_BUTTON_CLOSE:
        woodland_view_close(view);
        break;
    case WOODLAND_SSD_BUTTON_MAXIMIZE:
        woodland_view_set_maximized(view, !view->maximized);
        break;
    case WOODLAND_SSD_BUTTON_MINIMIZE:
        woodland_view_set_minimized(view, true);
        break;
    default:
        break;
    }
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        /* 拖动结束：先决定是否贴边铺满，再清抓取状态（铺满要用 grabbed_view） */
        if (server->cursor_mode == WOODLAND_CURSOR_MOVE && server->grabbed_view) {
            view_apply_snap(server->grabbed_view);
        }
        woodland_cursor_reset_mode(server);
        ssd_button_release(server);
    } else {
        /* 新的按下：先清掉上一次遗留的按下态 */
        ssd_clear_pressed(server);
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct woodland_view *view = woodland_view_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        if (view) {
            woodland_focus_view(view, true);
        } else {
            /* 未命中 surface：先查 SSD 区域（按钮/标题栏/边框） */
            uint32_t edges = 0;
            struct woodland_view *ssd_view = NULL;
            enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
            struct woodland_view *v;
            wl_list_for_each(v, &server->views, link) {
                enum woodland_ssd_part part = woodland_ssd_at(v,
                    server->cursor->x, server->cursor->y, &edges);
                if (part != WOODLAND_SSD_NONE) {
                    ssd_part = part;
                    ssd_view = v;
                    break;
                }
            }
            if (ssd_view) {
                woodland_focus_view(ssd_view, true);
                switch (ssd_part) {
                case WOODLAND_SSD_BUTTON_CLOSE:
                case WOODLAND_SSD_BUTTON_MAXIMIZE:
                case WOODLAND_SSD_BUTTON_MINIMIZE:
                    /* 记下按下目标，释放时再执行（见 ssd_button_release） */
                    server->pressed_view = ssd_view;
                    server->pressed_part = ssd_part;
                    woodland_ssd_set_pressed(ssd_view, ssd_part);
                    break;
                case WOODLAND_SSD_TITLEBAR:
                    woodland_view_begin_interactive(ssd_view, WOODLAND_CURSOR_MOVE, 0);
                    break;
                case WOODLAND_SSD_BORDER_LEFT:
                case WOODLAND_SSD_BORDER_RIGHT:
                case WOODLAND_SSD_BORDER_TOP:
                case WOODLAND_SSD_BORDER_BOTTOM:
                    woodland_view_begin_interactive(ssd_view, WOODLAND_CURSOR_RESIZE, edges);
                    break;
                default:
                    break;
                }
            } else {
                /* 点中 layer surface 时按 interactivity 给焦点 */
                struct wlr_surface *lsurf = NULL;
                double lsx, lsy;
                struct wlr_layer_surface_v1 *layer = woodland_layer_surface_at(
                    server, server->cursor->x, server->cursor->y,
                    &lsurf, &lsx, &lsy);
                if (layer) {
                    woodland_layer_try_focus(server, layer);
                }
            }
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
    (void)data;
}

void woodland_cursor_init_events(struct woodland_server *server) {
    server->cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
        &server->cursor_motion_absolute);
    server->cursor_button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
}

void woodland_cursor_finish_events(struct woodland_server *server) {
    if (server->cursor) {
        wl_list_remove(&server->cursor_motion.link);
        wl_list_remove(&server->cursor_motion_absolute.link);
        wl_list_remove(&server->cursor_button.link);
        wl_list_remove(&server->cursor_axis.link);
        wl_list_remove(&server->cursor_frame.link);
    }
}
