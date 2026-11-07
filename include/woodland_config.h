// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WOODLAND_CONFIG_H
#define WOODLAND_WOODLAND_CONFIG_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

#include "placement.h"

struct woodland_server;
struct woodland_json;

struct woodland_config_general {
    enum woodland_placement_policy placement_policy;
    bool placement_specified;
    uint32_t workspace_count;
    bool focus_follows_mouse;
    int32_t snap_distance;
    bool snap_preview;
    bool allow_tearing;
};

struct woodland_config_decorations {
    bool enabled;
    int32_t titlebar_height;
    int32_t border_width;
    int32_t button_width;
    int32_t resize_grab_width;
    char *font;
    uint32_t active_title_color; /* 0xAARRGGBB */
    uint32_t inactive_title_color;
    uint32_t active_border_color;
    uint32_t inactive_border_color;
    /*
     * 文件里显式给过颜色。默认色与内置浅色调色板同值，光看数值分不出
     * "用户要这个颜色"和"没写所以拿默认"——而 treeland-appearance 的
     * 深色方案要不要盖掉它们，取决于这个区分。
     */
    bool colors_specified;
};

struct woodland_config_keyboard {
    char *layout;
    char *variant;
    char *options;
    char *model;
    int32_t repeat_rate;
    int32_t repeat_delay;
};

struct woodland_config_touchpad {
    bool tap_to_click;
    bool natural_scroll;
    double accel_speed;
    char *accel_profile; /* "flat" | "adaptive" */
};

struct woodland_config_output_rule {
    struct wl_list link; /* struct woodland_config.output_rules */
    char *name;          /* 匹配显示器名称，"*" 代表默认规则 */
    char *mode;          /* "1920x1080@60Hz" 或 "preferred" */
    int32_t width;
    int32_t height;
    int32_t refresh_mhz; /* 毫赫兹，如 60000 = 60Hz */
    double scale;
    int32_t x;
    int32_t y;
    bool auto_position;
    enum wl_output_transform transform;
    bool adaptive_sync;
    bool enabled;
};

struct woodland_config_cursor {
    char *theme;
    int32_t size;
};

/*
 * [effects]：圆角、模糊与窗口阴影。
 *
 * 编译期由 -Deffects 决定 scenefx 是否参与链接，启用时以下字段是运行期配置：
 *
 *   enabled         总开关。false 时圆角按 0 算、不建 blur/shadow 节点、capability 报 0，
 *                   等价于"这台机器不画任何特效"，但协议状态照常记录。
 *   blur            模糊开关（默认 true）。只有在客户端经 ext-background-effect-v1
 *                   主动声明区域时才真糊——开关是"允许不允许"，不是"无条件糊"。
 *   blur_optimized  优化模糊开关（默认 false）。开启后顶层窗口与上层 layer 的模糊只采样
 *                   每输出预渲染好的壁纸/背景层缓存，不再每帧重糊底下移动的窗口；
 *                   bottom 层（如贴在壁纸上的 Dock）无论此开关如何都使用预渲染缓存。
 *   blur_passes / blur_radius / blur_noise / blur_brightness / blur_contrast / blur_saturation
 *                   scenefx 全局模糊参数（默认 3 / 5 / 0.0 / 1.0 / 1.0 / 1.0）。
 *   corner_radius   唯一的圆角半径来源；窗口经 treeland-decoration 覆盖时以窗口为准。
 *   shadow          全局默认窗口阴影开关（默认 false）；窗口经 treeland-decoration
 *                   覆盖时以窗口为准。
 *   shadow_radius / shadow_offset_x / shadow_offset_y / shadow_color
 *                   默认阴影参数（0xAARRGGBB，与 ssd.*_color 同格式）。
 */
struct woodland_config_effects {
    bool enabled;
    bool blur;
    bool blur_optimized;
    int32_t blur_passes;
    int32_t blur_radius;
    double blur_noise;
    double blur_brightness;
    double blur_contrast;
    double blur_saturation;
    int32_t corner_radius;
    bool shadow;
    int32_t shadow_radius;
    int32_t shadow_offset_x;
    int32_t shadow_offset_y;
    uint32_t shadow_color;
};

/*
 * [appearance]：treeland-appearance-manager 的运行期写入。
 *
 * 为什么不把这些值留在协议族里：消费方是 ssd / icon / effect 三个模块，而
 * 项目规定依赖只能单向（协议层可以调它们，它们不能反向依赖协议层）。放在
 * 配置对象里，三者都只是"读 server->config"，协议层负责写——和
 * view->ssd_overrides 同一套思路，只是粒度是用户级而不是窗口级。
 *
 * 语义边界：这些字段**不落盘**。配置文件是初值来源，SIGHUP 重载会把它们
 * 重置回文件里的值（协议改的是"此刻"，不是"以后"）。想持久化就写文件。
 *
 * 只有真有渲染消费方的设置在这里；monospace_font / window_opacity 这类
 * woodland 还没有消费方的，存在协议族里，wctl 标成 recorded_only。
 */
struct woodland_config_appearance {
    bool color_scheme_set; /* 协议设过配色方案；false 时按浅色渲染 */
    bool dark;             /* color_scheme_set && 方案是 dark */
    bool accent_set;       /* 协议设过强调色 */
    uint32_t accent_rgb;   /* 0xRRGGBB，只影响激活窗口的边框 */
    char *icon_theme;      /* NULL = 未设，按 icon.c 的内置主题表找图标 */
};

enum woodland_scale_filter {
    WOODLAND_SCALE_FILTER_SMART = 0,
    WOODLAND_SCALE_FILTER_NEAREST,
    WOODLAND_SCALE_FILTER_BILINEAR,
};

struct woodland_config_xwayland {
    enum woodland_scale_filter scale_filter;
};

struct woodland_config {
    char *loaded_path; /* 实际生效的文件路径，无文件或内置默认时为 NULL */
    struct woodland_config_general general;
    struct woodland_config_decorations ssd;
    struct woodland_config_keyboard keyboard;
    struct woodland_config_touchpad touchpad;
    struct woodland_config_cursor cursor;
    struct woodland_config_effects effects;
    struct woodland_config_appearance appearance;
    struct wl_list output_rules; /* struct woodland_config_output_rule.link */
    struct woodland_config_xwayland xwayland;
};

/**
 * 初始化配置对象为硬编码内置默认值。
 */
void woodland_config_default_init(struct woodland_config *config);

/**
 * 分配并初始化一个默认配置对象。
 */
struct woodland_config *woodland_config_create_default(void);

/**
 * 载入配置（若 path_override 为 NULL，按 XDG 规范查找 ~/.config/woodland/config.toml 等）。
 * 无论配置文件是否存在或损坏，恒返回有效的配置对象（绝不返回 NULL）。
 */
struct woodland_config *woodland_config_load(const char *path_override);

/**
 * 按 XDG 规范查找配置文件实际路径（由调用方 free，找不到返回 NULL）。
 */
char *woodland_config_resolve_path(const char *path_override);

/**
 * 解析内存中的 TOML 字符串内容（单测与外部注入用）。
 */
struct woodland_config *woodland_config_parse_string(const char *content, size_t len,
                                                     const char *source_name);

/**
 * 释放配置对象分配的所有内存。
 */
void woodland_config_destroy(struct woodland_config *config);

/**
 * 触发配置热重载（SIGHUP 或 wctl reload 驱动）。
 * Phase 1 读取新文件并解析，校验成功后执行 Phase 2 应用副作用并记入 trace。
 * 成功返回 true；若解析或校验失败返回 false（保持旧配置完全不变）。
 */
bool woodland_config_reload(struct woodland_server *server);

/**
 * 将新配置与当前配置对比并向各子系统应用变更副作用。
 */
void woodland_config_apply(struct woodland_server *server, struct woodland_config *new_config);

/**
 * 将配置对象导出为 JSON（供 IPC get_config 命令）。
 */
void woodland_config_dump_json(const struct woodland_config *config, struct woodland_json *out);

#endif /* WOODLAND_CONFIG_H */
