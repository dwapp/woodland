// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "output.h"
#include "view.h"
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

    /* 添加一个 800x600 headless output */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 最大化几何 = 输出 layout box */
    struct wlr_box max_geo = woodland_view_maximized_geometry(&server);
    assert(max_geo.width == 800);
    assert(max_geo.height == 600);

    /* 全屏几何 = 指定输出完整 box */
    struct wlr_box fs_geo = woodland_view_fullscreen_geometry(&server, out);
    assert(fs_geo.width == 800);
    assert(fs_geo.height == 600);

    /* exclusive zone 占用后 maximize 应避开：手动设 usable_area */
    struct woodland_output *wo =
        wl_container_of(server.outputs.next, wo, link);
    wo->usable_area = (struct wlr_box){ .x = 10, .y = 10, .width = 780, .height = 580 };
    wo->usable_area_valid = true;
    struct wlr_box max_geo2 = woodland_view_maximized_geometry(&server);
    assert(max_geo2.x == 10 && max_geo2.y == 10);
    assert(max_geo2.width == 780 && max_geo2.height == 580);

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
