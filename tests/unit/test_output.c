// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include "output.h"
#include "woodland.h"

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();

    assert(woodland_output_init(&server));

    /* 无 output 时 stable */
    assert(server.outputs.next == &server.outputs);
    assert(server.outputs.prev == &server.outputs);

    /* 模拟 new_output 事件 */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 去重：重复 emit 不应重复创建 */
    wl_signal_emit_mutable(&server.backend->events.new_output, out);
    size_t n = 0;
    struct woodland_output *o;
    wl_list_for_each(o, &server.outputs, link) n++;
    assert(n == 1);

    woodland_output_finish(&server);

    /* 逆序释放 wlr 资源（ASan 下验证无泄漏） */
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
