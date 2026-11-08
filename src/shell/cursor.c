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
#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include "util/listener.h"
#include <wlr/types/wlr_cursor.h>
#include "scene.h"
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>

#include "cursor.h"
#include "dnd.h"
#include "focus.h"
#include "layer.h"
#include "session_lock.h"
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
static void ssd_update_hover(struct woodland_server *server, struct woodland_view *view,
                             enum woodland_ssd_part part)
{
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
static void ssd_clear_pressed(struct woodland_server *server)
{
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
static void ssd_button_release(struct woodland_server *server)
{
    struct woodland_view *view = server->pressed_view;
    enum woodland_ssd_part part = server->pressed_part;
    ssd_clear_pressed(server);

    if (!view || !view->mapped) {
        return;
    }
    uint32_t edges = 0;
    if (woodland_ssd_at(view, server->cursor->x, server->cursor->y, &edges) != part) {
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

void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time)
{
    if (woodland_session_lock_is_locked(server)) {
        if (server->cursor_mode != WOODLAND_CURSOR_PASSTHROUGH) {
            woodland_cursor_reset_mode(server);
        }
        double sx = 0, sy = 0;
        struct wlr_surface *surface = NULL;
        woodland_session_lock_surface_at(server, server->cursor->x, server->cursor->y, &surface,
                                         &sx, &sy);
        if (surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
        } else {
            wlr_seat_pointer_clear_focus(server->seat);
            woodland_cursor_set(server, "default");
        }
        return;
    }

    /*
     * 拖拽优先于一切光标策略：图标要跟手、投放目标每帧都要重算，而 SSD
     * hover / 贴边吸附 / 光标形状在拖拽期间全都不该跑（拖拽不是隐式抓取，
     * 也不能把客户端设的拖拽光标顶掉）。
     *
     * 必须在 cursor_mode 分支之前：窗口拖动/缩放的 grab 与拖拽互斥，真同时
     * 出现时以拖拽为准（它有 grab，按 grab 的状态走）。
     */
    if (woodland_dnd_active(server)) {
        woodland_dnd_motion(server, time);
        return;
    }

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
    struct woodland_view *view =
        woodland_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    /* 未命中 surface 时，检查是否悬停在某视图的 SSD 区域（标题栏/边框） */
    enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
    uint32_t edges = 0;
    struct woodland_view *ssd_view = NULL;
    if (!view) {
        struct woodland_view *v;
        wl_list_for_each(v, &server->views, link)
        {
            enum woodland_ssd_part part =
                woodland_ssd_at(v, server->cursor->x, server->cursor->y, &edges);
            if (part != WOODLAND_SSD_NONE) {
                ssd_part = part;
                ssd_view = v;
                break;
            }
        }
    }

    if (ssd_view) {
        /* 悬停 SSD：按区域切换光标，不给 surface 指针焦点 */
        if (ssd_part == WOODLAND_SSD_TITLEBAR) {
            woodland_cursor_set(server, "default");
        } else if (ssd_part >= WOODLAND_SSD_BORDER_LEFT && ssd_part <= WOODLAND_SSD_BORDER_BOTTOM) {
            uint32_t resize_edges = edges;
            int w = 0, h = 0;
            if (woodland_ssd_content_size(ssd_view, &w, &h) && w > 0 && h > 0) {
                double rx = server->cursor->x - ssd_view->scene_tree->node.x;
                double ry = server->cursor->y - ssd_view->scene_tree->node.y;
                int corner_dist = 16;
                if (rx <= corner_dist) {
                    resize_edges |= WLR_EDGE_LEFT;
                } else if (rx >= w - corner_dist) {
                    resize_edges |= WLR_EDGE_RIGHT;
                }
                if (ry <= corner_dist) {
                    resize_edges |= WLR_EDGE_TOP;
                } else if (ry >= h - corner_dist) {
                    resize_edges |= WLR_EDGE_BOTTOM;
                }
            }
            woodland_cursor_set_resize(server, resize_edges);
        } else {
            woodland_cursor_set(server, "default");
        }
        /*
         * 必须用 notify_ 版本：裸 wlr_seat_pointer_clear_focus() 绕过 pointer
         * grab。平时没差别（默认 grab 的实现就是它），但拖拽期间 grab 是
         * wlr_drag 的——绕过它，drag 收不到 leave，表现是"在标题栏上松手，
         * 文件掉进了上一个窗口"。
         */
        wlr_seat_pointer_notify_clear_focus(seat);
        /* 只有按钮才亮；标题栏/边框传 NONE（顺带清掉上一个按钮的高亮） */
        bool on_button = ssd_part == WOODLAND_SSD_BUTTON_CLOSE ||
                         ssd_part == WOODLAND_SSD_BUTTON_MAXIMIZE ||
                         ssd_part == WOODLAND_SSD_BUTTON_MINIMIZE;
        ssd_update_hover(server, on_button ? ssd_view : NULL,
                         on_button ? ssd_part : WOODLAND_SSD_NONE);
        return;
    }

    /* 离开 SSD 区域（含移到客户端内容上）：清掉按钮高亮 */
    ssd_update_hover(server, NULL, WOODLAND_SSD_NONE);

    if (!view) {
        woodland_cursor_set(server, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        /* 同上：走 grab 才不会被拖拽绕过 */
        wlr_seat_pointer_notify_clear_focus(seat);
    }
}

/* ==================== wlr_cursor 监听器 ==================== */

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
    /* 手动计数的示例：已经在改这个回调、又想知道"它跑了吗/跑了多少次"
     * 时，开头插这一行就够。想零回调侵入见 connect_verbose（output.frame） */
    woodland_listener_count(listener);
    struct woodland_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    if (server->current_constraint &&
        server->current_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
        server->seat->pointer_state.focused_surface == server->current_constraint->surface) {
        if (woodland_pointer_constraint_is_focused(server, server->current_constraint)) {
            return;
        }
    }

    double dx = event->delta_x;
    double dy = event->delta_y;
    if (server->current_constraint &&
        server->current_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
        woodland_pointer_constraint_apply(server, &dx, &dy);
    }

    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    woodland_cursor_process_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    if (server->current_constraint &&
        server->current_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
        server->seat->pointer_state.focused_surface == server->current_constraint->surface) {
        if (woodland_pointer_constraint_is_focused(server, server->current_constraint)) {
            return;
        }
    }
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    woodland_cursor_process_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    /* 一次性快捷键捕获：pointer button 以 interrupted 终止它，事件被消费 */
    if (server->shortcut_pointer_hook && server->shortcut_pointer_hook(server)) {
        return;
    }
    if (woodland_session_lock_is_locked(server)) {
        double sx = 0, sy = 0;
        struct wlr_surface *surface = NULL;
        woodland_session_lock_surface_at(server, server->cursor->x, server->cursor->y, &surface,
                                         &sx, &sy);
        if (surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
                                           event->state);
        } else {
            wlr_seat_pointer_clear_focus(server->seat);
        }
        return;
    }

    /*
     * 拖拽期间按钮只做一件事：让 grab 收尾（drop 就在 notify_button 里发生，
     * 随后 drag 自己销毁）。顺序不能反——labwc #1657 的"release 发给了
     * layer client"就是先做收尾再下发造成的。
     *
     * 也不能因为"光标下没有 surface"就跳过下发：桌面上松手一样要结束拖拽
     * （labwc #1673）。
     */
    bool dragging = woodland_dnd_active(server);
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (dragging) {
        if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
            /*
             * 拖拽期间 hover / pressed 不会更新（motion 早就 return 了），
             * 所以"按下启动拖拽"那一下留下的高亮要在这里统一清掉。
             */
            ssd_clear_pressed(server);
            ssd_update_hover(server, NULL, WOODLAND_SSD_NONE);
        }
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->grab_context.active = false;
        server->grab_context.view = NULL;
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
        struct woodland_view *view =
            woodland_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        if (view) {
            woodland_focus_view(view, true);
            server->grab_context.view = view;
            server->grab_context.cursor_x = server->cursor->x;
            server->grab_context.cursor_y = server->cursor->y;
            server->grab_context.node_x = view->scene_tree ? view->scene_tree->node.x : 0;
            server->grab_context.node_y = view->scene_tree ? view->scene_tree->node.y : 0;
            server->grab_context.active = true;
        } else {
#if WOODLAND_HAVE_XWAYLAND
            if (surface) {
                struct wlr_xwayland_surface *xsurface =
                    wlr_xwayland_surface_try_from_wlr_surface(surface);
                if (xsurface && wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
                    woodland_seat_focus_surface(server, surface);
                    return;
                }
            }
#endif
            /* 未命中 surface：先查 SSD 区域（按钮/标题栏/边框） */
            uint32_t edges = 0;
            struct woodland_view *ssd_view = NULL;
            enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
            struct woodland_view *v;
            wl_list_for_each(v, &server->views, link)
            {
                enum woodland_ssd_part part =
                    woodland_ssd_at(v, server->cursor->x, server->cursor->y, &edges);
                if (part != WOODLAND_SSD_NONE) {
                    ssd_part = part;
                    ssd_view = v;
                    break;
                }
            }
            if (ssd_view) {
                woodland_focus_view(ssd_view, true);
                server->grab_context.view = ssd_view;
                server->grab_context.cursor_x = server->cursor->x;
                server->grab_context.cursor_y = server->cursor->y;
                server->grab_context.node_x = ssd_view->scene_tree ? ssd_view->scene_tree->node.x : 0;
                server->grab_context.node_y = ssd_view->scene_tree ? ssd_view->scene_tree->node.y : 0;
                server->grab_context.active = true;
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
                case WOODLAND_SSD_BORDER_BOTTOM: {
                    uint32_t resize_edges = edges;
                    int w = 0, h = 0;
                    if (woodland_ssd_content_size(ssd_view, &w, &h) && w > 0 && h > 0) {
                        double rx = server->cursor->x - ssd_view->scene_tree->node.x;
                        double ry = server->cursor->y - ssd_view->scene_tree->node.y;
                        int corner_dist = 16;
                        if (rx <= corner_dist) {
                            resize_edges |= WLR_EDGE_LEFT;
                        } else if (rx >= w - corner_dist) {
                            resize_edges |= WLR_EDGE_RIGHT;
                        }
                        if (ry <= corner_dist) {
                            resize_edges |= WLR_EDGE_TOP;
                        } else if (ry >= h - corner_dist) {
                            resize_edges |= WLR_EDGE_BOTTOM;
                        }
                    }
                    woodland_view_begin_interactive(ssd_view, WOODLAND_CURSOR_RESIZE, resize_edges);
                    break;
                }
                default:
                    break;
                }
            } else {
                /* 点中 layer surface 时按 interactivity 给焦点 */
                struct wlr_surface *lsurf = NULL;
                double lsx, lsy;
                struct wlr_layer_surface_v1 *layer = woodland_layer_surface_at(
                    server, server->cursor->x, server->cursor->y, &lsurf, &lsx, &lsy);
                if (layer) {
                    woodland_layer_try_focus(server, layer);
                }
            }
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    /* 与 button 一样：wheel 也终止一次性捕获，且不转发给客户端 */
    if (server->shortcut_pointer_hook && server->shortcut_pointer_hook(server)) {
        return;
    }
    if (woodland_session_lock_is_locked(server)) {
        double sx = 0, sy = 0;
        struct wlr_surface *surface = NULL;
        woodland_session_lock_surface_at(server, server->cursor->x, server->cursor->y, &surface,
                                         &sx, &sy);
        if (surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
                                         event->delta, event->delta_discrete, event->source,
                                         event->relative_direction);
        } else {
            wlr_seat_pointer_clear_focus(server->seat);
        }
        return;
    }

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
    (void)data;
}

void woodland_cursor_init_events(struct woodland_server *server)
{
    woodland_listener_connect(&server->cursor_motion, &server->cursor->events.motion,
                              server_cursor_motion, "cursor.motion");
    woodland_listener_connect(&server->cursor_motion_absolute,
                              &server->cursor->events.motion_absolute,
                              server_cursor_motion_absolute, "cursor.motion_absolute");
    woodland_listener_connect(&server->cursor_button, &server->cursor->events.button,
                              server_cursor_button, "cursor.button");
    woodland_listener_connect(&server->cursor_axis, &server->cursor->events.axis,
                              server_cursor_axis, "cursor.axis");
    woodland_listener_connect(&server->cursor_frame, &server->cursor->events.frame,
                              server_cursor_frame, "cursor.frame");
}

void woodland_cursor_finish_events(struct woodland_server *server)
{
    if (server->cursor) {
        wl_list_remove(&server->cursor_motion.link);
        wl_list_remove(&server->cursor_motion_absolute.link);
        wl_list_remove(&server->cursor_button.link);
        wl_list_remove(&server->cursor_axis.link);
        wl_list_remove(&server->cursor_frame.link);
    }
}
