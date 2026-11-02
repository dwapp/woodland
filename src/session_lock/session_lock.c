// SPDX-License-Identifier: GPL-2.0-only
#include "session_lock.h"

#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "output.h"
#include "seat.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"

static void lock_output_reconfigure(struct woodland_session_lock_output *lock_output);
static void session_lock_output_destroy(struct woodland_session_lock_output *lock_output);

/* ==================== 焦点与路由 ==================== */

static void focus_lock_surface(struct woodland_server *server, struct wlr_surface *surface)
{
    server->session_lock->focused_surface = surface;
    if (surface) {
        if (!wl_list_empty(&server->keyboards)) {
            struct woodland_keyboard *kb = wl_container_of(server->keyboards.next, kb, link);
            wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        }
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
    } else {
        wlr_seat_keyboard_clear_focus(server->seat);
    }
}

static void refocus_lock_surfaces(struct woodland_session_lock *session_lock)
{
    struct woodland_session_lock_output *lo;
    wl_list_for_each(lo, &session_lock->lock_outputs, link)
    {
        if (lo->surface && lo->surface->surface && lo->surface->surface->mapped) {
            focus_lock_surface(session_lock->server, lo->surface->surface);
            return;
        }
    }
    focus_lock_surface(session_lock->server, NULL);
}

/* ==================== surface 事件 ==================== */

static void handle_surface_map(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock_output *lock_output =
        wl_container_of(listener, lock_output, surface_map);
    (void)data;

    /* 新表面映射后，若当前无焦点锁屏表面，则优先聚焦该表面 */
    if (!lock_output->session_lock->focused_surface) {
        focus_lock_surface(lock_output->session_lock->server, lock_output->surface->surface);
    }
    woodland_cursor_process_motion(lock_output->session_lock->server, 0);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock_output *lock_output =
        wl_container_of(listener, lock_output, surface_destroy);
    (void)data;

    bool was_focused =
        (lock_output->session_lock->focused_surface == lock_output->surface->surface);

    wl_list_remove(&lock_output->surface_destroy.link);
    wl_list_remove(&lock_output->surface_map.link);
    lock_output->surface = NULL;
    lock_output->surface_tree = NULL;

    if (was_focused) {
        refocus_lock_surfaces(lock_output->session_lock);
    }
}

/* ==================== output 事件 ==================== */

static void handle_output_commit(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock_output *lock_output =
        wl_container_of(listener, lock_output, output_commit);
    struct wlr_output_event_commit *event = data;

    uint32_t changed = WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM;
    if (event->state->committed & changed) {
        lock_output_reconfigure(lock_output);
    }
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock_output *lock_output =
        wl_container_of(listener, lock_output, output_destroy);
    (void)data;
    session_lock_output_destroy(lock_output);
}

/* ==================== lock_output 管理 ==================== */

static void lock_output_reconfigure(struct woodland_session_lock_output *lock_output)
{
    struct woodland_server *server = lock_output->session_lock->server;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, lock_output->output->wlr_output, &box);

    wlr_scene_node_set_position(&lock_output->tree->node, box.x, box.y);
    wlr_scene_rect_set_size(lock_output->background, box.width, box.height);

    if (lock_output->surface) {
        wlr_session_lock_surface_v1_configure(lock_output->surface, box.width, box.height);
    }
}

static struct woodland_session_lock_output *
session_lock_output_create(struct woodland_session_lock *session_lock,
                           struct woodland_output *output)
{
    struct woodland_session_lock_output *lock_output = calloc(1, sizeof(*lock_output));
    if (!lock_output) {
        return NULL;
    }
    lock_output->session_lock = session_lock;
    lock_output->output = output;

    lock_output->tree = wlr_scene_tree_create(&session_lock->server->scene->tree);
    if (!lock_output->tree) {
        free(lock_output);
        return NULL;
    }
    wlr_scene_node_raise_to_top(&lock_output->tree->node);

    /* 协议规定合成器必须以不透明单色填满屏幕，完全遮挡所有正常内容 */
    float black[4] = {0.f, 0.f, 0.f, 1.f};
    lock_output->background = wlr_scene_rect_create(lock_output->tree, 0, 0, black);
    if (!lock_output->background) {
        wlr_scene_node_destroy(&lock_output->tree->node);
        free(lock_output);
        return NULL;
    }

    woodland_listener_connect(&lock_output->output_destroy, &output->wlr_output->events.destroy,
                              handle_output_destroy, "session_lock.output_destroy");
    woodland_listener_connect(&lock_output->output_commit, &output->wlr_output->events.commit,
                              handle_output_commit, "session_lock.output_commit");

    lock_output_reconfigure(lock_output);
    wl_list_insert(&session_lock->lock_outputs, &lock_output->link);
    return lock_output;
}

static void session_lock_output_destroy(struct woodland_session_lock_output *lock_output)
{
    if (lock_output->surface) {
        bool was_focused =
            (lock_output->session_lock->focused_surface == lock_output->surface->surface);
        wl_list_remove(&lock_output->surface_destroy.link);
        wl_list_remove(&lock_output->surface_map.link);
        lock_output->surface = NULL;
        lock_output->surface_tree = NULL;
        if (was_focused) {
            refocus_lock_surfaces(lock_output->session_lock);
        }
    }
    wl_list_remove(&lock_output->output_destroy.link);
    wl_list_remove(&lock_output->output_commit.link);
    wl_list_remove(&lock_output->link);
    wlr_scene_node_destroy(&lock_output->tree->node);
    free(lock_output);
}

static void session_lock_destroy_outputs(struct woodland_session_lock *session_lock)
{
    struct woodland_session_lock_output *lock_output, *tmp;
    wl_list_for_each_safe(lock_output, tmp, &session_lock->lock_outputs, link)
    {
        session_lock_output_destroy(lock_output);
    }
    session_lock->focused_surface = NULL;
}

/* ==================== lock 协议事件 ==================== */

static void handle_new_surface(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock =
        wl_container_of(listener, session_lock, lock_new_surface);
    struct wlr_session_lock_surface_v1 *surface = data;
    struct woodland_output *output = surface->output->data;

    struct woodland_session_lock_output *lock_output = NULL;
    struct woodland_session_lock_output *lo;
    wl_list_for_each(lo, &session_lock->lock_outputs, link)
    {
        if (lo->output == output) {
            lock_output = lo;
            break;
        }
    }
    if (!lock_output) {
        wlr_log(WLR_ERROR, "new session lock surface on unknown output");
        return;
    }

    lock_output->surface = surface;
    lock_output->surface_tree =
        wlr_scene_subsurface_tree_create(lock_output->tree, surface->surface);
    if (!lock_output->surface_tree) {
        wlr_log(WLR_ERROR, "failed to create session lock scene subsurface tree");
        lock_output->surface = NULL;
        return;
    }

    woodland_listener_connect(&lock_output->surface_destroy, &surface->events.destroy,
                              handle_surface_destroy, "session_lock.surface_destroy");
    woodland_listener_connect(&lock_output->surface_map, &surface->surface->events.map,
                              handle_surface_map, "session_lock.surface_map");

    lock_output_reconfigure(lock_output);
}

static void handle_unlock(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock =
        wl_container_of(listener, session_lock, lock_unlock);
    (void)data;

    session_lock_destroy_outputs(session_lock);
    session_lock->locked = false;

    if (session_lock->lock) {
        wl_list_remove(&session_lock->lock_new_surface.link);
        wl_list_remove(&session_lock->lock_unlock.link);
        wl_list_remove(&session_lock->lock_destroy.link);
        session_lock->lock = NULL;
    }

    struct woodland_server *server = session_lock->server;
    if (session_lock->last_focused_view && session_lock->last_focused_view->mapped) {
        woodland_focus_view(session_lock->last_focused_view, false);
    } else {
        woodland_focus_fallback(server);
    }
    session_lock->last_focused_view = NULL;

    woodland_cursor_process_motion(server, 0);
}

static void handle_lock_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock =
        wl_container_of(listener, session_lock, lock_destroy);
    (void)data;

    /*
     * 规范要求：若客户端未发送 unlock 即销毁 lock 对象（如崩溃/退出），
     * 合成器必须保持锁定状态（locked 依然为 true），并继续维持黑屏遮罩。
     */
    wl_list_remove(&session_lock->lock_new_surface.link);
    wl_list_remove(&session_lock->lock_unlock.link);
    wl_list_remove(&session_lock->lock_destroy.link);
    session_lock->lock = NULL;
}

static void handle_new_lock(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock = wl_container_of(listener, session_lock, new_lock);
    struct wlr_session_lock_v1 *lock = data;

    if (session_lock->lock) {
        wlr_log(WLR_ERROR, "session already locked by another client");
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    if (session_lock->locked) {
        /* 替换此前意外退出的锁屏客户端（abandoned lock） */
        wlr_log(WLR_INFO, "replacing abandoned session lock");
        session_lock_destroy_outputs(session_lock);
    }

    session_lock->last_focused_view = session_lock->server->focused_view;
    wlr_seat_keyboard_clear_focus(session_lock->server->seat);
    wlr_seat_pointer_clear_focus(session_lock->server->seat);
    woodland_cursor_reset_mode(session_lock->server);

    bool output_failed = false;
    struct woodland_output *output;
    wl_list_for_each(output, &session_lock->server->outputs, link)
    {
        if (!session_lock_output_create(session_lock, output)) {
            output_failed = true;
            break;
        }
    }

    if (output_failed) {
        wlr_log(WLR_ERROR, "failed to create session lock output mask, aborting lock");
        session_lock_destroy_outputs(session_lock);
        wlr_session_lock_v1_destroy(lock);
        if (session_lock->last_focused_view) {
            woodland_focus_view(session_lock->last_focused_view, false);
        }
        return;
    }

    woodland_listener_connect(&session_lock->lock_new_surface, &lock->events.new_surface,
                              handle_new_surface, "session_lock.new_surface");
    woodland_listener_connect(&session_lock->lock_unlock, &lock->events.unlock, handle_unlock,
                              "session_lock.unlock");
    woodland_listener_connect(&session_lock->lock_destroy, &lock->events.destroy,
                              handle_lock_destroy, "session_lock.destroy");

    session_lock->locked = true;
    session_lock->lock = lock;
    wlr_session_lock_v1_send_locked(lock);
}

static void handle_manager_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock =
        wl_container_of(listener, session_lock, manager_destroy);
    (void)data;
    woodland_session_lock_finish(session_lock->server);
}
static void handle_layout_change(struct wl_listener *listener, void *data)
{
    struct woodland_session_lock *session_lock =
        wl_container_of(listener, session_lock, layout_change);
    (void)data;
    woodland_session_lock_update_layout(session_lock->server);
}

/* ==================== 对外接口 ==================== */

bool woodland_session_lock_init(struct woodland_server *server)
{
    struct woodland_session_lock *session_lock = calloc(1, sizeof(*session_lock));
    if (!session_lock) {
        return false;
    }
    session_lock->server = server;
    wl_list_init(&session_lock->lock_outputs);

    session_lock->wlr_manager = wlr_session_lock_manager_v1_create(server->display);
    if (!session_lock->wlr_manager) {
        free(session_lock);
        return false;
    }

    woodland_listener_connect(&session_lock->new_lock, &session_lock->wlr_manager->events.new_lock,
                              handle_new_lock, "session_lock.new_lock");
    woodland_listener_connect(&session_lock->manager_destroy,
                              &session_lock->wlr_manager->events.destroy, handle_manager_destroy,
                              "session_lock.manager_destroy");
    woodland_listener_connect(&session_lock->layout_change, &server->output_layout->events.change,
                              handle_layout_change, "session_lock.layout_change");

    server->session_lock = session_lock;
    return true;
}

void woodland_session_lock_finish(struct woodland_server *server)
{
    if (!server || !server->session_lock) {
        return;
    }
    struct woodland_session_lock *session_lock = server->session_lock;

    session_lock_destroy_outputs(session_lock);

    if (session_lock->lock) {
        wl_list_remove(&session_lock->lock_new_surface.link);
        wl_list_remove(&session_lock->lock_unlock.link);
        wl_list_remove(&session_lock->lock_destroy.link);
        session_lock->lock = NULL;
    }

    wl_list_remove(&session_lock->new_lock.link);
    wl_list_remove(&session_lock->manager_destroy.link);
    wl_list_remove(&session_lock->layout_change.link);
    server->session_lock = NULL;
    free(session_lock);
}

bool woodland_session_lock_is_locked(const struct woodland_server *server)
{
    return server && server->session_lock && server->session_lock->locked;
}

void woodland_session_lock_add_output(struct woodland_server *server,
                                      struct woodland_output *output)
{
    if (!woodland_session_lock_is_locked(server)) {
        return;
    }
    if (!session_lock_output_create(server->session_lock, output)) {
        wlr_log(WLR_ERROR, "failed to create session lock output for new output");
    }
}

void woodland_session_lock_update_layout(struct woodland_server *server)
{
    if (!woodland_session_lock_is_locked(server)) {
        return;
    }
    struct woodland_session_lock_output *lo;
    wl_list_for_each(lo, &server->session_lock->lock_outputs, link)
    {
        lock_output_reconfigure(lo);
    }
}

void woodland_session_lock_handle_view_destroy(struct woodland_server *server,
                                               struct woodland_view *view)
{
    if (server && server->session_lock && server->session_lock->last_focused_view == view) {
        server->session_lock->last_focused_view = NULL;
    }
}

bool woodland_session_lock_surface_at(struct woodland_server *server, double lx, double ly,
                                      struct wlr_surface **surface, double *sx, double *sy)
{
    if (!woodland_session_lock_is_locked(server)) {
        return false;
    }
    struct wlr_output *wlr_out = wlr_output_layout_output_at(server->output_layout, lx, ly);
    if (!wlr_out) {
        return false;
    }
    struct woodland_session_lock_output *lo;
    wl_list_for_each(lo, &server->session_lock->lock_outputs, link)
    {
        if (lo->output->wlr_output == wlr_out) {
            if (lo->surface && lo->surface->surface && lo->surface->surface->mapped) {
                struct wlr_box box;
                wlr_output_layout_get_box(server->output_layout, wlr_out, &box);
                double local_x = lx - box.x;
                double local_y = ly - box.y;
                if (wlr_surface_point_accepts_input(lo->surface->surface, local_x, local_y)) {
                    if (surface) {
                        *surface = lo->surface->surface;
                    }
                    if (sx) {
                        *sx = local_x;
                    }
                    if (sy) {
                        *sy = local_y;
                    }
                    return true;
                }
            }
            break;
        }
    }
    return false;
}
