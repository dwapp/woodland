// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WOODLAND_H
#define WOODLAND_WOODLAND_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "ssd.h"

/** 交互式光标模式（tinywl 语义：移动/缩放/穿越）。 */
enum woodland_cursor_mode {
    WOODLAND_CURSOR_PASSTHROUGH,
    WOODLAND_CURSOR_MOVE,
    WOODLAND_CURSOR_RESIZE,
};

struct woodland_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wl_list outputs;               // struct woodland_output.link 链表头
    struct wl_listener new_output;        // backend->events.new_output 监听器
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;

    /* seat 与输入 */
    struct wlr_seat *seat;
    struct wl_list keyboards;             // struct woodland_keyboard.link 链表头
    struct wl_listener new_input;         // backend->events.new_input 监听器
    struct wl_listener request_cursor;    // seat->events.request_set_cursor
    struct wl_listener request_set_selection; // seat->events.request_set_selection
    struct wl_listener pointer_focus_change;  // seat->pointer_state.events.focus_change

    /* 光标 */
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor_manager;
    struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_listener request_set_shape;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    enum woodland_cursor_mode cursor_mode;
    struct woodland_view *grabbed_view;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    /* SSD 按钮的指针状态：悬停高亮 / 按下（释放时执行动作） */
    struct woodland_view *hover_view;
    enum woodland_ssd_part hover_part;
    struct woodland_view *pressed_view;
    enum woodland_ssd_part pressed_part;

    /* shell 与视图 */
    struct wlr_xdg_shell *xdg_shell;
    struct wl_list views;                 // struct woodland_view.link 链表头
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;

    /* zxdg 装饰 */
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wl_listener new_toplevel_decoration;

    /* 工作区 */
    struct wl_list workspaces;            // struct woodland_workspace.link
    struct woodland_workspace *active_workspace;
    struct wlr_ext_workspace_manager_v1 *ext_workspace_manager;
    struct wlr_ext_workspace_group_handle_v1 *ext_workspace_group;
    struct wl_listener ext_workspace_commit;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

    /* layer-shell */
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wlr_layer_surface_v1 *focused_layer; // 当前持有键盘焦点的 layer surface

    const char *startup_cmd;              // -s 启动命令
};

#endif
