// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "output.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"

/* ==================== 几何纯函数 ==================== */

struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server) {
    struct wlr_box box = {0};
    struct wlr_output *output = NULL;

    /* 取输出 layout 中的第一个输出作为最大化目标（单输出场景与多输出首个一致） */
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        output = wo->wlr_output;
        break;
    }
    if (!output) {
        return box;
    }

    wlr_output_layout_get_box(server->output_layout, output, &box);
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
    /* 只聚焦当前工作区的 mapped 视图 */
    if (!view->mapped || view->workspace != server->active_workspace) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = view->xdg_toplevel->base->surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
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
    view->workspace = server->active_workspace;
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

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_maximize);
    if (view->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
    }
    (void)data;
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_fullscreen);
    if (view->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
    }
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
        woodland_view_focus(view);
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
