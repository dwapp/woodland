// SPDX-License-Identifier: MIT
#include <signal.h>

#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/util/log.h>

#include "cursor.h"
#include "output.h"
#include "server.h"
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

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

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
    if (wl_display_add_socket_auto(server->display) == NULL) {
        wlr_log(WLR_ERROR, "failed to add wayland socket");
        return false;
    }

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return false;
    }

    return true;
}

void woodland_server_run(struct woodland_server *server) {
    wl_display_run(server->display);
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
