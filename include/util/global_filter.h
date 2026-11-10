// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_UTIL_GLOBAL_FILTER_H
#define WOODLAND_UTIL_GLOBAL_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

/*
 * wl_display 全局接口过滤器的多路分发入口。
 *
 * libwayland-server 的 wl_display_set_global_filter() 是 display 级单例：
 * 任意模块直接调用都会静默覆盖其他模块的过滤器并导致权限/隔离失效。
 * 所有模块（security-context、xwayland、后续 IME 权限校验等）统一经本接口
 * 注册具名 checker：
 *   - 任一 checker 返回 false 即拒绝向该 client 暴露该 global；
 *   - 同名重复注册或超出上限会记录 ERROR 并返回 false（不覆盖已有 checker）；
 *   - display 销毁时通过 destroy listener 自动回收。
 */
#define WOODLAND_GLOBAL_FILTER_MAX 32

/**
 * 为 display 注册一个具名全局过滤器 checker。
 * 参数无效、同名已存在或达到上限时记 ERROR 并返回 false。
 */
bool woodland_global_filter_add(struct wl_display *display,
                                const char *name,
                                wl_display_global_filter_func_t filter,
                                void *data);

/**
 * 注销之前注册的同名 checker；最后一个注销后清空 display 上的过滤器。
 */
void woodland_global_filter_remove(struct wl_display *display, const char *name);

/** 当前注册在该 display 上的 checker 数量。 */
size_t woodland_global_filter_count(struct wl_display *display);

/** 指定名字的 checker 是否已在 display 上注册。 */
bool woodland_global_filter_has(struct wl_display *display, const char *name);

#endif
