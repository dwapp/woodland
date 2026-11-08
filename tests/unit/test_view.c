// SPDX-License-Identifier: GPL-2.0-only
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "xwayland.h"
#include <assert.h>
#include <string.h>
#include "placement.h"
#include "util/json.h"
#include "woodland_config.h"
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "scene.h"
#include "workspace.h"
#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif
static int mock_restack_calls = 0;
static void mock_test_restack_hook(struct woodland_view *v) {
    (void)v;
    mock_restack_calls++;
}


int main(void)
{
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

    assert(woodland_output_init(&server));

    /* 添加一个 800x600 headless output */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 最大化几何 = 输出 layout box */
    struct wlr_box max_geo = woodland_view_maximized_geometry(&server);
    assert(max_geo.width == 800);
    assert(max_geo.height == 600);

    /* 全屏几何 = 指定输出完整 box */
    struct wlr_box fs_geo = woodland_view_fullscreen_geometry(&server, out);
    assert(fs_geo.width == 800);
    assert(fs_geo.height == 600);

    /* exclusive zone 占用后 maximize 应避开：手动设 usable_area */
    struct woodland_output *wo = wl_container_of(server.outputs.next, wo, link);
    wo->usable_area = (struct wlr_box){.x = 10, .y = 10, .width = 780, .height = 580};
    wo->usable_area_valid = true;
    struct wlr_box max_geo2 = woodland_view_maximized_geometry(&server);
    assert(max_geo2.x == 10 && max_geo2.y == 10);
    assert(max_geo2.width == 780 && max_geo2.height == 580);

    /*
     * 放置：外框（内容 + SSD 边距）整体落进工作区。
     * 内容原点必须让出左上边距，否则标题栏会顶到屏幕外。
     */
    struct woodland_ssd_margins margins = {
        .left = 1,
        .top = 27,
        .right = 1,
        .bottom = 1,
    };
    struct wlr_box place;

    /* 无层叠：内容原点 = 工作区原点 + (left, top) */
    woodland_view_placement(&max_geo2, 400, 300, &margins, 0, &place);
    assert(place.x == 11 && place.y == 37);
    assert(place.width == 400 && place.height == 300);

    /* 层叠偏移 32：整体右移下移 32 */
    woodland_view_placement(&max_geo2, 400, 300, &margins, 32, &place);
    assert(place.x == 43 && place.y == 69);

    /* 偏移过大：回贴工作区右下，外框右/下边缘正好贴住工作区边界 */
    woodland_view_placement(&max_geo2, 400, 300, &margins, 1000, &place);
    assert(place.x + place.width + margins.right == 10 + 780);
    assert(place.y + place.height + margins.bottom == 10 + 580);

    /* 内容比工作区还大：贴回左上，不外溢到负坐标 */
    woodland_view_placement(&max_geo2, 1000, 800, &margins, 0, &place);
    assert(place.x == 11 && place.y == 37);

    /*
     * 贴边吸附（单轴）：工作区 x∈[10,790]，阈值 20。
     * 候选 = 工作区两端 + 外部边（示例 400 / 700）。
     */
    int edges[] = {400, 700};

    /* 离边缘远：不动 */
    assert(woodland_snap_axis(300, 200, 10, 790, NULL, 0, 20) == 300);
    /* 距左边缘 5px：贴 lo */
    assert(woodland_snap_axis(15, 200, 10, 790, NULL, 0, 20) == 10);
    /* 距右边缘 15px：外框右边贴 hi（hi - size = 590） */
    assert(woodland_snap_axis(575, 200, 10, 790, NULL, 0, 20) == 590);
    /* 距左边缘 25px（> 阈值）：不吸附 */
    assert(woodland_snap_axis(35, 200, 10, 790, NULL, 0, 20) == 35);

    /* 贴到邻窗左边缘（400）：起点贴上去 */
    assert(woodland_snap_axis(390, 200, 10, 790, edges, 2, 20) == 400);
    /* 贴到邻窗左边缘：末边贴上去（400 - 200 = 200） */
    assert(woodland_snap_axis(210, 200, 10, 790, edges, 2, 20) == 200);
    /* 贴到邻窗右边缘（700）：末边贴上去（700 - 200 = 500） */
    assert(woodland_snap_axis(510, 200, 10, 790, edges, 2, 20) == 500);
    /* 同距离时优先工作区边缘：desired=20 距 lo(10) 10px、距 400... 不匹配 */
    assert(woodland_snap_axis(20, 200, 10, 790, edges, 2, 20) == 10);

    /* 外框比工作区还大：不吸附 */
    assert(woodland_snap_axis(700, 800, 10, 790, NULL, 0, 20) == 700);
    /* 边缘阻尼（labwc resistance 核心原则：仅在试图穿透边界时拦截，内部移动绝不拉扯，脱离边界绝不阻拦） */
    /* 内部向左移动但未穿透：从 50 移到 15 (lo=10, 阈值 20) -> 保持 15，完全跟随光标 */
    assert(woodland_snap_axis_motion(50, 15, 200, 10, 790, NULL, 0, 20) == 15);
    /* 试图向左穿出左边界：从 15 移到 5 (试图穿过 lo=10) -> 拦截在 lo=10 */
    assert(woodland_snap_axis_motion(15, 5, 200, 10, 790, NULL, 0, 20) == 10);
    /* 穿透力量超过阻尼阈值：从 10 移到 -15 (< 10-20=-10) -> 允许穿透，到达 -15 */
    assert(woodland_snap_axis_motion(10, -15, 200, 10, 790, NULL, 0, 20) == -15);
    /* 离开左边界向内移动：从 10 移到 15 -> 绝不往回拉扯，到达 15 */
    assert(woodland_snap_axis_motion(10, 15, 200, 10, 790, NULL, 0, 20) == 15);

    /* 内部向右移动但未穿透：从 550 移到 580 (hi - size = 590) -> 保持 580，完全跟随光标 */
    assert(woodland_snap_axis_motion(550, 580, 200, 10, 790, NULL, 0, 20) == 580);
    /* 试图向右穿出右边界：从 580 移到 595 (试图穿过 590) -> 拦截在 590 */
    assert(woodland_snap_axis_motion(580, 595, 200, 10, 790, NULL, 0, 20) == 590);
    /* 离开右边界向内移动：从 590 移到 580 -> 绝不往回拉扯，到达 580 */
    assert(woodland_snap_axis_motion(590, 580, 200, 10, 790, NULL, 0, 20) == 580);

    /* 邻窗阻尼：邻窗右边缘位于 400，从 420 向左试图穿入 395 -> 拦截在 400 */
    assert(woodland_snap_axis_motion(420, 395, 200, 10, 790, edges, 2, 20) == 400);
    /* 离开邻窗右边缘向右移动：从 400 移到 405 -> 绝不往回拉扯，到达 405 */
    assert(woodland_snap_axis_motion(400, 405, 200, 10, 790, edges, 2, 20) == 405);
    /* 阈值 <= 0：关闭阻尼，原样返回 */
    assert(woodland_snap_axis_motion(15, 5, 200, 10, 790, NULL, 0, 0) == 5);

    /*
     * transient 居中：外框（内容 + margins）以父窗口内容框为中心，
     * 越界贴回工作区内侧。
     */
    struct wlr_box parent_box = {.x = 200, .y = 150, .width = 400, .height = 300};

    /* 居中：外框 202x148 → 父框中心 (400,300) → 外框原点 (299,226) */
    woodland_view_center_on(&parent_box, 200, 120, &margins, &max_geo2, &place);
    assert(place.x == 300 && place.y == 253);
    assert(place.width == 200 && place.height == 120);

    /* 父窗口贴右下角：居中会越界，贴回工作区右下 */
    parent_box = (struct wlr_box){.x = 770, .y = 560, .width = 400, .height = 300};
    woodland_view_center_on(&parent_box, 200, 120, &margins, &max_geo2, &place);
    assert(place.x == 10 + 780 - 202 + 1);
    assert(place.y == 10 + 580 - 148 + 27);

    /* 子窗口比工作区还大：贴左上，不产生负向越界 */
    woodland_view_center_on(&parent_box, 800, 600, &margins, &max_geo2, &place);
    assert(place.x == 11 && place.y == 37);

    /*
     * 拖动吸附的端到端验证：伪造视图 + 光标位置，走
     * woodland_cursor_process_motion 的 MOVE 分支。
     */
    /* 只需要一个能读 x/y 的光标对象；xcursor 主题与本用例无关 */
    server.cursor = wlr_cursor_create();
    assert(server.cursor);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wl_list_init(&server.views);

    struct wlr_xdg_surface drag_base = {0};
    drag_base.geometry = (struct wlr_box){.width = 400, .height = 300};
    struct wlr_xdg_toplevel drag_toplevel = {.base = &drag_base};
    struct woodland_view drag_view = {0};
    drag_view.server = &server;
    drag_view.xdg_toplevel = &drag_toplevel;
    drag_view.mapped = true;
    drag_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(&server.views, &drag_view.link);

    server.grabbed_view = &drag_view;
    server.cursor_mode = WOODLAND_CURSOR_MOVE;
    server.grab_x = 0;
    server.grab_y = 0;

    /* 窗口在 (50, 50)，向左上拖动试图穿出左边界 (lo=10) 与顶边界 (lo=10)：
     * cursor=(5, 30) -> desired_outer=(4, 3)，在阻尼阈值内分别拦截在 (10, 10) */
    wlr_scene_node_set_position(&drag_view.scene_tree->node, 50 + margins.left, 50 + margins.top);
    server.cursor->x = 5;
    server.cursor->y = 30;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 10 + margins.left); /* 11 */
    assert(drag_view.scene_tree->node.y == 10 + margins.top);  /* 37 */

    /* 中间空地：不穿透边界 -> 完全跟随光标，内容原点=光标位置 */
    server.cursor->x = 300;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 300);
    assert(drag_view.scene_tree->node.y == 200);

    /* 右边框阻尼：试图向右穿出工作区右边界（790 - 402 = 388），cursor=392 -> 拦截在 388 */
    server.cursor->x = 392;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 388 + margins.left); /* 389 */

    /* 贴邻窗：第二个窗口内容 (500,100) 200x200，左外边缘 499；
     * 我们的外框向右试图穿入邻窗左边缘 (cursor=502 -> desired_x=501 > 499) -> 拦截在 499 */
    struct wlr_xdg_surface peer_base = {0};
    peer_base.geometry = (struct wlr_box){.width = 200, .height = 200};
    struct wlr_xdg_toplevel peer_toplevel = {.base = &peer_base};
    struct woodland_view peer_view = {0};
    peer_view.server = &server;
    peer_view.xdg_toplevel = &peer_toplevel;
    peer_view.mapped = true;
    peer_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&peer_view.scene_tree->node, 500, 100);
    wl_list_insert(&server.views, &peer_view.link);

    server.cursor->x = 502;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 500);
    assert(drag_view.scene_tree->node.y == 200);

    wl_list_remove(&drag_view.link);
    wl_list_remove(&peer_view.link);
    server.grabbed_view = NULL;
    server.cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;

    /* 验证 foreign-toplevel 句柄的 output_enter / output_leave 同步 */
    server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.display);
    assert(server.foreign_toplevel_manager != NULL);

    /* 添加第二个 800x600 输出并排布在 (800, 0) */
    struct wlr_output *out2 = wlr_headless_add_output(server.backend, 800, 600);
    assert(out2 != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out2);
    wlr_output_layout_add(server.output_layout, out, 0, 0);
    wlr_output_layout_add(server.output_layout, out2, 800, 0);

    struct wlr_xdg_surface ft_base = {0};
    ft_base.geometry = (struct wlr_box){.width = 200, .height = 200};
    struct wlr_xdg_toplevel ft_toplevel = {.base = &ft_base};
    struct woodland_view ft_view = {0};
    ft_view.server = &server;
    ft_view.xdg_toplevel = &ft_toplevel;
    ft_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    woodland_view_init_handles(&ft_view);
    assert(ft_view.toplevel_handle != NULL);
    assert(wl_list_empty(&ft_view.toplevel_handle->outputs));

    /* 映射到输出 1 内部 (100, 100) -> 触发 output 1 enter */
    ft_view.mapped = true;
    wlr_scene_node_set_position(&ft_view.scene_tree->node, 100, 100);
    wl_list_insert(&server.views, &ft_view.link);
    woodland_view_update_outputs(&ft_view);
    assert(wl_list_length(&ft_view.toplevel_handle->outputs) == 1);
    struct wlr_foreign_toplevel_handle_v1_output *tlo =
        wl_container_of(ft_view.toplevel_handle->outputs.next, tlo, link);
    assert(tlo->output == out);
    assert(ft_view.outputs_initialized && ft_view.outputs != 0);
    uint64_t old_outputs = ft_view.outputs;
    /* 同一屏幕内平移 (150, 100) -> outputs 掩码保持不变 */
    wlr_scene_node_set_position(&ft_view.scene_tree->node, 150, 100);
    woodland_view_update_outputs(&ft_view);
    assert(ft_view.outputs == old_outputs);

    /* 测试 grab_context 前置记录与复用 */
    server.grab_context.active = true;
    server.grab_context.view = &ft_view;
    server.grab_context.cursor_x = 200;
    server.grab_context.cursor_y = 150;
    server.grab_context.node_x = 150;
    server.grab_context.node_y = 100;
    /* 模拟客户端 request_move 延迟到达期间光标移动到 (220, 160) */
    server.cursor->x = 220;
    server.cursor->y = 160;
    woodland_view_begin_interactive(&ft_view, WOODLAND_CURSOR_MOVE, 0);
    /* 抓取偏移量使用按下时的精准坐标 (200 - 150 = 50, 150 - 100 = 50) */
    assert(server.grab_x == 50);
    assert(server.grab_y == 50);
    assert(server.drag_start_x == 200);
    assert(server.drag_start_y == 150);
    assert(!server.grab_context.active);
    woodland_cursor_reset_mode(&server);

    /* 移到输出 2 内部 (900, 100) -> 离开输出 1，进入输出 2 */
    wlr_scene_node_set_position(&ft_view.scene_tree->node, 900, 100);
    woodland_view_update_outputs(&ft_view);
    assert(wl_list_length(&ft_view.toplevel_handle->outputs) == 1);
    tlo = wl_container_of(ft_view.toplevel_handle->outputs.next, tlo, link);
    assert(tlo->output == out2);

    /* 跨两个输出 (750, 100) -> 同时存在于输出 1 与输出 2 */
    wlr_scene_node_set_position(&ft_view.scene_tree->node, 750, 100);
    woodland_view_update_outputs(&ft_view);
    assert(wl_list_length(&ft_view.toplevel_handle->outputs) == 2);

    /* 取消映射 -> 离开所有输出 */
    ft_view.mapped = false;
    woodland_view_update_outputs(&ft_view);
    assert(wl_list_empty(&ft_view.toplevel_handle->outputs));

    wl_list_remove(&ft_view.link);
    wl_list_remove(&ft_view.ft_request_activate.link);
    wl_list_remove(&ft_view.ft_request_maximize.link);
    wl_list_remove(&ft_view.ft_request_fullscreen.link);
    wl_list_remove(&ft_view.ft_request_minimize.link);
    wl_list_remove(&ft_view.ft_request_close.link);
    wlr_foreign_toplevel_handle_v1_destroy(ft_view.toplevel_handle);
    wlr_scene_node_destroy(&ft_view.scene_tree->node);
    /* Xwayland 视图的缩放滤波 (scale_filter) 验证 */
    struct woodland_view x11_view = {0};
    x11_view.server = &server;
    x11_view.type = WOODLAND_VIEW_XWAYLAND;
    x11_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    struct wlr_scene_buffer *mock_buf = wlr_scene_buffer_create(x11_view.scene_tree, NULL);
    struct wlr_scene_surface mock_scene_surface = { .buffer = mock_buf };
    x11_view.scene_surface = &mock_scene_surface;

    /* 默认配置 (smart)：输出缩放 1.0 时为 BILINEAR */
    woodland_xwayland_view_update_scale_filter(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_BILINEAR);

    /* 输出缩放 2.0 (整数倍) 时在 smart 模式下自动转为 NEAREST，消除模糊 */
    out->scale = 2.0f;
    woodland_xwayland_view_update_scale_filter(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_NEAREST);

    /* 配置 scale_filter = "linear" 时强制为 BILINEAR */
    struct woodland_config cfg_linear = {0};
    cfg_linear.xwayland.scale_filter = WOODLAND_SCALE_FILTER_BILINEAR;
    server.config = &cfg_linear;
    woodland_xwayland_view_update_scale_filter(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_BILINEAR);

    /* 配置 scale_filter = "nearest" 时在任意缩放 (>1.0) 下均为 NEAREST */
    struct woodland_config cfg_nearest = {0};
    cfg_nearest.xwayland.scale_filter = WOODLAND_SCALE_FILTER_NEAREST;
    server.config = &cfg_nearest;
    out->scale = 1.25f;
    woodland_xwayland_view_update_scale_filter(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_NEAREST);

    server.config = NULL;
    /* 验证在 2.0x 输出上的 SSD 自动缩放 */
    out->scale = 2.0f;
    woodland_ssd_create(&x11_view);
    assert(x11_view.ssd != NULL);
    assert(woodland_ssd_get_scale(&x11_view) == 2.0f);
    woodland_ssd_destroy(&x11_view);

    /* 跨显示器移动验证：从 1.0x 移到 2.0x 显示器，自动切 NEAREST 并将 SSD 升到 2.0x */
    out->scale = 1.0f;
    out2->scale = 2.0f;
    server.xwayland_scale_filter_hook = woodland_xwayland_view_update_scale_filter;
    wlr_scene_node_set_position(&x11_view.scene_tree->node, 100, 100);
    woodland_ssd_create(&x11_view);
    woodland_view_update_outputs(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_BILINEAR);
    assert(woodland_ssd_get_scale(&x11_view) == 1.0f);

    wlr_scene_node_set_position(&x11_view.scene_tree->node, 900, 100);
    woodland_view_update_outputs(&x11_view);
    assert(mock_buf->filter_mode == WLR_SCALE_FILTER_NEAREST);
    assert(woodland_ssd_get_scale(&x11_view) == 2.0f);

    woodland_ssd_destroy(&x11_view);
    server.xwayland_scale_filter_hook = NULL;

    wlr_scene_node_destroy(&x11_view.scene_tree->node);


    /* ---------- 贴边预览（snap preview）测试 ---------- */
    struct woodland_config cfg = {0};
    cfg.general.snap_preview = true;
    cfg.general.snap_distance = 20;
    server.config = &cfg;

    woodland_snap_preview_init(&server);
    assert(!server.snap_preview.edges);

    /* 初始状态 IPC json 输出 active=false */
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_snap_preview_status(&server, &json);
    assert(strstr(woodland_json_data(&json), "\"active\":false"));
    woodland_json_finish(&json);

    /* 创建测试 view 并展示左半屏贴边预览 */
    struct wlr_xdg_surface preview_base = {0};
    preview_base.geometry = (struct wlr_box){.width = 400, .height = 300};
    struct wlr_xdg_toplevel preview_toplevel = {.base = &preview_base};
    struct woodland_view preview_view = {0};
    preview_view.server = &server;
    preview_view.xdg_toplevel = &preview_toplevel;
    preview_view.mapped = true;
    preview_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    assert(preview_view.scene_tree);

    woodland_snap_preview_show(&server, &preview_view, wo, WLR_EDGE_LEFT);
    assert(server.snap_preview.edges == WLR_EDGE_LEFT);
    assert(server.snap_preview.tree != NULL);
    assert(server.snap_preview.tree->node.enabled);
    /* wo 工作区为 (10, 10, 780, 580) -> 左半屏为 (10, 10, 390, 580) */
    assert(server.snap_preview.box.width == 390);
    assert(server.snap_preview.box.height == 580);
    assert(server.snap_preview.box.x == 10);
    assert(server.snap_preview.box.y == 10);

    /* 检查 IPC json 输出 active=true 且 edges 包含 "left" */
    woodland_json_init(&json);
    woodland_snap_preview_status(&server, &json);
    assert(strstr(woodland_json_data(&json), "\"active\":true"));
    assert(strstr(woodland_json_data(&json), "\"left\""));
    woodland_json_finish(&json);

    /* 隐藏预览 */
    woodland_snap_preview_hide(&server);
    assert(!server.snap_preview.edges);
    assert(!server.snap_preview.tree->node.enabled);

    /* 展示全屏预览 (TOP) */
    woodland_snap_preview_show(&server, &preview_view, wo, WLR_EDGE_TOP);
    assert(server.snap_preview.edges == WLR_EDGE_TOP);
    assert(server.snap_preview.box.width == 780);
    assert(server.snap_preview.box.height == 580);

    /* 释放松手铺满 */
    server.grabbed_view = &preview_view;
    preview_view.pending_snap_edges = WLR_EDGE_TOP;
    shell_view_apply_snap(&preview_view);
    assert(preview_view.snapped_edges == WLR_EDGE_TOP);
    assert(preview_view.maximized);
    assert(!server.snap_preview.edges);
    assert(server.snap_preview.tree == NULL);

    /* 拖拽最大化窗口恢复：按光标水平比例锚定（labwc max_move_scale 同款） */
    preview_base.geometry = (struct wlr_box){.width = 780, .height = 580};
    server.cursor->x = 10 + 0.75 * 780; /* 595 */
    server.cursor->y = 15;
    preview_view.saved_geometry = (struct wlr_box){.x = 50, .y = 50, .width = 400, .height = 300};
    woodland_view_begin_interactive(&preview_view, WOODLAND_CURSOR_MOVE, 0);
    /* 抖动在防抖阈值内：保持最大化状态（防抖误触） */
    server.cursor->x = 597;
    woodland_cursor_process_motion(&server, 0);
    assert(preview_view.maximized);

    /* 真正拖动超过阈值：触发 unsnap 并按比例锚定 */
    server.cursor->x = 625;
    woodland_cursor_process_motion(&server, 0);
    assert(!preview_view.maximized);
    assert(server.cursor_mode == WOODLAND_CURSOR_MOVE);
    /* 模拟客户端处理 configure(400, 300) 后提交新尺寸 */
    preview_base.geometry = (struct wlr_box){.width = 400, .height = 300};
    struct wlr_box unsnapped_outer;
    woodland_view_outer_box(&preview_view, &unsnapped_outer);
    assert(unsnapped_outer.x != (int)(server.cursor->x - unsnapped_outer.width / 2));
    double frac = (server.cursor->x - unsnapped_outer.x) / (double)unsnapped_outer.width;
    assert(frac >= 0.70 && frac <= 0.85);
    woodland_cursor_reset_mode(&server);
    wlr_scene_node_destroy(&preview_view.scene_tree->node);

    /*
     * ---------- 拖动管线端到端：光标 → pending → 预览 → 松手铺满 ----------
     * 只有输入事件是伪造的（直接写 cursor->x/y），几何推进、贴边判定、
     * 预览显隐、松手应用走的都是真实函数：woodland_cursor_process_motion
     * → shell_process_cursor_move → woodland_snap_preview_update →
     * shell_view_apply_snap。前面那些用例是"直接调 show()"，绕过了判定。
     */
    struct wlr_xdg_surface pipe_base = {0};
    pipe_base.geometry = (struct wlr_box){.width = 400, .height = 300};
    struct wlr_xdg_toplevel pipe_toplevel = {.base = &pipe_base};
    struct woodland_view pipe_view = {0};
    pipe_view.server = &server;
    pipe_view.xdg_toplevel = &pipe_toplevel;
    pipe_view.mapped = true;
    pipe_view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(&server.views, &pipe_view.link);

    server.grabbed_view = &pipe_view;
    server.cursor_mode = WOODLAND_CURSOR_MOVE;
    server.grab_x = 0;
    server.grab_y = 0;
    server.drag_start_x = 300;
    server.drag_start_y = 200;

    /* 光标在屏幕中央：既不贴边也不预览 */
    server.cursor->x = 300;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(pipe_view.pending_snap_edges == 0);
    assert(server.snap_preview.edges == 0);
    assert(server.snap_preview.tree == NULL);

    /*
     * 判定语义钉死：**只看光标**（见 woodland_snap_determine_edges）。
     * 窗口外框是否贴住工作区边不参与判定——"窗口正好被拖到贴边、光标还在
     * 标题栏中间"只是在挪窗口，用户没表达要占半屏。松手路径
     * （shell_view_apply_snap）用的就是这个返回值，两边一致，所以不会出现
     * "预览没亮、窗口却跳了"。
     */
    struct woodland_view probe_view = {0};
    struct wlr_box probe_area = {.x = 10, .y = 10, .width = 780, .height = 580};
    /* 光标在屏幕中央：不判（哪怕外框正好贴住左边，也不再触发） */
    assert(woodland_snap_determine_edges(&probe_view, &probe_area, 400, 300, 20) == 0);
    /* 光标自己贴到左边才判左半屏 */
    assert(woodland_snap_determine_edges(&probe_view, &probe_area, 12, 300, 20) == WLR_EDGE_LEFT);
    /* 光标贴右边：与窗口几何无关，照样判 */
    assert(woodland_snap_determine_edges(&probe_view, &probe_area, 788, 300, 20) ==
           WLR_EDGE_RIGHT);
    /* 全屏窗口不参与 */
    probe_view.fullscreen = true;
    assert(woodland_snap_determine_edges(&probe_view, &probe_area, 12, 300, 20) == 0);
    probe_view.fullscreen = false;

    /* [general] snap_preview = false：判定照旧（松手照样铺满），但不上屏 */
    cfg.general.snap_preview = false;
    server.cursor->x = 12;
    woodland_cursor_process_motion(&server, 0);
    assert(pipe_view.pending_snap_edges == WLR_EDGE_LEFT);
    assert(server.snap_preview.tree == NULL);
    cfg.general.snap_preview = true;
    server.cursor->x = 300;
    woodland_cursor_process_motion(&server, 0);
    assert(server.snap_preview.tree == NULL);

    /* 拖到左沿（工作区 x=10，光标 x=12 在阈值内）：左半屏预览亮起 */
    server.cursor->x = 12;
    woodland_cursor_process_motion(&server, 0);
    assert(pipe_view.pending_snap_edges == WLR_EDGE_LEFT);
    assert(server.snap_preview.edges == WLR_EDGE_LEFT);
    assert(server.snap_preview.tree != NULL && server.snap_preview.tree->node.enabled);
    assert(server.snap_preview.box.x == 10 && server.snap_preview.box.width == 390);
    /* 预览树挂在被拖窗口的同一个父节点下（不是新开一棵根树），
     * 且位置是布局绝对坐标——AGENTS.md 陷阱 3 是"重复累计 tree 自身的 x/y" */
    assert(server.snap_preview.tree->node.parent == pipe_view.scene_tree->node.parent);
    assert(server.snap_preview.tree->node.x == 10);
    assert(server.snap_preview.tree->node.y == 10);
    /*
     * z 序：预览必须紧贴在被拖窗口"下面"（同级兄弟里排在它前面）。
     * 这条是"把 place_below 从每帧挪到建树一次"这个优化的前提——节点
     * 顺序一旦不是这样，预览要么盖住窗口、要么沉到壁纸下面（画了看不见）。
     */
    assert(pipe_view.scene_tree->node.link.prev == &server.snap_preview.tree->node.link);

    /* 光标拖回中央：预览立刻消失，pending 清零（松手不会铺满） */

    /* 样式变更要能穿透"位置没变就跳过"的缓存：预览还亮着的时候改配置，
     * 光标不动也要当帧重设节点属性（边框 2px → 6px，填充内缩跟着变） */
    cfg.general.snap_preview_border_width = 6;
    woodland_cursor_process_motion(&server, 0);
    assert(server.snap_preview.fill != NULL);
    assert(server.snap_preview.fill->node.x == 6);
    assert(server.snap_preview.fill->node.y == 6);
    cfg.general.snap_preview_border_width = 2;
    woodland_cursor_process_motion(&server, 0);
    assert(server.snap_preview.fill->node.x == 2);

    server.cursor->x = 300;
    woodland_cursor_process_motion(&server, 0);
    assert(pipe_view.pending_snap_edges == 0);
    assert(server.snap_preview.edges == 0);
    assert(server.snap_preview.tree != NULL && !server.snap_preview.tree->node.enabled);

    /* 再拖到左沿后松手：按左半屏铺满，预览整棵树拆掉。
     * 松手前摘掉假 toplevel：wlr_xdg_toplevel_set_tiled 会解引用
     * base->client（单测里没有真客户端），而铺满的几何路径不依赖它 */
    server.cursor->x = 12;
    woodland_cursor_process_motion(&server, 0);
    assert(server.snap_preview.edges == WLR_EDGE_LEFT);
    /* 隐藏后重新显示也不重排 z（树复用、父节点没变），顺序仍然是"预览在窗口下" */
    assert(pipe_view.scene_tree->node.link.prev == &server.snap_preview.tree->node.link);
    pipe_view.xdg_toplevel = NULL;
    shell_view_apply_snap(&pipe_view);
    assert(pipe_view.pending_snap_edges == 0);
    assert(pipe_view.snapped_edges == WLR_EDGE_LEFT);
    /* 外框铺到工作区左半屏，内容原点再让出该 view 自己的边距 */
    struct woodland_ssd_margins pipe_margins;
    woodland_ssd_get_margins(&pipe_view, &pipe_margins);
    assert(pipe_view.scene_tree->node.x == 10 + pipe_margins.left);
    assert(pipe_view.scene_tree->node.y == 10 + pipe_margins.top);
    assert(!server.snap_preview.edges);
    assert(server.snap_preview.tree == NULL);

    /* 抓取被打断（例如客户端断开、ESC 取消）时预览也要收干净 */
    pipe_view.xdg_toplevel = &pipe_toplevel;
    server.cursor->x = 12;
    woodland_cursor_process_motion(&server, 0);
    assert(server.snap_preview.tree != NULL);
    /* reset_mode 末尾要把光标换回 "default"，那条路要 xcursor manager；
     * 单测里没有主题目录，建一个空 manager 只为不空指针 */
    server.xcursor_manager = wlr_xcursor_manager_create("Adwaita", 24);
    woodland_cursor_reset_mode(&server);
    assert(server.snap_preview.tree == NULL);
    assert(server.snap_preview.pending_view == NULL);
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);
    if (server.xcursor_manager) {
        wlr_xcursor_manager_destroy(server.xcursor_manager);
        server.xcursor_manager = NULL;
    }

    wl_list_remove(&pipe_view.link);
    wlr_scene_node_destroy(&pipe_view.scene_tree->node);

#if WOODLAND_HAVE_XWAYLAND
    /* ==================== Xwayland toplevel 过滤与任务栏测试 ==================== */
    struct wlr_xwayland_surface xs_normal = {0};
    struct woodland_view xv = {0};
    xv.server = &server;
    xv.type = WOODLAND_VIEW_XWAYLAND;
    xv.xwayland_surface = &xs_normal;
    xv.scene_tree = wlr_scene_tree_create(&server.scene->tree);

    /* 1. 默认 X11 窗口（无特殊 window_type、无 skip_taskbar）应视为 toplevel */
    assert(woodland_view_wants_foreign_toplevel(&xv));

    /* 2. skip_taskbar 的 X11 窗口不应进任务栏 */
    xs_normal.skip_taskbar = true;
    assert(!woodland_view_wants_foreign_toplevel(&xv));
    xs_normal.skip_taskbar = false;
    assert(woodland_view_wants_foreign_toplevel(&xv));

    /* 3. override_redirect 的 X11 窗口不应进任务栏 */
    xs_normal.override_redirect = true;
    assert(!woodland_view_wants_foreign_toplevel(&xv));
    xs_normal.override_redirect = false;
    xv.override_redirect = true;
    assert(!woodland_view_wants_foreign_toplevel(&xv));
    xv.override_redirect = false;
    assert(woodland_view_wants_foreign_toplevel(&xv));

    /* 6. 动态任务栏句柄同步：未 mapped 时调用 update_taskbar_presence 不产生 handle */
    xv.mapped = false;
    woodland_view_update_taskbar_presence(&xv);
    assert(xv.toplevel_handle == NULL);

    /* 7. mapped 且为 toplevel 时自动创建 handle */
    xv.mapped = true;
    woodland_view_update_taskbar_presence(&xv);
    assert(xv.toplevel_handle != NULL);

    /* 8. 动态设置 skip_taskbar 后，update_taskbar_presence 自动销毁 handle */
    xs_normal.skip_taskbar = true;
    woodland_view_update_taskbar_presence(&xv);
    assert(xv.toplevel_handle == NULL);

    /* 9. 取消 skip_taskbar 后重新创建 handle */
    xs_normal.skip_taskbar = false;
    woodland_view_update_taskbar_presence(&xv);
    assert(xv.toplevel_handle != NULL);

    /* 10. unmap 时自动销毁 handle */
    xv.mapped = false;
    woodland_view_update_taskbar_presence(&xv);
    assert(xv.toplevel_handle == NULL);

    wlr_scene_node_destroy(&xv.scene_tree->node);
    /* ==================== X11 位置与 Z 轴同步测试 ==================== */
    /* 1. 真实调用生产函数 woodland_xwayland_initial_position_hint 验证位置提示解析 */
    xs_normal.x = 250;
    xs_normal.y = 180;
    int16_t hx = 0, hy = 0;
    /* 无 hints 时应返回 false */
    xs_normal.size_hints = NULL;
    assert(!woodland_xwayland_initial_position_hint(&xs_normal, &hx, &hy));

    /* flags 没有 US/P 标志时应返回 false */
    xcb_size_hints_t size_hints = {0};
    xs_normal.size_hints = &size_hints;
    assert(!woodland_xwayland_initial_position_hint(&xs_normal, &hx, &hy));

    /* 设置 US_POSITION：返回 true 且坐标匹配 */
    size_hints.flags = XCB_ICCCM_SIZE_HINT_US_POSITION;
    assert(woodland_xwayland_initial_position_hint(&xs_normal, &hx, &hy));
    assert(hx == 250 && hy == 180);

    /* 设置 P_POSITION：返回 true 且坐标匹配 */
    size_hints.flags = XCB_ICCCM_SIZE_HINT_P_POSITION;
    hx = hy = 0;
    assert(woodland_xwayland_initial_position_hint(&xs_normal, &hx, &hy));
    assert(hx == 250 && hy == 180);
    xs_normal.size_hints = NULL;

    /* 2. 真实执行 view_raise：有真实 surface 的父子窗口联动提升测试 */
    server.xwayland_restack_hook = mock_test_restack_hook;

    struct woodland_workspace ws_test = {.server = &server, .name = "test"};
    server.active_workspace = &ws_test;

    struct wlr_surface parent_surf = {0};
    struct wlr_xwayland_surface parent_xs = {.surface = &parent_surf};
    struct woodland_view parent_v = {
        .server = &server,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &parent_xs,
        .mapped = true,
        .workspace = &ws_test,
        .scene_tree = wlr_scene_tree_create(&server.scene->tree),
    };

    struct wlr_surface child_surf = {0};
    struct wlr_xwayland_surface child_xs = {.surface = &child_surf};
    struct woodland_view child_v = {
        .server = &server,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &child_xs,
        .mapped = true,
        .parent = &parent_v,
        .workspace = &ws_test,
        .scene_tree = wlr_scene_tree_create(&server.scene->tree),
    };

    struct wlr_surface other_surf = {0};
    struct wlr_xwayland_surface other_xs = {.surface = &other_surf};
    struct woodland_view other_v = {
        .server = &server,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &other_xs,
        .mapped = true,
        .workspace = &ws_test,
        .scene_tree = wlr_scene_tree_create(&server.scene->tree),
    };

    /* 2.1 基础功能验证：两个独立无关联窗口，点击/聚焦下层窗口必须提升至 Z 轴最顶层 */
    wl_list_insert(&server.views, &other_v.link);
    wl_list_insert(&server.views, &parent_v.link);
    assert(server.views.next == &parent_v.link);
    assert(parent_v.link.next == &other_v.link);

    /* 聚焦下层的 other_v：必须提升到链表头且高于 parent_v */
    woodland_focus_view(&other_v, true);
    assert(server.views.next == &other_v.link);
    assert(other_v.link.next == &parent_v.link);

    /* 再聚焦 parent_v：必须重新回到最顶层 */
    woodland_focus_view(&parent_v, true);
    assert(server.views.next == &parent_v.link);
    assert(parent_v.link.next == &other_v.link);

    wl_list_remove(&other_v.link);
    wl_list_remove(&parent_v.link);

    /* 2.2 父子联动提升与层叠层级验证：
     * 初始顺序：other_v (最上) -> parent_v -> child_v (最下) */
    wl_list_insert(&server.views, &child_v.link);
    wl_list_insert(&server.views, &parent_v.link);
    wl_list_insert(&server.views, &other_v.link);
    assert(server.views.next == &other_v.link);

    /* 聚焦父窗口 parent_v 并提升：
     * parent_v 与 child_v 必须联动提升到 other_v 之上，且 child_v 必须保持在 parent_v 上方 */
    mock_restack_calls = 0;
    woodland_focus_view(&parent_v, true);
    assert(mock_restack_calls > 0);
    assert(server.views.next == &child_v.link);
    assert(child_v.link.next == &parent_v.link);
    assert(parent_v.link.next == &other_v.link);

    /* 聚焦外部 unrelated 窗口 other_v：other_v 提升到最顶层 */
    woodland_focus_view(&other_v, true);
    assert(server.views.next == &other_v.link);
    assert(other_v.link.next == &child_v.link);
    assert(child_v.link.next == &parent_v.link);

    /* 聚焦子窗口 child_v：child_v 提升到最顶层，parent_v 仍在 other_v 之上 */
    woodland_focus_view(&child_v, true);
    assert(server.views.next == &child_v.link);
    assert(child_v.link.next == &parent_v.link);
    assert(parent_v.link.next == &other_v.link);

    server.xwayland_restack_hook = NULL;
    wl_list_remove(&other_v.link);
    wl_list_remove(&child_v.link);
    wl_list_remove(&parent_v.link);
    wlr_scene_node_destroy(&other_v.scene_tree->node);
    wlr_scene_node_destroy(&child_v.scene_tree->node);
    wlr_scene_node_destroy(&parent_v.scene_tree->node);
#endif
    woodland_snap_preview_finish(&server);

    wlr_cursor_destroy(server.cursor);
    server.cursor = NULL;
    woodland_output_finish(&server);

    /* 逆序释放 wlr 资源 */
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
