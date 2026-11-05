// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include "scene.h"
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "icon.h"
#include "ssd.h"
#include "text_buffer.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
/* SSD 颜色（对齐 labwc 默认主题 theme_builtin()，premultiplied RGBA float） */
static const float COLOR_TITLEBAR_ACTIVE[4] = {0.882f, 0.871f, 0.859f, 1.0f};   /* #e1dedb */
static const float COLOR_TITLEBAR_INACTIVE[4] = {0.965f, 0.961f, 0.957f, 1.0f}; /* #f6f5f4 */
static const float COLOR_BORDER[4] = {0.667f, 0.667f, 0.667f, 1.0f};            /* #aaaaaa */
static const float COLOR_TITLE_TEXT_ACTIVE[4] = {0.188f, 0.188f, 0.188f,
                                                 1.0f}; /* #303030 Deepin 优雅暗灰 */
static const float COLOR_TITLE_TEXT_INACTIVE[4] = {0.588f, 0.588f, 0.588f,
                                                   1.0f}; /* #969696 非激活弱化 */

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
    struct wlr_scene_rect *titlebar;
    struct wlr_scene_rect *border[BORDER_COUNT];
    struct wlr_scene_rect *button_bg[BUTTON_COUNT]; // 按钮悬停/按下底色（在图标之下）
    struct wlr_scene_buffer *button[BUTTON_COUNT];
    struct woodland_text_buffer *title_text;
    struct wlr_scene_buffer *title_icon; // 标题栏应用图标
    char *icon_key;                      // 当前加载图标使用的 app_id 键
    enum woodland_ssd_part hover_part;   // 悬停的按钮（NONE = 无）
    enum woodland_ssd_part pressed_part; // 按下的按钮（NONE = 无）
    int width;                           /* 内容宽（缓存的几何） */
    int height;                          /* 内容高 */
    bool maximized;                      /* 缓存的最大化状态 */
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

static void ssd_get_titlebar_color(const struct woodland_view *view, bool active, float out[4])
{
    if (view && view->server && view->server->config && view->server->config->loaded_path) {
        uint32_t c = active ? view->server->config->ssd.active_title_color
                            : view->server->config->ssd.inactive_title_color;
        color_to_rgba_float(c, out);
        return;
    }
    const float *def = active ? COLOR_TITLEBAR_ACTIVE : COLOR_TITLEBAR_INACTIVE;
    memcpy(out, def, sizeof(float) * 4);
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
    if (view && view->server && view->server->config && view->server->config->loaded_path) {
        uint32_t c = active ? view->server->config->ssd.active_border_color
                            : view->server->config->ssd.inactive_border_color;
        color_to_rgba_float(c, out);
        return;
    }
    memcpy(out, COLOR_BORDER, sizeof(float) * 4);
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

/*
 * 单条边框矩形的几何（相对内容原点）。woodland 没有圆角（圆角属 effects/），
 * 所以取 labwc 的直角/tiled 形态：上下边横向跨满（含左右边框的宽度），
 * 左右边纵向从标题栏顶延伸到底部——四边首尾相接，标题栏也被边框围住，
 * 不留 1px 的空隙。
 */
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
    /* 标题栏被隐藏后按钮不存在：残留的 hover 不能让关闭键"亮着" */
    if (ssd_titlebar_height(ssd->view) <= 0) {
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
    int bwn = ssd_button_width(ssd->view);
    struct wlr_buffer *close_buf =
        woodland_button_icon_buffer(WOODLAND_SSD_ICON_CLOSE, bwn, close_active);
    if (close_buf) {
        wlr_scene_buffer_set_buffer(ssd->button[BUTTON_CLOSE], close_buf);
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

    /* 边框：包住内容 + 标题栏，构成完整外框（负坐标） */
    for (int i = 0; i < BORDER_COUNT; i++) {
        int bx, by, rw, rh;
        ssd_border_geometry(ssd, i, &bx, &by, &rw, &rh);
        wlr_scene_node_set_position(&ssd->border[i]->node, bx, by);
        wlr_scene_rect_set_size(ssd->border[i], rw, rh);
    }

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
    int bh = WOODLAND_SSD_BUTTON_H;
    int bwn = ssd_button_width(ssd->view);
    int gap = WOODLAND_SSD_BUTTON_GAP;
    int by = -th + (th - bh) / 2;
    int x = w - gap - bwn;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        int idx = button_order[i];
        /* 底色在图标之下，位置与图标完全重合 */
        wlr_scene_node_set_position(&ssd->button_bg[idx]->node, x, by);
        wlr_scene_rect_set_size(ssd->button_bg[idx], bwn, bh);
        wlr_scene_node_set_position(&ssd->button[idx]->node, x, by);
        x -= gap + bwn;
    }

    /* 左侧应用图标：垂直居中于标题栏 */
    int icon_size = th >= 32 ? 22 : 16;
    int icon_pad = 8;
    if (ssd->title_icon) {
        int icon_y = -th + (th - icon_size) / 2;
        wlr_scene_node_set_position(&ssd->title_icon->node, icon_pad, icon_y);
    }

    /* 标题文字：按 GXDE/Deepin 风格全宽居中，左右分别与应用图标及控制按钮保持安全距离 */
    if (ssd->title_text) {
        int right_btns = BUTTON_COUNT * bwn + (BUTTON_COUNT - 1) * gap;
        int left_bound = ssd->title_icon ? (icon_pad + icon_size + 8) : 8;
        int avail_w = (w - right_btns - 8) - left_bound;
        if (avail_w < 1) {
            avail_w = 1;
        }
        const float *text_color = ssd->active ? COLOR_TITLE_TEXT_ACTIVE : COLOR_TITLE_TEXT_INACTIVE;
        woodland_text_buffer_update(ssd->title_text, ssd->view->title, avail_w, th, text_color,
                                    1.0f);

        int title_w = woodland_text_buffer_get_width(ssd->title_text);
        int tx = (w - title_w) / 2;
        if (tx < left_bound) {
            tx = left_bound;
        }
        if (tx + title_w > w - right_btns - 8) {
            tx = w - right_btns - 8 - title_w;
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
    int icon_size = th >= 32 ? 22 : 16;
    struct wlr_buffer *icon_buf = woodland_ssd_get_icon_buffer(view, icon_size, 1.0f);
    if (icon_buf) {
        if (ssd->title_icon) {
            wlr_scene_buffer_set_buffer(ssd->title_icon, icon_buf);
        } else {
            ssd->title_icon = wlr_scene_buffer_create(ssd->tree, icon_buf);
        }
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
    ssd->titlebar = make_rect(ssd->tree, 0, 0, 1, 1, tc);
    for (int i = 0; i < BORDER_COUNT; i++) {
        ssd->border[i] = make_rect(ssd->tree, 0, 0, 1, 1, bc);
    }
    /* 按钮底色：先建（在下层），图标后建（盖在上面）；默认隐藏 */
    for (int i = 0; i < BUTTON_COUNT; i++) {
        ssd->button_bg[i] = make_rect(ssd->tree, 0, 0, WOODLAND_SSD_BUTTON_W, WOODLAND_SSD_BUTTON_H,
                                      COLOR_BUTTON_HOVER);
        if (ssd->button_bg[i]) {
            wlr_scene_node_set_enabled(&ssd->button_bg[i]->node, false);
        }
    }

    /* 按钮符号（—/□/×，黑色，对齐 labwc 默认黑色按钮图标） */
    ssd->button[BUTTON_CLOSE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_CLOSE, WOODLAND_SSD_BUTTON_W);
    ssd->button[BUTTON_MAXIMIZE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_MAXIMIZE, WOODLAND_SSD_BUTTON_W);
    ssd->button[BUTTON_MINIMIZE] =
        woodland_button_icon_create(ssd->tree, WOODLAND_SSD_ICON_ICONIFY, WOODLAND_SSD_BUTTON_W);

    ssd->title_text = woodland_text_buffer_create(ssd->tree);

    ssd_update_icon(ssd);

    if (!ssd->titlebar || !ssd->border[BORDER_TOP] || !ssd->border[BORDER_BOTTOM] ||
        !ssd->border[BORDER_LEFT] || !ssd->border[BORDER_RIGHT] || !ssd->button_bg[BUTTON_CLOSE] ||
        !ssd->button_bg[BUTTON_MAXIMIZE] || !ssd->button_bg[BUTTON_MINIMIZE] ||
        !ssd->button[BUTTON_CLOSE] || !ssd->button[BUTTON_MAXIMIZE] ||
        !ssd->button[BUTTON_MINIMIZE]) {
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
    for (int i = 0; i < BORDER_COUNT; i++) {
        wlr_scene_node_set_enabled(&ssd->border[i]->node, !maximized);
    }
    /* 标题栏被客户端隐藏时，标题栏本体/按钮/图标/文字一起收掉——
     * 只不是"高度设成 0"，是为了不留下 0 尺寸的节点参与命中与渲染 */
    bool titlebar = ssd_titlebar_height(view) > 0;
    wlr_scene_node_set_enabled(&ssd->titlebar->node, titlebar);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        wlr_scene_node_set_enabled(&ssd->button[i]->node, titlebar);
        if (!titlebar) {
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
    ssd_update_icon(ssd);

    /* 最大化状态切换时更新按钮图标（最大化切换为还原图标，反之亦然） */
    bool max = view->maximized;
    if (ssd->maximized != max) {
        ssd->maximized = max;
        enum woodland_ssd_button_icon icon =
            max ? WOODLAND_SSD_ICON_RESTORE : WOODLAND_SSD_ICON_MAXIMIZE;
        int bwn = ssd_button_width(view);
        struct wlr_buffer *buf = woodland_button_icon_buffer(icon, bwn, false);
        if (buf) {
            wlr_scene_buffer_set_buffer(ssd->button[BUTTON_MAXIMIZE], buf);
            wlr_buffer_drop(buf);
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
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return;
    }
    bool changed = (ssd->active != active);
    ssd->active = active;
    float tc[4];
    ssd_get_titlebar_color(view, active, tc);
    wlr_scene_rect_set_color(ssd->titlebar, tc);
    float bc[4];
    ssd_get_border_color(view, active, bc);
    for (int i = 0; i < BORDER_COUNT; i++) {
        wlr_scene_rect_set_color(ssd->border[i], bc);
    }
    /* 激活态改变时，刷新标题栏排版及文本前景色 */
    if (changed) {
        ssd_layout(ssd);
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
        /* 按钮区（右侧） */
        int bh = WOODLAND_SSD_BUTTON_H;
        int bwn = ssd_button_width(ssd->view);
        int gap = WOODLAND_SSD_BUTTON_GAP;
        int by = -th + (th - bh) / 2;
        if (ry >= by && ry < by + bh) {
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

bool woodland_ssd_titlebar_visible(const struct woodland_view *view)
{
    return ssd_titlebar_height(view) > 0;
}

void woodland_ssd_set_corner_radius(struct woodland_view *view, int radius)
{
    struct woodland_ssd *ssd = view ? view->ssd : NULL;
    if (!ssd) {
        return;
    }
    struct fx_corner_radii top = corner_radii_top(radius);
    struct fx_corner_radii bottom = corner_radii_bottom(radius);
    struct fx_corner_radii square = corner_radii_none();
    wlr_scene_rect_set_corner_radii(ssd->titlebar, top);
    wlr_scene_rect_set_corner_radii(ssd->border[BORDER_TOP], top);
    wlr_scene_rect_set_corner_radii(ssd->border[BORDER_BOTTOM], bottom);
    /* 左右竖条保持直角：1px 宽的矩形套半径只会把边线啃掉一截 */
    wlr_scene_rect_set_corner_radii(ssd->border[BORDER_LEFT], square);
    wlr_scene_rect_set_corner_radii(ssd->border[BORDER_RIGHT], square);
}

void woodland_ssd_finish(void)
{
    woodland_icon_cache_finish();
}
