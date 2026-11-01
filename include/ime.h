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

struct woodland_ime_relay;

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
