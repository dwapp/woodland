// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TEXT_BUFFER_H
#define WOODLAND_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
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
 * @param font_desc pango 字体描述串（"Sans 10"）；NULL/空串用内置默认。
 *                  逐次传入而不是创建时定型：treeland-appearance-manager
 *                  可以在运行期改字体，改了就要重排。
 */
void woodland_text_buffer_update(struct woodland_text_buffer *tb, const char *text, int max_width,
                                 int height, const float color[4], float scale,
                                 const char *font_desc);

/**
 * 拆分 pango 字体描述串成 family + 点数（10.5pt → 10.5）。
 *
 * 协议里 font 与 font_size 是两个独立请求，而配置只有一个描述串；改其中一个
 * 要把另一个原样保留，所以要先拆再拼。解析交给 pango 自己做——按空格切
 * 会在 "DejaVu Sans Bold 10" 这类描述上拆错。
 */
bool woodland_font_split(const char *desc, char *family, size_t family_len, double *size_pt);

/**
 * 在已有描述串的基础上改 family 和/或点数，输出新的描述串。
 *
 * 为什么不是"拆成 family+size 再拼"：pango 把 "DejaVu Sans Bold 10.5" 里的
 * Bold 解析成**字重**而不是族名的一部分（family 是 "DejaVu Sans"）。自己拼
 * 会把字重丢掉——协议只传 family 与 size，不该顺手抹掉用户配置里的字重。
 * 交给 pango 的 set_family/set_size + to_string 才保得住。
 *
 * family 为 NULL/空串表示不改族名；size_pt <= 0 表示不改字号。desc 为 NULL
 * 时从内置默认字体起步。缓冲不足返回 false，不会写出半截字符串。
 */
bool woodland_font_with(const char *desc, const char *family, double size_pt, char *buf,
                        size_t len);

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
                                                     enum woodland_ssd_button_icon icon, int size,
                                                     float scale);

/**
 * 绘制按钮图标并返回 wlr_buffer（inverted 为 true 时使用白色线条，用于关闭悬停/按下态）。
 */
struct wlr_buffer *woodland_button_icon_buffer(enum woodland_ssd_button_icon icon, int size,
                                               bool inverted, float scale);

#endif /* WOODLAND_TEXT_BUFFER_H */
