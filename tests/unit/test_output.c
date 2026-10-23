// SPDX-License-Identifier: MIT
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

    woodland_output_finish(&server);
    return 0;
}
