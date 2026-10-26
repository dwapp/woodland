// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_VIEW_H
#define WOODLAND_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct woodland_server;
struct woodland_workspace;
struct wlr_surface;
struct wlr_output;

struct woodland_view {
    struct wl_list link;               // woodland_server.views
    struct woodland_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct woodland_workspace *workspace;   // 归属工作区
    bool mapped;
    bool minimized;
    bool maximized;
    bool fullscreen;
    char *title;
    char *app_id;
    struct wlr_box saved_geometry;     // 退出 max/fullscreen 时恢复
    struct wlr_foreign_toplevel_handle_v1 *toplevel_handle;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener set_parent;
    struct wl_listener ft_request_activate;
    struct wl_listener ft_request_maximize;
    struct wl_listener ft_request_fullscreen;
    struct wl_listener ft_request_minimize;
    struct wl_listener ft_request_close;
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

/** 最大化几何（布局坐标，取输出 layout 的活动 box）。 */
struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server);

/** 全屏几何（指定输出的完整 box，布局坐标）。 */
struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output);

#endif
