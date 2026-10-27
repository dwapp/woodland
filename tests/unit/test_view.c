// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"

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
    struct woodland_output *wo =
        wl_container_of(server.outputs.next, wo, link);
    wo->usable_area = (struct wlr_box){ .x = 10, .y = 10, .width = 780, .height = 580 };
    wo->usable_area_valid = true;
    struct wlr_box max_geo2 = woodland_view_maximized_geometry(&server);
    assert(max_geo2.x == 10 && max_geo2.y == 10);
    assert(max_geo2.width == 780 && max_geo2.height == 580);

    /*
     * 放置：外框（内容 + SSD 边距）整体落进工作区。
     * 内容原点必须让出左上边距，否则标题栏会顶到屏幕外。
     */
    struct woodland_ssd_margins margins = {
        .left = 1, .top = 27, .right = 1, .bottom = 1,
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
    int edges[] = { 400, 700 };

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
    struct wlr_box parent_box = { .x = 200, .y = 150, .width = 400, .height = 300 };

    /* 居中：外框 202x148 → 父框中心 (400,300) → 外框原点 (299,226) */
    woodland_view_center_on(&parent_box, 200, 120, &margins, &max_geo2, &place);
    assert(place.x == 300 && place.y == 253);
    assert(place.width == 200 && place.height == 120);

    /* 父窗口贴右下角：居中会越界，贴回工作区右下 */
    parent_box = (struct wlr_box){ .x = 770, .y = 560, .width = 400, .height = 300 };
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
    drag_base.geometry = (struct wlr_box){ .width = 400, .height = 300 };
    struct wlr_xdg_toplevel drag_toplevel = { .base = &drag_base };
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
    assert(drag_view.scene_tree->node.x
        == 10 + margins.left);       /* 11 */
    assert(drag_view.scene_tree->node.y
        == 10 + margins.top);        /* 37 */

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
    peer_base.geometry = (struct wlr_box){ .width = 200, .height = 200 };
    struct wlr_xdg_toplevel peer_toplevel = { .base = &peer_base };
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
