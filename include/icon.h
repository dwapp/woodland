// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SSD_ICON_H
#define WOODLAND_SSD_ICON_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_buffer.h>

struct woodland_view;

/**
 * 为指定视图加载/创建标题栏应用图标 buffer（返回的 buffer 引用计数为 1，调用方需 drop）。
 * @param view 目标视图（从中读取 app_id 与 title）
 * @param size 图标像素尺寸（宽高相同，例如 16 或 24）
 * @param scale 输出设备缩放比例
 */
struct wlr_buffer *woodland_ssd_get_icon_buffer(struct woodland_view *view, int size, float scale);

/**
 * 根据应用 ID 或名称检索图标绝对路径（返回 malloc 字符串，未找到返回 NULL）。
 */
char *woodland_icon_find_path(const char *app_id);

/**
 * 从 .desktop 文件中解析 [Desktop Entry] 下的 Icon= 字段（未找到返回 NULL）。
 */
char *woodland_icon_from_desktop_file(const char *desktop_path);

/**
 * 释放图标缓存与已分配资源。
 */
void woodland_icon_cache_finish(void);

#endif /* WOODLAND_SSD_ICON_H */
