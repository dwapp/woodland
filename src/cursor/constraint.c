// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "cursor.h"
#include "woodland.h"

static void warp_cursor_to_constraint_hint(struct woodland_server *server,
                                           struct wlr_pointer_constraint_v1 *constraint)
{
    if (!constraint || !(constraint->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT)) {
        return;
    }
    struct wlr_surface *surface = constraint->surface;
    if (!surface || !server->seat ||
        server->seat->pointer_state.focused_surface != surface ||
        !woodland_pointer_constraint_is_focused(server, constraint)) {
        return;
    }

    double surface_lx = server->cursor->x - server->seat->pointer_state.sx;
    double surface_ly = server->cursor->y - server->seat->pointer_state.sy;
    double hint_x = constraint->current.cursor_hint.x;
    double hint_y = constraint->current.cursor_hint.y;

    wlr_cursor_warp(server->cursor, NULL, surface_lx + hint_x, surface_ly + hint_y);
    wlr_seat_pointer_warp(server->seat, hint_x, hint_y);
}

static void handle_constraint_commit(struct wl_listener *listener, void *data)
{
    struct woodland_server *server =
        wl_container_of(listener, server, constraint_commit);
    (void)data;
    (void)server;
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_server *server =
        wl_container_of(listener, server, constraint_destroy);
    struct wlr_pointer_constraint_v1 *constraint = data;

    if (server->current_constraint == constraint) {
        warp_cursor_to_constraint_hint(server, constraint);
        if (server->constraint_commit.link.prev) {
            wl_list_remove(&server->constraint_commit.link);
            server->constraint_commit.link.prev = NULL;
        }
        if (server->constraint_destroy.link.prev) {
            wl_list_remove(&server->constraint_destroy.link);
            server->constraint_destroy.link.prev = NULL;
        }
        server->current_constraint = NULL;
    }
}

struct wl_client *woodland_pointer_constraint_get_client(
        const struct wlr_pointer_constraint_v1 *constraint)
{
    if (!constraint) {
        return NULL;
    }
    /* 约束的 resource 一定在；surface 的 resource 只是兜底（销毁中途 resource
     * 可能已经被置空，但那之后也不该有人再问这个问题） */
    if (constraint->resource) {
        return wl_resource_get_client(constraint->resource);
    }
    if (constraint->surface && constraint->surface->resource) {
        return wl_resource_get_client(constraint->surface->resource);
    }
    return NULL;
}

bool woodland_pointer_constraint_matches_keyboard_focus(struct woodland_server *server,
                                                        struct wl_client *client)
{
    if (!server || !server->seat || !client) {
        return false;
    }
    struct wlr_surface *kb_surface = server->seat->keyboard_state.focused_surface;
    if (!kb_surface || !kb_surface->resource) {
        return false;
    }
    return wl_resource_get_client(kb_surface->resource) == client;
}

bool woodland_pointer_constraint_is_focused(struct woodland_server *server,
                                            struct wlr_pointer_constraint_v1 *constraint)
{
    if (!server || !constraint || !constraint->surface) {
        return false;
    }
    if (server->pointer_constraint_is_focused_hook) {
        return server->pointer_constraint_is_focused_hook(server, constraint);
    }

    /* 没有 shell 钩子（例如只链 cursor_lib 的单测）时的兜底：只认键盘焦点表面。
     * 同一 surface 用指针相等判，因为此时 resource 未必还在（假 surface 没 resource） */
    if (!server->seat) {
        return false;
    }
    if (server->seat->keyboard_state.focused_surface == constraint->surface) {
        return true;
    }
    return woodland_pointer_constraint_matches_keyboard_focus(
        server, woodland_pointer_constraint_get_client(constraint));
}

void woodland_pointer_constraint_set(struct woodland_server *server,
                                     struct wlr_pointer_constraint_v1 *constraint)
{
    if (constraint && !woodland_pointer_constraint_is_focused(server, constraint)) {
        constraint = NULL;
    }

    if (server->current_constraint == constraint) {
        return;
    }

    if (server->current_constraint) {
        struct wlr_pointer_constraint_v1 *old = server->current_constraint;
        server->current_constraint = NULL;
        if (!constraint) {
            warp_cursor_to_constraint_hint(server, old);
        }
        if (server->constraint_destroy.link.prev) {
            wl_list_remove(&server->constraint_destroy.link);
            server->constraint_destroy.link.prev = NULL;
        }
        if (server->constraint_commit.link.prev) {
            wl_list_remove(&server->constraint_commit.link);
            server->constraint_commit.link.prev = NULL;
        }
        if (old->resource) {
            wlr_pointer_constraint_v1_send_deactivated(old);
        }
    }

    server->current_constraint = constraint;

    if (!constraint) {
        return;
    }

    if (constraint->resource) {
        wlr_pointer_constraint_v1_send_activated(constraint);
    }

    woodland_listener_connect(&server->constraint_destroy,
                              &constraint->events.destroy,
                              handle_constraint_destroy,
                              "pointer_constraint.destroy");

    woodland_listener_connect(&server->constraint_commit,
                              &constraint->surface->events.commit,
                              handle_constraint_commit,
                              "pointer_constraint.commit");
}

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data)
{
    struct woodland_server *server =
        wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    if (server->seat &&
        server->seat->pointer_state.focused_surface == constraint->surface &&
        woodland_pointer_constraint_is_focused(server, constraint)) {
        woodland_pointer_constraint_set(server, constraint);
    }
}

void woodland_pointer_constraint_recheck(struct woodland_server *server,
                                         struct wlr_surface *surface)
{
    if (!server->pointer_constraints) {
        return;
    }
    struct wlr_pointer_constraint_v1 *constraint = NULL;
    if (surface && server->seat) {
        constraint = wlr_pointer_constraints_v1_constraint_for_surface(
            server->pointer_constraints, surface, server->seat);
    }
    woodland_pointer_constraint_set(server, constraint);
}

void woodland_pointer_constraint_update(struct woodland_server *server)
{
    if (!server || !server->seat) {
        return;
    }
    /* 约束挂在"指针焦点表面"上；键盘焦点只决定它有效与否，不改变它挂在哪。
     * 所以这里就是 recheck 的一次转发——别在这儿写第二份 surface 查询。 */
    woodland_pointer_constraint_recheck(server,
                                        server->seat->pointer_state.focused_surface);
}

void woodland_pointer_constraint_apply(struct woodland_server *server,
                                       double *dx, double *dy)
{
    struct wlr_pointer_constraint_v1 *constraint = server->current_constraint;
    if (!constraint || constraint->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) {
        return;
    }
    struct wlr_surface *surface = constraint->surface;
    if (!surface || !server->seat ||
        server->seat->pointer_state.focused_surface != surface ||
        !woodland_pointer_constraint_is_focused(server, constraint)) {
        return;
    }

    double sx = server->seat->pointer_state.sx;
    double sy = server->seat->pointer_state.sy;
    double target_sx = sx + *dx;
    double target_sy = sy + *dy;
    double confined_sx = sx;
    double confined_sy = sy;

    if (!wlr_region_confine(&constraint->region, sx, sy, target_sx, target_sy,
                            &confined_sx, &confined_sy)) {
        *dx = 0;
        *dy = 0;
        return;
    }

    *dx = confined_sx - sx;
    *dy = confined_sy - sy;
}

bool woodland_pointer_constraints_init(struct woodland_server *server)
{
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
    if (!server->pointer_constraints) {
        wlr_log(WLR_ERROR, "failed to create pointer constraints manager");
        return false;
    }

    woodland_listener_connect(&server->new_pointer_constraint,
                              &server->pointer_constraints->events.new_constraint,
                              handle_new_pointer_constraint,
                              "pointer_constraint.new");
    return true;
}

void woodland_pointer_constraints_finish(struct woodland_server *server)
{
    if (server->current_constraint) {
        woodland_pointer_constraint_set(server, NULL);
    }
    if (server->new_pointer_constraint.link.prev) {
        wl_list_remove(&server->new_pointer_constraint.link);
        server->new_pointer_constraint.link.prev = NULL;
    }
    server->pointer_constraints = NULL;
}
