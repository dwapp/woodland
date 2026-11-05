// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TEXT_BUFFER_H
#define WOODLAND_TEXT_BUFFER_H

#include <stdbool.h>
#include <wlr/types/wlr_buffer.h>

struct wlr_scene_tree;
struct wlr_scene_buffer;

/** 按钮符号类型（对齐 labwc titleLayout: iconify/max/close）。 */
enum woodland_ssd_button_icon {
    WOODLAND_SSD_ICON_ICONIFY,
    WOODLAND_SSD_ICON_MAXIMIZE,
    WOODLAND_SSD_ICON_RESTORE,
    WOODLAND_SSD_ICON_CLOSE,
};

/** 用 pango + cairo 渲染单行文本到 wlr_buffer 的封装。 */
struct woodland_text_buffer;

/**
 * 创建文本 buffer，挂到 parent 树（内部创建 wlr_scene_buffer 节点）。
 * 初始无内容；用 woodland_text_buffer_update 设置文本。
 */
struct woodland_text_buffer *woodland_text_buffer_create(struct wlr_scene_tree *parent);

/** 销毁文本 buffer（含 scene buffer 节点与 backing wlr_buffer）。 */
void woodland_text_buffer_destroy(struct woodland_text_buffer *tb);

/**
 * 更新文本内容并重渲染。
 * @param tb       文本 buffer
 * @param text     文本（NULL 或空串时清空内容）
 * @param height   目标高度（用于垂直居中；标题栏高）
 * @param color    前景色 RGBA（premultiplied，0~1）
 * @param scale    输出缩放（buffer 像素 = 逻辑 * scale）
 */
void woodland_text_buffer_update(struct woodland_text_buffer *tb, const char *text, int max_width,
                                 int height, const float color[4], float scale);

/** 移动文本 buffer 节点。 */
void woodland_text_buffer_set_position(struct woodland_text_buffer *tb, int x, int y);

/**
 * 显示/隐藏文本节点（不销毁内容）。
 *
 * 标题栏被 treeland-decoration 隐藏时用：节点还在、只是不画，
 * 重新显示时不用重排版。
 */
void woodland_text_buffer_set_enabled(struct woodland_text_buffer *tb, bool enabled);

/** 返回文本逻辑宽度（空文本返回 0）。 */
int woodland_text_buffer_get_width(const struct woodland_text_buffer *tb);

/**
 * 用 cairo 绘制一个黑色按钮符号（—/□/×）并作为 wlr_scene_buffer 挂到 parent。
 * @return 新创建的 scene_buffer 节点（归调用者销毁）；失败返回 NULL。
 */
struct wlr_scene_buffer *woodland_button_icon_create(struct wlr_scene_tree *parent,
                                                     enum woodland_ssd_button_icon icon, int size);

/**
 * 绘制按钮图标并返回 wlr_buffer（inverted 为 true 时使用白色线条，用于关闭悬停/按下态）。
 */
struct wlr_buffer *woodland_button_icon_buffer(enum woodland_ssd_button_icon icon, int size,
                                               bool inverted);

#endif /* WOODLAND_TEXT_BUFFER_H */
