// SPDX-License-Identifier: GPL-2.0-only
/*
 * 位置管理单测：输出查找、放置策略、约束、贴边铺满、尺寸限制。
 * 全部走纯函数，不需要合成器跑起来。
 */
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "scene.h"
#include <wlr/util/edges.h>

#include "layer.h"
#include "output.h"
#include "placement.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"

static struct woodland_output *add_output(struct woodland_server *server,
                                          int w, int h, int x, int y) {
    struct wlr_output *wlr_output = wlr_headless_add_output(server->backend, w, h);
    assert(wlr_output != NULL);
    wl_signal_emit_mutable(&server->backend->events.new_output, wlr_output);
    wlr_output_layout_add(server->output_layout, wlr_output, x, y);
    /* 找到刚加进来的 woodland_output（链表头是最后一个走完 init 的） */
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        if (wo->wlr_output == wlr_output) {
            return wo;
        }
    }
    assert(false);
    return NULL;
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
    assert(woodland_output_init(&server));

    struct woodland_output *o1 = add_output(&server, 800, 600, 0, 0);
    struct wlr_box area;
    woodland_output_workarea(o1, &area);
    assert(area.x == 0 && area.y == 0 && area.width == 800 && area.height == 600);

    /* 与真实 SSD 边距保持一致（边框 1 + 标题栏），避免测试里写死一个过期的高度 */
    struct woodland_ssd_margins margins = {
        .left = WOODLAND_SSD_BORDER_W,
        .top = WOODLAND_SSD_BORDER_W + WOODLAND_SSD_TITLEBAR_H,
        .right = WOODLAND_SSD_BORDER_W,
        .bottom = WOODLAND_SSD_BORDER_W,
    };

    /* ---------- 贴边铺满 ---------- */
    struct wlr_box tile;
    /* 左边：半屏，内容原点让出左边距，外框左上贴工作区左上 */
    assert(woodland_snap_tile_box(&area, WLR_EDGE_LEFT, &tile));
    assert(tile.x == 0 && tile.y == 0);
    assert(tile.width == 400 && tile.height == 600);
    /* 右边：贴右半屏 */
    assert(woodland_snap_tile_box(&area, WLR_EDGE_RIGHT, &tile));
    assert(tile.x == 400 && tile.width == 400);
    /* 上边单独：整块工作区（最大化语义） */
    assert(woodland_snap_tile_box(&area, WLR_EDGE_TOP, &tile));
    assert(tile.x == 0 && tile.y == 0);
    assert(tile.width == 800 && tile.height == 600);
    /* 左上角：四分之一屏 */
    assert(woodland_snap_tile_box(&area, WLR_EDGE_LEFT | WLR_EDGE_TOP, &tile));
    assert(tile.width == 400 && tile.height == 300);
    /* 右下角：四分之一屏贴右下 */
    assert(woodland_snap_tile_box(&area, WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM, &tile));
    assert(tile.x == 400 && tile.y == 300);
    assert(tile.width == 400 && tile.height == 300);
    /* 左右同时贴住：没有半屏语义 */
    assert(!woodland_snap_tile_box(&area, WLR_EDGE_LEFT | WLR_EDGE_RIGHT, &tile));
    /* 没有边：不铺满 */
    assert(!woodland_snap_tile_box(&area, 0, &tile));

    /* ---------- 光标贴边触发判定（唯一触发条件，阈值 20） ---------- */
    /* 1. 左沿中间（height=600，角区域为 <150 和 >450，中间 150~450） */
    assert(woodland_snap_cursor_edges(&area, 5, 300, 20) == WLR_EDGE_LEFT);
    /* 2. 左沿上部（y=50 <= 150） -> 左上 1/4 屏 */
    assert(woodland_snap_cursor_edges(&area, 10, 50, 20) == (WLR_EDGE_LEFT | WLR_EDGE_TOP));
    /* 3. 左沿下部（y=550 >= 450） -> 左下 1/4 屏 */
    assert(woodland_snap_cursor_edges(&area, 15, 550, 20) == (WLR_EDGE_LEFT | WLR_EDGE_BOTTOM));
    /* 4. 右沿中间 -> 右半屏 */
    assert(woodland_snap_cursor_edges(&area, 795, 300, 20) == WLR_EDGE_RIGHT);
    /* 5. 右沿上部 -> 右上 1/4 屏 */
    assert(woodland_snap_cursor_edges(&area, 785, 100, 20) == (WLR_EDGE_RIGHT | WLR_EDGE_TOP));
    /* 6. 右沿下部 -> 右下 1/4 屏 */
    assert(woodland_snap_cursor_edges(&area, 790, 500, 20) == (WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM));
    /* 7. 上沿中间（不在左右边缘阈值内） -> 最大化 (TOP) */
    assert(woodland_snap_cursor_edges(&area, 400, 10, 20) == WLR_EDGE_TOP);
    /* 8. 屏幕中央 -> 不触发 */
    assert(woodland_snap_cursor_edges(&area, 400, 300, 20) == 0);
    /* 9. 略超出屏幕边缘（拖到边缘外面 5px） -> 依然触发 */
    assert(woodland_snap_cursor_edges(&area, -5, 300, 20) == WLR_EDGE_LEFT);
    assert(woodland_snap_cursor_edges(&area, 400, -5, 20) == WLR_EDGE_TOP);
    /* 10. 距离边缘超过阈值（x=30 > 20） -> 不触发 */
    assert(woodland_snap_cursor_edges(&area, 30, 300, 20) == 0);

    /* ---------- 约束 ---------- */
    struct wlr_box box = { .x = -50, .y = -80, .width = 300, .height = 200 };
    woodland_box_constrain(&box, &area);
    assert(box.x == 0 && box.y == 0);
    box = (struct wlr_box){ .x = 700, .y = 500, .width = 300, .height = 200 };
    woodland_box_constrain(&box, &area);
    assert(box.x == 500 && box.y == 400);
    /* 比工作区大：贴左上，不让左上角跑出去 */
    box = (struct wlr_box){ .x = 100, .y = 100, .width = 900, .height = 700 };
    woodland_box_constrain(&box, &area);
    assert(box.x == 0 && box.y == 0);

    /* ---------- 放置策略 ---------- */
    struct wlr_box place;
    /*
     * 居中：外框 (400+左+右) x (300+上+下) 居中 → 外框原点，
     * 内容原点再加左上边距 (margins.left, margins.top)
     */
    woodland_placement_center(&area, 400, 300, &margins, &place);
    assert(place.x == (800 - (400 + margins.left + margins.right)) / 2 + margins.left);
    assert(place.y == (600 - (300 + margins.top + margins.bottom)) / 2 + margins.top);
    assert(place.width == 400 && place.height == 300);

    /* 光标处：外框左上角贴光标 */
    woodland_placement_cursor(&area, 100, 120, 300, 200, &margins, &place);
    assert(place.x == 100 + margins.left && place.y == 120 + margins.top);
    /* 光标贴右下角：整体拉回工作区内 */
    woodland_placement_cursor(&area, 790, 590, 300, 200, &margins, &place);
    assert(place.x + place.width + margins.right <= 800);
    assert(place.y + place.height + margins.bottom <= 600);

    /*
     * 自动放置：已有窗口占满左上 400x300，候选里应当选不与它重叠的位置。
     * 期望结果在左侧之外（x >= 400）或下方（y >= 300）。
     */
    struct wlr_box obstacles[1] = {
        { .x = 0, .y = 0, .width = 400, .height = 300 },
    };
    woodland_placement_autoplace(&area, 300, 200, &margins, obstacles, 1, 0, &place);
    {
        struct wlr_box cand = {
            .x = place.x - margins.left,
            .y = place.y - margins.top,
            .width = place.width + margins.left + margins.right,
            .height = place.height + margins.top + margins.bottom,
        };
        bool no_overlap = (cand.x >= 400) || (cand.y >= 300);
        assert(no_overlap);
    }

    /* ---------- 多输出 ---------- */
    struct woodland_output *o2 = add_output(&server, 800, 600, 800, 0);
    struct wlr_box area2;
    woodland_output_workarea(o2, &area2);
    assert(area2.x == 800 && area2.width == 800);

    struct wlr_box hit;
    woodland_workarea_at(&server, 900, 100, &hit);
    assert(hit.x == 800);
    woodland_workarea_at(&server, 100, 100, &hit);
    assert(hit.x == 0);
    /* 输出之间的空隙：退化为最近/首个输出，不会给出零矩形 */
    woodland_workarea_at(&server, 5000, 5000, &hit);
    assert(hit.width > 0 && hit.height > 0);

    assert(woodland_output_at(&server, 900, 100) == o2);
    assert(woodland_output_at(&server, 100, 100) == o1);
    assert(woodland_output_at(&server, 5000, 5000) == NULL);

    /* 右/下侧的副屏：closest_point 返回的最近点在框内距右边 1/256 处，
     * 拿它加点偏移反查输出会探到框外。以前这里会退回首输出，把右副屏上
     * 的窗口按主屏工作区摆放（最大化/全屏摆错屏） */
    struct woodland_output *o3 = add_output(&server, 800, 600, 2400, 0);
    assert(woodland_output_nearest(&server, 100, 300) == o1);
    /* 空隙里：离 o3 左边更近 */
    assert(woodland_output_nearest(&server, 2300, 300) == o3);
    /* 拖到最右输出的外面：仍应判给 o3，而不是首输出 */
    assert(woodland_output_nearest(&server, 3000, 300) == o3);
    woodland_workarea_at(&server, 3000, 300, &hit);
    assert(hit.x == 2400 && hit.width == 800);

    /* 最大化几何按输出算：副屏最大化不该跑到主屏 */
    struct wlr_box max2 = woodland_view_maximized_geometry_on(&server, o2);
    assert(max2.x == 800 && max2.width == 800);
    struct wlr_box max1 = woodland_view_maximized_geometry_on(&server, o1);
    assert(max1.x == 0 && max1.width == 800);

    /*
     * ---------- 输出边的"布局外沿 vs 显示器接缝" ----------
     * 布局 o1[0,800) o2[800,1600) 空隙 o3[2400,3200)。
     * 接缝（外面还有显示器）要防抖，外沿与空隙可以立刻给预览。
     */
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o1->wlr_output, WLR_EDGE_LEFT));
    /* o1 右边紧挨 o2：是接缝 */
    assert(!woodland_snap_edge_is_layout_edge(server.output_layout, o1->wlr_output,
                                              WLR_EDGE_RIGHT));
    /* 没有任何显示器在 o1 上下：外沿 */
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o1->wlr_output, WLR_EDGE_TOP));
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o1->wlr_output,
                                             WLR_EDGE_BOTTOM));
    /* o2 左边是 o1（接缝），右边是空隙（无输出 → 按外沿处理） */
    assert(!woodland_snap_edge_is_layout_edge(server.output_layout, o2->wlr_output,
                                              WLR_EDGE_LEFT));
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o2->wlr_output,
                                             WLR_EDGE_RIGHT));
    /* 最右输出：左邻近空隙、右边是墙 */
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o3->wlr_output, WLR_EDGE_LEFT));
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o3->wlr_output,
                                             WLR_EDGE_RIGHT));
    /* 拿不到布局/输出：按外沿处理（宁可少一次防抖，也不要永远不出预览） */
    assert(woodland_snap_edge_is_layout_edge(NULL, o1->wlr_output, WLR_EDGE_LEFT));
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, NULL, WLR_EDGE_LEFT));
    /* 非单边（例如 LEFT|TOP 传进来）：不参与判定，直接按外沿 */
    assert(woodland_snap_edge_is_layout_edge(server.output_layout, o1->wlr_output,
                                              WLR_EDGE_LEFT | WLR_EDGE_TOP));

    /* ---------- 尺寸限制（无客户端时是 1/1/无上限） ---------- */
    struct woodland_view fake_view = {0};
    int min_w, min_h, max_w, max_h;
    woodland_view_size_limits(&fake_view, &min_w, &min_h, &max_w, &max_h);
    assert(min_w == 1 && min_h == 1 && max_w == 0 && max_h == 0);

    /* ---------- 策略名解析 ---------- */
    enum woodland_placement_policy policy;
    assert(woodland_placement_policy_parse("autoplace", &policy));
    assert(policy == WOODLAND_PLACE_AUTOPLACE);
    assert(woodland_placement_policy_parse("center", &policy));
    assert(policy == WOODLAND_PLACE_CENTER);
    assert(woodland_placement_policy_parse("cursor", &policy));
    assert(policy == WOODLAND_PLACE_CURSOR);
    assert(woodland_placement_policy_parse("cascade", &policy));
    assert(policy == WOODLAND_PLACE_CASCADE);
    assert(!woodland_placement_policy_parse("nonsense", &policy));
    assert(strcmp(woodland_placement_policy_name(WOODLAND_PLACE_AUTOPLACE),
                  "autoplace") == 0);

    /* ---------- 浮动状态判定与非浮动窗口不重放 ---------- */
    struct woodland_view v = {0};
    assert(woodland_view_is_floating(&v));
    v.fullscreen = true;
    assert(!woodland_view_is_floating(&v));
    v.fullscreen = false;
    v.maximized = true;
    assert(!woodland_view_is_floating(&v));
    v.maximized = false;
    v.snapped_edges = 1;
    assert(!woodland_view_is_floating(&v));

    /* 全屏窗口调用 woodland_view_place_initial 绝不修改场景节点位置（避让 workarea 的 bug 修复） */
    v.fullscreen = true;
    v.server = &server;
    v.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&v.scene_tree->node, 0, 0);
    woodland_view_place_initial(&v);
    assert(v.scene_tree->node.x == 0);
    assert(v.scene_tree->node.y == 0);
    wlr_scene_node_destroy(&v.scene_tree->node);

    /* ---------- 标题栏约束（纯函数） ---------- */
    struct wlr_box workarea = { .x = 0, .y = 40, .width = 800, .height = 520 };
    int tb_h = 33;
    /* 1. 完全在顶部之上（如顶部新出现 40px 面板）-> 拉回 workarea.y */
    struct wlr_box tb_box = { .x = 100, .y = 0, .width = 300, .height = 200 };
    woodland_box_constrain_titlebar(&tb_box, tb_h, &workarea);
    assert(tb_box.x == 100 && tb_box.y == 40);
    /* 2. 完全在底部之下（y >= 40 + 520 = 560）-> 拉回 560 - 33 = 527 */
    tb_box = (struct wlr_box){ .x = 100, .y = 570, .width = 300, .height = 200 };
    woodland_box_constrain_titlebar(&tb_box, tb_h, &workarea);
    assert(tb_box.x == 100 && tb_box.y == 527);
    /* 3. 完全在左侧之外 -> 拉回 workarea.x */
    tb_box = (struct wlr_box){ .x = -350, .y = 100, .width = 300, .height = 200 };
    woodland_box_constrain_titlebar(&tb_box, tb_h, &workarea);
    assert(tb_box.x == 0 && tb_box.y == 100);
    /* 4. 完全在右侧之外 -> 拉回 workarea.x + width - box.width */
    tb_box = (struct wlr_box){ .x = 850, .y = 100, .width = 300, .height = 200 };
    woodland_box_constrain_titlebar(&tb_box, tb_h, &workarea);
    assert(tb_box.x == 500 && tb_box.y == 100);
    /* 5. 仅部分超出底边与右边（未完全超出）-> 原地不动 */
    tb_box = (struct wlr_box){ .x = 700, .y = 545, .width = 300, .height = 200 };
    woodland_box_constrain_titlebar(&tb_box, tb_h, &workarea);
    assert(tb_box.x == 700 && tb_box.y == 545);

    /* ---------- 可用区域变化决策纯函数（最大化 / 全屏 / 贴边 / 浮动） ---------- */
    struct wlr_box small_area = { .x = 0, .y = 40, .width = 800, .height = 520 };
    struct wlr_box full_800 = { .x = 0, .y = 0, .width = 800, .height = 600 };
    struct wlr_box target = {0};

    /* 1. 最大化：工作区变小与变大均触发 RESIZE，已匹配则 NONE */
    struct woodland_view tv = { .server = &server, .maximized = true };
    struct wlr_box cur = { .x = 0, .y = 0, .width = 800, .height = 600 };
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_RESIZE);
    assert(wlr_box_equal(&target, &small_area));
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &small_area, &target) ==
           WOODLAND_WORKAREA_NONE);
    /* 面积变大（800x520 -> 800x600） */
    assert(woodland_view_workarea_target(&tv, &full_800, &full_800, &small_area, &target) ==
           WOODLAND_WORKAREA_RESIZE);
    assert(wlr_box_equal(&target, &full_800));

    /* 2. 全屏：有效 full_area 触发 RESIZE，无效全 0 矩形则返回 NONE（不会缩成 1x1） */
    tv = (struct woodland_view){ .server = &server, .fullscreen = true };
    cur = (struct wlr_box){ .x = 0, .y = 0, .width = 640, .height = 480 };
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_RESIZE);
    assert(wlr_box_equal(&target, &full_800));
    struct wlr_box zero_box = {0};
    assert(woodland_view_workarea_target(&tv, &small_area, &zero_box, &cur, &target) ==
           WOODLAND_WORKAREA_NONE);

    /* 3. 贴边：合法边触发 RESIZE 到对应贴边框；非法边组合直接 NONE（不掉进浮动分支） */
    tv = (struct woodland_view){ .server = &server, .snapped_edges = WLR_EDGE_LEFT };
    cur = (struct wlr_box){ .x = 0, .y = 0, .width = 400, .height = 600 };
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_RESIZE);
    assert(target.x == 0 && target.y == 40 && target.width == 400 && target.height == 520);
    tv.snapped_edges = WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_NONE);

    /* 4. 浮动与 grabbed_view 跳过 */
    tv = (struct woodland_view){ .server = &server };
    cur = (struct wlr_box){ .x = 100, .y = 0, .width = 300, .height = 200 };
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_MOVE);
    assert(target.x == 100 && target.y == 40);
    server.grabbed_view = &tv;
    assert(woodland_view_workarea_target(&tv, &small_area, &full_800, &cur, &target) ==
           WOODLAND_WORKAREA_NONE);
    server.grabbed_view = NULL;

    /* ---------- 可用区域变化后同步最大化窗口与浮动窗口位置 ---------- */
    wl_list_init(&server.views);
    o1->usable_area = (struct wlr_box){ .x = 0, .y = 40, .width = 800, .height = 520 };
    o1->usable_area_valid = true;

    /* 最大化窗口：同步新工作区尺寸与位置 */
    struct wlr_xdg_surface max_base = {0};
    max_base.geometry = (struct wlr_box){ .width = 800, .height = 600 - WOODLAND_SSD_TITLEBAR_H };
    struct wlr_xdg_toplevel max_tl = { .base = &max_base };
    struct woodland_view max_v = {0};
    max_v.server = &server;
    max_v.xdg_toplevel = &max_tl;
    max_v.mapped = true;
    max_v.maximized = true;
    max_v.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&max_v.scene_tree->node, 0, WOODLAND_SSD_TITLEBAR_H);
    wl_list_insert(&server.views, &max_v.link);

    /* SSD 浮动窗口：原本在 y=0 处，顶部新增 40px 面板后标题栏完全被盖住 -> 下移至 y=40 */
    struct wlr_xdg_surface ssd_base = {0};
    ssd_base.geometry = (struct wlr_box){ .width = 300, .height = 200 };
    struct wlr_xdg_toplevel ssd_tl = { .base = &ssd_base };
    struct woodland_view ssd_v = {0};
    ssd_v.server = &server;
    ssd_v.xdg_toplevel = &ssd_tl;
    ssd_v.mapped = true;
    ssd_v.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&ssd_v.scene_tree->node, 100 + margins.left, 0 + margins.top);
    wl_list_insert(&server.views, &ssd_v.link);

    /* CSD 浮动窗口：原本在 y=560 处，底部缩窄到 560 后标题栏完全在屏幕外 -> 上移露出近似 SSD 标题栏 */
    struct wlr_xdg_surface csd_base = {0};
    csd_base.geometry = (struct wlr_box){ .width = 300, .height = 200 };
    struct wlr_xdg_toplevel csd_tl = { .base = &csd_base };
    struct woodland_view csd_v = {0};
    csd_v.server = &server;
    csd_v.xdg_toplevel = &csd_tl;
    csd_v.mapped = true;
    csd_v.deco_csd = true;
    csd_v.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&csd_v.scene_tree->node, 100, 560);
    wl_list_insert(&server.views, &csd_v.link);

    /* 未 mapped 视图：即使标题栏在屏幕外也不应被移动 */
    struct wlr_xdg_surface unmapped_base = {0};
    unmapped_base.geometry = (struct wlr_box){ .width = 300, .height = 200 };
    struct wlr_xdg_toplevel unmapped_tl = { .base = &unmapped_base };
    struct woodland_view unmapped_v = {0};
    unmapped_v.server = &server;
    unmapped_v.xdg_toplevel = &unmapped_tl;
    unmapped_v.mapped = false;
    unmapped_v.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_position(&unmapped_v.scene_tree->node, 100, -500);
    wl_list_insert(&server.views, &unmapped_v.link);

    /* 输出正在销毁时守卫：直接跳过，不动任何视图 */
    o1->destroying = true;
    woodland_shell_handle_output_workarea_change(&server, o1);
    assert(max_v.scene_tree->node.y == WOODLAND_SSD_TITLEBAR_H);
    o1->destroying = false;

    woodland_shell_handle_output_workarea_change(&server, o1);

    /* 最大化窗口位置已移到 y=40 + titlebar_h */
    assert(max_v.scene_tree->node.x == 0);
    assert(max_v.scene_tree->node.y == 40 + WOODLAND_SSD_TITLEBAR_H);
    /* SSD 浮动窗口外框 y 移到 40，内容 y = 40 + margins.top */
    assert(ssd_v.scene_tree->node.x == 100 + margins.left);
    assert(ssd_v.scene_tree->node.y == 40 + margins.top);
    /* CSD 浮动窗口按 WOODLAND_SSD_TITLEBAR_H 近似标题栏高度拉回到 560 - WOODLAND_SSD_TITLEBAR_H */
    assert(csd_v.scene_tree->node.x == 100);
    assert(csd_v.scene_tree->node.y == 40 + 520 - WOODLAND_SSD_TITLEBAR_H);
    /* 未 mapped 视图保持原位 */
    assert(unmapped_v.scene_tree->node.y == -500);

    /* 验证 layer.c 接线：woodland_layer_arrange 检测到 usable_area 恢复 800x600 后自动触发重排 */
    woodland_layer_arrange(&server, o1);
    assert(o1->usable_area.x == 0 && o1->usable_area.y == 0 &&
           o1->usable_area.width == 800 && o1->usable_area.height == 600);
    assert(max_v.scene_tree->node.x == 0);
    assert(max_v.scene_tree->node.y == WOODLAND_SSD_TITLEBAR_H);

    wl_list_remove(&unmapped_v.link);
    wl_list_remove(&csd_v.link);
    wl_list_remove(&ssd_v.link);
    wl_list_remove(&max_v.link);
    wlr_scene_node_destroy(&unmapped_v.scene_tree->node);
    wlr_scene_node_destroy(&csd_v.scene_tree->node);
    wlr_scene_node_destroy(&ssd_v.scene_tree->node);
    wlr_scene_node_destroy(&max_v.scene_tree->node);

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
