// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"
#include "treeland-wine-window-management-unstable-v1-protocol.h"

struct woodland_wine_window_control {
    struct wl_list link; /* woodland_wine_wm_session.controls */
    struct woodland_wine_wm_session *session;
    struct wl_resource *resource;
    struct woodland_view *view;
    struct wl_listener toplevel_destroy;
    uint32_t window_id;
    int32_t last_reported_x;
    int32_t last_reported_y;
    uint32_t last_reported_topmost;
    bool suppress_position_event;
};

struct woodland_wine_wm_session {
    struct wl_list link; /* woodland_proto_wine_wm.sessions */
    struct woodland_proto_wine_wm *wm;
    struct wl_resource *resource;
    struct wl_list controls; /* struct woodland_wine_window_control */
    uint32_t next_window_id;
};

struct woodland_proto_wine_wm {
    struct woodland_proto *proto;
    struct wl_global *global;
    struct wl_list sessions; /* struct woodland_wine_wm_session */
    struct wl_listener view_changed;

    uint32_t session_count;
    uint32_t control_count;
    uint32_t position_requests;
    uint32_t z_order_requests;
    int32_t last_x;
    int32_t last_y;
    uint32_t last_serial;
    uint32_t last_topmost;
};

static void control_handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct woodland_wine_window_control *control =
        wl_container_of(listener, control, toplevel_destroy);
    wl_list_remove(&control->toplevel_destroy.link);
    wl_list_init(&control->toplevel_destroy.link);
    control->view = NULL;
}

static void control_destroy(struct woodland_wine_window_control *control)
{
    if (!control) {
        return;
    }
    if (control->view && control->toplevel_destroy.link.prev) {
        wl_list_remove(&control->toplevel_destroy.link);
        wl_list_init(&control->toplevel_destroy.link);
    }
    if (control->link.prev && control->link.next) {
        wl_list_remove(&control->link);
        wl_list_init(&control->link);
    }
    if (control->session && control->session->wm && control->session->wm->control_count > 0) {
        control->session->wm->control_count--;
    }
    control->view = NULL;
    free(control);
}

static void control_resource_destroy(struct wl_resource *resource)
{
    struct woodland_wine_window_control *control = wl_resource_get_user_data(resource);
    if (control) {
        control->resource = NULL;
        control_destroy(control);
    }
}

static void control_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void control_handle_set_position(struct wl_client *client,
                                       struct wl_resource *resource,
                                       int32_t x,
                                       int32_t y,
                                       uint32_t serial)
{
    (void)client;
    if (serial == 0) {
        return;
    }
    struct woodland_wine_window_control *control = wl_resource_get_user_data(resource);
    if (!control || !control->view || !control->session || !control->session->wm) {
        return;
    }

    struct woodland_proto_wine_wm *wm = control->session->wm;
    wm->position_requests++;

    struct woodland_view *view = control->view;
    struct woodland_server *server = view->server;

    /* 校验 (x, y) 是否落在有效输出几何内 */
    bool in_screen = false;
    if (server && server->output_layout) {
        in_screen = wlr_output_layout_contains_point(server->output_layout, NULL, x, y);
    }

    if (!in_screen) {
        /* 超出屏幕：拒绝并以请求的 serial 回报原坐标 */
        int32_t cur_x = view->scene_tree ? (int32_t)view->scene_tree->node.x : 0;
        int32_t cur_y = view->scene_tree ? (int32_t)view->scene_tree->node.y : 0;
        wm->last_x = cur_x;
        wm->last_y = cur_y;
        wm->last_serial = serial;
        treeland_wine_window_control_v1_send_configure_position(resource, cur_x, cur_y, serial);
        return;
    }

    control->suppress_position_event = true;
    if (view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    }
    woodland_view_update_outputs(view);
    control->suppress_position_event = false;

    control->last_reported_x = x;
    control->last_reported_y = y;
    wm->last_x = x;
    wm->last_y = y;
    wm->last_serial = serial;

    treeland_wine_window_control_v1_send_configure_position(resource, x, y, serial);
}

static void control_handle_set_z_order(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t op,
                                      uint32_t sibling_id)
{
    (void)client;
    struct woodland_wine_window_control *control = wl_resource_get_user_data(resource);
    if (!control || !control->view || !control->session || !control->session->wm) {
        return;
    }

    struct woodland_proto_wine_wm *wm = control->session->wm;
    wm->z_order_requests++;

    if (op != TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_INSERT_AFTER && sibling_id != 0) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_CONTROL_V1_ERROR_INVALID_SIBLING,
                               "sibling_id must be 0 for op %u", op);
        return;
    }

    struct woodland_view *view = control->view;

    switch (op) {
    case TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_TOP:
        woodland_view_restack(view);
        wm->last_topmost = (woodland_view_get_layer(view) == WOODLAND_VIEW_LAYER_ALWAYS_ON_TOP) ? 1 : 0;
        /* hwnd_top 不改变 tier，不发射 configure_stacking */
        break;

    case TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_BOTTOM:
        if (view->always_on_top) {
            woodland_view_set_always_on_top(view, false);
        }
        if (view->scene_tree) {
            wlr_scene_node_lower_to_bottom(&view->scene_tree->node);
        }
        wl_list_remove(&view->link);
        wl_list_insert(view->server->views.prev, &view->link);
        if (view->server->xwayland_restack_hook) {
            view->server->xwayland_restack_hook(view);
        }
        control->last_reported_topmost = 0;
        wm->last_topmost = 0;
        treeland_wine_window_control_v1_send_configure_stacking(resource, 0);
        break;

    case TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_TOPMOST:
        woodland_view_set_always_on_top(view, true);
        control->last_reported_topmost = 1;
        wm->last_topmost = 1;
        treeland_wine_window_control_v1_send_configure_stacking(resource, 1);
        break;

    case TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_NOTOPMOST:
        woodland_view_set_always_on_top(view, false);
        control->last_reported_topmost = 0;
        wm->last_topmost = 0;
        treeland_wine_window_control_v1_send_configure_stacking(resource, 0);
        break;

    case TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_INSERT_AFTER: {
        struct woodland_wine_window_control *sib = NULL;
        struct woodland_wine_window_control *c;
        wl_list_for_each(c, &control->session->controls, link) {
            if (c->window_id == sibling_id) {
                sib = c;
                break;
            }
        }
        if (!sib || sib == control || !sib->view) {
            return;
        }

        /* 跨层级操作不混合：若层级不同，按规范退化为目标 tier 内的 hwnd_top */
        if (woodland_view_get_layer(view) != woodland_view_get_layer(sib->view)) {
            woodland_view_restack(view);
            return;
        }

        if (view->scene_tree && sib->view->scene_tree) {
            wlr_scene_node_place_below(&view->scene_tree->node, &sib->view->scene_tree->node);
        }
        wl_list_remove(&view->link);
        wl_list_insert(&sib->view->link, &view->link);
        if (view->server->xwayland_restack_hook) {
            view->server->xwayland_restack_hook(view);
        }
        /* hwnd_insert_after 不改变 tier，不发射 configure_stacking */
        break;
    }
    default:
        break;
    }
}

static const struct treeland_wine_window_control_v1_interface control_impl = {
    .destroy = control_handle_destroy,
    .set_position = control_handle_set_position,
    .set_z_order = control_handle_set_z_order,
};

static void session_destroy(struct woodland_wine_wm_session *session)
{
    if (!session) {
        return;
    }
    struct woodland_wine_window_control *c, *tmp;
    wl_list_for_each_safe(c, tmp, &session->controls, link) {
        if (c->resource) {
            wl_resource_set_user_data(c->resource, NULL);
            c->resource = NULL;
        }
        control_destroy(c);
    }
    if (session->link.prev && session->link.next) {
        wl_list_remove(&session->link);
        wl_list_init(&session->link);
    }
    if (session->wm && session->wm->session_count > 0) {
        session->wm->session_count--;
    }
    free(session);
}

static void session_resource_destroy(struct wl_resource *resource)
{
    struct woodland_wine_wm_session *session = wl_resource_get_user_data(resource);
    if (session) {
        session->resource = NULL;
        session_destroy(session);
    }
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct woodland_wine_wm_session *session = wl_resource_get_user_data(resource);
    if (session && !wl_list_empty(&session->controls)) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "cannot destroy manager before destroying all child control objects");
        return;
    }
    wl_resource_destroy(resource);
}

static void manager_handle_get_window_control(struct wl_client *client,
                                             struct wl_resource *resource,
                                             uint32_t id,
                                             struct wl_resource *toplevel_resource)
{
    struct woodland_wine_wm_session *session = wl_resource_get_user_data(resource);
    if (!session || !session->wm) {
        return;
    }

    if (wl_resource_get_client(toplevel_resource) != client) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "toplevel belongs to another client");
        return;
    }

    struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (!xdg_toplevel || !xdg_toplevel->base || !xdg_toplevel->base->surface) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "invalid or defunct toplevel");
        return;
    }

    struct woodland_view *view = woodland_view_from_surface(xdg_toplevel->base->surface);
    if (!view || view->type != WOODLAND_VIEW_XDG || view->xdg_toplevel != xdg_toplevel) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "invalid or defunct toplevel");
        return;
    }

    /* 检查是否已有 control 对象绑定给该 toplevel */
    struct woodland_wine_wm_session *s;
    struct woodland_wine_window_control *c;
    wl_list_for_each(s, &session->wm->sessions, link) {
        wl_list_for_each(c, &s->controls, link) {
            if (c->view == view) {
                wl_resource_post_error(resource,
                                       TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_TOPLEVEL_ALREADY_CONTROLLED,
                                       "toplevel already controlled");
                return;
            }
        }
    }

    struct woodland_wine_window_control *control = calloc(1, sizeof(*control));
    if (!control) {
        wl_resource_post_no_memory(resource);
        return;
    }

    control->session = session;
    control->view = view;
    control->window_id = ++session->next_window_id;
    wl_list_init(&control->link);
    wl_list_init(&control->toplevel_destroy.link);

    control->resource = wl_resource_create(client,
                                          &treeland_wine_window_control_v1_interface,
                                          wl_resource_get_version(resource),
                                          id);
    if (!control->resource) {
        free(control);
        wl_resource_post_no_memory(resource);
        return;
    }

    wl_resource_set_implementation(control->resource, &control_impl, control,
                                   control_resource_destroy);

    woodland_listener_connect(&control->toplevel_destroy, &xdg_toplevel->events.destroy,
                              control_handle_toplevel_destroy, "wine_wm.toplevel_destroy");

    wl_list_insert(&session->controls, &control->link);
    session->wm->control_count++;

    /* 规范约定的初始事件序列：window_id → configure_position → configure_stacking */
    treeland_wine_window_control_v1_send_window_id(control->resource, control->window_id);

    int32_t x = view->scene_tree ? (int32_t)view->scene_tree->node.x : 0;
    int32_t y = view->scene_tree ? (int32_t)view->scene_tree->node.y : 0;
    control->last_reported_x = x;
    control->last_reported_y = y;
    treeland_wine_window_control_v1_send_configure_position(control->resource, x, y, 0);

    uint32_t topmost = (woodland_view_get_layer(view) == WOODLAND_VIEW_LAYER_ALWAYS_ON_TOP) ? 1 : 0;
    control->last_reported_topmost = topmost;
    treeland_wine_window_control_v1_send_configure_stacking(control->resource, topmost);
}

static const struct treeland_wine_window_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_window_control = manager_handle_get_window_control,
};

static void wine_wm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_wine_wm *wm = data;
    if (!wm) {
        return;
    }

    if (woodland_proto_security_context_is_sandboxed(wm->proto, client)) {
        struct wl_resource *res = wl_resource_create(client,
                                                     &treeland_wine_window_manager_v1_interface,
                                                     version, id);
        if (res) {
            wl_resource_post_error(res, WL_DISPLAY_ERROR_INVALID_OBJECT,
                                   "sandboxed client not permitted to bind treeland_wine_window_manager_v1");
        }
        return;
    }

    struct woodland_wine_wm_session *session = calloc(1, sizeof(*session));
    if (!session) {
        struct wl_resource *res = wl_resource_create(client,
                                                     &treeland_wine_window_manager_v1_interface,
                                                     version, id);
        if (res) {
            wl_resource_post_no_memory(res);
        }
        return;
    }

    session->wm = wm;
    wl_list_init(&session->link);
    wl_list_init(&session->controls);

    session->resource = wl_resource_create(client,
                                          &treeland_wine_window_manager_v1_interface,
                                          version, id);
    if (!session->resource) {
        free(session);
        return;
    }

    wl_resource_set_implementation(session->resource, &manager_impl, session,
                                   session_resource_destroy);
    wl_list_insert(&wm->sessions, &session->link);
    wm->session_count++;
}

static void handle_view_changed(struct wl_listener *listener, void *data)
{
    struct woodland_proto_wine_wm *wm = wl_container_of(listener, wm, view_changed);
    struct woodland_view *view = data;
    if (!wm || !view) {
        return;
    }

    struct woodland_wine_wm_session *s;
    struct woodland_wine_window_control *c;
    wl_list_for_each(s, &wm->sessions, link) {
        wl_list_for_each(c, &s->controls, link) {
            if (c->view != view || !c->resource) {
                continue;
            }

            if (!c->suppress_position_event && view->scene_tree) {
                int32_t cur_x = (int32_t)view->scene_tree->node.x;
                int32_t cur_y = (int32_t)view->scene_tree->node.y;
                if (cur_x != c->last_reported_x || cur_y != c->last_reported_y) {
                    c->last_reported_x = cur_x;
                    c->last_reported_y = cur_y;
                    treeland_wine_window_control_v1_send_configure_position(c->resource,
                                                                           cur_x, cur_y, 0);
                }
            }

            uint32_t topmost = (woodland_view_get_layer(view) == WOODLAND_VIEW_LAYER_ALWAYS_ON_TOP) ? 1 : 0;
            if (topmost != c->last_reported_topmost) {
                c->last_reported_topmost = topmost;
                treeland_wine_window_control_v1_send_configure_stacking(c->resource, topmost);
            }
        }
    }
}

bool woodland_proto_wine_wm_init(struct woodland_proto *proto)
{
    if (!proto || !proto->server || !proto->server->display) {
        return false;
    }

    struct woodland_proto_wine_wm *wm = calloc(1, sizeof(*wm));
    if (!wm) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_proto_wine_wm");
        return false;
    }

    wm->proto = proto;
    wl_list_init(&wm->sessions);
    wl_list_init(&wm->view_changed.link);

    wm->global = wl_global_create(proto->server->display,
                                  &treeland_wine_window_manager_v1_interface,
                                  1,
                                  wm,
                                  wine_wm_bind);
    if (!wm->global) {
        wlr_log(WLR_ERROR, "failed to create treeland_wine_window_manager_v1 global");
        free(wm);
        return false;
    }

    if (proto->server->view_changed.listener_list.next) {
        woodland_listener_connect(&wm->view_changed, &proto->server->view_changed,
                                  handle_view_changed, "wine_wm.view_changed");
    }

    proto->wine_wm = wm;
    wlr_log(WLR_INFO, "treeland_wine_window_manager_v1 initialized");
    return true;
}

void woodland_proto_wine_wm_finish(struct woodland_proto *proto)
{
    if (!proto || !proto->wine_wm) {
        return;
    }

    struct woodland_proto_wine_wm *wm = proto->wine_wm;
    if (wm->view_changed.link.prev) {
        wl_list_remove(&wm->view_changed.link);
        wl_list_init(&wm->view_changed.link);
    }

    struct woodland_wine_wm_session *s, *tmp;
    wl_list_for_each_safe(s, tmp, &wm->sessions, link) {
        if (s->resource) {
            wl_resource_set_user_data(s->resource, NULL);
            s->resource = NULL;
        }
        session_destroy(s);
    }

    if (wm->global) {
        wl_global_destroy(wm->global);
        wm->global = NULL;
    }

    proto->wine_wm = NULL;
    free(wm);
}

void woodland_protocol_wine_wm_status(struct woodland_server *server,
                                      struct woodland_json *out)
{
    if (!out) {
        return;
    }

    struct woodland_proto_wine_wm *wm =
        (server && server->proto) ? server->proto->wine_wm : NULL;

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "available", wm != NULL);
    if (wm) {
        woodland_json_obj_int(&obj, "session_count", (int)wm->session_count);
        woodland_json_obj_int(&obj, "control_count", (int)wm->control_count);
        woodland_json_obj_int(&obj, "position_requests", (int)wm->position_requests);
        woodland_json_obj_int(&obj, "z_order_requests", (int)wm->z_order_requests);
        woodland_json_obj_int(&obj, "last_x", wm->last_x);
        woodland_json_obj_int(&obj, "last_y", wm->last_y);
        woodland_json_obj_int(&obj, "last_serial", (int)wm->last_serial);
        woodland_json_obj_int(&obj, "last_topmost", (int)wm->last_topmost);
    }
    woodland_json_obj_close(&obj);
}

bool woodland_protocol_wine_wm_query_state(struct woodland_server *server,
                                           struct woodland_wine_wm_query_state *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    struct woodland_proto_wine_wm *wm =
        (server && server->proto) ? server->proto->wine_wm : NULL;
    if (!wm) {
        return false;
    }

    out->available = true;
    out->session_count = wm->session_count;
    out->control_count = wm->control_count;
    out->position_requests = wm->position_requests;
    out->z_order_requests = wm->z_order_requests;
    out->last_x = wm->last_x;
    out->last_y = wm->last_y;
    out->last_serial = wm->last_serial;
    out->last_topmost = wm->last_topmost;
    return true;
}
