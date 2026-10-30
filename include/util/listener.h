// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_UTIL_LISTENER_H
#define WOODLAND_UTIL_LISTENER_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-core.h>

struct woodland_json;

/*
 * wl_listener 的带账目封装：注册时记名字和序号，emit 时计数。
 *
 * 为什么需要它：监听器按注册顺序调用，顺序就是行为（AGENTS.md 坑 2）。
 * 但注册散布在十几个 init 里，唯一的"顺序文档"是注释——注释会过期，
 * 崩溃不会提前打招呼。get_listeners 把注册顺序变成运行时可见的事实：
 * 漏注册、注册反了，一眼看出，不用靠复现 crash 反推。
 *
 * 用法（替代 notify 赋值 + wl_signal_add 两行）：
 *     woodland_listener_connect(&view->request_move,
 *                               &toplevel->events.request_move,
 *                               xdg_toplevel_request_move, "xdg.request_move");
 *
 * 回调照旧写 wl_container_of(listener, ...)，工具不改任何运行时语义；
 * 摘除也照旧 wl_list_remove。emit 计数两条路：connect_verbose 注册
 * （自动计数），或回调开头插一行 woodland_listener_count(listener)。
 * 不插也行：名字和注册序号仍然全量可见。
 *
 * 上限 256 条是防御性的：注册点全局只有百余处，超过说明有泄漏
 * （listener 注册了没摘），dump 会把溢出标出来而不是静默丢弃。
 */
#define WOODLAND_LISTENER_MAX 256

/**
 * 注册并记账。name 建议带模块前缀，如 "shell.map"、"input.cursor_motion"。
 */
void woodland_listener_connect(struct wl_listener *listener,
                               struct wl_signal *signal,
                               wl_notify_func_t notify, const char *name);

/**
 * 同 connect，但 emit 自动计数（经账本 thunk 转调真实回调，回调里的
 * wl_container_of 不受影响）。给"这个 handler 到底跑没跑"这类问题的
 * 首选答案：注册行换个名字，emits 就自己涨。
 *
 * 两种计数的取舍：
 *   - verbose 注册（这里）：零回调侵入，但每次 emit 多一次账本线性查找
 *     ——留给需要观察的监听器，别全量用；
 *   - 手动 count（下面）：回调开头插一行，零查找开销，适合已经在
 *     改这个回调、顺手想计数的时候。
 */
void woodland_listener_connect_verbose(struct wl_listener *listener,
                                       struct wl_signal *signal,
                                       wl_notify_func_t notify,
                                       const char *name);

/** 在回调开头调用，给宿主 listener 的 emit 计数 +1。listener 可为 NULL。 */
void woodland_listener_count(struct wl_listener *listener);

/** 当前注册总数。 */
uint32_t woodland_listener_total(void);

/** 按 JSON 导出全部账目（seq 升序）：seq/name/emits。 */
void woodland_listener_dump(struct woodland_json *out);

#endif
