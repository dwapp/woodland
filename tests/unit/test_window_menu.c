// SPDX-License-Identifier: GPL-2.0-only
#include "output.h"
#include "scene.h"
#include "ssd.h"
#include "util/json.h"
#include "view.h"
#include "window_menu.h"
#include "woodland.h"
#include "workspace.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

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

    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    server.cursor = wlr_cursor_create();
    assert(server.cursor);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wl_list_init(&server.workspaces);
    wl_list_init(&server.views);

    /* 模拟两个工作区 */
    struct woodland_workspace ws1 = {0};
    ws1.name = strdup("1");
    ws1.server = &server;
    ws1.tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(server.workspaces.prev, &ws1.link);

    struct woodland_workspace ws2 = {0};
    ws2.name = strdup("2");
    ws2.server = &server;
    ws2.tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(server.workspaces.prev, &ws2.link);

    server.active_workspace = &ws1;

    /* 初始化窗口菜单 */
    assert(woodland_window_menu_init(&server));
    assert(!woodland_window_menu_is_active(&server));

    /* 模拟一个视图 */
    struct wlr_xdg_surface fake_base = {0};
    fake_base.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 400, .height = 300};
    struct wlr_xdg_toplevel fake_toplevel = {.base = &fake_base};

    struct woodland_view view = {0};
    view.id = 1;
    view.server = &server;
    view.workspace = &ws1;
    view.type = WOODLAND_VIEW_XDG;
    view.xdg_toplevel = &fake_toplevel;
    view.scene_tree = wlr_scene_tree_create(ws1.tree);
    assert(view.scene_tree);
    wlr_scene_node_set_position(&view.scene_tree->node, 100, 100);

    /* 1. 弹出菜单测试 */
    woodland_window_menu_show(&view, 150, 80);
    assert(woodland_window_menu_is_active(&server));

    /* 状态序列化测试 */
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_window_menu_status(&server, &json);
    const char *status_str = woodland_json_data(&json);
    assert(status_str != NULL);
    assert(strstr(status_str, "\"active\":true") != NULL);
    assert(strstr(status_str, "\"view\":1") != NULL);
    woodland_json_finish(&json);

    /* 2. 键盘导航测试：Escape 关闭 */
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_Escape));
    assert(!woodland_window_menu_is_active(&server));

    /* 3. 再次打开菜单，测试点击外部关闭 */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    /* 点击外部 (0, 0) */
    assert(woodland_window_menu_handle_button(&server, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 0, 0));
    assert(!woodland_window_menu_is_active(&server));

    /* 4. 键盘导航选中最大化并执行 */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(!view.maximized);
    /* 初始从 -1 按一次 Down 聚焦第一项 (最大化) */
    woodland_window_menu_handle_key(&server, XKB_KEY_Down);
    woodland_window_menu_handle_key(&server, XKB_KEY_Return);
    assert(!woodland_window_menu_is_active(&server));
    assert(view.maximized);

    /* 5. 再次打开菜单，执行还原 (此时第一项变为还原) */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    /* 再次按 Down 聚焦第一项 (还原) */
    woodland_window_menu_handle_key(&server, XKB_KEY_Down);
    woodland_window_menu_handle_key(&server, XKB_KEY_Return);
    assert(!woodland_window_menu_is_active(&server));
    assert(!view.maximized);

    /* 6. 单字母快捷键测试：'n' 最小化 */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(!view.minimized);
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_n));
    assert(!woodland_window_menu_is_active(&server));
    assert(view.minimized);
    view.minimized = false;

    /* 测试菜单触发移动 (Move) */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_m));
    assert(!woodland_window_menu_is_active(&server));
    assert(server.cursor_mode == WOODLAND_CURSOR_MOVE);
    assert(server.grabbed_view == &view);
    assert(server.interactive_click_to_finish == true);
    /* 键盘微调与确认 */
    woodland_interactive_handle_key(&server, XKB_KEY_Right, 0);
    woodland_interactive_handle_key(&server, XKB_KEY_Down, 0);
    woodland_interactive_handle_key(&server, XKB_KEY_Return, 0);
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);
    assert(server.grabbed_view == NULL);
    assert(server.interactive_click_to_finish == false);

    /* 测试菜单触发调整大小 (Resize) */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_r));
    assert(!woodland_window_menu_is_active(&server));
    assert(server.cursor_mode == WOODLAND_CURSOR_RESIZE);
    assert(server.grabbed_view == &view);
    assert(server.interactive_click_to_finish == true);
    /* 按 Escape 取消 */
    woodland_interactive_handle_key(&server, XKB_KEY_Escape, 0);
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);
    assert(server.grabbed_view == NULL);
    assert(server.interactive_click_to_finish == false);
    /* 7. 测试工作区子菜单导航与移动窗口 */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(view.workspace == &ws1);
    /* 按 'w' 展开工作区子菜单 */
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_w));
    assert(woodland_window_menu_is_active(&server));
    assert(server.window_menu->sub_has_focus);
    assert(server.window_menu->sub.item_count == 2);
    assert(server.window_menu->sub.items[0].checked == true);
    assert(server.window_menu->sub.items[1].checked == false);
    /* 移动光标到工作区 2 并确认 */
    woodland_window_menu_handle_key(&server, XKB_KEY_Down);
    assert(server.window_menu->sub.hovered_index == 1);
    woodland_window_menu_handle_key(&server, XKB_KEY_Return);
    assert(!woodland_window_menu_is_active(&server));
    assert(view.workspace == &ws2);
    /* "移动到工作区" 采用"移动并跟随"语义：当前可见工作区也切到目标
     * （与 src/protocol/shortcut.c:shortcut_move_view_to_workspace 一致）。
     * 锁住这一意图，避免日后被误改成只搬窗口不切工作区。 */
    assert(server.active_workspace == &ws2);

    /* 8. 测试多屏幕子菜单 */
    struct wlr_output *out2 = wlr_headless_add_output(server.backend, 800, 600);
    assert(out2 != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out2);

    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    /* 多显示器下 's' 展开屏幕子菜单 */
    assert(woodland_window_menu_handle_key(&server, XKB_KEY_s));
    assert(server.window_menu->sub_has_focus);
    assert(server.window_menu->sub.item_count == 2);
    woodland_window_menu_handle_key(&server, XKB_KEY_Escape);
    assert(!woodland_window_menu_is_active(&server));

    /* 9. 中英文多语言文本切换测试 */
    setenv("LANG", "zh_CN.UTF-8", 1);
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(strstr(server.window_menu->root.items[0].text, "最大化") != NULL);
    woodland_window_menu_hide(&server);

    setenv("LANG", "C", 1);
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    assert(strstr(server.window_menu->root.items[0].text, "Maximize") != NULL);
    woodland_window_menu_hide(&server);

    /* 10. 测试 view unmap / hide_for_view */
    woodland_window_menu_show(&view, 200, 100);
    assert(woodland_window_menu_is_active(&server));
    woodland_window_menu_hide_for_view(&view);
    assert(!woodland_window_menu_is_active(&server));

    woodland_window_menu_finish(&server);
    assert(server.window_menu == NULL);

    free(ws1.name);
    free(ws2.name);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_cursor_destroy(server.cursor);
    woodland_output_finish(&server);
    wl_display_destroy(server.display);
    return 0;
}
