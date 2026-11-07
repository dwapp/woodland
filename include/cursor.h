// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_CURSOR_H
#define WOODLAND_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_server;
struct wlr_surface;
struct wlr_pointer_constraint_v1;
struct wl_client;

bool woodland_cursor_init(struct woodland_server *server);
void woodland_cursor_finish(struct woodland_server *server);
void woodland_cursor_set(struct woodland_server *server, const char *name);
void woodland_cursor_load_scale(struct woodland_server *server, float scale);

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

/**
 * 约束的申请者现在是否持有焦点。走 shell 挂的钩子（视图 / layer / 锁屏都在那一层
 * 才有答案）；没有钩子时退到"键盘焦点表面同 client"这条最粗的判定。
 */
bool woodland_pointer_constraint_is_focused(struct woodland_server *server,
                                            struct wlr_pointer_constraint_v1 *constraint);

/** 申请约束的 wl_client（先看约束的 resource，再退到 surface 的）。拿不到返回 NULL。 */
struct wl_client *woodland_pointer_constraint_get_client(
    const struct wlr_pointer_constraint_v1 *constraint);

/**
 * 键盘焦点表面是否与 client 同属一个 wl_client（**不含**"同一 surface"这层判断：
 * 调用方自己有 surface 指针，用指针相等判更稳，resource 不一定还在）。
 *
 * 约束按 surface 申请，但 popup / subsurface 拿到键盘焦点时它也应当继续有效，
 * 所以比到 client 一级。shell 层的 `shell_pointer_constraint_is_focused()` 与
 * cursor 层没有钩子时的兜底共用这**一条**判定，别各写一份。
 */
bool woodland_pointer_constraint_matches_keyboard_focus(struct woodland_server *server,
                                                        struct wl_client *client);

/**
 * 按当前指针焦点表面重算一次约束状态（键盘焦点 / 视图焦点变化后调用）。
 * 内部就是 `woodland_pointer_constraint_recheck(server, 指针焦点表面)`。
 */
void woodland_pointer_constraint_update(struct woodland_server *server);
#endif
