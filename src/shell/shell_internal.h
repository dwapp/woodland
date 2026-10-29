// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SHELL_INTERNAL_H
#define WOODLAND_SHELL_INTERNAL_H

#include <stdbool.h>

struct woodland_view;
struct woodland_server;
struct wlr_box;

/*
 * shell 内部的分工（不进 include/，对外仍是 include/view.h 那一套）：
 *
 *   shell.c           xdg toplevel 生命周期、popup、decoration、双后端访问器
 *   toplevel_state.c  窗口状态（最大化/全屏/最小化/标题/app_id/parent）与
 *                     foreign-toplevel 句柄及请求
 *   interactive.c     交互式 move/resize、拖动吸附与松手铺满
 *   cursor.c          光标事件分发、SSD 按钮 hover/pressed
 *
 * 四个文件编进同一个 shell_lib，所以这里只放"确实需要跨文件"的少数符号，
 * 其余一律 static。方向是单向的：cursor → interactive → toplevel_state，
 * 反过来不依赖，避免回到互引用。
 */

/* ---- toplevel_state.c ---- */

/**
 * 挂上窗口状态相关的 xdg 监听器（max/fullscreen/minimize/title/app_id/parent）。
 *
 * 由 shell.c 在建视图时统一调用，免得注册逻辑散在三个文件里。
 */
void shell_toplevel_state_attach(struct woodland_view *view);

/* 下面三个给 interactive.c 用：拖动恢复、贴边恢复、松手铺满都要落几何 */
void shell_view_apply_geometry(struct woodland_view *view,
                               const struct wlr_box *box);
void shell_view_schedule_configure(struct woodland_view *view);
void shell_view_apply_maximized_state(struct woodland_view *view,
                                      bool maximized);

/* ---- interactive.c ---- */

/** 挂上 request_move / request_resize 监听器。 */
void shell_interactive_attach(struct woodland_view *view);

/** 光标拖动中的几何推进（由 cursor.c 按 cursor_mode 分派）。 */
void shell_process_cursor_move(struct woodland_server *server);
void shell_process_cursor_resize(struct woodland_server *server);

/** 松手时把"拖到边上的窗口"铺满（由 cursor.c 在释放分支调用）。 */
void shell_view_apply_snap(struct woodland_view *view);

#endif
