// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "output.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"

/* ==================== 几何纯函数 ==================== */

struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server) {
    struct wlr_box box = {0};

    /* 取输出 layout 中的第一个输出作为最大化目标（单输出场景与多输出首个一致） */
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        /* usable_area 是输出局部坐标，须加 layout 偏移转成布局坐标 */
        wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
        if (wo->usable_area_valid) {
            box.x += wo->usable_area.x;
            box.y += wo->usable_area.y;
            box.width = wo->usable_area.width;
            box.height = wo->usable_area.height;
        }
        return box;
    }
    return box;
}

struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output) {
    (void)server;
    struct wlr_box box = {0};
    if (!output) {
        return box;
    }
    /* 全屏取输出完整 box，布局坐标 */
    wlr_output_layout_get_box(server->output_layout, output, &box);
    return box;
}

/* ==================== view 聚焦与命中 ==================== */

void woodland_view_focus(struct woodland_view *view) {
    if (view == NULL) {
        return;
    }
    struct woodland_server *server = view->server;
    /* 只聚焦当前工作区、已 mapped 且未最小化的视图 */
    if (!view->mapped || view->minimized ||
        view->workspace != server->active_workspace) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = view->xdg_toplevel->base->surface;
    if (prev_surface == surface) {
        return;
    }
    /* view 得焦点即 layer surface 失焦点 */
    server->focused_layer = NULL;
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
            struct wlr_scene_tree *prev_tree = prev_toplevel->base->data;
            if (prev_tree) {
                struct woodland_view *prev_view = prev_tree->node.data;
                if (prev_view && prev_view->toplevel_handle) {
                    wlr_foreign_toplevel_handle_v1_set_activated(
                        prev_view->toplevel_handle, false);
                }
            }
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_activated(view->toplevel_handle, true);
    }
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

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

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map);
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
    woodland_view_focus(view);
    (void)data;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    if (view == view->server->grabbed_view) {
        woodland_cursor_reset_mode(view->server);
    }
    view->mapped = false;
    wl_list_remove(&view->link);
    (void)data;
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, commit);
    if (view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }
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
    /* foreign-toplevel handle 与 listener */
    if (view->toplevel_handle) {
        wl_list_remove(&view->ft_request_activate.link);
        wl_list_remove(&view->ft_request_maximize.link);
        wl_list_remove(&view->ft_request_fullscreen.link);
        wl_list_remove(&view->ft_request_minimize.link);
        wl_list_remove(&view->ft_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(view->toplevel_handle);
    }
    free(view);
    (void)data;
}

/* ==================== 交互式 move/resize ==================== */

static void begin_interactive(struct woodland_view *view,
                              enum woodland_cursor_mode mode, uint32_t edges) {
    struct woodland_server *server = view->server;
    server->grabbed_view = view;
    server->cursor_mode = mode;

    if (mode == WOODLAND_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - view->scene_tree->node.x;
        server->grab_y = server->cursor->y - view->scene_tree->node.y;
    } else {
        struct wlr_box *geo_box = &view->xdg_toplevel->base->geometry;
        double border_x = (view->scene_tree->node.x + geo_box->x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (view->scene_tree->node.y + geo_box->y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = *geo_box;
        server->grab_geobox.x += view->scene_tree->node.x;
        server->grab_geobox.y += view->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

/* ==================== 窗口状态处理 ==================== */

static void view_save_geometry(struct woodland_view *view) {
    view->saved_geometry.x = view->scene_tree->node.x;
    view->saved_geometry.y = view->scene_tree->node.y;
    view->saved_geometry.width = view->xdg_toplevel->base->geometry.width;
    view->saved_geometry.height = view->xdg_toplevel->base->geometry.height;
}

static void view_apply_geometry(struct woodland_view *view, struct wlr_box *box) {
    wlr_scene_node_set_position(&view->scene_tree->node, box->x, box->y);
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, box->width, box->height);
}

static void view_set_maximized(struct woodland_view *view, bool maximized) {
    if (maximized == view->maximized) {
        return;
    }
    struct woodland_server *server = view->server;
    if (maximized) {
        /* 先退出 fullscreen（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->fullscreen) {
            view->fullscreen = false;
            view_apply_geometry(view, &view->saved_geometry);
            wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, false);
        }
        view_save_geometry(view);
        view->maximized = true;
        struct wlr_box box = woodland_view_maximized_geometry(server);
        view_apply_geometry(view, &box);
        wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
    } else {
        view->maximized = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
    }
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
    }
}

static void view_set_fullscreen(struct woodland_view *view, bool fullscreen,
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
            wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
        }
        view_save_geometry(view);
        view->fullscreen = true;
        if (!output) {
            /* 无指定输出时取第一个输出 */
            struct woodland_output *wo;
            wl_list_for_each(wo, &server->outputs, link) {
                output = wo->wlr_output;
                break;
            }
        }
        struct wlr_box box = woodland_view_fullscreen_geometry(server, output);
        view_apply_geometry(view, &box);
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, true);
    } else {
        view->fullscreen = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, false);
    }
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
    }
}

static void view_set_minimized(struct woodland_view *view, bool minimized) {
    if (minimized == view->minimized) {
        return;
    }
    view->minimized = minimized;
    wlr_scene_node_set_enabled(&view->scene_tree->node, !minimized);
    if (minimized) {
        /* 最小化后若它是焦点视图则清键盘焦点并同步 deactivated */
        struct wlr_surface *focused =
            view->server->seat->keyboard_state.focused_surface;
        if (focused == view->xdg_toplevel->base->surface) {
            wlr_seat_keyboard_notify_clear_focus(view->server->seat);
            wlr_xdg_toplevel_set_activated(view->xdg_toplevel, false);
            if (view->toplevel_handle) {
                wlr_foreign_toplevel_handle_v1_set_activated(
                    view->toplevel_handle, false);
            }
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
    view_set_maximized(view, view->xdg_toplevel->requested.maximized);
    (void)data;
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_fullscreen);
    view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen,
                        view->xdg_toplevel->requested.fullscreen_output);
    (void)data;
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_minimize);
    view_set_minimized(view, view->xdg_toplevel->requested.minimized);
    (void)data;
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    view->title = view->xdg_toplevel->title;
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_title(
            view->toplevel_handle, view->title);
    }
    (void)data;
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_app_id);
    view->app_id = view->xdg_toplevel->app_id;
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_app_id(
            view->toplevel_handle, view->app_id);
    }
    (void)data;
}

static void xdg_toplevel_set_parent(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xdg_toplevel *parent = view->xdg_toplevel->parent;
    if (parent && parent->base->data) {
        struct wlr_scene_tree *parent_tree = parent->base->data;
        view->parent = parent_tree->node.data;
    } else {
        view->parent = NULL;
    }
    /* 同步 foreign-toplevel parent */
    if (view->toplevel_handle) {
        struct wlr_foreign_toplevel_handle_v1 *parent_handle =
            (view->parent && view->parent->toplevel_handle)
                ? view->parent->toplevel_handle : NULL;
        wlr_foreign_toplevel_handle_v1_set_parent(
            view->toplevel_handle, parent_handle);
    }
    (void)data;
}

/* ==================== foreign-toplevel 请求 ==================== */

static void ft_request_activate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_activate);
    /* 若最小化则先恢复 */
    if (view->minimized) {
        view_set_minimized(view, false);
    }
    woodland_view_focus(view);
    (void)data;
}

static void ft_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    view_set_maximized(view, event->maximized);
}

static void ft_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    view_set_fullscreen(view, event->fullscreen, event->output);
}

static void ft_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    view_set_minimized(view, event->minimized);
}

static void ft_request_close(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_close);
    wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    (void)data;
}

/* ==================== 交互式 motion 处理 ==================== */

void woodland_cursor_reset_mode(struct woodland_server *server) {
    server->cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
}

static void process_cursor_move(struct woodland_server *server) {
    struct woodland_view *view = server->grabbed_view;
    wlr_scene_node_set_position(&view->scene_tree->node,
        server->cursor->x - server->grab_x,
        server->cursor->y - server->grab_y);
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

    struct wlr_box *geo_box = &view->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&view->scene_tree->node,
        new_left - geo_box->x, new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, new_width, new_height);
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

    /* foreign-toplevel handle */
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

/* ==================== init/finish ==================== */

bool woodland_shell_init(struct woodland_server *server) {
    wl_list_init(&server->views);

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        return false;
    }

    server->foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(server->display);

    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    return true;
}

void woodland_shell_finish(struct woodland_server *server) {
    if (server->xdg_shell) {
        wl_list_remove(&server->new_xdg_toplevel.link);
        wl_list_remove(&server->new_xdg_popup.link);
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

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        woodland_cursor_reset_mode(server);
    } else {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct woodland_view *view = woodland_view_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        if (view) {
            woodland_view_focus(view);
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
