// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "ssd.h"
#include "text_buffer.h"
#include "view.h"
#include "woodland.h"

/* SSD 颜色（对齐 labwc 默认主题 theme_builtin()，premultiplied RGBA float） */
static const float COLOR_TITLEBAR_ACTIVE[4]   = {0.882f, 0.871f, 0.859f, 1.0f}; /* #e1dedb */
static const float COLOR_TITLEBAR_INACTIVE[4] = {0.965f, 0.961f, 0.957f, 1.0f}; /* #f6f5f4 */
static const float COLOR_BORDER[4]            = {0.667f, 0.667f, 0.667f, 1.0f}; /* #aaaaaa */
static const float COLOR_TITLE_TEXT[4]        = {0.0f, 0.0f, 0.0f, 1.0f};       /* #000000 标题文字 */

enum { BORDER_TOP = 0, BORDER_BOTTOM, BORDER_LEFT, BORDER_RIGHT, BORDER_COUNT };
enum { BUTTON_CLOSE = 0, BUTTON_MAXIMIZE, BUTTON_MINIMIZE, BUTTON_COUNT };

struct woodland_ssd {
    struct woodland_view *view;
    struct wlr_scene_tree *tree;
    bool active;
    struct wlr_scene_rect *titlebar;
    struct wlr_scene_rect *border[BORDER_COUNT];
    struct wlr_scene_buffer *button[BUTTON_COUNT];
    struct woodland_text_buffer *title_text;
    int width;   /* 内容宽（缓存的几何） */
    int height;  /* 内容高 */
};

/* 内容尺寸来源：xdg toplevel base geometry */
static void ssd_content_size(struct woodland_view *view, int *w, int *h) {
    struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
    *w = xdg->geometry.width;
    *h = xdg->geometry.height;
}

void woodland_ssd_get_margins(struct woodland_view *view,
                              struct woodland_ssd_margins *margins) {
    margins->left = margins->top = margins->right = margins->bottom = 0;
    /* CSD（客户端自绘）与 fullscreen 都没有服务端装饰占用空间 */
    if (view->deco_csd || view->fullscreen) {
        return;
    }
    if (view->maximized) {
        /* 最大化隐藏边框、保留标题栏（见 ssd_apply_state） */
        margins->top = WOODLAND_SSD_TITLEBAR_H;
        return;
    }
    margins->left = margins->right = margins->bottom = WOODLAND_SSD_BORDER_W;
    margins->top = WOODLAND_SSD_TITLEBAR_H + WOODLAND_SSD_BORDER_W;
}

static struct wlr_scene_rect *make_rect(struct wlr_scene_tree *parent,
        int x, int y, int w, int h, const float color[4]) {
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
static void ssd_border_geometry(int w, int h, int index,
        int *out_x, int *out_y, int *out_w, int *out_h) {
    int th = WOODLAND_SSD_TITLEBAR_H;
    int b = WOODLAND_SSD_BORDER_W;
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

/* 重新摆放所有组件（标题栏/边框/按钮） */
static void ssd_layout(struct woodland_ssd *ssd) {
    int w = ssd->width;
    int h = ssd->height;
    int th = WOODLAND_SSD_TITLEBAR_H;

    /* 标题栏：内容顶部上方 */
    wlr_scene_node_set_position(&ssd->titlebar->node, 0, -th);
    wlr_scene_rect_set_size(ssd->titlebar, w, th);

    /* 边框：包住内容 + 标题栏，构成完整外框（负坐标） */
    for (int i = 0; i < BORDER_COUNT; i++) {
        int bx, by, rw, rh;
        ssd_border_geometry(w, h, i, &bx, &by, &rw, &rh);
        wlr_scene_node_set_position(&ssd->border[i]->node, bx, by);
        wlr_scene_rect_set_size(ssd->border[i], rw, rh);
    }

    /* 按钮：labwc 默认 titleLayout "icon:iconify,max,close" 的右段，
     * 自右向左 close/max/iconify；左段是窗口图标（woodland 无窗口图标，留空） */
    int bh = WOODLAND_SSD_BUTTON_H;
    int bwn = WOODLAND_SSD_BUTTON_W;
    int gap = WOODLAND_SSD_BUTTON_GAP;
    int by = -th + (th - bh) / 2;
    int x = w - gap - bwn;
    wlr_scene_node_set_position(&ssd->button[BUTTON_CLOSE]->node, x, by);
    x -= gap + bwn;
    wlr_scene_node_set_position(&ssd->button[BUTTON_MAXIMIZE]->node, x, by);
    x -= gap + bwn;
    wlr_scene_node_set_position(&ssd->button[BUTTON_MINIMIZE]->node, x, by);

    /* 标题文字：在左侧留白与右侧按钮区之间居中（labwc window.label.text.justify: center） */
    if (ssd->title_text) {
        int title_w = woodland_text_buffer_get_width(ssd->title_text);
        int right_btns = BUTTON_COUNT * bwn + (BUTTON_COUNT - 1) * gap;
        int avail = w - right_btns;
        int tx = (avail - title_w) / 2;
        if (tx < 0) {
            tx = 0;
        }
        woodland_text_buffer_set_position(ssd->title_text, tx, -th);
    }
}

void woodland_ssd_create(struct woodland_view *view) {
    if (view->ssd) {
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

    /* 位置/尺寸都交给 ssd_layout() 统一设置，这里只建节点（1x1 占位） */
    ssd->titlebar = make_rect(ssd->tree, 0, 0, 1, 1, COLOR_TITLEBAR_INACTIVE);
    for (int i = 0; i < BORDER_COUNT; i++) {
        ssd->border[i] = make_rect(ssd->tree, 0, 0, 1, 1, COLOR_BORDER);
    }

    /* 按钮符号（—/□/×，黑色，对齐 labwc 默认黑色按钮图标） */
    ssd->button[BUTTON_CLOSE] = woodland_button_icon_create(
        ssd->tree, WOODLAND_SSD_ICON_CLOSE, WOODLAND_SSD_BUTTON_W);
    ssd->button[BUTTON_MAXIMIZE] = woodland_button_icon_create(
        ssd->tree, WOODLAND_SSD_ICON_MAXIMIZE, WOODLAND_SSD_BUTTON_W);
    ssd->button[BUTTON_MINIMIZE] = woodland_button_icon_create(
        ssd->tree, WOODLAND_SSD_ICON_ICONIFY, WOODLAND_SSD_BUTTON_W);

    ssd->title_text = woodland_text_buffer_create(ssd->tree);

    if (!ssd->titlebar || !ssd->border[BORDER_TOP] || !ssd->border[BORDER_BOTTOM]
        || !ssd->border[BORDER_LEFT] || !ssd->border[BORDER_RIGHT]
        || !ssd->button[BUTTON_CLOSE] || !ssd->button[BUTTON_MAXIMIZE]
        || !ssd->button[BUTTON_MINIMIZE]) {
        wlr_log(WLR_ERROR, "failed to create ssd scene rects");
        wlr_scene_node_destroy(&ssd->tree->node);
        free(ssd);
        return;
    }

    ssd_layout(ssd);
    view->ssd = ssd;
    woodland_ssd_update(view);
}

void woodland_ssd_destroy(struct woodland_view *view) {
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return;
    }
    view->ssd = NULL;
    /* title_text 的 scene buffer 节点随 ssd->tree 级联销毁，这里只 free 封装 */
    if (ssd->title_text) {
        free(ssd->title_text);
    }
    wlr_scene_node_destroy(&ssd->tree->node);
    free(ssd);
}

/* 依据视图状态决定 SSD 可见性：fullscreen 全隐；maximized 只留标题栏 */
static void ssd_apply_state(struct woodland_ssd *ssd) {
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
    wlr_scene_node_set_enabled(&ssd->titlebar->node, true);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        wlr_scene_node_set_enabled(&ssd->button[i]->node, true);
    }
}

void woodland_ssd_update(struct woodland_view *view) {
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd) {
        return;
    }
    int w, h;
    ssd_content_size(view, &w, &h);
    ssd->width = w;
    ssd->height = h;
    /* 先更新标题文本（内部会重算宽度），再统一布局，保证标题按新宽度居中 */
    if (ssd->title_text) {
        woodland_text_buffer_update(ssd->title_text, view->title,
            WOODLAND_SSD_TITLEBAR_H, COLOR_TITLE_TEXT, 1.0f);
    }
    ssd_layout(ssd);
    ssd_apply_state(ssd);
}

void woodland_ssd_set_active(struct woodland_view *view, bool active) {
    struct woodland_ssd *ssd = view->ssd;
    if (!ssd || ssd->active == active) {
        return;
    }
    ssd->active = active;
    const float *tc = active ? COLOR_TITLEBAR_ACTIVE : COLOR_TITLEBAR_INACTIVE;
    wlr_scene_rect_set_color(ssd->titlebar, tc);
    /* 边框颜色 labwc 默认不随激活态变化（均为 #aaaaaa） */
}

/* 相对坐标（相对 view 内容原点）命中测试 */
static enum woodland_ssd_part ssd_hit_relative(struct woodland_ssd *ssd,
        double rx, double ry, uint32_t *edges) {
    int w = ssd->width;
    int h = ssd->height;
    int th = WOODLAND_SSD_TITLEBAR_H;

    /* 标题栏（含按钮） */
    if (rx >= 0 && rx < w && ry >= -th && ry < 0) {
        /* 按钮区（右侧） */
        int bh = WOODLAND_SSD_BUTTON_H;
        int bwn = WOODLAND_SSD_BUTTON_W;
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

    /* 边框：几何与 ssd_layout() 共用 ssd_border_geometry()，保证所见即所点 */
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
        ssd_border_geometry(w, h, i, &bx, &by, &rw, &rh);
        if (rx >= bx && rx < bx + rw && ry >= by && ry < by + rh) {
            *edges = edge_of[i];
            return part_of[i];
        }
    }

    return WOODLAND_SSD_NONE;
}

enum woodland_ssd_part woodland_ssd_at(struct woodland_view *view,
                                       double lx, double ly,
                                       uint32_t *resize_edges) {
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
