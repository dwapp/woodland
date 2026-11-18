// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <assert.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "effect.h"
#include "focus.h"
#include "icon.h"
#include "multitask_view.h"
#include "output.h"
#include "scene.h"
#include "selfcheck.h"
#include "session_lock.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "window_menu.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MV_WS_BAR_TOP 20
#define MV_WS_PILL_HEIGHT 44
#define MV_WS_PILL_GAP 8
#define MV_WS_PILL_MIN_W 50
#define MV_WS_PILL_MAX_W 108

#define MV_GRID_MARGIN_X 40
#define MV_GRID_TOP (MV_WS_BAR_TOP + MV_WS_PILL_HEIGHT + 24)
#define MV_GRID_BOTTOM_MARGIN 36
#define MV_CARD_GAP_X 24
#define MV_CARD_GAP_Y 24
#define MV_CARD_HEADER_H 36
#define MV_CARD_PADDING 8
#define MV_CARD_CLOSE_SIZE 22
#define MV_CARD_ICON_SIZE 20
#define MV_CARD_RADIUS 10.0

/* ==================== cairo 包装为 wlr_buffer ==================== */

struct mv_cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void mv_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct mv_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (b->surface) {
        cairo_surface_destroy(b->surface);
    }
    wlr_buffer_finish(&b->base);
    free(b);
}

static bool mv_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                                            void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct mv_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (!b || !b->surface || cairo_surface_status(b->surface) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    cairo_surface_flush(b->surface);
    *data = cairo_image_surface_get_data(b->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)cairo_image_surface_get_stride(b->surface);
    return true;
}

static void mv_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    struct mv_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_mark_dirty(b->surface);
}

static const struct wlr_buffer_impl mv_buffer_impl = {
    .destroy = mv_buffer_destroy,
    .begin_data_ptr_access = mv_buffer_begin_data_ptr_access,
    .end_data_ptr_access = mv_buffer_end_data_ptr_access,
};

static struct wlr_buffer *mv_wrap_cairo_surface(cairo_surface_t *surf, int width, int height)
{
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (surf) {
            cairo_surface_destroy(surf);
        }
        return NULL;
    }
    struct mv_cairo_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    int px_w = cairo_image_surface_get_width(surf);
    int px_h = cairo_image_surface_get_height(surf);
    wlr_buffer_init(&b->base, &mv_buffer_impl, px_w > 0 ? px_w : width, px_h > 0 ? px_h : height);
    return &b->base;
}

/* ==================== 几何与纯函数 ==================== */

static bool mv_is_zh_locale(void)
{
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang) {
        lang = getenv("LC_MESSAGES");
    }
    if (!lang || !*lang) {
        lang = getenv("LANG");
    }
    return lang && strncmp(lang, "zh", 2) == 0;
}

static void mv_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double max_r = (w < h ? w : h) / 2.0;
    if (r > max_r) {
        r = max_r;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void mv_fit_box_within(int src_w, int src_h, const struct wlr_box *bounds,
                              struct wlr_box *out)
{
    if (!bounds || !out || src_w <= 0 || src_h <= 0 || bounds->width <= 0 || bounds->height <= 0) {
        if (out) {
            *out = (struct wlr_box){0};
        }
        return;
    }
    double scale_x = (double)bounds->width / (double)src_w;
    double scale_y = (double)bounds->height / (double)src_h;
    double scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale > 1.0) {
        scale = 1.0;
    }
    int w = (int)round((double)src_w * scale);
    int h = (int)round((double)src_h * scale);
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    out->width = w;
    out->height = h;
    out->x = bounds->x + (bounds->width - w) / 2;
    out->y = bounds->y + (bounds->height - h) / 2;
}

void woodland_multitask_compute_grid(const struct wlr_box *area, int count,
                                     int *out_cols, int *out_rows,
                                     struct wlr_box *out_boxes)
{
    if (out_cols) {
        *out_cols = 0;
    }
    if (out_rows) {
        *out_rows = 0;
    }
    if (!area || count <= 0 || area->width <= 0 || area->height <= 0) {
        return;
    }
    if (count > WOODLAND_MULTITASK_MAX_CARDS) {
        count = WOODLAND_MULTITASK_MAX_CARDS;
    }

    int cols = 1;
    if (count == 1) {
        cols = 1;
    } else if (count == 2) {
        cols = 2;
    } else if (count == 3) {
        cols = 3;
    } else if (count == 4) {
        cols = 2;
    } else if (count <= 6) {
        cols = 3;
    } else {
        cols = (int)ceil(sqrt((double)count * 1.3));
        if (cols < 3) {
            cols = 3;
        }
    }
    int rows = (count + cols - 1) / cols;

    if (out_cols) {
        *out_cols = cols;
    }
    if (out_rows) {
        *out_rows = rows;
    }
    if (!out_boxes) {
        return;
    }

    int total_gap_x = (cols - 1) * MV_CARD_GAP_X;
    int total_gap_y = (rows - 1) * MV_CARD_GAP_Y;
    int cell_w = (area->width - total_gap_x) / cols;
    int cell_h = (area->height - total_gap_y) / rows;
    if (cell_w < 80) {
        cell_w = 80;
    }
    if (cell_h < 60) {
        cell_h = 60;
    }

    /* 单窗/双窗时限制最大卡片尺寸，避免占满全屏显得笨重 */
    int max_w = (count == 1) ? (area->width * 62 / 100) : (area->width * 46 / 100);
    int max_h = (count == 1) ? (area->height * 68 / 100) : (area->height * 60 / 100);
    if (max_w < 160) {
        max_w = cell_w;
    }
    if (max_h < 120) {
        max_h = cell_h;
    }

    int card_w = cell_w < max_w ? cell_w : max_w;
    int card_h = cell_h < max_h ? cell_h : max_h;

    /* 保持协调的宽高比（约 16:11，含顶部标题栏） */
    if (card_w > (card_h * 16) / 10) {
        card_w = (card_h * 16) / 10;
    } else if (card_h > (card_w * 11) / 12) {
        card_h = (card_w * 11) / 12;
    }
    if (card_w < 80) {
        card_w = 80;
    }
    if (card_h < 60) {
        card_h = 60;
    }

    int grid_h = rows * card_h + (rows - 1) * MV_CARD_GAP_Y;
    int start_y = area->y + (area->height - grid_h) / 2;
    if (start_y < area->y) {
        start_y = area->y;
    }

    for (int i = 0; i < count; i++) {
        int r = i / cols;
        int c = i % cols;
        int row_items = (r == rows - 1) ? (count - r * cols) : cols;
        int row_w = row_items * card_w + (row_items - 1) * MV_CARD_GAP_X;
        int start_x = area->x + (area->width - row_w) / 2;
        out_boxes[i].x = start_x + c * (card_w + MV_CARD_GAP_X);
        out_boxes[i].y = start_y + r * (card_h + MV_CARD_GAP_Y);
        out_boxes[i].width = card_w;
        out_boxes[i].height = card_h;
    }
}

/* ==================== 离屏窗口缩略图渲染（labwc osd-thumbnail 模式） ==================== */

struct mv_temp_tex {
    struct wlr_texture *textures[32];
    int count;
};

static bool mv_tree_has_client_buffer(struct wlr_scene_node *node)
{
    if (!node) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
        return sb && sb->buffer != NULL && wlr_scene_surface_try_from_buffer(sb) != NULL;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            if (mv_tree_has_client_buffer(child)) {
                return true;
            }
        }
    }
    return false;
}

static void mv_render_scene_node(struct wlr_renderer *renderer, struct wlr_render_pass *pass,
                                 struct wlr_scene_node *node, int x, int y,
                                 struct mv_temp_tex *temps)
{
    if (!node) {
        return;
    }
    switch (node->type) {
    case WLR_SCENE_NODE_TREE: {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            if (!child->enabled) {
                continue;
            }
            mv_render_scene_node(renderer, pass, child, x + child->x, y + child->y, temps);
        }
        break;
    }
    case WLR_SCENE_NODE_BUFFER: {
        struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
        if (!scene_buffer || !scene_buffer->buffer ||
            !wlr_scene_surface_try_from_buffer(scene_buffer)) {
            break;
        }
        struct wlr_texture *texture = NULL;
        struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
        if (client_buffer) {
            texture = client_buffer->texture;
        }
        if (!texture && renderer && temps && temps->count < 32) {
            texture = wlr_texture_from_buffer(renderer, scene_buffer->buffer);
            if (texture) {
                temps->textures[temps->count++] = texture;
            }
        }
        if (!texture) {
            break;
        }
        int dst_w = scene_buffer->dst_width > 0 ? scene_buffer->dst_width : (int)texture->width;
        int dst_h = scene_buffer->dst_height > 0 ? scene_buffer->dst_height : (int)texture->height;
        if (dst_w <= 0 || dst_h <= 0) {
            break;
        }
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture = texture,
            .src_box = scene_buffer->src_box,
            .dst_box = {
                .x = x,
                .y = y,
                .width = dst_w,
                .height = dst_h,
            },
            .transform = scene_buffer->transform,
            .filter_mode = WLR_SCALE_FILTER_BILINEAR,
        });
        break;
    }
    default:
        break;
    }
}

static const struct wlr_drm_format *mv_pick_render_format(struct woodland_server *server,
                                                          struct woodland_output *output)
{
    if (server && server->renderer) {
        const struct wlr_drm_format_set *formats =
            wlr_renderer_get_texture_formats(server->renderer, server->renderer->render_buffer_caps);
        if (formats) {
            static const uint32_t candidates[] = {
                DRM_FORMAT_ARGB8888,
                DRM_FORMAT_XRGB8888,
                DRM_FORMAT_ABGR8888,
                DRM_FORMAT_XBGR8888,
            };
            for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
                const struct wlr_drm_format *fmt = wlr_drm_format_set_get(formats, candidates[i]);
                if (fmt) {
                    return fmt;
                }
            }
        }
    }
    if (output && output->wlr_output && output->wlr_output->swapchain) {
        return &output->wlr_output->swapchain->format;
    }
    return NULL;
}

static struct wlr_buffer *mv_render_view_thumbnail(struct woodland_multitask_view *mv,
                                                   struct woodland_view *view)
{
    if (!mv || !mv->server || !view || !view->scene_tree) {
        return NULL;
    }
    struct woodland_server *server = mv->server;
    if (!server->renderer || !server->allocator) {
        return NULL;
    }

    if (!mv_tree_has_client_buffer(&view->scene_tree->node)) {
        return NULL;
    }

    struct wlr_box content = {0};
    woodland_view_content_box(view, &content);
    if (content.width <= 0 || content.height <= 0) {
        return NULL;
    }

    const struct wlr_drm_format *format = mv_pick_render_format(server, mv->output);
    if (!format) {
        return NULL;
    }

    struct wlr_buffer *buffer =
        wlr_allocator_create_buffer(server->allocator, content.width, content.height, format);
    if (!buffer) {
        return NULL;
    }

    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
    if (!pass) {
        wlr_buffer_drop(buffer);
        return NULL;
    }

    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = {0, 0, content.width, content.height},
        .color = {0.0f, 0.0f, 0.0f, 0.0f},
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    struct mv_temp_tex temps = {0};
    /* view->scene_tree 原点即内容原点，只累计子节点的相对偏移（见 AGENTS.md 约束 3） */
    mv_render_scene_node(server->renderer, pass, &view->scene_tree->node,
                         -content.x, -content.y, &temps);

    bool ok = wlr_render_pass_submit(pass);
    for (int i = 0; i < temps.count; i++) {
        wlr_texture_destroy(temps.textures[i]);
    }
    if (!ok) {
        wlr_buffer_drop(buffer);
        return NULL;
    }
    return buffer;
}

/* ==================== 配色与字体辅助 ==================== */

static void mv_get_accent_rgb(const struct woodland_server *server,
                              double *r, double *g, double *b)
{
    if (server && server->config && server->config->appearance.accent_set) {
        uint32_t rgb = server->config->appearance.accent_rgb;
        *r = ((rgb >> 16) & 0xFF) / 255.0;
        *g = ((rgb >> 8) & 0xFF) / 255.0;
        *b = (rgb & 0xFF) / 255.0;
        return;
    }
    *r = 0.20;
    *g = 0.52;
    *b = 0.96;
}

static const char *mv_get_font_desc(const struct woodland_server *server)
{
    if (server && server->config && server->config->ssd.font && server->config->ssd.font[0]) {
        return server->config->ssd.font;
    }
    return "Sans 10";
}

/* ==================== 工作区选择栏绘制 ==================== */

static int mv_count_workspace_views(const struct woodland_server *server,
                                    const struct woodland_workspace *ws)
{
    if (!server || !ws || !server->views.next) {
        return 0;
    }
    int count = 0;
    struct woodland_view *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped && !v->override_redirect && v->workspace == ws) {
            count++;
        }
    }
    return count;
}

static void mv_render_workspace_item(struct woodland_multitask_view *mv, int index)
{
    if (!mv || index < 0 || index >= mv->workspace_count || !mv->ws_bar_tree) {
        return;
    }
    struct multitask_workspace_item *it = &mv->workspaces[index];
    float scale = mv->scale > 0.0f ? mv->scale : 1.0f;
    int px_w = (int)ceil(it->box.width * scale);
    int px_h = (int)ceil(it->box.height * scale);
    if (px_w <= 0 || px_h <= 0) {
        return;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *cr = cairo_create(surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    bool is_selected = (it->workspace == mv->selected_workspace);
    bool is_hovered = (index == mv->hovered_workspace);
    double ar, ag, ab;
    mv_get_accent_rgb(mv->server, &ar, &ag, &ab);

    mv_rounded_rect(cr, 1.0, 1.0, it->box.width - 2.0, it->box.height - 2.0, 8.0);
    if (is_selected) {
        cairo_set_source_rgba(cr, ar, ag, ab, 0.88);
    } else if (is_hovered) {
        cairo_set_source_rgba(cr, 0.26, 0.29, 0.36, 0.88);
    } else {
        cairo_set_source_rgba(cr, 0.16, 0.18, 0.23, 0.82);
    }
    cairo_fill_preserve(cr);

    if (is_selected) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
        cairo_set_line_width(cr, 1.5);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_set_line_width(cr, 1.0);
    }
    cairo_stroke(cr);

    char label[32];
    const char *ws_name = (it->workspace && it->workspace->name) ? it->workspace->name : "?";
    if (it->window_count > 0) {
        snprintf(label, sizeof(label), "%s · %d", ws_name, it->window_count);
    } else {
        snprintf(label, sizeof(label), "%s", ws_name);
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(mv_get_font_desc(mv->server));
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }
    pango_layout_set_text(layout, label, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgba(cr, 0.96, 0.97, 0.99, is_selected ? 1.0 : 0.86);
    cairo_move_to(cr, (it->box.width - tw) / 2.0, (it->box.height - th) / 2.0);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    struct wlr_buffer *buf = mv_wrap_cairo_surface(surf, px_w, px_h);
    if (!buf) {
        return;
    }
    if (!it->scene_buffer) {
        it->scene_buffer = wlr_scene_buffer_create(mv->ws_bar_tree, buf);
    } else {
        wlr_scene_buffer_set_buffer(it->scene_buffer, buf);
    }
    if (it->scene_buffer) {
        wlr_scene_buffer_set_dest_size(it->scene_buffer, it->box.width, it->box.height);
        wlr_scene_node_set_position(&it->scene_buffer->node, it->box.x, it->box.y);
        wlr_scene_node_set_enabled(&it->scene_buffer->node, true);
    }
    wlr_buffer_drop(buf);
}

/* ==================== 窗口卡片绘制 ==================== */

static void mv_render_card_frame(struct woodland_multitask_view *mv, int index, bool has_live_thumb)
{
    if (!mv || index < 0 || index >= mv->card_count) {
        return;
    }
    struct multitask_card_item *card = &mv->cards[index];
    if (!card->tree) {
        return;
    }
    float scale = mv->scale > 0.0f ? mv->scale : 1.0f;
    int w = card->box.width;
    int h = card->box.height;
    int px_w = (int)ceil(w * scale);
    int px_h = (int)ceil(h * scale);
    if (px_w <= 0 || px_h <= 0) {
        return;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *cr = cairo_create(surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    bool is_selected = (index == mv->selected_index);
    bool is_hovered = (index == mv->hovered_card);
    bool close_hovered = (index == mv->hovered_close);
    double ar, ag, ab;
    mv_get_accent_rgb(mv->server, &ar, &ag, &ab);

    /* 1. 卡片底板 */
    mv_rounded_rect(cr, 1.5, 1.5, w - 3.0, h - 3.0, MV_CARD_RADIUS);
    if (is_selected) {
        cairo_set_source_rgba(cr, 0.18, 0.21, 0.27, 0.95);
    } else if (is_hovered) {
        cairo_set_source_rgba(cr, 0.16, 0.18, 0.24, 0.92);
    } else {
        cairo_set_source_rgba(cr, 0.13, 0.15, 0.20, 0.90);
    }
    cairo_fill(cr);

    /* 2. 缩略图内衬区域 */
    mv_rounded_rect(cr, card->thumb_box.x, card->thumb_box.y,
                    card->thumb_box.width, card->thumb_box.height, 6.0);
    cairo_set_source_rgba(cr, 0.08, 0.09, 0.12, 0.85);
    cairo_fill(cr);

    /* 无实时表面缓冲（如单测或空 surface）时画一个简洁的窗口轮廓占位 */
    if (!has_live_thumb && card->thumb_box.width > 24 && card->thumb_box.height > 24) {
        double pw = card->thumb_box.width * 0.6;
        double ph = card->thumb_box.height * 0.6;
        double px = card->thumb_box.x + (card->thumb_box.width - pw) / 2.0;
        double py = card->thumb_box.y + (card->thumb_box.height - ph) / 2.0;
        mv_rounded_rect(cr, px, py, pw, ph, 5.0);
        cairo_set_source_rgba(cr, 0.22, 0.26, 0.34, 0.65);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    /* 3. 顶部标题文字 */
    const char *title = (card->view && card->view->title && card->view->title[0])
                            ? card->view->title
                            : ((card->view && card->view->app_id && card->view->app_id[0])
                                   ? card->view->app_id
                                   : (mv_is_zh_locale() ? "未命名窗口" : "Untitled"));

    int text_left = MV_CARD_PADDING + MV_CARD_ICON_SIZE + 8;
    int text_right = w - MV_CARD_PADDING - MV_CARD_CLOSE_SIZE - 8;
    int max_text_w = text_right - text_left;
    if (max_text_w > 10) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *desc =
            pango_font_description_from_string(mv_get_font_desc(mv->server));
        if (desc) {
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);
        }
        pango_layout_set_width(layout, max_text_w * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(layout, true);
        pango_layout_set_text(layout, title, -1);

        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_set_source_rgba(cr, 0.95, 0.96, 0.98, 0.96);
        cairo_move_to(cr, text_left, (MV_CARD_HEADER_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    /* 4. 右上角关闭按钮 */
    double cx = (card->close_box.x - card->box.x) + card->close_box.width / 2.0;
    double cy = (card->close_box.y - card->box.y) + card->close_box.height / 2.0;
    double cr_rad = card->close_box.width / 2.0;
    cairo_arc(cr, cx, cy, cr_rad, 0, 2.0 * M_PI);
    if (close_hovered) {
        cairo_set_source_rgba(cr, 0.90, 0.28, 0.28, 0.95);
    } else if (is_selected || is_hovered) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
    }
    cairo_fill(cr);

    double cross = 4.0;
    cairo_set_source_rgba(cr, 0.96, 0.97, 0.99, (close_hovered || is_selected || is_hovered) ? 0.95 : 0.65);
    cairo_set_line_width(cr, 1.6);
    cairo_move_to(cr, cx - cross, cy - cross);
    cairo_line_to(cr, cx + cross, cy + cross);
    cairo_move_to(cr, cx + cross, cy - cross);
    cairo_line_to(cr, cx - cross, cy + cross);
    cairo_stroke(cr);

    /* 5. 外框高亮描边 */
    mv_rounded_rect(cr, 1.5, 1.5, w - 3.0, h - 3.0, MV_CARD_RADIUS);
    if (is_selected) {
        cairo_set_source_rgba(cr, ar, ag, ab, 0.95);
        cairo_set_line_width(cr, 2.5);
    } else if (is_hovered) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.38);
        cairo_set_line_width(cr, 1.5);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_set_line_width(cr, 1.0);
    }
    cairo_stroke(cr);

    cairo_destroy(cr);

    struct wlr_buffer *buf = mv_wrap_cairo_surface(surf, px_w, px_h);
    if (!buf) {
        return;
    }
    if (!card->frame_buffer) {
        card->frame_buffer = wlr_scene_buffer_create(card->tree, buf);
    } else {
        wlr_scene_buffer_set_buffer(card->frame_buffer, buf);
    }
    if (card->frame_buffer) {
        wlr_scene_buffer_set_dest_size(card->frame_buffer, w, h);
        wlr_scene_node_set_position(&card->frame_buffer->node, 0, 0);
    }
    wlr_buffer_drop(buf);
}

static void mv_render_empty_hint(struct woodland_multitask_view *mv)
{
    if (!mv || !mv->cards_tree) {
        return;
    }
    int w = 260;
    int h = 52;
    float scale = mv->scale > 0.0f ? mv->scale : 1.0f;
    int px_w = (int)ceil(w * scale);
    int px_h = (int)ceil(h * scale);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *cr = cairo_create(surf);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    mv_rounded_rect(cr, 1.0, 1.0, w - 2.0, h - 2.0, 10.0);
    cairo_set_source_rgba(cr, 0.14, 0.16, 0.21, 0.82);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const char *msg = mv_is_zh_locale() ? "当前工作区无打开窗口" : "No Open Windows";
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(mv_get_font_desc(mv->server));
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }
    pango_layout_set_text(layout, msg, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgba(cr, 0.85, 0.88, 0.92, 0.85);
    cairo_move_to(cr, (w - tw) / 2.0, (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    struct wlr_buffer *buf = mv_wrap_cairo_surface(surf, px_w, px_h);
    if (!buf) {
        return;
    }
    mv->empty_hint_buffer = wlr_scene_buffer_create(mv->cards_tree, buf);
    if (mv->empty_hint_buffer) {
        wlr_scene_buffer_set_dest_size(mv->empty_hint_buffer, w, h);
        wlr_scene_node_set_position(&mv->empty_hint_buffer->node,
                                    (mv->output_box.width - w) / 2,
                                    (mv->output_box.height - h) / 2);
    }
    wlr_buffer_drop(buf);
}

/* ==================== 场景树构建与刷新 ==================== */

static void mv_clear_cards(struct woodland_multitask_view *mv)
{
    if (!mv) {
        return;
    }
    for (int i = 0; i < mv->card_count; i++) {
        if (mv->cards[i].tree) {
            wlr_scene_node_destroy(&mv->cards[i].tree->node);
        }
    }
    if (mv->empty_hint_buffer) {
        wlr_scene_node_destroy(&mv->empty_hint_buffer->node);
        mv->empty_hint_buffer = NULL;
    }
    memset(mv->cards, 0, sizeof(mv->cards));
    mv->card_count = 0;
    mv->thumb_count = 0;
    mv->dropped_cards = 0;
    mv->grid_cols = 0;
    mv->grid_rows = 0;
    mv->selected_index = -1;
    mv->hovered_card = -1;
    mv->hovered_close = -1;
}

static void mv_clear_workspaces(struct woodland_multitask_view *mv)
{
    if (!mv) {
        return;
    }
    for (int i = 0; i < mv->workspace_count; i++) {
        if (mv->workspaces[i].scene_buffer) {
            wlr_scene_node_destroy(&mv->workspaces[i].scene_buffer->node);
        }
    }
    memset(mv->workspaces, 0, sizeof(mv->workspaces));
    mv->workspace_count = 0;
    mv->dropped_workspaces = 0;
    mv->hovered_workspace = -1;
}

static void mv_build_workspaces(struct woodland_multitask_view *mv)
{
    mv_clear_workspaces(mv);
    if (!mv || !mv->server || !mv->server->workspaces.next) {
        return;
    }

    struct woodland_workspace *ws;
    wl_list_for_each(ws, &mv->server->workspaces, link) {
        if (mv->workspace_count >= WOODLAND_MULTITASK_MAX_WORKSPACES) {
            mv->dropped_workspaces++;
            continue;
        }
        struct multitask_workspace_item *it = &mv->workspaces[mv->workspace_count++];
        it->workspace = ws;
        it->window_count = mv_count_workspace_views(mv->server, ws);
    }
    if (mv->dropped_workspaces > 0) {
        wlr_log(WLR_INFO, "multitask_view: truncated %d workspaces beyond limit %d",
                mv->dropped_workspaces, WOODLAND_MULTITASK_MAX_WORKSPACES);
    }
    if (mv->workspace_count == 0) {
        return;
    }

    int avail_w = mv->output_box.width - 2 * MV_GRID_MARGIN_X;
    if (avail_w < 200) {
        avail_w = mv->output_box.width - 16;
    }
    int pill_w = (avail_w - (mv->workspace_count - 1) * MV_WS_PILL_GAP) / mv->workspace_count;
    if (pill_w > MV_WS_PILL_MAX_W) {
        pill_w = MV_WS_PILL_MAX_W;
    }
    if (pill_w < MV_WS_PILL_MIN_W) {
        pill_w = MV_WS_PILL_MIN_W;
    }

    int total_w = mv->workspace_count * pill_w + (mv->workspace_count - 1) * MV_WS_PILL_GAP;
    int start_x = (mv->output_box.width - total_w) / 2;
    if (start_x < 8) {
        start_x = 8;
    }

    for (int i = 0; i < mv->workspace_count; i++) {
        mv->workspaces[i].box = (struct wlr_box){
            .x = start_x + i * (pill_w + MV_WS_PILL_GAP),
            .y = MV_WS_BAR_TOP,
            .width = pill_w,
            .height = MV_WS_PILL_HEIGHT,
        };
        mv_render_workspace_item(mv, i);
    }
}

static void mv_build_cards(struct woodland_multitask_view *mv, struct woodland_view *prefer_selected)
{
    mv_clear_cards(mv);
    if (!mv || !mv->server || !mv->server->views.next) {
        return;
    }

    struct woodland_view *v;
    wl_list_for_each(v, &mv->server->views, link) {
        if (!v->mapped || v->override_redirect) {
            continue;
        }
        if (mv->selected_workspace && v->workspace != mv->selected_workspace) {
            continue;
        }
        if (mv->card_count >= WOODLAND_MULTITASK_MAX_CARDS) {
            mv->dropped_cards++;
            continue;
        }
        mv->cards[mv->card_count++].view = v;
    }
    if (mv->dropped_cards > 0) {
        wlr_log(WLR_INFO, "multitask_view: truncated %d cards beyond limit %d",
                mv->dropped_cards, WOODLAND_MULTITASK_MAX_CARDS);
    }

    if (mv->card_count == 0) {
        mv_render_empty_hint(mv);
        return;
    }

    mv->selected_index = 0;
    if (prefer_selected) {
        for (int i = 0; i < mv->card_count; i++) {
            if (mv->cards[i].view == prefer_selected) {
                mv->selected_index = i;
                break;
            }
        }
    }

    struct wlr_box grid_area = {
        .x = MV_GRID_MARGIN_X,
        .y = MV_GRID_TOP,
        .width = mv->output_box.width - 2 * MV_GRID_MARGIN_X,
        .height = mv->output_box.height - MV_GRID_TOP - MV_GRID_BOTTOM_MARGIN,
    };
    if (grid_area.width < 120) {
        grid_area.x = 8;
        grid_area.width = mv->output_box.width - 16;
    }
    if (grid_area.height < 100) {
        grid_area.y = MV_WS_BAR_TOP + MV_WS_PILL_HEIGHT + 8;
        grid_area.height = mv->output_box.height - grid_area.y - 8;
    }

    struct wlr_box boxes[WOODLAND_MULTITASK_MAX_CARDS];
    woodland_multitask_compute_grid(&grid_area, mv->card_count,
                                    &mv->grid_cols, &mv->grid_rows, boxes);

    for (int i = 0; i < mv->card_count; i++) {
        struct multitask_card_item *card = &mv->cards[i];
        card->box = boxes[i];
        card->thumb_box = (struct wlr_box){
            .x = MV_CARD_PADDING,
            .y = MV_CARD_HEADER_H,
            .width = card->box.width - 2 * MV_CARD_PADDING,
            .height = card->box.height - MV_CARD_HEADER_H - MV_CARD_PADDING,
        };
        card->close_box = (struct wlr_box){
            .x = card->box.x + card->box.width - MV_CARD_PADDING - MV_CARD_CLOSE_SIZE,
            .y = card->box.y + (MV_CARD_HEADER_H - MV_CARD_CLOSE_SIZE) / 2,
            .width = MV_CARD_CLOSE_SIZE,
            .height = MV_CARD_CLOSE_SIZE,
        };

        if (!mv->cards_tree) {
            continue;
        }
        card->tree = wlr_scene_tree_create(mv->cards_tree);
        if (!card->tree) {
            continue;
        }
        wlr_scene_node_set_position(&card->tree->node, card->box.x, card->box.y);

#if WOODLAND_HAVE_EFFECTS
        if (woodland_effect_server_enabled(mv->server)) {
            float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.35f};
            struct clipped_region clip = {
                .area = {0, 0, card->box.width, card->box.height},
                .corners = {(int)MV_CARD_RADIUS, (int)MV_CARD_RADIUS,
                            (int)MV_CARD_RADIUS, (int)MV_CARD_RADIUS},
            };
            card->shadow = wlr_scene_shadow_create(card->tree, card->box.width + 20,
                                                   card->box.height + 20, (int)MV_CARD_RADIUS,
                                                   10.0f, shadow_color);
            if (card->shadow) {
                wlr_scene_node_set_position(&card->shadow->node, -10, -6);
                wlr_scene_shadow_set_clipped_region(card->shadow, clip);
            }
        }
#endif

        struct wlr_buffer *thumb_buf = mv_render_view_thumbnail(mv, card->view);
        mv_render_card_frame(mv, i, thumb_buf != NULL);

        if (thumb_buf) {
            card->thumb_buffer = wlr_scene_buffer_create(card->tree, thumb_buf);
            if (card->thumb_buffer) {
                struct wlr_box fitted = {0};
                mv_fit_box_within(thumb_buf->width, thumb_buf->height, &card->thumb_box, &fitted);
                wlr_scene_buffer_set_dest_size(card->thumb_buffer, fitted.width, fitted.height);
                wlr_scene_node_set_position(&card->thumb_buffer->node, fitted.x, fitted.y);
                mv->thumb_count++;
            }
            wlr_buffer_drop(thumb_buf);
        }

        struct wlr_buffer *icon_buf =
            woodland_ssd_get_icon_buffer(card->view, MV_CARD_ICON_SIZE, mv->scale);
        if (icon_buf) {
            card->icon_buffer = wlr_scene_buffer_create(card->tree, icon_buf);
            if (card->icon_buffer) {
                wlr_scene_buffer_set_dest_size(card->icon_buffer, MV_CARD_ICON_SIZE,
                                               MV_CARD_ICON_SIZE);
                wlr_scene_node_set_position(&card->icon_buffer->node, MV_CARD_PADDING,
                                            (MV_CARD_HEADER_H - MV_CARD_ICON_SIZE) / 2);
            }
        }
    }
}

static bool mv_ensure_scene_trees(struct woodland_multitask_view *mv)
{
    if (!mv || !mv->server || !mv->server->scene) {
        return false;
    }
    if (!mv->tree) {
        mv->tree = wlr_scene_tree_create(&mv->server->scene->tree);
        if (!mv->tree) {
            return false;
        }
        wlr_scene_node_set_enabled(&mv->tree->node, false);
    }
    if (!mv->backdrop) {
        float bg[4] = {0.07f * 0.78f, 0.08f * 0.78f, 0.11f * 0.78f, 0.78f};
        mv->backdrop = wlr_scene_rect_create(mv->tree, 0, 0, bg);
    }
    if (!mv->ws_bar_tree) {
        mv->ws_bar_tree = wlr_scene_tree_create(mv->tree);
    }
    if (!mv->cards_tree) {
        mv->cards_tree = wlr_scene_tree_create(mv->tree);
    }
    return mv->backdrop && mv->ws_bar_tree && mv->cards_tree;
}

static void mv_resolve_output(struct woodland_multitask_view *mv)
{
    struct woodland_server *server = mv->server;
    struct woodland_output *out = NULL;
    if (server->cursor) {
        out = woodland_output_at(server, server->cursor->x, server->cursor->y);
    }
    if (!out && server->focused_view) {
        out = woodland_output_for_view(server->focused_view);
    }
    if (!out) {
        out = woodland_output_first(server);
    }
    mv->output = out;
    mv->scale = (out && out->wlr_output && out->wlr_output->scale > 0.0f)
                    ? out->wlr_output->scale
                    : 1.0f;

    struct wlr_box box = {0};
    if (out && out->wlr_output && server->output_layout) {
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
    }
    if (box.width <= 0 || box.height <= 0) {
        if (out && out->wlr_output) {
            wlr_output_effective_resolution(out->wlr_output, &box.width, &box.height);
        }
    }
    if (box.width <= 0 || box.height <= 0) {
        box = (struct wlr_box){0, 0, 1280, 720};
    }
    mv->output_box = box;
}

static void mv_raise_tree(struct woodland_multitask_view *mv)
{
    if (!mv || !mv->tree) {
        return;
    }
    wlr_scene_node_raise_to_top(&mv->tree->node);
    /* 若此刻正处于拖拽，保持 dnd->icons 在最顶层以满足 selfcheck 不变量 */
    if (mv->server && mv->server->dnd.drag && mv->server->dnd.icons) {
        wlr_scene_node_raise_to_top(&mv->server->dnd.icons->node);
    }
}

static void mv_rebuild_all(struct woodland_multitask_view *mv,
                           struct woodland_view *prefer_selected)
{
    mv_resolve_output(mv);
    if (mv_ensure_scene_trees(mv)) {
        wlr_scene_node_set_position(&mv->tree->node, mv->output_box.x, mv->output_box.y);
        wlr_scene_rect_set_size(mv->backdrop, mv->output_box.width, mv->output_box.height);
        struct wlr_box blur_reg = {0, 0, mv->output_box.width, mv->output_box.height};
        if (woodland_effect_server_blur_enabled(mv->server)) {
            woodland_effect_blur_apply(mv->tree, 0, 0, mv->output_box.width, mv->output_box.height,
                                       &blur_reg, NULL);
        } else {
            woodland_effect_blur_clear(mv->tree);
        }
    }
    mv_build_workspaces(mv);
    mv_build_cards(mv, prefer_selected);
}

/* ==================== 公开生命周期与开关接口 ==================== */

bool woodland_multitask_view_init(struct woodland_server *server)
{
    if (!server) {
        return false;
    }
    if (server->multitask_view) {
        return true;
    }
    struct woodland_multitask_view *mv = calloc(1, sizeof(*mv));
    if (!mv) {
        return false;
    }
    mv->server = server;
    mv->selected_index = -1;
    mv->hovered_workspace = -1;
    mv->hovered_card = -1;
    mv->hovered_close = -1;
    if (server->scene) {
        if (!mv_ensure_scene_trees(mv)) {
            if (mv->tree) {
                wlr_scene_node_destroy(&mv->tree->node);
            }
            free(mv);
            return false;
        }
    }
    server->multitask_view = mv;
    return true;
}

void woodland_multitask_view_finish(struct woodland_server *server)
{
    if (!server || !server->multitask_view) {
        return;
    }
    struct woodland_multitask_view *mv = server->multitask_view;
    woodland_multitask_view_close(server, false);
    if (mv->tree) {
        wlr_scene_node_destroy(&mv->tree->node);
        mv->tree = NULL;
        mv->backdrop = NULL;
        mv->ws_bar_tree = NULL;
        mv->cards_tree = NULL;
    }
    free(mv);
    server->multitask_view = NULL;
}

bool woodland_multitask_view_is_active(const struct woodland_server *server)
{
    return server && server->multitask_view && server->multitask_view->active;
}

void woodland_multitask_view_open(struct woodland_server *server)
{
    if (!server || woodland_session_lock_is_locked(server)) {
        return;
    }
    if (!server->multitask_view && !woodland_multitask_view_init(server)) {
        return;
    }
    struct woodland_multitask_view *mv = server->multitask_view;

    /* 打开多任务视图前收起窗口菜单、结束移动/缩放交互 */
    woodland_window_menu_hide(server);
    if (server->cursor_mode != WOODLAND_CURSOR_PASSTHROUGH) {
        woodland_cursor_reset_mode(server);
    }

    mv->active = true;
    mv->selected_workspace = server->active_workspace;
    if (!mv->selected_workspace && server->workspaces.next &&
        !wl_list_empty(&server->workspaces)) {
        mv->selected_workspace =
            wl_container_of(server->workspaces.next, mv->selected_workspace, link);
    }

    mv_rebuild_all(mv, server->focused_view);

    if (mv->tree) {
        wlr_scene_node_set_enabled(&mv->tree->node, true);
        mv_raise_tree(mv);
    }
    if (server->seat) {
        wlr_seat_pointer_notify_clear_focus(server->seat);
    }

    woodland_trace_add("multitask",
                       (mv->selected_index >= 0 && mv->cards[mv->selected_index].view)
                           ? mv->cards[mv->selected_index].view->id
                           : 0,
                       "open ws=%s cards=%d",
                       (mv->selected_workspace && mv->selected_workspace->name)
                           ? mv->selected_workspace->name
                           : "-",
                       mv->card_count);
    woodland_selfcheck_schedule(server, "multitask_open");
}

void woodland_multitask_view_close(struct woodland_server *server, bool activate_selected)
{
    if (!server || !server->multitask_view || !server->multitask_view->active) {
        return;
    }
    struct woodland_multitask_view *mv = server->multitask_view;
    struct woodland_view *target_view = NULL;
    if (activate_selected && mv->selected_index >= 0 && mv->selected_index < mv->card_count) {
        target_view = mv->cards[mv->selected_index].view;
    }
    struct woodland_workspace *target_ws = mv->selected_workspace;

    mv->active = false;
    mv_clear_cards(mv);
    mv_clear_workspaces(mv);
    if (mv->backdrop) {
        wlr_scene_rect_set_size(mv->backdrop, 0, 0);
    }
    if (mv->tree) {
        woodland_effect_blur_clear(mv->tree);
        wlr_scene_node_set_enabled(&mv->tree->node, false);
    }

    if (activate_selected) {
        if (target_view && target_view->mapped) {
            if (target_view->workspace && target_view->workspace != server->active_workspace) {
                woodland_workspace_switch_focus(server, target_view->workspace, false);
            }
            if (target_view->minimized) {
                woodland_view_set_minimized(target_view, false);
            }
            woodland_focus_view(target_view, true);
            if (!woodland_view_surface(target_view)) {
                server->focused_view = target_view;
            }
        } else if (target_ws && target_ws != server->active_workspace) {
            woodland_workspace_switch(server, target_ws);
        }
    }

    woodland_trace_add("multitask", target_view ? target_view->id : 0, "close activate=%d",
                       activate_selected ? 1 : 0);
    woodland_selfcheck_schedule(server, "multitask_close");
}

void woodland_multitask_view_toggle(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    if (woodland_multitask_view_is_active(server)) {
        woodland_multitask_view_close(server, false);
    } else {
        woodland_multitask_view_open(server);
    }
}

void woodland_multitask_view_handle_view_unmap(struct woodland_view *view)
{
    if (!view || !view->server || !woodland_multitask_view_is_active(view->server)) {
        return;
    }
    struct woodland_multitask_view *mv = view->server->multitask_view;
    bool found = false;
    int prev_selected = mv->selected_index;
    for (int i = 0; i < mv->card_count; i++) {
        if (mv->cards[i].view == view) {
            found = true;
            break;
        }
    }
    if (!found && view->workspace != mv->selected_workspace) {
        /* 仅刷新顶栏工作区窗口计数 */
        mv_build_workspaces(mv);
        return;
    }

    struct woodland_view *next_prefer = NULL;
    if (prev_selected >= 0 && prev_selected < mv->card_count) {
        if (mv->cards[prev_selected].view != view) {
            next_prefer = mv->cards[prev_selected].view;
        } else if (prev_selected + 1 < mv->card_count) {
            next_prefer = mv->cards[prev_selected + 1].view;
        } else if (prev_selected - 1 >= 0) {
            next_prefer = mv->cards[prev_selected - 1].view;
        }
    }
    mv_build_workspaces(mv);
    mv_build_cards(mv, next_prefer);
}

/* ==================== 指针与键盘交互 ==================== */

static bool box_contains_point(const struct wlr_box *box, double x, double y)
{
    return box && x >= box->x && x < (box->x + box->width) &&
           y >= box->y && y < (box->y + box->height);
}

static void mv_select_workspace(struct woodland_multitask_view *mv,
                                struct woodland_workspace *ws)
{
    if (!mv || !ws || mv->selected_workspace == ws) {
        return;
    }
    mv->selected_workspace = ws;
    if (mv->server->active_workspace != ws) {
        woodland_workspace_switch(mv->server, ws);
    }
    mv_build_workspaces(mv);
    mv_build_cards(mv, mv->server->focused_view);
    if (mv->tree) {
        mv_raise_tree(mv);
    }
    woodland_trace_add("multitask", 0, "switch_workspace ws=%s cards=%d",
                       ws->name ? ws->name : "-", mv->card_count);
}

void woodland_multitask_view_handle_motion(struct woodland_server *server, double lx, double ly)
{
    if (!woodland_multitask_view_is_active(server)) {
        return;
    }
    struct woodland_multitask_view *mv = server->multitask_view;
    double rx = lx - mv->output_box.x;
    double ry = ly - mv->output_box.y;

    int new_ws = -1;
    for (int i = 0; i < mv->workspace_count; i++) {
        if (box_contains_point(&mv->workspaces[i].box, rx, ry)) {
            new_ws = i;
            break;
        }
    }
    if (new_ws != mv->hovered_workspace) {
        int old_ws = mv->hovered_workspace;
        mv->hovered_workspace = new_ws;
        if (old_ws >= 0) {
            mv_render_workspace_item(mv, old_ws);
        }
        if (new_ws >= 0) {
            mv_render_workspace_item(mv, new_ws);
        }
    }

    int new_card = -1;
    int new_close = -1;
    for (int i = 0; i < mv->card_count; i++) {
        if (box_contains_point(&mv->cards[i].box, rx, ry)) {
            new_card = i;
            if (box_contains_point(&mv->cards[i].close_box, rx, ry)) {
                new_close = i;
            }
            break;
        }
    }

    int old_card = mv->hovered_card;
    int old_close = mv->hovered_close;
    int old_sel = mv->selected_index;

    mv->hovered_card = new_card;
    mv->hovered_close = new_close;
    if (new_card >= 0) {
        mv->selected_index = new_card;
    }

    if (old_card != mv->hovered_card || old_close != mv->hovered_close ||
        old_sel != mv->selected_index) {
        for (int i = 0; i < mv->card_count; i++) {
            if (i == old_card || i == new_card || i == old_sel || i == mv->selected_index ||
                i == old_close || i == new_close) {
                mv_render_card_frame(mv, i, mv->cards[i].thumb_buffer != NULL);
            }
        }
    }
}

bool woodland_multitask_view_handle_button(struct woodland_server *server, uint32_t button,
                                           uint32_t state, double lx, double ly)
{
    if (!woodland_multitask_view_is_active(server)) {
        return false;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return true;
    }

    struct woodland_multitask_view *mv = server->multitask_view;
    double rx = lx - mv->output_box.x;
    double ry = ly - mv->output_box.y;

    if (button == BTN_RIGHT) {
        woodland_multitask_view_close(server, false);
        return true;
    }

    if (button == BTN_MIDDLE) {
        for (int i = 0; i < mv->card_count; i++) {
            if (box_contains_point(&mv->cards[i].box, rx, ry)) {
                struct woodland_view *v = mv->cards[i].view;
                if (v) {
                    woodland_trace_add("multitask", v->id, "close_view button=middle");
                    woodland_view_close(v);
                }
                return true;
            }
        }
        return true;
    }

    if (button != BTN_LEFT) {
        return true;
    }

    /* 1. 命中顶部工作区栏 */
    for (int i = 0; i < mv->workspace_count; i++) {
        if (box_contains_point(&mv->workspaces[i].box, rx, ry)) {
            mv_select_workspace(mv, mv->workspaces[i].workspace);
            return true;
        }
    }

    /* 2. 命中窗口卡片或其关闭按钮 */
    for (int i = 0; i < mv->card_count; i++) {
        if (box_contains_point(&mv->cards[i].box, rx, ry)) {
            struct woodland_view *v = mv->cards[i].view;
            if (box_contains_point(&mv->cards[i].close_box, rx, ry)) {
                if (v) {
                    woodland_trace_add("multitask", v->id, "close_view button=close_icon");
                    woodland_view_close(v);
                }
                return true;
            }
            mv->selected_index = i;
            woodland_multitask_view_close(server, true);
            return true;
        }
    }

    /* 3. 点击空白背景关闭 */
    woodland_multitask_view_close(server, false);
    return true;
}

static void mv_set_selected_index(struct woodland_multitask_view *mv, int next_index)
{
    if (!mv || mv->card_count <= 0) {
        return;
    }
    if (next_index < 0) {
        next_index = 0;
    }
    if (next_index >= mv->card_count) {
        next_index = mv->card_count - 1;
    }
    if (next_index == mv->selected_index) {
        return;
    }
    int prev = mv->selected_index;
    mv->selected_index = next_index;
    if (prev >= 0 && prev < mv->card_count) {
        mv_render_card_frame(mv, prev, mv->cards[prev].thumb_buffer != NULL);
    }
    mv_render_card_frame(mv, next_index, mv->cards[next_index].thumb_buffer != NULL);
}

bool woodland_multitask_view_handle_key(struct woodland_server *server, xkb_keysym_t sym,
                                        uint32_t modifiers)
{
    if (!woodland_multitask_view_is_active(server)) {
        return false;
    }
    struct woodland_multitask_view *mv = server->multitask_view;

    /* 带 Ctrl/Meta 修饰键的组合交给全局快捷键判断（例如 Meta+S 再次切换关闭） */
    if (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO)) {
        return false;
    }

    switch (sym) {
    case XKB_KEY_Escape:
        woodland_multitask_view_close(server, false);
        return true;
    case XKB_KEY_s:
    case XKB_KEY_S:
        /* 与内置 Alt+S toggle 对齐：打开后再按 Alt+S 关闭 */
        if (modifiers & WLR_MODIFIER_ALT) {
            woodland_multitask_view_close(server, false);
            return true;
        }
        return false;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        woodland_multitask_view_close(server, true);
        return true;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab: {
        if (mv->card_count > 0) {
            bool backward = (sym == XKB_KEY_ISO_Left_Tab) || ((modifiers & WLR_MODIFIER_SHIFT) != 0);
            int cur = mv->selected_index >= 0 ? mv->selected_index : 0;
            int next = backward ? (cur - 1 + mv->card_count) % mv->card_count
                                : (cur + 1) % mv->card_count;
            mv_set_selected_index(mv, next);
        }
        return true;
    }
    case XKB_KEY_Left: {
        if (mv->card_count > 0) {
            int cur = mv->selected_index >= 0 ? mv->selected_index : 0;
            mv_set_selected_index(mv, cur > 0 ? cur - 1 : mv->card_count - 1);
        }
        return true;
    }
    case XKB_KEY_Right: {
        if (mv->card_count > 0) {
            int cur = mv->selected_index >= 0 ? mv->selected_index : 0;
            mv_set_selected_index(mv, (cur + 1) % mv->card_count);
        }
        return true;
    }
    case XKB_KEY_Up: {
        if (mv->card_count > 0 && mv->grid_cols > 0) {
            int cur = mv->selected_index >= 0 ? mv->selected_index : 0;
            if (cur - mv->grid_cols >= 0) {
                mv_set_selected_index(mv, cur - mv->grid_cols);
            }
        }
        return true;
    }
    case XKB_KEY_Down: {
        if (mv->card_count > 0 && mv->grid_cols > 0) {
            int cur = mv->selected_index >= 0 ? mv->selected_index : 0;
            int next = cur + mv->grid_cols;
            if (next >= mv->card_count && (cur / mv->grid_cols) < (mv->grid_rows - 1)) {
                next = mv->card_count - 1;
            }
            if (next < mv->card_count) {
                mv_set_selected_index(mv, next);
            }
        }
        return true;
    }
    case XKB_KEY_Delete:
    case XKB_KEY_BackSpace: {
        if (mv->selected_index >= 0 && mv->selected_index < mv->card_count) {
            struct woodland_view *v = mv->cards[mv->selected_index].view;
            if (v) {
                woodland_trace_add("multitask", v->id, "close_view key=delete");
                woodland_view_close(v);
            }
        }
        return true;
    }
    case XKB_KEY_Prior:
    case XKB_KEY_Next: {
        if (mv->selected_workspace && server->workspaces.next &&
            !wl_list_empty(&server->workspaces)) {
            struct wl_list *link = (sym == XKB_KEY_Next) ? mv->selected_workspace->link.next
                                                         : mv->selected_workspace->link.prev;
            if (link != &server->workspaces) {
                struct woodland_workspace *adj = wl_container_of(link, adj, link);
                mv_select_workspace(mv, adj);
            }
        }
        return true;
    }
    case XKB_KEY_1:
    case XKB_KEY_2:
    case XKB_KEY_3:
    case XKB_KEY_4:
    case XKB_KEY_5:
    case XKB_KEY_6:
    case XKB_KEY_7:
    case XKB_KEY_8:
    case XKB_KEY_9:
    case XKB_KEY_0: {
        char name[4];
        if (sym == XKB_KEY_0) {
            snprintf(name, sizeof(name), "10");
        } else {
            snprintf(name, sizeof(name), "%d", (int)(sym - XKB_KEY_1 + 1));
        }
        struct woodland_workspace *ws = woodland_workspace_by_name(server, name);
        if (ws) {
            mv_select_workspace(mv, ws);
        }
        return true;
    }
    default:
        return false;
    }
}

void woodland_multitask_view_status(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    bool active = woodland_multitask_view_is_active(server);
    woodland_json_obj_bool(&obj, "active", active);
    if (active && server->multitask_view) {
        struct woodland_multitask_view *mv = server->multitask_view;
        woodland_json_obj_str(&obj, "workspace",
                              mv->selected_workspace ? mv->selected_workspace->name : NULL);
        woodland_json_obj_int(&obj, "card_count", mv->card_count);
        woodland_json_obj_int(&obj, "thumb_count", mv->thumb_count);
        woodland_json_obj_int(&obj, "dropped_cards", mv->dropped_cards);
        woodland_json_obj_int(&obj, "dropped_workspaces", mv->dropped_workspaces);
        woodland_json_obj_int(&obj, "selected_index", mv->selected_index);
        if (mv->selected_index >= 0 && mv->selected_index < mv->card_count &&
            mv->cards[mv->selected_index].view) {
            woodland_json_obj_int(&obj, "selected_view",
                                  (int64_t)mv->cards[mv->selected_index].view->id);
        } else {
            woodland_json_obj_str(&obj, "selected_view", NULL);
        }
    }
    woodland_json_obj_close(&obj);
}
