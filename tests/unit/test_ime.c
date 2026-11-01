// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>

#include "ime.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "seat.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "woodland.h"

struct client_state {
    struct zwp_input_method_manager_v2 *im_mgr;
    struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
    struct wl_seat *seat;
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
        state->im_mgr = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        state->vk_mgr =
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
    }
    (void)version;
}
static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void sync_callback(void *data, struct wl_callback *cb, uint32_t serial)
{
    bool *done = data;
    *done = true;
    wl_callback_destroy(cb);
    (void)serial;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_callback,
};

static void pump_roundtrip(struct woodland_server *server, struct wl_display *client_disp)
{
    bool done = false;
    struct wl_callback *cb = wl_display_sync(client_disp);
    assert(cb != NULL);
    wl_callback_add_listener(cb, &sync_listener, &done);
    while (!done) {
        assert(wl_display_get_error(client_disp) == 0);
        wl_display_flush(client_disp);
        wl_event_loop_dispatch(server->event_loop, 0);
        wl_display_flush_clients(server->display);
        wl_display_dispatch(client_disp);
    }
}

int main(void)
{
    setenv("WLR_RENDERER", "pixman", 1);
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

    /* 1. 初始化 IME 中继器并断言三个核心协议 global 均成功注册 */
    assert(woodland_ime_init(&server));
    assert(server.ime != NULL);
    assert(server.ime->input_method_manager != NULL);
    assert(server.ime->input_method_manager->global != NULL);
    assert(server.ime->text_input_manager != NULL);
    assert(server.ime->text_input_manager->global != NULL);
    assert(server.ime->virtual_keyboard_manager != NULL);
    assert(server.ime->virtual_keyboard_manager->global != NULL);

    /* 2. 焦点切换测试：覆盖非空 surface 焦点切换分支与 surface 销毁回退分支 */
    struct wlr_surface fake_surface = {0};
    wl_signal_init(&fake_surface.events.destroy);

    woodland_ime_set_focus(&server, &fake_surface);
    assert(server.ime->focused_surface == &fake_surface);

    woodland_ime_set_focus(&server, NULL);
    assert(server.ime->focused_surface == NULL);

    /* 重新赋非空焦点后触发 destroy 信号，断言自动置空焦点 */
    woodland_ime_set_focus(&server, &fake_surface);
    assert(server.ime->focused_surface == &fake_surface);
    wl_signal_emit_mutable(&fake_surface.events.destroy, NULL);
    assert(server.ime->focused_surface == NULL);

    /* 3. 普通物理键盘初始化接入 seat */
    struct wlr_keyboard regular_kb = {0};
    wlr_keyboard_init(&regular_kb, NULL, "regular-kb");
    woodland_seat_new_keyboard(&server, &regular_kb.base);
    assert(!wl_list_empty(&server.keyboards));

    struct woodland_keyboard *regular_woodland =
        wl_container_of(server.keyboards.next, regular_woodland, link);

    struct wlr_keyboard_key_event event = {
        .time_msec = 1000,
        .keycode = 30, // KEY_A
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
        .update_state = true,
    };

    /* 无输入法绑定时：普通键盘不被判定为模拟键盘，grab 不拦截 */
    assert(!woodland_ime_is_emulated_keyboard(&server, &regular_kb));
    assert(!woodland_ime_keyboard_grab_key(&server, &regular_kb, &event));
    assert(!woodland_ime_keyboard_grab_modifiers(&server, &regular_kb));

    /* 4. 客户端连接并绑定 input-method-v2 与 virtual-keyboard-v1 */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *client = wl_client_create(server.display, fds[0]);
    assert(client != NULL);

    struct wl_display *client_display = wl_display_connect_to_fd(fds[1]);
    assert(client_display != NULL);

    struct client_state state = {0};
    struct wl_registry *registry = wl_display_get_registry(client_display);
    wl_registry_add_listener(registry, &registry_listener, &state);

    pump_roundtrip(&server, client_display);

    assert(state.im_mgr != NULL);
    assert(state.vk_mgr != NULL);
    assert(state.seat != NULL);

    /* 客户端创建 input_method 与 virtual_keyboard 实例 */
    struct zwp_input_method_v2 *im =
        zwp_input_method_manager_v2_get_input_method(state.im_mgr, state.seat);
    assert(im != NULL);

    struct zwp_virtual_keyboard_v1 *vk =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.vk_mgr, state.seat);
    assert(vk != NULL);

    pump_roundtrip(&server, client_display);

    /* 服务端应已收到绑定并记录 input_method */
    assert(server.ime->input_method != NULL);

    /* 寻找服务端新增的虚拟键盘 */
    struct woodland_keyboard *vk_woodland = NULL;
    struct woodland_keyboard *k;
    wl_list_for_each(k, &server.keyboards, link)
    {
        if (k != regular_woodland) {
            vk_woodland = k;
            break;
        }
    }
    assert(vk_woodland != NULL);

    /* 验证同 client 虚拟键盘判定：
     * virtual_keyboard 与 input_method 属于同一客户端连接 -> true
     * regular_kb 为物理键盘 -> false */
    assert(woodland_ime_is_emulated_keyboard(&server, vk_woodland->wlr_keyboard) == true);
    assert(woodland_ime_is_emulated_keyboard(&server, regular_woodland->wlr_keyboard) == false);

    /* 5. 激活输入法 grab 后的按键转发与防死循环断言 */
    struct zwp_input_method_keyboard_grab_v2 *grab = zwp_input_method_v2_grab_keyboard(im);
    pump_roundtrip(&server, client_display);
    assert(server.ime->input_method->keyboard_grab != NULL);

    /* 普通键盘按键与修饰键：应被 grab 转发拦截 (返回 true) */
    assert(woodland_ime_keyboard_grab_key(&server, regular_woodland->wlr_keyboard, &event) == true);
    assert(woodland_ime_keyboard_grab_modifiers(&server, regular_woodland->wlr_keyboard) == true);

    /* 虚拟键盘按键与修饰键：防死循环机制必须拒绝转发至自身 grab (返回 false)！ */
    assert(woodland_ime_keyboard_grab_key(&server, vk_woodland->wlr_keyboard, &event) == false);
    assert(woodland_ime_keyboard_grab_modifiers(&server, vk_woodland->wlr_keyboard) == false);

    /* 6. 销毁客户端对象与断开连接 */
    zwp_input_method_keyboard_grab_v2_release(grab);
    zwp_virtual_keyboard_v1_destroy(vk);
    zwp_input_method_v2_destroy(im);
    zwp_input_method_manager_v2_destroy(state.im_mgr);
    zwp_virtual_keyboard_manager_v1_destroy(state.vk_mgr);
    wl_seat_release(state.seat);
    wl_registry_destroy(registry);
    pump_roundtrip(&server, client_display);
    wl_display_disconnect(client_display);
    wl_display_destroy_clients(server.display);
    wl_event_loop_dispatch(server.event_loop, 0);

    /* 物理键盘销毁 */
    wlr_keyboard_finish(&regular_kb);

    /* 7. 析构与逆序清理（ASan 验证无泄漏） */
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
