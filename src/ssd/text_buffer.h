// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TEXT_BUFFER_H
#define WOODLAND_TEXT_BUFFER_H

#include <stdbool.h>
#include <wlr/types/wlr_buffer.h>

struct wlr_scene_tree;

/** 用 pango + cairo 渲染单行文本到 wlr_buffer 的封装。 */
struct woodland_text_buffer;

/**
 * 创建文本 buffer，挂到 parent 树（内部创建 wlr_scene_buffer 节点）。
 * 初始无内容；用 woodland_text_buffer_update 设置文本。
 */
struct woodland_text_buffer *woodland_text_buffer_create(
    struct wlr_scene_tree *parent);

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
void woodland_text_buffer_update(struct woodland_text_buffer *tb,
                                 const char *text, int height,
                                 const float color[4], float scale);

/** 移动文本 buffer 节点。 */
void woodland_text_buffer_set_position(struct woodland_text_buffer *tb,
                                       int x, int y);

#endif /* WOODLAND_TEXT_BUFFER_H */
