// SPDX-License-Identifier: GPL-2.0-only
/*
 * 焦点管理：统一聚焦入口、焦点回退、z 序循环、layer 让位、模态对话框接管。
 *
 * 模型对齐 labwc：
 *   - server->views 是 z 序链表（最上在前），聚焦时 raise 到表头；
 *   - 焦点回退 = 取 z 序最上的可聚焦视图（labwc desktop_focus_topmost_view）；
 *   - 焦点循环沿 z 序前进/后退（labwc cycle 的简化版，不做 OSD 预览）。
 * 把"z 序"和"焦点"放在同一条链表上是有意为之：两者在平铺/层叠桌面里
 * 本来就强相关，分开维护反而要处理两份顺序不同步的问题。
 */
#include "config.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/util/log.h>
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "focus.h"
#include "selfcheck.h"
#include "session_lock.h"
#include "ssd.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

/* 父链/子链遍历的深度上限：防御环状 parent 关系导致死循环 */
#define FOCUS_CHAIN_MAX 8
/* 循环切换的候选上限 */
#define FOCUS_CYCLE_MAX 64

/* ==================== 基础判断 ==================== */

bool woodland_view_focusable(const struct woodland_view *view)
{
    if (!view || !view->server) {
        return false;
    }
    return view->mapped && !view->minimized && !view->override_redirect &&
           view->workspace == view->server->active_workspace;
}

struct woodland_view *woodland_focus_current(struct woodland_server *server)
{
    if (!server) {
        return NULL;
    }
    /* 显式记录的焦点优先：没有键盘设备时 focused_surface 恒为 NULL，
     * 只靠它反查会丢掉"谁是焦点"这个状态 */
    if (server->focused_view && woodland_view_focusable(server->focused_view)) {
        return server->focused_view;
    }
    if (!server->seat) {
        return NULL;
    }
    return woodland_view_from_surface(server->seat->keyboard_state.focused_surface);
}

/*
 * 取"已记录的焦点视图"，即使它当前已经不可聚焦。
 *
 * 典型场景：工作区切换已经改了 active_workspace，旧焦点视图因为不在
 * 新工作区而 focusable 为假；headless 下又没有键盘设备，反查不到任何
 * surface，woodland_focus_current() 于是返回 NULL。这时候如果按它来清
 * 焦点，旧视图的 activated / SSD / foreign-toplevel 就永远留在激活态，
 * 下一次聚焦会同时出现两个激活窗口。
 *
 * 清外观要看"我们记着谁"，不是"谁现在还能聚焦"。
 */
static struct woodland_view *focus_recorded(struct woodland_server *server)
{
    struct woodland_view *view = woodland_focus_current(server);
    if (!view) {
        view = server->focused_view;
    }
    return view;
}

static void ft_set_activated(struct woodland_view *view, bool activated)
{
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_activated(view->toplevel_handle, activated);
    }
}

/*
 * 让一个视图进入"激活"外观：客户端 activated 状态 + SSD 激活色 +
 * foreign-toplevel 状态。三处必须一起改，否则会出现"标题栏是灰的但
 * 键盘输入在那"的错位。
 */
static void view_set_focused_look(struct woodland_view *view, bool focused)
{
    woodland_view_set_activated(view, focused);
    woodland_ssd_set_active(view, focused);
    ft_set_activated(view, focused);
}

/* ==================== 模态对话框 ==================== */

static bool view_is_modal(const struct woodland_view *view)
{
    if (view->type == WOODLAND_VIEW_XDG && view->xdg_toplevel) {
        /* xdg-dialog-v1：GTK4/Qt 的模态对话框走这个扩展 */
        struct wlr_xdg_dialog_v1 *dialog =
            wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(view->xdg_toplevel);
        return dialog != NULL && dialog->modal;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND && view->xwayland_surface) {
        return view->xwayland_surface->modal;
    }
#endif
    return false;
}

struct woodland_view *woodland_focus_modal_dialog(struct woodland_view *view)
{
    if (!view || !view->server) {
        return view;
    }
    if (view_is_modal(view)) {
        return view;
    }

    /* 找到 transient 树的根：模态关系是同组窗口之间的 */
    struct woodland_view *root = view;
    for (int i = 0; i < FOCUS_CHAIN_MAX && root->parent; i++) {
        root = root->parent;
    }

    /*
     * 在 z 序里找 root 的子孙中第一个模态窗口（表头最上，先命中即最上层）。
     * 这样点父窗口任意位置，焦点都会落到它当前打开的模态对话框上。
     */
    struct woodland_view *it;
    wl_list_for_each(it, &view->server->views, link)
    {
        if (!it->mapped || it->minimized || !it->parent) {
            continue;
        }
        struct woodland_view *p = it->parent;
        for (int d = 0; d < FOCUS_CHAIN_MAX && p; d++) {
            if (p == root) {
                if (view_is_modal(it)) {
                    return it;
                }
                break;
            }
            p = p->parent;
        }
    }
    return view;
}

/* ==================== 聚焦入口 ==================== */

static void view_raise(struct woodland_view *view)
{
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&view->server->views, &view->link);
}

void woodland_focus_view(struct woodland_view *view, bool raise)
{
    if (!view || !view->server) {
        return;
    }
    struct woodland_server *server = view->server;
    if (woodland_session_lock_is_locked(server)) {
        return;
    }

    if (view->override_redirect || !view->mapped) {
        return;
    }

    if (view->minimized) {
        /* 恢复会重新 map，map 路径会再次调用本函数（届时已可聚焦） */
        woodland_view_set_minimized(view, false);
        return;
    }

    /* 不在当前工作区：先切过去（清焦点，稍后由本次聚焦覆盖） */
    if (view->workspace && view->workspace != server->active_workspace) {
        woodland_workspace_switch_focus(server, view->workspace, false);
    }

    /* 模态子窗口优先 */
    view = woodland_focus_modal_dialog(view);
    if (!view || !woodland_view_focusable(view)) {
        return;
    }

    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface) {
        return;
    }

    struct wlr_seat *seat = server->seat;
    struct woodland_view *prev = focus_recorded(server);
    bool already = (prev == view) && (seat->keyboard_state.focused_surface == surface);

    /* toplevel 拿到焦点即 layer 失焦 */
    server->focused_layer = NULL;

    if (prev && prev != view) {
        view_set_focused_look(prev, false);
    }
    if (!already) {
        view_set_focused_look(view, true);
    } else {
        /* 已聚焦但仍要校正外观（例如刚从 layer 焦点退回来） */
        woodland_ssd_set_active(view, true);
        ft_set_activated(view, true);
    }

    if (raise) {
        view_raise(view);
    }
    server->focused_view = view;
    woodland_trace_add("focus", view->id, "prev=%u raise=%d", prev ? prev->id : 0, raise ? 1 : 0);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
    woodland_selfcheck_schedule(server, "focus");
}

void woodland_focus_none(struct woodland_server *server)
{
    if (!server || !server->seat) {
        return;
    }
    struct woodland_view *prev = focus_recorded(server);
    if (prev) {
        view_set_focused_look(prev, false);
    }
    server->focused_view = NULL;
    server->focused_layer = NULL;
    /* prev 的 id 已经在 obj 里；没有额外的决策依据，别在 detail 里重复一遍 */
    woodland_trace_add("focus_none", prev ? prev->id : 0, NULL);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    woodland_selfcheck_schedule(server, "focus_none");
}

void woodland_focus_yield_to_layer(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    struct woodland_view *prev = focus_recorded(server);
    if (prev) {
        view_set_focused_look(prev, false);
    }
    /* 注意力已经交给 layer，toplevel 不再算"焦点视图" */
    server->focused_view = NULL;
    /* obj 已经记了被让出焦点的视图，没有别的依据可写 */
    woodland_trace_add("layer_focus", prev ? prev->id : 0, NULL);
}

struct woodland_view *woodland_focus_topmost(struct woodland_server *server)
{
    if (!server) {
        return NULL;
    }
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link)
    {
        if (woodland_view_focusable(it)) {
            return it;
        }
    }
    return NULL;
}

struct woodland_view *woodland_focus_pick(struct woodland_server *server,
                                          struct woodland_view *from, bool forward)
{
    if (!server) {
        return NULL;
    }
    struct woodland_view *list[FOCUS_CYCLE_MAX];
    int n = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link)
    {
        if (n >= FOCUS_CYCLE_MAX) {
            break;
        }
        if (woodland_view_focusable(it)) {
            list[n++] = it;
        }
    }
    if (n == 0) {
        return NULL;
    }
    if (n == 1) {
        return list[0];
    }
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (list[i] == from) {
            idx = i;
            break;
        }
    }
    int next;
    if (idx < 0) {
        next = forward ? 0 : n - 1;
    } else {
        next = (idx + (forward ? 1 : n - 1)) % n;
    }
    return list[next];
}

void woodland_focus_fallback(struct woodland_server *server)
{
    if (!server || !server->seat) {
        return;
    }

    /* layer 还活着且仍要键盘：把焦点重新交给它（别抢回 toplevel） */
    if (server->focused_layer) {
        struct wlr_surface *lsurface = server->focused_layer->surface;
        if (lsurface && lsurface->mapped) {
            struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
            if (keyboard != NULL) {
                wlr_seat_keyboard_notify_enter(server->seat, lsurface, keyboard->keycodes,
                                               keyboard->num_keycodes, &keyboard->modifiers);
            }
            return;
        }
        server->focused_layer = NULL;
    }

    struct woodland_view *best = woodland_focus_topmost(server);
    if (best) {
        woodland_focus_view(best, false);
    } else {
        woodland_focus_none(server);
    }
}

void woodland_focus_cycle(struct woodland_server *server, bool forward)
{
    if (!server) {
        return;
    }
    /* 先按快照算目标再聚焦：focus_view 会改 z 序，不能边遍历边算 */
    struct woodland_view *next =
        woodland_focus_pick(server, woodland_focus_current(server), forward);
    if (next) {
        woodland_focus_view(next, true);
    }
}
