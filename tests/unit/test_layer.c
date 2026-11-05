// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include "scene.h"
#include "layer.h"
#include "output.h"
#include "woodland.h"

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend);
    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator);
    server.scene = wlr_scene_create();
    assert(server.scene);

    assert(woodland_output_init(&server));
    assert(woodland_layer_init(&server));

    /* layer_shell 已创建 */
    assert(server.layer_shell != NULL);

    /* 添加 800x600 headless output，触发 new_output */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 找到该输出，断言 4 棵 layer_tree 非空 */
    struct woodland_output *wo =
        wl_container_of(server.outputs.next, wo, link);
    for (int i = 0; i < 4; i++) {
        assert(wo->layer_tree[i] != NULL);
    }

    /* z-order：background(layer_tree[0]) 是场景最底，overlay(layer_tree[3]) 最顶 */
    struct wl_list *children = &server.scene->tree.children;
    struct wlr_scene_node *first_node =
        wl_container_of(children->next, first_node, link);
    struct wlr_scene_node *last_node =
        wl_container_of(children->prev, last_node, link);
    assert(first_node == &wo->layer_tree[0]->node);
    assert(last_node == &wo->layer_tree[3]->node);

    woodland_layer_finish(&server);
    woodland_output_finish(&server);

    /* 逆序释放 wlr 资源 */
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_allocator_destroy(server.allocator);
    server.allocator = NULL;
    wlr_renderer_destroy(server.renderer);
    server.renderer = NULL;
    wlr_backend_destroy(server.backend);
    server.backend = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;
    return 0;
}
