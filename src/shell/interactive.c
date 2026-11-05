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
#include "woodland.h"
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

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
 * 拖动已最大化/已贴边的窗口：先恢复原尺寸，再把标题栏摆到光标下方。
 *
 * 直接把窗口平移到光标处是错的——最大化窗口一旦被拖，用户期望它缩回
 * 原来的大小并"挂在"光标上（labwc / 多数桌面的行为）。
 */
static void view_unsnap_for_drag(struct woodland_view *view)
{
    if (!view->maximized && view->snapped_edges == 0) {
        return;
    }
    struct woodland_server *server = view->server;

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

    struct woodland_ssd_margins m;
    woodland_ssd_get_margins(view, &m);
    struct wlr_box target = {
        .x = (int)(server->cursor->x - restore.width / 2.0),
        .y = (int)(server->cursor->y - m.top / 2.0),
        .width = restore.width,
        .height = restore.height,
    };
    shell_view_apply_geometry(view, &target);
    woodland_view_constrain(view);
    shell_view_schedule_configure(view);
}

/* 释放鼠标时把"拖到边上的窗口"铺满（左/右半屏、上边最大化、四角 1/4） */
void shell_view_apply_snap(struct woodland_view *view)
{
    uint32_t edges = view->pending_snap_edges;
    view->pending_snap_edges = 0;
    if (woodland_view_snap_to(view, edges)) {
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
    server->drag_start_x = server->cursor->x;
    server->drag_start_y = server->cursor->y;

    if (mode == WOODLAND_CURSOR_MOVE) {
        /* 拖动最大化/贴边的窗口：先恢复原尺寸再跟随光标（labwc 同款），
         * 否则窗口只能被整体平移，用户没法把它"拉下来" */
        view_unsnap_for_drag(view);
        server->grab_x = server->cursor->x - view->scene_tree->node.x;
        server->grab_y = server->cursor->y - view->scene_tree->node.y;
        woodland_cursor_set(server, "grabbing");
    } else {
        /* 手动缩放即放弃贴边状态（尺寸由用户接管） */
        view->snapped_edges = 0;
        view->pending_snap_edges = 0;
        struct wlr_box geo_box;
        woodland_view_content_box(view, &geo_box);
        double border_x =
            (view->scene_tree->node.x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (view->scene_tree->node.y + geo_box.y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += view->scene_tree->node.x;
        server->grab_geobox.y += view->scene_tree->node.y;

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

        int edges[WOODLAND_SNAP_MAX_EDGES];
        int n = collect_snap_edges(server, view, true, y, outer_h, edges, WOODLAND_SNAP_MAX_EDGES);
        x = woodland_snap_axis(x, outer_w, area.x, area.x + area.width, edges, n,
                               WOODLAND_SNAP_DISTANCE);

        n = collect_snap_edges(server, view, false, x, outer_w, edges, WOODLAND_SNAP_MAX_EDGES);
        y = woodland_snap_axis(y, outer_h, area.y, area.y + area.height, edges, n,
                               WOODLAND_SNAP_DISTANCE);

        /*
         * 贴边铺满的判定：只有真的拖动了（超过阈值距离）才算，
         * 否则"按住标题栏不动就松手"会把刚开在角落的窗口变成半屏。
         */
        double dx = server->cursor->x - server->drag_start_x;
        double dy = server->cursor->y - server->drag_start_y;
        if ((dx < 0 ? -dx : dx) > WOODLAND_SNAP_DISTANCE ||
            (dy < 0 ? -dy : dy) > WOODLAND_SNAP_DISTANCE) {
            struct wlr_box outer = {x, y, outer_w, outer_h};
            view->pending_snap_edges =
                woodland_snap_edges_for(&outer, &area, WOODLAND_SNAP_DISTANCE);
        }
    }

    wlr_scene_node_set_position(&view->scene_tree->node, x + margins.left, y + margins.top);
    woodland_view_update_outputs(view);
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
    server->cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
    server->resize_edges = 0;
    woodland_cursor_set(server, "default");
}

void shell_interactive_attach(struct woodland_view *view)
{
    woodland_listener_connect(&view->request_move, &view->xdg_toplevel->events.request_move,
                              xdg_toplevel_request_move, "interactive.request_move");
    woodland_listener_connect(&view->request_resize, &view->xdg_toplevel->events.request_resize,
                              xdg_toplevel_request_resize, "interactive.request_resize");
}
