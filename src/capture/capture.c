// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/log.h>

#include "capture.h"
#include "view.h"
#include "woodland.h"

/* 捕获源随它绑定的 scene 节点销毁（wlroots 内部监听节点 destroy），
 * 这里只负责把 view 上的指针清掉，避免视图销毁时访问已释放的源。 */
static void handle_capture_source_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, capture_source_destroy);
    wl_list_remove(&view->capture_source_destroy.link);
    view->capture_source = NULL;
    (void)data;
}

/* 客户端要抓某个窗口：首次请求时惰性建源，之后复用同一个源（同一窗口的
 * 多个截图客户端共享）。源绑定 view->scene_tree，抓的是「窗口 + SSD 装饰」
 * 整棵子树——woodland 没有像 labwc 那样额外维护一棵无装饰的 capture 场景树。 */
static void handle_toplevel_capture_request(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, toplevel_capture_request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;

    struct woodland_view *view = request->toplevel_handle->data;
    if (!view) {
        wlr_log(WLR_DEBUG, "toplevel capture request for unknown handle");
        return;
    }

    if (!view->capture_source) {
        view->capture_source =
            wlr_ext_image_capture_source_v1_create_with_scene_node(
                &view->scene_tree->node, server->event_loop,
                server->allocator, server->renderer);
        if (!view->capture_source) {
            wlr_log(WLR_ERROR, "failed to create scene capture source");
            return;
        }
        woodland_listener_connect(&view->capture_source_destroy, &view->capture_source->events.destroy,
                          handle_capture_source_destroy, "capture.destroy");
    }

    if (!wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
            request, view->capture_source)) {
        wlr_log(WLR_ERROR, "failed to accept toplevel capture request");
    }
}

bool woodland_capture_init(struct woodland_server *server) {
    /* wlr_screencopy_v1 不提供事件：客户端 attach 缓冲后锁住输出的
     * attach_render 并请求一帧，我们的帧回调正常渲染提交，wlroots 在
     * output->events.commit 里把内容拷进客户端缓冲。 */
    server->screencopy_manager = wlr_screencopy_manager_v1_create(server->display);
    if (!server->screencopy_manager) {
        wlr_log(WLR_ERROR, "failed to create screencopy manager");
        return false;
    }

    server->image_copy_capture_manager =
        wlr_ext_image_copy_capture_manager_v1_create(server->display, 1);
    if (!server->image_copy_capture_manager) {
        wlr_log(WLR_ERROR, "failed to create image copy capture manager");
        return false;
    }

    server->output_capture_source_manager =
        wlr_ext_output_image_capture_source_manager_v1_create(server->display, 1);
    if (!server->output_capture_source_manager) {
        wlr_log(WLR_ERROR, "failed to create output capture source manager");
        return false;
    }

    server->toplevel_capture_manager =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
            server->display, 1);
    if (!server->toplevel_capture_manager) {
        wlr_log(WLR_ERROR, "failed to create toplevel capture source manager");
        return false;
    }

    woodland_listener_connect(&server->toplevel_capture_request, &server->toplevel_capture_manager->events.new_request,
                          handle_toplevel_capture_request, "capture.new_request");

    return true;
}

void woodland_capture_finish(struct woodland_server *server) {
    if (server->toplevel_capture_request.link.prev) {
        wl_list_remove(&server->toplevel_capture_request.link);
    }
}

void woodland_capture_view_finish(struct woodland_view *view) {
    if (!view->capture_source) {
        return;
    }
    /* 源自己监听 scene 节点 destroy，节点随 xdg surface 销毁时释放；
     * 这里只摘掉回调，别让它在 view 释放后再摸内存。 */
    wl_list_remove(&view->capture_source_destroy.link);
    view->capture_source = NULL;
}
