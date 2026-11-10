// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_IME_H
#define WOODLAND_IME_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct woodland_server;
struct woodland_keyboard;
struct wlr_surface;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct wlr_input_method_manager_v2;
struct wlr_text_input_manager_v3;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_input_method_v2;
struct wlr_scene_tree;
struct woodland_text_input;
struct woodland_ime_popup;

struct woodland_ime_relay {
    struct woodland_server *server;
    struct wlr_input_method_manager_v2 *input_method_manager;
    struct wlr_text_input_manager_v3 *text_input_manager;
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;

    struct wlr_input_method_v2 *input_method;
    struct wlr_surface *focused_surface;
    struct woodland_text_input *active_text_input;

    struct wl_list text_inputs; // struct woodland_text_input.link
    struct wl_list popups;      // struct woodland_ime_popup.link
    struct wlr_scene_tree *popup_tree;

    struct wl_listener new_input_method;
    struct wl_listener new_text_input;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener seat_focus_change;
    struct wl_listener focused_surface_destroy;

    struct wl_listener input_method_commit;
    struct wl_listener input_method_grab_keyboard;
    struct wl_listener input_method_destroy;
    struct wl_listener input_method_new_popup_surface;
    struct wl_listener keyboard_grab_destroy;
};
/**
 * 安全特权策略说明 (Security Policy):
 *
 * Woodland 为单用户 Wayland 会话合成器，目前已通过 wp_security_context_manager_v1
 * 对带沙箱上下文的客户端屏蔽特权全局（含 zwp_input_method_manager_v2 与
 * zwp_virtual_keyboard_manager_v1）。对于未沙箱化的本地同会话客户端，这两者仍
 * 暴露在全局接口列表中，供用户环境下的输入法引擎（如 Fcitx5、IBus）绑定使用。
 *
 * [已知风险与限制]：
 * 任何未带 security context 且能连入主 Wayland socket 的客户端均可请求输入法或虚拟键盘权限。
 * 后续若对输入法 / 虚拟键盘的绑定方施加进一步权限校验，应在 wl_global 的 bind 回调或
 * 统一全局过滤器入口 woodland_global_filter_add()（include/util/global_filter.h）中注册
 * checker，严禁直接调用 wl_display_set_global_filter() 覆盖既有过滤策略。
 */

/**
 * 初始化 IME 模块与输入法中继器（zwp_input_method_v2, zwp_text_input_v3,
 * zwp_virtual_keyboard_v1）。
 */
bool woodland_ime_init(struct woodland_server *server);

/**
 * 释放 IME 模块所有资源与监听器。
 */
void woodland_ime_finish(struct woodland_server *server);

/**
 * 更新当前拥有键盘焦点的 surface。
 */
void woodland_ime_set_focus(struct woodland_server *server, struct wlr_surface *surface);

/**
 * 转发物理键盘按键至输入法抓取对象（如果存在 active keyboard grab）。
 * 返回 true 表示按键已被输入法拦截处理；false 表示需继续由合成器或应用处理。
 */
bool woodland_ime_keyboard_grab_key(struct woodland_server *server, struct wlr_keyboard *keyboard,
                                    struct wlr_keyboard_key_event *event);

/**
 * 转发修饰键状态至输入法抓取对象。
 * 返回 true 表示修饰键已被转发；false 表示未拦截。
 */
bool woodland_ime_keyboard_grab_modifiers(struct woodland_server *server,
                                          struct wlr_keyboard *keyboard);

/**
 * 判断指定键盘是否是由当前输入法客户端模拟的虚拟键盘（防止按键死循环）。
 */
bool woodland_ime_is_emulated_keyboard(struct woodland_server *server,
                                       struct wlr_keyboard *keyboard);

#endif /* WOODLAND_IME_H */
