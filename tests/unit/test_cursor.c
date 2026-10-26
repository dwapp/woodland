// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "cursor.h"
#include "woodland.h"

int main(void) {
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
