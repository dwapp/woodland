// SPDX-License-Identifier: MIT
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include "cursor.h"
#include "output.h"
#include "seat.h"
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

    /* 基础协议已在 server.c 创建，这里模拟 init 顺序测试各模块 */
    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_shell_init(&server));
    woodland_cursor_init_events(&server);

    /* seat 与 xdg_shell 已创建 */
    assert(server.seat != NULL);
    assert(server.xdg_shell != NULL);

    /* 视图链表空 */
    assert(server.views.next == &server.views);
    assert(server.views.prev == &server.views);

    /* 光标模式初始为 passthrough */
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);

    /* 逆序释放（模拟 server finish） */
    woodland_cursor_finish_events(&server);
    woodland_shell_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

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
