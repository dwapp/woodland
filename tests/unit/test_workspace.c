// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include "scene.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

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
    server.seat = wlr_seat_create(server.display, "test-seat");
    assert(server.seat);
    /* 工作区切换会回调焦点回退，回退要遍历 z 序视图链表 */
    wl_list_init(&server.views);

    assert(woodland_workspace_init(&server));

    /* 活动工作区是 "1" */
    assert(server.active_workspace != NULL);
    assert(strcmp(server.active_workspace->name, "1") == 0);

    /* 共 10 个工作区 */
    size_t n = 0;
    struct woodland_workspace *ws;
    wl_list_for_each(ws, &server.workspaces, link) {
        n++;
    }
    assert(n == 10);

    /* 工作区 1 enabled，其余 disabled */
    wl_list_for_each(ws, &server.workspaces, link) {
        if (strcmp(ws->name, "1") == 0) {
            assert(ws->tree->node.enabled == true);
        } else {
            assert(ws->tree->node.enabled == false);
        }
    }

    /* by_name 查找 */
    struct woodland_workspace *ws10 =
        woodland_workspace_by_name(&server, "10");
    assert(ws10 != NULL);
    assert(strcmp(ws10->name, "10") == 0);
    assert(woodland_workspace_by_name(&server, "11") == NULL);

    /* 切换工作区后清键盘焦点 */
    struct woodland_workspace *ws3 =
        woodland_workspace_by_name(&server, "3");
    assert(ws3 != NULL);
    woodland_workspace_switch(&server, ws3);
    assert(server.active_workspace == ws3);
    assert(ws3->tree->node.enabled == true);
    assert(server.seat->keyboard_state.focused_surface == NULL);
    struct woodland_workspace *ws1 =
        woodland_workspace_by_name(&server, "1");
    assert(ws1->tree->node.enabled == false);

    /*
     * ---------- 切到别的工作区时，旧焦点视图必须真的清掉激活态 ----------
     *
     * 这是"两个窗口标题栏同时亮"的成因：woodland_focus_current() 用
     * `workspace == active_workspace` 判断可聚焦，如果先改 active_workspace
     * 再清焦点，旧视图已经不可聚焦、headless 下又没有键盘设备可反查，
     * 就"找不到是谁"了——它的 activated / SSD / foreign-toplevel 会一直
     * 亮着，等新焦点设上来就有两个激活窗口。
     */
    woodland_workspace_switch(&server, ws1);
    assert(server.active_workspace == ws1);

    struct woodland_view v = {
        .server = &server,
        .type = WOODLAND_VIEW_XDG,
        .mapped = true,
        .workspace = ws1,
        .activated = true,
    };
    wl_list_insert(&server.views, &v.link);
    server.focused_view = &v;
    /* 假的 xdg 视图没有真实 toplevel，看 activated 这个真值就够 */
    assert(v.activated);

    /* restore_focus=false 正是 woodland_focus_view() 跨工作区聚焦走的路径 */
    woodland_workspace_switch_focus(&server, ws3, false);
    assert(server.active_workspace == ws3);
    assert(server.focused_view == NULL);
    assert(v.activated == false);   /* 漏清这里就会留成第二个激活视图 */

    wl_list_remove(&v.link);

    woodland_workspace_finish(&server);
    assert(server.active_workspace == NULL);

    /* 逆序释放 wlr 资源 */
    wlr_seat_destroy(server.seat);
    server.seat = NULL;
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
