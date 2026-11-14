// SPDX-License-Identifier: GPL-2.0-only
/*
 * 交互式 move/resize：从按下到松手的整段过程。
 *
 * 三块内容其实是一条链：
 *   begin_interactive() 记录抓取点（未贴边的先恢复尺寸，否则最大化的窗口
 *     拖不动）；
 *   process_cursor_move/resize() 逐帧推进几何，移动时顺带算吸附；
 *   释放时 apply_snap() 决定是否铺满。
 *
 * 吸附的**判定**在 placement.c（纯几何、可单测），这里只负责"拖动过程中
 * 累积候选边"和"松手后执行"，所以两处不会各算一套。
 */
#include "util/listener.h"
#include <stdlib.h>

#include "cursor.h"
#include "output.h"
#include "placement.h"
#include "shell_internal.h"
#include "ssd.h"
#include "view.h"
#include "window_menu.h"
#include "woodland.h"
#include "woodland_config.h"
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <xkbcommon/xkbcommon.h>

/* ==================== 吸附候选边 ==================== */

/* 吸附候选边上限：每个视图贡献 2 条边（左右或上下） */
#define WOODLAND_SNAP_MAX_EDGES 64

/*
 * 收集吸附候选线（纯数据，交给 woodland_snap_axis 决策）。
 *
 * x_edges=true 收集竖直边（外框左右边），此时只考虑纵向范围与
 * [span_start, span_start+span_len) 有交叠的视图——否则隔着半屏的窗口
 * 也会把我们的窗口吸过去。
 */
static int collect_snap_edges(struct woodland_server *server, struct woodland_view *exclude,
                              bool x_edges, int span_start, int span_len, int *edges, int max_edges)
{
    int n = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link)
    {
        if (it == exclude || !it->mapped || it->minimized || it->fullscreen ||
            it->workspace != exclude->workspace) {
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
        if (other_start + other_len <= span_start || span_start + span_len <= other_start) {
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

/* ==================== 拖动前恢复尺寸 ==================== */

/*
 * 按光标相对原窗口外框的水平比例锚定新外框，避免硬编码居中导致的跳变（labwc max_move_scale 同款）。
 */
static int max_move_scale(double pos_cursor, double pos_old, double size_old, double size_new)
{
    if (size_old <= 0.0) {
        return (int)(pos_cursor - size_new / 2.0);
    }
    double anchor_frac = (pos_cursor - pos_old) / size_old;
    if (anchor_frac < 0.0) {
        anchor_frac = 0.0;
    } else if (anchor_frac > 1.0) {
        anchor_frac = 1.0;
    }
    return (int)(pos_cursor - anchor_frac * size_new);
}

/*
 * 拖动已最大化/已贴边的窗口：先恢复原尺寸，再把标题栏按比例锚定到光标下方。
 */
static void view_unsnap_for_drag(struct woodland_view *view, double cur_x, double cur_y)
{
    if (!view->maximized && view->snapped_edges == 0) {
        return;
    }

    struct wlr_box restore; /* 内容框 */
    if (view->maximized) {
        restore = view->saved_geometry;
        /* 直接落标志 + 通知客户端，位置稍后统一按光标摆 */
        view->maximized = false;
        shell_view_apply_maximized_state(view, false);
        if (view->toplevel_handle) {
            wlr_foreign_toplevel_handle_v1_set_maximized(view->toplevel_handle, false);
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

    if (restore.width <= 0 || restore.height <= 0) {
        restore.width = 640;
        restore.height = 480;
    }

    struct woodland_ssd_margins m;
    woodland_ssd_get_margins(view, &m);

    /* 获取旧外框，按光标相对旧外框的水平比例锚定新外框，使用传入的精确光标坐标 */
    struct wlr_box old_outer;
    woodland_view_outer_box(view, &old_outer);
    int new_outer_w = restore.width + m.left + m.right;
    int new_outer_h = restore.height + m.top + m.bottom;
    int target_outer_x = max_move_scale(cur_x, old_outer.x, old_outer.width, new_outer_w);
    int target_outer_y = (int)(cur_y - m.top / 2.0) - m.top;

    struct wlr_box new_outer = {
        .x = target_outer_x,
        .y = target_outer_y,
        .width = new_outer_w,
        .height = new_outer_h,
    };
    struct woodland_output *wo = woodland_output_for_view(view);
    if (wo) {
        struct wlr_box area;
        woodland_output_workarea(wo, &area);
        woodland_box_constrain(&new_outer, &area);
    }

    struct wlr_box target = {
        .x = new_outer.x + m.left,
        .y = new_outer.y + m.top,
        .width = restore.width,
        .height = restore.height,
    };
    shell_view_apply_geometry(view, &target);
    shell_view_schedule_configure(view);
}

/* 释放鼠标时把"拖到边上的窗口"铺满（左/右半屏、上边最大化、四角 1/4） */
void shell_view_apply_snap(struct woodland_view *view)
{
    struct woodland_server *server = view->server;
    uint32_t edges = view->pending_snap_edges;
    struct woodland_output *target_wo = NULL;
    if (server) {
        if (server->snap_preview.edges == edges && server->snap_preview.output) {
            target_wo = server->snap_preview.output;
        } else if (server->snap_preview.pending_edges == edges &&
                   server->snap_preview.pending_output) {
            target_wo = server->snap_preview.pending_output;
        }
        woodland_snap_preview_destroy(server);
    }
    view->pending_snap_edges = 0;
    if (woodland_view_snap_to_output(view, edges, target_wo)) {
        shell_view_schedule_configure(view);
    }
}

/* ==================== 抓取开始 ==================== */

void woodland_view_begin_interactive(struct woodland_view *view, enum woodland_cursor_mode mode,
                                     uint32_t edges)
{
    struct woodland_server *server = view->server;
    server->grabbed_view = view;
    server->cursor_mode = mode;

    double cur_x = server->cursor ? server->cursor->x : 0;
    double cur_y = server->cursor ? server->cursor->y : 0;
    int node_x = view->scene_tree ? view->scene_tree->node.x : 0;
    int node_y = view->scene_tree ? view->scene_tree->node.y : 0;

    /* 记录初始几何以便 Escape 恢复 */
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    server->interactive_initial_geo = content;
    server->interactive_initial_geo.x += node_x;
    server->interactive_initial_geo.y += node_y;
    /* 若存在按下时记录的前置抓取上下文且匹配本视图，复用它以消除 RPC 往返延迟带来的漂移 */
    if (server->grab_context.active && server->grab_context.view == view) {
        cur_x = server->grab_context.cursor_x;
        cur_y = server->grab_context.cursor_y;
        node_x = server->grab_context.node_x;
        node_y = server->grab_context.node_y;
        server->grab_context.active = false;
    }

    server->drag_start_x = cur_x;
    server->drag_start_y = cur_y;

    if (mode == WOODLAND_CURSOR_MOVE) {
        /*
         * 若配置将 snap_distance 设为 0，则立即恢复尺寸；
         * 否则延迟到 shell_process_cursor_move 超过防抖阈值才恢复，
         * 避免双击最大化或点击标题栏按钮时的微小抖动误触解绑（labwc unmaximize_threshold 同款）。
         */
        int unmax_threshold =
            server->config ? server->config->general.snap_distance : WOODLAND_SNAP_DISTANCE;
        if (unmax_threshold <= 0) {
            view_unsnap_for_drag(view, cur_x, cur_y);
            if (view->scene_tree) {
                node_x = view->scene_tree->node.x;
                node_y = view->scene_tree->node.y;
            }
        }
        server->grab_x = cur_x - node_x;
        server->grab_y = cur_y - node_y;
        woodland_cursor_set(server, "grabbing");
    } else {
        /* 手动缩放即放弃贴边状态（尺寸由用户接管） */
        view->snapped_edges = 0;
        view->pending_snap_edges = 0;
        struct wlr_box geo_box;
        woodland_view_content_box(view, &geo_box);
        double border_x =
            (node_x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (node_y + geo_box.y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = cur_x - border_x;
        server->grab_y = cur_y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += node_x;
        server->grab_geobox.y += node_y;

        server->resize_edges = edges;
        woodland_cursor_set_resize(server, edges);
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

/* ==================== 拖动过程 ==================== */

/*
 * 拖动移动：外框跟随光标，靠近工作区边缘或其它窗口边缘时吸附，
 * 并记录"松手即铺满"的边。
 *
 * 先按 x 吸附（用当前的纵向跨度判断邻窗是否并排），再按吸附后的 x
 * 算 y，避免两次吸附互相打脸。吸附基准是**窗口所在输出**的工作区，
 * 多输出下光标在副屏时不会被主屏的边吸走。
 */
void shell_process_cursor_move(struct woodland_server *server)
{
    struct woodland_view *view = server->grabbed_view;
    if (!view) {
        return;
    }

    /* 拖动已最大化/已贴边的窗口：在防抖阈值内保持原状态，超过阈值才真正恢复（labwc unmaximize_threshold 同款） */
    if (view->maximized || view->snapped_edges != 0) {
        double dx = server->cursor->x - server->drag_start_x;
        double dy = server->cursor->y - server->drag_start_y;
        int unmax_threshold =
            server->config ? server->config->general.snap_distance : WOODLAND_SNAP_DISTANCE;
        if (unmax_threshold <= 0) {
            unmax_threshold = WOODLAND_SNAP_DISTANCE;
        }
        if ((dx < 0 ? -dx : dx) < unmax_threshold && (dy < 0 ? -dy : dy) < unmax_threshold) {
            return;
        }
        view_unsnap_for_drag(view, server->cursor->x, server->cursor->y);
        if (view->scene_tree) {
            server->grab_x = server->cursor->x - view->scene_tree->node.x;
            server->grab_y = server->cursor->y - view->scene_tree->node.y;
        }
    }

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
        struct woodland_output *wo =
            woodland_output_at(server, server->cursor->x, server->cursor->y);
        if (!wo) {
            wo = woodland_output_for_view(view);
        }
        struct wlr_box area;
        woodland_output_workarea(wo, &area);

        /* 吸附阈值和"真的拖动了"用同一个数：配置给了 snap_distance 就以它
         * 为准，否则退回内置默认。两处用不同值会出现"吸过去了但不算拖动" */
        int snap_threshold =
            server->config ? server->config->general.snap_distance : WOODLAND_SNAP_DISTANCE;

        int cur_x = (view->scene_tree ? (int)view->scene_tree->node.x : (x + margins.left)) - margins.left;
        int cur_y = (view->scene_tree ? (int)view->scene_tree->node.y : (y + margins.top)) - margins.top;

        int edges[WOODLAND_SNAP_MAX_EDGES];
        int n = collect_snap_edges(server, view, true, y, outer_h, edges, WOODLAND_SNAP_MAX_EDGES);
        x = woodland_snap_axis_motion(cur_x, x, outer_w, area.x, area.x + area.width, edges, n, snap_threshold);

        n = collect_snap_edges(server, view, false, x, outer_w, edges, WOODLAND_SNAP_MAX_EDGES);
        y = woodland_snap_axis_motion(cur_y, y, outer_h, area.y, area.y + area.height, edges, n, snap_threshold);

        /*
         * 贴边铺满的判定：只有真的拖动了（超过阈值距离）才算，
         * 否则"按住标题栏不动就松手"会把刚开在角落的窗口变成半屏。
         * 判定只看光标是否撞到工作区边，窗口外框贴不贴边不参与。
         */
        double dx = server->cursor->x - server->drag_start_x;
        double dy = server->cursor->y - server->drag_start_y;
        if ((dx < 0 ? -dx : dx) > snap_threshold || (dy < 0 ? -dy : dy) > snap_threshold) {
            view->pending_snap_edges = woodland_snap_determine_edges(
                view, &area, server->cursor->x, server->cursor->y, snap_threshold);
        }
    }

    wlr_scene_node_set_position(&view->scene_tree->node, x + margins.left, y + margins.top);
    woodland_view_update_outputs(view);
    woodland_snap_preview_update(server, view);
}

void shell_process_cursor_resize(struct woodland_server *server)
{
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
    wlr_scene_node_set_position(&view->scene_tree->node, new_left - geo_box.x, new_top - geo_box.y);

    woodland_view_request_size(view, new_right - new_left, new_bottom - new_top);
    woodland_view_update_outputs(view);
}

void woodland_cursor_reset_mode(struct woodland_server *server)
{
    woodland_snap_preview_destroy(server);
    woodland_window_menu_hide(server);
    server->cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
    server->resize_edges = 0;
    server->interactive_click_to_finish = false;
    woodland_cursor_set(server, "default");
    server->grab_context.active = false;
    server->grab_context.view = NULL;
}

void shell_interactive_attach(struct woodland_view *view)
{
    woodland_listener_connect(&view->request_move, &view->xdg_toplevel->events.request_move,
                              xdg_toplevel_request_move, "interactive.request_move");
    woodland_listener_connect(&view->request_resize, &view->xdg_toplevel->events.request_resize,
                              xdg_toplevel_request_resize, "interactive.request_resize");
}

bool woodland_interactive_handle_key(struct woodland_server *server, xkb_keysym_t sym,
                                     uint32_t modifiers)
{
    if (!server || (server->cursor_mode != WOODLAND_CURSOR_MOVE &&
                    server->cursor_mode != WOODLAND_CURSOR_RESIZE)) {
        return false;
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        if (server->cursor_mode == WOODLAND_CURSOR_MOVE && server->grabbed_view) {
            shell_view_apply_snap(server->grabbed_view);
        }
        woodland_cursor_reset_mode(server);
        return true;
    }

    if (sym == XKB_KEY_Escape) {
        if (server->grabbed_view && server->interactive_initial_geo.width > 0) {
            shell_view_apply_geometry(server->grabbed_view, &server->interactive_initial_geo);
        }
        woodland_cursor_reset_mode(server);
        return true;
    }

    int step = (modifiers & WLR_MODIFIER_SHIFT) ? 50 : 10;
    int dx = (sym == XKB_KEY_Right) ? step : ((sym == XKB_KEY_Left) ? -step : 0);
    int dy = (sym == XKB_KEY_Down) ? step : ((sym == XKB_KEY_Up) ? -step : 0);

    if (dx != 0 || dy != 0) {
        if (server->cursor) {
            wlr_cursor_move(server->cursor, NULL, dx, dy);
            if (server->cursor_mode == WOODLAND_CURSOR_MOVE) {
                shell_process_cursor_move(server);
            } else if (server->cursor_mode == WOODLAND_CURSOR_RESIZE) {
                shell_process_cursor_resize(server);
            }
        }
        return true;
    }

    return true;
}
