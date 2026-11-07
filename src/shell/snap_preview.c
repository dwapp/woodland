// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include "scene.h"
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "output.h"
#include "placement.h"
#include "shell_internal.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

#define SNAP_PREVIEW_FILTER_MS 200
/* 配置缺省（server->config 为空，例如单测直接调）时的边框宽度 */
#define SNAP_PREVIEW_DEFAULT_BORDER 2

/*
 * 预览配色：显式配置 > treeland-appearance 强调色 > 内置天蓝。
 *
 * 强调色只借 RGB：不透明度仍按 [general] 给的来（边框用颜色自带的 alpha、
 * 填充用 snap_preview_opacity）。这样桌面换主题时预览跟着变，用户想固定
 * 自己的颜色时又压得住。
 */
static void snap_preview_get_colors(struct woodland_server *server, float fill_color[4],
                                    float border_color[4])
{
    struct woodland_config *cfg = server ? server->config : NULL;
    uint32_t color = cfg ? cfg->general.snap_preview_color : 0xBF0080FFu;
    if (cfg && !cfg->general.snap_preview_color_specified && cfg->appearance.accent_set) {
        color = (color & 0xFF000000u) | (cfg->appearance.accent_rgb & 0xFFFFFFu);
    }
    float r = ((color >> 16) & 0xff) / 255.0f;
    float g = ((color >> 8) & 0xff) / 255.0f;
    float b = (color & 0xff) / 255.0f;
    float border_alpha = ((color >> 24) & 0xff) / 255.0f;
    float fill_alpha = cfg ? (float)cfg->general.snap_preview_opacity : 0.25f;

    fill_color[0] = r;
    fill_color[1] = g;
    fill_color[2] = b;
    fill_color[3] = fill_alpha;

    border_color[0] = r;
    border_color[1] = g;
    border_color[2] = b;
    border_color[3] = border_alpha;
}

static int snap_preview_border_width(struct woodland_server *server)
{
    int32_t w = server && server->config ? server->config->general.snap_preview_border_width
                                         : SNAP_PREVIEW_DEFAULT_BORDER;
    if (w < 0) {
        w = 0;
    }
    if (w > 32) {
        w = 32;
    }
    return (int)w;
}

/*
 * 样式指纹：边框宽度 + 配色，量化到 8bit 后做 FNV-1a。
 *
 * 用途是让 show() 的"位置没变就跳过"不会连样式变更一起跳过——拖动中途
 * SIGHUP 重载配置、treeland-appearance 换强调色，都得当帧生效。
 */
static uint32_t snap_preview_style_key(int border_width, const float fill_color[4],
                                       const float border_color[4])
{
    const float *parts[2] = {fill_color, border_color};
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)(border_width & 0xff)) * 16777619u;
    for (int i = 0; i < 2; i++) {
        for (int c = 0; c < 4; c++) {
            unsigned int v = (unsigned int)(parts[i][c] * 255.0f + 0.5f);
            h = (h ^ (v & 0xff)) * 16777619u;
        }
    }
    return h;
}

static int handle_snap_filter_timer(void *data)
{
    struct woodland_snap_preview *sp = data;
    if (!sp || !sp->server) {
        return 0;
    }
    sp->filter_pending = false;
    if (sp->server->cursor_mode == WOODLAND_CURSOR_MOVE &&
        sp->server->grabbed_view == sp->pending_view &&
        sp->pending_edges != 0 && sp->pending_output) {
        woodland_snap_preview_show(sp->server, sp->pending_view, sp->pending_output,
                                   sp->pending_edges);
    }
    return 0;
}

void woodland_snap_preview_init(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    memset(&server->snap_preview, 0, sizeof(server->snap_preview));
    server->snap_preview.server = server;
    if (server->event_loop) {
        server->snap_preview.filter_timer =
            wl_event_loop_add_timer(server->event_loop, handle_snap_filter_timer,
                                    &server->snap_preview);
    }
}

void woodland_snap_preview_finish(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    struct woodland_snap_preview *sp = &server->snap_preview;
    if (sp->filter_timer) {
        wl_event_source_remove(sp->filter_timer);
        sp->filter_timer = NULL;
    }
    sp->filter_pending = false;
    woodland_snap_preview_destroy(server);
}

void woodland_snap_preview_destroy(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    struct woodland_snap_preview *sp = &server->snap_preview;
    if (sp->filter_timer && sp->filter_pending) {
        wl_event_source_timer_update(sp->filter_timer, 0);
        sp->filter_pending = false;
    }
    sp->pending_edges = 0;
    sp->pending_output = NULL;
    sp->pending_view = NULL;

    if (sp->tree) {
        wlr_scene_node_destroy(&sp->tree->node);
        sp->tree = NULL;
        sp->fill = NULL;
        sp->border = NULL;
        for (int i = 0; i < 4; i++) {
            sp->border_edges[i] = NULL;
        }
    }
    sp->edges = 0;
    sp->output = NULL;
    sp->style_key = 0;
    memset(&sp->box, 0, sizeof(sp->box));
}

void woodland_snap_preview_hide(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    struct woodland_snap_preview *sp = &server->snap_preview;
    if (sp->filter_timer && sp->filter_pending) {
        wl_event_source_timer_update(sp->filter_timer, 0);
        sp->filter_pending = false;
    }
    sp->pending_edges = 0;
    sp->pending_output = NULL;
    sp->pending_view = NULL;

    if (sp->tree && sp->tree->node.enabled) {
        wlr_scene_node_set_enabled(&sp->tree->node, false);
        woodland_trace_add("snap", 0, "preview_hide");
    }
    sp->edges = 0;
    sp->output = NULL;
    memset(&sp->box, 0, sizeof(sp->box));
}

void woodland_snap_preview_show(struct woodland_server *server, struct woodland_view *view,
                                struct woodland_output *output, uint32_t edges)
{
    if (!server || !view || !output || edges == 0) {
        woodland_snap_preview_hide(server);
        return;
    }

    struct wlr_box area;
    woodland_output_workarea(output, &area);
    struct wlr_box tile_box;
    if (!woodland_snap_tile_box(&area, edges, &tile_box)) {
        woodland_snap_preview_hide(server);
        return;
    }

    struct woodland_snap_preview *sp = &server->snap_preview;

    const int b = snap_preview_border_width(server);
    float fill_color[4];
    float border_color[4];
    snap_preview_get_colors(server, fill_color, border_color);
    uint32_t style_key = snap_preview_style_key(b, fill_color, border_color);

    /* 位置和样式都没变就什么都不用做（拖动中每次 motion 都会进来） */
    if (sp->tree && sp->tree->node.enabled && sp->edges == edges && sp->output == output &&
        sp->style_key == style_key && sp->box.x == tile_box.x && sp->box.y == tile_box.y &&
        sp->box.width == tile_box.width && sp->box.height == tile_box.height) {
        return;
    }

    /* 挂载到被拖动窗口所在的父场景树（工作区树），并置于被拖动窗口之下 */
    struct wlr_scene_tree *parent = view->scene_tree ? view->scene_tree->node.parent : NULL;
    if (!parent) {
        parent = &server->scene->tree;
    }

    int inner_w = tile_box.width - 2 * b;
    int inner_h = tile_box.height - 2 * b;
    if (inner_w < 1) {
        inner_w = 1;
    }
    if (inner_h < 1) {
        inner_h = 1;
    }

    if (!sp->tree) {
        sp->tree = wlr_scene_tree_create(parent);
        if (!sp->tree) {
            return;
        }

#if WOODLAND_HAVE_EFFECTS
        sp->border = wlr_scene_rect_create(sp->tree, tile_box.width, tile_box.height, border_color);
        sp->fill = wlr_scene_rect_create(sp->tree, inner_w, inner_h, fill_color);
#else
        sp->fill = wlr_scene_rect_create(sp->tree, inner_w, inner_h, fill_color);
        /* 四条边框 */
        sp->border_edges[0] = wlr_scene_rect_create(sp->tree, tile_box.width, b, border_color);
        sp->border_edges[1] = wlr_scene_rect_create(sp->tree, tile_box.width, b, border_color);
        sp->border_edges[2] = wlr_scene_rect_create(sp->tree, b, inner_h, border_color);
        sp->border_edges[3] = wlr_scene_rect_create(sp->tree, b, inner_h, border_color);
#endif
        /*
         * z 序只在建树时排一次：预览树和被拖的窗口是兄弟节点，拖动过程中
         * 窗口既不换父树也不换 z，每次 update 都 place_below 等于每帧白排一次。
         */
        if (view->scene_tree) {
            wlr_scene_node_place_below(&sp->tree->node, &view->scene_tree->node);
        }
    } else if (sp->tree->node.parent != parent) {
        /* 窗口换了父树（切工作区后重挂一类）：跟着搬，并重新压回窗口下面 */
        wlr_scene_node_reparent(&sp->tree->node, parent);
        if (view->scene_tree) {
            wlr_scene_node_place_below(&sp->tree->node, &view->scene_tree->node);
        }
    }

    /* 尺寸 / 位置 / 配色每次重设：节点复用，只是改属性 */
#if WOODLAND_HAVE_EFFECTS
    if (sp->border) {
        wlr_scene_rect_set_size(sp->border, tile_box.width, tile_box.height);
        wlr_scene_rect_set_color(sp->border, border_color);
    }
    if (sp->fill) {
        wlr_scene_rect_set_size(sp->fill, inner_w, inner_h);
        wlr_scene_rect_set_color(sp->fill, fill_color);
        wlr_scene_node_set_position(&sp->fill->node, b, b);
    }
#else
    if (sp->fill) {
        wlr_scene_rect_set_size(sp->fill, inner_w, inner_h);
        wlr_scene_rect_set_color(sp->fill, fill_color);
        wlr_scene_node_set_position(&sp->fill->node, b, b);
    }
    if (sp->border_edges[0]) {
        wlr_scene_rect_set_size(sp->border_edges[0], tile_box.width, b);
        wlr_scene_node_set_position(&sp->border_edges[0]->node, 0, 0);
        wlr_scene_rect_set_color(sp->border_edges[0], border_color);
    }
    if (sp->border_edges[1]) {
        wlr_scene_rect_set_size(sp->border_edges[1], tile_box.width, b);
        wlr_scene_node_set_position(&sp->border_edges[1]->node, 0, tile_box.height - b);
        wlr_scene_rect_set_color(sp->border_edges[1], border_color);
    }
    if (sp->border_edges[2]) {
        wlr_scene_rect_set_size(sp->border_edges[2], b, inner_h);
        wlr_scene_node_set_position(&sp->border_edges[2]->node, 0, b);
        wlr_scene_rect_set_color(sp->border_edges[2], border_color);
    }
    if (sp->border_edges[3]) {
        wlr_scene_rect_set_size(sp->border_edges[3], b, inner_h);
        wlr_scene_node_set_position(&sp->border_edges[3]->node, tile_box.width - b, b);
        wlr_scene_rect_set_color(sp->border_edges[3], border_color);
    }
#endif

#if WOODLAND_HAVE_EFFECTS
    /*
     * 圆角描边：外框 rect 画满色，再用 clipped_region 把内框挖掉，
     * 剩下的一圈就是边框；内半径 r、外半径 r+b 才能让内外弧同心。
     * 不在 1px 细条上设圆角（scenefx 会按 rect 自身尺寸 clamp 掉半径）。
     */
    if (sp->border && sp->fill) {
        int r = (server->config && server->config->effects.enabled)
                    ? server->config->effects.corner_radius
                    : 0;
        struct clipped_region clip = clipped_region_get_default();
        clip.area = (struct wlr_box){b, b, inner_w, inner_h};

        if (r > 0) {
            wlr_scene_rect_set_corner_radii(sp->border, corner_radii_new(r + b, r + b, r + b, r + b));
            clip.corners = corner_radii_new(r, r, r, r);
            wlr_scene_rect_set_corner_radii(sp->fill, corner_radii_new(r, r, r, r));
        } else {
            wlr_scene_rect_set_corner_radii(sp->border, corner_radii_none());
            wlr_scene_rect_set_corner_radii(sp->fill, corner_radii_none());
        }
        wlr_scene_rect_set_clipped_region(sp->border, clip);
    }
#endif

    /* z 序已在建树（或换父树）时排过，这里只摆位置、开可见 */
    wlr_scene_node_set_position(&sp->tree->node, tile_box.x, tile_box.y);
    wlr_scene_node_set_enabled(&sp->tree->node, true);

    sp->edges = edges;
    sp->output = output;
    sp->box = tile_box;
    sp->style_key = style_key;

    woodland_trace_add("snap", view->id, "preview output=%s edges=%u box=%dx%d at %d,%d",
                       output->wlr_output ? output->wlr_output->name : "-", edges,
                       tile_box.width, tile_box.height, tile_box.x, tile_box.y);
}

void woodland_snap_preview_update(struct woodland_server *server, struct woodland_view *view)
{
    if (!server || !server->config || !server->config->general.snap_preview) {
        woodland_snap_preview_hide(server);
        return;
    }

    if (!view || view != server->grabbed_view || view->pending_snap_edges == 0) {
        woodland_snap_preview_hide(server);
        return;
    }

    struct woodland_output *wo = woodland_output_at(server, server->cursor->x, server->cursor->y);
    if (!wo) {
        wo = woodland_output_for_view(view);
    }
    if (!wo) {
        woodland_snap_preview_hide(server);
        return;
    }

    uint32_t edges = view->pending_snap_edges;

    /*
     * 触发的边是输出之间的接缝还是屏幕墙外沿：接缝（外面还有显示器）
     * 要防抖，外沿可以立刻显示。只看参与判定的那几条边。
     */
    struct wlr_output *wo_output = wo->wlr_output;
    bool is_outer = true;
    if ((edges & WLR_EDGE_LEFT) &&
        !woodland_snap_edge_is_layout_edge(server->output_layout, wo_output, WLR_EDGE_LEFT)) {
        is_outer = false;
    }
    if ((edges & WLR_EDGE_RIGHT) &&
        !woodland_snap_edge_is_layout_edge(server->output_layout, wo_output, WLR_EDGE_RIGHT)) {
        is_outer = false;
    }
    if ((edges & WLR_EDGE_TOP) &&
        !woodland_snap_edge_is_layout_edge(server->output_layout, wo_output, WLR_EDGE_TOP)) {
        is_outer = false;
    }

    struct woodland_snap_preview *sp = &server->snap_preview;

    if (!is_outer) {
        /* 非外边界（两显示器之间的接缝）：跨屏时防抖延时 200ms */
        if (sp->edges != edges || sp->output != wo) {
            if (sp->edges != 0) {
                woodland_snap_preview_hide(server);
            }
            if (!sp->filter_pending || sp->pending_edges != edges || sp->pending_output != wo) {
                sp->pending_edges = edges;
                sp->pending_output = wo;
                sp->pending_view = view;
                sp->filter_pending = true;
                if (sp->filter_timer) {
                    wl_event_source_timer_update(sp->filter_timer, SNAP_PREVIEW_FILTER_MS);
                }
            }
            return;
        }
    } else {
        /* 外边界：若此前有跨屏延时，取消它并立即显示预览 */
        if (sp->filter_pending && sp->filter_timer) {
            wl_event_source_timer_update(sp->filter_timer, 0);
            sp->filter_pending = false;
        }
        woodland_snap_preview_show(server, view, wo, edges);
    }
}

uint32_t woodland_snap_determine_edges(struct woodland_view *view,
                                       const struct wlr_box *area,
                                       double cursor_x, double cursor_y,
                                       int threshold)
{
    if (!view || !area || threshold <= 0) {
        return 0;
    }
    if (view->fullscreen) {
        return 0;
    }
    /* 尺寸被客户端钉死的窗口（max == min）铺满也没意义 */
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (max_w > 0 && max_h > 0 && min_w == max_w && min_h == max_h) {
        return 0;
    }

    /*
     * 判定只看**光标**（gxde-wlcom 语义）：光标撞到工作区边才铺满。
     *
     * 曾经有一条"外框贴住工作区边"的兜底，已删除。它会让"窗口正好被拖到
     * 贴边、光标还在标题栏中间"也触发铺满——用户只是在挪窗口，没表达要
     * 占半屏。贴边铺满必须是光标这一下的动作，而不是窗口几何的副产物。
     * 松手路径（shell_view_apply_snap）用的就是这个返回值，所以删掉兜底
     * 不会出现"预览没亮、窗口却跳了"：两边都不再认为该铺满。
     */
    return woodland_snap_cursor_edges(area, cursor_x, cursor_y, threshold);
}

void woodland_snap_preview_status(struct woodland_server *server, struct woodland_json *out)
{
    if (!server || !out) {
        return;
    }
    struct woodland_snap_preview *sp = &server->snap_preview;
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);

    bool active = (sp->edges != 0 && sp->tree && sp->tree->node.enabled);
    woodland_json_obj_bool(&obj, "active", active);
    if (active) {
        static const struct {
            uint32_t edge;
            const char *name;
        } edge_names[] = {
            {WLR_EDGE_LEFT, "left"},
            {WLR_EDGE_RIGHT, "right"},
            {WLR_EDGE_TOP, "top"},
            {WLR_EDGE_BOTTOM, "bottom"},
        };
        woodland_json_obj_key(&obj, "edges");
        struct woodland_json_arr arr;
        woodland_json_arr_open(&arr, out);
        for (size_t i = 0; i < sizeof(edge_names) / sizeof(edge_names[0]); i++) {
            if (sp->edges & edge_names[i].edge) {
                woodland_json_arr_sep(&arr);
                woodland_json_string(out, edge_names[i].name);
            }
        }
        woodland_json_arr_close(&arr);

        woodland_json_obj_key(&obj, "box");
        struct woodland_json_obj box_obj;
        woodland_json_obj_open(&box_obj, out);
        woodland_json_obj_int(&box_obj, "x", sp->box.x);
        woodland_json_obj_int(&box_obj, "y", sp->box.y);
        woodland_json_obj_int(&box_obj, "width", sp->box.width);
        woodland_json_obj_int(&box_obj, "height", sp->box.height);
        woodland_json_obj_close(&box_obj);

        if (sp->output && sp->output->wlr_output) {
            woodland_json_obj_str(&obj, "output", sp->output->wlr_output->name);
        } else {
            woodland_json_obj_str(&obj, "output", "-");
        }
    }
    woodland_json_obj_close(&obj);
}
