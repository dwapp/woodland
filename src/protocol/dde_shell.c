// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "output.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "treeland-dde-shell-unstable-v2-protocol.h"

#define MAX_CURSOR_PLACEMENT_OFFSET 10000

struct dde_shell_manager_resource {
    struct wl_list link;
    struct wl_resource *resource;
    struct woodland_proto_dde_shell *shell;
};

struct woodland_dde_shell_surface {
    struct wl_list link;
    struct woodland_proto_dde_shell *shell;
    struct wl_resource *resource;
    struct wlr_surface *wlr_surface;
    struct wl_listener surface_destroy;

    enum woodland_dde_shell_role role;
    bool has_position_hint;
    int32_t position_x;
    int32_t position_y;
    bool has_cursor_placement_hint;
    int32_t cursor_x_offset;
    int32_t cursor_y_offset;
    uint32_t skip_flags;
    bool accept_keyboard_focus;
};

struct woodland_proto_dde_shell {
    struct woodland_proto *proto;
    struct wl_global *global;
    struct wl_list resources; /* struct dde_shell_manager_resource */
    struct wl_list surfaces;  /* struct woodland_dde_shell_surface */

    uint32_t surface_create_count;
    uint32_t duplicate_error_count;
    uint32_t role_change_count;
    uint32_t position_hint_count;
    uint32_t cursor_hint_count;
};

bool woodland_dde_shell_role_is_valid(uint32_t role, uint32_t version)
{
    if (version < 2) {
        return role <= (uint32_t)WOODLAND_DDE_SHELL_ROLE_OVERLAY;
    }
    return role <= (uint32_t)WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP;
}

static struct woodland_dde_shell_surface *find_surface(struct woodland_proto_dde_shell *shell,
                                                       const struct wlr_surface *wlr_surface)
{
    if (!shell || !wlr_surface) {
        return NULL;
    }
    struct woodland_dde_shell_surface *surf;
    wl_list_for_each(surf, &shell->surfaces, link) {
        if (surf->wlr_surface == wlr_surface) {
            return surf;
        }
    }
    return NULL;
}

static void handle_view_role_notify(struct woodland_view *view, void *user_data)
{
    struct woodland_dde_shell_surface *surface = user_data;
    if (!surface || !surface->resource) {
        return;
    }
    surface->role = (enum woodland_dde_shell_role)view->dde_shell.role;
    if (wl_resource_get_version(surface->resource) >= TREELAND_DDE_SHELL_SURFACE_V2_ROLE_SINCE_VERSION) {
        treeland_dde_shell_surface_v2_send_role(surface->resource, (uint32_t)surface->role);
    }
}

static void init_view_dde_shell_state(struct woodland_view *view,
                                      struct woodland_dde_shell_surface *surface)
{
    if (!view || !surface) {
        return;
    }
    view->dde_shell.active = true;
    view->dde_shell.role = (uint32_t)surface->role;
    view->dde_shell.has_position_hint = surface->has_position_hint;
    view->dde_shell.position_x = surface->position_x;
    view->dde_shell.position_y = surface->position_y;
    view->dde_shell.has_cursor_placement_hint = surface->has_cursor_placement_hint;
    view->dde_shell.cursor_x_offset = surface->cursor_x_offset;
    view->dde_shell.cursor_y_offset = surface->cursor_y_offset;
    view->dde_shell.skip_flags = surface->skip_flags;
    view->dde_shell.accept_keyboard_focus = surface->accept_keyboard_focus;
    view->dde_shell.role_notify = handle_view_role_notify;
    view->dde_shell.user_data = surface;

    if (surface->role == WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP) {
        view->always_on_top = true;
    } else {
        view->always_on_top = false;
    }
}

static void dde_shell_init_view(struct woodland_view *view)
{
    if (!view || !view->server || !view->server->proto || !view->server->proto->dde_shell) {
        return;
    }
    struct woodland_proto_dde_shell *shell = view->server->proto->dde_shell;
    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface) {
        return;
    }
    struct woodland_dde_shell_surface *surf = find_surface(shell, surface);
    if (surf) {
        init_view_dde_shell_state(view, surf);
    }
}

/* ==================== treeland_dde_shell_surface_v2 ==================== */

static void shell_surface_handle_destroy(struct wl_client *client,
                                         struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void shell_surface_handle_set_role(struct wl_client *client,
                                          struct wl_resource *resource,
                                          uint32_t role)
{
    (void)client;
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    if (!woodland_dde_shell_role_is_valid(role, version)) {
        wl_resource_post_error(resource, 0,
                               "Invalid treeland_dde_shell_surface_v2::role: %u", role);
        return;
    }

    if (surface->role == (enum woodland_dde_shell_role)role) {
        return;
    }

    surface->role = (enum woodland_dde_shell_role)role;
    surface->shell->role_change_count++;

    if (version >= TREELAND_DDE_SHELL_SURFACE_V2_ROLE_SINCE_VERSION) {
        treeland_dde_shell_surface_v2_send_role(resource, role);
    }

    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view) {
        view->dde_shell.role = role;
        if (role == WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP) {
            view->always_on_top = true;
        } else {
            view->always_on_top = false;
        }
        woodland_view_restack(view);
    }
    woodland_trace_add("dde_shell", view ? view->id : 0, "set_role=%u", role);
}

static void shell_surface_handle_set_position_hint(struct wl_client *client,
                                                   struct wl_resource *resource,
                                                   struct wl_resource *output_resource,
                                                   int32_t x,
                                                   int32_t y)
{
    (void)client;
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    int anchor_x = 0;
    int anchor_y = 0;
    struct woodland_server *server = surface->shell->proto->server;
    if (output_resource) {
        struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
        if (!wlr_output) {
            wl_resource_post_error(resource, 0, "invalid wl_output resource");
            return;
        }
        if (server && server->output_layout) {
            struct wlr_box box;
            wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
            anchor_x = box.x;
            anchor_y = box.y;
        }
    } else {
        struct woodland_output *wo = woodland_output_first(server);
        if (wo && wo->wlr_output && server && server->output_layout) {
            struct wlr_box box;
            wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
            anchor_x = box.x;
            anchor_y = box.y;
        }
    }

    surface->has_cursor_placement_hint = false;
    surface->has_position_hint = true;
    surface->position_x = anchor_x + x;
    surface->position_y = anchor_y + y;
    surface->shell->position_hint_count++;

    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view) {
        view->dde_shell.has_cursor_placement_hint = false;
        view->dde_shell.has_position_hint = true;
        view->dde_shell.position_x = surface->position_x;
        view->dde_shell.position_y = surface->position_y;
    }
}

static void shell_surface_handle_set_cursor_placement_hint(struct wl_client *client,
                                                           struct wl_resource *resource,
                                                           int32_t x_offset,
                                                           int32_t y_offset)
{
    (void)client;
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    if (x_offset < -MAX_CURSOR_PLACEMENT_OFFSET || x_offset > MAX_CURSOR_PLACEMENT_OFFSET ||
        y_offset < -MAX_CURSOR_PLACEMENT_OFFSET || y_offset > MAX_CURSOR_PLACEMENT_OFFSET) {
        wl_resource_post_error(resource, 0,
                               "Invalid cursor placement hint offset: %d,%d",
                               x_offset, y_offset);
        return;
    }

    surface->has_position_hint = false;
    surface->has_cursor_placement_hint = true;
    surface->cursor_x_offset = x_offset;
    surface->cursor_y_offset = y_offset;
    surface->shell->cursor_hint_count++;

    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view) {
        view->dde_shell.has_position_hint = false;
        view->dde_shell.has_cursor_placement_hint = true;
        view->dde_shell.cursor_x_offset = x_offset;
        view->dde_shell.cursor_y_offset = y_offset;
    }
}

static void shell_surface_handle_set_skip_flags(struct wl_client *client,
                                                struct wl_resource *resource,
                                                uint32_t flags)
{
    (void)client;
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    surface->skip_flags = flags;
    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view) {
        view->dde_shell.skip_flags = flags;
        woodland_view_update_taskbar_presence(view);
    }
}

static void shell_surface_handle_set_accept_keyboard_focus(struct wl_client *client,
                                                           struct wl_resource *resource,
                                                           uint32_t accept)
{
    (void)client;
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    surface->accept_keyboard_focus = (accept != 0);
    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view) {
        view->dde_shell.accept_keyboard_focus = surface->accept_keyboard_focus;
        if (!surface->accept_keyboard_focus && view->server->focused_view == view) {
            woodland_focus_none(view->server);
        }
    }
}

static const struct treeland_dde_shell_surface_v2_interface shell_surface_implementation = {
    .destroy = shell_surface_handle_destroy,
    .set_role = shell_surface_handle_set_role,
    .set_position_hint = shell_surface_handle_set_position_hint,
    .set_cursor_placement_hint = shell_surface_handle_set_cursor_placement_hint,
    .set_skip_flags = shell_surface_handle_set_skip_flags,
    .set_accept_keyboard_focus = shell_surface_handle_set_accept_keyboard_focus,
};

static void shell_surface_destroy(struct woodland_dde_shell_surface *surface)
{
    if (!surface) {
        return;
    }
    wl_list_remove(&surface->surface_destroy.link);
    wl_list_remove(&surface->link);

    struct woodland_view *view = woodland_view_from_surface(surface->wlr_surface);
    if (view && view->dde_shell.user_data == surface) {
        view->dde_shell.active = false;
        view->dde_shell.role_notify = NULL;
        view->dde_shell.user_data = NULL;
        woodland_view_restack(view);
    }
    free(surface);
}

static void shell_surface_resource_destroy(struct wl_resource *resource)
{
    struct woodland_dde_shell_surface *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        return;
    }
    surface->resource = NULL;
    shell_surface_destroy(surface);
}

static void handle_wlr_surface_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct woodland_dde_shell_surface *surface =
        wl_container_of(listener, surface, surface_destroy);
    if (surface->resource) {
        wl_resource_destroy(surface->resource);
    } else {
        shell_surface_destroy(surface);
    }
}

/* ==================== treeland_dde_shell_manager_v2 ==================== */

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_get_shell_surface(struct wl_client *client,
                                             struct wl_resource *resource,
                                             uint32_t id,
                                             struct wl_resource *surface_resource)
{
    struct dde_shell_manager_resource *mgr_res = wl_resource_get_user_data(resource);
    struct woodland_proto_dde_shell *shell = mgr_res ? mgr_res->shell : NULL;
    if (!shell) {
        return;
    }
    struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);
    if (!wlr_surface) {
        wl_resource_post_error(resource, 0, "invalid wl_surface resource");
        return;
    }

    struct woodland_dde_shell_surface *existing = find_surface(shell, wlr_surface);
    if (existing) {
        shell->duplicate_error_count++;
        wl_resource_post_error(resource,
                               TREELAND_DDE_SHELL_MANAGER_V2_ERROR_ALREADY_SHELL_SURFACE,
                               "the wl_surface already has a shell surface object");
        return;
    }

    struct woodland_dde_shell_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) {
        wl_client_post_no_memory(client);
        return;
    }
    surf->shell = shell;
    surf->wlr_surface = wlr_surface;
    surf->role = WOODLAND_DDE_SHELL_ROLE_OVERLAY;
    surf->accept_keyboard_focus = true;

    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    surf->resource = wl_resource_create(client, &treeland_dde_shell_surface_v2_interface,
                                        version, id);
    if (!surf->resource) {
        free(surf);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(surf->resource, &shell_surface_implementation, surf,
                                   shell_surface_resource_destroy);

    surf->surface_destroy.notify = handle_wlr_surface_destroy;
    wl_signal_add(&wlr_surface->events.destroy, &surf->surface_destroy);

    wl_list_insert(&shell->surfaces, &surf->link);
    shell->surface_create_count++;

    if (version >= TREELAND_DDE_SHELL_SURFACE_V2_ROLE_SINCE_VERSION) {
        treeland_dde_shell_surface_v2_send_role(surf->resource, (uint32_t)surf->role);
    }

    struct woodland_view *view = woodland_view_from_surface(wlr_surface);
    if (view) {
        init_view_dde_shell_state(view, surf);
    }
    woodland_trace_add("dde_shell", view ? view->id : 0, "create surface id=%u role=overlay", id);
}

static const struct treeland_dde_shell_manager_v2_interface manager_implementation = {
    .destroy = manager_handle_destroy,
    .get_shell_surface = manager_handle_get_shell_surface,
};

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct dde_shell_manager_resource *mgr_res = wl_resource_get_user_data(resource);
    if (!mgr_res) {
        return;
    }
    wl_list_remove(&mgr_res->link);
    free(mgr_res);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_dde_shell *shell = data;
    if (woodland_proto_security_context_is_sandboxed(shell->proto, client)) {
        wlr_log(WLR_ERROR, "dde-shell: denied bind from sandboxed client");
        wl_client_post_implementation_error(client,
            "treeland_dde_shell_manager_v2: bind rejected for a sandboxed client");
        return;
    }
    if (version > 2) {
        version = 2;
    }
    struct dde_shell_manager_resource *mgr_res = calloc(1, sizeof(*mgr_res));
    if (!mgr_res) {
        wl_client_post_no_memory(client);
        return;
    }
    mgr_res->shell = shell;
    mgr_res->resource = wl_resource_create(client, &treeland_dde_shell_manager_v2_interface,
                                          version, id);
    if (!mgr_res->resource) {
        free(mgr_res);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(mgr_res->resource, &manager_implementation, mgr_res,
                                   manager_resource_destroy);
    wl_list_insert(&shell->resources, &mgr_res->link);
}

/* ==================== 接口函数 ==================== */

bool woodland_proto_dde_shell_init(struct woodland_proto *proto)
{
    struct woodland_proto_dde_shell *shell = calloc(1, sizeof(*shell));
    if (!shell) {
        wlr_log(WLR_ERROR, "cannot allocate dde_shell protocol state");
        return false;
    }
    shell->proto = proto;
    wl_list_init(&shell->resources);
    wl_list_init(&shell->surfaces);

    proto->dde_shell = shell;
    proto->server->dde_shell_init_view_hook = dde_shell_init_view;

    shell->global = wl_global_create(proto->server->display,
                                     &treeland_dde_shell_manager_v2_interface,
                                     2, shell, manager_bind);
    if (!shell->global) {
        wlr_log(WLR_ERROR, "failed to create treeland_dde_shell_manager_v2 global");
        proto->server->dde_shell_init_view_hook = NULL;
        free(shell);
        proto->dde_shell = NULL;
        return false;
    }
    wlr_log(WLR_INFO, "treeland_dde_shell_manager_v2 global advertised");
    return true;
}

void woodland_proto_dde_shell_finish(struct woodland_proto *proto)
{
    struct woodland_proto_dde_shell *shell = proto->dde_shell;
    if (!shell) {
        return;
    }
    proto->server->dde_shell_init_view_hook = NULL;

    if (shell->global) {
        wl_global_destroy(shell->global);
        shell->global = NULL;
    }

    struct woodland_dde_shell_surface *surf, *surf_tmp;
    wl_list_for_each_safe(surf, surf_tmp, &shell->surfaces, link) {
        if (surf->resource) {
            wl_resource_destroy(surf->resource);
        } else {
            shell_surface_destroy(surf);
        }
    }

    struct dde_shell_manager_resource *mgr, *mgr_tmp;
    wl_list_for_each_safe(mgr, mgr_tmp, &shell->resources, link) {
        wl_resource_destroy(mgr->resource);
    }

    free(shell);
    proto->dde_shell = NULL;
}

void woodland_protocol_dde_shell_status(struct woodland_server *server,
                                        struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);

    struct woodland_proto_dde_shell *shell =
        (server && server->proto) ? server->proto->dde_shell : NULL;
    if (!shell) {
        woodland_json_obj_bool(&obj, "available", false);
        woodland_json_obj_close(&obj);
        return;
    }

    woodland_json_obj_bool(&obj, "available", true);
    woodland_json_obj_int(&obj, "bindings", (int)wl_list_length(&shell->resources));
    woodland_json_obj_int(&obj, "surfaces", (int)wl_list_length(&shell->surfaces));
    woodland_json_obj_int(&obj, "surface_create_count", (int)shell->surface_create_count);
    woodland_json_obj_int(&obj, "duplicate_error_count", (int)shell->duplicate_error_count);
    woodland_json_obj_int(&obj, "role_change_count", (int)shell->role_change_count);
    woodland_json_obj_int(&obj, "position_hint_count", (int)shell->position_hint_count);
    woodland_json_obj_int(&obj, "cursor_hint_count", (int)shell->cursor_hint_count);
    woodland_json_obj_close(&obj);
}

void woodland_protocol_dde_shell_view_status(const struct woodland_view *view,
                                             struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    if (!view || !view->dde_shell.active) {
        woodland_json_obj_bool(&obj, "active", false);
        woodland_json_obj_close(&obj);
        return;
    }

    woodland_json_obj_bool(&obj, "active", true);
    const char *role_str = "normal";
    if (view->dde_shell.role == WOODLAND_DDE_SHELL_ROLE_OVERLAY) {
        role_str = "overlay";
    } else if (view->dde_shell.role == WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP) {
        role_str = "always_on_top";
    }
    woodland_json_obj_str(&obj, "role", role_str);
    woodland_json_obj_bool(&obj, "has_position_hint", view->dde_shell.has_position_hint);
    if (view->dde_shell.has_position_hint) {
        int pos[2] = {view->dde_shell.position_x, view->dde_shell.position_y};
        woodland_json_obj_key(&obj, "position");
        woodland_json_int_array(out, pos, 2);
    }
    woodland_json_obj_bool(&obj, "has_cursor_placement_hint",
                           view->dde_shell.has_cursor_placement_hint);
    if (view->dde_shell.has_cursor_placement_hint) {
        int offset[2] = {view->dde_shell.cursor_x_offset, view->dde_shell.cursor_y_offset};
        woodland_json_obj_key(&obj, "cursor_offset");
        woodland_json_int_array(out, offset, 2);
    }
    woodland_json_obj_int(&obj, "skip_flags", (int)view->dde_shell.skip_flags);
    woodland_json_obj_bool(&obj, "accept_keyboard_focus", view->dde_shell.accept_keyboard_focus);
    woodland_json_obj_close(&obj);
}

bool woodland_protocol_dde_shell_query_surface_state(struct woodland_server *server,
                                                     struct woodland_dde_shell_surface_state *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    struct woodland_proto_dde_shell *shell =
        (server && server->proto) ? server->proto->dde_shell : NULL;
    if (!shell) {
        return false;
    }

    out->duplicate_errors = (int)shell->duplicate_error_count;

    if (wl_list_empty(&shell->surfaces)) {
        return true;
    }

    struct woodland_dde_shell_surface *surf =
        wl_container_of(shell->surfaces.prev, surf, link);
    out->bound = true;
    out->role = (uint32_t)surf->role;
    out->role_normal = (surf->role == WOODLAND_DDE_SHELL_ROLE_NORMAL);
    out->role_overlay = (surf->role == WOODLAND_DDE_SHELL_ROLE_OVERLAY);
    out->role_always_on_top = (surf->role == WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP);
    out->position_set = surf->has_position_hint;
    out->position_x = surf->position_x;
    out->position_y = surf->position_y;
    out->cursor_set = surf->has_cursor_placement_hint;
    out->cursor_x = surf->cursor_x_offset;
    out->cursor_y = surf->cursor_y_offset;
    out->skip_flags = surf->skip_flags;
    out->accept_keyboard_focus = surf->accept_keyboard_focus;

    struct woodland_view *view = woodland_view_from_surface(surf->wlr_surface);
    if (view && view->mapped) {
        out->wrapper_found = true;
        out->wrapper_z = (int)woodland_view_get_layer(view);
        out->wrapper_always_on_top = view->always_on_top;
    }

    return true;
}
