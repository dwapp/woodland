// SPDX-License-Identifier: GPL-2.0-only
/*
 * 位置管理：输出查找、放置策略、约束、贴边铺满、尺寸限制。
 *
 * 这里的几何函数全部是纯函数（入参出参都是 wlr_box），只有
 * woodland_view_place_initial() / woodland_view_constrain() 会写 scene 树，
 * 这样单测可以在不起合成器的情况下断言摆放结果。
 */
#include "config.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output_layout.h>
#include "scene.h"
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#endif

#include "output.h"
#include "placement.h"
#include "ssd.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"

/* ==================== 策略名 ==================== */

const char *woodland_placement_policy_name(enum woodland_placement_policy policy)
{
    switch (policy) {
    case WOODLAND_PLACE_CENTER:
        return "center";
    case WOODLAND_PLACE_CURSOR:
        return "cursor";
    case WOODLAND_PLACE_AUTOPLACE:
        return "autoplace";
    case WOODLAND_PLACE_CASCADE:
    default:
        return "cascade";
    }
}

bool woodland_placement_policy_parse(const char *name, enum woodland_placement_policy *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "cascade") == 0) {
        *out = WOODLAND_PLACE_CASCADE;
    } else if (strcmp(name, "center") == 0) {
        *out = WOODLAND_PLACE_CENTER;
    } else if (strcmp(name, "cursor") == 0) {
        *out = WOODLAND_PLACE_CURSOR;
    } else if (strcmp(name, "autoplace") == 0) {
        *out = WOODLAND_PLACE_AUTOPLACE;
    } else {
        return false;
    }
    return true;
}

/* ==================== 输出与工作区 ==================== */

struct woodland_output *woodland_output_first(struct woodland_server *server)
{
    if (!server || !server->outputs.next || wl_list_empty(&server->outputs)) {
        return NULL;
    }
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        return wo;
    }
    return NULL;
}

struct woodland_output *woodland_output_at(struct woodland_server *server, double lx, double ly)
{
    if (!server || !server->output_layout || !server->outputs.next || wl_list_empty(&server->outputs)) {
        return NULL;
    }
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
        if (wlr_box_contains_point(&box, lx, ly)) {
            return wo;
        }
    }
    return NULL;
}

/*
 * 离 (lx,ly) 最近的输出，用于「点不在任何输出上」（输出之间的空隙、
 * 窗口被拖到输出之外）的情形。
 *
 * 不用 wlr_output_layout_closest_point：它只回填坐标，要拿回输出还得
 * 再探一次；而它返回的最近点落在框内距右/下边 1/256 处，加点偏移去
 * 探就可能探到框外，于是右/下侧的副屏会被误判成主屏。这里直接按每个
 * 输出的布局框算最近点距离，取最小者，边界不用猜。
 */
struct woodland_output *woodland_output_nearest(struct woodland_server *server, double lx,
                                                double ly)
{
    if (!server || !server->output_layout || !server->outputs.next || wl_list_empty(&server->outputs)) {
        return NULL;
    }
    struct woodland_output *best = NULL;
    double best_dist = 0;
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        if (!wo->wlr_output) {
            continue;
        }
        struct wlr_box box;
        /* 布局坐标：与 cursor->x/y、窗口几何同坐标系 */
        wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
        if (box.width <= 0 || box.height <= 0) {
            continue;
        }
        double px, py;
        wlr_box_closest_point(&box, lx, ly, &px, &py);
        double dx = lx - px;
        double dy = ly - py;
        double dist = dx * dx + dy * dy;
        if (!best || dist < best_dist) {
            best = wo;
            best_dist = dist;
        }
    }
    return best;
}

/* 外框中心所在的输出；落在输出之间的空隙时退化为最近的输出。 */
struct woodland_output *woodland_output_for_view(struct woodland_view *view)
{
    if (!view || !view->server || !view->server->output_layout ||
        !view->server->outputs.next || wl_list_empty(&view->server->outputs)) {
        return NULL;
    }
    struct woodland_server *server = view->server;
    struct wlr_box outer;
    woodland_view_outer_box(view, &outer);

    double cx = outer.x + outer.width / 2.0;
    double cy = outer.y + outer.height / 2.0;

    struct woodland_output *wo = woodland_output_at(server, cx, cy);
    if (wo) {
        return wo;
    }
    wo = woodland_output_nearest(server, cx, cy);
    if (wo) {
        return wo;
    }
    return woodland_output_first(server);
}

void woodland_output_workarea(struct woodland_output *output, struct wlr_box *out)
{
    memset(out, 0, sizeof(*out));
    if (!output || !output->wlr_output) {
        return;
    }
    /* layout box 是输出在全局布局里的位置，usable_area 是输出局部坐标，
     * 必须加布局偏移才是"布局坐标下的工作区"。 */
    wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, out);
    if (output->usable_area_valid) {
        out->x += output->usable_area.x;
        out->y += output->usable_area.y;
        out->width = output->usable_area.width;
        out->height = output->usable_area.height;
    }
}

void woodland_workarea_at(struct woodland_server *server, double lx, double ly, struct wlr_box *out)
{
    struct woodland_output *wo = woodland_output_at(server, lx, ly);
    if (!wo) {
        wo = woodland_output_nearest(server, lx, ly);
    }
    if (!wo) {
        wo = woodland_output_first(server);
    }
    woodland_output_workarea(wo, out);
}

struct wlr_box woodland_view_maximized_geometry_on(struct woodland_server *server,
                                                   struct woodland_output *output)
{
    struct wlr_box box = {0};
    if (!output) {
        output = woodland_output_first(server);
    }
    woodland_output_workarea(output, &box);
    return box;
}

struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server)
{
    /* 历史语义：首个输出。业务代码用 _on() 版本按窗口所在输出算。 */
    return woodland_view_maximized_geometry_on(server, NULL);
}

struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output)
{
    struct wlr_box box = {0};
    if (!output || !server) {
        return box;
    }
    /* 全屏取输出完整 box，布局坐标（不吃 usable_area） */
    wlr_output_layout_get_box(server->output_layout, output, &box);
    return box;
}

/* ==================== 外框与约束 ==================== */

void woodland_view_outer_box(const struct woodland_view *view, struct wlr_box *out)
{
    memset(out, 0, sizeof(*out));
    if (!view || !view->scene_tree) {
        return;
    }
    struct woodland_ssd_margins margins;
    /* get_margins 只读状态位，虽非 const 但不改 view */
    woodland_ssd_get_margins((struct woodland_view *)view, &margins);
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    out->x = view->scene_tree->node.x - margins.left;
    out->y = view->scene_tree->node.y - margins.top;
    out->width = content.width + margins.left + margins.right;
    out->height = content.height + margins.top + margins.bottom;
}

void woodland_box_constrain(struct wlr_box *box, const struct wlr_box *area)
{
    if (!box || !area || area->width <= 0 || area->height <= 0) {
        return;
    }
    /* 比工作区还大：贴左上，保证标题栏可抓（右下允许被裁） */
    if (box->width >= area->width) {
        box->x = area->x;
    } else if (box->x + box->width > area->x + area->width) {
        box->x = area->x + area->width - box->width;
    } else if (box->x < area->x) {
        box->x = area->x;
    }
    if (box->height >= area->height) {
        box->y = area->y;
    } else if (box->y + box->height > area->y + area->height) {
        box->y = area->y + area->height - box->height;
    } else if (box->y < area->y) {
        box->y = area->y;
    }
}

void woodland_view_constrain(struct woodland_view *view)
{
    struct woodland_output *wo = woodland_output_for_view(view);
    if (!wo) {
        return;
    }
    struct wlr_box area;
    woodland_output_workarea(wo, &area);
    struct wlr_box outer;
    woodland_view_outer_box(view, &outer);
    int old_x = outer.x;
    int old_y = outer.y;
    woodland_box_constrain(&outer, &area);
    if (outer.x != old_x || outer.y != old_y) {
        /* 外框位移 → 内容原点同量位移 */
        wlr_scene_node_set_position(&view->scene_tree->node,
                                    view->scene_tree->node.x + (outer.x - old_x),
                                    view->scene_tree->node.y + (outer.y - old_y));
    }
}

/* ==================== 放置几何 ==================== */

void woodland_view_placement(const struct wlr_box *area, int content_w, int content_h,
                             const struct woodland_ssd_margins *margins, int cascade,
                             struct wlr_box *out)
{
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;

    int x = area->x + cascade;
    int y = area->y + cascade;
    int max_x = area->x + area->width - outer_w;
    int max_y = area->y + area->height - outer_h;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < area->x) {
        x = area->x;
    }
    if (y < area->y) {
        y = area->y;
    }

    out->x = x + margins->left;
    out->y = y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

void woodland_placement_center(const struct wlr_box *area, int content_w, int content_h,
                               const struct woodland_ssd_margins *margins, struct wlr_box *out)
{
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;
    int x = area->x + (area->width - outer_w) / 2;
    int y = area->y + (area->height - outer_h) / 2;

    struct wlr_box outer = {
        .x = x,
        .y = y,
        .width = outer_w,
        .height = outer_h,
    };
    woodland_box_constrain(&outer, area);

    out->x = outer.x + margins->left;
    out->y = outer.y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

void woodland_placement_cursor(const struct wlr_box *area, double cursor_x, double cursor_y,
                               int content_w, int content_h,
                               const struct woodland_ssd_margins *margins, struct wlr_box *out)
{
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;
    /* 外框左上角贴光标：这是 labwc "cursor" 策略的语义（不是居中于光标） */
    struct wlr_box outer = {
        .x = (int)cursor_x,
        .y = (int)cursor_y,
        .width = outer_w,
        .height = outer_h,
    };
    woodland_box_constrain(&outer, area);

    out->x = outer.x + margins->left;
    out->y = outer.y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

static int overlap_area(const struct wlr_box *a, const struct wlr_box *b)
{
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2 = (a->x + a->width) < (b->x + b->width) ? (a->x + a->width) : (b->x + b->width);
    int y2 = (a->y + a->height) < (b->y + b->height) ? (a->y + a->height) : (b->y + b->height);
    if (x2 <= x1 || y2 <= y1) {
        return 0;
    }
    return (x2 - x1) * (y2 - y1);
}

static int total_overlap(const struct wlr_box *cand, const struct wlr_box *obstacles, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += overlap_area(cand, &obstacles[i]);
    }
    return sum;
}

void woodland_placement_autoplace(const struct wlr_box *area, int content_w, int content_h,
                                  const struct woodland_ssd_margins *margins,
                                  const struct wlr_box *obstacles, int n_obstacles, int cascade,
                                  struct wlr_box *out)
{
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;

    /* 候选 c 个：四角 + 层叠位 + 每个障碍的 4 条贴靠位 */
    int cap = 5 + n_obstacles * 4;
    struct wlr_box *cands = calloc((size_t)cap, sizeof(*cands));
    if (!cands) {
        woodland_view_placement(area, content_w, content_h, margins, cascade, out);
        return;
    }

    int n = 0;
    /* 工作区四角 */
    cands[n++] = (struct wlr_box){area->x, area->y, outer_w, outer_h};
    cands[n++] = (struct wlr_box){area->x + area->width - outer_w, area->y, outer_w, outer_h};
    cands[n++] = (struct wlr_box){area->x, area->y + area->height - outer_h, outer_w, outer_h};
    cands[n++] = (struct wlr_box){area->x + area->width - outer_w, area->y + area->height - outer_h,
                                  outer_w, outer_h};
    /* 层叠位 */
    cands[n++] = (struct wlr_box){area->x + cascade, area->y + cascade, outer_w, outer_h};
    /* 每个障碍的右侧/左侧/下方/上方贴靠 */
    for (int i = 0; i < n_obstacles && n + 4 <= cap; i++) {
        const struct wlr_box *o = &obstacles[i];
        cands[n++] = (struct wlr_box){o->x + o->width, o->y, outer_w, outer_h};
        cands[n++] = (struct wlr_box){o->x - outer_w, o->y, outer_w, outer_h};
        cands[n++] = (struct wlr_box){o->x, o->y + o->height, outer_w, outer_h};
        cands[n++] = (struct wlr_box){o->x, o->y - outer_h, outer_w, outer_h};
    }

    struct wlr_box best = cands[0];
    int best_overlap = INT_MAX;
    int best_dist = INT_MAX;
    for (int i = 0; i < n; i++) {
        struct wlr_box cand = cands[i];
        /* 候选必须先落进工作区，越界的贴靠位是无效候选 */
        woodland_box_constrain(&cand, area);
        int ov = total_overlap(&cand, obstacles, n_obstacles);
        /*
         * 重叠相同时取离工作区左上更近的：否则"右上角"这类候选会先命中，
         * 窗口被甩到屏幕另一头，而不是紧挨着已有窗口排开。
         */
        int dx = cand.x - area->x;
        int dy = cand.y - area->y;
        int dist = dx + dy;
        if (ov < best_overlap || (ov == best_overlap && dist < best_dist)) {
            best_overlap = ov;
            best_dist = dist;
            best = cand;
        }
    }
    free(cands);

    out->x = best.x + margins->left;
    out->y = best.y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

/* ==================== 贴边铺满 ==================== */

uint32_t woodland_snap_edges_for(const struct wlr_box *outer, const struct wlr_box *area,
                                 int threshold)
{
    uint32_t edges = 0;
    if (!outer || !area) {
        return 0;
    }
    int right = outer->x + outer->width;
    int bottom = outer->y + outer->height;
    int area_right = area->x + area->width;
    int area_bottom = area->y + area->height;

    /* 外框比工作区还大时，"贴住某条边"没有意义（另一条边必然溢出），
     * 铺满也只会把窗口压得更怪，直接不判 */
    if (outer->width <= area->width) {
        if (abs(outer->x - area->x) <= threshold) {
            edges |= WLR_EDGE_LEFT;
        }
        if (abs(right - area_right) <= threshold) {
            edges |= WLR_EDGE_RIGHT;
        }
    }
    if (outer->height <= area->height) {
        if (abs(outer->y - area->y) <= threshold) {
            edges |= WLR_EDGE_TOP;
        }
        if (abs(bottom - area_bottom) <= threshold) {
            edges |= WLR_EDGE_BOTTOM;
        }
    }
    return edges;
}

bool woodland_snap_tile_box(const struct wlr_box *area, uint32_t edges, struct wlr_box *out_outer)
{
    if (!area || !out_outer) {
        return false;
    }
    bool left = edges & WLR_EDGE_LEFT;
    bool right = edges & WLR_EDGE_RIGHT;
    bool top = edges & WLR_EDGE_TOP;
    bool bottom = edges & WLR_EDGE_BOTTOM;

    /* 左右同时贴住 → 没有"半屏"语义；上下同时贴住同理，交给最大化处理 */
    if ((left && right) && (top && bottom)) {
        return false;
    }

    struct wlr_box box = *area;
    /* 顺序要紧：左右优先（四角是"半屏 + 半高"），只有单贴上边才是最大化 */
    if (left && !right) {
        box.width = area->width / 2;
        if (top) {
            box.height = area->height / 2;
        } else if (bottom) {
            box.height = area->height / 2;
            box.y = area->y + area->height - box.height;
        }
    } else if (right && !left) {
        box.width = area->width / 2;
        box.x = area->x + area->width - box.width;
        if (top) {
            box.height = area->height / 2;
        } else if (bottom) {
            box.height = area->height / 2;
            box.y = area->y + area->height - box.height;
        }
    } else if (top && bottom) {
        /* 只有纵向两端贴住：纵向铺满，横向保持 */
        box.y = area->y;
        box.height = area->height;
    } else if (top && !bottom) {
        /* 上边：labwc snapToEdge 语义 —— 最大化（占满工作区） */
        box = *area;
    } else {
        return false;
    }

    *out_outer = box;
    return true;
}

void woodland_view_center_on(const struct wlr_box *parent_box, int content_w, int content_h,
                             const struct woodland_ssd_margins *margins, const struct wlr_box *area,
                             struct wlr_box *out)
{
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;

    int x = parent_box->x + (parent_box->width - outer_w) / 2;
    int y = parent_box->y + (parent_box->height - outer_h) / 2;

    struct wlr_box outer = {x, y, outer_w, outer_h};
    woodland_box_constrain(&outer, area);

    out->x = outer.x + margins->left;
    out->y = outer.y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

int woodland_snap_axis(int desired, int size, int lo, int hi, const int *edges, int n_edges,
                       int threshold)
{
    /* 外框比工作区还大：没有可贴的意义，原样返回 */
    if (size > hi - lo) {
        return desired;
    }

    int best = desired;
    int best_delta = threshold + 1;

    /* 先试工作区两端，再试其它窗口的边（同距离时更偏向前者） */
    for (int i = 0; i < n_edges + 2; i++) {
        if (i > 1) {
            int edge = edges[i - 2];
            int d0 = edge - desired;
            int d1 = d0 - size; /* 我们的末边贴到该边 */
            if (d0 < 0) {
                d0 = -d0;
            }
            if (d1 < 0) {
                d1 = -d1;
            }
            if (d0 <= threshold && d0 < best_delta) {
                best_delta = d0;
                best = edge;
            }
            if (d1 <= threshold && d1 < best_delta) {
                best_delta = d1;
                best = edge - size;
            }
            continue;
        }

        int cand = i == 0 ? lo : hi - size;
        int delta = cand - desired;
        if (delta < 0) {
            delta = -delta;
        }
        if (delta <= threshold && delta < best_delta) {
            best_delta = delta;
            best = cand;
        }
    }

    return best;
}

/* ==================== 首次放置 ==================== */

/*
 * 两个视图是否算"同一个工作区的邻居"。
 *
 * X11 的首次放置在 map_request 阶段，那时 view->workspace 还是 NULL
 * （工作区在 map 时才确定）；此时不能简单比较相等，否则第一个 X11 窗口
 * 永远看不到邻居，层叠偏移恒为 0，新窗口正好盖住旧窗口。
 */
static bool same_workspace(const struct woodland_view *a, const struct woodland_view *b)
{
    if (!a->workspace || !b->workspace) {
        return true;
    }
    return a->workspace == b->workspace;
}

/* 层叠序号：统计当前工作区其它已映射视图，排除自己。
 * X11 的首次放置发生在 map_request（尚未进链表），xdg 发生在 map
 * （已在链表里），排除自己后两者语义一致。 */
static int cascade_index(struct woodland_view *view)
{
    int count = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &view->server->views, link)
    {
        if (it != view && it->mapped && !it->minimized && same_workspace(it, view)) {
            count++;
        }
    }
    return (count % WOODLAND_CASCADE_WRAP) * WOODLAND_CASCADE_STEP;
}

/* 收集当前工作区其它已映射视图的外框，供 autoplace 判重叠 */
static struct wlr_box *collect_obstacles(struct woodland_view *view, int *n_out)
{
    int n = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &view->server->views, link)
    {
        if (it != view && it->mapped && !it->minimized && !it->fullscreen &&
            same_workspace(it, view)) {
            n++;
        }
    }
    if (n == 0) {
        *n_out = 0;
        return NULL;
    }
    struct wlr_box *boxes = calloc((size_t)n, sizeof(*boxes));
    if (!boxes) {
        *n_out = 0;
        return NULL;
    }
    int i = 0;
    wl_list_for_each(it, &view->server->views, link)
    {
        if (it != view && it->mapped && !it->minimized && !it->fullscreen &&
            same_workspace(it, view)) {
            woodland_view_outer_box(it, &boxes[i++]);
        }
    }
    *n_out = i;
    return boxes;
}

bool woodland_view_is_floating(const struct woodland_view *view)
{
    if (!view) {
        return false;
    }
    return !(view->fullscreen || view->maximized || view->snapped_edges != 0);
}

void woodland_view_place_initial(struct woodland_view *view)
{
    struct woodland_server *server = view->server;
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);

    struct wlr_box content;
    woodland_view_content_box(view, &content);
    int content_w = content.width;
    int content_h = content.height;

    /* 全屏、最大化或已平铺窗口不参与常规浮动放置（对齐 labwc set_initial_position） */
    if (!woodland_view_is_floating(view)) {
        return;
    }

    /* 目标输出：transient 跟着父窗口走，否则跟随光标（同 labwc placement） */
    struct woodland_output *wo = NULL;
    if (view->parent && view->parent->mapped) {
        wo = woodland_output_for_view(view->parent);
    } else if (server->cursor) {
        wo = woodland_output_at(server, server->cursor->x, server->cursor->y);
    }
    if (!wo) {
        wo = woodland_output_for_view(view);
    }
    if (!wo) {
        wo = woodland_output_first(server);
    }

    struct wlr_box area;
    woodland_output_workarea(wo, &area);
    if (area.width <= 0 || area.height <= 0) {
        return;
    }

    struct wlr_box geometry;
    if (view->parent && view->parent->mapped) {
        /* 对话框/工具窗：以父窗口外框为中心（越界时贴回工作区） */
        struct wlr_box parent_outer;
        woodland_view_outer_box(view->parent, &parent_outer);
        woodland_view_center_on(&parent_outer, content_w, content_h, &margins, &area, &geometry);
    } else {
        switch (server->placement_policy) {
        case WOODLAND_PLACE_CENTER:
            woodland_placement_center(&area, content_w, content_h, &margins, &geometry);
            break;
        case WOODLAND_PLACE_CURSOR:
            woodland_placement_cursor(&area, server->cursor->x, server->cursor->y, content_w,
                                      content_h, &margins, &geometry);
            break;
        case WOODLAND_PLACE_AUTOPLACE: {
            int n_obstacles = 0;
            struct wlr_box *obstacles = collect_obstacles(view, &n_obstacles);
            woodland_placement_autoplace(&area, content_w, content_h, &margins, obstacles,
                                         n_obstacles, cascade_index(view), &geometry);
            free(obstacles);
            break;
        }
        case WOODLAND_PLACE_CASCADE:
        default:
            woodland_view_placement(&area, content_w, content_h, &margins, cascade_index(view),
                                    &geometry);
            break;
        }
    }
    wlr_scene_node_set_position(&view->scene_tree->node, geometry.x, geometry.y);
    /* 只记"凭什么放这里"：几何本身 get_tree 里有，写进来是噪音 */
    if (view->parent && view->parent->mapped) {
        woodland_trace_add("place", view->id, "on parent=%u %dx%d at %d,%d", view->parent->id,
                           content_w, content_h, geometry.x, geometry.y);
    } else {
        woodland_trace_add("place", view->id, "policy=%s %dx%d at %d,%d",
                           woodland_placement_policy_name(server->placement_policy), content_w,
                           content_h, geometry.x, geometry.y);
    }
}

/* ==================== 外框几何应用与贴边 ==================== */

void woodland_view_apply_outer_geometry(struct woodland_view *view, const struct wlr_box *box)
{
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);
    int content_w = box->width - margins.left - margins.right;
    int content_h = box->height - margins.top - margins.bottom;
    if (content_w < 1) {
        content_w = 1;
    }
    if (content_h < 1) {
        content_h = 1;
    }
    wlr_scene_node_set_position(&view->scene_tree->node, box->x + margins.left,
                                box->y + margins.top);
    woodland_view_request_size(view, content_w, content_h);
    woodland_view_update_outputs(view);
}

void woodland_view_set_tiled(struct woodland_view *view, uint32_t edges)
{
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        /* EWMH 没有 tiled 语义，X11 端只靠几何表达 */
        return;
    }
#endif
    if (view->xdg_toplevel) {
        wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, edges);
    }
}

bool woodland_view_snap_to(struct woodland_view *view, uint32_t edges)
{
    if (!view || !view->mapped || edges == 0) {
        return false;
    }
    if (view->maximized || view->fullscreen) {
        return false;
    }
    struct wlr_box area;
    woodland_output_workarea(woodland_output_for_view(view), &area);
    if (area.width <= 0 || area.height <= 0) {
        return false;
    }

    /* 只有上边贴住 = 最大化：走状态机，客户端也能收到 maximized */
    if (edges == WLR_EDGE_TOP) {
        woodland_view_set_maximized(view, true);
        view->snapped_edges = WLR_EDGE_TOP;
        return true;
    }

    struct wlr_box tile;
    if (!woodland_snap_tile_box(&area, edges, &tile)) {
        return false;
    }
    /* 记住贴边前的外框，下次拖动时据此恢复尺寸 */
    woodland_view_outer_box(view, &view->snap_saved_geometry);
    view->snapped_edges = edges;
    woodland_view_set_tiled(view, edges);
    woodland_view_apply_outer_geometry(view, &tile);
    return true;
}

/* ==================== 尺寸限制 ==================== */

void woodland_view_size_limits(const struct woodland_view *view, int *min_w, int *min_h, int *max_w,
                               int *max_h)
{
    *min_w = 1;
    *min_h = 1;
    *max_w = 0; /* 0 = 无限制 */
    *max_h = 0;

    if (view->type == WOODLAND_VIEW_XDG && view->xdg_toplevel) {
        const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
        if (st->min_width > 0) {
            *min_w = st->min_width;
        }
        if (st->min_height > 0) {
            *min_h = st->min_height;
        }
        if (st->max_width > 0) {
            *max_w = st->max_width;
        }
        if (st->max_height > 0) {
            *max_h = st->max_height;
        }
#if WOODLAND_HAVE_XWAYLAND
    } else if (view->type == WOODLAND_VIEW_XWAYLAND && view->xwayland_surface) {
        const xcb_size_hints_t *hints = view->xwayland_surface->size_hints;
        if (hints) {
            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
                if (hints->min_width > 0) {
                    *min_w = hints->min_width;
                }
                if (hints->min_height > 0) {
                    *min_h = hints->min_height;
                }
            }
            if (hints->flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
                /* base size 是"内容区最小尺寸"，比 min_size 更贴近实际 */
                if (hints->base_width > *min_w) {
                    *min_w = hints->base_width;
                }
                if (hints->base_height > *min_h) {
                    *min_h = hints->base_height;
                }
            }
            if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
                if (hints->max_width > 0) {
                    *max_w = hints->max_width;
                }
                if (hints->max_height > 0) {
                    *max_h = hints->max_height;
                }
            }
        }
#endif
    }
}
