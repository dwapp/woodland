// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "capture.h"
#include "config.h"
#include "cursor.h"
#include "ipc.h"
#include "layer.h"
#include "output.h"
#include "seat.h"
#include "selfcheck.h"
#include "server.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"
#if WOODLAND_HAVE_XWAYLAND
#include "xwayland.h"
#endif

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
    /* ipc_fd 的 0 是 stdin：先置 -1，避免 finish 里误关标准输入 */
    server->ipc_fd = -1;
    wl_list_init(&server->ipc_clients);
    woodland_selfcheck_init(server);

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
    server->compositor = wlr_compositor_create(server->display, 5,
                                               server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_data_device_manager_create(server->display);

    /* 补充协议：全部是 wlroots 原生 manager，创建即完成接线。
     * alpha_modifier / single_pixel 由 scene 自动集成；
     * fractional_scale 由 scene surface 进入输出时自动 notify。
     * 版本号对齐 labwc（ext_data_control=1、fractional_scale=1）。 */
    wlr_data_control_manager_v1_create(server->display);
    wlr_ext_data_control_manager_v1_create(server->display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->display);
    wlr_fractional_scale_manager_v1_create(server->display, 1);
    wlr_alpha_modifier_v1_create(server->display);

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

    if (!woodland_layer_init(server)) {
        wlr_log(WLR_ERROR, "failed to init layer");
        return false;
    }

    if (!woodland_capture_init(server)) {
        wlr_log(WLR_ERROR, "failed to init capture");
        return false;
    }

    woodland_cursor_init_events(server);

    /* Xwayland：需要 compositor（surface 分配）与 seat（X 输入焦点），
     * 所以放在 seat/shell 之后。lazy 模式：首个 X 客户端连接才拉进程。 */
#if WOODLAND_HAVE_XWAYLAND
    if (!woodland_xwayland_init(server)) {
        wlr_log(WLR_ERROR, "failed to init xwayland");
        return false;
    }
#endif

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

    /* 调试接口：路径按 wayland socket 名派生，并导出 WOODLAND_IPC。
     * 起不来只记日志——调试面挂了不该影响合成器本身。 */
    if (!woodland_ipc_init(server, socket)) {
        wlr_log(WLR_ERROR, "debug IPC unavailable, continuing without it");
    }

    /* X 客户端（以及 startup command 拉起的 X 程序）通过 DISPLAY 找 Xwayland */
    if (server->xwayland && server->xwayland->display_name) {
        setenv("DISPLAY", server->xwayland->display_name, true);
        wlr_log(WLR_INFO, "DISPLAY=%s", server->xwayland->display_name);
    }
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
#if WOODLAND_HAVE_XWAYLAND
    woodland_xwayland_finish(server);
#endif
    woodland_selfcheck_finish(server);
    woodland_ipc_finish(server);
    woodland_capture_finish(server);
    woodland_shell_finish(server);
    woodland_layer_finish(server);
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
