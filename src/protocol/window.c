// SPDX-License-Identifier: GPL-2.0-only
/*
 * 窗口族：xdg-activation（把已有窗口拽到前台）与 xdg-toplevel-tag（窗口
 * 自带的分组标签）。
 *
 * 两者都只作用于"已经存在的视图"，不改变生命周期，也不碰几何：激活就是
 * 一次焦点切换，tag 就是一次字段赋值。跟 shell.c 的关系是单向的——本文件
 * 调 shell 的公开接口，shell 不反过来依赖这里。
 */
#include <stdlib.h>

#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "protocol_internal.h"
#include "view.h"
#include "woodland.h"

struct woodland_proto_window {
    struct woodland_server *server;

    struct wlr_xdg_activation_v1 *activation;
    struct wl_listener activate;

    struct wlr_xdg_toplevel_tag_manager_v1 *toplevel_tag;
    struct wl_listener set_tag;
};

/*
 * 这里只处理"已经存在的窗口被要求置顶/聚焦"（点通知栏、从浏览器拉起
 * 已开的窗口）。启动器拉起新窗口的情形不需要处理：map 路径本来就会
 * 聚焦新窗口。
 *
 * token 里的 app_id 提示没被消费——要按 app_id 找窗口得等窗口规则
 * （M6）落地，现在硬猜只能猜错。
 */
static void window_handle_activate(struct wl_listener *listener, void *data) {
    struct woodland_proto_window *window =
        wl_container_of(listener, window, activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;

    struct woodland_view *view = woodland_view_from_surface(event->surface);
    if (!view) {
        /* layer surface / 尚未建视图的 surface：不抢焦点 */
        return;
    }
    woodland_focus_view(view, true);
    (void)window;
}

static void window_handle_set_tag(struct wl_listener *listener, void *data) {
    struct woodland_proto_window *window =
        wl_container_of(listener, window, set_tag);
    struct wlr_xdg_toplevel_tag_manager_v1_set_tag_event *event = data;

    struct woodland_view *view =
        woodland_view_from_surface(event->toplevel->base->surface);
    if (!view) {
        return;
    }
    /* 只记录，不参与规则匹配：窗口规则还没做，存下来至少能在 wctl 里看到 */
    woodland_view_set_tag(view, event->tag);
    (void)window;
}

bool woodland_proto_window_init(struct woodland_proto *proto) {
    struct woodland_server *server = proto->server;

    struct woodland_proto_window *window = calloc(1, sizeof(*window));
    if (!window) {
        wlr_log(WLR_ERROR, "cannot allocate window protocol state");
        return false;
    }
    window->server = server;
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->window = window;

    window->activation = wlr_xdg_activation_v1_create(server->display);
    if (!window->activation) {
        wlr_log(WLR_ERROR, "failed to create xdg-activation");
        return false;
    }
    window->activate.notify = window_handle_activate;
    wl_signal_add(&window->activation->events.request_activate,
                  &window->activate);

    window->toplevel_tag = wlr_xdg_toplevel_tag_manager_v1_create(
        server->display, 1);
    if (!window->toplevel_tag) {
        wlr_log(WLR_ERROR, "failed to create xdg-toplevel-tag");
        return false;
    }
    window->set_tag.notify = window_handle_set_tag;
    wl_signal_add(&window->toplevel_tag->events.set_tag, &window->set_tag);

    return true;
}

void woodland_proto_window_finish(struct woodland_proto *proto) {
    struct woodland_proto_window *window = proto->window;
    if (!window) {
        return;
    }
    if (window->activate.link.prev) {
        wl_list_remove(&window->activate.link);
    }
    if (window->set_tag.link.prev) {
        wl_list_remove(&window->set_tag.link);
    }
    free(window);
    proto->window = NULL;
}
