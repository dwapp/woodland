// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_OUTPUT_CONFIG_H
#define WOODLAND_OUTPUT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

struct woodland_server;
struct woodland_output;

struct woodland_saved_output {
    char *name;
    bool enabled;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t refresh; /* 毫赫兹 (mHz)，例如 60000 代表 60Hz */
    double scale;
    enum wl_output_transform transform;
    bool adaptive_sync;
};

struct woodland_saved_outputs {
    struct woodland_saved_output *items;
    size_t count;
};

/** 从 $XDG_CONFIG_HOME/woodland/outputs.json 载入全部已保存配置 */
bool woodland_saved_outputs_load(struct woodland_saved_outputs *list);

/** 释放已载入的配置列表内存 */
void woodland_saved_outputs_finish(struct woodland_saved_outputs *list);

/** 在配置列表中按输出名称查找记录 */
const struct woodland_saved_output *
woodland_saved_outputs_find(const struct woodland_saved_outputs *list, const char *name);

/**
 * 将 server 中当前所有活动输出的状态持久化到 $XDG_CONFIG_HOME/woodland/outputs.json。
 * 采取合并更新策略：当前在线的输出更新其最新参数，离线输出的历史配置予以保留。
 */
void woodland_output_config_save(struct woodland_server *server);

/**
 * 新输出接入时，尝试根据已保存的 outputs.json 配置其模式、缩放、变换与布局位置。
 * 返回 true 表示命中了持久化配置并已应用；返回 false 表示未命中。
 */
bool woodland_output_config_restore(struct woodland_output *output,
                                    struct wlr_output_state *state,
                                    int32_t *out_x, int32_t *out_y);

#endif
