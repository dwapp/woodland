// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_OUTPUT_H
#define WOODLAND_OUTPUT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include "scene.h"

struct woodland_server;

struct woodland_output {
    struct wl_list link;               // woodland_server.outputs
    struct woodland_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_tree *layer_tree[4]; // 下标 = zwlr_layer_shell_v1_layer 枚举
    struct wlr_box usable_area;        // 扣除 exclusive zone 后的可用区域
    bool usable_area_valid;            // usable_area 是否已被 arrange 初始化
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

bool woodland_output_init(struct woodland_server *server);
void woodland_output_finish(struct woodland_server *server);

/** 判定给定输出当前是否允许异步页面翻转（撕裂） */
bool woodland_output_get_tearing_allowance(struct woodland_output *output);

/** 渲染并提交输出（包含撕裂判定与回退） */
bool woodland_output_commit(struct woodland_output *output);

#endif
