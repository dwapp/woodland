// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xwayland 原生分辨率坐标映射：见 include/xwayland_scale.h。
 *
 * 做法参考 Umbriel（src/xwayland/outputs.cpp）：每个输出在 X 坐标下按物理
 * 尺寸摆放，位置按所有输出里最大的缩放放大——这样输出之间保持原有排布，
 * 且无论各自缩放多少都不会重叠。
 */
#include "config.h"
#include "xwayland_scale.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "output.h"
#include "placement.h"
#include "woodland.h"

static int scaled(int value, double factor)
{
    return (int)lround((double)value * factor);
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

/* 点到矩形的距离平方；点在矩形内为 0 */
static long long distance_squared(const struct wlr_box *box, int x, int y)
{
    long long dx = max_int(max_int(box->x - x, 0), x - (box->x + box->width - 1));
    long long dy = max_int(max_int(box->y - y, 0), y - (box->y + box->height - 1));
    return dx * dx + dy * dy;
}

static bool region_equal(const struct woodland_xwl_region *a, const struct woodland_xwl_region *b)
{
    return wlr_box_equal(&a->layout, &b->layout) && wlr_box_equal(&a->x, &b->x) &&
           a->scale == b->scale;
}

int woodland_xwl_to_x(const struct woodland_xwl_region *r, int layout_x)
{
    return r->x.x + scaled(layout_x - r->layout.x, r->scale);
}

int woodland_xwl_to_y(const struct woodland_xwl_region *r, int layout_y)
{
    return r->x.y + scaled(layout_y - r->layout.y, r->scale);
}

int woodland_xwl_to_layout_x(const struct woodland_xwl_region *r, int x)
{
    return r->layout.x + scaled(x - r->x.x, 1.0 / r->scale);
}

int woodland_xwl_to_layout_y(const struct woodland_xwl_region *r, int y)
{
    return r->layout.y + scaled(y - r->x.y, 1.0 / r->scale);
}

int woodland_xwl_to_x_width(const struct woodland_xwl_region *r, int width)
{
    return width == r->layout.width ? r->x.width : scaled(width, r->scale);
}

int woodland_xwl_to_x_height(const struct woodland_xwl_region *r, int height)
{
    return height == r->layout.height ? r->x.height : scaled(height, r->scale);
}

int woodland_xwl_to_layout_width(const struct woodland_xwl_region *r, int width)
{
    return width == r->x.width ? r->layout.width : scaled(width, 1.0 / r->scale);
}

int woodland_xwl_to_layout_height(const struct woodland_xwl_region *r, int height)
{
    return height == r->x.height ? r->layout.height : scaled(height, 1.0 / r->scale);
}

bool woodland_xwl_map_update(struct woodland_xwl_map *map, struct wlr_output_layout *layout)
{
    struct woodland_xwl_map next;
    memset(&next, 0, sizeof(next));
    next.enabled = map->enabled;
    next.scale = 1.0;

    struct wlr_box extents = {0};
    if (layout) {
        wlr_output_layout_get_box(layout, NULL, &extents);
    }

    double position_scale = 0;
    struct wlr_output_layout_output *lo;
    if (layout) {
        wl_list_for_each(lo, &layout->outputs, link)
        {
            if (next.count >= WOODLAND_XWL_MAX_OUTPUTS) {
                break;
            }
            struct wlr_output *output = lo->output;
            struct wlr_box box = {0};
            wlr_output_layout_get_box(layout, output, &box);
            if (wlr_box_empty(&box)) {
                continue;
            }
            struct woodland_xwl_region region = {
                .layout = box,
                .x = {.x = 0, .y = 0, .width = box.width, .height = box.height},
                .scale = 1.0,
            };
            if (map->enabled && output->scale > 0.0f) {
                region.scale = output->scale;
                wlr_output_transformed_resolution(output, &region.x.width, &region.x.height);
            }
            if (region.scale > position_scale) {
                position_scale = region.scale;
            }
            next.outputs[next.count] = output;
            next.regions[next.count] = region;
            next.count++;
        }
    }
    /* 最大缩放把输出之间拉开得至少和任何一个输出放大得一样多 */
    for (int i = 0; i < next.count; i++) {
        next.regions[i].x.x = scaled(next.regions[i].layout.x - extents.x, position_scale);
        next.regions[i].x.y = scaled(next.regions[i].layout.y - extents.y, position_scale);
    }
    next.fallback = (struct woodland_xwl_region){
        .layout = {.x = extents.x, .y = extents.y, .width = 0, .height = 0},
        .scale = 1.0,
    };
    next.scale = next.count > 0 ? position_scale : 1.0;

    bool changed = next.count != map->count || next.scale != map->scale ||
                   !region_equal(&next.fallback, &map->fallback);
    for (int i = 0; !changed && i < next.count; i++) {
        changed = next.outputs[i] != map->outputs[i] ||
                  !region_equal(&next.regions[i], &map->regions[i]);
    }
    *map = next;
    return changed;
}

void woodland_xwl_map_region(const struct woodland_xwl_map *map, const struct wlr_output *output,
                             int lx, int ly, struct woodland_xwl_region *out)
{
    const struct woodland_xwl_region *nearest = NULL;
    long long nearest_distance = LLONG_MAX;
    for (int i = 0; i < map->count; i++) {
        if (output && map->outputs[i] == output) {
            *out = map->regions[i];
            return;
        }
        long long d = distance_squared(&map->regions[i].layout, lx, ly);
        if (d < nearest_distance) {
            nearest = &map->regions[i];
            nearest_distance = d;
        }
    }
    *out = nearest ? *nearest : map->fallback;
}

void woodland_xwl_map_region_at_x(const struct woodland_xwl_map *map, int x, int y,
                                  struct woodland_xwl_region *out)
{
    const struct woodland_xwl_region *nearest = NULL;
    long long nearest_distance = LLONG_MAX;
    for (int i = 0; i < map->count; i++) {
        long long d = distance_squared(&map->regions[i].x, x, y);
        if (d < nearest_distance) {
            nearest = &map->regions[i];
            nearest_distance = d;
        }
    }
    *out = nearest ? *nearest : map->fallback;
}

struct wlr_box woodland_xwl_map_x_box(const struct woodland_xwl_map *map,
                                      const struct wlr_output *output)
{
    for (int i = 0; i < map->count; i++) {
        if (map->outputs[i] == output) {
            return map->regions[i].x;
        }
    }
    return (struct wlr_box){0};
}

bool woodland_xwl_view_region(struct woodland_view *view, struct woodland_xwl_region *out)
{
    if (!view || !view->server || view->type != WOODLAND_VIEW_XWAYLAND ||
        !view->server->xwl_map.enabled || !view->scene_tree) {
        return false;
    }
    struct woodland_server *server = view->server;
    /* 绝不调 woodland_output_for_view / woodland_view_outer_box / woodland_view_content_box：
     * content_box 会反过来调本函数，形成无限递归。 */
    if (!view->placed) {
        struct woodland_output *wo = NULL;
        int lx = 0;
        int ly = 0;
        if (view->parent && view->parent->mapped && view->parent->scene_tree) {
            lx = view->parent->scene_tree->node.x;
            ly = view->parent->scene_tree->node.y;
            wo = woodland_output_at(server, lx, ly);
        } else if (server->cursor) {
            lx = (int)server->cursor->x;
            ly = (int)server->cursor->y;
            wo = woodland_output_at(server, server->cursor->x, server->cursor->y);
        }
        if (!wo) {
            wo = woodland_output_first(server);
        }
        woodland_xwl_map_region(&server->xwl_map, wo ? wo->wlr_output : NULL, lx, ly, out);
        return true;
    }

    int w = 0;
    int h = 0;
    if (view->scene_surface && view->scene_surface->buffer &&
        view->scene_surface->buffer->dst_width > 0 &&
        view->scene_surface->buffer->dst_height > 0) {
        w = view->scene_surface->buffer->dst_width;
        h = view->scene_surface->buffer->dst_height;
    }
#if WOODLAND_HAVE_XWAYLAND
    else if (view->xwayland_surface) {
        struct woodland_xwl_region origin_reg;
        woodland_xwl_map_region(&server->xwl_map, NULL, view->scene_tree->node.x,
                                view->scene_tree->node.y, &origin_reg);
        w = woodland_xwl_to_layout_width(&origin_reg, view->xwayland_surface->width);
        h = woodland_xwl_to_layout_height(&origin_reg, view->xwayland_surface->height);
    }
#endif

    int cx = view->scene_tree->node.x + w / 2;
    int cy = view->scene_tree->node.y + h / 2;
    struct woodland_output *wo = woodland_output_at(server, cx, cy);
    woodland_xwl_map_region(&server->xwl_map, wo ? wo->wlr_output : NULL, cx, cy, out);
    return true;
}

/* ==================== XSETTINGS 编码（参考 Umbriel xsettings.cpp） ==================== */

#define XSETTINGS_BASE_DPI (96.0 * 1024.0)
#define XSETTINGS_LSB_FIRST 0
#define XSETTINGS_TYPE_INTEGER 0
#define XSETTINGS_TYPE_STRING 1

struct xsettings_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool overflow;
};

static void xs_put_u8(struct xsettings_buf *b, uint8_t v)
{
    if (b->len + 1 > b->cap) {
        b->overflow = true;
        return;
    }
    b->data[b->len++] = v;
}

static void xs_put_u16(struct xsettings_buf *b, uint16_t v)
{
    xs_put_u8(b, (uint8_t)(v & 0xFFU));
    xs_put_u8(b, (uint8_t)(v >> 8U));
}

static void xs_put_u32(struct xsettings_buf *b, uint32_t v)
{
    xs_put_u8(b, (uint8_t)(v & 0xFFU));
    xs_put_u8(b, (uint8_t)((v >> 8U) & 0xFFU));
    xs_put_u8(b, (uint8_t)((v >> 16U) & 0xFFU));
    xs_put_u8(b, (uint8_t)((v >> 24U) & 0xFFU));
}

static void xs_put_padded(struct xsettings_buf *b, const char *s, size_t slen)
{
    size_t pad = (4 - (slen % 4)) % 4;
    if (b->len + slen + pad > b->cap) {
        b->overflow = true;
        return;
    }
    if (slen > 0) {
        memcpy(b->data + b->len, s, slen);
        b->len += slen;
    }
    for (size_t i = 0; i < pad; i++) {
        b->data[b->len++] = 0;
    }
}

static void xs_put_header(struct xsettings_buf *b, uint8_t type, const char *name, uint32_t serial)
{
    size_t nlen = strlen(name);
    xs_put_u8(b, type);
    xs_put_u8(b, 0);
    xs_put_u16(b, (uint16_t)nlen);
    xs_put_padded(b, name, nlen);
    xs_put_u32(b, serial);
}

static void xs_put_integer(struct xsettings_buf *b, const char *name, int32_t value, uint32_t serial)
{
    xs_put_header(b, XSETTINGS_TYPE_INTEGER, name, serial);
    xs_put_u32(b, (uint32_t)value);
}

static void xs_put_string(struct xsettings_buf *b, const char *name, const char *value,
                          uint32_t serial)
{
    size_t vlen = strlen(value);
    xs_put_header(b, XSETTINGS_TYPE_STRING, name, serial);
    xs_put_u32(b, (uint32_t)vlen);
    xs_put_padded(b, value, vlen);
}

void woodland_xsettings_for(double scale, const char *cursor_theme, int cursor_size,
                            struct woodland_xsettings_values *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    double effective = scale > 1.0 ? scale : 1.0;
    int32_t integer = (int32_t)floor(effective);
    if (integer < 1) {
        integer = 1;
    }
    int32_t xft_dpi = (int32_t)lround(XSETTINGS_BASE_DPI * effective);
    int base_cursor = cursor_size > 0 ? cursor_size : 24;

    out->window_scaling_factor = integer;
    out->unscaled_dpi = (int32_t)lround((double)xft_dpi / (double)integer);
    out->xft_dpi = xft_dpi;
    out->cursor_size = (int32_t)lround((double)base_cursor * effective);
    if (cursor_theme && cursor_theme[0]) {
        strncpy(out->cursor_theme, cursor_theme, sizeof(out->cursor_theme) - 1);
        out->cursor_theme[sizeof(out->cursor_theme) - 1] = '\0';
    }
}

size_t woodland_xsettings_encode(uint32_t serial,
                                 const struct woodland_xsettings_values *values,
                                 uint8_t *buf, size_t cap)
{
    if (!values || !buf || cap < 12) {
        return 0;
    }
    struct xsettings_buf b = {
        .data = buf,
        .len = 0,
        .cap = cap,
        .overflow = false,
    };
    bool has_theme = values->cursor_theme[0] != '\0';
    uint32_t count = has_theme ? 5U : 4U;

    xs_put_u8(&b, XSETTINGS_LSB_FIRST);
    xs_put_u8(&b, 0);
    xs_put_u8(&b, 0);
    xs_put_u8(&b, 0);
    xs_put_u32(&b, serial);
    xs_put_u32(&b, count);
    xs_put_integer(&b, "Gdk/WindowScalingFactor", values->window_scaling_factor, serial);
    xs_put_integer(&b, "Gdk/UnscaledDPI", values->unscaled_dpi, serial);
    xs_put_integer(&b, "Xft/DPI", values->xft_dpi, serial);
    xs_put_integer(&b, "Gtk/CursorThemeSize", values->cursor_size, serial);
    if (has_theme) {
        xs_put_string(&b, "Gtk/CursorThemeName", values->cursor_theme, serial);
    }
    return b.overflow ? 0 : b.len;
}
