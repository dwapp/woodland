// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include "output.h"
#include "woodland.h"

/*
 * output_removed 的观察者。
 *
 * 协议层靠这个信号在拔除后补一份 configuration + done：被 --off 禁用的
 * 输出早已不在 layout 里，销毁它不会触发 layout change，只靠 layout 驱动
 * 的广播补不上。这里断言信号确实发出来、参数是被销毁的 wlr_output，并且
 * 发出时它已经从 server->outputs 摘掉了（协议层广播会遍历那条链表）。
 */
struct removed_probe {
    struct wl_listener listener;
    struct woodland_server *server;
    int count;
    struct wlr_output *output;
    size_t outputs_at_emit;
};

static void handle_removed(struct wl_listener *listener, void *data) {
    struct removed_probe *probe =
        wl_container_of(listener, probe, listener);
    probe->count++;
    probe->output = data;

    size_t n = 0;
    struct woodland_output *o;
    wl_list_for_each(o, &probe->server->outputs, link) {
        n++;
    }
    probe->outputs_at_emit = n;
}

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();

    assert(woodland_output_init(&server));

    /* 无 output 时 stable */
    assert(server.outputs.next == &server.outputs);
    assert(server.outputs.prev == &server.outputs);

    /* 模拟 new_output 事件 */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 去重：重复 emit 不应重复创建 */
    wl_signal_emit_mutable(&server.backend->events.new_output, out);
    size_t n = 0;
    struct woodland_output *o;
    wl_list_for_each(o, &server.outputs, link) n++;
    assert(n == 1);

    /* 非桌面输出（如 VR 头显）：不应配置为桌面输出 */
    struct wlr_output *vr_out = wlr_headless_add_output(server.backend, 1440, 1600);
    assert(vr_out != NULL);
    vr_out->non_desktop = true;
    wl_signal_emit_mutable(&server.backend->events.new_output, vr_out);
    assert(vr_out->data == NULL);
    size_t n_after_vr = 0;
    wl_list_for_each(o, &server.outputs, link) n_after_vr++;
    assert(n_after_vr == 1);
    wlr_output_destroy(vr_out);

    /* 拔除：信号必须发出一次，且发出时该输出已不在链表里 */
    struct removed_probe probe = { .server = &server };
    probe.listener.notify = handle_removed;
    wl_signal_add(&server.output_removed, &probe.listener);

    wlr_output_destroy(out);

    assert(probe.count == 1);
    assert(probe.output == out);
    assert(probe.outputs_at_emit == 0);
    assert(server.outputs.next == &server.outputs);
    wl_list_remove(&probe.listener.link);

    woodland_output_finish(&server);

    /* 逆序释放 wlr 资源（ASan 下验证无泄漏） */
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
