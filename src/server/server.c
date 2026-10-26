// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "cursor.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"

static int handle_terminate(int signal, void *data) {
    struct wl_display *display = data;
    wlr_log(WLR_INFO, "received signal %d, terminating", signal);
    wl_display_terminate(display);
    return 0;
}

static void handle_request_set_shape(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    const char *name = wlr_cursor_shape_v1_name(event->shape);
    woodland_cursor_set(server, name);
}

bool woodland_server_init(struct woodland_server *server) {
    server->display = wl_display_create();
    if (!server->display) {
        wlr_log(WLR_ERROR, "failed to create wayland display");
        return false;
    }

    wl_display_set_default_max_buffer_size(server->display, 1024 * 1024);

    server->event_loop = wl_display_get_event_loop(server->display);

    server->sigint_source = wl_event_loop_add_signal(
        server->event_loop, SIGINT, handle_terminate, server->display);
    server->sigterm_source = wl_event_loop_add_signal(
        server->event_loop, SIGTERM, handle_terminate, server->display);
    if (!server->sigint_source || !server->sigterm_source) {
        wlr_log(WLR_ERROR, "failed to register signal handler");
        return false;
    }
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);

    struct wlr_session *session = NULL;
    server->backend = wlr_backend_autocreate(server->event_loop, &session);
    if (!server->backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return false;
    }

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return false;
    }

    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

    /* 基础协议：compositor 是客户端分配 surface 的必需接口；
     * subcompositor 处理 subsurface；data-device-manager 处理剪贴板。
     * 三者均挂在 display 上，随 display 销毁自动释放。 */
    wlr_compositor_create(server->display, 5, server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_data_device_manager_create(server->display);

    server->scene = wlr_scene_create();
    if (!server->scene) {
        wlr_log(WLR_ERROR, "failed to create scene");
        return false;
    }

    if (!woodland_output_init(server)) {
        wlr_log(WLR_ERROR, "failed to init output");
        return false;
    }

    if (!woodland_cursor_init(server)) {
        wlr_log(WLR_ERROR, "failed to init cursor");
        return false;
    }

    if (!woodland_seat_init(server)) {
        wlr_log(WLR_ERROR, "failed to init seat");
        return false;
    }

    if (!woodland_shell_init(server)) {
        wlr_log(WLR_ERROR, "failed to init shell");
        return false;
    }

    if (!woodland_workspace_init(server)) {
        wlr_log(WLR_ERROR, "failed to init workspace");
        return false;
    }

    woodland_cursor_init_events(server);

    server->cursor_shape_manager =
        wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (!server->cursor_shape_manager) {
        wlr_log(WLR_ERROR, "failed to create cursor-shape manager");
        return false;
    }

    server->request_set_shape.notify = handle_request_set_shape;
    wl_signal_add(&server->cursor_shape_manager->events.request_set_shape,
                  &server->request_set_shape);

    return true;
}

bool woodland_server_start(struct woodland_server *server) {
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to add wayland socket");
        return false;
    }

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return false;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    if (server->startup_cmd) {
        pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "failed to fork for startup command");
            return false;
        } else if (pid == 0) {
            execl("/bin/sh", "/bin/sh", "-c", server->startup_cmd, (void *)NULL);
        }
    }

    return true;
}

void woodland_server_run(struct woodland_server *server) {
    wl_display_run(server->display);
    wl_display_destroy_clients(server->display);
}

void woodland_server_finish(struct woodland_server *server) {
    if (server->sigint_source) {
        wl_event_source_remove(server->sigint_source);
        server->sigint_source = NULL;
    }
    if (server->sigterm_source) {
        wl_event_source_remove(server->sigterm_source);
        server->sigterm_source = NULL;
    }
    if (server->request_set_shape.link.prev) {
        wl_list_remove(&server->request_set_shape.link);
    }
    woodland_cursor_finish_events(server);
    woodland_shell_finish(server);
    woodland_workspace_finish(server);
    woodland_seat_finish(server);
    woodland_cursor_finish(server);
    woodland_output_finish(server);
    if (server->scene) {
        wlr_scene_node_destroy(&server->scene->tree.node);
        server->scene = NULL;
    }
    if (server->allocator) {
        wlr_allocator_destroy(server->allocator);
        server->allocator = NULL;
    }
    if (server->renderer) {
        wlr_renderer_destroy(server->renderer);
        server->renderer = NULL;
    }
    if (server->backend) {
        wlr_backend_destroy(server->backend);
        server->backend = NULL;
    }
    if (server->display) {
        wl_display_destroy(server->display);
        server->display = NULL;
    }
}
