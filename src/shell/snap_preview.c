// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
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

#define SNAP_PREVIEW_BORDER_WIDTH 2
#define SNAP_PREVIEW_FILTER_MS 200

/* 判断某输出边缘是否为输出布局的外边界（跨屏防抖判定） */
static bool output_edge_is_layout_edge(struct woodland_server *server,
                                       struct woodland_output *output,
                                       uint32_t edge)
{
    if (!server || !server->output_layout || !output || !output->wlr_output) {
        return true;
    }
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);

    int test_x = 0;
    int test_y = 0;

    switch (edge) {
    case WLR_EDGE_LEFT:
        test_x = box.x - 1;
        test_y = box.y + box.height / 2;
        break;
    case WLR_EDGE_RIGHT:
        test_x = box.x + box.width;
        test_y = box.y + box.height / 2;
        break;
    case WLR_EDGE_TOP:
        test_x = box.x + box.width / 2;
        test_y = box.y - 1;
        break;
    case WLR_EDGE_BOTTOM:
        test_x = box.x + box.width / 2;
        test_y = box.y + box.height;
        break;
    default:
        return true;
    }

    return !wlr_output_layout_contains_point(server->output_layout, NULL, test_x, test_y);
}

static void snap_preview_get_colors(struct woodland_server *server, float fill_color[4],
                                    float border_color[4])
{
    if (server && server->config && server->config->appearance.accent_set) {
        uint32_t c = server->config->appearance.accent_rgb;
        float r = ((c >> 16) & 0xff) / 255.0f;
        float g = ((c >> 8) & 0xff) / 255.0f;
        float b = (c & 0xff) / 255.0f;
        fill_color[0] = r;
        fill_color[1] = g;
        fill_color[2] = b;
        fill_color[3] = 0.25f;

        border_color[0] = r;
        border_color[1] = g;
        border_color[2] = b;
        border_color[3] = 0.75f;
    } else {
        /* 默认半透明天蓝色 (0, 128, 255) */
        fill_color[0] = 0.0f;
        fill_color[1] = 0.5f;
        fill_color[2] = 1.0f;
        fill_color[3] = 0.25f;

        border_color[0] = 0.0f;
        border_color[1] = 0.5f;
        border_color[2] = 1.0f;
        border_color[3] = 0.75f;
    }
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
#if !WOODLAND_HAVE_EFFECTS
        for (int i = 0; i < 4; i++) {
            sp->border_edges[i] = NULL;
        }
#endif
    }
    sp->edges = 0;
    sp->output = NULL;
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

    if (sp->tree && sp->tree->node.enabled && sp->edges == edges && sp->output == output &&
        sp->box.x == tile_box.x && sp->box.y == tile_box.y &&
        sp->box.width == tile_box.width && sp->box.height == tile_box.height) {
        return;
    }

    /* 挂载到被拖动窗口所在的父场景树（工作区树），并置于被拖动窗口之下 */
    struct wlr_scene_tree *parent = view->scene_tree ? view->scene_tree->node.parent : NULL;
    if (!parent) {
        parent = &server->scene->tree;
    }

    float fill_color[4];
    float border_color[4];
    snap_preview_get_colors(server, fill_color, border_color);

    const int b = SNAP_PREVIEW_BORDER_WIDTH;
    int inner_w = tile_box.width - 2 * b;
    int inner_h = tile_box.height - 2 * b;
    if (inner_w < 1) inner_w = 1;
    if (inner_h < 1) inner_h = 1;

    if (!sp->tree) {
        sp->tree = wlr_scene_tree_create(parent);
        if (!sp->tree) {
            return;
        }

#if WOODLAND_HAVE_EFFECTS
        sp->border = wlr_scene_rect_create(sp->tree, tile_box.width, tile_box.height, border_color);
        sp->fill = wlr_scene_rect_create(sp->tree, inner_w, inner_h, fill_color);
        if (sp->fill) {
            wlr_scene_node_set_position(&sp->fill->node, b, b);
        }
#else
        sp->fill = wlr_scene_rect_create(sp->tree, inner_w, inner_h, fill_color);
        if (sp->fill) {
            wlr_scene_node_set_position(&sp->fill->node, b, b);
        }
        /* 四条边框 */
        sp->border_edges[0] = wlr_scene_rect_create(sp->tree, tile_box.width, b, border_color);
        sp->border_edges[1] = wlr_scene_rect_create(sp->tree, tile_box.width, b, border_color);
        sp->border_edges[2] = wlr_scene_rect_create(sp->tree, b, inner_h, border_color);
        sp->border_edges[3] = wlr_scene_rect_create(sp->tree, b, inner_h, border_color);
#endif
    } else {
        if (sp->tree->node.parent != parent) {
            wlr_scene_node_reparent(&sp->tree->node, parent);
        }
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
    }

#if WOODLAND_HAVE_EFFECTS
    if (server->config && server->config->effects.enabled && sp->border && sp->fill) {
        int r = server->config->effects.corner_radius;
        if (r > 0) {
            wlr_scene_rect_set_corner_radii(sp->border,
                                            corner_radii_new(r + b, r + b, r + b, r + b));
            struct clipped_region clip = clipped_region_get_default();
            clip.area = (struct wlr_box){b, b, inner_w, inner_h};
            clip.corners = corner_radii_new(r, r, r, r);
            wlr_scene_rect_set_clipped_region(sp->border, clip);

            wlr_scene_rect_set_corner_radii(sp->fill, corner_radii_new(r, r, r, r));
        } else {
            wlr_scene_rect_set_corner_radii(sp->border, corner_radii_none());
            struct clipped_region clip = clipped_region_get_default();
            clip.area = (struct wlr_box){b, b, inner_w, inner_h};
            wlr_scene_rect_set_clipped_region(sp->border, clip);
            wlr_scene_rect_set_corner_radii(sp->fill, corner_radii_none());
        }
    } else if (sp->border && sp->fill) {
        wlr_scene_rect_set_corner_radii(sp->border, corner_radii_none());
        struct clipped_region clip = clipped_region_get_default();
        clip.area = (struct wlr_box){b, b, inner_w, inner_h};
        wlr_scene_rect_set_clipped_region(sp->border, clip);
        wlr_scene_rect_set_corner_radii(sp->fill, corner_radii_none());
    }
#endif

    if (view->scene_tree) {
        wlr_scene_node_place_below(&sp->tree->node, &view->scene_tree->node);
    }
    wlr_scene_node_set_position(&sp->tree->node, tile_box.x, tile_box.y);
    wlr_scene_node_set_enabled(&sp->tree->node, true);

    sp->edges = edges;
    sp->output = output;
    sp->box = tile_box;

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

    /* 检查触发的边缘是否为整个输出布局的外边界 */
    bool is_outer = true;
    if (edges & WLR_EDGE_LEFT) {
        if (!output_edge_is_layout_edge(server, wo, WLR_EDGE_LEFT)) {
            is_outer = false;
        }
    }
    if (edges & WLR_EDGE_RIGHT) {
        if (!output_edge_is_layout_edge(server, wo, WLR_EDGE_RIGHT)) {
            is_outer = false;
        }
    }
    if (edges & WLR_EDGE_TOP) {
        if (!output_edge_is_layout_edge(server, wo, WLR_EDGE_TOP)) {
            is_outer = false;
        }
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

uint32_t woodland_snap_determine_edges(struct woodland_server *server,
                                       struct woodland_view *view,
                                       struct woodland_output *wo,
                                       const struct wlr_box *outer,
                                       const struct wlr_box *area,
                                       double cursor_x, double cursor_y,
                                       int threshold)
{
    (void)server;
    (void)wo;
    if (!view || !area || threshold <= 0) {
        return 0;
    }
    if (view->fullscreen) {
        return 0;
    }
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (max_w > 0 && max_h > 0 && min_w == max_w && min_h == max_h) {
        return 0;
    }

    uint32_t edges = woodland_snap_cursor_edges(area, cursor_x, cursor_y, threshold);
    if (edges == 0 && outer) {
        edges = woodland_snap_edges_for(outer, area, threshold);
    }
    return edges;
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
