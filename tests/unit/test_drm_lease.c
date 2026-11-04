// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "cursor.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "woodland.h"

int main(void)
{
    struct woodland_server server = {0};
    wl_list_init(&server.outputs);
    wl_list_init(&server.keyboards);
    wl_list_init(&server.views);
    wl_list_init(&server.workspaces);
    wl_signal_init(&server.output_removed);

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

    /* 初始化协议层（包含 drm_lease 协议族） */
    assert(woodland_protocol_init(&server));

    /* 空指针与安全防护测试 */
    woodland_protocol_offer_drm_lease(NULL, NULL);
    woodland_protocol_offer_drm_lease(&server, NULL);
    woodland_protocol_withdraw_drm_lease(NULL, NULL);
    woodland_protocol_withdraw_drm_lease(&server, NULL);

    /* 添加 headless 输出并测试 new_output 信号分发 */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 对非 DRM 输出调用 offer/withdraw 应该是安全的无操作（包括多次重复调用） */
    woodland_protocol_offer_drm_lease(&server, out);
    woodland_protocol_offer_drm_lease(&server, out);
    woodland_protocol_withdraw_drm_lease(&server, out);
    woodland_protocol_withdraw_drm_lease(&server, out);
    /* 销毁输出 */
    wlr_output_destroy(out);

    /* 逆序清理 */
    woodland_protocol_finish(&server);
    assert(server.proto == NULL);

    /* 二次调用 finish 保证幂等性 */
    woodland_protocol_finish(&server);

    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    return 0;
}
