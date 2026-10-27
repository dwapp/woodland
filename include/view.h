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
struct wlr_scene_surface;
struct wlr_xwayland_surface;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_image_capture_source_v1;

/** 交互式光标模式（tinywl 语义：移动/缩放/穿越）。 */
enum woodland_cursor_mode {
    WOODLAND_CURSOR_PASSTHROUGH,
    WOODLAND_CURSOR_MOVE,
    WOODLAND_CURSOR_RESIZE,
};

/** 视图后端：Wayland 原生（xdg-shell）或 X11（Xwayland）。 */
enum woodland_view_type {
    WOODLAND_VIEW_XDG = 0,
    WOODLAND_VIEW_XWAYLAND,
};

struct woodland_view {
    struct wl_list link;               // woodland_server.views
    struct woodland_server *server;
    enum woodland_view_type type;
    struct wlr_xdg_toplevel *xdg_toplevel;         // type == XDG 时有效
    struct wlr_xwayland_surface *xwayland_surface; // type == XWAYLAND 时有效
    /* X11 视图的 surface 节点：associate 时建、dissociate 时清（xdg 由 shell 自带） */
    struct wlr_scene_surface *scene_surface;
    bool override_redirect;            // X11 覆盖重定向窗口：无装饰、不聚焦、不进窗口列表
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
    /* ext-foreign-toplevel-list-v1 的句柄（截图/任务栏客户端用） */
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_toplevel_handle;
    struct wl_listener ext_handle_destroy;
    /* 按窗口取源时惰性创建的捕获源，绑定 view->scene_tree */
    struct wlr_ext_image_capture_source_v1 *capture_source;
    struct wl_listener capture_source_destroy;
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
    /* Xwayland 独有事件（type == XWAYLAND 时才接线） */
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map_request;      // xsurface->events.map_request
    struct wl_listener request_configure;
    struct wl_listener request_activate;
    struct wl_listener request_close;
    struct wl_listener set_class;
    struct wl_listener set_geometry;
};

/* ==================== 双后端访问器 ==================== */

/*
 * 下面几个函数把「xdg toplevel 还是 xwayland surface」的差异收在一处，
 * shell/ssd/seat/layer 都只走这些接口，不再直接摸 view->xdg_toplevel。
 */

/** 视图当前的 wlr_surface（未 associate / 已销毁时为 NULL）。 */
struct wlr_surface *woodland_view_surface(const struct woodland_view *view);

/** 内容矩形（相对 scene_tree 原点）。xdg 取 base->geometry，X11 取窗口尺寸、原点 0,0。 */
void woodland_view_content_box(const struct woodland_view *view, struct wlr_box *box);

/** 请求客户端调整内容尺寸（xdg set_size / X11 configure）。 */
void woodland_view_request_size(struct woodland_view *view, int width, int height);

/** 设置激活态（xdg set_activated / X11 _NET_WM_STATE + 输入焦点）。 */
void woodland_view_set_activated(struct woodland_view *view, bool activated);

/** 请求客户端关闭窗口（标题栏关闭按钮）。 */
void woodland_view_close(struct woodland_view *view);

/** 由 wlr_surface 反查视图（可能是 xdg toplevel 或 xwayland surface）。 */
struct woodland_view *woodland_view_from_surface(struct wlr_surface *surface);

/* ==================== 视图生命周期（两个后端共用） ==================== */

/** 窗口 map：归入工作区、建 SSD、首次放置、聚焦。 */
void woodland_view_map(struct woodland_view *view);

/** 首次放置：transient 相对父窗口居中，其余按 SSD 边距层叠（只摆位置）。 */
void woodland_view_place_initial(struct woodland_view *view);

/** 窗口 unmap：清指针状态、销毁 SSD、摘出 views 链表。 */
void woodland_view_unmap(struct woodland_view *view);

/** 释放视图（各后端先摘掉自己的 listener 再调这里）。 */
void woodland_view_destroy(struct woodland_view *view);

/** 创建 foreign-toplevel / ext-foreign-toplevel-list 句柄（两个后端共用）。 */
void woodland_view_init_handles(struct woodland_view *view);

/** 开始交互式移动/缩放。 */
void woodland_view_begin_interactive(struct woodland_view *view,
                                    enum woodland_cursor_mode mode,
                                    uint32_t edges);

void woodland_view_set_maximized(struct woodland_view *view, bool maximized);
void woodland_view_set_fullscreen(struct woodland_view *view, bool fullscreen,
                                  struct wlr_output *output);
void woodland_view_set_minimized(struct woodland_view *view, bool minimized);

/** 更新标题/应用标识（内部拷一份，不接管入参所有权）。 */
void woodland_view_set_title(struct woodland_view *view, const char *title);
void woodland_view_set_app_id(struct woodland_view *view, const char *app_id);

/** 设置父视图（transient 对话框相对父窗口居中）。 */
void woodland_view_set_parent(struct woodland_view *view,
                              struct woodland_view *parent);

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
