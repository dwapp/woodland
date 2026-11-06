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

    /* 测试全屏时的 TOP layer 动态可见性（对齐 labwc） */
    uint32_t top = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    assert(wo->layer_tree[top]->node.enabled == true);

    /* 初始化 views 链表 */
    wl_list_init(&server.views);

    /* 创建测试 view 并加入 views 链表 */
    struct wlr_xdg_surface fs_base = {0};
    fs_base.geometry = (struct wlr_box){.width = 800, .height = 600};
    struct wlr_xdg_toplevel fs_toplevel = {.base = &fs_base};
    struct woodland_view fs_view = {0};
    fs_view.server = &server;
    fs_view.xdg_toplevel = &fs_toplevel;
    fs_view.mapped = true;
    fs_view.fullscreen = true;
    fs_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(&server.views, &fs_view.link);

    /* 全屏窗口可见且未被遮挡：TOP layer 应被隐藏 */
    woodland_layer_update_top_layer_visibility(&server);
    assert(wo->layer_tree[top]->node.enabled == false);

    /* 窗口退出全屏：TOP layer 应恢复可见 */
    fs_view.fullscreen = false;
    woodland_layer_update_top_layer_visibility(&server);
    assert(wo->layer_tree[top]->node.enabled == true);

    /* 窗口重新全屏，但上面有另一个可见普通窗口遮挡：TOP layer 应恢复可见 */
    fs_view.fullscreen = true;
    struct wlr_xdg_surface top_base = {0};
    top_base.geometry = (struct wlr_box){.width = 400, .height = 300};
    struct wlr_xdg_toplevel top_toplevel = {.base = &top_base};
    struct woodland_view top_view = {0};
    top_view.server = &server;
    top_view.xdg_toplevel = &top_toplevel;
    top_view.mapped = true;
    top_view.fullscreen = false;
    top_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    /* 插入到表头（即 Z 序在全屏窗口上方） */
    wl_list_insert(&server.views, &top_view.link);

    woodland_layer_update_top_layer_visibility(&server);
    assert(wo->layer_tree[top]->node.enabled == true);

    /* 清理测试 view */
    wl_list_remove(&top_view.link);
    wlr_scene_node_destroy(&top_view.scene_tree->node);
    wl_list_remove(&fs_view.link);
    wlr_scene_node_destroy(&fs_view.scene_tree->node);

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
