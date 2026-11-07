// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include "scene.h"
#include <xkbcommon/xkbcommon.h>

#include "cursor.h"
#include "output.h"
#include "seat.h"
#include "view.h"
#include "woodland.h"

/*
 * VT 切换键识别（XF86Switch_VT_1..12 → 1..12）。
 *
 * 这是唯一的回归点：段内等差映射必须一格不差，段外一律 0——
 * 多认一格就会把普通功能键当 VT 键吞掉，少认一格就是按了没反应。
 */
static void test_vt_from_keysym(void)
{
    for (unsigned vt = 1; vt <= 12; vt++) {
        xkb_keysym_t sym = XKB_KEY_XF86Switch_VT_1 + (vt - 1);
        assert(woodland_seat_vt_from_keysym(sym) == vt);
    }
    /* 段前一格 / 段后一格 / 普通功能键 / 零值都不是 VT 键 */
    assert(woodland_seat_vt_from_keysym(XKB_KEY_XF86Switch_VT_1 - 1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_XF86Switch_VT_12 + 1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_F1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_Escape) == 0);
    assert(woodland_seat_vt_from_keysym(0) == 0);
}

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

    test_vt_from_keysym();

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
