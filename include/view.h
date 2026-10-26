// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_VIEW_H
#define WOODLAND_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>

struct woodland_server;
struct wlr_surface;

struct woodland_view {
    struct wl_list link;               // woodland_server.views
    struct woodland_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

bool woodland_shell_init(struct woodland_server *server);
void woodland_shell_finish(struct woodland_server *server);

/** 命中测试：返回 (lx,ly) 处的视图及其 surface 局部坐标。 */
struct woodland_view *woodland_view_at(struct woodland_server *server,
                                       double lx, double ly,
                                       struct wlr_surface **surface,
                                       double *sx, double *sy);

/** 聚焦视图（键盘焦点 + 激活态 + raise_to_top）。 */
void woodland_view_focus(struct woodland_view *view);

/** 交互式移动/缩放的指针 motion 处理。 */
void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time);

/** 结束交互式 move/resize。 */
void woodland_cursor_reset_mode(struct woodland_server *server);

/** 接线 cursor 事件监听器（motion/button/axis/frame）。 */
void woodland_cursor_init_events(struct woodland_server *server);
void woodland_cursor_finish_events(struct woodland_server *server);

#endif
