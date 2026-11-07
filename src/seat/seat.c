// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <stdint.h>
#include <stdlib.h>

#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include "scene.h"
#include <xkbcommon/xkbcommon.h>

#include "cursor.h"
#include "focus.h"
#include "ime.h"
#include "protocol.h"
#include "seat.h"
#include "session_lock.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

/* ==================== 键盘 ==================== */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
    struct woodland_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    if (woodland_session_lock_is_locked(keyboard->server)) {
        if (keyboard->server->seat->keyboard_state.focused_surface &&
            wlr_session_lock_surface_v1_try_from_wlr_surface(
                keyboard->server->seat->keyboard_state.focused_surface)) {
            wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
            wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                               &keyboard->wlr_keyboard->modifiers);
        }
        (void)data;
        return;
    }
    if (!woodland_ime_keyboard_grab_modifiers(keyboard->server, keyboard->wlr_keyboard)) {
        wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                           &keyboard->wlr_keyboard->modifiers);
    }
    (void)data;
}

static bool handle_keybinding(struct woodland_server *server, xkb_keysym_t sym, uint32_t modifiers)
{
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server->display);
        break;
    case XKB_KEY_Tab:
        /* Alt+Tab 沿 z 序往前，Alt+Shift+Tab 往回（labwc NextWindow 语义） */
        woodland_focus_cycle(server, (modifiers & WLR_MODIFIER_SHIFT) == 0);
        break;
    case XKB_KEY_F1:
        /* 老键位保留：等价于下一个窗口 */
        woodland_focus_cycle(server, true);
        break;
    case XKB_KEY_1:
    case XKB_KEY_2:
    case XKB_KEY_3:
    case XKB_KEY_4:
    case XKB_KEY_5:
    case XKB_KEY_6:
    case XKB_KEY_7:
    case XKB_KEY_8:
    case XKB_KEY_9: {
        char name[2];
        name[0] = (char)('1' + (sym - XKB_KEY_1));
        name[1] = '\0';
        struct woodland_workspace *ws = woodland_workspace_by_name(server, name);
        if (ws) {
            woodland_workspace_switch(server, ws);
        }
        break;
    }
    case XKB_KEY_0: {
        struct woodland_workspace *ws = woodland_workspace_by_name(server, "10");
        if (ws) {
            woodland_workspace_switch(server, ws);
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

/*
 * VT 切换键识别与执行（labwc src/input/keyboard.c 的 change_vt /
 * handle_change_vt_key 同款语义）。
 *
 * 1. xkeyboard-config 的 compat 规则把 Ctrl+Alt+F1..F12 翻成
 *    XF86Switch_VT_1..12 keysym，所以不用自己核对修饰键组合——能从
 *    当前键盘状态翻出这个 keysym，组合就已经成立。
 * 2. press 被合成器吃掉后不下传客户端；对应的 release 要么在切回本
 *    VT 后才到、要么根本不来，晚到的孤儿 release 也一并丢弃——
 *    没有 press 配对的 release 会让客户端按键状态错乱。
 */
unsigned woodland_seat_vt_from_keysym(xkb_keysym_t sym)
{
    unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
    return (vt >= 1 && vt <= 12) ? vt : 0;
}

static void change_vt(struct woodland_server *server, unsigned vt)
{
    if (!server->session) {
        /* headless 后端没有 wlr_session：键路是通的，但切换无处执行 */
        woodland_trace_add("vt", 0, "no session, vt %u ignored", vt);
        return;
    }
    woodland_trace_add("vt", 0, "session change_vt %u", vt);
    wlr_session_change_vt(server->session, vt);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
    struct woodland_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct woodland_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    /* 任何按键都算"人还在"，idle 计时从这里续 */
    woodland_protocol_notify_activity(server);

    /*
     * VT 切换键（Ctrl+Alt+F1..F12）。排在锁屏与 shortcuts-inhibit 之前：
     * 切走 TTY 是系统级逃生通道，锁屏和游戏的全键抓取都不该拦住它
     * （labwc 同序：VT 键在 locked 判定之前处理）。
     */
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            unsigned vt = woodland_seat_vt_from_keysym(syms[i]);
            if (vt) {
                if (event->keycode < sizeof(server->vt_swallowed_keys) * 8) {
                    server->vt_swallowed_keys[event->keycode / 8] |=
                        (uint8_t)(1u << (event->keycode % 8));
                }
                change_vt(server, vt);
                return;
            }
        }
    } else if (event->keycode < sizeof(server->vt_swallowed_keys) * 8 &&
               (server->vt_swallowed_keys[event->keycode / 8] &
                (uint8_t)(1u << (event->keycode % 8))) != 0) {
        /* 孤儿 release：press 被 VT 切换吃掉的那颗键，release 不下传 */
        server->vt_swallowed_keys[event->keycode / 8] &=
            (uint8_t)~(1u << (event->keycode % 8));
        return;
    }

    /* 锁屏状态下：禁止所有快捷键，按键只能发给锁屏表面 */
    if (woodland_session_lock_is_locked(server)) {
        if (seat->keyboard_state.focused_surface &&
            wlr_session_lock_surface_v1_try_from_wlr_surface(
                seat->keyboard_state.focused_surface)) {
            wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
            wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        }
        return;
    }

    /*
     * 焦点 surface 若持有 keyboard-shortcuts-inhibit，合成器键位要让路
     * （游戏、远程桌面全屏抓键就靠这个）。抑制器随焦点走，切走即失效，
     * 所以不存在"客户端把 Alt+Tab 永久吞掉"的死角。
     *
     * 全局快捷键（treeland-shortcut-manager-v3）与一次性捕获排在内置
     * Alt+ 键位之前：capture 要抢在绑定前面，绑定命中后按键也不该再发给
     * 焦点 surface。抑制器生效时三者一起让路。
     */
    bool inhibited = woodland_protocol_shortcuts_inhibited(server);
    if (!inhibited &&
        woodland_protocol_shortcuts_key(server, keyboard->wlr_keyboard, event)) {
        return;
    }

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if (!inhibited && (modifiers & WLR_MODIFIER_ALT) &&
        event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(server, syms[i], modifiers);
        }
    }

    if (!handled) {
        if (!woodland_ime_keyboard_grab_key(server, keyboard->wlr_keyboard, event)) {
            wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
            wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        }
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
    (void)data;
}

void woodland_seat_new_keyboard(struct woodland_server *server, struct wlr_input_device *device)
{
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct woodland_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        return;
    }
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_rule_names rules = {0};
    int32_t repeat_rate = 25;
    int32_t repeat_delay = 600;
    if (server->config) {
        rules.model = server->config->keyboard.model;
        rules.layout = server->config->keyboard.layout;
        rules.variant = server->config->keyboard.variant;
        rules.options = server->config->keyboard.options;
        repeat_rate = server->config->keyboard.repeat_rate;
        repeat_delay = server->config->keyboard.repeat_delay;
    }

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, repeat_rate, repeat_delay);

    woodland_listener_connect(&keyboard->modifiers, &wlr_keyboard->events.modifiers,
                              keyboard_handle_modifiers, "seat.modifiers");
    woodland_listener_connect(&keyboard->key, &wlr_keyboard->events.key, keyboard_handle_key,
                              "seat.key");
    woodland_listener_connect(&keyboard->destroy, &device->events.destroy, keyboard_handle_destroy,
                              "seat.destroy");

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);

    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct woodland_server *server, struct wlr_input_device *device)
{
    wlr_cursor_attach_input_device(server->cursor, device);
    /* 手势协议按设备挂监听，交给协议层自己管生命周期 */
    woodland_protocol_attach_input(server, device);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        woodland_seat_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

/* ==================== seat 事件 ==================== */

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, pointer_focus_change);
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (event->new_surface == NULL) {
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
    }
    woodland_pointer_constraint_recheck(server, event->new_surface);
}
static void seat_keyboard_focus_change(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, keyboard_focus_change);
    (void)data;
    woodland_pointer_constraint_update(server);
}


static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ==================== init/finish ==================== */

bool woodland_seat_init(struct woodland_server *server)
{
    wl_list_init(&server->keyboards);

    server->seat = wlr_seat_create(server->display, "seat0");
    if (!server->seat) {
        return false;
    }

    woodland_listener_connect(&server->new_input, &server->backend->events.new_input,
                              server_new_input, "seat.new_input");

    woodland_listener_connect(&server->request_cursor, &server->seat->events.request_set_cursor,
                              seat_request_cursor, "seat.request_set_cursor");

    woodland_listener_connect(&server->pointer_focus_change,
                              &server->seat->pointer_state.events.focus_change,
                              seat_pointer_focus_change, "seat.focus_change");
    woodland_listener_connect(&server->keyboard_focus_change,
                              &server->seat->keyboard_state.events.focus_change,
                              seat_keyboard_focus_change, "seat.keyboard_focus_change");

    woodland_listener_connect(&server->request_set_selection,
                              &server->seat->events.request_set_selection,
                              seat_request_set_selection, "seat.request_set_selection");

    return true;
}

void woodland_seat_finish(struct woodland_server *server)
{
    if (server->seat) {
        wl_list_remove(&server->new_input.link);
        wl_list_remove(&server->request_cursor.link);
        wl_list_remove(&server->request_set_selection.link);
        wl_list_remove(&server->pointer_focus_change.link);
        if (server->keyboard_focus_change.link.prev) {
            wl_list_remove(&server->keyboard_focus_change.link);
            server->keyboard_focus_change.link.prev = NULL;
        }
    }
}
