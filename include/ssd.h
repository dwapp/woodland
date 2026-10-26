// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SSD_H
#define WOODLAND_SSD_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_view;
struct wlr_scene_tree;

/* SSD 几何常量（硬编码，见设计决策） */
#define WOODLAND_SSD_TITLEBAR_H 24
#define WOODLAND_SSD_BORDER_W   4
#define WOODLAND_SSD_BUTTON_W  18
#define WOODLAND_SSD_BUTTON_H  18
#define WOODLAND_SSD_BUTTON_GAP 4

/** SSD 可命中区域类型（用于光标交互）。 */
enum woodland_ssd_part {
    WOODLAND_SSD_NONE = 0,
    WOODLAND_SSD_TITLEBAR,     /* 拖拽移动 */
    WOODLAND_SSD_BORDER_LEFT,  /* 缩放左边缘 */
    WOODLAND_SSD_BORDER_RIGHT,
    WOODLAND_SSD_BORDER_TOP,
    WOODLAND_SSD_BORDER_BOTTOM,
    WOODLAND_SSD_BUTTON_CLOSE,
    WOODLAND_SSD_BUTTON_MAXIMIZE,
    WOODLAND_SSD_BUTTON_MINIMIZE,
};

struct woodland_ssd;

/** 创建 SSD 并挂到 view->scene_tree（lower_to_bottom）。 */
void woodland_ssd_create(struct woodland_view *view);

/** 销毁 SSD（清空 view->ssd）。 */
void woodland_ssd_destroy(struct woodland_view *view);

/** 重建/更新 SSD 几何（内容尺寸或状态变化后调用）。 */
void woodland_ssd_update(struct woodland_view *view);

/** 切换激活态（颜色两态）。 */
void woodland_ssd_set_active(struct woodland_view *view, bool active);

/** 命中测试：返回 (lx,ly)（布局坐标）命中的 SSD 区域，并输出 resize edges。 */
enum woodland_ssd_part woodland_ssd_at(struct woodland_view *view,
                                       double lx, double ly,
                                       uint32_t *resize_edges);

#endif /* WOODLAND_SSD_H */
