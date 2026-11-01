// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SSD_H
#define WOODLAND_SSD_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_view;
struct wlr_scene_tree;

/* SSD 几何常量（对齐 labwc 默认主题，见 theme_builtin()） */
#define WOODLAND_SSD_TITLEBAR_H 26
#define WOODLAND_SSD_BORDER_W 1
#define WOODLAND_SSD_BUTTON_W 26
#define WOODLAND_SSD_BUTTON_H 26
#define WOODLAND_SSD_BUTTON_GAP 0

/** SSD 可命中区域类型（用于光标交互）。 */
enum woodland_ssd_part {
    WOODLAND_SSD_NONE = 0,
    WOODLAND_SSD_TITLEBAR,    /* 拖拽移动 */
    WOODLAND_SSD_BORDER_LEFT, /* 缩放左边缘 */
    WOODLAND_SSD_BORDER_RIGHT,
    WOODLAND_SSD_BORDER_TOP,
    WOODLAND_SSD_BORDER_BOTTOM,
    WOODLAND_SSD_BUTTON_CLOSE,
    WOODLAND_SSD_BUTTON_MAXIMIZE,
    WOODLAND_SSD_BUTTON_MINIMIZE,
};

struct woodland_ssd;

/* SSD 占用的外边距（内容矩形之外的部分），语义同 labwc ssd_thickness() */
struct woodland_ssd_margins {
    int left;
    int top;
    int right;
    int bottom;
};

/** 创建 SSD 并挂到 view->scene_tree（lower_to_bottom）。 */
void woodland_ssd_create(struct woodland_view *view);

/**
 * 当前状态下 SSD 占用的外边距。
 *
 * 窗口布局（放置/最大化）必须把它算进去：内容原点 = 外框原点 + 边距。
 * 不依赖 SSD 是否已创建（map 之前就要能算），只看 deco_csd / maximized / fullscreen。
 */
void woodland_ssd_get_margins(struct woodland_view *view, struct woodland_ssd_margins *margins);

/** 销毁 SSD（清空 view->ssd）。 */
void woodland_ssd_destroy(struct woodland_view *view);

/** 重建/更新 SSD 几何（内容尺寸或状态变化后调用）。 */
void woodland_ssd_update(struct woodland_view *view);

/** 切换激活态（颜色两态）。 */
void woodland_ssd_set_active(struct woodland_view *view, bool active);

/**
 * 设置按钮悬停态（part 为 WOODLAND_SSD_BUTTON_* 或 WOODLAND_SSD_NONE）。
 *
 * 非按钮区域（标题栏/边框）一律传 NONE：悬停高亮只作用于按钮。
 */
void woodland_ssd_set_hover(struct woodland_view *view, enum woodland_ssd_part part);

/**
 * 设置按钮按下态（part 为 WOODLAND_SSD_BUTTON_* 或 WOODLAND_SSD_NONE）。
 *
 * 按下态比悬停态更深一层；释放后由调用方清回 NONE。
 */
void woodland_ssd_set_pressed(struct woodland_view *view, enum woodland_ssd_part part);

/** 命中测试：返回 (lx,ly)（布局坐标）命中的 SSD 区域，并输出 resize edges。 */
enum woodland_ssd_part woodland_ssd_at(struct woodland_view *view, double lx, double ly,
                                       uint32_t *resize_edges);

/**
 * SSD 是否处于激活（标题栏激活色）状态；没有 SSD 时返回 false。
 *
 * 单独暴露是因为激活态分散在三处（客户端 activated、SSD 颜色、
 * foreign-toplevel 位），不变量检查要逐处核对，不能靠看像素。
 */
bool woodland_ssd_is_active(const struct woodland_view *view);

/**
 * SSD 内部缓存的内容尺寸；没有 SSD 时返回 false。
 *
 * 与当前内容尺寸不一致说明 ssd_update() 少调了一次（窗口拉宽后按钮
 * 还停在旧位置就是这么来的）。
 */
bool woodland_ssd_content_size(const struct woodland_view *view, int *width, int *height);

/** 释放 SSD 全局缓存与资源。 */
void woodland_ssd_finish(void);
#endif /* WOODLAND_SSD_H */
