// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include "scene.h"

#include "cursor.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "woodland.h"

struct client_state {
    struct zwlr_gamma_control_manager_v1 *manager;
    struct wl_output *output;
    uint32_t gamma_size;
    bool failed;
};

static void control_handle_gamma_size(void *data, struct zwlr_gamma_control_v1 *control,
                                      uint32_t size)
{
    struct client_state *state = data;
    state->gamma_size = size;
    (void)control;
}

static void control_handle_failed(void *data, struct zwlr_gamma_control_v1 *control)
{
    struct client_state *state = data;
    state->failed = true;
    (void)control;
}

static const struct zwlr_gamma_control_v1_listener control_listener = {
    .gamma_size = control_handle_gamma_size,
    .failed = control_handle_failed,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
        state->manager =
            wl_registry_bind(registry, name, &zwlr_gamma_control_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        state->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
    (void)version;
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void sync_callback(void *data, struct wl_callback *cb, uint32_t serial)
{
    bool *done = data;
    *done = true;
    wl_callback_destroy(cb);
    (void)serial;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_callback,
};

static void pump_roundtrip(struct woodland_server *server, struct wl_display *client_disp)
{
    bool done = false;
    struct wl_callback *cb = wl_display_sync(client_disp);
    assert(cb != NULL);
    wl_callback_add_listener(cb, &sync_listener, &done);
    while (!done) {
        assert(wl_display_get_error(client_disp) == 0);
        wl_display_flush(client_disp);
        wl_event_loop_dispatch(server->event_loop, 0);
        wl_display_flush_clients(server->display);
        wl_display_dispatch(client_disp);
    }
}

int main(void)
{
    setenv("WLR_RENDERER", "pixman", 1);

    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    server.event_loop = wl_display_get_event_loop(server.display);
    assert(server.event_loop != NULL);

    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend != NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer != NULL);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    woodland_cursor_init_events(&server);

    /* 初始化协议层（包含 output-management, power, gamma-control） */
    assert(woodland_protocol_init(&server));

    /* 添加一个无头输出并触发初始化 */
    struct wlr_output *wlr_out = wlr_headless_add_output(server.backend, 800, 600);
    assert(wlr_out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, wlr_out);
    assert(!wl_list_empty(&server.outputs));

    struct woodland_output *out = wl_container_of(server.outputs.next, out, link);
    assert(out->scene_output != NULL);
    assert(out->scene_output->WLR_PRIVATE.gamma_lut == NULL);

    /* 客户端连接并检索 gamma control manager */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *client = wl_client_create(server.display, fds[0]);
    assert(client != NULL);
    struct wl_display *client_display = wl_display_connect_to_fd(fds[1]);
    assert(client_display != NULL);

    struct client_state state = {0};
    struct wl_registry *reg = wl_display_get_registry(client_display);
    wl_registry_add_listener(reg, &registry_listener, &state);
    pump_roundtrip(&server, client_display);

    assert(state.manager != NULL);
    assert(state.output != NULL);

    /* 1. 客户端申请 gamma control 对象并协商色表大小 */
    struct zwlr_gamma_control_v1 *control =
        zwlr_gamma_control_manager_v1_get_gamma_control(state.manager, state.output);
    assert(control != NULL);
    zwlr_gamma_control_v1_add_listener(control, &control_listener, &state);

    pump_roundtrip(&server, client_display);

    /* 无头输出通过 fallback_gamma_size 协商为 256 */
    assert(state.gamma_size == 256);
    assert(state.failed == false);

    /* 2. 构造并下发 256 阶伽马 LUT 表 */
    uint32_t ramp_size = state.gamma_size;
    size_t table_bytes = 3 * ramp_size * sizeof(uint16_t);
    uint16_t *lut = malloc(table_bytes);
    assert(lut != NULL);

    for (uint32_t i = 0; i < ramp_size; i++) {
        uint16_t val = (uint16_t)(i * 65535 / (ramp_size - 1));
        lut[i] = val;                 // R
        lut[ramp_size + i] = val;     // G
        lut[2 * ramp_size + i] = val; // B
    }

    int memfd = memfd_create("test-gamma", MFD_CLOEXEC);
    assert(memfd >= 0);
    assert(ftruncate(memfd, table_bytes) == 0);
    assert(write(memfd, lut, table_bytes) == (ssize_t)table_bytes);
    assert(lseek(memfd, 0, SEEK_SET) == 0);
    free(lut);

    zwlr_gamma_control_v1_set_gamma(control, memfd);
    close(memfd);

    pump_roundtrip(&server, client_display);

    /* 验证服务端场景输出中 gamma_lut 已经被正确挂载并设置 */
    assert(out->scene_output->WLR_PRIVATE.gamma_lut != NULL);

    /* 3. 验证同一个输出上重复申请 gamma control 必须被拒绝 (发送 failed 事件) */
    struct client_state state2 = {0};
    struct zwlr_gamma_control_v1 *control2 =
        zwlr_gamma_control_manager_v1_get_gamma_control(state.manager, state.output);
    assert(control2 != NULL);
    zwlr_gamma_control_v1_add_listener(control2, &control_listener, &state2);

    pump_roundtrip(&server, client_display);
    assert(state2.failed == true);

    /* 4. 客户端销毁 gamma control，服务端恢复 gamma_lut 为空 */
    zwlr_gamma_control_v1_destroy(control);
    zwlr_gamma_control_v1_destroy(control2);
    pump_roundtrip(&server, client_display);

    assert(out->scene_output->WLR_PRIVATE.gamma_lut == NULL);

    /* 5. 清理客户端与服务端资源 */
    zwlr_gamma_control_manager_v1_destroy(state.manager);
    wl_output_destroy(state.output);
    wl_registry_destroy(reg);
    pump_roundtrip(&server, client_display);
    wl_display_disconnect(client_display);
    wl_client_destroy(client);

    woodland_protocol_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish_events(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_allocator_destroy(server.allocator);
    server.allocator = NULL;
    wlr_renderer_destroy(server.renderer);
    server.renderer = NULL;
    wlr_backend_destroy(server.backend);
    server.backend = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;

    return 0;
}
