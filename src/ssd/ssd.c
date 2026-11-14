// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include "config.h"

#include "scene.h"
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>
#include "output.h"

#include "effect.h"
#include "icon.h"
#include "ssd.h"
#include "text_buffer.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
/* SSD 颜色（中性灰阶，与右键窗口菜单 window_menu.c 同一套观感；premultiplied RGBA float）
 * 原先的暖米色 #e1dedb 与菜单的近白浮层放一起明显割裂，这里统一成中性灰。
 *
 * 边框 0.80 / 文字 0.12 / 弱化 0.55 与菜单逐个相同；深色那一套也是逐个抄的。
 * 只有激活底色 0.957 是**故意**比菜单浮层的 0.98 深一档：标题栏是常驻 chrome，
 * 和浮层同亮度会失去层次；更实际的原因是浅色下激活/非激活只差 1/255 就分不出来了
 * （非激活 0.976，若激活也用 0.98 就等于没有状态区分）。 */
static const float COLOR_TITLEBAR_ACTIVE[4] = {0.957f, 0.957f, 0.957f, 1.0f};   /* #f4f4f4 */
static const float COLOR_TITLEBAR_INACTIVE[4] = {0.976f, 0.976f, 0.976f, 1.0f}; /* #f9f9f9 */
static const float COLOR_BORDER[4] = {0.800f, 0.800f, 0.800f, 1.0f};            /* #cccccc */
static const float COLOR_TITLE_TEXT_ACTIVE[4] = {0.12f, 0.12f, 0.12f,
                                                 1.0f}; /* #1f1f1f 与菜单文字一致 */
static const float COLOR_TITLE_TEXT_INACTIVE[4] = {0.55f, 0.55f, 0.55f,
                                                   1.0f}; /* #8c8c8c 非激活弱化 */

/*
 * 深色配色方案的内置调色板。
 *
 * 由 treeland-appearance-manager 的 set_color_scheme 选择，但**只在用户没有在
 * 配置文件里显式写颜色时**生效（[decorations] colors_specified）：写了颜色
 * 就是"我要这个颜色"，控制中心的一次请求不该把它盖掉。两个色板都取整数灰阶，
 * 和浅色板一样只用一种色相，避免和客户端的主题打架。
 */
static const float COLOR_TITLEBAR_ACTIVE_DARK[4] = {0.161f, 0.161f, 0.161f, 1.0f};   /* #292929 对齐菜单深色底 */
static const float COLOR_TITLEBAR_INACTIVE_DARK[4] = {0.125f, 0.125f, 0.125f, 1.0f}; /* #202020 */
static const float COLOR_BORDER_DARK[4] = {0.322f, 0.322f, 0.322f, 1.0f};            /* #525252 对齐菜单深色边框 */
static const float COLOR_TITLE_TEXT_ACTIVE_DARK[4] = {0.92f, 0.92f, 0.92f, 1.0f};    /* #ebebeb 对齐菜单深色文字 */
static const float COLOR_TITLE_TEXT_INACTIVE_DARK[4] = {0.50f, 0.50f, 0.50f,
                                                        1.0f}; /* #808080 */

/* 按钮态高亮覆盖层：GXDE/Deepin 风格，关闭按钮悬停警示红，最小化/最大化轻量暗色半透明 */
static const float COLOR_BUTTON_HOVER[4] = {0.0f, 0.0f, 0.0f, 0.08f};
static const float COLOR_BUTTON_PRESSED[4] = {0.0f, 0.0f, 0.0f, 0.18f};
static const float COLOR_BUTTON_CLOSE_HOVER[4] = {0.91f, 0.16f, 0.18f, 0.90f}; /* #e8292e 警示红 */
static const float COLOR_BUTTON_CLOSE_PRESSED[4] = {0.78f, 0.12f, 0.14f, 0.95f}; /* #c71f24 深红 */

enum { BORDER_TOP = 0, BORDER_BOTTOM, BORDER_LEFT, BORDER_RIGHT, BORDER_COUNT };
enum { BUTTON_CLOSE = 0, BUTTON_MAXIMIZE, BUTTON_MINIMIZE, BUTTON_COUNT };

struct woodland_ssd {
    struct woodland_view *view;
    struct wlr_scene_tree *tree;
    bool active;
#if WOODLAND_HAVE_EFFECTS
    /* 整框圆角描边：一个 rect + clipped_region 挖空内框，见 set_corner_radius */
    struct wlr_scene_rect *frame;
    int corners[4];                      /* 当前外框内半径（顺时针 TL/TR/BR/BL） */
#else
    struct wlr_scene_rect *border[BORDER_COUNT];
#endif
    struct wlr_scene_rect *titlebar;
    struct wlr_scene_rect *button_bg[BUTTON_COUNT]; // 按钮悬停/按下底色（在图标之下）
    struct wlr_scene_buffer *button[BUTTON_COUNT];
    struct woodland_text_buffer *title_text;
    struct wlr_scene_buffer *title_icon; // 标题栏应用图标
    char *icon_key;                      // 当前加载图标使用的 app_id 键
    enum woodland_ssd_part hover_part;   // 悬停的按钮（NONE = 无）
    enum woodland_ssd_part pressed_part; // 按下的按钮（NONE = 无）
    int width;                           /* 内容宽（缓存的几何） */
    int height;                          /* 内容高 */
    /* corner_radius 移动至上方的 #if WOODLAND_HAVE_EFFECTS 块内 */
    bool maximized;                      /* 缓存的最大化状态 */
    bool buttons_hidden;                 /* 窗口太窄，三个按钮整组收起（见 ssd_layout） */
    float scale;                         /* 渲染当前使用的输出缩放 */
};

/* 内容尺寸来源：走 view 的双后端访问器（xdg base geometry / X11 窗口尺寸） */
static void ssd_content_size(struct woodland_view *view, int *w, int *h)
{
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    *w = content.width;
    *h = content.height;
}

static int ssd_titlebar_height(const struct woodland_view *view)
{
    /*
     * treeland-decoration 的 titlebar_mode=hide：把标题栏高度当成 0。
     *
     * 不去改 config（那是全局的），而是在这里"折"掉：边距、边框几何、命中
     * 测试、按钮排版全都走这个函数或 ssd_border_width()，改一处就全线生效，
     * 不会出现"标题栏没了但边距还留着 26px"这种只改一半的状态。
     */
    if (view && view->ssd_overrides.titlebar_hidden) {
        return 0;
    }
    if (view && view->server && view->server->config) {
        return view->server->config->ssd.titlebar_height;
    }
    return WOODLAND_SSD_TITLEBAR_H;
}

static float ssd_view_scale(struct woodland_view *view)
{
    if (!view || !view->server) {
        return 1.0f;
    }
    struct woodland_output *wo = woodland_output_for_view(view);
    return (wo && wo->wlr_output && wo->wlr_output->scale > 0.0f) ? wo->wlr_output->scale : 1.0f;
}

static int ssd_border_width(const struct woodland_view *view)
{
    /* 客户端接管宽度后，配置里的 border_width 对这个窗口失效（协议 §opt-in） */
    if (view && view->ssd_overrides.border_set) {
        return view->ssd_overrides.border_width;
    }
    if (view && view->server && view->server->config) {
        return view->server->config->ssd.border_width;
    }
    return WOODLAND_SSD_BORDER_W;
}

static int ssd_grab_width(const struct woodland_view *view)
{
    int b = ssd_border_width(view);
    int g = WOODLAND_SSD_RESIZE_GRAB_W;
    if (view && view->server && view->server->config) {
        if (view->server->config->ssd.resize_grab_width > 0) {
            g = view->server->config->ssd.resize_grab_width;
        }
    }
    return b > g ? b : g;
}

static int ssd_button_width(const struct woodland_view *view)
{
    if (view && view->server && view->server->config) {
        return view->server->config->ssd.button_width;
    }
    return WOODLAND_SSD_BUTTON_W;
}

/*
 * 按钮格子的高度。
 *
 * 默认与标题栏等高（撑满）。客户端可以通过协议把标题栏调矮
 * （treeland-decoration set_titlebar_height），矮到比 BUTTON_H 还小时必须
 * 跟着缩，否则按钮会画到标题栏外面：布局（ssd_layout）和命中测试
 * （ssd_hit_relative）各算一遍的话，一旦漏改就会出现"看得见但点不到"。
 * 所以格子高度只在这里算一次，两处都调它。
 *
 * 注意这只是**格子**的高度，符号边长另算（ssd_button_glyph_size）：格子可以
 * 是扁的，符号不能是。
 */
static int ssd_button_height(int th)
{
    int bh = WOODLAND_SSD_BUTTON_H;
    if (th > 0 && bh > th) {
        bh = th;
    }
    return bh;
}

/*
 * 按钮符号的方形边长：取按钮格子的短边。
 *
 * 符号是 cairo 按 px×px 画的方形 buffer（见 woodland_button_icon_buffer），
 * dest 必须也是方形，否则 □ / × 会被拉伸。两个方向本来各自独立：宽度来自
 * [decorations] button_width（1~199 可配），高度跟标题栏走（协议能压到几十
 * 像素）。默认 32/32 时两者恰好相等，把问题完全掩盖住了——一旦不等，按
 * (bwn, bh) 设 dest 就会横向或纵向压扁；矮标题栏下还会溢出格子，把图标
 * 画到客户端内容上。所以只认短边。
 *
 * 栅格化（woodland_button_icon_buffer）和摆位（ssd_place_glyph）必须取同一个
 * 值：按 A 画、按 B 摆会拉伸发虚。所以这里和 ssd_button_height 一样，只留
 * 一个入口，别在调用点各写一遍 MIN。
 */
static int ssd_button_glyph_size(int bwn, int bh)
{
    int side = bwn < bh ? bwn : bh;
    return side > 0 ? side : 1;
}

/*
 * 把第 idx 个按钮符号摆进以 (x, y) 为左上角、bwn×bh 的格子：方形 dest + 居中。
 *
 * 底色 rect 和命中区仍然占满整格（见 ssd_layout / ssd_hit_relative），所以
 * 图标变小不会让"能点的范围"跟着缩水——只是点下去看不到的那圈空白也响应。
 */
static void ssd_place_glyph(struct woodland_ssd *ssd, int idx, int x, int y, int bwn, int bh)
{
    int side = ssd_button_glyph_size(bwn, bh);
    wlr_scene_buffer_set_dest_size(ssd->button[idx], side, side);
    wlr_scene_node_set_position(&ssd->button[idx]->node, x + (bwn - side) / 2,
                                y + (bh - side) / 2);
}

/*
 * 左侧应用图标边长，随标题栏高度走：比标题栏矮 12px，限制在 [16, 24]。
 *
 * 栅格化（ssd_update_icon）和摆位（ssd_layout）必须用同一个值：buffer 按 A
 * 画、dest_size 按 B 摆，A < B 时图标会被拉伸发虚。所以只保留这一个入口，
 * 不要在别处再写一遍阈值。
 */
static int ssd_icon_size(int th)
{
    int size = th - 12;
    if (size > 24) {
        size = 24;
    }
    if (size < 16) {
        size = 16;
    }
    return size;
}

static void color_to_rgba_float(uint32_t argb, float out[4])
{
    float a = ((argb >> 24) & 0xFF) / 255.0f;
    float r = (((argb >> 16) & 0xFF) / 255.0f) * a;
    float g = (((argb >> 8) & 0xFF) / 255.0f) * a;
    float b = ((argb & 0xFF) / 255.0f) * a;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

/* 0xRRGGBBAA（协议里 r,g,b,a 的书写顺序）→ premultiplied RGBA float */
static void rgba_to_rgba_float(uint32_t rgba, float out[4])
{
    float a = (rgba & 0xFF) / 255.0f;
    float r = (((rgba >> 24) & 0xFF) / 255.0f) * a;
    float g = (((rgba >> 16) & 0xFF) / 255.0f) * a;
    float b = (((rgba >> 8) & 0xFF) / 255.0f) * a;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

/*
 * 内置深色板生效的条件：配色方案是深色，且用户没有显式配 SSD 颜色。
 *
 * 标题文字不走这个判断（它没有单独的配置键，只看配色方案），所以别把文字
 * 颜色的分支并进来。抽成函数是因为标题栏和边框两处原来各写一遍，而
 * woodland_ssd_get_palette() 还要用第三次。
 */
static bool ssd_uses_dark_palette(const struct woodland_view *view)
{
    const struct woodland_config *cfg = (view && view->server) ? view->server->config : NULL;
    return cfg && cfg->appearance.color_scheme_set && cfg->appearance.dark &&
           !cfg->ssd.colors_specified;
}

static void ssd_get_titlebar_color(const struct woodland_view *view, bool active, float out[4])
{
    const struct woodland_config *cfg = (view && view->server) ? view->server->config : NULL;

    /* 深色方案优先于内置浅色板，但排在用户显式颜色之后 */
    if (ssd_uses_dark_palette(view)) {
        const float *dark = active ? COLOR_TITLEBAR_ACTIVE_DARK : COLOR_TITLEBAR_INACTIVE_DARK;
        memcpy(out, dark, sizeof(float) * 4);
        return;
    }
    if (cfg && cfg->loaded_path) {
        uint32_t c = active ? cfg->ssd.active_title_color : cfg->ssd.inactive_title_color;
        color_to_rgba_float(c, out);
        return;
    }
    const float *def = active ? COLOR_TITLEBAR_ACTIVE : COLOR_TITLEBAR_INACTIVE;
    memcpy(out, def, sizeof(float) * 4);
}

/* 强调色（0xRRGGBB，协议里没有 alpha）→ premultiplied RGBA，按不透明处理 */
static void accent_to_rgba_float(uint32_t rgb, float out[4])
{
    out[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = (rgb & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

static void ssd_get_border_color(const struct woodland_view *view, bool active, float out[4])
{
    /*
     * 协议只给一个颜色（没有激活/非激活两套），所以覆盖后两个状态同色：
     * 客户端要的是"我指定的边框"，不是"我指定了激活色"。想区分状态就
     * 不发 set_border，用配置里的两套色。
     */
    if (view && view->ssd_overrides.border_set) {
        rgba_to_rgba_float(view->ssd_overrides.border_rgba, out);
        return;
    }
    const struct woodland_config *cfg = (view && view->server) ? view->server->config : NULL;
    /*
     * 强调色只染激活窗口的边框：它代表"焦点在哪"，给非激活窗口也上色就
     * 分不出哪个是当前窗口了。用户显式配了边框色时不覆盖（同配色方案的规矩）。
     */
    if (cfg && cfg->appearance.accent_set && !cfg->ssd.colors_specified && active) {
        accent_to_rgba_float(cfg->appearance.accent_rgb, out);
        return;
    }
    if (ssd_uses_dark_palette(view)) {
        memcpy(out, COLOR_BORDER_DARK, sizeof(float) * 4);
        return;
    }
    if (cfg && cfg->loaded_path) {
        uint32_t c = active ? cfg->ssd.active_border_color : cfg->ssd.inactive_border_color;
        color_to_rgba_float(c, out);
        return;
    }
    memcpy(out, COLOR_BORDER, sizeof(float) * 4);
}

static unsigned channel_byte(float v)
{
    if (!(v > 0.0f)) { /* 顺手挡掉 NaN：!(v > 0) 对 NaN 为真 */
        return 0;
    }
    if (v > 255.0f) {
        return 255;
    }
    return (unsigned)(v + 0.5f);
}

/*
 * premultiplied RGBA float → 0xRRGGBB。
 *
 * 除回 alpha 是为了报出"作者写的颜色"而不是预乘后的暗色：边框覆盖值带
 * alpha 时预乘过的分量直接取 8 位会偏暗。SSD 自己的色板都是不透明的，
 * 这条分支只对客端覆盖有意义。
 */
static uint32_t rgba_float_to_rgb(const float c[4])
{
    if (c[3] <= 0.0f) {
        return 0;
    }
    float inv = 255.0f / c[3];
    return (channel_byte(c[0] * inv) << 16) | (channel_byte(c[1] * inv) << 8) |
           channel_byte(c[2] * inv);
}

void woodland_ssd_get_palette(const struct woodland_view *view,
                              struct woodland_ssd_palette *palette)
{
    if (!palette) {
        return;
    }
    float c[4];

    ssd_get_titlebar_color(view, true, c);
    palette->titlebar_active = rgba_float_to_rgb(c);
    ssd_get_titlebar_color(view, false, c);
    palette->titlebar_inactive = rgba_float_to_rgb(c);
    ssd_get_border_color(view, true, c);
    palette->border_active = rgba_float_to_rgb(c);
    ssd_get_border_color(view, false, c);
    palette->border_inactive = rgba_float_to_rgb(c);
    palette->dark = ssd_uses_dark_palette(view);
}

/* 标题文字颜色：跟随配色方案（配置里没有单独的标题文字色键） */
static const float *ssd_title_text_color(const struct woodland_view *view, bool active)
{
    const struct woodland_config *cfg = (view && view->server) ? view->server->config : NULL;
    if (cfg && cfg->appearance.color_scheme_set && cfg->appearance.dark) {
        return active ? COLOR_TITLE_TEXT_ACTIVE_DARK : COLOR_TITLE_TEXT_INACTIVE_DARK;
    }
    return active ? COLOR_TITLE_TEXT_ACTIVE : COLOR_TITLE_TEXT_INACTIVE;
}

/* 标题字体：配置里的 pango 描述串，NULL 交给 text_buffer 的内置默认 */
static const char *ssd_font(const struct woodland_view *view)
{
    const struct woodland_config *cfg = (view && view->server) ? view->server->config : NULL;
    return cfg ? cfg->ssd.font : NULL;
}

void woodland_ssd_get_margins(struct woodland_view *view, struct woodland_ssd_margins *margins)
{
    margins->left = margins->top = margins->right = margins->bottom = 0;
    if (!view) {
        return;
    }
    /* CSD（客户端自绘）与 fullscreen 以及配置关闭都没有服务端装饰占用空间 */
    if ((view->server && view->server->config && !view->server->config->ssd.enabled) ||
        view->deco_csd || view->fullscreen) {
        return;
    }
    int th = ssd_titlebar_height(view);
    int b = ssd_border_width(view);
    if (view->maximized) {
        /* 最大化隐藏边框、保留标题栏（见 ssd_apply_state） */
        margins->top = th;
        return;
    }
    margins->left = margins->right = margins->bottom = b;
    margins->top = th + b;
}

static struct wlr_scene_rect *make_rect(struct wlr_scene_tree *parent, int x, int y, int w, int h,
                                        const float color[4])
{
    struct wlr_scene_rect *r = wlr_scene_rect_create(parent, w, h, color);
    if (!r) {
        return NULL;
    }
    wlr_scene_node_set_position(&r->node, x, y);
    return r;
}

#if WOODLAND_HAVE_EFFECTS
/*
 * 外框（相对内容原点）：从 (-b, -th-b) 到 (w+b, h+b)。可见描边不再由四条
 * 1px 细条拼——细条在圆角处会被半径 clamp 掉、或者留下直角尖角，改由单个
 * frame rect + clipped_region 挖空内框来画（见 woodland_ssd_set_corner_radius）。
 * 下面的命中几何仍按四条边算，与渲染节点无关。
 */
static void ssd_frame_geometry(const struct woodland_ssd *ssd, int *out_x, int *out_y,
                               int *out_w, int *out_h)
{
    int b = ssd_border_width(ssd->view);
    *out_x = -b;
    *out_y = -ssd_titlebar_height(ssd->view) - b;
    *out_w = ssd->width + 2 * b;
    *out_h = ssd->height + ssd_titlebar_height(ssd->view) + 2 * b;
}

/*
 * 画一圈等宽的圆角描边：frame 是覆盖整个外框的实心 rect，clipped_region
 * 把内框挖掉，剩下的就是边框。外圆角必须比内圆角大一个边框厚度，弧线上的
 * 线宽才不会忽粗忽细（swayfx transaction.c / mango rect_apply_border 同款）。
 *
 * 内框的局部坐标是 (b, b, 内容宽, 标题栏高+内容高)：frame 节点本身在
 * (-b, -th-b)，所以挖空区正好落在内容 + 标题栏上，由它俩的圆角接住。
 */
static void ssd_apply_frame_corners(struct woodland_ssd *ssd)
{
    if (!ssd->frame || !ssd->titlebar) {
        return;
    }
    int b = ssd_border_width(ssd->view);
    int th = ssd_titlebar_height(ssd->view);
    int tl = ssd->corners[0];
    int tr = ssd->corners[1];
    int br = ssd->corners[2];
    int bl = ssd->corners[3];

    wlr_scene_rect_set_corner_radii(
        ssd->frame, corner_radii_new(tl > 0 ? tl + b : 0, tr > 0 ? tr + b : 0,
                                     br > 0 ? br + b : 0, bl > 0 ? bl + b : 0));
    wlr_scene_rect_set_corner_radii(ssd->titlebar, corner_radii_new(tl, tr, 0, 0));

    struct clipped_region clip = clipped_region_get_default();
    clip.area = (struct wlr_box){b, b, ssd->width, th + ssd->height};
    clip.corners = corner_radii_new(tl, tr, br, bl);
    wlr_scene_rect_set_clipped_region(ssd->frame, clip);
}
#else
static void ssd_border_geometry(const struct woodland_ssd *ssd, int index, int *out_x, int *out_y,
                                int *out_w, int *out_h)
{
    int w = ssd->width;
    int h = ssd->height;
    int th = ssd_titlebar_height(ssd->view);
    int b = ssd_border_width(ssd->view);
    switch (index) {
    case BORDER_TOP:
        *out_x = -b;
        *out_y = -th - b;
        *out_w = w + 2 * b;
        *out_h = b;
        break;
    case BORDER_BOTTOM:
        *out_x = -b;
        *out_y = h;
        *out_w = w + 2 * b;
        *out_h = b;
        break;
    case BORDER_LEFT:
        *out_x = -b;
        *out_y = -th;
        *out_w = b;
        *out_h = th + h;
        break;
    default: /* BORDER_RIGHT */
        *out_x = w;
        *out_y = -th;
        *out_w = b;
        *out_h = th + h;
        break;
    }
}
#endif

/* 命中测试边框几何：向外扩展为抓取边距（默认 8px），解决细边框难以拖拽的问题 */
static void ssd_border_hit_geometry(const struct woodland_ssd *ssd, int index, int *out_x, int *out_y,
                                    int *out_w, int *out_h)
{
    int w = ssd->width;
    int h = ssd->height;
    int th = ssd_titlebar_height(ssd->view);
    int g = ssd_grab_width(ssd->view);
    switch (index) {
    case BORDER_TOP:
        *out_x = -g;
        *out_y = -th - g;
        *out_w = w + 2 * g;
        *out_h = g;
        break;
    case BORDER_BOTTOM:
        *out_x = -g;
        *out_y = h;
        *out_w = w + 2 * g;
        *out_h = g;
        break;
    case BORDER_LEFT:
        *out_x = -g;
        *out_y = -th - g;
        *out_w = g;
        *out_h = th + h + 2 * g;
        break;
    default: /* BORDER_RIGHT */
        *out_x = w;
        *out_y = -th - g;
        *out_w = g;
        *out_h = th + h + 2 * g;
        break;
    }
}

/* SSD 部件 → 按钮下标（非按钮返回 -1） */
static int button_index(enum woodland_ssd_part part)
{
    switch (part) {
    case WOODLAND_SSD_BUTTON_CLOSE:
        return BUTTON_CLOSE;
    case WOODLAND_SSD_BUTTON_MAXIMIZE:
        return BUTTON_MAXIMIZE;
    case WOODLAND_SSD_BUTTON_MINIMIZE:
        return BUTTON_MINIMIZE;
    default:
        return -1;
    }
}

/* 按 hover/pressed 状态刷新按钮底色（图标不动，只叠一层半透明覆盖） */
static void ssd_update_buttons(struct woodland_ssd *ssd)
{
    int hovered = button_index(ssd->hover_part);
    int pressed = button_index(ssd->pressed_part);
    /* 标题栏隐藏或按钮因放不下而收起时按钮不存在：残留的 hover 不能让关闭键"亮着" */
    if (ssd_titlebar_height(ssd->view) <= 0 || ssd->buttons_hidden) {
        hovered = -1;
        pressed = -1;
    }
    for (int i = 0; i < BUTTON_COUNT; i++) {
        const float *color = NULL;
        if (i == BUTTON_CLOSE) {
            if (i == pressed) {
                color = COLOR_BUTTON_CLOSE_PRESSED;
            } else if (i == hovered) {
                color = COLOR_BUTTON_CLOSE_HOVER;
            }
        } else {
            if (i == pressed) {
                color = COLOR_BUTTON_PRESSED;
            } else if (i == hovered) {
                color = COLOR_BUTTON_HOVER;
            }
        }
        if (color) {
            wlr_scene_rect_set_color(ssd->button_bg[i], color);
        }
        wlr_scene_node_set_enabled(&ssd->button_bg[i]->node, color != NULL);
    }

    /* 关闭按钮悬停/按压时切换为鲜明的白色图标 */
    bool close_active = (hovered == BUTTON_CLOSE || pressed == BUTTON_CLOSE);
    int side = ssd_button_glyph_size(
        ssd_button_width(ssd->view), ssd_button_height(ssd_titlebar_height(ssd->view)));
    struct wlr_buffer *close_buf =
        woodland_button_icon_buffer(WOODLAND_SSD_ICON_CLOSE, side, close_active, ssd->scale);
    if (close_buf) {
        wlr_scene_buffer_set_buffer(ssd->button[BUTTON_CLOSE], close_buf);
        wlr_scene_buffer_set_dest_size(ssd->button[BUTTON_CLOSE], side, side);
        wlr_buffer_drop(close_buf);
    }
}

/*
 * 重新摆放所有组件（标题栏/边框/按钮）。
 *
 * th == 0（客户端 set_titlebar_mode(hide)）时标题栏那一整段整个跳过：这些
 * 节点的尺寸还停在上一次的值，位置也还在内容上方，让它们留在那儿只会被
 * 上一个分支（ssd_apply_state）关掉，重排反而会把 0 高的矩形算进去。
 */
static void ssd_layout(struct woodland_ssd *ssd)
{
    int w = ssd->width;
    int th = ssd_titlebar_height(ssd->view);
    if (th > 0) {
        /* 标题栏：内容顶部上方 */
        wlr_scene_node_set_position(&ssd->titlebar->node, 0, -th);
        wlr_scene_rect_set_size(ssd->titlebar, w, th);
    }

#if WOODLAND_HAVE_EFFECTS
    /* 整框描边：几何变了，clipped_region 的挖空区必须同步重算 */
    int fx, fy, fw, fh;
    ssd_frame_geometry(ssd, &fx, &fy, &fw, &fh);
    wlr_scene_node_set_position(&ssd->frame->node, fx, fy);
    wlr_scene_rect_set_size(ssd->frame, fw, fh);
    ssd_apply_frame_corners(ssd);
#else
    for (int i = 0; i < BORDER_COUNT; i++) {
        int bx, by, rw, rh;
        ssd_border_geometry(ssd, i, &bx, &by, &rw, &rh);
        wlr_scene_node_set_position(&ssd->border[i]->node, bx, by);
        wlr_scene_rect_set_size(ssd->border[i], rw, rh);
    }
#endif

    if (th <= 0) {
        return;
    }

    /* 按钮：labwc 默认 titleLayout "icon:iconify,max,close" 的右段，
     * 自右向左 close/max/iconify；左段是窗口图标（woodland 无窗口图标，留空） */
    static const int button_order[BUTTON_COUNT] = {
        BUTTON_CLOSE,
        BUTTON_MAXIMIZE,
        BUTTON_MINIMIZE,
    };
    int bh = ssd_button_height(th);
    int bwn = ssd_button_width(ssd->view);
    int gap = WOODLAND_SSD_BUTTON_GAP;
    int by = -th + (th - bh) / 2;
    int icon_size = ssd_icon_size(th);
    int icon_pad = 8;
    int right_btns = BUTTON_COUNT * bwn + (BUTTON_COUNT - 1) * gap;
    int left_bound = ssd->title_icon ? (icon_pad + icon_size + 8) : 8;

    /*
     * 放不下"图标 + 一点标题 + 三个按钮"时，整组按钮收起。
     *
     * 不收的话最左那个按钮会压到应用图标上：两个图形叠在一起谁也点不准，悬停
     * 底色还会盖住图标。收起后整条标题栏都能拖动，关闭/最大化仍可从右键菜单或
     * 快捷键走——labwc 对放不下的部件也是"不画"，而不是画到别人身上。
     *
     * 收起是整组而不是逐个收：只留一个关闭键看着能用，实际宽度只够画半个，
     * 比一个都没有更难解释。
     *
     * 这里仍然照常摆位，只留下"放不下"这个结论——开/关节点统一由
     * ssd_apply_state 负责。布局只管几何、状态只管显隐，两处都写会互相覆盖；
     * 而布局若因为放不下就跳过摆位，节点会停在创建时的 (0,0)，一旦谁把它重新
     * 打开，图标就画到内容区左上角去了。
     */
    ssd->buttons_hidden = w < left_bound + right_btns + 8;
    int x = w - gap - bwn;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        int idx = button_order[i];
        /* 底色铺满整格（也是命中区），符号在格子里居中 */
        wlr_scene_node_set_position(&ssd->button_bg[idx]->node, x, by);
        wlr_scene_rect_set_size(ssd->button_bg[idx], bwn, bh);
        ssd_place_glyph(ssd, idx, x, by, bwn, bh);
        x -= gap + bwn;
    }

    /* 左侧应用图标：垂直居中于标题栏（32px 标题栏配 20px 图标） */
    if (ssd->title_icon) {
        int icon_y = -th + (th - icon_size) / 2;
        wlr_scene_node_set_position(&ssd->title_icon->node, icon_pad, icon_y);
        wlr_scene_buffer_set_dest_size(ssd->title_icon, icon_size, icon_size);
    }

    /* 标题文字：按 GXDE/Deepin 风格全宽居中，左右分别与应用图标及控制按钮保持安全距离 */
    if (ssd->title_text) {
        /* 按钮收起时右侧不再需要让位 */
        int pad_right = ssd->buttons_hidden ? 0 : right_btns;
        int avail_w = (w - pad_right - 8) - left_bound;
        if (avail_w < 1) {
            avail_w = 1;
        }
        const float *text_color = ssd_title_text_color(ssd->view, ssd->active);
        woodland_text_buffer_update(ssd->title_text, ssd->view->title, avail_w, th, text_color,
                                    ssd->scale, ssd_font(ssd->view));

        int title_w = woodland_text_buffer_get_width(ssd->title_text);
        int tx = (w - title_w) / 2;
        if (tx < left_bound) {
            tx = left_bound;
        }
        if (tx + title_w > w - pad_right - 8) {
            tx = w - pad_right - 8 - title_w;
        }
        if (tx < left_bound) {
            tx = left_bound;
        }
        woodland_text_buffer_set_position(ssd->title_text, tx, -th);
    }
}

static void ssd_update_icon(struct woodland_ssd *ssd)
{
    struct woodland_view *view = ssd->view;
    const char *key = view->app_id ? view->app_id : (view->title ? view->title : "default");
    if (ssd->title_icon && ssd->icon_key && strcmp(ssd->icon_key, key) == 0) {
        return;
    }
    free(ssd->icon_key);
    ssd->icon_key = strdup(key);

    int th = ssd_titlebar_height(view);
    int icon_size = ssd_icon_size(th);
    struct wlr_buffer *icon_buf = woodland_ssd_get_icon_buffer(view, icon_size, ssd->scale);
    if (icon_buf) {
        if (ssd->title_icon) {
            wlr_scene_buffer_set_buffer(ssd->title_icon, icon_buf);
        } else {
            ssd->title_icon = wlr_scene_buffer_create(ssd->tree, icon_buf);
        }
        wlr_scene_buffer_set_dest_size(ssd->title_icon, icon_size, icon_size);
    }
}

void woodland_ssd_create(struct woodland_view *view)
{
    if (view->ssd) {
        return;
    }
    if (view->server && view->server->config && !view->server->config->ssd.enabled) {
        return;
    }
    struct woodland_ssd *ssd = calloc(1, sizeof(*ssd));
    if (!ssd) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_ssd");
        return;
    }
    ssd->view = view;
    ssd->tree = wlr_scene_tree_create(view->scene_tree);
    if (!ssd->tree) {
        free(ssd);
        return;
    }
    wlr_scene_node_lower_to_bottom(&ssd->tree->node);

    int w, h;
    ssd_content_size(view, &w, &h);
    ssd->width = w;
    ssd->height = h;
    ssd->active = false;
    ssd->hover_part = WOODLAND_SSD_NONE;
    ssd->pressed_part = WOODLAND_SSD_NONE;

    /* 位置/尺寸都交给 ssd_layout() 统一设置，这里只建节点（1x1 占位） */
    float tc[4];
    ssd_get_titlebar_color(view, false, tc);
    float bc[4];
    ssd_get_border_color(view, false, bc);
#if WOODLAND_HAVE_EFFECTS
    /* frame 先建：在最底层，标题栏与内容会盖住它的挖空区 */
    ssd->frame = make_rect(ssd->tree, 0, 0, 1, 1, bc);
#else
    for (int i = 0; i < BORDER_COUNT; i++) {
        ssd->border[i] = make_rect(ssd->tree, 0, 0, 1, 1, bc);
    }
#endif
    ssd->titlebar = make_rect(ssd->tree, 0, 0, 1, 1, tc);
    /* 按钮底色：先建（在下层），图标后建（盖在上面）；默认隐藏 */
    for (int i = 0; i < BUTTON_COUNT; i++) {
        ssd->button_bg[i] = make_rect(ssd->tree, 0, 0, WOODLAND_SSD_BUTTON_W, WOODLAND_SSD_BUTTON_H,
                                      COLOR_BUTTON_HOVER);
        if (ssd->button_bg[i]) {
            wlr_scene_node_set_enabled(&ssd->button_bg[i]->node, false);
        }
    }

    /* 按钮符号（—/□/×，黑色，对齐 labwc 默认黑色按钮图标）。
     * 边长走 ssd_button_glyph_size：方块格子（默认 32/32）之外，宽度可配、
     * 标题栏高度可被协议压矮，两个方向必须取同一个方形边长。 */
    ssd->scale = ssd_view_scale(view);
    int btn_side = ssd_button_glyph_size(ssd_button_width(view),
                                         ssd_button_height(ssd_titlebar_height(view)));
    ssd->button[BUTTON_CLOSE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_CLOSE, btn_side, ssd->scale);
    ssd->button[BUTTON_MAXIMIZE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_MAXIMIZE, btn_side, ssd->scale);
    ssd->button[BUTTON_MINIMIZE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_ICONIFY, btn_side, ssd->scale);

    ssd->title_text = woodland_text_buffer_create(ssd->tree);

    ssd_update_icon(ssd);

#if WOODLAND_HAVE_EFFECTS
    if (!ssd->frame || !ssd->titlebar || !ssd->button_bg[BUTTON_CLOSE] ||
        !ssd->button_bg[BUTTON_MAXIMIZE] || !ssd->button_bg[BUTTON_MINIMIZE] ||
        !ssd->button[BUTTON_CLOSE] || !ssd->button[BUTTON_MAXIMIZE] ||
        !ssd->button[BUTTON_MINIMIZE]) {
#else
    if (!ssd->titlebar || !ssd->border[BORDER_TOP] || !ssd->border[BORDER_BOTTOM] ||
        !ssd->border[BORDER_LEFT] || !ssd->border[BORDER_RIGHT] || !ssd->button_bg[BUTTON_CLOSE] ||
        !ssd->button_bg[BUTTON_MAXIMIZE] || !ssd->button_bg[BUTTON_MINIMIZE] ||
        !ssd->button[BUTTON_CLOSE] || !ssd->button[BUTTON_MAXIMIZE] ||
        !ssd->button[BUTTON_MINIMIZE]) {
#endif
        wlr_log(WLR_ERROR, "failed to create ssd scene rects");
        wlr_scene_node_destroy(&ssd->tree->node);
        free(ssd);
        return;
    }

    ssd_layout(ssd);
    view->ssd = ssd;
    woodland_ssd_update(view);
}

void woodland_ssd_destroy(struct woodland_view *view)
{
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return;
    }
    view->ssd = NULL;
    /* title_text 的 scene buffer 节点随 ssd->tree 级联销毁，这里只 free 封装 */
    if (ssd->title_text) {
        free(ssd->title_text);
    }
    free(ssd->icon_key);
    wlr_scene_node_destroy(&ssd->tree->node);
    free(ssd);
}

/* 依据视图状态决定 SSD 可见性：fullscreen 全隐；maximized 只留标题栏 */
static void ssd_apply_state(struct woodland_ssd *ssd)
{
    struct woodland_view *view = ssd->view;
    if (view->fullscreen) {
        wlr_scene_node_set_enabled(&ssd->tree->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&ssd->tree->node, true);

    bool maximized = view->maximized;
#if WOODLAND_HAVE_EFFECTS
    wlr_scene_node_set_enabled(&ssd->frame->node, !maximized);
#else
    for (int i = 0; i < BORDER_COUNT; i++) {
        wlr_scene_node_set_enabled(&ssd->border[i]->node, !maximized);
    }
#endif
    /* 标题栏被客户端隐藏时，标题栏本体/按钮/图标/文字一起收掉——
     * 只不是"高度设成 0"，是为了不留下 0 尺寸的节点参与命中与渲染 */
    bool titlebar = ssd_titlebar_height(view) > 0;
    wlr_scene_node_set_enabled(&ssd->titlebar->node, titlebar);
    /* buttons_hidden 是布局算出来的"放不下"：这个函数在 ssd_layout 之后跑，
     * 这里的 true 会把布局刚关掉的按钮重新打开，必须一起判断 */
    bool buttons = titlebar && !ssd->buttons_hidden;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        wlr_scene_node_set_enabled(&ssd->button[i]->node, buttons);
        if (!buttons) {
            wlr_scene_node_set_enabled(&ssd->button_bg[i]->node, false);
        }
    }
    if (ssd->title_icon) {
        wlr_scene_node_set_enabled(&ssd->title_icon->node, titlebar);
    }
    if (ssd->title_text) {
        woodland_text_buffer_set_enabled(ssd->title_text, titlebar);
    }
}

void woodland_ssd_update(struct woodland_view *view)
{
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return;
    }
    int w, h;
    ssd_content_size(view, &w, &h);
    ssd->width = w;
    ssd->height = h;
    float scale = ssd_view_scale(view);
    bool scale_changed = (ssd->scale != scale);
    ssd->scale = scale;
    if (scale_changed) {
        free(ssd->icon_key);
        ssd->icon_key = NULL;
    }
    ssd_update_icon(ssd);

    /* 最大化状态切换或缩放变化时更新按钮图标 */
    bool max = view->maximized;
    bool max_changed = (ssd->maximized != max);
    if (max_changed || scale_changed) {
        ssd->maximized = max;
        enum woodland_ssd_button_icon icon =
            max ? WOODLAND_SSD_ICON_RESTORE : WOODLAND_SSD_ICON_MAXIMIZE;
        int bwn = ssd_button_width(view);
        int side = ssd_button_glyph_size(bwn, ssd_button_height(ssd_titlebar_height(view)));
        struct wlr_buffer *buf = woodland_button_icon_buffer(icon, side, false, ssd->scale);
        if (buf) {
            wlr_scene_buffer_set_buffer(ssd->button[BUTTON_MAXIMIZE], buf);
            wlr_scene_buffer_set_dest_size(ssd->button[BUTTON_MAXIMIZE], side, side);
            wlr_buffer_drop(buf);
        }
        if (scale_changed) {
            /*
             * 关闭按钮的反色图标取决于悬停/按下态。重建时一律用非反色版本，
             * 会让"正亮着的关闭键"在缩放变化后突然变回深灰（红底深灰图标），
             * 直到指针再动一次才恢复——所以这里按当前态决定。
             */
            bool close_active = (ssd->hover_part == WOODLAND_SSD_BUTTON_CLOSE ||
                                 ssd->pressed_part == WOODLAND_SSD_BUTTON_CLOSE);
            struct wlr_buffer *min_buf =
                woodland_button_icon_buffer(WOODLAND_SSD_ICON_ICONIFY, side, false, ssd->scale);
            if (min_buf) {
                wlr_scene_buffer_set_buffer(ssd->button[BUTTON_MINIMIZE], min_buf);
                wlr_scene_buffer_set_dest_size(ssd->button[BUTTON_MINIMIZE], side, side);
                wlr_buffer_drop(min_buf);
            }
            struct wlr_buffer *close_buf = woodland_button_icon_buffer(
                WOODLAND_SSD_ICON_CLOSE, side, close_active, ssd->scale);
            if (close_buf) {
                wlr_scene_buffer_set_buffer(ssd->button[BUTTON_CLOSE], close_buf);
                wlr_scene_buffer_set_dest_size(ssd->button[BUTTON_CLOSE], side, side);
                wlr_buffer_drop(close_buf);
            }
        }
    }

    ssd_layout(ssd);
    ssd_apply_state(ssd);
    woodland_ssd_set_active(view, ssd->active);
    /* 几何/状态变了：圆角与模糊节点跟着内容尺寸重算 */
    woodland_effect_update(view);
}

/*
 * 装饰覆盖值变了：重排几何、刷新颜色、清掉可能指向已经消失的按钮的高亮。
 *
 * 为什么不是"调用方自己调 update()"：覆盖值会改变边距（标题栏在不在、
 * 边框多宽），三处状态必须一起收敛——几何（ssd_layout）、可见性
 * （ssd_apply_state，被 update 带上）、颜色（set_active）。漏掉任何一处
 * 的表现都是"改了但没反应"，而且只对某个属性出现，很难一次看全。
 */
void woodland_ssd_deco_changed(struct woodland_view *view)
{
    struct woodland_ssd *ssd = view ? view->ssd : NULL;
    if (!ssd) {
        /* 还没 map：覆盖值已经记在 view 上，SSD 创建时会读到 */
        return;
    }
    if (view->server) {
        /* 服务器侧也记着"哪个窗口的哪个按钮亮着"，一起清掉 */
        if (view->server->hover_view == view) {
            view->server->hover_view = NULL;
            view->server->hover_part = WOODLAND_SSD_NONE;
        }
        if (view->server->pressed_view == view) {
            view->server->pressed_view = NULL;
            view->server->pressed_part = WOODLAND_SSD_NONE;
        }
    }
    ssd->hover_part = WOODLAND_SSD_NONE;
    ssd->pressed_part = WOODLAND_SSD_NONE;
    woodland_ssd_update(view); /* layout + apply_state + set_active */
    ssd_update_buttons(ssd);
}

void woodland_ssd_set_active(struct woodland_view *view, bool active)
{
    struct woodland_ssd *ssd = view ? view->ssd : NULL;
    if (!ssd) {
        woodland_effect_update(view);
        return;
    }
    bool changed = (ssd->active != active);
    ssd->active = active;
    float tc[4];
    ssd_get_titlebar_color(view, active, tc);
    wlr_scene_rect_set_color(ssd->titlebar, tc);
    float bc[4];
    ssd_get_border_color(view, active, bc);
#if WOODLAND_HAVE_EFFECTS
    wlr_scene_rect_set_color(ssd->frame, bc);
#else
    for (int i = 0; i < BORDER_COUNT; i++) {
        wlr_scene_rect_set_color(ssd->border[i], bc);
    }
#endif
    /* 激活态改变时，刷新标题栏排版及文本前景色 */
    if (changed) {
        ssd_layout(ssd);
        woodland_effect_update(view);
    }
}

void woodland_ssd_set_hover(struct woodland_view *view, enum woodland_ssd_part part)
{
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd || ssd->hover_part == part) {
        return;
    }
    ssd->hover_part = part;
    ssd_update_buttons(ssd);
}

void woodland_ssd_set_pressed(struct woodland_view *view, enum woodland_ssd_part part)
{
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd || ssd->pressed_part == part) {
        return;
    }
    ssd->pressed_part = part;
    ssd_update_buttons(ssd);
}

/* 相对坐标（相对 view 内容原点）命中测试 */
static enum woodland_ssd_part ssd_hit_relative(struct woodland_ssd *ssd, double rx, double ry,
                                               uint32_t *edges)
{
    int w = ssd->width;
    int th = ssd_titlebar_height(ssd->view);

    /* 标题栏（含按钮） */
    if (rx >= 0 && rx < w && ry >= -th && ry < 0) {
        /* 按钮区（右侧）。按钮收起时这一带仍是标题栏：能拖，但点不到按钮 */
        int bh = ssd_button_height(th);
        int bwn = ssd_button_width(ssd->view);
        int gap = WOODLAND_SSD_BUTTON_GAP;
        int by = -th + (th - bh) / 2;
        if (!ssd->buttons_hidden && ry >= by && ry < by + bh) {
            int x = w - gap - bwn;
            if (rx >= x && rx < x + bwn) {
                return WOODLAND_SSD_BUTTON_CLOSE;
            }
            x -= gap + bwn;
            if (rx >= x && rx < x + bwn) {
                return WOODLAND_SSD_BUTTON_MAXIMIZE;
            }
            x -= gap + bwn;
            if (rx >= x && rx < x + bwn) {
                return WOODLAND_SSD_BUTTON_MINIMIZE;
            }
        }
        return WOODLAND_SSD_TITLEBAR;
    }

    /* 最大化或全屏视图不提供边框缩放 */
    if (ssd->view && (ssd->view->fullscreen || ssd->view->maximized)) {
        return WOODLAND_SSD_NONE;
    }

    /* 边框命中区域：向外扩展至 grab_width（默认 8px），使细边框也易于抓取 */
    static const enum woodland_ssd_part part_of[BORDER_COUNT] = {
        [BORDER_TOP] = WOODLAND_SSD_BORDER_TOP,
        [BORDER_BOTTOM] = WOODLAND_SSD_BORDER_BOTTOM,
        [BORDER_LEFT] = WOODLAND_SSD_BORDER_LEFT,
        [BORDER_RIGHT] = WOODLAND_SSD_BORDER_RIGHT,
    };
    static const uint32_t edge_of[BORDER_COUNT] = {
        [BORDER_TOP] = WLR_EDGE_TOP,
        [BORDER_BOTTOM] = WLR_EDGE_BOTTOM,
        [BORDER_LEFT] = WLR_EDGE_LEFT,
        [BORDER_RIGHT] = WLR_EDGE_RIGHT,
    };
    for (int i = 0; i < BORDER_COUNT; i++) {
        int bx, by, rw, rh;
        ssd_border_hit_geometry(ssd, i, &bx, &by, &rw, &rh);
        if (rx >= bx && rx < bx + rw && ry >= by && ry < by + rh) {
            *edges = edge_of[i];
            return part_of[i];
        }
    }

    return WOODLAND_SSD_NONE;
}

enum woodland_ssd_part woodland_ssd_at(struct woodland_view *view, double lx, double ly,
                                       uint32_t *resize_edges)
{
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return WOODLAND_SSD_NONE;
    }
    if (view->fullscreen) {
        return WOODLAND_SSD_NONE;
    }
    double rx = lx - view->scene_tree->node.x;
    double ry = ly - view->scene_tree->node.y;
    return ssd_hit_relative(ssd, rx, ry, resize_edges);
}

bool woodland_ssd_is_active(const struct woodland_view *view)
{
    return view && view->ssd && view->ssd->active;
}

bool woodland_ssd_content_size(const struct woodland_view *view, int *width, int *height)
{
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    if (!view || !view->ssd) {
        return false;
    }
    if (width) {
        *width = view->ssd->width;
    }
    if (height) {
        *height = view->ssd->height;
    }
    return true;
}

float woodland_ssd_get_scale(const struct woodland_view *view)
{
    return (view && view->ssd) ? view->ssd->scale : 1.0f;
}

bool woodland_ssd_titlebar_visible(const struct woodland_view *view)
{
    return ssd_titlebar_height(view) > 0;
}

void woodland_ssd_set_corners(struct woodland_view *view, int top_left, int top_right,
                              int bottom_right, int bottom_left)
{
#if WOODLAND_HAVE_EFFECTS
    struct woodland_ssd *ssd = view ? view->ssd : NULL;
    if (!ssd) {
        return;
    }
    ssd->corners[0] = top_left < 0 ? 0 : top_left;
    ssd->corners[1] = top_right < 0 ? 0 : top_right;
    ssd->corners[2] = bottom_right < 0 ? 0 : bottom_right;
    ssd->corners[3] = bottom_left < 0 ? 0 : bottom_left;
    ssd_apply_frame_corners(ssd);
#else
    (void)view;
    (void)top_left;
    (void)top_right;
    (void)bottom_right;
    (void)bottom_left;
#endif
}

void woodland_ssd_set_corner_radius(struct woodland_view *view, int radius)
{
    woodland_ssd_set_corners(view, radius, radius, radius, radius);
}

/*
 * 用户级外观变了（标题栏高度/字体/配色方案/强调色/图标主题），重排这个窗口。
 *
 * 与 woodland_ssd_deco_changed 的差别：这里还要丢掉图标缓存键。图标按
 * app_id 缓存，只有键被清掉，ssd_update_icon 才会重新按新主题查一遍——
 * 少了这一行，"改了图标主题"只对之后新开的窗口生效，看着像没生效。
 */
void woodland_ssd_refresh(struct woodland_view *view)
{
    struct woodland_ssd *ssd = view ? view->ssd : NULL;
    if (!ssd) {
        /* 还没 map：新值都在 config 里，创建时会读到 */
        return;
    }
    free(ssd->icon_key);
    ssd->icon_key = NULL;
    woodland_ssd_deco_changed(view);
}

void woodland_ssd_refresh_all(struct woodland_server *server)
{
    if (!server || !server->views.next) {
        return;
    }
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link)
    {
        woodland_ssd_refresh(view);
    }
}

void woodland_ssd_finish(void)
{
    woodland_icon_cache_finish();
}
