// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <assert.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "scene.h"
#include "output.h"
#include "placement.h"
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

#define ITEM_NORMAL_HEIGHT WOODLAND_MENU_ITEM_HEIGHT
#define ITEM_SEPARATOR_HEIGHT WOODLAND_MENU_SEPARATOR_HEIGHT
#define MENU_PADDING_Y WOODLAND_MENU_PADDING_Y
#define MENU_PADDING_X WOODLAND_MENU_PADDING_X
#define MAX_MENU_ITEMS WOODLAND_MENU_MAX_ITEMS

/* ==================== cairo 包装为 wlr_buffer ==================== */

struct menu_cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void menu_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct menu_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (b->surface) {
        cairo_surface_destroy(b->surface);
    }
    wlr_buffer_finish(&b->base);
    free(b);
}

static bool menu_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                                             void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct menu_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (!b || !b->surface || cairo_surface_status(b->surface) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    cairo_surface_flush(b->surface);
    *data = cairo_image_surface_get_data(b->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(b->surface);
    return true;
}

static void menu_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    struct menu_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_mark_dirty(b->surface);
}

static const struct wlr_buffer_impl menu_buffer_impl = {
    .destroy = menu_buffer_destroy,
    .begin_data_ptr_access = menu_buffer_begin_data_ptr_access,
    .end_data_ptr_access = menu_buffer_end_data_ptr_access,
};

static struct wlr_buffer *menu_wrap_cairo_surface(cairo_surface_t *surf, int width, int height)
{
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (surf) {
            cairo_surface_destroy(surf);
        }
        return NULL;
    }
    struct menu_cairo_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    int px_w = cairo_image_surface_get_width(surf);
    int px_h = cairo_image_surface_get_height(surf);
    wlr_buffer_init(&b->base, &menu_buffer_impl, px_w > 0 ? px_w : width, px_h > 0 ? px_h : height);
    return &b->base;
}

/* ==================== 几何与绘制辅助 ==================== */

static bool is_zh_locale(void)
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

static bool view_is_resizable(const struct woodland_view *view)
{
    if (!view) {
        return false;
    }
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    if (max_w > 0 && max_h > 0 && min_w == max_w && min_h == max_h) {
        return false;
    }
    return true;
}

static void cairo_rounded_rectangle(cairo_t *cr, double x, double y, double w, double h, double r)
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

static int window_menu_item_height(const struct window_menu_item *it)
{
    return it->is_separator ? ITEM_SEPARATOR_HEIGHT : ITEM_NORMAL_HEIGHT;
}

static int window_menu_item_y(const struct window_menu_level *level, int index)
{
    int y = MENU_PADDING_Y;
    for (int i = 0; i < index && i < level->item_count; i++) {
        y += window_menu_item_height(&level->items[i]);
    }
    return y;
}

static int window_menu_level_item_at(const struct window_menu_level *level, int local_y)
{
    int y = MENU_PADDING_Y;
    for (int i = 0; i < level->item_count; i++) {
        int h = window_menu_item_height(&level->items[i]);
        if (local_y >= y && local_y < y + h) {
            if (level->items[i].is_separator || !level->items[i].enabled) {
                return -1;
            }
            return i;
        }
        y += h;
    }
    return -1;
}

static void window_menu_measure_level(struct woodland_window_menu *menu,
                                     struct window_menu_level *level)
{
    const char *font_desc = (menu->server->config && menu->server->config->ssd.font)
                                ? menu->server->config->ssd.font
                                : "Sans 10";
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(font_desc);
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }

    int max_text_w = 0;
    int max_sc_w = 0;
    int total_h = MENU_PADDING_Y * 2;

    for (int i = 0; i < level->item_count; i++) {
        if (level->items[i].is_separator) {
            total_h += ITEM_SEPARATOR_HEIGHT;
            continue;
        }
        total_h += ITEM_NORMAL_HEIGHT;

        pango_layout_set_text(layout, level->items[i].text, -1);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout, &tw, &th);
        if (tw > max_text_w) {
            max_text_w = tw;
        }

        if (level->items[i].shortcut[0]) {
            pango_layout_set_text(layout, level->items[i].shortcut, -1);
            int sw = 0, sh = 0;
            pango_layout_get_pixel_size(layout, &sw, &sh);
            if (sw > max_sc_w) {
                max_sc_w = sw;
            }
        }
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    int w = 28 + max_text_w + 20 + max_sc_w + 24;
    if (w < 190) {
        w = 190;
    }
    level->width = w;
    level->height = total_h;
}

static void window_menu_render_level(struct woodland_window_menu *menu,
                                     struct window_menu_level *level)
{
    if (!menu || !level || level->item_count == 0) {
        return;
    }
    float scale = menu->scale > 0.0f ? menu->scale : 1.0f;
    int px_w = (int)ceil(level->width * scale);
    int px_h = (int)ceil(level->height * scale);
    if (px_w <= 0 || px_h <= 0) {
        return;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *cr = cairo_create(surf);

    bool dark = false;
    if (menu->server->config) {
        if (menu->server->config->appearance.color_scheme_set) {
            dark = menu->server->config->appearance.dark;
        }
    }

    float bg_rgba[4] = {0.98f, 0.98f, 0.98f, 0.98f};
    float border_rgba[4] = {0.80f, 0.80f, 0.80f, 0.90f};
    float text_rgba[4] = {0.12f, 0.12f, 0.12f, 1.0f};
    float dimmed_rgba[4] = {0.55f, 0.55f, 0.55f, 1.0f};
    float sep_rgba[4] = {0.88f, 0.88f, 0.88f, 0.90f};
    float hover_bg_rgba[4] = {0.18f, 0.53f, 0.93f, 0.92f};
    float hover_text_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float hover_dimmed_rgba[4] = {0.88f, 0.88f, 0.88f, 1.0f};

    if (dark) {
        bg_rgba[0] = 0.16f; bg_rgba[1] = 0.16f; bg_rgba[2] = 0.16f; bg_rgba[3] = 0.98f;
        border_rgba[0] = 0.32f; border_rgba[1] = 0.32f; border_rgba[2] = 0.32f; border_rgba[3] = 0.90f;
        text_rgba[0] = 0.92f; text_rgba[1] = 0.92f; text_rgba[2] = 0.92f;
        dimmed_rgba[0] = 0.50f; dimmed_rgba[1] = 0.50f; dimmed_rgba[2] = 0.50f;
        sep_rgba[0] = 0.28f; sep_rgba[1] = 0.28f; sep_rgba[2] = 0.28f;
    }

    if (menu->server->config && menu->server->config->appearance.accent_set) {
        uint32_t rgb = menu->server->config->appearance.accent_rgb;
        hover_bg_rgba[0] = ((rgb >> 16) & 0xff) / 255.0f;
        hover_bg_rgba[1] = ((rgb >> 8) & 0xff) / 255.0f;
        hover_bg_rgba[2] = (rgb & 0xff) / 255.0f;
        hover_bg_rgba[3] = 0.92f;
    }

    double corner_r = 8.0;
    if (menu->server->config && menu->server->config->effects.enabled &&
        menu->server->config->effects.corner_radius > 0) {
        corner_r = (double)menu->server->config->effects.corner_radius;
        if (corner_r > 12.0) {
            corner_r = 12.0;
        }
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_rounded_rectangle(cr, 0.5, 0.5, level->width - 1.0, level->height - 1.0, corner_r);
    cairo_set_source_rgba(cr, bg_rgba[0], bg_rgba[1], bg_rgba[2], bg_rgba[3]);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, border_rgba[0], border_rgba[1], border_rgba[2], border_rgba[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const char *font_desc = (menu->server->config && menu->server->config->ssd.font)
                                ? menu->server->config->ssd.font
                                : "Sans 10";
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(font_desc);
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }

    int cur_y = MENU_PADDING_Y;
    for (int i = 0; i < level->item_count; i++) {
        struct window_menu_item *it = &level->items[i];
        int h = window_menu_item_height(it);

        if (it->is_separator) {
            cairo_set_source_rgba(cr, sep_rgba[0], sep_rgba[1], sep_rgba[2], sep_rgba[3]);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 8, cur_y + 4.5);
            cairo_line_to(cr, level->width - 8, cur_y + 4.5);
            cairo_stroke(cr);
            cur_y += h;
            continue;
        }

        bool hovered = (level->hovered_index == i && it->enabled);

        if (hovered) {
            cairo_rounded_rectangle(cr, MENU_PADDING_X, cur_y + 1,
                                    level->width - 2 * MENU_PADDING_X, h - 2, 5.0);
            cairo_set_source_rgba(cr, hover_bg_rgba[0], hover_bg_rgba[1], hover_bg_rgba[2],
                                  hover_bg_rgba[3]);
            cairo_fill(cr);
        }

        float *cur_text_color = hovered ? hover_text_rgba : (it->enabled ? text_rgba : dimmed_rgba);
        float *cur_sc_color = hovered ? hover_dimmed_rgba : dimmed_rgba;

        if (it->checked) {
            cairo_set_source_rgba(cr, cur_text_color[0], cur_text_color[1], cur_text_color[2],
                                  cur_text_color[3]);
            pango_layout_set_text(layout, "✓", -1);
            cairo_move_to(cr, 10, cur_y + 6);
            pango_cairo_show_layout(cr, layout);
        }

        cairo_set_source_rgba(cr, cur_text_color[0], cur_text_color[1], cur_text_color[2],
                              cur_text_color[3]);
        pango_layout_set_text(layout, it->text, -1);
        cairo_move_to(cr, 28, cur_y + 6);
        pango_cairo_show_layout(cr, layout);

        if (it->shortcut[0]) {
            cairo_set_source_rgba(cr, cur_sc_color[0], cur_sc_color[1], cur_sc_color[2],
                                  cur_sc_color[3]);
            pango_layout_set_text(layout, it->shortcut, -1);
            int sc_w = 0, sc_h = 0;
            pango_layout_get_pixel_size(layout, &sc_w, &sc_h);
            int sc_x = level->width - (it->has_submenu ? 30 : 16) - sc_w;
            cairo_move_to(cr, sc_x, cur_y + 6);
            pango_cairo_show_layout(cr, layout);
        }

        if (it->has_submenu) {
            cairo_set_source_rgba(cr, cur_text_color[0], cur_text_color[1], cur_text_color[2],
                                  cur_text_color[3]);
            pango_layout_set_text(layout, "▸", -1);
            cairo_move_to(cr, level->width - 16, cur_y + 6);
            pango_cairo_show_layout(cr, layout);
        }

        cur_y += h;
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    struct wlr_buffer *wlr_buf = menu_wrap_cairo_surface(surf, level->width, level->height);
    if (!wlr_buf) {
        return;
    }

    if (!level->scene_buffer) {
        level->scene_buffer = wlr_scene_buffer_create(menu->tree, wlr_buf);
    } else {
        wlr_scene_buffer_set_buffer(level->scene_buffer, wlr_buf);
    }
    wlr_buffer_drop(wlr_buf);

    if (level->scene_buffer) {
        wlr_scene_buffer_set_dest_size(level->scene_buffer, level->width, level->height);
        wlr_scene_node_set_position(&level->scene_buffer->node, level->x, level->y);
        wlr_scene_node_set_enabled(&level->scene_buffer->node, true);
    }

#if WOODLAND_HAVE_EFFECTS
    if (menu->server->config && menu->server->config->effects.enabled &&
        menu->server->config->effects.shadow) {
        float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.30f};
        if (!level->shadow) {
            level->shadow = wlr_scene_shadow_create(menu->tree, level->width, level->height,
                                                    (int)corner_r, 12.0f, shadow_color);
        } else {
            wlr_scene_shadow_set_size(level->shadow, level->width, level->height);
            wlr_scene_shadow_set_corner_radius(level->shadow, (int)corner_r);
            wlr_scene_shadow_set_blur_sigma(level->shadow, 12.0f);
            wlr_scene_shadow_set_color(level->shadow, shadow_color);
        }
        if (level->shadow) {
            wlr_scene_node_set_position(&level->shadow->node, level->x, level->y + 2);
            wlr_scene_node_set_enabled(&level->shadow->node, true);
            if (level->scene_buffer) {
                wlr_scene_node_place_below(&level->shadow->node, &level->scene_buffer->node);
            }
        }
    }
#endif
}

/* ==================== 菜单内容构建 ==================== */

static void populate_workspace_submenu(struct woodland_window_menu *menu)
{
    bool zh = is_zh_locale();
    struct woodland_workspace *ws;
    wl_list_for_each(ws, &menu->server->workspaces, link) {
        if (menu->sub.item_count >= MAX_MENU_ITEMS) {
            break;
        }
        struct window_menu_item *it = &menu->sub.items[menu->sub.item_count++];
        memset(it, 0, sizeof(*it));
        if (zh) {
            snprintf(it->text, sizeof(it->text), "工作区 %s", ws->name ? ws->name : "");
        } else {
            snprintf(it->text, sizeof(it->text), "Workspace %s", ws->name ? ws->name : "");
        }
        it->enabled = true;
        it->checked = (menu->view && menu->view->workspace == ws);
        it->action = WINDOW_MENU_ACTION_WORKSPACE;
        it->action_data = ws;
    }
}

static void populate_output_submenu(struct woodland_window_menu *menu)
{
    bool zh = is_zh_locale();
    struct woodland_output *cur_wo = woodland_output_for_view(menu->view);
    struct woodland_output *wo;
    int idx = 1;
    wl_list_for_each(wo, &menu->server->outputs, link) {
        if (!wo->wlr_output || !wo->wlr_output->enabled) {
            continue;
        }
        if (menu->sub.item_count >= MAX_MENU_ITEMS) {
            break;
        }
        struct window_menu_item *it = &menu->sub.items[menu->sub.item_count++];
        memset(it, 0, sizeof(*it));
        const char *name = wo->wlr_output->name ? wo->wlr_output->name : "";
        if (zh) {
            snprintf(it->text, sizeof(it->text), "屏幕 %d (%s)", idx, name);
        } else {
            snprintf(it->text, sizeof(it->text), "Screen %d (%s)", idx, name);
        }
        it->enabled = true;
        it->checked = (cur_wo == wo);
        it->action = WINDOW_MENU_ACTION_OUTPUT;
        it->action_data = wo;
        idx++;
    }
}

static void populate_root_menu(struct woodland_window_menu *menu)
{
    menu->root.item_count = 0;
    menu->root.hovered_index = -1;
    bool zh = is_zh_locale();
    struct woodland_view *view = menu->view;
    bool resizable = view_is_resizable(view);
    bool maximizable = resizable;
    bool is_max = view && view->maximized;
    bool is_full = view && view->fullscreen;

    /* 1. 最大化 / 还原 */
    struct window_menu_item *it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    if (is_max) {
        snprintf(it->text, sizeof(it->text), "%s", zh ? "还原(_R)" : "Restore(_R)");
        it->accel_key = 'r';
    } else {
        snprintf(it->text, sizeof(it->text), "%s", zh ? "最大化(_X)" : "Maximize(_X)");
        it->accel_key = 'x';
    }
    snprintf(it->shortcut, sizeof(it->shortcut), "Alt+F10");
    it->enabled = maximizable && !is_full;
    it->action = WINDOW_MENU_ACTION_MAXIMIZE;

    /* 2. 最小化 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->text, sizeof(it->text), "%s", zh ? "最小化(_N)" : "Minimize(_N)");
    snprintf(it->shortcut, sizeof(it->shortcut), "Alt+F9");
    it->accel_key = 'n';
    it->enabled = true;
    it->action = WINDOW_MENU_ACTION_MINIMIZE;

    /* 3. 移动 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->text, sizeof(it->text), "%s", zh ? "移动(_M)" : "Move(_M)");
    it->accel_key = 'm';
    it->enabled = !is_max && !is_full;
    it->action = WINDOW_MENU_ACTION_MOVE;

    /* 4. 调整大小 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->text, sizeof(it->text), "%s", zh ? "调整大小(_R)" : "Resize(_R)");
    it->accel_key = 'r';
    it->enabled = resizable && !is_max && !is_full;
    it->action = WINDOW_MENU_ACTION_RESIZE;

    /* 5. 全屏 / 退出全屏 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    if (is_full) {
        snprintf(it->text, sizeof(it->text), "%s", zh ? "退出全屏(_F)" : "Exit Fullscreen(_F)");
    } else {
        snprintf(it->text, sizeof(it->text), "%s", zh ? "全屏(_F)" : "Fullscreen(_F)");
    }
    snprintf(it->shortcut, sizeof(it->shortcut), "F11");
    it->accel_key = 'f';
    it->enabled = true;
    it->action = WINDOW_MENU_ACTION_FULLSCREEN;

    /* 分隔线 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    it->is_separator = true;

    /* 6. 工作区子菜单 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->text, sizeof(it->text), "%s", zh ? "工作区(_W)" : "Workspace(_W)");
    it->accel_key = 'w';
    it->enabled = !wl_list_empty(&menu->server->workspaces);
    it->has_submenu = true;
    it->action = WINDOW_MENU_ACTION_WORKSPACE;

    /* 7. 移动到屏幕子菜单（多屏幕时启用） */
    int out_count = 0;
    struct woodland_output *wo;
    wl_list_for_each(wo, &menu->server->outputs, link) {
        if (wo->wlr_output && wo->wlr_output->enabled) {
            out_count++;
        }
    }
    if (out_count > 1) {
        it = &menu->root.items[menu->root.item_count++];
        memset(it, 0, sizeof(*it));
        snprintf(it->text, sizeof(it->text), "%s", zh ? "移动到屏幕(_S)" : "Move to Screen(_S)");
        it->accel_key = 's';
        it->enabled = true;
        it->has_submenu = true;
        it->action = WINDOW_MENU_ACTION_OUTPUT;
    }

    /* 分隔线 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    it->is_separator = true;

    /* 8. 关闭 */
    it = &menu->root.items[menu->root.item_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->text, sizeof(it->text), "%s", zh ? "关闭(_C)" : "Close(_C)");
    snprintf(it->shortcut, sizeof(it->shortcut), "Alt+F4");
    it->accel_key = 'c';
    it->enabled = true;
    it->action = WINDOW_MENU_ACTION_CLOSE;
}

static void window_menu_close_submenu(struct woodland_window_menu *menu)
{
    if (menu->active_sub_item < 0) {
        return;
    }
    menu->active_sub_item = -1;
    menu->sub_has_focus = false;
    menu->sub.hovered_index = -1;
    if (menu->sub.scene_buffer) {
        wlr_scene_node_set_enabled(&menu->sub.scene_buffer->node, false);
    }
#if WOODLAND_HAVE_EFFECTS
    if (menu->sub.shadow) {
        wlr_scene_node_set_enabled(&menu->sub.shadow->node, false);
    }
#endif
}

static void window_menu_open_submenu(struct woodland_window_menu *menu, int root_item_idx)
{
    if (root_item_idx < 0 || root_item_idx >= menu->root.item_count) {
        return;
    }
    if (!menu->root.items[root_item_idx].has_submenu) {
        return;
    }
    if (menu->active_sub_item == root_item_idx) {
        return;
    }
    window_menu_close_submenu(menu);
    menu->active_sub_item = root_item_idx;
    menu->sub.item_count = 0;
    menu->sub.hovered_index = -1;

    enum window_menu_action action = menu->root.items[root_item_idx].action;
    if (action == WINDOW_MENU_ACTION_WORKSPACE) {
        populate_workspace_submenu(menu);
    } else if (action == WINDOW_MENU_ACTION_OUTPUT) {
        populate_output_submenu(menu);
    }

    if (menu->sub.item_count == 0) {
        menu->active_sub_item = -1;
        return;
    }

    window_menu_measure_level(menu, &menu->sub);

    int root_item_y = window_menu_item_y(&menu->root, root_item_idx);
    int sub_x = menu->root.x + menu->root.width - 2;
    int sub_y = menu->root.y + root_item_y - 4;

    struct wlr_box area = {0};
    if (menu->output) {
        woodland_output_workarea(menu->output, &area);
    }
    if (area.width <= 0 || area.height <= 0) {
        area = (struct wlr_box){0, 0, 1920, 1080};
    }

    if (sub_x + menu->sub.width > area.x + area.width) {
        sub_x = menu->root.x - menu->sub.width + 2;
    }
    if (sub_x < area.x) {
        sub_x = area.x;
    }
    if (sub_y + menu->sub.height > area.y + area.height) {
        sub_y = area.y + area.height - menu->sub.height;
    }
    if (sub_y < area.y) {
        sub_y = area.y;
    }
    menu->sub.x = sub_x;
    menu->sub.y = sub_y;

    window_menu_render_level(menu, &menu->sub);
}

static void window_menu_execute_action(struct woodland_server *server,
                                       struct woodland_view *view,
                                       const struct window_menu_item *it)
{
    if (!server || !view || !it) {
        return;
    }

    switch (it->action) {
    case WINDOW_MENU_ACTION_MAXIMIZE:
        woodland_trace_add("menu", view->id, "action=maximize toggle=%d", !view->maximized);
        woodland_view_set_maximized(view, !view->maximized);
        break;
    case WINDOW_MENU_ACTION_MINIMIZE:
        woodland_trace_add("menu", view->id, "action=minimize");
        woodland_view_set_minimized(view, true);
        break;
    case WINDOW_MENU_ACTION_MOVE: {
        woodland_trace_add("menu", view->id, "action=move");
        struct wlr_box content;
        woodland_view_content_box(view, &content);
        struct woodland_ssd_margins margins;
        woodland_ssd_get_margins(view, &margins);
        int node_x = view->scene_tree ? view->scene_tree->node.x : 0;
        int node_y = view->scene_tree ? view->scene_tree->node.y : 0;
        double target_x = node_x + content.x + content.width / 2.0;
        double target_y = node_y + content.y - margins.top / 2.0;
        if (server->cursor) {
            wlr_cursor_warp(server->cursor, NULL, target_x, target_y);
        }
        server->interactive_click_to_finish = true;
        woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
        break;
    }
    case WINDOW_MENU_ACTION_RESIZE: {
        woodland_trace_add("menu", view->id, "action=resize");
        struct wlr_box content;
        woodland_view_content_box(view, &content);
        struct woodland_ssd_margins margins;
        woodland_ssd_get_margins(view, &margins);
        int node_x = view->scene_tree ? view->scene_tree->node.x : 0;
        int node_y = view->scene_tree ? view->scene_tree->node.y : 0;
        double target_x = node_x + content.x + content.width + margins.right;
        double target_y = node_y + content.y + content.height + margins.bottom;
        if (server->cursor) {
            wlr_cursor_warp(server->cursor, NULL, target_x, target_y);
        }
        server->interactive_click_to_finish = true;
        woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE,
                                        WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
        break;
    }
    case WINDOW_MENU_ACTION_FULLSCREEN:
        woodland_trace_add("menu", view->id, "action=fullscreen toggle=%d", !view->fullscreen);
        woodland_view_set_fullscreen(view, !view->fullscreen, NULL);
        break;
    case WINDOW_MENU_ACTION_WORKSPACE: {
        struct woodland_workspace *target = it->action_data;
        if (target && view->workspace != target) {
            woodland_trace_add("menu", view->id, "action=workspace target=%s",
                               target->name ? target->name : "");
            view->workspace = target;
            wlr_scene_node_reparent(&view->scene_tree->node, target->tree);
            woodland_workspace_switch_focus(server, target, false);
            woodland_focus_view(view, true);
        }
        break;
    }
    case WINDOW_MENU_ACTION_OUTPUT: {
        struct woodland_output *target_out = it->action_data;
        if (target_out) {
            woodland_trace_add("menu", view->id, "action=output target=%s",
                               target_out->wlr_output ? target_out->wlr_output->name : "");
            struct wlr_box workarea = {0};
            woodland_output_workarea(target_out, &workarea);
            if (view->maximized) {
                view->saved_geometry.x = workarea.x + 50;
                view->saved_geometry.y = workarea.y + 50;
                struct wlr_box max_box =
                    woodland_view_maximized_geometry_on(server, target_out);
                woodland_view_apply_outer_geometry(view, &max_box);
            } else {
                struct woodland_ssd_margins margins;
                woodland_ssd_get_margins(view, &margins);
                struct wlr_box cur;
                woodland_view_content_box(view, &cur);
                int outer_w = cur.width + margins.left + margins.right;
                int outer_h = cur.height + margins.top + margins.bottom;
                struct wlr_box new_box = {
                    .x = workarea.x + (workarea.width - outer_w) / 2,
                    .y = workarea.y + (workarea.height - outer_h) / 2,
                    .width = outer_w,
                    .height = outer_h,
                };
                woodland_view_apply_outer_geometry(view, &new_box);
            }
            woodland_view_update_outputs(view);
            woodland_focus_view(view, true);
        }
        break;
    }
    case WINDOW_MENU_ACTION_CLOSE:
        woodland_trace_add("menu", view->id, "action=close");
        woodland_view_close(view);
        break;
    default:
        break;
    }
}

/* ==================== 公开 API ==================== */

bool woodland_window_menu_init(struct woodland_server *server)
{
    if (!server) {
        return false;
    }
    struct woodland_window_menu *menu = calloc(1, sizeof(*menu));
    if (!menu) {
        return false;
    }
    menu->server = server;
    menu->active_sub_item = -1;
    menu->root.hovered_index = -1;
    menu->sub.hovered_index = -1;
    if (server->scene) {
        menu->tree = wlr_scene_tree_create(&server->scene->tree);
        if (!menu->tree) {
            free(menu);
            return false;
        }
        wlr_scene_node_set_enabled(&menu->tree->node, false);
    }
    server->window_menu = menu;
    return true;
}

void woodland_window_menu_finish(struct woodland_server *server)
{
    if (!server || !server->window_menu) {
        return;
    }
    struct woodland_window_menu *menu = server->window_menu;
    woodland_window_menu_hide(server);
    if (menu->tree) {
        wlr_scene_node_destroy(&menu->tree->node);
        menu->tree = NULL;
    }
    free(menu);
    server->window_menu = NULL;
}

bool woodland_window_menu_is_active(const struct woodland_server *server)
{
    return server && server->window_menu && server->window_menu->active;
}

void woodland_window_menu_show(struct woodland_view *view, double lx, double ly)
{
    if (!view || !view->server) {
        return;
    }
    struct woodland_server *server = view->server;
    if (!server->window_menu) {
        if (!woodland_window_menu_init(server)) {
            return;
        }
    }
    struct woodland_window_menu *menu = server->window_menu;
    if (!menu->tree && server->scene) {
        menu->tree = wlr_scene_tree_create(&server->scene->tree);
        if (!menu->tree) {
            return;
        }
        wlr_scene_node_set_enabled(&menu->tree->node, false);
    }

    if (menu->active) {
        woodland_window_menu_hide(server);
    }

    menu->view = view;
    menu->active = true;
    menu->active_sub_item = -1;
    menu->sub_has_focus = false;

    menu->output = woodland_output_at(server, lx, ly);
    if (!menu->output) {
        menu->output = woodland_output_for_view(view);
    }
    if (!menu->output) {
        menu->output = woodland_output_first(server);
    }
    menu->scale = (menu->output && menu->output->wlr_output &&
                   menu->output->wlr_output->scale > 0.0f)
                      ? menu->output->wlr_output->scale
                      : 1.0f;

    populate_root_menu(menu);
    window_menu_measure_level(menu, &menu->root);

    struct wlr_box workarea = {0};
    if (menu->output) {
        woodland_output_workarea(menu->output, &workarea);
    }
    if (workarea.width <= 0 || workarea.height <= 0) {
        workarea = (struct wlr_box){0, 0, 1920, 1080};
    }

    int x = (int)lx;
    int y = (int)ly;
    if (x + menu->root.width > workarea.x + workarea.width) {
        x = workarea.x + workarea.width - menu->root.width;
    }
    if (x < workarea.x) {
        x = workarea.x;
    }
    if (y + menu->root.height > workarea.y + workarea.height) {
        y = workarea.y + workarea.height - menu->root.height;
    }
    if (y < workarea.y) {
        y = workarea.y;
    }

    menu->root.x = x;
    menu->root.y = y;

    window_menu_render_level(menu, &menu->root);

    if (menu->tree) {
        wlr_scene_node_set_enabled(&menu->tree->node, true);
        wlr_scene_node_raise_to_top(&menu->tree->node);
    }

    woodland_trace_add("menu", view->id, "show at %d,%d", x, y);
}

void woodland_window_menu_hide(struct woodland_server *server)
{
    if (!server || !server->window_menu || !server->window_menu->active) {
        return;
    }
    struct woodland_window_menu *menu = server->window_menu;
    menu->active = false;
    window_menu_close_submenu(menu);
    if (menu->root.scene_buffer) {
        wlr_scene_node_set_enabled(&menu->root.scene_buffer->node, false);
    }
#if WOODLAND_HAVE_EFFECTS
    if (menu->root.shadow) {
        wlr_scene_node_set_enabled(&menu->root.shadow->node, false);
    }
#endif
    if (menu->tree) {
        wlr_scene_node_set_enabled(&menu->tree->node, false);
    }
    uint32_t view_id = menu->view ? menu->view->id : 0;
    menu->view = NULL;
    woodland_trace_add("menu", view_id, "hide");
}

void woodland_window_menu_hide_for_view(struct woodland_view *view)
{
    if (!view || !view->server || !view->server->window_menu) {
        return;
    }
    if (view->server->window_menu->active && view->server->window_menu->view == view) {
        woodland_window_menu_hide(view->server);
    }
}

bool woodland_window_menu_handle_button(struct woodland_server *server, uint32_t button,
                                       uint32_t state, double lx, double ly)
{
    if (!server || !server->window_menu || !server->window_menu->active) {
        return false;
    }
    struct woodland_window_menu *menu = server->window_menu;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return true;
    }

    if (button != BTN_LEFT && button != BTN_RIGHT) {
        return true;
    }

    if (menu->active_sub_item >= 0 && menu->sub.scene_buffer &&
        menu->sub.scene_buffer->node.enabled) {
        if (lx >= menu->sub.x && lx < menu->sub.x + menu->sub.width &&
            ly >= menu->sub.y && ly < menu->sub.y + menu->sub.height) {
            int idx = window_menu_level_item_at(&menu->sub, (int)(ly - menu->sub.y));
            if (idx >= 0 && menu->sub.items[idx].enabled) {
                struct window_menu_item it = menu->sub.items[idx];
                struct woodland_view *view = menu->view;
                woodland_window_menu_hide(server);
                window_menu_execute_action(server, view, &it);
            }
            return true;
        }
    }

    if (lx >= menu->root.x && lx < menu->root.x + menu->root.width &&
        ly >= menu->root.y && ly < menu->root.y + menu->root.height) {
        int idx = window_menu_level_item_at(&menu->root, (int)(ly - menu->root.y));
        if (idx >= 0 && menu->root.items[idx].enabled) {
            if (menu->root.items[idx].has_submenu) {
                window_menu_open_submenu(menu, idx);
                menu->sub_has_focus = true;
            } else {
                struct window_menu_item it = menu->root.items[idx];
                struct woodland_view *view = menu->view;
                woodland_window_menu_hide(server);
                window_menu_execute_action(server, view, &it);
            }
        }
        return true;
    }

    woodland_window_menu_hide(server);
    return true;
}

void woodland_window_menu_handle_motion(struct woodland_server *server, double lx, double ly)
{
    if (!server || !server->window_menu || !server->window_menu->active) {
        return;
    }
    struct woodland_window_menu *menu = server->window_menu;

    if (menu->active_sub_item >= 0 && menu->sub.scene_buffer &&
        menu->sub.scene_buffer->node.enabled) {
        if (lx >= menu->sub.x && lx < menu->sub.x + menu->sub.width &&
            ly >= menu->sub.y && ly < menu->sub.y + menu->sub.height) {
            menu->sub_has_focus = true;
            int idx = window_menu_level_item_at(&menu->sub, (int)(ly - menu->sub.y));
            if (idx != menu->sub.hovered_index) {
                menu->sub.hovered_index = idx;
                window_menu_render_level(menu, &menu->sub);
            }
            return;
        }
    }

    if (lx >= menu->root.x && lx < menu->root.x + menu->root.width &&
        ly >= menu->root.y && ly < menu->root.y + menu->root.height) {
        menu->sub_has_focus = false;
        int idx = window_menu_level_item_at(&menu->root, (int)(ly - menu->root.y));
        if (idx != menu->root.hovered_index) {
            menu->root.hovered_index = idx;
            if (idx >= 0 && menu->root.items[idx].has_submenu) {
                window_menu_open_submenu(menu, idx);
            } else if (menu->active_sub_item >= 0) {
                window_menu_close_submenu(menu);
            }
            window_menu_render_level(menu, &menu->root);
        }
        return;
    }

    if (menu->sub_has_focus) {
        if (menu->sub.hovered_index != -1) {
            menu->sub.hovered_index = -1;
            window_menu_render_level(menu, &menu->sub);
        }
    } else {
        if (menu->root.hovered_index != -1 && menu->active_sub_item < 0) {
            menu->root.hovered_index = -1;
            window_menu_render_level(menu, &menu->root);
        }
    }
}

bool woodland_window_menu_handle_key(struct woodland_server *server, xkb_keysym_t sym)
{
    if (!server || !server->window_menu || !server->window_menu->active) {
        return false;
    }
    struct woodland_window_menu *menu = server->window_menu;

    if (sym == XKB_KEY_Escape) {
        woodland_window_menu_hide(server);
        return true;
    }

    if (sym == XKB_KEY_Down) {
        struct window_menu_level *lvl = menu->sub_has_focus ? &menu->sub : &menu->root;
        int next = lvl->hovered_index + 1;
        for (int step = 0; step < lvl->item_count; step++) {
            if (next >= lvl->item_count) {
                next = 0;
            }
            if (!lvl->items[next].is_separator && lvl->items[next].enabled) {
                lvl->hovered_index = next;
                break;
            }
            next++;
        }
        if (!menu->sub_has_focus && menu->active_sub_item >= 0 &&
            menu->active_sub_item != lvl->hovered_index) {
            window_menu_close_submenu(menu);
        }
        window_menu_render_level(menu, lvl);
        return true;
    }

    if (sym == XKB_KEY_Up) {
        struct window_menu_level *lvl = menu->sub_has_focus ? &menu->sub : &menu->root;
        int prev = lvl->hovered_index - 1;
        for (int step = 0; step < lvl->item_count; step++) {
            if (prev < 0) {
                prev = lvl->item_count - 1;
            }
            if (!lvl->items[prev].is_separator && lvl->items[prev].enabled) {
                lvl->hovered_index = prev;
                break;
            }
            prev--;
        }
        if (!menu->sub_has_focus && menu->active_sub_item >= 0 &&
            menu->active_sub_item != lvl->hovered_index) {
            window_menu_close_submenu(menu);
        }
        window_menu_render_level(menu, lvl);
        return true;
    }

    if (sym == XKB_KEY_Right) {
        if (!menu->sub_has_focus && menu->root.hovered_index >= 0 &&
            menu->root.items[menu->root.hovered_index].has_submenu) {
            window_menu_open_submenu(menu, menu->root.hovered_index);
            menu->sub_has_focus = true;
            for (int i = 0; i < menu->sub.item_count; i++) {
                if (!menu->sub.items[i].is_separator && menu->sub.items[i].enabled) {
                    menu->sub.hovered_index = i;
                    break;
                }
            }
            window_menu_render_level(menu, &menu->sub);
            return true;
        }
    }

    if (sym == XKB_KEY_Left) {
        if (menu->sub_has_focus) {
            menu->sub_has_focus = false;
            window_menu_close_submenu(menu);
            window_menu_render_level(menu, &menu->root);
            return true;
        }
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
        if (menu->sub_has_focus && menu->sub.hovered_index >= 0) {
            struct window_menu_item it = menu->sub.items[menu->sub.hovered_index];
            struct woodland_view *view = menu->view;
            woodland_window_menu_hide(server);
            window_menu_execute_action(server, view, &it);
        }
        if (!menu->sub_has_focus && menu->root.hovered_index >= 0) {
            if (menu->root.items[menu->root.hovered_index].has_submenu) {
                window_menu_open_submenu(menu, menu->root.hovered_index);
                menu->sub_has_focus = true;
                for (int i = 0; i < menu->sub.item_count; i++) {
                    if (!menu->sub.items[i].is_separator && menu->sub.items[i].enabled) {
                        menu->sub.hovered_index = i;
                        break;
                    }
                }
                window_menu_render_level(menu, &menu->sub);
                return true;
            } else {
                struct window_menu_item it = menu->root.items[menu->root.hovered_index];
                struct woodland_view *view = menu->view;
                woodland_window_menu_hide(server);
                window_menu_execute_action(server, view, &it);
            }
        }
    }

    char c = 0;
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
        c = (char)('a' + (sym - XKB_KEY_a));
    } else if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
        c = (char)('a' + (sym - XKB_KEY_A));
    }
    if (c != 0) {
        for (int i = 0; i < menu->root.item_count; i++) {
            if (menu->root.items[i].enabled && menu->root.items[i].accel_key == c) {
                if (menu->root.items[i].has_submenu) {
                    window_menu_open_submenu(menu, i);
                    menu->sub_has_focus = true;
                    menu->sub.hovered_index = 0;
                    window_menu_render_level(menu, &menu->sub);
                } else {
                    struct window_menu_item it = menu->root.items[i];
                    struct woodland_view *view = menu->view;
                    woodland_window_menu_hide(server);
                    window_menu_execute_action(server, view, &it);
                }
                return true;
            }
        }
    }

    return true;
}

void woodland_window_menu_status(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    bool active = woodland_window_menu_is_active(server);
    woodland_json_obj_bool(&obj, "active", active);
    if (active && server->window_menu && server->window_menu->view) {
        woodland_json_obj_int(&obj, "view", (int64_t)server->window_menu->view->id);
        woodland_json_obj_int(&obj, "x", server->window_menu->root.x);
        woodland_json_obj_int(&obj, "y", server->window_menu->root.y);
    }
    woodland_json_obj_close(&obj);
}
