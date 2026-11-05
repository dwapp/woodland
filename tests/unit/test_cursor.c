// SPDX-License-Identifier: GPL-2.0-only
#include "cursor.h"
#include "woodland.h"
#include <assert.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include "scene.h"
#include <wlr/types/wlr_xcursor_manager.h>

int main(void)
{
    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();
    server.output_layout = wlr_output_layout_create(server.display);

    assert(woodland_cursor_init(&server));
    assert(server.cursor != NULL);
    assert(server.xcursor_manager != NULL);

    /* 设置已知形状不崩溃 */
    woodland_cursor_set(&server, "default");
    woodland_cursor_set(&server, "pointer");
    /* 未知形状不崩溃（回退到 default） */
    woodland_cursor_set(&server, "nonexistent-shape-xyz");
    /* 校验边缘及四角映射的标准光标名称 */
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_TOP), "n-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_BOTTOM), "s-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_LEFT), "w-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_RIGHT), "e-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_TOP | WLR_EDGE_LEFT), "nw-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_TOP | WLR_EDGE_RIGHT), "ne-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_BOTTOM | WLR_EDGE_LEFT), "sw-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT), "se-resize") == 0);
    assert(strcmp(woodland_cursor_resize_name(0), "default") == 0);

    /* 校验传统 X11 回退光标名称 */
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_TOP), "top_side") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_BOTTOM), "bottom_side") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_LEFT), "left_side") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_RIGHT), "right_side") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_TOP | WLR_EDGE_LEFT),
                  "top_left_corner") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_TOP | WLR_EDGE_RIGHT),
                  "top_right_corner") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_BOTTOM | WLR_EDGE_LEFT),
                  "bottom_left_corner") == 0);
    assert(strcmp(woodland_cursor_resize_fallback_name(WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT),
                  "bottom_right_corner") == 0);

    /* 设置缩放光标不崩溃 */
    woodland_cursor_set_resize(&server, WLR_EDGE_TOP);
    woodland_cursor_set_resize(&server, WLR_EDGE_BOTTOM);
    woodland_cursor_set_resize(&server, WLR_EDGE_LEFT);
    woodland_cursor_set_resize(&server, WLR_EDGE_RIGHT);
    woodland_cursor_set_resize(&server, WLR_EDGE_TOP | WLR_EDGE_LEFT);
    woodland_cursor_set_resize(&server, WLR_EDGE_TOP | WLR_EDGE_RIGHT);
    woodland_cursor_set_resize(&server, WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
    woodland_cursor_set_resize(&server, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
    woodland_cursor_set_resize(&server, 0);
    woodland_cursor_finish(&server);

    /* 逆序释放 wlr 资源（ASan 下验证无泄漏） */
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_output_layout_destroy(server.output_layout);
    server.output_layout = NULL;
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
