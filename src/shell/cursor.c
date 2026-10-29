// SPDX-License-Identifier: GPL-2.0-only
/*
 * 光标事件：从 wlr_cursor 进来的 motion / button / axis / frame，以及指针
 * 停在 SSD 上时的 hover/pressed 状态。
 *
 * 这一层只做"分发"——motion 按 cursor_mode 交给 interactive.c，按下按命中
 * 对象分给标题栏拖动、边框缩放、SSD 按钮或 layer surface，自己不改任何
 * 窗口几何。SSD 按钮的 hover/pressed 是纯指针状态，跟着这里走，因为它
 * 唯一的更新时机就是 motion 与 button。
 */
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "focus.h"
#include "layer.h"
#include "shell_internal.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"

/* ==================== SSD 按钮 hover / pressed ==================== */

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

/* ==================== motion 分发 ==================== */

void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time) {
    if (server->cursor_mode == WOODLAND_CURSOR_MOVE) {
        shell_process_cursor_move(server);
        return;
    } else if (server->cursor_mode == WOODLAND_CURSOR_RESIZE) {
        shell_process_cursor_resize(server);
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

/* ==================== wlr_cursor 监听器 ==================== */

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
        /* 拖动结束：先决定是否贴边铺满，再清抓取状态（铺满要用 grabbed_view） */
        if (server->cursor_mode == WOODLAND_CURSOR_MOVE && server->grabbed_view) {
            shell_view_apply_snap(server->grabbed_view);
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
