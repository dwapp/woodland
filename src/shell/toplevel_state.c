// SPDX-License-Identifier: GPL-2.0-only
/*
 * 窗口状态：最大化、全屏、最小化、标题、app_id、parent，以及
 * foreign-toplevel / ext-foreign-toplevel-list 句柄。
 *
 * 这些是同一件事的三层：
 *   1. 状态标志写在 view 上（合成器自己认的那份真值）；
 *   2. 同步给客户端（xdg 走 configure，X11 走 _NET_WM_STATE）；
 *   3. 同步给任务栏（foreign-toplevel 句柄的 set_*）。
 * 任何一处漏掉，都会出现"窗口铺满了但任务栏还显示普通尺寸"这类割裂。
 *
 * 几何只从 placement.c 取（最大化框、全屏框），这里负责落位与状态广播。
 */
#include "util/listener.h"
#include <stdlib.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "focus.h"
#include "output.h"
#include "layer.h"
#include "placement.h"
#include "shell_internal.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"

/* ==================== 几何落位 ==================== */

static void view_save_geometry(struct woodland_view *view)
{
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    view->saved_geometry.x = view->scene_tree->node.x;
    view->saved_geometry.y = view->scene_tree->node.y;
    view->saved_geometry.width = content.width;
    view->saved_geometry.height = content.height;
}

void shell_view_apply_geometry(struct woodland_view *view, const struct wlr_box *box)
{
    /* 先落位置再改尺寸：X11 的 configure 会把当前位置一并发给客户端 */
    wlr_scene_node_set_position(&view->scene_tree->node, box->x, box->y);
    woodland_view_request_size(view, box->width, box->height);
    woodland_view_update_outputs(view);
}

/* X11 的 configure 已由 woodland_view_request_size 直接发出，无需再排程 */
void shell_view_schedule_configure(struct woodland_view *view)
{
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        return;
    }
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

/* ==================== 状态标志的协议侧同步 ==================== */

void shell_view_apply_maximized_state(struct woodland_view *view, bool maximized)
{
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface, maximized, maximized);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
}

static void view_apply_fullscreen_state(struct woodland_view *view, bool fullscreen)
{
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, fullscreen);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
}

/* ==================== 最大化 / 全屏 / 最小化 ==================== */

void woodland_view_set_maximized(struct woodland_view *view, bool maximized)
{
    if (maximized == view->maximized) {
        return;
    }
    struct woodland_server *server = view->server;
    if (maximized) {
        /* 先退出 fullscreen（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->fullscreen) {
            view->fullscreen = false;
            shell_view_apply_geometry(view, &view->saved_geometry);
            view_apply_fullscreen_state(view, false);
        }
        view_save_geometry(view);
        view->maximized = true;
        /* 最大化取"这个窗口所在输出"的工作区，多输出下才不会被拽到主屏 */
        struct wlr_box box =
            woodland_view_maximized_geometry_on(server, woodland_output_for_view(view));
        /* 最大化保留标题栏，内容让出 titlebar 高度 */
        woodland_view_apply_outer_geometry(view, &box);
        shell_view_apply_maximized_state(view, true);
    } else {
        view->maximized = false;
        struct wlr_box box = view->saved_geometry;
        shell_view_apply_geometry(view, &box);
        shell_view_apply_maximized_state(view, false);
    }
    shell_view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(view->toplevel_handle, view->maximized);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(view->toplevel_handle, view->fullscreen);
    }
    woodland_view_update_outputs(view);
    woodland_signal_emit(&view->server->view_changed, view);
}

void woodland_view_set_fullscreen(struct woodland_view *view, bool fullscreen,
                                  struct wlr_output *output)
{
    if (fullscreen == view->fullscreen) {
        return;
    }
    struct woodland_server *server = view->server;
    if (fullscreen) {
        /* 先退出 maximize（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->maximized) {
            view->maximized = false;
            shell_view_apply_geometry(view, &view->saved_geometry);
            shell_view_apply_maximized_state(view, false);
        }
        view_save_geometry(view);
        view->fullscreen = true;
        if (!output) {
            /* 无指定输出：落到窗口当前所在的输出（多输出下不能固定主屏） */
            struct woodland_output *wo = woodland_output_for_view(view);
            if (!wo) {
                wo = woodland_output_first(server);
            }
            output = wo ? wo->wlr_output : NULL;
        }
        struct wlr_box box = woodland_view_fullscreen_geometry(server, output);
        woodland_view_apply_outer_geometry(view, &box);
        view_apply_fullscreen_state(view, true);
    } else {
        view->fullscreen = false;
        struct wlr_box box = view->saved_geometry;
        shell_view_apply_geometry(view, &box);
        view_apply_fullscreen_state(view, false);
    }
    shell_view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(view->toplevel_handle, view->fullscreen);
        wlr_foreign_toplevel_handle_v1_set_maximized(view->toplevel_handle, view->maximized);
    }
    woodland_view_update_outputs(view);
    woodland_signal_emit(&view->server->view_changed, view);
    woodland_layer_update_top_layer_visibility(view->server);
}

void woodland_view_set_minimized(struct woodland_view *view, bool minimized)
{
    if (minimized == view->minimized) {
        return;
    }
    view->minimized = minimized;
    wlr_scene_node_set_enabled(&view->scene_tree->node, !minimized);
    if (minimized) {
        /* 最小化掉焦点视图：交给焦点回退（挑同工作区下一个窗口），
         * 否则键盘焦点会悬空，用户必须先点一下才能继续输入 */
        struct woodland_view *focused = woodland_focus_current(view->server);
        if (focused == view) {
            woodland_focus_fallback(view->server);
        }
    }
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_minimized(view->toplevel_handle, minimized);
    }
    woodland_signal_emit(&view->server->view_changed, view);
    woodland_layer_update_top_layer_visibility(view->server);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, request_maximize);
    if (!view->xdg_toplevel->base->initialized) {
        return;
    }
    woodland_view_set_maximized(view, view->xdg_toplevel->requested.maximized);
    (void)data;
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, request_fullscreen);
    if (!view->xdg_toplevel->base->initialized) {
        return;
    }
    woodland_view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen,
                                 view->xdg_toplevel->requested.fullscreen_output);
    (void)data;
}
static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, request_minimize);
    woodland_view_set_minimized(view, view->xdg_toplevel->requested.minimized);
    (void)data;
}

/* ==================== 标题 / app_id / parent / tag ==================== */

/* 客户端/合成器侧销毁句柄时清指针，避免视图销毁时二次释放 */
static void ext_toplevel_handle_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ext_handle_destroy);
    wl_list_remove(&view->ext_handle_destroy.link);
    view->ext_toplevel_handle = NULL;
    (void)data;
}

/* ext-foreign-toplevel-list 的句柄状态：只有 title/app_id 两项 */
static void ext_toplevel_sync_state(struct woodland_view *view)
{
    if (!view->ext_toplevel_handle) {
        return;
    }
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = view->title,
        .app_id = view->app_id,
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_toplevel_handle, &state);
}

void woodland_view_set_title(struct woodland_view *view, const char *title)
{
    char *copy = title ? strdup(title) : NULL;
    if (title && !copy) {
        return;
    }
    free(view->title);
    view->title = copy;
    /* wlroots 的 set_title 不接受 NULL（内部 strdup），空标题就不下发 */
    if (view->toplevel_handle && view->title) {
        wlr_foreign_toplevel_handle_v1_set_title(view->toplevel_handle, view->title);
    }
    ext_toplevel_sync_state(view);
    /* 标题变化时刷新 SSD 标题文本 */
    if (view->ssd) {
        woodland_ssd_update(view);
    }
    woodland_signal_emit(&view->server->view_changed, view);
}

void woodland_view_set_tag(struct woodland_view *view, const char *tag)
{
    char *copy = tag ? strdup(tag) : NULL;
    if (tag && !copy) {
        return;
    }
    free(view->tag);
    view->tag = copy;
}

void woodland_view_set_app_id(struct woodland_view *view, const char *app_id)
{
    char *copy = app_id ? strdup(app_id) : NULL;
    if (app_id && !copy) {
        return;
    }
    free(view->app_id);
    view->app_id = copy;
    if (view->toplevel_handle && view->app_id) {
        wlr_foreign_toplevel_handle_v1_set_app_id(view->toplevel_handle, view->app_id);
    }
    ext_toplevel_sync_state(view);
    if (view->ssd) {
        woodland_ssd_update(view);
    }
    woodland_signal_emit(&view->server->view_changed, view);
}

void woodland_view_set_parent(struct woodland_view *view, struct woodland_view *parent)
{
    view->parent = parent;
    /* 同步 foreign-toplevel parent */
    if (view->toplevel_handle) {
        struct wlr_foreign_toplevel_handle_v1 *parent_handle =
            (parent && parent->toplevel_handle) ? parent->toplevel_handle : NULL;
        wlr_foreign_toplevel_handle_v1_set_parent(view->toplevel_handle, parent_handle);
    }
    woodland_signal_emit(&view->server->view_changed, view);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    woodland_view_set_title(view, view->xdg_toplevel->title);
    (void)data;
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, set_app_id);
    woodland_view_set_app_id(view, view->xdg_toplevel->app_id);
    (void)data;
}

static void xdg_toplevel_set_parent(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xdg_toplevel *parent = view->xdg_toplevel->parent;
    struct woodland_view *parent_view = NULL;
    if (parent && parent->base->data) {
        struct wlr_scene_tree *parent_tree = parent->base->data;
        parent_view = parent_tree->node.data;
    }
    woodland_view_set_parent(view, parent_view);
    (void)data;
}

/* ==================== foreign-toplevel 请求 ==================== */

static void ft_request_activate(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ft_request_activate);
    /* 若最小化则先恢复 */
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_focus_view(view, true);
    (void)data;
}

static void ft_request_maximize(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ft_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    woodland_view_set_maximized(view, event->maximized);
}

static void ft_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ft_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    woodland_view_set_fullscreen(view, event->fullscreen, event->output);
}

static void ft_request_minimize(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ft_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    woodland_view_set_minimized(view, event->minimized);
}

static void ft_request_close(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, ft_request_close);
    woodland_view_close(view);
    (void)data;
}

/* foreign-toplevel 与 ext-foreign-toplevel-list 句柄：两个后端共用 */
void woodland_view_init_handles(struct woodland_view *view)
{
    struct woodland_server *server = view->server;

    if (server->foreign_toplevel_manager) {
        view->toplevel_handle =
            wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
        if (view->toplevel_handle) {
            view->toplevel_handle->data = view;
            woodland_listener_connect(&view->ft_request_activate,
                                      &view->toplevel_handle->events.request_activate,
                                      ft_request_activate, "toplevel_state.request_activate");
            woodland_listener_connect(&view->ft_request_maximize,
                                      &view->toplevel_handle->events.request_maximize,
                                      ft_request_maximize, "toplevel_state.request_maximize");
            woodland_listener_connect(&view->ft_request_fullscreen,
                                      &view->toplevel_handle->events.request_fullscreen,
                                      ft_request_fullscreen, "toplevel_state.request_fullscreen");
            woodland_listener_connect(&view->ft_request_minimize,
                                      &view->toplevel_handle->events.request_minimize,
                                      ft_request_minimize, "toplevel_state.request_minimize");
            woodland_listener_connect(&view->ft_request_close,
                                      &view->toplevel_handle->events.request_close,
                                      ft_request_close, "toplevel_state.request_close");
        }
    }

    /* ext-foreign-toplevel-list-v1 句柄：截图/任务栏类客户端（lswt、grim -T）用。
     * handle->data 指回 view，capture 模块据它建「按窗口取源」。 */
    if (server->ext_toplevel_list) {
        struct wlr_ext_foreign_toplevel_handle_v1_state state = {
            .title = view->title,
            .app_id = view->app_id,
        };
        view->ext_toplevel_handle =
            wlr_ext_foreign_toplevel_handle_v1_create(server->ext_toplevel_list, &state);
        if (view->ext_toplevel_handle) {
            view->ext_toplevel_handle->data = view;
            woodland_listener_connect(&view->ext_handle_destroy,
                                      &view->ext_toplevel_handle->events.destroy,
                                      ext_toplevel_handle_destroy, "toplevel_state.destroy");
        }
    }
    woodland_view_update_outputs(view);
}

void woodland_view_update_outputs(struct woodland_view *view)
{
    /*
     * 先广播"窗口状态变了"：所属输出是 foreign-toplevel 的属性之一，而这里
     * 是它唯一会变的地方。放在最前面，下面的早退路径（没有 toplevel 句柄、
     * 没有布局）也不会漏掉通知——协议层自己会重新算它需要的那份。
     */
    if (view && view->server) {
        woodland_signal_emit(&view->server->view_changed, view);
        if (view->fullscreen) {
            woodland_layer_update_top_layer_visibility(view->server);
        }
    }
    if (!view || !view->server || !view->toplevel_handle) {
        return;
    }

    struct woodland_server *server = view->server;
    if (!server->output_layout) {
        return;
    }

    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        if (!wo->wlr_output) {
            continue;
        }
        if (woodland_view_on_output(view, wo->wlr_output)) {
            wlr_foreign_toplevel_handle_v1_output_enter(view->toplevel_handle, wo->wlr_output);
        } else {
            wlr_foreign_toplevel_handle_v1_output_leave(view->toplevel_handle, wo->wlr_output);
        }
    }
}

bool woodland_view_on_output(struct woodland_view *view, struct wlr_output *output)
{
    if (!view || !view->server || !output) {
        return false;
    }
    /* 没 map 的窗口不属于任何输出：它的位置只是"下次摆在哪"的草稿 */
    if (!view->mapped) {
        return false;
    }
    struct wlr_box box = {0};
    woodland_view_outer_box(view, &box);
    if (box.width > 0 && box.height > 0) {
        return view->server->output_layout &&
               wlr_output_layout_intersects(view->server->output_layout, output, &box);
    }
    /* 外框还没算出来（刚 map、客户端还没提交内容）：退到"窗口所属输出"。
     * 不这么做会得到"这个窗口哪个输出都不在"的短暂真空，Dock 那一侧的
     * output_enter 就永远补不上了。 */
    struct woodland_output *wo = woodland_output_for_view(view);
    return wo && wo->wlr_output == output;
}

/* ==================== 监听器注册 ==================== */

void shell_toplevel_state_attach(struct woodland_view *view)
{
    woodland_listener_connect(&view->request_maximize, &view->xdg_toplevel->events.request_maximize,
                              xdg_toplevel_request_maximize, "toplevel_state.request_maximize");
    woodland_listener_connect(&view->request_fullscreen,
                              &view->xdg_toplevel->events.request_fullscreen,
                              xdg_toplevel_request_fullscreen, "toplevel_state.request_fullscreen");
    woodland_listener_connect(&view->request_minimize, &view->xdg_toplevel->events.request_minimize,
                              xdg_toplevel_request_minimize, "toplevel_state.request_minimize");
    woodland_listener_connect(&view->set_title, &view->xdg_toplevel->events.set_title,
                              xdg_toplevel_set_title, "toplevel_state.set_title");
    woodland_listener_connect(&view->set_app_id, &view->xdg_toplevel->events.set_app_id,
                              xdg_toplevel_set_app_id, "toplevel_state.set_app_id");
    woodland_listener_connect(&view->set_parent, &view->xdg_toplevel->events.set_parent,
                              xdg_toplevel_set_parent, "toplevel_state.set_parent");
}
