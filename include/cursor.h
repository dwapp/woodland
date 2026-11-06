// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_CURSOR_H
#define WOODLAND_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_server;
struct wlr_surface;
struct wlr_pointer_constraint_v1;

bool woodland_cursor_init(struct woodland_server *server);
void woodland_cursor_finish(struct woodland_server *server);
void woodland_cursor_set(struct woodland_server *server, const char *name);

/*
 * 光标尺寸的支持范围。协议（treeland-appearance-manager）要求把越界值
 * clamp 到"合成器支持的最小/最大"，这里是那个范围：
 *   下界 8：再小的光标连箭头都画不出来，等于没有；
 *   上界 256：主流光标集最大档就是 256（hidpi 2x 的 128 也够）。
 * 配置文件里的 [cursor] size 不走这个裁剪——那是用户自己写的值。
 */
#define WOODLAND_CURSOR_MIN_SIZE 8
#define WOODLAND_CURSOR_MAX_SIZE 256
#define WOODLAND_CURSOR_DEFAULT_SIZE 24

/**
 * 热切换光标主题与尺寸（重建 xcursor manager 并重设当前光标）。
 *
 * theme 为 NULL 或空串表示交给 wlroots 的默认主题链。请求的主题没装上时
 * 自动退到默认主题，`effective`（可为 NULL）返回实际生效的主题名——协议
 * 那边要把真实值报给观测者，不能报"已生效"却仍是旧主题。
 *
 * 失败返回 false（此时旧 manager 原封不动，不留下半切换状态）。
 */
bool woodland_cursor_apply_theme(struct woodland_server *server, const char *theme, int size,
                                 const char **effective);
void woodland_cursor_set_resize(struct woodland_server *server, uint32_t edges);
const char *woodland_cursor_resize_name(uint32_t edges);
const char *woodland_cursor_resize_fallback_name(uint32_t edges);

/* 指针约束管理（zwp_pointer_constraints_v1） */
bool woodland_pointer_constraints_init(struct woodland_server *server);
void woodland_pointer_constraints_finish(struct woodland_server *server);
void woodland_pointer_constraint_recheck(struct woodland_server *server,
                                         struct wlr_surface *surface);
void woodland_pointer_constraint_set(struct woodland_server *server,
                                     struct wlr_pointer_constraint_v1 *constraint);
void woodland_pointer_constraint_apply(struct woodland_server *server,
                                       double *dx, double *dy);
#endif
