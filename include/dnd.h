// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_DND_H
#define WOODLAND_DND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

/*
 * 拖拽（DnD，wl_data_device.start_drag）的合成器侧。
 *
 * 协议状态机在 wlroots 里：serial 校验、grab、offer、drop 全都不用我们写。
 * 合成器必须自己做的只有三件事，少一件拖拽就是残的：
 *
 *   1. 收到 request_start_drag 时决定起不起（否则客户端一直挂着等）；
 *   2. 每帧算 hit-test 并 notify_enter / notify_motion（否则 drop 没目标）；
 *   3. 自己摆图标（wlroots 建完节点就不管位置了）。
 *
 * 剪贴板（selection）不在这里：那条链已经由 seat.c / protocol/input.c 转发完，
 * 本模块只管拖拽。
 */

struct woodland_server;
struct wlr_drag;
struct wlr_drag_icon;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_surface;

/**
 * 一次拖拽的图标。
 *
 * tree 是 icons 的子节点（不是 icons 本身）：icons 跟着光标走，tree 只承担
 * hotspot 增量（surface->current.dx/dy），两者相加才是图标的屏幕位置。
 * 子树本身由 wlr_scene_drag_icon_create() 建，它自己管 dx/dy 和销毁，
 * 这里只留一个指针 + 一条"子树没了"的监听器把它清空。
 */
struct woodland_dnd_icon {
    struct wlr_scene_tree *tree; /* NULL = 本次拖拽没有图标（客户端可以不设） */
    struct wl_listener tree_destroy;
};

struct woodland_dnd {
    struct woodland_server *server;
    /* scene 根的子节点，init 时建；空闲时 disabled（不渲染、不参与 hit-test） */
    struct wlr_scene_tree *icons;
    struct wlr_drag *drag;            /* NULL = 当前没有拖拽 */
    struct woodland_dnd_icon icon;
    struct {
        struct wl_listener request; /* seat->events.request_start_drag */
        struct wl_listener start;   /* seat->events.start_drag */
        struct wl_listener destroy; /* drag->events.destroy（每次拖拽挂/摘） */
    } events;
};

/** 建图标树并挂 seat 监听器。必须在 wlr_seat 建好后调用。 */
bool woodland_dnd_init(struct woodland_server *server);
void woodland_dnd_finish(struct woodland_server *server);

/** 当前是否正在拖拽。光标层的每条分支都要用它做守卫。 */
bool woodland_dnd_active(const struct woodland_server *server);

/** 挪图标 + 重算投放目标 + 下发 enter/motion。只在 motion 分支里调。 */
void woodland_dnd_motion(struct woodland_server *server, uint32_t time);

/**
 * 取消当前拖拽（锁屏等"用户已经不能继续操作"的场合）。
 *
 * 走 pointer/keyboard grab 的 cancel：wlr_drag 在自己的 cancel 里销毁自己，
 * 收尾（图标树、trace、自检）统一由 destroy 处理器做。
 */
void woodland_dnd_cancel(struct woodland_server *server);

/*
 * 状态机入口。生产路径由 seat 的 request_start_drag / start_drag /
 * drag->destroy 驱动；单独暴露是为了让单测能直接驱动——真的 wlr_drag
 * 需要一个真实客户端的 seat_client，单测里造不出来。
 */
void woodland_dnd_handle_start(struct woodland_server *server, struct wlr_drag *drag);
void woodland_dnd_handle_destroy(struct woodland_server *server, struct wlr_drag *drag);

/**
 * 节点是否落在图标子树里。
 *
 * 投放目标的 hit-test 必须跳过自己：拖拽图标 surface 的输入区域在各工具包
 * 里不一致（有的把 hotspot 设成可命中），一旦命中自己，drop 就永远送不到
 * 真正的窗口。
 */
bool woodland_dnd_node_is_icon(const struct woodland_dnd *dnd, struct wlr_scene_node *node);

#endif /* WOODLAND_DND_H */
