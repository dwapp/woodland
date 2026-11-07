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
#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

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

    /* 左上角附近：外框 (12,15) 距工作区边缘 2/5px → 吸附到 (10,10) */
    server.cursor->x = 13;
    server.cursor->y = 42;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 10 + margins.left); /* 11 */
    assert(drag_view.scene_tree->node.y == 10 + margins.top);  /* 37 */

    /* 中间空地：x/y 都离候选线 > 阈值 → 不吸附，内容原点=光标位置 */
    server.cursor->x = 300;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 300);
    assert(drag_view.scene_tree->node.y == 200);

    /* 右边框附近：外框右边贴住工作区右边界（790 - 402 = 388） */
    server.cursor->x = 386;
    server.cursor->y = 200;
    woodland_cursor_process_motion(&server, 0);
    assert(drag_view.scene_tree->node.x == 388 + margins.left); /* 389 */

    /* 贴邻窗：第二个窗口内容 (500,100) 200x200，左外边缘 499；
     * 我们的外框 x 拖到 494（差 5px）→ 吸附到 499 */
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

    server.cursor->x = 495;
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
    struct woodland_view preview_view = {0};
    preview_view.server = &server;
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
