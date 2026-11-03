// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_VIEW_H
#define WOODLAND_VIEW_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

/* 位置管理与焦点管理的接口都按 struct woodland_view 定义，
 * 统一从这里带出去，调用方只需要 include "view.h"。 */
#include "focus.h"
#include "placement.h"

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
    struct wl_list link; // woodland_server.views
    struct woodland_server *server;
    /*
     * 稳定标识（进程内自增）。调试接口里用它指代窗口：指针每次运行都变，
     * 没法写进断言脚本，而 id 单调递增、可读、也能在日志里对上。
     */
    uint32_t id;
    enum woodland_view_type type;
    struct wlr_xdg_toplevel *xdg_toplevel;         // type == XDG 时有效
    struct wlr_xwayland_surface *xwayland_surface; // type == XWAYLAND 时有效
    /* X11 视图的 surface 节点：associate 时建、dissociate 时清（xdg 由 shell 自带） */
    struct wlr_scene_surface *scene_surface;
    bool override_redirect; // X11 覆盖重定向窗口：无装饰、不聚焦、不进窗口列表
    struct wlr_scene_tree *scene_tree;
    struct woodland_workspace *workspace; // 归属工作区
    struct woodland_view *parent;         // 父视图（transient 对话框）
    bool mapped;
    bool placed; // 是否已做过首次放置（remap 不再重摆）
    bool minimized;
    bool maximized;
    bool fullscreen;
    /*
     * 我们下发给客户端/装饰的激活态。
     *
     * 单独记一份而不是每次去问后端：X11 的 wlr_xwayland_surface 根本没有
     * activated 字段，而"三态同步"（客户端 activated、SSD 颜色、
     * foreign-toplevel 位）需要一个共同的真值，不变量检查也靠它。
     */
    bool activated;
    char *title;
    char *app_id;
    /*
     * xdg-toplevel-tag 的 tag 字符串（客户端自报的窗口类别，如
     * "org.gnome.Nautilus"）。目前只存不用：窗口规则还没做，但存下来
     * 才能经 wctl 看见，也免得协议变成"绑得上但什么都不发生"。
     */
    char *tag;
    struct woodland_ssd *ssd;                    // 服务端装饰（默认 SSD，见设计）
    bool deco_csd;                               // 客户端请求 CSD（map 后不建 SSD）
    bool deco_registered;                        // 是否已注册 zxdg_decoration listener
    struct wlr_xdg_toplevel_decoration_v1 *deco; // zxdg decoration 对象
    struct wl_listener deco_destroy;             // zxdg_decoration 销毁监听器
    struct wl_listener deco_request_mode;        // zxdg_decoration 模式请求监听器
    struct wlr_box saved_geometry;               // 退出 max/fullscreen 时恢复
    /*
     * 贴边铺满（snap to edge）：
     *   snapped_edges       当前贴住的边（WLR_EDGE_* 位掩码，0 = 未贴边）
     *   pending_snap_edges  拖动中"松手就会贴"的边，释放时据此决策
     *   snap_saved_geometry 贴边前的外框，拖出时按它恢复尺寸
     */
    uint32_t snapped_edges;
    uint32_t pending_snap_edges;
    struct wlr_box snap_saved_geometry;
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
    struct wl_listener map_request; // xsurface->events.map_request
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

/** 窗口 unmap：清指针状态、销毁 SSD、摘出 views 链表。 */
void woodland_view_unmap(struct woodland_view *view);

/** 释放视图（各后端先摘掉自己的 listener 再调这里）。 */
void woodland_view_destroy(struct woodland_view *view);

/** 创建 foreign-toplevel / ext-foreign-toplevel-list 句柄（两个后端共用）。 */
void woodland_view_init_handles(struct woodland_view *view);

/** 开始交互式移动/缩放。 */
void woodland_view_begin_interactive(struct woodland_view *view, enum woodland_cursor_mode mode,
                                     uint32_t edges);

void woodland_view_set_maximized(struct woodland_view *view, bool maximized);
void woodland_view_set_fullscreen(struct woodland_view *view, bool fullscreen,
                                  struct wlr_output *output);
void woodland_view_set_minimized(struct woodland_view *view, bool minimized);

/** 更新标题/应用标识（内部拷一份，不接管入参所有权）。 */
void woodland_view_set_title(struct woodland_view *view, const char *title);
void woodland_view_set_app_id(struct woodland_view *view, const char *app_id);
/** 记录 xdg-toplevel-tag（内部拷一份）。传 NULL 清空。 */
void woodland_view_set_tag(struct woodland_view *view, const char *tag);

/** 设置父视图（transient 对话框相对父窗口居中）。 */
void woodland_view_set_parent(struct woodland_view *view, struct woodland_view *parent);

/** 更新窗口在 foreign-toplevel 中的所属显示器（触发 output_enter/output_leave）。 */
void woodland_view_update_outputs(struct woodland_view *view);

bool woodland_shell_init(struct woodland_server *server);
void woodland_shell_finish(struct woodland_server *server);

/** 命中测试：返回 (lx,ly) 处的视图及其 surface 局部坐标。 */
struct woodland_view *woodland_view_at(struct woodland_server *server, double lx, double ly,
                                       struct wlr_surface **surface, double *sx, double *sy);

/** 交互式移动/缩放的指针 motion 处理。 */
void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time);

/** 结束交互式 move/resize。 */
void woodland_cursor_reset_mode(struct woodland_server *server);

/** 接线 cursor 事件监听器（motion/button/axis/frame）。 */
void woodland_cursor_init_events(struct woodland_server *server);
void woodland_cursor_finish_events(struct woodland_server *server);

/*
 * 几何相关的纯函数（放置/吸附/居中/约束/贴边铺满）与焦点管理接口
 * 分别在 placement.h / focus.h 里声明，本头文件已一并 include。
 */

#endif
