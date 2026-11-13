// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_XWAYLAND_H
#define WOODLAND_XWAYLAND_H

#include <stdbool.h>

struct woodland_server;
struct woodland_view;
struct wlr_xwayland_surface;
#include <stdint.h>

/**
 * 初始化 Xwayland 支持。
 *
 * 创建 lazy 模式的 wlr_xwayland（首个 X 客户端连接时才真正拉起 Xwayland 进程），
 * 接上 seat，并监听 new_surface 生成 woodland_view（override-redirect 的走
 * unmanaged 路径：无装饰、不参与焦点和工作区）。
 *
 * 必须在 seat / shell 就绪之后调用（server_init 中位于 shell_init 之后）。
 */
bool woodland_xwayland_init(struct woodland_server *server);

/** 摘掉监听器（display 销毁会连带释放 wlr_xwayland）。 */
void woodland_xwayland_finish(struct woodland_server *server);

/** 更新 X11 窗口的缩放滤波模式（nearest / bilinear / smart） */
void woodland_xwayland_view_update_scale_filter(struct woodland_view *view);

/** 刷新全部 unmanaged 窗口的缩放滤波模式（输出缩放改变时） */
void woodland_xwayland_refresh_unmanaged_filters(struct woodland_server *server);

/** 设置/刷新 Xwayland 根光标（按当前主题与尺寸） */
void woodland_xwayland_update_cursor(struct woodland_server *server);

/** 刷新 Xwayland XWM 客户端连接的出站请求缓冲区。 */
void woodland_xwayland_flush(struct woodland_server *server);

/** 当 X11 视图被提升至最前时，向 X11 侧同步 Z 轴堆叠顺序并刷新。 */
void woodland_xwayland_restack_view(struct woodland_view *view);

/** 提取 X11 客户端的初始位置提示（USPosition/PPosition），有提示返回 true */
bool woodland_xwayland_initial_position_hint(const struct wlr_xwayland_surface *xsurface,
                                            int16_t *x, int16_t *y);

/**
 * 按 X11 window id 把**已映射**的 XWayland 窗口左上角摆到布局坐标 (x,y)。
 *
 * 受管理的顶层窗口（server->views）与 override-redirect 窗口
 * （server->xwayland_unmanaged）都算——Dock 的 XEmbed 托盘图标常是后者。
 * 两处都同步 set_position 与 wlr_xwayland_surface_configure()：只挪 scene
 * 节点会被下一次 X11 set_geometry 弹回去，只 configure 则画面不会立刻动。
 *
 * view_id 写命中的受管理视图 id（unmanaged 时为 0），unmanaged 写是否为
 * override-redirect 窗口；找不到 wid 或窗口尚未映射时返回 false，两个出参
 * 都会被清零（不代表"出错"，协议据此回 callback.done(1)）。
 *
 * 这是 server->xwindow_move_hook 的实现，由本协议族之外的模块**不要**直接调：
 * 编译期关掉 xwayland 时 xwayland.c 根本不参与链接。
 */
bool woodland_xwayland_move_by_wid(struct woodland_server *server, uint32_t wid,
                                   int x, int y, uint32_t *view_id,
                                   bool *unmanaged);

#endif /* WOODLAND_XWAYLAND_H */
