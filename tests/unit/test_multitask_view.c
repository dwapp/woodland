// SPDX-License-Identifier: GPL-2.0-only
#include "multitask_view.h"
#include "output.h"
#include "scene.h"
#include "ssd.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

static void test_compute_grid(void)
{
    struct wlr_box area = {.x = 40, .y = 88, .width = 1200, .height = 600};
    int cols = -1, rows = -1;
    struct wlr_box boxes[16];

    woodland_multitask_compute_grid(&area, 0, &cols, &rows, boxes);
    assert(cols == 0 && rows == 0);

    woodland_multitask_compute_grid(&area, 1, &cols, &rows, boxes);
    assert(cols == 1 && rows == 1);
    assert(boxes[0].width > 0 && boxes[0].height > 0);
    assert(boxes[0].x >= area.x && boxes[0].y >= area.y);
    assert(boxes[0].x + boxes[0].width <= area.x + area.width);
    assert(boxes[0].y + boxes[0].height <= area.y + area.height);

    woodland_multitask_compute_grid(&area, 2, &cols, &rows, boxes);
    assert(cols == 2 && rows == 1);
    assert(boxes[0].x + boxes[0].width < boxes[1].x);

    woodland_multitask_compute_grid(&area, 4, &cols, &rows, boxes);
    assert(cols == 2 && rows == 2);
    assert(boxes[0].y == boxes[1].y);
    assert(boxes[2].y > boxes[0].y + boxes[0].height);

    woodland_multitask_compute_grid(&area, 5, &cols, &rows, boxes);
    assert(cols == 3 && rows == 2);
    /* 最后一行只有 2 张卡片，应当水平居中（第 4 张的 x 大于第一行第 1 张的 x） */
    assert(boxes[3].x > boxes[0].x);
}

int main(void)
{
    test_compute_grid();

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

    struct wlr_output *out = wlr_headless_add_output(server.backend, 1280, 720);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    server.cursor = wlr_cursor_create();
    assert(server.cursor);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wl_list_init(&server.workspaces);
    wl_list_init(&server.views);
    wl_signal_init(&server.workspace_changed);

    struct woodland_workspace ws1 = {0};
    ws1.name = strdup("1");
    ws1.server = &server;
    ws1.tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(server.workspaces.prev, &ws1.link);

    struct woodland_workspace ws2 = {0};
    ws2.name = strdup("2");
    ws2.server = &server;
    ws2.tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_set_enabled(&ws2.tree->node, false);
    wl_list_insert(server.workspaces.prev, &ws2.link);

    server.active_workspace = &ws1;

    assert(woodland_multitask_view_init(&server));
    assert(!woodland_multitask_view_is_active(&server));

    struct wlr_xdg_surface fake_base1 = {0};
    fake_base1.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 400, .height = 300};
    struct wlr_xdg_toplevel fake_tl1 = {.base = &fake_base1};

    struct woodland_view v1 = {0};
    v1.id = 1;
    v1.server = &server;
    v1.workspace = &ws1;
    v1.mapped = true;
    v1.type = WOODLAND_VIEW_XDG;
    v1.xdg_toplevel = &fake_tl1;
    v1.title = strdup("Window One");
    v1.scene_tree = wlr_scene_tree_create(ws1.tree);
    wl_list_insert(&server.views, &v1.link);

    struct wlr_xdg_surface fake_base2 = {0};
    fake_base2.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 500, .height = 360};
    struct wlr_xdg_toplevel fake_tl2 = {.base = &fake_base2};

    struct woodland_view v2 = {0};
    v2.id = 2;
    v2.server = &server;
    v2.workspace = &ws1;
    v2.mapped = true;
    v2.minimized = true;
    v2.type = WOODLAND_VIEW_XDG;
    v2.xdg_toplevel = &fake_tl2;
    v2.title = strdup("Window Two");
    v2.scene_tree = wlr_scene_tree_create(ws1.tree);
    wlr_scene_node_set_enabled(&v2.scene_tree->node, false);
    wl_list_insert(server.views.prev, &v2.link);

    server.focused_view = &v1;

    /* 1. 打开多任务视图并验证 JSON 状态 */
    woodland_multitask_view_open(&server);
    assert(woodland_multitask_view_is_active(&server));
    assert(server.multitask_view->card_count == 2);
    assert(server.multitask_view->selected_index == 0);

    struct woodland_json json;
    woodland_json_init(&json);
    woodland_multitask_view_status(&server, &json);
    assert(strstr(woodland_json_data(&json), "\"active\":true") != NULL);
    assert(strstr(woodland_json_data(&json), "\"card_count\":2") != NULL);
    assert(strstr(woodland_json_data(&json), "\"selected_view\":1") != NULL);
    woodland_json_finish(&json);

    /* 2. 键盘导航切换选中项并按 Enter 激活（含自动恢复最小化窗口 v2） */
    assert(woodland_multitask_view_handle_key(&server, XKB_KEY_Right, 0));
    assert(server.multitask_view->selected_index == 1);
    assert(woodland_multitask_view_handle_key(&server, XKB_KEY_Return, 0));
    assert(!woodland_multitask_view_is_active(&server));
    assert(!v2.minimized);
    assert(server.focused_view == &v2);

    /* 3. Toggle 打开 + Escape 取消关闭 */
    woodland_multitask_view_toggle(&server);
    assert(woodland_multitask_view_is_active(&server));
    assert(woodland_multitask_view_handle_key(&server, XKB_KEY_Escape, 0));
    assert(!woodland_multitask_view_is_active(&server));

    /* 4. 顶栏点击切换工作区 + 空白处点击关闭 */
    woodland_multitask_view_open(&server);
    assert(server.multitask_view->workspace_count == 2);
    struct wlr_box ws2_box = server.multitask_view->workspaces[1].box;
    woodland_multitask_view_handle_motion(&server, ws2_box.x + 5, ws2_box.y + 5);
    assert(server.multitask_view->hovered_workspace == 1);
    assert(woodland_multitask_view_handle_button(
        &server, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, ws2_box.x + 5, ws2_box.y + 5));
    /* 点击工作区 2 后保持多任务视图打开，且切换到空工作区 2 */
    assert(woodland_multitask_view_is_active(&server));
    assert(server.active_workspace == &ws2);
    assert(server.multitask_view->card_count == 0);

    /* 数字键 '1' 切回工作区 1 */
    assert(woodland_multitask_view_handle_key(&server, XKB_KEY_1, 0));
    assert(server.active_workspace == &ws1);
    assert(server.multitask_view->card_count == 2);

    /* 视图在中途 unmap 时自动移除卡片 */
    wl_list_remove(&v2.link);
    v2.mapped = false;
    woodland_multitask_view_handle_view_unmap(&v2);
    assert(server.multitask_view->card_count == 1);
    assert(server.multitask_view->selected_index == 0);

    /* 点击空白处 (2, 2) 退出 */
    assert(woodland_multitask_view_handle_button(
        &server, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 2, 2));
    assert(!woodland_multitask_view_is_active(&server));

    woodland_multitask_view_finish(&server);
    assert(server.multitask_view == NULL);

    free(v1.title);
    free(v2.title);
    free(ws1.name);
    free(ws2.name);
    wlr_cursor_destroy(server.cursor);
    woodland_output_finish(&server);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    printf("multitask_view unit passed\n");
    return 0;
}
