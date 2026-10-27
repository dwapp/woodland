// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_VIEW_H
#define WOODLAND_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct woodland_server;
struct woodland_workspace;
struct woodland_ssd;
struct woodland_ssd_margins;
struct wlr_surface;
struct wlr_output;

struct woodland_view {
    struct wl_list link;               // woodland_server.views
    struct woodland_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct woodland_workspace *workspace;   // 归属工作区
    struct woodland_view *parent;           // 父视图（transient 对话框）
    bool mapped;
    bool placed;                       // 是否已做过首次放置（remap 不再重摆）
    bool minimized;
    bool maximized;
    bool fullscreen;
    char *title;
    char *app_id;
    struct woodland_ssd *ssd;          // 服务端装饰（默认 SSD，见设计）
    bool deco_csd;                     // 客户端请求 CSD（map 后不建 SSD）
    bool deco_registered;              // 是否已注册 zxdg_decoration listener
    struct wlr_xdg_toplevel_decoration_v1 *deco; // zxdg decoration 对象
    struct wl_listener deco_destroy;   // zxdg_decoration 销毁监听器
    struct wl_listener deco_request_mode; // zxdg_decoration 模式请求监听器
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

/**
 * 计算新窗口的内容原点（布局坐标）。
 *
 * area 为可用工作区，content_w/h 为客户端内容尺寸，margins 为 SSD 外扩，
 * cascade 为层叠偏移像素。外框（内容 + margins）整体落在 area 内，
 * 这样标题栏不会被输出边缘裁掉。
 */
void woodland_view_placement(const struct wlr_box *area,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             int cascade, struct wlr_box *out);

/** 贴边吸附阈值（px）：外框边缘距目标线不超过该值即吸附（labwc screen_edge_strength 默认 20）。 */
#define WOODLAND_SNAP_DISTANCE 20

/**
 * 单轴贴边吸附（纯函数）。
 *
 * desired/size：移动中外框在该轴的起点与长度；[lo, hi] 是工作区在该轴的范围；
 * edges/n_edges 是其它窗口外框在该轴的边坐标；threshold 是吸附阈值。
 *
 * 候选位置 = 工作区两端（起点贴 lo、末边贴 hi）+ 每条 edge 的"贴上去"(edge)
 * 与"贴回来"(edge - size)，取位移最小且在阈值内者。外框比工作区还大时不吸附。
 */
int woodland_snap_axis(int desired, int size, int lo, int hi,
                       const int *edges, int n_edges, int threshold);

/**
 * transient 相对父窗口居中（纯函数，语义同 labwc view_compute_centered_position）。
 *
 * 外框（内容 + margins）以 parent_box 为中心；越出 area 时贴回 area 内侧，
 * 中心点也落不进 area 时贴左上。out 为内容原点。
 */
void woodland_view_center_on(const struct wlr_box *parent_box,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             const struct wlr_box *area, struct wlr_box *out);

#endif
