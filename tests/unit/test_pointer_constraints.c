// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include "scene.h"
#include <wlr/types/wlr_seat.h>

#include "cursor.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "woodland.h"
#include "workspace.h"

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

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    assert(woodland_output_init(&server));
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    /* 协议层要挂壁纸族，壁纸族监听 workspace_changed */
    assert(woodland_workspace_init(&server));
    assert(woodland_protocol_init(&server));

    /* 验证指针约束管理器正常创建 */
    assert(server.pointer_constraints != NULL);
    assert(server.current_constraint == NULL);

    /* 空指针安全测试 */
    woodland_pointer_constraint_recheck(&server, NULL);
    assert(server.current_constraint == NULL);

    /* 创建测试 surface */
    struct wlr_surface surface = {0};
    wl_signal_init(&surface.events.destroy);
    wl_signal_init(&surface.events.commit);
    /* 设置指针焦点与键盘焦点到测试 surface (表面坐标 10, 10，光标布局坐标 100, 100) */
    wlr_cursor_warp(server.cursor, NULL, 100, 100);
    server.seat->pointer_state.focused_surface = &surface;
    server.seat->pointer_state.sx = 10;
    server.seat->pointer_state.sy = 10;
    server.seat->keyboard_state.focused_surface = &surface;
    /* 模拟 CONFINED 约束并测试区域限制计算 */
    struct wlr_pointer_constraint_v1 constraint = {0};
    constraint.type = WLR_POINTER_CONSTRAINT_V1_CONFINED;
    constraint.surface = &surface;
    constraint.seat = server.seat;
    wl_signal_init(&constraint.events.destroy);
    pixman_region32_init_rect(&constraint.region, 0, 0, 50, 50);

    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == &constraint);

    /* 测试 1: 在约束区域内移动 (从 10,10 移到 20,20，增量 +10, +10) */
    double dx = 10, dy = 10;
    woodland_pointer_constraint_apply(&server, &dx, &dy);
    assert(dx == 10 && dy == 10);

    /* 测试 2: 试图移出边界 (从 10,10 移到 100,100，被限制在 box 50x50 边界内) */
    dx = 90;
    dy = 90;
    woodland_pointer_constraint_apply(&server, &dx, &dy);
    assert(dx < 90 && dy < 90);
    assert(10 + dx <= 50);
    assert(10 + dy <= 50);

    /* 测试 3: LOCKED 约束与光标位置提示 (cursor hint warp) */
    constraint.type = WLR_POINTER_CONSTRAINT_V1_LOCKED;
    constraint.current.committed = WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT;
    constraint.current.cursor_hint.x = 25;
    constraint.current.cursor_hint.y = 30;

    /* 解除约束，光标应自动 warp 到 cursor_hint 指定位置 (surface_lx + 25, surface_ly + 30)
     * surface_lx = 100 - 10 = 90, surface_ly = 100 - 10 = 90
     * 预期坐标: 90 + 25 = 115, 90 + 30 = 120 */
    woodland_pointer_constraint_set(&server, NULL);
    assert(server.current_constraint == NULL);
    assert(server.cursor->x == 115);
    assert(server.cursor->y == 120);

    /* 测试 4: 约束被销毁事件 */
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == &constraint);
    wl_signal_emit_mutable(&constraint.events.destroy, &constraint);
    assert(server.current_constraint == NULL);
    /* 测试 5: 焦点不在申请限制的客户端上时，应该忽略限制 */
    struct wlr_surface other_surface = {0};
    wl_signal_init(&other_surface.events.destroy);
    wl_signal_init(&other_surface.events.commit);

    /* 重新激活 CONFINED 约束 */
    constraint.type = WLR_POINTER_CONSTRAINT_V1_CONFINED;
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == &constraint);

    /* 焦点切换到 other_surface */
    server.seat->keyboard_state.focused_surface = &other_surface;

    /* 约束应被忽略：调用 apply 时 dx, dy 不被裁剪 */
    dx = 90;
    dy = 90;
    woodland_pointer_constraint_apply(&server, &dx, &dy);
    assert(dx == 90 && dy == 90);

    /* 焦点不在该客户端上时，recheck 应当忽略限制且不设为当前约束 */
    woodland_pointer_constraint_recheck(&server, &surface);
    assert(server.current_constraint == NULL);

    /* 焦点不在该客户端上时，调用 set 应当忽略限制 */
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == NULL);

    /* 当焦点重新回到 surface 时，限制恢复有效 */
    server.seat->keyboard_state.focused_surface = &surface;
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == &constraint);
    dx = 90;
    dy = 90;
    woodland_pointer_constraint_apply(&server, &dx, &dy);
    assert(dx < 90 && dy < 90);
    assert(10 + dx <= 50);
    assert(10 + dy <= 50);

    /* 焦点切换后调用 update，当前约束应被清除 */
    server.seat->keyboard_state.focused_surface = &other_surface;
    woodland_pointer_constraint_update(&server);
    assert(server.current_constraint == NULL);

    /* 测试基于客户端 (wl_client) 的焦点校验：同客户端的子表面持焦点时允许限制 */
    int dummy_a = 0, dummy_b = 0;
    struct wl_client *mock_client_a = (struct wl_client *)&dummy_a;
    struct wl_client *mock_client_b = (struct wl_client *)&dummy_b;
    struct wl_resource res_a = {0};
    struct wl_resource res_b = {0};
    res_a.client = mock_client_a;
    res_b.client = mock_client_b;
    surface.resource = &res_a;
    other_surface.resource = &res_b;

    /* 焦点在 client B，应当忽略限制 */
    server.seat->keyboard_state.focused_surface = &other_surface;
    assert(!woodland_pointer_constraint_is_focused(&server, &constraint));
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == NULL);

    /* 焦点切换到 client A 的同客户端另一表面（例如 popup / subsurface），允许限制 */
    struct wlr_surface popup_surface = {0};
    wl_signal_init(&popup_surface.events.destroy);
    wl_signal_init(&popup_surface.events.commit);
    popup_surface.resource = &res_a;
    server.seat->keyboard_state.focused_surface = &popup_surface;
    assert(woodland_pointer_constraint_is_focused(&server, &constraint));
    woodland_pointer_constraint_set(&server, &constraint);
    assert(server.current_constraint == &constraint);

    /* 清理测试资源引用与约束 */
    surface.resource = NULL;
    other_surface.resource = NULL;
    popup_surface.resource = NULL;
    woodland_pointer_constraint_set(&server, NULL);

    /* 清理测试约束 */
    pixman_region32_fini(&constraint.region);

    /* 逆序释放子系统 */
    woodland_protocol_finish(&server);
    woodland_workspace_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);
    wlr_output_destroy(out);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    return 0;
}
