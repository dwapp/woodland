// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_XWAYLAND_SCALE_H
#define WOODLAND_XWAYLAND_SCALE_H

/*
 * Xwayland 原生分辨率（native resolution）坐标映射。
 *
 * X11 客户端没有"每输出缩放"的概念。默认情况下 Xwayland 按输出的**逻辑**
 * 尺寸（物理 ÷ scale）建 X 屏幕，再由合成器把窗口放大 scale 倍：1.25 这类
 * 分数缩放下画面发糊，全屏游戏也只能选到比真实分辨率低的模式。
 *
 * 开启 [xwayland] native_resolution 后，Xwayland 经专用 xdg-output global
 * 看到每个输出的**物理**尺寸，窗口按 1 像素 = 1 物理像素渲染，合成器再把
 * 它缩回逻辑尺寸显示。此后 X 坐标 / 尺寸与合成器布局坐标 / 尺寸之间差一个
 * 缩放系数，所有交给 X11 或从 X11 收到的几何都要过这里的换算。
 *
 * 本文件只含纯几何换算（不依赖 Xwayland 协议），shell 与 xwayland 两侧共用。
 * 关闭时 map->enabled == false，所有换算恒等。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct wlr_output;
struct wlr_output_layout;
struct woodland_view;

#define WOODLAND_XWL_MAX_OUTPUTS 16

/** 一个输出的那一块布局，如何映射到 X 坐标。 */
struct woodland_xwl_region {
    struct wlr_box layout; // 布局（逻辑）坐标下的输出矩形
    struct wlr_box x;      // X 坐标下的输出矩形（原生分辨率时为物理尺寸）
    double scale;          // X 像素 / 布局单位：1，或该输出的缩放
};

struct woodland_xwl_map {
    bool enabled;  // native_resolution 是否启用；false 时换算恒等
    double scale;  // 所有输出里最大的缩放；输出间距按它放大，保证不重叠
    int count;
    const struct wlr_output *outputs[WOODLAND_XWL_MAX_OUTPUTS];
    struct woodland_xwl_region regions[WOODLAND_XWL_MAX_OUTPUTS];
    struct woodland_xwl_region fallback; // 没有任何输出时：平移到 X 原点
};

/**
 * 按输出布局重算 X 屏幕。X 根窗口只覆盖非负坐标，所以布局左上角当 X 原点。
 * 返回布局到 X 坐标的映射是否变了。
 */
bool woodland_xwl_map_update(struct woodland_xwl_map *map, struct wlr_output_layout *layout);

/** output 对应的区域；output 为空或不在布局里时取离 (lx,ly) 最近的输出。 */
void woodland_xwl_map_region(const struct woodland_xwl_map *map, const struct wlr_output *output,
                             int lx, int ly, struct woodland_xwl_region *out);

/** X 点 (x,y) 所在、或离它最近的输出区域。 */
void woodland_xwl_map_region_at_x(const struct woodland_xwl_map *map, int x, int y,
                                  struct woodland_xwl_region *out);

/** 某输出在 X 坐标下的矩形；不在布局里时为空矩形。 */
struct wlr_box woodland_xwl_map_x_box(const struct woodland_xwl_map *map,
                                      const struct wlr_output *output);

int woodland_xwl_to_x(const struct woodland_xwl_region *r, int layout_x);
int woodland_xwl_to_y(const struct woodland_xwl_region *r, int layout_y);
int woodland_xwl_to_layout_x(const struct woodland_xwl_region *r, int x);
int woodland_xwl_to_layout_y(const struct woodland_xwl_region *r, int y);
/* 长度换算：恰好等于整块输出时精确映射到另一侧的整块，不受取整影响 */
int woodland_xwl_to_x_width(const struct woodland_xwl_region *r, int width);
int woodland_xwl_to_x_height(const struct woodland_xwl_region *r, int height);
int woodland_xwl_to_layout_width(const struct woodland_xwl_region *r, int width);
int woodland_xwl_to_layout_height(const struct woodland_xwl_region *r, int height);

/**
 * view 当前所在输出的区域。native_resolution 关闭、或不是 X11 视图时返回
 * false（调用方此时不做任何换算）。
 */
bool woodland_xwl_view_region(struct woodland_view *view, struct woodland_xwl_region *out);

/** XSETTINGS 缩放与 DPI 参数（供 GTK / Chromium / Electron / Steam 等读取）。 */
struct woodland_xsettings_values {
    int32_t window_scaling_factor;
    int32_t unscaled_dpi;
    int32_t xft_dpi;
    int32_t cursor_size;
    char cursor_theme[128];
};

/** 计算指定缩放、光标主题与基础尺寸对应的 XSETTINGS 参数。 */
void woodland_xsettings_for(double scale, const char *cursor_theme, int cursor_size,
                            struct woodland_xsettings_values *out);

/** 将 XSETTINGS 参数按 X11 _XSETTINGS_SETTINGS 二进制格式编码；返回字节数，失败返回 0。 */
size_t woodland_xsettings_encode(uint32_t serial,
                                 const struct woodland_xsettings_values *values,
                                 uint8_t *buf, size_t cap);

#endif /* WOODLAND_XWAYLAND_SCALE_H */
