// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "output.h"
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

    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    if (!layout_output) {
        wlr_log(WLR_ERROR, "failed to add output to layout");
        wl_list_remove(&output->frame.link);
        wl_list_remove(&output->request_state.link);
        wl_list_remove(&output->destroy.link);
        free(output);
        return;
    }

    struct wlr_scene_output *scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    if (!scene_output) {
        wlr_log(WLR_ERROR, "failed to create scene output");
        wl_list_remove(&output->frame.link);
        wl_list_remove(&output->request_state.link);
        wl_list_remove(&output->destroy.link);
        wlr_output_layout_remove(server->output_layout, wlr_output);
        free(output);
        return;
    }
    output->scene_output = scene_output;

    wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
                                       scene_output);

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

    wl_list_insert(&server->outputs, &output->link);

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

    wlr_output_create_global(wlr_output, server->display);
}

bool woodland_output_init(struct woodland_server *server) {
    wl_list_init(&server->outputs);

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
