// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alt+Tab 窗口切换器：候选快照 + 一个居中面板。
 *
 * 状态机很小，只有三个动作：
 *   begin  按住 Alt 第一次按 Tab：拍候选快照、预选下一个、显示面板
 *   step   Alt 仍按着再按 Tab：只在快照里移动高亮，不动焦点、不动 z 序
 *   finish 松开 Alt（提交 / Escape 取消）：收起面板，提交时聚焦选中项
 *
 * 三条设计约束，改之前先读：
 *   1. **拍快照而不是每步重算**：聚焦会 raise（改 views 链表与 scene 序），
 *      边遍历边改会让连按 Tab 在窗口之间弹回。快照的生命周期就是"面板开着"
 *      这段时间，候选被 unmap/销毁时从快照里摘掉（见 handle_view_unmapped），
 *      保证面板里不会出现悬空指针。最小化**不摘**——那一行还在，只是标成
 *      "(已最小化)"，提交时先恢复再聚焦（见 switcher_finish）。
 *   2. **切换期间焦点不动**：我们只高亮，不逐个窗口真聚焦。少一次来回切换，
 *      也避免客户端收到一串 activated/inactivated 抖动。
 *   3. **渲染只依赖快照与配置**：面板是一块 cairo 画好的 buffer，尺寸按行数
 *      与最长标题算，滚动窗口跟着高亮走。不做缩略图、不做图标——这个模块
 *      的目标是"能看清楚在切谁"，不是复刻 treeland 的 switch-window。
 *
 * 键位与 focus_cycle 的分工见 window_switcher.h。
 */
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "multitask_view.h"
#include "output.h"
#include "placement.h"
#include "scene.h"
#include "session_lock.h"
#include "trace.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "window_menu.h"
#include "window_switcher.h"
#include "woodland.h"
#include "woodland_config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SWITCHER_ROW_HEIGHT 30
#define SWITCHER_PADDING 6
#define SWITCHER_TEXT_INSET 10
#define SWITCHER_MIN_WIDTH 260
#define SWITCHER_MAX_WIDTH 560
/* 一行的文本缓冲：标题 + 后缀，再长也没意义（面板最宽 560px 就 ellipsize 了） */
#define SWITCHER_ROW_TEXT_MAX 256

/* ==================== cairo 包装为 wlr_buffer ==================== */

/*
 * 和 window_menu.c 里那份结构相同（scenefx/wlroots 只认 wlr_buffer，
 * cairo 只认 image surface）。两处各自持有一份而不是抽公共模块，是因为
 * 生命周期完全贴着自己那棵树：这里唯一的持有者是 scene_buffer 的 buffer
 * 指针，销毁时机由 scene 决定，抽出去反而要引入"谁负责 drop"的约定。
 */
struct switcher_cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void switcher_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct switcher_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (b->surface) {
        cairo_surface_destroy(b->surface);
    }
    wlr_buffer_finish(&b->base);
    free(b);
}

static bool switcher_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                                                 void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct switcher_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (!b->surface || cairo_surface_status(b->surface) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    cairo_surface_flush(b->surface);
    *data = cairo_image_surface_get_data(b->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(b->surface);
    return true;
}

static void switcher_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    struct switcher_cairo_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_mark_dirty(b->surface);
}

static const struct wlr_buffer_impl switcher_buffer_impl = {
    .destroy = switcher_buffer_destroy,
    .begin_data_ptr_access = switcher_buffer_begin_data_ptr_access,
    .end_data_ptr_access = switcher_buffer_end_data_ptr_access,
};

static struct wlr_buffer *switcher_wrap_cairo_surface(cairo_surface_t *surf, int width, int height)
{
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (surf) {
            cairo_surface_destroy(surf);
        }
        return NULL;
    }
    struct switcher_cairo_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    int px_w = cairo_image_surface_get_width(surf);
    int px_h = cairo_image_surface_get_height(surf);
    wlr_buffer_init(&b->base, &switcher_buffer_impl, px_w > 0 ? px_w : width,
                    px_h > 0 ? px_h : height);
    return &b->base;
}

/* ==================== 状态 ==================== */

struct woodland_window_switcher {
    struct woodland_server *server;
    bool active;
    /* 面板树：scene 根的子节点，打开时抬到最上面（盖住 layer 的 overlay 层） */
    struct wlr_scene_tree *tree;
    struct wlr_scene_buffer *panel;
#if WOODLAND_HAVE_EFFECTS
    struct wlr_scene_shadow *shadow;
#endif
    /*
     * 只缓存"画出来的几何"，不缓存 `struct woodland_output *`。
     *
     * output 会在拔显示器时被 free（output.c 先摘链、发信号、再 free），
     * 而面板的生命周期是跨事件的：把指针存下来，就得处理"切换途中那个
     * 输出没了"。改成每次布局时现挑（switcher_relayout），指针的生命周期
     * 缩到一次函数调用之内，这个坑就不存在了。
     */
    float scale;
    int x, y;         /* 布局坐标下的面板左上角 */
    int width, height; /* 逻辑尺寸 */
    /*
     * 候选快照。定长内嵌（32 个指针 = 256 字节）而不是 malloc：切换期间
     * 不该有"分配失败"这条路径，count 只在 [0, MAX_ITEMS] 里动。
     */
    struct woodland_view *views[WOODLAND_SWITCHER_MAX_ITEMS];
    /*
     * 每项在拍快照时的最小化状态。**不读 view->minimized 现算**：这个值要
     * 用来判断"面板需要重画吗"，而 view_changed 发得很勤（标题、激活态），
     * 现算就分不出"真的变了"和"又发了一次"，每次都得整块重排重画。
     */
    bool minimized[WOODLAND_SWITCHER_MAX_ITEMS];
    int count;
    int selected; /* 高亮下标；-1 = 没有高亮 */
    int scroll;   /* 面板里第一行对应快照的哪一项（候选多于可见行时） */
    struct wl_listener view_unmapped;
    struct wl_listener view_changed;
    struct wl_listener lock_changed;
    struct wl_listener workspace_changed;
};

/* ==================== 候选与下标（纯逻辑） ==================== */

int woodland_switcher_next_index(int count, int from, bool forward)
{
    if (count <= 0) {
        return -1;
    }
    if (from < 0 || from >= count) {
        return forward ? 0 : count - 1;
    }
    return (from + (forward ? 1 : count - 1)) % count;
}

int woodland_switcher_initial_index(struct woodland_view *const *views, int count,
                                    const struct woodland_view *origin, bool forward)
{
    if (!views || count <= 0) {
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (views[i] == origin) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* 焦点不在候选集里（在 layer surface 上、或根本没有焦点）：
         * 往前取最上那个，往回取最下那个——和 focus_pick 的兜底一致 */
        return forward ? 0 : count - 1;
    }
    return woodland_switcher_next_index(count, idx, forward);
}

static int switcher_collect(struct woodland_server *server,
                            struct woodland_view **out, bool *minimized, int max)
{
    int n = 0;
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link)
    {
        if (n >= max) {
            break;
        }
        /*
         * 判定用 woodland_view_restorable()（focusable 忽略最小化）：
         * 最小化的窗口自己拿不到焦点，但仍然是个合法的切换目标——提交时
         * 先恢复再聚焦。其余条件（工作区 / mapped / override-redirect /
         * X11 焦点模型）和焦点回退那套完全一致，写在 focus.c 里一份。
         */
        if (woodland_view_restorable(view)) {
            minimized[n] = view->minimized;
            out[n++] = view;
        }
    }
    return n;
}

static int switcher_index_of(const struct woodland_window_switcher *sw,
                             const struct woodland_view *view)
{
    for (int i = 0; i < sw->count; i++) {
        if (sw->views[i] == view) {
            return i;
        }
    }
    return -1;
}

/* ==================== 绘制 ==================== */

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

static const char *switcher_label(const struct woodland_view *view)
{
    if (!view) {
        return "";
    }
    if (view->title && view->title[0]) {
        return view->title;
    }
    if (view->app_id && view->app_id[0]) {
        return view->app_id;
    }
    return is_zh_locale() ? "未命名窗口" : "Untitled window";
}

/*
 * 一行要显示的完整文本：标题 +（最小化时）后缀。
 *
 * 不能直接 snprintf("%s%s") 拼：标题很长时被截断的正好是尾巴上的后缀，
 * 而"已最小化"这个提示恰恰是最不能丢的信息。所以先给后缀留足位置再截
 * 标题，后缀整段追加。面板宽度那边还有 pango 的 ellipsize 兜一层。
 */
static void switcher_row_text(const struct woodland_view *view, bool minimized,
                              char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }
    const char *label = switcher_label(view);
    const char *suffix = "";
    if (minimized) {
        suffix = is_zh_locale() ? "  (已最小化)" : "  (minimized)";
    }
    size_t suffix_len = strlen(suffix);
    if (suffix_len + 1 >= size) {
        /* 缓冲小到放不下后缀：宁可不标，也不能写到界外 */
        suffix_len = 0;
    }
    size_t title_max = size - 1 - suffix_len;
    size_t label_len = strlen(label);
    size_t copy_len = label_len < title_max ? label_len : title_max;
    if (copy_len < label_len) {
        /* 回退到完整的 UTF-8 字符边界，避免把半个多字节字符交给 Pango */
        while (copy_len > 0 && ((unsigned char)label[copy_len] & 0xC0) == 0x80) {
            copy_len--;
        }
    }
    if (copy_len > 0) {
        memcpy(buf, label, copy_len);
    }
    memcpy(buf + copy_len, suffix, suffix_len);
    buf[copy_len + suffix_len] = '\0';
}

static void switcher_cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                                        double r)
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

static int switcher_visible_rows(const struct woodland_window_switcher *sw)
{
    if (sw->count <= 0) {
        return 0;
    }
    return sw->count < WOODLAND_SWITCHER_MAX_ROWS ? sw->count : WOODLAND_SWITCHER_MAX_ROWS;
}

static const char *switcher_font(const struct woodland_window_switcher *sw)
{
    if (sw->server->config && sw->server->config->ssd.font && sw->server->config->ssd.font[0]) {
        return sw->server->config->ssd.font;
    }
    return "Sans 10";
}

static bool switcher_dark(const struct woodland_window_switcher *sw)
{
    const struct woodland_config *cfg = sw->server->config;
    return cfg && cfg->appearance.color_scheme_set && cfg->appearance.dark;
}

static double switcher_corner_radius(const struct woodland_window_switcher *sw)
{
    int r = 0;
    const struct woodland_config *cfg = sw->server->config;
    if (cfg && cfg->effects.enabled) {
        r = cfg->effects.corner_radius;
    }
    if (r < 0) {
        r = 0;
    }
    int max_r = (sw->width < sw->height ? sw->width : sw->height) / 2;
    if (r > max_r) {
        r = max_r;
    }
    return (double)r;
}

static void switcher_measure(struct woodland_window_switcher *sw)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        sw->width = SWITCHER_MIN_WIDTH;
        sw->height = SWITCHER_PADDING * 2 + switcher_visible_rows(sw) * SWITCHER_ROW_HEIGHT;
        return;
    }
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(switcher_font(sw));
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }
    pango_layout_set_single_paragraph_mode(layout, TRUE);

    int max_text_w = 0;
    char text_buf[SWITCHER_ROW_TEXT_MAX];
    for (int i = 0; i < sw->count; i++) {
        switcher_row_text(sw->views[i], sw->minimized[i], text_buf, sizeof(text_buf));
        pango_layout_set_text(layout, text_buf, -1);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout, &tw, &th);
        if (tw > max_text_w) {
            max_text_w = tw;
        }
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    int w = SWITCHER_PADDING * 2 + SWITCHER_TEXT_INSET * 2 + max_text_w;
    if (w < SWITCHER_MIN_WIDTH) {
        w = SWITCHER_MIN_WIDTH;
    }
    if (w > SWITCHER_MAX_WIDTH) {
        w = SWITCHER_MAX_WIDTH;
    }
    sw->width = w;
    sw->height = SWITCHER_PADDING * 2 + switcher_visible_rows(sw) * SWITCHER_ROW_HEIGHT;
}

/* 让高亮始终落在可见行窗口内（候选多于可见行时才有意义） */
static void switcher_clamp_scroll(struct woodland_window_switcher *sw)
{
    int rows = switcher_visible_rows(sw);
    if (rows <= 0 || sw->selected < 0) {
        sw->scroll = 0;
        return;
    }
    if (sw->selected < sw->scroll) {
        sw->scroll = sw->selected;
    }
    if (sw->selected >= sw->scroll + rows) {
        sw->scroll = sw->selected - rows + 1;
    }
    int max_scroll = sw->count - rows;
    if (sw->scroll > max_scroll) {
        sw->scroll = max_scroll;
    }
    if (sw->scroll < 0) {
        sw->scroll = 0;
    }
}

static void switcher_place(struct woodland_window_switcher *sw,
                           struct woodland_output *output)
{
    struct wlr_box area = {0};
    if (output) {
        woodland_output_workarea(output, &area);
    }
    if (area.width <= 0 || area.height <= 0) {
        area = (struct wlr_box){0, 0, 1920, 1080};
    }
    sw->x = area.x + (area.width - sw->width) / 2;
    sw->y = area.y + (area.height - sw->height) / 2;
}

static void switcher_update_shadow(struct woodland_window_switcher *sw, double corner_r)
{
#if WOODLAND_HAVE_EFFECTS
    const struct woodland_config *cfg = sw->server->config;
    if (!cfg || !cfg->effects.enabled || !cfg->effects.shadow) {
        if (sw->shadow) {
            wlr_scene_node_set_enabled(&sw->shadow->node, false);
        }
        return;
    }
    float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.30f};
    if (!sw->shadow) {
        sw->shadow = wlr_scene_shadow_create(sw->tree, sw->width, sw->height, (int)corner_r, 12.0f,
                                             shadow_color);
    } else {
        wlr_scene_shadow_set_size(sw->shadow, sw->width, sw->height);
        wlr_scene_shadow_set_corner_radius(sw->shadow, (int)corner_r);
        wlr_scene_shadow_set_blur_sigma(sw->shadow, 12.0f);
        wlr_scene_shadow_set_color(sw->shadow, shadow_color);
    }
    if (!sw->shadow) {
        return;
    }
    wlr_scene_node_set_position(&sw->shadow->node, sw->x, sw->y + 2);
    wlr_scene_node_set_enabled(&sw->shadow->node, true);
    if (sw->panel) {
        wlr_scene_node_place_below(&sw->shadow->node, &sw->panel->node);
    }
#else
    (void)sw;
    (void)corner_r;
#endif
}

static void switcher_render(struct woodland_window_switcher *sw)
{
    if (!sw->tree || sw->count <= 0 || sw->width <= 0 || sw->height <= 0) {
        return;
    }
    float scale = sw->scale > 0.0f ? sw->scale : 1.0f;
    int px_w = (int)ceil(sw->width * scale);
    int px_h = (int)ceil(sw->height * scale);
    if (px_w <= 0 || px_h <= 0) {
        return;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *cr = cairo_create(surf);

    bool dark = switcher_dark(sw);
    float bg[4] = {0.98f, 0.98f, 0.98f, 0.98f};
    float border[4] = {0.80f, 0.80f, 0.80f, 0.90f};
    float text[4] = {0.12f, 0.12f, 0.12f, 1.0f};
    float sel_bg[4] = {0.18f, 0.53f, 0.93f, 0.92f};
    float sel_text[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (dark) {
        bg[0] = 0.16f; bg[1] = 0.16f; bg[2] = 0.16f;
        border[0] = 0.32f; border[1] = 0.32f; border[2] = 0.32f;
        text[0] = 0.92f; text[1] = 0.92f; text[2] = 0.92f;
    }
    const struct woodland_config *cfg = sw->server->config;
    if (cfg && cfg->appearance.accent_set) {
        uint32_t rgb = cfg->appearance.accent_rgb;
        sel_bg[0] = ((rgb >> 16) & 0xff) / 255.0f;
        sel_bg[1] = ((rgb >> 8) & 0xff) / 255.0f;
        sel_bg[2] = (rgb & 0xff) / 255.0f;
    }

    /*
     * 最小化行压暗：只在浅色/深色文字里改 alpha，不另配一套颜色——配色
     * （含 accent）有三层来源，多一套就是多一处要同步的常量。选中那行
     * 不受影响：高亮底色已经把"要切到它"说得够清楚了。
     */
    float dim[4] = {text[0], text[1], text[2], 0.55f};

    double corner_r = switcher_corner_radius(sw);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    switcher_cairo_rounded_rect(cr, 0.5, 0.5, sw->width - 1.0, sw->height - 1.0, corner_r);
    cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], bg[3]);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, border[0], border[1], border[2], border[3]);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(switcher_font(sw));
    if (desc) {
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
    }
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(
        layout, (sw->width - 2 * SWITCHER_PADDING - 2 * SWITCHER_TEXT_INSET) * PANGO_SCALE);

    int rows = switcher_visible_rows(sw);
    for (int i = 0; i < rows; i++) {
        int idx = sw->scroll + i;
        if (idx < 0 || idx >= sw->count) {
            break;
        }
        int y = SWITCHER_PADDING + i * SWITCHER_ROW_HEIGHT;
        bool selected = (idx == sw->selected);
        if (selected) {
            switcher_cairo_rounded_rect(cr, SWITCHER_PADDING, y,
                                        sw->width - 2 * SWITCHER_PADDING,
                                        SWITCHER_ROW_HEIGHT - 2, 4.0);
            cairo_set_source_rgba(cr, sel_bg[0], sel_bg[1], sel_bg[2], sel_bg[3]);
            cairo_fill(cr);
        }
        float *fg = selected ? sel_text : (sw->minimized[idx] ? dim : text);
        cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
        char text_buf[SWITCHER_ROW_TEXT_MAX];
        switcher_row_text(sw->views[idx], sw->minimized[idx], text_buf, sizeof(text_buf));
        pango_layout_set_text(layout, text_buf, -1);
        /*
         * pango 的 (x,y) 是文本**左上角**，不是基线。按行高中点直接 move_to
         * 会让文字整体下移半行、被面板底边切掉——第一版就是这么画歪的。
         * 先量一次高度再居中。
         */
        int text_h = 0;
        pango_layout_get_pixel_size(layout, NULL, &text_h);
        cairo_move_to(cr, SWITCHER_PADDING + SWITCHER_TEXT_INSET,
                      y + (SWITCHER_ROW_HEIGHT - text_h) / 2.0);
        pango_cairo_show_layout(cr, layout);
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    struct wlr_buffer *buf = switcher_wrap_cairo_surface(surf, sw->width, sw->height);
    if (!buf) {
        return;
    }
    if (!sw->panel) {
        sw->panel = wlr_scene_buffer_create(sw->tree, buf);
    } else {
        wlr_scene_buffer_set_buffer(sw->panel, buf);
    }
    wlr_buffer_drop(buf);

    if (sw->panel) {
        wlr_scene_buffer_set_dest_size(sw->panel, sw->width, sw->height);
        wlr_scene_node_set_position(&sw->panel->node, sw->x, sw->y);
        wlr_scene_node_set_enabled(&sw->panel->node, true);
    }
    switcher_update_shadow(sw, corner_r);
}

/* 内容变了（候选增删、高亮移动）：滚动窗口跟上，重画 */
static void switcher_refresh(struct woodland_window_switcher *sw)
{
    switcher_clamp_scroll(sw);
    switcher_render(sw);
}

/* 挑面板落在哪个输出：跟焦点窗口走，没有焦点就第一个。顺带把 scale 记下来 */
static struct woodland_output *switcher_pick_output(struct woodland_window_switcher *sw)
{
    struct woodland_server *server = sw->server;
    struct woodland_view *focus = woodland_focus_current(server);
    struct woodland_output *output = focus ? woodland_output_for_view(focus) : NULL;
    if (!output) {
        output = woodland_output_first(server);
    }
    if (output && output->wlr_output && output->wlr_output->scale > 0.0f) {
        sw->scale = output->wlr_output->scale;
    } else {
        sw->scale = 1.0f;
    }
    return output;
}

/*
 * 重新量尺寸、重新定位、重画。凡是"候选集或所在输出可能变了"的时机都调它：
 * 打开时、切换途中掉了一个窗口（行数变了）。output 是局部变量，函数返回即
 * 失效——这正是我们要的，见结构体里那段注释。
 */
static void switcher_relayout(struct woodland_window_switcher *sw)
{
    struct woodland_output *output = switcher_pick_output(sw);
    switcher_measure(sw);
    switcher_place(sw, output);
    switcher_refresh(sw);
}

/* ==================== 生命周期 ==================== */

static void switcher_finish(struct woodland_server *server, bool apply)
{
    struct woodland_window_switcher *sw = server ? server->window_switcher : NULL;
    if (!sw || !sw->active) {
        return;
    }
    struct woodland_view *target = NULL;
    if (apply && sw->selected >= 0 && sw->selected < sw->count) {
        target = sw->views[sw->selected];
    }
    sw->active = false;
    if (sw->tree) {
        wlr_scene_node_set_enabled(&sw->tree->node, false);
    }
    /*
     * 放掉面板 buffer：scene_buffer 持有的是我们包的 cairo 缓冲，一个
     * 560×300 的 ARGB 面就是 600KB 出头。面板关着的时候它不会被渲染，
     * 留着只是等下一次 Alt+Tab 再整块重画（重画本来就要重做）。
     */
    if (sw->panel) {
        wlr_scene_buffer_set_buffer(sw->panel, NULL);
    }
    sw->count = 0;
    sw->selected = -1;
    sw->scroll = 0;
    if (target) {
        woodland_trace_add("switcher", target->id, "commit");
        /*
         * 最小化的目标：woodland_focus_view() 会先恢复再聚焦（一步到位，
         * 见 focus.c 里那段注释），这里只是把"多做了这一步"记进轨迹——
         * 从外面看，"切到一个最小化窗口"和"切到一个普通窗口"的差别就只有
         * 这条 trace，出问题时不给它就会看成一团。
         *
         * 判据读的是 view 自己的字段而不是快照里的标记：快照那份是给"要不要
         * 重画"用的，这里要的是"马上会发生什么"，两个问题不要混。
         */
        if (target->minimized) {
            woodland_trace_add("switcher", target->id, "restore");
        }
        /* raise=true：切换器选中的窗口到最上面，和"点任务栏激活"一样 */
        woodland_focus_view(target, true);
    } else {
        woodland_trace_add("switcher", 0, "cancel");
    }
}

/*
 * 锁屏开始时面板必须立刻收起。
 *
 * 光靠"锁屏树 raise_to_top 盖住面板"不够：面板的 active 状态还留着，
 * 解锁后第一次 Tab 会落进一个陈旧的高亮项，松手就换焦点——用户看到的是
 * "解锁后突然跳了一下窗口"。信号发出时锁定状态已经落定，直接读即可。
 */
static void handle_lock_changed(struct wl_listener *listener, void *data)
{
    struct woodland_window_switcher *sw = wl_container_of(listener, sw, lock_changed);
    (void)data;
    if (woodland_session_lock_is_locked(sw->server)) {
        switcher_finish(sw->server, false);
    }
}

/*
 * 切换工作区时面板必须立刻收起。
 *
 * 快照记录的是打开那一刻所在工作区的候选；若面板开着时外部经
 * ext-workspace / treeland-compositor-action 切走了工作区，
 * workspace_switch 不会逐个发 view_changed，快照里就全成了旧工作区的窗口。
 */
static void handle_workspace_changed(struct wl_listener *listener, void *data)
{
    struct woodland_window_switcher *sw = wl_container_of(listener, sw, workspace_changed);
    (void)data;
    if (sw->active) {
        switcher_finish(sw->server, false);
    }
}

/*
 * 把第 idx 项从快照里摘掉：剩下的是指针数组，memmove 让顺序保持 z 序。
 * 先摘再从剩余项里挑高亮——被摘的那一项本身就可能是当前高亮。
 * 摘空了就顺手收面板（没有候选的 Alt+Tab 面板没有意义）。
 * 行数变了面板高度也要跟着变，所以摘完要走一次 relayout。
 */
static void switcher_drop(struct woodland_window_switcher *sw, int idx)
{
    for (int i = idx; i + 1 < sw->count; i++) {
        sw->views[i] = sw->views[i + 1];
        sw->minimized[i] = sw->minimized[i + 1];
    }
    sw->count--;
    if (sw->count == 0) {
        switcher_finish(sw->server, false);
        return;
    }
    if (idx < sw->selected) {
        sw->selected--;
    }
    if (sw->selected >= sw->count) {
        sw->selected = sw->count - 1;
    }
    if (sw->selected < 0) {
        sw->selected = 0;
    }
    switcher_relayout(sw);
}

static void handle_view_unmapped(struct wl_listener *listener, void *data)
{
    struct woodland_window_switcher *sw = wl_container_of(listener, sw, view_unmapped);
    struct woodland_view *view = data;
    if (!sw->active || !view) {
        return;
    }
    int idx = switcher_index_of(sw, view);
    if (idx < 0) {
        return;
    }
    switcher_drop(sw, idx);
    woodland_trace_add("switcher", view->id, "drop n=%d sel=%d", sw->count, sw->selected);
}

/*
 * `view_changed` 也要看：**最小化不发 `view_unmapped`**（toplevel_state.c 的
 * `woodland_view_set_minimized` 只是关掉 scene 节点再发 `view_changed`）。
 * 被挪到别的工作区、或者客户端自己 unmap 了同样只走这里。
 *
 * 判定顺序是**先"还能不能切"、再"最小化状态变没变"**，不能反过来：
 * 一次信号里两件事同时成立（最小化 + 被挪去别的工作区）时，反过来写会走进
 * "更新标记、重排重画"那条分支然后 return，于是面板里留下一行**已经切不过去**
 * 的候选，要等下一个信号才摘掉。先判 restorable 就没有这个中间态。
 *
 *   - 不再是候选（换了工作区、unmap 了）→ 摘掉；
 *   - 最小化状态变了 → 行**留着**（Alt+Tab 也是把最小化窗口叫回来的入口），
 *     更新标记后重排重画（后缀会改宽度）；
 *   - 其余（标题 / 激活态之类）→ 直接返回，这个信号发得很勤，不能每次都重画。
 */
static void handle_view_changed(struct wl_listener *listener, void *data)
{
    struct woodland_window_switcher *sw = wl_container_of(listener, sw, view_changed);
    struct woodland_view *view = data;
    if (!sw->active || !view) {
        return;
    }
    /* 先按指针查下标：快照外的窗口（另一个工作区的、图标之类）跟我们无关，
     * 这个信号一发就是全场广播，别在这里做多余的事 */
    int idx = switcher_index_of(sw, view);
    if (idx < 0) {
        return;
    }
    if (!woodland_view_restorable(view)) {
        switcher_drop(sw, idx);
        woodland_trace_add("switcher", view->id, "drop n=%d sel=%d", sw->count, sw->selected);
        return;
    }
    if (view->minimized == sw->minimized[idx]) {
        return;
    }
    sw->minimized[idx] = view->minimized;
    /* 行数不变，但文本变了（后缀），高度不变宽度可能变 → 走 relayout */
    switcher_relayout(sw);
    if (view->minimized) {
        woodland_trace_add("switcher", view->id, "min n=%d sel=%d", sw->count, sw->selected);
    } else {
        woodland_trace_add("switcher", view->id, "unmin n=%d sel=%d", sw->count, sw->selected);
    }
}

bool woodland_window_switcher_init(struct woodland_server *server)
{
    if (!server) {
        return false;
    }
    struct woodland_window_switcher *sw = calloc(1, sizeof(*sw));
    if (!sw) {
        return false;
    }
    sw->server = server;
    sw->selected = -1;
    if (server->scene) {
        sw->tree = wlr_scene_tree_create(&server->scene->tree);
        if (!sw->tree) {
            free(sw);
            return false;
        }
        wlr_scene_node_set_enabled(&sw->tree->node, false);
    }
    woodland_listener_connect(&sw->view_unmapped, &server->view_unmapped, handle_view_unmapped,
                              "switcher.unmapped");
    woodland_listener_connect(&sw->view_changed, &server->view_changed, handle_view_changed,
                              "switcher.changed");
    /*
     * session_lock 在 server.c 里比 shell 早初始化，正常都拿得到；单测造的是
     * 裸 server（没有 session_lock），所以这里必须容忍 NULL。先 wl_list_init
     * 再条件连接：收尾时那个 link 永远处于"可 remove"的状态。
     */
    wl_list_init(&sw->lock_changed.link);
    if (server->session_lock) {
        woodland_listener_connect(&sw->lock_changed, &server->session_lock->lock_changed,
                                  handle_lock_changed, "switcher.lock");
    }
    /*
     * workspace_init 排在 shell_init 之后，这里若尚未初始化则先建信号，
     * workspace_init 会保留这条监听链表（见 workspace.c）。
     */
    if (!server->workspace_changed.listener_list.next) {
        wl_signal_init(&server->workspace_changed);
    }
    woodland_listener_connect(&sw->workspace_changed, &server->workspace_changed,
                              handle_workspace_changed, "switcher.workspace");
    server->window_switcher = sw;
    return true;
}

void woodland_window_switcher_finish(struct woodland_server *server)
{
    if (!server || !server->window_switcher) {
        return;
    }
    struct woodland_window_switcher *sw = server->window_switcher;
    switcher_finish(server, false);
    wl_list_remove(&sw->view_unmapped.link);
    wl_list_remove(&sw->view_changed.link);
    if (sw->lock_changed.link.prev) {
        wl_list_remove(&sw->lock_changed.link);
    }
    if (sw->workspace_changed.link.prev) {
        wl_list_remove(&sw->workspace_changed.link);
    }
    if (sw->tree) {
        wlr_scene_node_destroy(&sw->tree->node);
        sw->tree = NULL;
        sw->panel = NULL;
#if WOODLAND_HAVE_EFFECTS
        sw->shadow = NULL;
#endif
    }
    free(sw);
    server->window_switcher = NULL;
}

bool woodland_window_switcher_is_active(const struct woodland_server *server)
{
    return server && server->window_switcher && server->window_switcher->active;
}

bool woodland_window_switcher_begin(struct woodland_server *server, bool forward)
{
    /*
     * 正在拖动 / 缩放窗口时不接这一下。
     *
     * 那两个状态里指针是"主控"：键盘归 interactive 那一层（方向键微调、
     * Escape 取消，见 seat.c 的 cursor_mode 分支）。这时候开面板会把键盘
     * 整块抢走（Escape 变成取消切换而不是取消拖动），提交时还会在拖动
     * 途中改焦点——拖到一半窗口跳到别的层上，谁也说不清发生了什么。
     * 手势语义上也不对：Alt+Tab 是"选窗口"，不是"边拖边选"。
     */
    if (!server || !server->window_switcher || woodland_session_lock_is_locked(server) ||
        woodland_multitask_view_is_active(server) ||
        server->cursor_mode == WOODLAND_CURSOR_MOVE ||
        server->cursor_mode == WOODLAND_CURSOR_RESIZE) {
        return false;
    }
    struct woodland_window_switcher *sw = server->window_switcher;

    /*
     * 窗口菜单开着时先收起来。
     *
     * 不收的话两块面板块并存：面板在上面盖住菜单，提交后焦点换了、菜单还
     * 挂在**原来那个窗口**上——接下来一个 Enter 就会作用到旧窗口（典型的
     * "切换完莫名其妙关掉一个窗口"）。Alt+Tab 是"选窗口"的模态手势，右键
     * 菜单在这个上下文里没有意义，直接收掉最省事。
     */
    woodland_window_menu_hide(server);

    if (sw->active) {
        return woodland_window_switcher_step(server, forward);
    }
    if (!sw->tree) {
        return false;
    }

    struct woodland_view *origin = woodland_focus_current(server);
    sw->count = switcher_collect(server, sw->views, sw->minimized, WOODLAND_SWITCHER_MAX_ITEMS);
    if (sw->count == 0) {
        /*
         * 一个能切的目标都没有：不弹空面板，也不吞这一下 Alt+Tab。
         * 注意"能切"包含最小化的窗口——只剩它们时面板照开，那是把窗口
         * 叫回来最快的路径（提交时 focus_view 会先恢复再聚焦）。
         */
        return false;
    }
    sw->selected = woodland_switcher_initial_index(sw->views, sw->count, origin, forward);
    sw->scroll = 0;
    sw->active = true;

    switcher_relayout(sw);

    wlr_scene_node_set_enabled(&sw->tree->node, true);
    wlr_scene_node_raise_to_top(&sw->tree->node);
    /* 拖拽图标树必须始终在最顶层（满足 selfcheck 的 dnd_icon 不变量） */
    if (server->dnd.drag && server->dnd.icons) {
        wlr_scene_node_raise_to_top(&server->dnd.icons->node);
    }

    woodland_trace_add("switcher", origin ? origin->id : 0, "begin n=%d dir=%s sel=%d", sw->count,
                       forward ? "fwd" : "back", sw->selected);
    return true;
}

bool woodland_window_switcher_step(struct woodland_server *server, bool forward)
{
    struct woodland_window_switcher *sw = server ? server->window_switcher : NULL;
    if (!sw || !sw->active) {
        return false;
    }
    if (sw->count <= 1) {
        /* 只有一个候选：连按 Tab 也只是把它自己再高亮一次 */
        return true;
    }
    int next = woodland_switcher_next_index(sw->count, sw->selected, forward);
    if (next < 0 || next == sw->selected) {
        return true;
    }
    sw->selected = next;
    switcher_refresh(sw);
    return true;
}

void woodland_window_switcher_cancel(struct woodland_server *server)
{
    switcher_finish(server, false);
}

bool woodland_window_switcher_handle_key(struct woodland_server *server, xkb_keysym_t sym,
                                         uint32_t state, uint32_t modifiers)
{
    struct woodland_window_switcher *sw = server ? server->window_switcher : NULL;
    if (!sw || !sw->active) {
        return false;
    }

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        /*
         * 松开 Alt = 提交。这里只看 keysym 不看 modifiers：wlr_keyboard 是否
         * 已经把这一下写进 xkb 状态是内部行为，靠它判断"Alt 还在不在"
         * 属于依赖实现细节。其余按键的 release 一律吞掉——press 已经被
         * 切换器吃掉，孤立的 release 下发给客户端会让它的按键状态错乱
         * （和 seat.c 里 VT 键的孤儿 release 同一套理由）。
         */
        if (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R) {
            switcher_finish(server, true);
        }
        return true;
    }

    switch (sym) {
    case XKB_KEY_Tab:
        woodland_window_switcher_step(server, (modifiers & WLR_MODIFIER_SHIFT) == 0);
        return true;
    case XKB_KEY_ISO_Left_Tab:
        /* Shift+Tab 在 xkb 里就是这个 keysym（compat 规则翻出来的） */
        woodland_window_switcher_step(server, false);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Down:
        woodland_window_switcher_step(server, true);
        return true;
    case XKB_KEY_Left:
    case XKB_KEY_Up:
        woodland_window_switcher_step(server, false);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        switcher_finish(server, true);
        return true;
    case XKB_KEY_Escape:
        switcher_finish(server, false);
        return true;
    default:
        /* 其它按键不解释，但也不下发：切换期间键盘归面板 */
        return true;
    }
}

int woodland_window_switcher_snapshot(const struct woodland_server *server,
                                      struct woodland_view *out[], int max, int *selected)
{
    const struct woodland_window_switcher *sw = server ? server->window_switcher : NULL;
    if (selected) {
        *selected = -1;
    }
    if (!sw || !out || max <= 0 || !sw->active) {
        return 0;
    }
    int n = sw->count < max ? sw->count : max;
    for (int i = 0; i < n; i++) {
        out[i] = sw->views[i];
    }
    if (selected) {
        *selected = sw->selected;
    }
    return n;
}

void woodland_window_switcher_status(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_window_switcher *sw = server ? server->window_switcher : NULL;
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "active", sw ? sw->active : false);
    woodland_json_obj_int(&obj, "count", sw ? sw->count : 0);
    woodland_json_obj_int(&obj, "index", sw ? sw->selected : -1);
    bool has_selected = sw && sw->active && sw->selected >= 0 && sw->selected < sw->count;
    woodland_json_obj_int(&obj, "selected", has_selected ? (int64_t)sw->views[sw->selected]->id : 0);
    woodland_json_obj_close(&obj);
}
