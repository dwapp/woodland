// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "output.h"
#include "trace.h"
#include "woodland.h"

static void handle_frame(struct wl_listener *listener, void *data) {
    struct woodland_output *output = wl_container_of(listener, output, frame);
    if (!wlr_scene_output_commit(output->scene_output, NULL)) {
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
    (void)data;
}

static void handle_request_state(struct wl_listener *listener, void *data) {
    struct woodland_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    struct woodland_output *output = wl_container_of(listener, output, destroy);
    struct woodland_server *server = output->server;
    /* 先销毁各 layer surface（触发 node_destroy 链清理），再销毁 layer tree */
    for (int i = 0; i < 4; i++) {
        if (output->layer_tree[i]) {
            struct wlr_scene_node *node, *tmp;
            wl_list_for_each_safe(node, tmp,
                &output->layer_tree[i]->children, link) {
                struct woodland_layer_surface *surface = node->data;
                if (surface) {
                    wlr_layer_surface_v1_destroy(surface->layer_surface);
                }
            }
        }
    }
    wlr_scene_output_destroy(output->scene_output);
    for (int i = 0; i < 4; i++) {
        if (output->layer_tree[i]) {
            wlr_scene_node_destroy(&output->layer_tree[i]->node);
            output->layer_tree[i] = NULL;
        }
    }
    output->wlr_output->data = NULL;
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->link);

    /*
     * 摘链之后才通知：协议层的广播会遍历 server->outputs，这里必须已经
     * 看不到它。用 wlr_output 而不是 output 作参数，因为 output 马上就被
     * 释放了，而在 wlr_output_finish() 期间它仍然有效。
     *
     * 这里走 server 的信号而不是直接调协议层，是为了不让 output 反过来
     * 依赖 protocol（test_output 只链 output_lib）。
     */
    wl_signal_emit_mutable(&server->output_removed, output->wlr_output);
    woodland_trace_add("output_remove", 0, "%s heads=%d",
                       output->wlr_output->name,
                       wl_list_length(&server->outputs));

    free(output);
    (void)data;
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    /* 去重：同一 output 可能被重复通知 */
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output == wlr_output) {
            return;
        }
    }

    /* ---- 1. 建壳：分配 woodland_output 并接通渲染 ---- */
    output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_output");
        return;
    }
    output->server = server;
    output->wlr_output = wlr_output;
    wlr_output->data = output;

    if (!wlr_output_init_render(wlr_output, server->allocator,
                                server->renderer)) {
        wlr_log(WLR_ERROR, "failed to init output render");
        free(output);
        return;
    }

    /* ---- 2. 挂监听：frame / request_state / destroy ---- */
    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    /*
     * ---- 3. 建 scene：scene_output + 4 棵 layer tree ----
     *
     * 顺序在这里很关键：layout_add_auto() 会立刻触发 layout change，而
     * output-management 的广播正是挂在那个信号上。所以凡是广播里要读的
     * 东西（链表成员、scene_output、layer tree）都必须先就位，否则客户端
     * 收到的第一份配置里根本没有这个 head。
     *
     * 这个 bug 只在热插拔时暴露：headless 输出的 preferred_mode 是 NULL，
     * 下面只提交 ENABLED，不会引起 layout 重算，也就没有第二次广播来纠正
     * 第一份——wdisplays / kanshi 从此看不到这个输出。
     */
    struct wlr_scene_output *scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    if (!scene_output) {
        wlr_log(WLR_ERROR, "failed to create scene output");
        goto err_listeners;
    }
    output->scene_output = scene_output;

    /* 创建 4 棵 layer tree，z-order 从下到上：
     * background → bottom → [工作区] → top → overlay。
     * lower_to_bottom 先 [1] 后 [0]，保证 background 在 bottom 之下；
     * raise_to_top 先 [2] 后 [3]，保证 overlay 在 top 之上。 */
    for (int i = 0; i < 4; i++) {
        output->layer_tree[i] = wlr_scene_tree_create(&server->scene->tree);
    }
    wlr_scene_node_lower_to_bottom(&output->layer_tree[1]->node);
    wlr_scene_node_lower_to_bottom(&output->layer_tree[0]->node);
    wlr_scene_node_raise_to_top(&output->layer_tree[2]->node);
    wlr_scene_node_raise_to_top(&output->layer_tree[3]->node);

    /* ---- 4. 入链 + 布局 + 提交 + 广播 ---- */
    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    if (!layout_output) {
        wlr_log(WLR_ERROR, "failed to add output to layout");
        wl_list_remove(&output->link);
        goto err_trees;
    }

    wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
                                       scene_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "failed to commit output state");
    }
    wlr_output_state_finish(&state);

    /* commit 之后还要再广播一次（enabled/mode 这时才是真值），
     * 由 server.c 的 new_output 后置监听器负责：output 层不该反过来
     * 依赖协议层 */
    wlr_output_create_global(wlr_output, server->display);
    woodland_trace_add("output_add", 0, "%s %dx%d heads=%d", wlr_output->name,
                       wlr_output->width, wlr_output->height,
                       wl_list_length(&server->outputs));
    return;

err_trees:
    for (int i = 0; i < 4; i++) {
        if (output->layer_tree[i]) {
            wlr_scene_node_destroy(&output->layer_tree[i]->node);
        }
    }
    wlr_scene_output_destroy(scene_output);
err_listeners:
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

bool woodland_output_init(struct woodland_server *server) {
    wl_list_init(&server->outputs);
    wl_signal_init(&server->output_removed);

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) {
        return false;
    }

    server->scene_layout =
        wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_layout) {
        wlr_output_layout_destroy(server->output_layout);
        server->output_layout = NULL;
        return false;
    }

    /* xdg-output：客户端（grim 等截图工具）靠它拿输出的布局位置与逻辑尺寸，
     * 否则只能按 wl_output 的物理模式猜，区域截图会算错。 */
    if (!wlr_xdg_output_manager_v1_create(server->display, server->output_layout)) {
        wlr_log(WLR_ERROR, "failed to create xdg-output manager");
        return false;
    }

    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    return true;
}

void woodland_output_finish(struct woodland_server *server) {
    /* output_layout 非空 ⇔ init 走完全部步骤 ⇔ new_output 已接线 */
    if (server->output_layout) {
        wl_list_remove(&server->new_output.link);
    }

    /* 释放所有 woodland_output 并摘除其在 wlr_output 上的监听器。
     * scene_output 由 scene 销毁负责（server.c 在 scene 释放前调用本函数，
     * 若这里不摘监听，backend 销毁时会再次触发 handle_destroy 而 UAF） */
    struct woodland_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &server->outputs, link) {
        wl_list_remove(&output->frame.link);
        wl_list_remove(&output->request_state.link);
        wl_list_remove(&output->destroy.link);
        wl_list_remove(&output->link);
        free(output);
    }

    if (server->scene_layout) {
        /* 自动随 output_layout 销毁；销毁 output_layout 前先置空自身引用 */
        server->scene_layout = NULL;
    }
    if (server->output_layout) {
        wlr_output_layout_destroy(server->output_layout);
        server->output_layout = NULL;
    }
}
