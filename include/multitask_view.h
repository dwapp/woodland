// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_MULTITASK_VIEW_H
#define WOODLAND_MULTITASK_VIEW_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

struct woodland_server;
struct woodland_view;
struct woodland_workspace;
struct woodland_output;
struct woodland_json;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
#if WOODLAND_HAVE_EFFECTS
struct wlr_scene_shadow;
#endif

#define WOODLAND_MULTITASK_MAX_WORKSPACES 12
#define WOODLAND_MULTITASK_MAX_CARDS 64

struct multitask_workspace_item {
    struct woodland_workspace *workspace;
    struct wlr_box box; /* 相对 multitask_view->tree（输出左上角为原点）的逻辑矩形 */
    int window_count;
    struct wlr_scene_buffer *scene_buffer;
};

struct multitask_card_item {
    struct woodland_view *view;
    struct wlr_box box;       /* 卡片外框（相对 multitask_view->tree） */
    struct wlr_box thumb_box; /* 缩略图区域（相对 card->tree） */
    struct wlr_box close_box; /* 关闭按钮热区（相对 multitask_view->tree） */
    struct wlr_scene_tree *tree;
    struct wlr_scene_buffer *frame_buffer;
    struct wlr_scene_buffer *thumb_buffer;
    struct wlr_scene_buffer *icon_buffer;
#if WOODLAND_HAVE_EFFECTS
    struct wlr_scene_shadow *shadow;
#endif
};

struct woodland_multitask_view {
    struct woodland_server *server;
    bool active;
    struct woodland_output *output;
    struct wlr_box output_box;
    float scale;

    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *backdrop;
    struct wlr_scene_tree *ws_bar_tree;
    struct wlr_scene_tree *cards_tree;
    struct wlr_scene_buffer *empty_hint_buffer;

    struct woodland_workspace *selected_workspace;
    struct multitask_workspace_item workspaces[WOODLAND_MULTITASK_MAX_WORKSPACES];
    int workspace_count;
    int hovered_workspace; /* -1 表示无 */

    struct multitask_card_item cards[WOODLAND_MULTITASK_MAX_CARDS];
    int card_count;
    int grid_cols;
    int grid_rows;
    int selected_index; /* 当前选中的卡片索引，无卡片时为 -1 */
    int hovered_card;   /* 鼠标悬停的卡片索引，-1 表示无 */
    int hovered_close;  /* 鼠标悬停在关闭按钮上的卡片索引，-1 表示无 */
};

/**
 * 纯几何函数：在给定矩形区域 area 内为 count 张卡片计算网格行列数与每张卡片的包围盒。
 * 供渲染实现与单元测试共用。
 */
void woodland_multitask_compute_grid(const struct wlr_box *area, int count,
                                     int *out_cols, int *out_rows,
                                     struct wlr_box *out_boxes);

/** 初始化多任务视图（由 shell_init 调用）。 */
bool woodland_multitask_view_init(struct woodland_server *server);

/** 释放多任务视图（由 shell_finish 调用）。 */
void woodland_multitask_view_finish(struct woodland_server *server);

/** 多任务视图当前是否处于打开状态。 */
bool woodland_multitask_view_is_active(const struct woodland_server *server);

/** 打开多任务视图（已打开则刷新）。 */
void woodland_multitask_view_open(struct woodland_server *server);

/**
 * 关闭多任务视图。
 * activate_selected=true 时激活并聚焦当前选中的窗口（若最小化则先恢复并切换到其所属工作区）。
 */
void woodland_multitask_view_close(struct woodland_server *server, bool activate_selected);

/** 切换多任务视图开关状态。 */
void woodland_multitask_view_toggle(struct woodland_server *server);

/** 视图 unmap/destroy 时通知多任务视图刷新并清理引用。 */
void woodland_multitask_view_handle_view_unmap(struct woodland_view *view);

/** 处理指针按下/释放事件。返回 true 表示事件被多任务视图消费。 */
bool woodland_multitask_view_handle_button(struct woodland_server *server, uint32_t button,
                                           uint32_t state, double lx, double ly);

/** 处理指针移动事件，更新悬停与选中态。 */
void woodland_multitask_view_handle_motion(struct woodland_server *server, double lx, double ly);

/** 处理键盘按键事件（方向键/Tab/Enter/Escape/Delete/数字键切工作区）。返回 true 表示消费。 */
bool woodland_multitask_view_handle_key(struct woodland_server *server, xkb_keysym_t sym,
                                        uint32_t modifiers);

/** 导出多任务视图状态到 JSON（供 wctl get_tree 使用）。 */
void woodland_multitask_view_status(struct woodland_server *server, struct woodland_json *out);

#endif /* WOODLAND_MULTITASK_VIEW_H */
