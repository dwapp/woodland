// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_OUTPUT_H
#define WOODLAND_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include "scene.h"

struct woodland_server;
struct wlr_scene_optimized_blur;

struct woodland_output {
    struct wl_list link;               // woodland_server.outputs
    struct woodland_server *server;
    uint64_t id_bit;                   // 输出唯一位标识（1ULL << n，用于视图输出缓存闸门）
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_tree *layer_tree[4]; // 下标 = zwlr_layer_shell_v1_layer 枚举
    struct wlr_scene_optimized_blur *optimized_blur; // background 层之上的预模糊缓存节点
    struct wlr_box usable_area;        // 扣除 exclusive zone 后的可用区域
    bool usable_area_valid;            // usable_area 是否已被 arrange 初始化
    /*
     * 上一次 commit 时的缩放。
     *
     * `wlr_output->scale` 有三条改动路径（backend 的 request_state、
     * output-management 的 apply、配置重载），只有第一条会发 request_state，
     * 但三条都会以一次成功的 commit 收尾，所以缩放相关的重刷挂在 commit 上、
     * 用这个字段当闸门——commit 每帧都发，不比对就等于每帧重排全部 SSD。
     */
    float last_scale;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener commit;
    struct wl_listener destroy;
};

bool woodland_output_init(struct woodland_server *server);
void woodland_output_finish(struct woodland_server *server);

/**
 * 把配置里 [[output]] 的 scale 应用到已存在的输出。
 *
 * 只消费 scale：position / enabled 仍不消费（既有缺口，不是这里漏了）。
 * 只在配置重载之后调用——`woodland_config_apply()` 也会被
 * treeland-appearance 的 set_* 触发，在那里重放配置值会把用户刚用
 * wlr-randr 调的缩放打回去。缩放相关的刷新由 output 的 commit 监听器
 * 统一负责（`output.c` 里的 woodland_output_refresh_all()），这里只管提交。
 */
void woodland_output_apply_rules(struct woodland_server *server);

/** 判定给定输出当前是否允许异步页面翻转（撕裂） */
bool woodland_output_get_tearing_allowance(struct woodland_output *output);

/** 渲染并提交输出（包含撕裂判定与回退） */
bool woodland_output_commit(struct woodland_output *output);

#endif
