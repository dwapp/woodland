// SPDX-License-Identifier: GPL-2.0-only
/*
 * 撕裂控制族（wp_tearing_control_manager_v1）。
 *
 * 允许客户端（如游戏、低延迟应用）对其表面请求异步页面翻转（async page flip）以允许撕裂。
 * 协议对象在 surface 提交时通过 set_hint 信号将 tearing_hint (vsync / async) 同步到视图，
 * 渲染循环依据当前焦点视图的 hint 及配置决定是否在 output commit 时置位 tearing_page_flip。
 */
#include "util/listener.h"
#include <stdlib.h>

#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>

#include "protocol_internal.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"


static void handle_controller_set_hint(struct wl_listener *listener, void *data)
{
    struct woodland_tearing_controller *controller =
        wl_container_of(listener, controller, set_hint);
    (void)data;

    struct woodland_view *view =
        woodland_view_from_surface(controller->tearing_control->surface);
    if (view) {
        view->tearing_hint = (controller->tearing_control->current ==
                              WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
                                 ? WOODLAND_TEARING_ASYNC
                                 : WOODLAND_TEARING_VSYNC;
        woodland_trace_add("tearing", view->id, "hint=%s",
                           view->tearing_hint == WOODLAND_TEARING_ASYNC ? "async" : "vsync");
    }
}

static void handle_controller_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_tearing_controller *controller =
        wl_container_of(listener, controller, destroy);
    (void)data;

    struct woodland_view *view =
        woodland_view_from_surface(controller->tearing_control->surface);
    if (view) {
        view->tearing_hint = WOODLAND_TEARING_VSYNC;
    }

    wl_list_remove(&controller->link);
    wl_list_remove(&controller->set_hint.link);
    wl_list_remove(&controller->destroy.link);
    free(controller);
}

static void handle_new_object(struct wl_listener *listener, void *data)
{
    struct woodland_proto_tearing *tearing =
        wl_container_of(listener, tearing, new_object);
    struct wlr_tearing_control_v1 *tearing_control = data;

    struct woodland_tearing_controller *controller = calloc(1, sizeof(*controller));
    if (!controller) {
        wlr_log(WLR_ERROR, "failed to allocate tearing controller");
        return;
    }

    controller->tearing = tearing;
    controller->tearing_control = tearing_control;
    wl_list_insert(&tearing->controllers, &controller->link);

    woodland_listener_connect(&controller->set_hint, &tearing_control->events.set_hint,
                              handle_controller_set_hint, "tearing_control.set_hint");
    woodland_listener_connect(&controller->destroy, &tearing_control->events.destroy,
                              handle_controller_destroy, "tearing_control.destroy");

    struct woodland_view *view =
        woodland_view_from_surface(tearing_control->surface);
    if (view) {
        view->tearing_hint = (tearing_control->current ==
                              WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
                                 ? WOODLAND_TEARING_ASYNC
                                 : WOODLAND_TEARING_VSYNC;
    }
}

static void handle_view_mapped(struct wl_listener *listener, void *data)
{
    struct woodland_proto_tearing *tearing =
        wl_container_of(listener, tearing, view_mapped);
    struct woodland_view *view = data;
    if (!view) {
        return;
    }

    struct wlr_surface *surface = woodland_view_surface(view);
    if (surface && tearing->manager) {
        enum wp_tearing_control_v1_presentation_hint hint =
            wlr_tearing_control_manager_v1_surface_hint_from_surface(tearing->manager, surface);
        view->tearing_hint = (hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
                                 ? WOODLAND_TEARING_ASYNC
                                 : WOODLAND_TEARING_VSYNC;
    }
}

bool woodland_proto_tearing_init(struct woodland_proto *proto)
{
    struct woodland_proto_tearing *tearing = calloc(1, sizeof(*tearing));
    if (!tearing) {
        wlr_log(WLR_ERROR, "cannot allocate tearing protocol state");
        return false;
    }

    tearing->server = proto->server;
    wl_list_init(&tearing->controllers);

    tearing->manager = wlr_tearing_control_manager_v1_create(proto->server->display, 1);
    if (!tearing->manager) {
        wlr_log(WLR_ERROR, "failed to create wp_tearing_control_manager_v1");
        free(tearing);
        return false;
    }

    woodland_listener_connect(&tearing->new_object, &tearing->manager->events.new_object,
                              handle_new_object, "tearing.new_object");

    if (proto->server->view_mapped.listener_list.next) {
        woodland_listener_connect(&tearing->view_mapped, &proto->server->view_mapped,
                                  handle_view_mapped, "tearing.view_mapped");
        tearing->watching_views = true;
    } else {
        wlr_log(WLR_DEBUG, "tearing: view_mapped signal not initialised, skip view watching");
    }
    proto->tearing = tearing;
    return true;
}

void woodland_proto_tearing_finish(struct woodland_proto *proto)
{
    if (!proto || !proto->tearing) {
        return;
    }

    struct woodland_proto_tearing *tearing = proto->tearing;

    struct woodland_tearing_controller *controller, *tmp;
    wl_list_for_each_safe(controller, tmp, &tearing->controllers, link)
    {
        wl_list_remove(&controller->link);
        wl_list_remove(&controller->set_hint.link);
        wl_list_remove(&controller->destroy.link);
        free(controller);
    }

    if (tearing->watching_views) {
        wl_list_remove(&tearing->view_mapped.link);
        tearing->watching_views = false;
    }

    wl_list_remove(&tearing->new_object.link);
    free(tearing);
    proto->tearing = NULL;
}
