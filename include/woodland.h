// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WOODLAND_H
#define WOODLAND_WOODLAND_H

#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include "scene.h"
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "ssd.h"
#include "view.h"

struct wlr_xwayland;
struct wlr_compositor;
/* 上游协议（src/protocol）：整体不透明，外部只经 protocol.h 的接口访问 */
struct woodland_proto;
struct woodland_config;
struct woodland_ime_relay;
struct woodland_session_lock;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_constraint_v1;
struct woodland_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;
    struct wl_event_source *sighup_source;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wl_list outputs;        // struct woodland_output.link 链表头
    struct wl_listener new_output; // backend->events.new_output 监听器
    /*
     * 新输出的收尾广播：在 woodland_output 完全就位（含 commit）之后跑，
     * 因为 output 层不该反过来依赖协议层。注册顺序决定执行顺序，所以在
     * protocol init 之后才挂上去。
     */
    struct wl_listener new_output_broadcast;
    /*
     * 输出销毁通知：参数是即将消失的 wlr_output（struct woodland_output
     * 已经从 server->outputs 摘掉并释放）。给协议层留的钩子——output 层
     * 不能反向调用协议层，而输出销毁本身不一定触发 layout change（被
     * --off 禁用的输出早已不在布局里），靠 layout change 驱动的广播补不上。
     */
    struct wl_signal output_removed;
    /*
     * 当前工作区切换完成后的通知（参数是被切到的 struct woodland_workspace）。
     *
     * 和 output_removed 同一个理由：workspace 层不该反向依赖协议层，而
     * "切工作区"对壁纸族不是记账而是**行为**——每个源一个 surface 全堆在
     * 同一位置，必须按新工作区重挑哪张可见、其余的暂停。
     */
    struct wl_signal workspace_changed;
    /*
     * 壁纸树的场景节点（没有协议层时为 NULL）。
     *
     * 挂在 server 上而不是让 self_check 反查协议层：shell 库不能反过来依赖
     * protocol 库，否则每个链接 shell 的单元测试都要连带链接协议族。
     * 壁纸族负责在建立/销毁时置上、置空。
     */
    struct wlr_scene_tree *wallpaper_tree;
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;

    /* seat 与输入 */
    struct wlr_seat *seat;
    struct wl_list keyboards;                   // struct woodland_keyboard.link 链表头
    struct wl_listener new_input;               // backend->events.new_input 监听器
    struct wl_listener request_cursor;          // seat->events.request_set_cursor
    struct wl_listener request_set_selection;   // seat->events.request_set_selection
    struct wl_listener pointer_focus_change;    // seat->pointer_state.events.focus_change
    struct woodland_ime_relay *ime;             // 输入法中继器与协议管理
    struct woodland_session_lock *session_lock; // 会话锁状态与锁屏管理

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
    double drag_start_x, drag_start_y; // 本次拖动起点（判断是否真的拖动了）
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    /* 指针约束（zwp_pointer_constraints_v1） */
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *current_constraint;
    struct wl_listener new_pointer_constraint;
    struct wl_listener constraint_destroy;
    struct wl_listener constraint_commit;

    /* SSD 按钮的指针状态：悬停高亮 / 按下（释放时执行动作） */
    struct woodland_view *hover_view;
    enum woodland_ssd_part hover_part;
    struct woodland_view *pressed_view;
    enum woodland_ssd_part pressed_part;

    /* shell 与视图 */
    struct wlr_xdg_shell *xdg_shell;
    struct wl_list views; // struct woodland_view.link 链表头（z 序，最上在前）
    /*
     * 当前焦点视图。
     *
     * 不能只靠 seat->keyboard_state.focused_surface：没有键盘设备
     * （headless / 纯触摸）时键盘 enter 不会发出，那个字段永远是 NULL，
     * 焦点切换就只能靠猜。这里显式记一份，surface 字段仍用于下发输入。
     */
    struct woodland_view *focused_view;
    /* 新窗口放置策略（-p / woodland_placement_policy_parse） */
    enum woodland_placement_policy placement_policy;
    bool placement_specified;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;

    /* xdg-dialog-v1：模态对话框标志（焦点分配要用） */
    struct wlr_xdg_wm_dialog_v1 *xdg_dialog_manager;

    /* zxdg 装饰 */
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wl_listener new_toplevel_decoration;

    /* 工作区 */
    struct wl_list workspaces; // struct woodland_workspace.link
    struct woodland_workspace *active_workspace;
    struct wlr_ext_workspace_manager_v1 *ext_workspace_manager;
    struct wlr_ext_workspace_group_handle_v1 *ext_workspace_group;
    struct wl_listener ext_workspace_commit;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

    /* layer-shell */
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wlr_layer_surface_v1 *focused_layer; // 当前持有键盘焦点的 layer surface

    /*
     * 上游协议（src/protocol）。
     *
     * 只有一个不透明指针：各协议族的状态、监听器与链表都在 protocol 模块
     * 内部持有（见 src/protocol/protocol_internal.h）。这里保持"知道它存在、不知道它
     * 里面有什么"，否则每接一个协议就要在全局结构里加两行。
     */
    struct woodland_proto *proto;

    /* 截图/录屏（src/capture） */
    struct wlr_screencopy_manager_v1 *screencopy_manager;
    struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture_manager;
    struct wlr_ext_output_image_capture_source_manager_v1 *output_capture_source_manager;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_capture_manager;
    struct wl_listener toplevel_capture_request;
    struct wlr_ext_foreign_toplevel_list_v1 *ext_toplevel_list;

    /* Xwayland（src/xwayland） */
    struct wlr_compositor *compositor; // wlr_xwayland_create 需要
    struct wlr_xwayland *xwayland;
    struct wl_list xwayland_unmanaged;              // override-redirect 窗口链表头
    struct wlr_scene_tree *xwayland_unmanaged_tree; // 浮在普通视图之上
    struct wl_listener new_xwayland_surface;
    struct wl_listener xwayland_ready;

    /*
     * 调试接口（src/ipc）：unix socket 上的行分隔 JSON。
     *
     * 路径在 start 阶段由 wayland socket 名推出，并导出到环境变量
     * WOODLAND_IPC，脚本/测试不必自己拼。
     */
    char *ipc_path;
    int ipc_fd;
    struct wl_event_source *ipc_source;
    struct wl_list ipc_clients; // struct woodland_ipc_client.link

    /*
     * 不变量自检（src/selfcheck）。
     *
     * self_check 由 --self-check 打开；打开后每次状态变化都会在 idle 阶段
     * 跑一遍断言，失败只记日志（不退出：调试期崩掉合成器比看到报警更糟）。
     */
    bool self_check;
    struct wl_event_source *selfcheck_idle;
    const char *selfcheck_reason;

    /* 视图 id：调试接口里的稳定标识（指针每次运行都变，没法写断言） */
    uint32_t next_view_id;

    const char *startup_cmd;        // -s 启动命令
    const char *config_path;        // -c 配置文件路径
    struct woodland_config *config; // 运行时配置
};

#endif
