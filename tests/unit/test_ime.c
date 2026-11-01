// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "ime.h"
#include "seat.h"
#include "woodland.h"

int main(void)
{
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    server.event_loop = wl_display_get_event_loop(server.display);
    assert(server.event_loop != NULL);

    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend != NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer != NULL);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    server.output_layout = wlr_output_layout_create(server.display);
    assert(server.output_layout != NULL);

    assert(woodland_seat_init(&server));
    assert(server.seat != NULL);

    /* 1. 初始化 IME 中继器 */
    assert(woodland_ime_init(&server));
    assert(server.ime != NULL);

    /* 2. 焦点切换安全测试 */
    woodland_ime_set_focus(&server, NULL);

    /* 3. 键盘抓取与虚拟键盘防死循环测试（无输入法绑定时返回 false，不拦截正常输入） */
    struct wlr_keyboard keyboard = {0};
    wlr_keyboard_init(&keyboard, NULL, "test");
    struct wlr_keyboard_key_event event = {
        .time_msec = 1000,
        .keycode = 30, // KEY_A
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
        .update_state = true,
    };

    assert(!woodland_ime_is_emulated_keyboard(&server, &keyboard));
    assert(!woodland_ime_keyboard_grab_key(&server, &keyboard, &event));
    assert(!woodland_ime_keyboard_grab_modifiers(&server, &keyboard));

    wlr_keyboard_finish(&keyboard);

    /* 4. 析构与逆序清理（ASan 验证无泄漏） */
    woodland_ime_finish(&server);
    assert(server.ime == NULL);

    woodland_seat_finish(&server);

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
