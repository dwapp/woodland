// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SESSION_LOCK_H
#define WOODLAND_SESSION_LOCK_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_session_lock_v1.h>

struct woodland_server;
struct woodland_output;
struct woodland_view;
struct wlr_surface;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct woodland_text_buffer;

struct woodland_session_lock_output {
    struct wl_list link; // woodland_session_lock.lock_outputs
    struct woodland_session_lock *session_lock;
    struct woodland_output *output;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *background;
    struct wlr_scene_tree *surface_tree;
    struct woodland_text_buffer *crash_title;
    struct woodland_text_buffer *crash_sub;
    struct woodland_text_buffer *crash_hint;

    struct wl_listener output_destroy;
    struct wl_listener output_commit;
    struct wl_listener surface_destroy;
    struct wl_listener surface_map;
};

struct woodland_session_lock {
    struct woodland_server *server;
    struct wlr_session_lock_manager_v1 *wlr_manager;

    struct wlr_session_lock_v1 *lock;
    bool locked;

    struct woodland_view *last_focused_view;
    struct wlr_surface *focused_surface;

    struct wl_list lock_outputs; // struct woodland_session_lock_output.link

    /*
     * 锁定状态变化（两边都是"已经落定"之后才 emit，监听方直接读
     * woodland_session_lock_is_locked 就是新状态）。
     *
     * 用信号而不是让 session_lock 去调协议层：协议层挂在它上面（壁纸要按
     * 锁定状态发 pause/play），反过来依赖会绕成一个圈。
     */
    struct wl_signal lock_changed;

    struct wl_listener new_lock;
    struct wl_listener manager_destroy;

    struct wl_listener lock_new_surface;
    struct wl_listener lock_unlock;
    struct wl_listener lock_destroy;
    struct wl_listener layout_change;
};

/**
 * 初始化 ext-session-lock-v1 协议管理器。
 */
bool woodland_session_lock_init(struct woodland_server *server);

/**
 * 释放会话锁模块所有资源与监听器。
 */
void woodland_session_lock_finish(struct woodland_server *server);

/**
 * 当前会话是否处于锁定状态。
 */
bool woodland_session_lock_is_locked(const struct woodland_server *server);

/**
 * 当新输出插入且处于锁定状态时，为新输出创建遮罩与锁屏表面监听。
 */
void woodland_session_lock_add_output(struct woodland_server *server,
                                      struct woodland_output *output);

/**
 * 输出布局/分辨率变动时更新遮罩尺寸与锁屏表面尺寸。
 */
void woodland_session_lock_update_layout(struct woodland_server *server);

/**
 * 窗口销毁时清理会话锁中缓存的 last_focused_view 弱引用。
 */
void woodland_session_lock_handle_view_destroy(struct woodland_server *server,
                                               struct woodland_view *view);

/**
 * 命中测试：查询光标所在坐标下的锁屏表面及局部坐标。
 */
bool woodland_session_lock_surface_at(struct woodland_server *server, double lx, double ly,
                                      struct wlr_surface **surface, double *sx, double *sy);

#endif /* WOODLAND_SESSION_LOCK_H */
