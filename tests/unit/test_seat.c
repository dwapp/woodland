// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor.h"
#include "output.h"
#include "seat.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"

/*
 * VT 切换键识别（XF86Switch_VT_1..12 → 1..12）。
 *
 * 这是唯一的回归点：段内等差映射必须一格不差，段外一律 0——
 * 多认一格就会把普通功能键当 VT 键吞掉，少认一格就是按了没反应。
 */
static void test_vt_from_keysym(void)
{
    for (unsigned vt = 1; vt <= 12; vt++) {
        xkb_keysym_t sym = XKB_KEY_XF86Switch_VT_1 + (vt - 1);
        assert(woodland_seat_vt_from_keysym(sym) == vt);
    }
    /* 段前一格 / 段后一格 / 普通功能键 / 零值都不是 VT 键 */
    assert(woodland_seat_vt_from_keysym(XKB_KEY_XF86Switch_VT_1 - 1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_XF86Switch_VT_12 + 1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_F1) == 0);
    assert(woodland_seat_vt_from_keysym(XKB_KEY_Escape) == 0);
    assert(woodland_seat_vt_from_keysym(0) == 0);
}

/*
 * SSD 边框 resize 抓取区与下层窗口 Z 序裁决：
 *   1. 上层视图 A 的外扩 resize 抓取区落在下层视图 B 的客户端 surface 上时，
 *      即使 surface_view == B，也必须命中 A 的 SSD；
 *   2. 若 B 位于 A 之上（surface_view == B），A 的 SSD 不得抢走 B 的命中；
 *   3. 最小化或非活动工作区的上层视图 C 不应拦截命中。
 */
static void test_cursor_ssd_at_z_order(struct woodland_server *server)
{
    struct wlr_xdg_surface base_a = {0};
    base_a.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 200, .height = 150};
    struct wlr_xdg_toplevel top_a = {.base = &base_a};

    struct wlr_xdg_surface base_b = {0};
    base_b.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 400, .height = 300};
    struct wlr_xdg_toplevel top_b = {.base = &base_b};

    struct woodland_view view_a = {0};
    view_a.server = server;
    view_a.xdg_toplevel = &top_a;
    view_a.mapped = true;
    view_a.workspace = server->active_workspace;
    view_a.scene_tree = wlr_scene_tree_create(&server->scene->tree);
    assert(view_a.scene_tree);
    wlr_scene_node_set_position(&view_a.scene_tree->node, 100, 100);
    woodland_ssd_create(&view_a);
    assert(view_a.ssd != NULL);

    struct woodland_view view_b = {0};
    view_b.server = server;
    view_b.xdg_toplevel = &top_b;
    view_b.mapped = true;
    view_b.workspace = server->active_workspace;
    view_b.scene_tree = wlr_scene_tree_create(&server->scene->tree);
    assert(view_b.scene_tree);
    wlr_scene_node_set_position(&view_b.scene_tree->node, 50, 50);
    woodland_ssd_create(&view_b);
    assert(view_b.ssd != NULL);

    /* server->views 链表按从顶到底排列：A 在 B 之上 */
    wl_list_insert(&server->views, &view_b.link);
    wl_list_insert(&server->views, &view_a.link);

    /* 坐标 (96, 150) 处于 A 左边框向外 4px 的 resize 抓取区，同时也在 B 内容区内 */
    enum woodland_ssd_part part = WOODLAND_SSD_NONE;
    uint32_t edges = 0;
    struct woodland_view *hit =
        woodland_cursor_ssd_at(server, &view_b, 96.0, 150.0, &part, &edges);
    assert(hit == &view_a);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);

    /* 调换 Z 序：B 在 A 之上时，B 的客户端 surface 必须优先于下层 A 的 SSD */
    wl_list_remove(&view_b.link);
    wl_list_insert(&server->views, &view_b.link);
    part = WOODLAND_SSD_NONE;
    edges = 0;
    hit = woodland_cursor_ssd_at(server, &view_b, 96.0, 150.0, &part, &edges);
    assert(hit == NULL);

    /* 把 A 放回最上层，但设为最小化：不应再拦截 B */
    wl_list_remove(&view_a.link);
    wl_list_insert(&server->views, &view_a.link);
    view_a.minimized = true;
    hit = woodland_cursor_ssd_at(server, &view_b, 96.0, 150.0, &part, &edges);
    assert(hit == NULL);

    /* 恢复链表与释放临时节点 */
    wl_list_remove(&view_a.link);
    wl_list_remove(&view_b.link);
    woodland_ssd_destroy(&view_a);
    woodland_ssd_destroy(&view_b);
    wlr_scene_node_destroy(&view_a.scene_tree->node);
    wlr_scene_node_destroy(&view_b.scene_tree->node);
}

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend);
    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator);
    server.scene = wlr_scene_create();
    assert(server.scene);

    /* 基础协议已在 server.c 创建，这里模拟 init 顺序测试各模块 */
    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_shell_init(&server));
    woodland_cursor_init_events(&server);

    /* seat 与 xdg_shell 已创建 */
    assert(server.seat != NULL);
    assert(server.xdg_shell != NULL);

    /* 视图链表空 */
    assert(server.views.next == &server.views);
    assert(server.views.prev == &server.views);

    /* 光标模式初始为 passthrough */
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);

    test_vt_from_keysym();
    test_cursor_ssd_at_z_order(&server);

    /* 逆序释放（模拟 server finish） */
    woodland_cursor_finish_events(&server);
    woodland_shell_finish(&server);
    woodland_seat_finish(&server);
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
