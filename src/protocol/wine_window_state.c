// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "session_lock.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"
#include "treeland-wine-window-state-unstable-v1-protocol.h"

struct woodland_wine_state {
    struct wl_list link; /* woodland_wine_state_binding.states */
    struct woodland_wine_state_binding *binding;
    struct wl_resource *resource;
    struct woodland_view *view;
    struct wl_listener toplevel_destroy;
    struct wl_event_source *attention_timer;
    uint32_t last_reported_flags;
};

struct woodland_wine_state_binding {
    struct wl_list link; /* woodland_proto_wine_state.bindings */
    struct woodland_proto_wine_state *ws;
    struct wl_resource *resource;
    struct wl_list states; /* struct woodland_wine_state */
};

struct woodland_proto_wine_state {
    struct woodland_proto *proto;
    struct wl_global *global;
    struct wl_list bindings; /* struct woodland_wine_state_binding */
    struct wl_listener view_changed;

    uint32_t binding_count;
    uint32_t state_count;
    uint32_t unminimize_count;
    uint32_t activate_count;
    uint32_t activate_denied_count;
    uint32_t attention_set_count;
    uint32_t attention_clear_count;
    uint32_t last_state_flags;
};

static uint32_t compute_view_state_flags(const struct woodland_view *view)
{
    if (!view) {
        return 0;
    }
    uint32_t flags = 0;
    if (view->minimized) {
        flags |= TREELAND_WINE_WINDOW_STATE_V1_STATE_MINIMIZED;
    }
    if (view->attention) {
        flags |= TREELAND_WINE_WINDOW_STATE_V1_STATE_ATTENTION;
    }
    return flags;
}

static int attention_timer_handle(void *data)
{
    struct woodland_wine_state *state = data;
    if (!state) {
        return 0;
    }
    if (state->attention_timer) {
        wl_event_source_remove(state->attention_timer);
        state->attention_timer = NULL;
    }
    if (state->view) {
        woodland_view_set_attention(state->view, false);
    }
    return 0;
}

static void state_handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct woodland_wine_state *state =
        wl_container_of(listener, state, toplevel_destroy);
    if (state->attention_timer) {
        wl_event_source_remove(state->attention_timer);
        state->attention_timer = NULL;
    }
    wl_list_remove(&state->toplevel_destroy.link);
    wl_list_init(&state->toplevel_destroy.link);
    state->view = NULL;
}

static void state_destroy(struct woodland_wine_state *state)
{
    if (!state) {
        return;
    }
    if (state->attention_timer) {
        wl_event_source_remove(state->attention_timer);
        state->attention_timer = NULL;
    }
    if (state->view && state->toplevel_destroy.link.prev) {
        wl_list_remove(&state->toplevel_destroy.link);
        wl_list_init(&state->toplevel_destroy.link);
    }
    if (state->link.prev && state->link.next) {
        wl_list_remove(&state->link);
        wl_list_init(&state->link);
    }
    if (state->binding && state->binding->ws && state->binding->ws->state_count > 0) {
        state->binding->ws->state_count--;
    }
    state->view = NULL;
    free(state);
}

static void state_resource_destroy(struct wl_resource *resource)
{
    struct woodland_wine_state *state = wl_resource_get_user_data(resource);
    if (state) {
        state->resource = NULL;
        state_destroy(state);
    }
}

static void state_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void state_handle_unminimize(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct woodland_wine_state *state = wl_resource_get_user_data(resource);
    if (!state || !state->view || !state->binding || !state->binding->ws) {
        return;
    }

    state->binding->ws->unminimize_count++;
    if (state->view->minimized) {
        woodland_view_set_minimized(state->view, false);
    }
}

static void state_handle_activate(struct wl_client *client,
                                 struct wl_resource *resource,
                                 uint32_t serial,
                                 uint32_t reason,
                                 struct wl_resource *seat_resource)
{
    (void)client;
    (void)seat_resource;
    struct woodland_wine_state *state = wl_resource_get_user_data(resource);
    if (!state || !state->binding || !state->binding->ws) {
        return;
    }

    struct woodland_proto_wine_state *ws = state->binding->ws;
    ws->activate_count++;

    struct woodland_view *view = state->view;
    if (!view || !view->mapped || !view->server) {
        ws->activate_denied_count++;
        treeland_wine_window_state_v1_send_activate_denied(resource, serial);
        return;
    }

    if (woodland_session_lock_is_locked(view->server)) {
        ws->activate_denied_count++;
        treeland_wine_window_state_v1_send_activate_denied(resource, serial);
        return;
    }

    if (view->minimized) {
        if (reason == TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_RESTORE) {
            woodland_view_set_minimized(view, false);
        } else if (!woodland_view_focusable(view)) {
            ws->activate_denied_count++;
            treeland_wine_window_state_v1_send_activate_denied(resource, serial);
            return;
        }
    }

    if (!woodland_view_focusable(view)) {
        ws->activate_denied_count++;
        treeland_wine_window_state_v1_send_activate_denied(resource, serial);
        return;
    }

    woodland_focus_view(view, true);
}

static void state_handle_set_attention(struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t count,
                                      uint32_t timeout_ms)
{
    (void)client;
    (void)count;
    struct woodland_wine_state *state = wl_resource_get_user_data(resource);
    if (!state || !state->view || !state->binding || !state->binding->ws) {
        return;
    }

    struct woodland_proto_wine_state *ws = state->binding->ws;
    ws->attention_set_count++;

    if (state->attention_timer) {
        wl_event_source_remove(state->attention_timer);
        state->attention_timer = NULL;
    }

    woodland_view_set_attention(state->view, true);

    if (timeout_ms > 0 && ws->proto && ws->proto->server && ws->proto->server->event_loop) {
        state->attention_timer = wl_event_loop_add_timer(ws->proto->server->event_loop,
                                                         attention_timer_handle,
                                                         state);
        if (state->attention_timer) {
            wl_event_source_timer_update(state->attention_timer, (int)timeout_ms);
        }
    }
}

static void state_handle_clear_attention(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct woodland_wine_state *state = wl_resource_get_user_data(resource);
    if (!state || !state->view || !state->binding || !state->binding->ws) {
        return;
    }

    state->binding->ws->attention_clear_count++;

    if (state->attention_timer) {
        wl_event_source_remove(state->attention_timer);
        state->attention_timer = NULL;
    }

    woodland_view_set_attention(state->view, false);
}

static const struct treeland_wine_window_state_v1_interface state_impl = {
    .destroy = state_handle_destroy,
    .unminimize = state_handle_unminimize,
    .activate = state_handle_activate,
    .set_attention = state_handle_set_attention,
    .clear_attention = state_handle_clear_attention,
};

static void binding_destroy(struct woodland_wine_state_binding *binding)
{
    if (!binding) {
        return;
    }
    struct woodland_wine_state *s, *tmp;
    wl_list_for_each_safe(s, tmp, &binding->states, link) {
        if (s->resource) {
            wl_resource_set_user_data(s->resource, NULL);
            s->resource = NULL;
        }
        state_destroy(s);
    }
    if (binding->link.prev && binding->link.next) {
        wl_list_remove(&binding->link);
        wl_list_init(&binding->link);
    }
    if (binding->ws && binding->ws->binding_count > 0) {
        binding->ws->binding_count--;
    }
    free(binding);
}

static void binding_resource_destroy(struct wl_resource *resource)
{
    struct woodland_wine_state_binding *binding = wl_resource_get_user_data(resource);
    if (binding) {
        binding->resource = NULL;
        binding_destroy(binding);
    }
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct woodland_wine_state_binding *binding = wl_resource_get_user_data(resource);
    if (binding && !wl_list_empty(&binding->states)) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "cannot destroy manager before destroying all child state objects");
        return;
    }
    wl_resource_destroy(resource);
}

static void manager_handle_get_window_state(struct wl_client *client,
                                           struct wl_resource *resource,
                                           uint32_t id,
                                           struct wl_resource *toplevel_resource)
{
    struct woodland_wine_state_binding *binding = wl_resource_get_user_data(resource);
    if (!binding || !binding->ws) {
        return;
    }

    if (wl_resource_get_client(toplevel_resource) != client) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "toplevel belongs to another client");
        return;
    }

    struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    if (!xdg_toplevel || !xdg_toplevel->base || !xdg_toplevel->base->surface) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "invalid or defunct toplevel");
        return;
    }

    struct woodland_view *view = woodland_view_from_surface(xdg_toplevel->base->surface);
    if (!view || view->type != WOODLAND_VIEW_XDG || view->xdg_toplevel != xdg_toplevel) {
        wl_resource_post_error(resource,
                               TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL,
                               "invalid or defunct toplevel");
        return;
    }

    /* 检查是否已有 state 对象绑定给该 toplevel */
    struct woodland_wine_state_binding *b;
    struct woodland_wine_state *s;
    wl_list_for_each(b, &binding->ws->bindings, link) {
        wl_list_for_each(s, &b->states, link) {
            if (s->view == view) {
                wl_resource_post_error(resource,
                                       TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_TOPLEVEL_ALREADY_BOUND,
                                       "toplevel already bound");
                return;
            }
        }
    }

    struct woodland_wine_state *state = calloc(1, sizeof(*state));
    if (!state) {
        wl_resource_post_no_memory(resource);
        return;
    }

    state->binding = binding;
    state->view = view;
    wl_list_init(&state->link);
    wl_list_init(&state->toplevel_destroy.link);

    state->resource = wl_resource_create(client,
                                        &treeland_wine_window_state_v1_interface,
                                        wl_resource_get_version(resource),
                                        id);
    if (!state->resource) {
        free(state);
        wl_resource_post_no_memory(resource);
        return;
    }

    wl_resource_set_implementation(state->resource, &state_impl, state,
                                   state_resource_destroy);

    woodland_listener_connect(&state->toplevel_destroy, &xdg_toplevel->events.destroy,
                              state_handle_toplevel_destroy, "wine_state.toplevel_destroy");

    wl_list_insert(&binding->states, &state->link);
    binding->ws->state_count++;

    uint32_t flags = compute_view_state_flags(view);
    state->last_reported_flags = flags;
    binding->ws->last_state_flags = flags;
    treeland_wine_window_state_v1_send_state_changed(state->resource, flags);
}

static const struct treeland_wine_window_state_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_window_state = manager_handle_get_window_state,
};

static void wine_state_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_wine_state *ws = data;
    if (!ws) {
        return;
    }

    if (woodland_proto_security_context_is_sandboxed(ws->proto, client)) {
        struct wl_resource *res = wl_resource_create(client,
                                                     &treeland_wine_window_state_manager_v1_interface,
                                                     version, id);
        if (res) {
            wl_resource_post_error(res, WL_DISPLAY_ERROR_INVALID_OBJECT,
                                   "sandboxed client not permitted to bind treeland_wine_window_state_manager_v1");
        }
        return;
    }

    struct woodland_wine_state_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        struct wl_resource *res = wl_resource_create(client,
                                                     &treeland_wine_window_state_manager_v1_interface,
                                                     version, id);
        if (res) {
            wl_resource_post_no_memory(res);
        }
        return;
    }

    binding->ws = ws;
    wl_list_init(&binding->link);
    wl_list_init(&binding->states);

    binding->resource = wl_resource_create(client,
                                          &treeland_wine_window_state_manager_v1_interface,
                                          version, id);
    if (!binding->resource) {
        free(binding);
        return;
    }

    wl_resource_set_implementation(binding->resource, &manager_impl, binding,
                                   binding_resource_destroy);
    wl_list_insert(&ws->bindings, &binding->link);
    ws->binding_count++;
}

static void handle_view_changed(struct wl_listener *listener, void *data)
{
    struct woodland_proto_wine_state *ws = wl_container_of(listener, ws, view_changed);
    struct woodland_view *view = data;
    if (!ws || !view) {
        return;
    }

    struct woodland_wine_state_binding *b;
    struct woodland_wine_state *s;
    wl_list_for_each(b, &ws->bindings, link) {
        wl_list_for_each(s, &b->states, link) {
            if (s->view != view || !s->resource) {
                continue;
            }

            uint32_t flags = compute_view_state_flags(view);
            if (flags != s->last_reported_flags) {
                s->last_reported_flags = flags;
                ws->last_state_flags = flags;
                treeland_wine_window_state_v1_send_state_changed(s->resource, flags);
            }
        }
    }
}

bool woodland_proto_wine_state_init(struct woodland_proto *proto)
{
    if (!proto || !proto->server || !proto->server->display) {
        return false;
    }

    struct woodland_proto_wine_state *ws = calloc(1, sizeof(*ws));
    if (!ws) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_proto_wine_state");
        return false;
    }

    ws->proto = proto;
    wl_list_init(&ws->bindings);
    wl_list_init(&ws->view_changed.link);

    ws->global = wl_global_create(proto->server->display,
                                  &treeland_wine_window_state_manager_v1_interface,
                                  1,
                                  ws,
                                  wine_state_bind);
    if (!ws->global) {
        wlr_log(WLR_ERROR, "failed to create treeland_wine_window_state_manager_v1 global");
        free(ws);
        return false;
    }

    if (proto->server->view_changed.listener_list.next) {
        woodland_listener_connect(&ws->view_changed, &proto->server->view_changed,
                                  handle_view_changed, "wine_state.view_changed");
    }

    proto->wine_state = ws;
    wlr_log(WLR_INFO, "treeland_wine_window_state_manager_v1 initialized");
    return true;
}

void woodland_proto_wine_state_finish(struct woodland_proto *proto)
{
    if (!proto || !proto->wine_state) {
        return;
    }

    struct woodland_proto_wine_state *ws = proto->wine_state;
    if (ws->view_changed.link.prev) {
        wl_list_remove(&ws->view_changed.link);
        wl_list_init(&ws->view_changed.link);
    }

    struct woodland_wine_state_binding *b, *tmp;
    wl_list_for_each_safe(b, tmp, &ws->bindings, link) {
        if (b->resource) {
            wl_resource_set_user_data(b->resource, NULL);
            b->resource = NULL;
        }
        binding_destroy(b);
    }

    if (ws->global) {
        wl_global_destroy(ws->global);
        ws->global = NULL;
    }

    proto->wine_state = NULL;
    free(ws);
}

void woodland_protocol_wine_state_status(struct woodland_server *server,
                                         struct woodland_json *out)
{
    if (!out) {
        return;
    }

    struct woodland_proto_wine_state *ws =
        (server && server->proto) ? server->proto->wine_state : NULL;

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "available", ws != NULL);
    if (ws) {
        woodland_json_obj_int(&obj, "binding_count", (int)ws->binding_count);
        woodland_json_obj_int(&obj, "state_count", (int)ws->state_count);
        woodland_json_obj_int(&obj, "unminimize_count", (int)ws->unminimize_count);
        woodland_json_obj_int(&obj, "activate_count", (int)ws->activate_count);
        woodland_json_obj_int(&obj, "activate_denied_count", (int)ws->activate_denied_count);
        woodland_json_obj_int(&obj, "attention_set_count", (int)ws->attention_set_count);
        woodland_json_obj_int(&obj, "attention_clear_count", (int)ws->attention_clear_count);
        woodland_json_obj_int(&obj, "last_state_flags", (int)ws->last_state_flags);
    }
    woodland_json_obj_close(&obj);
}

bool woodland_protocol_wine_state_query_state(struct woodland_server *server,
                                              struct woodland_wine_state_query_state *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    struct woodland_proto_wine_state *ws =
        (server && server->proto) ? server->proto->wine_state : NULL;
    if (!ws) {
        return false;
    }

    out->available = true;
    out->binding_count = ws->binding_count;
    out->state_count = ws->state_count;
    out->unminimize_count = ws->unminimize_count;
    out->activate_count = ws->activate_count;
    out->activate_denied_count = ws->activate_denied_count;
    out->attention_set_count = ws->attention_set_count;
    out->attention_clear_count = ws->attention_clear_count;
    out->last_state_flags = ws->last_state_flags;
    return true;
}
