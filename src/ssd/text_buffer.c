// SPDX-License-Identifier: GPL-2.0-only
#include "text_buffer.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/interfaces/wlr_buffer.h>
#include "scene.h"
#include <wlr/util/log.h>

struct woodland_text_buffer {
    struct wlr_scene_buffer *scene_buffer;
    int width;
    int height;
};

/* 标题字体（对齐 labwc 默认主题 font_activewindow: sans, size 10） */
#define WOODLAND_SSD_FONT "Sans 10"

/* 给 pango layout 套用统一字体 */
static void apply_font(PangoLayout *layout)
{
    PangoFontDescription *desc = pango_font_description_from_string(WOODLAND_SSD_FONT);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

/* ==================== cairo 图像 surface → wlr_buffer ==================== */

static struct wlr_buffer_impl text_buffer_impl;

struct text_wlr_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void text_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct text_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_destroy(b->surface);
    wlr_buffer_finish(&b->base);
    free(b);
}

static bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                                              void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct text_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_flush(b->surface);
    *data = cairo_image_surface_get_data(b->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(b->surface);
    return true;
}

static void text_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    struct text_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_mark_dirty(b->surface);
}

static struct wlr_buffer_impl text_buffer_impl = {
    .destroy = text_buffer_destroy,
    .begin_data_ptr_access = text_buffer_begin_data_ptr_access,
    .end_data_ptr_access = text_buffer_end_data_ptr_access,
};

/* 创建 1x1 占位 buffer */
static struct wlr_buffer *text_create_placeholder(void)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    struct text_wlr_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    wlr_buffer_init(&b->base, &text_buffer_impl, 1, 1);
    return &b->base;
}

/* ==================== 公开 API ==================== */

struct woodland_text_buffer *woodland_text_buffer_create(struct wlr_scene_tree *parent)
{
    struct woodland_text_buffer *tb = calloc(1, sizeof(*tb));
    if (!tb) {
        return NULL;
    }
    struct wlr_buffer *placeholder = text_create_placeholder();
    if (!placeholder) {
        free(tb);
        return NULL;
    }
    tb->scene_buffer = wlr_scene_buffer_create(parent, placeholder);
    wlr_buffer_drop(placeholder); /* scene buffer 持有引用 */
    if (!tb->scene_buffer) {
        free(tb);
        return NULL;
    }
    return tb;
}

void woodland_text_buffer_destroy(struct woodland_text_buffer *tb)
{
    if (!tb) {
        return;
    }
    wlr_scene_node_destroy(&tb->scene_buffer->node);
    free(tb);
}

/* 计算文本宽度（逻辑像素），-1 表示空 */
static int text_measure(const char *text)
{
    if (!text || !*text) {
        return -1;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *c = cairo_create(surf);
    PangoLayout *layout = pango_cairo_create_layout(c);
    apply_font(layout);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    cairo_destroy(c);
    cairo_surface_destroy(surf);
    return w;
}

void woodland_text_buffer_update(struct woodland_text_buffer *tb, const char *text, int max_width,
                                 int height, const float color[4], float scale)
{
    if (!tb) {
        return;
    }
    if (!text || !*text) {
        /* 空文本：1x1 占位 */
        struct wlr_buffer *placeholder = text_create_placeholder();
        if (placeholder) {
            wlr_scene_buffer_set_buffer(tb->scene_buffer, placeholder);
            wlr_buffer_drop(placeholder);
        }
        tb->width = 1;
        tb->height = 1;
        return;
    }

    if (scale <= 0) {
        scale = 1.0f;
    }

    int natural_w = text_measure(text);
    if (natural_w <= 0) {
        natural_w = 1;
    }
    int logical_w = (max_width > 0 && natural_w > max_width) ? max_width : natural_w;
    if (logical_w <= 0) {
        logical_w = 1;
    }
    int logical_h = height > 0 ? height : 24;

    int px_w = (int)(logical_w * scale);
    int px_h = (int)(logical_h * scale);
    if (px_w <= 0)
        px_w = 1;
    if (px_h <= 0)
        px_h = 1;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *c = cairo_create(surf);

    /* 前景色（premultiplied 已是 0~1） */
    cairo_set_source_rgba(c, color[0], color[1], color[2], color[3]);

    PangoLayout *layout = pango_cairo_create_layout(c);
    apply_font(layout);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, logical_w * PANGO_SCALE);
    pango_layout_set_height(layout, logical_h * PANGO_SCALE);

    /* 垂直居中 */
    int text_h = 0;
    pango_layout_get_pixel_size(layout, NULL, &text_h);
    cairo_move_to(c, 0, (logical_h - text_h) / 2);

    pango_cairo_update_layout(c, layout);
    pango_cairo_show_layout(c, layout);
    g_object_unref(layout);

    cairo_surface_flush(surf);
    cairo_destroy(c);

    struct text_wlr_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return;
    }
    b->surface = surf;
    wlr_buffer_init(&b->base, &text_buffer_impl, logical_w, logical_h);

    wlr_scene_buffer_set_buffer(tb->scene_buffer, &b->base);
    wlr_buffer_drop(&b->base);

    tb->width = logical_w;
    tb->height = logical_h;
}

void woodland_text_buffer_set_position(struct woodland_text_buffer *tb, int x, int y)
{
    if (!tb) {
        return;
    }
    wlr_scene_node_set_position(&tb->scene_buffer->node, x, y);
}

int woodland_text_buffer_get_width(const struct woodland_text_buffer *tb)
{
    return tb ? tb->width : 0;
}

void woodland_text_buffer_set_enabled(struct woodland_text_buffer *tb, bool enabled)
{
    if (!tb || !tb->scene_buffer) {
        return;
    }
    wlr_scene_node_set_enabled(&tb->scene_buffer->node, enabled);
}

/* ==================== 按钮符号（cairo 画线） ==================== */

struct wlr_buffer *woodland_button_icon_buffer(enum woodland_ssd_button_icon icon, int size,
                                               bool inverted)
{
    if (size <= 0) {
        return NULL;
    }
    float scale = 1.0f;
    int px = (int)(size * scale);
    if (px <= 0)
        px = 1;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *c = cairo_create(surf);

    /* 符号颜色：取 Deepin 风格 #303030，反色（关闭按钮悬停/按压）时为纯白 */
    if (inverted) {
        cairo_set_source_rgba(c, 1.0, 1.0, 1.0, 1.0);
    } else {
        cairo_set_source_rgba(c, 0.188, 0.188, 0.188, 1.0);
    }
    cairo_set_line_width(c, 1.25);
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);

    double pad = size * 0.32;
    double s = size - 2 * pad;

    switch (icon) {
    case WOODLAND_SSD_ICON_ICONIFY:
        /* 居中横线 — */
        cairo_move_to(c, pad, size * 0.5);
        cairo_line_to(c, size - pad, size * 0.5);
        break;
    case WOODLAND_SSD_ICON_MAXIMIZE:
        /* 单个方框 □ */
        cairo_rectangle(c, pad, pad, s, s);
        break;
    case WOODLAND_SSD_ICON_RESTORE: {
        /* 两个重叠窗口 (restore / unmaximize，参考 GXDE base_light.svg) */
        double off = s * 0.25;
        double fw = s - off;
        /* 前方小窗 */
        cairo_rectangle(c, pad, pad + off, fw, fw);
        cairo_stroke(c);
        /* 后方小窗折线 */
        cairo_move_to(c, pad + off, pad + off);
        cairo_line_to(c, pad + off, pad);
        cairo_line_to(c, pad + s, pad);
        cairo_line_to(c, pad + s, pad + fw);
        cairo_line_to(c, pad + fw, pad + fw);
        break;
    }
    case WOODLAND_SSD_ICON_CLOSE:
        /* 细十字 × */
        cairo_move_to(c, pad, pad);
        cairo_line_to(c, size - pad, size - pad);
        cairo_move_to(c, size - pad, pad);
        cairo_line_to(c, pad, size - pad);
        break;
    }
    cairo_stroke(c);

    cairo_surface_flush(surf);
    cairo_destroy(c);

    struct text_wlr_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    wlr_buffer_init(&b->base, &text_buffer_impl, size, size);
    return &b->base;
}

struct wlr_scene_buffer *woodland_button_icon_create(struct wlr_scene_tree *parent,
                                                     enum woodland_ssd_button_icon icon, int size)
{
    struct wlr_buffer *buf = woodland_button_icon_buffer(icon, size, false);
    if (!buf) {
        return NULL;
    }
    struct wlr_scene_buffer *sb = wlr_scene_buffer_create(parent, buf);
    wlr_buffer_drop(buf);
    return sb;
}
