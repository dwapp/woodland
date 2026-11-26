// SPDX-License-Identifier: GPL-2.0-only
/*
 * 焦点管理单测：可聚焦判定、z 序回退目标、Alt+Tab 循环选择。
 *
 * 不做真实聚焦（那需要 surface 与 seat 的键盘 grab），只验证"选谁"——
 * 选择逻辑（focus_topmost / focus_pick）与下发逻辑是分开的。
 */
#include <assert.h>
#include <stdbool.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include "scene.h"

#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif

#include "focus.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

int main(void) {
    struct woodland_server server = {0};
    wl_list_init(&server.views);
    wl_list_init(&server.workspaces);

    struct woodland_workspace ws1 = { .server = &server, .name = "1" };
    struct woodland_workspace ws2 = { .server = &server, .name = "2" };
    server.active_workspace = &ws1;

    /* z 序（链表头最上）：a b c d e */
    struct woodland_view a = { .server = &server, .mapped = true, .workspace = &ws1 };
    struct woodland_view b = { .server = &server, .mapped = true, .workspace = &ws1 };
    struct woodland_view c = { .server = &server, .mapped = true, .workspace = &ws1,
                               .minimized = true };
    struct woodland_view d = { .server = &server, .mapped = true, .workspace = &ws2 };
    struct woodland_view e = { .server = &server, .mapped = true, .workspace = &ws1,
                               .override_redirect = true };

    /* wl_list_insert 是头插：反过来插，最终顺序才是 a b c d e */
    wl_list_insert(&server.views, &e.link);
    wl_list_insert(&server.views, &d.link);
    wl_list_insert(&server.views, &c.link);
    wl_list_insert(&server.views, &b.link);
    wl_list_insert(&server.views, &a.link);

    /* ---------- 可聚焦判定 ---------- */
    assert(woodland_view_focusable(&a));
    assert(!woodland_view_focusable(&c));   /* 最小化 */
    assert(!woodland_view_focusable(&d));   /* 别的���作区 */
    assert(!woodland_view_focusable(&e));   /* override-redirect */

    server.active_workspace = &ws2;
    assert(!woodland_view_focusable(&a));
    assert(woodland_view_focusable(&d));
    server.active_workspace = &ws1;

    /*
     * 可恢复判定：就是上面那份判定放过"最小化"一条，其余条件一模一样。
     * 两者只在 c（最小化）上有分歧——这条分歧是 Alt+Tab 候选集的全部依据，
     * 写成两个独立实现的话迟早会漂，所以钉在这里。
     */
    assert(woodland_view_restorable(&a));
    assert(woodland_view_restorable(&c)); /* 拿不到焦点，但恢复之后可以 */
    assert(!woodland_view_restorable(&d));
    assert(!woodland_view_restorable(&e));
    assert(!woodland_view_restorable(NULL));
    struct woodland_view unmapped = { .server = &server, .mapped = false, .workspace = &ws1 };
    assert(!woodland_view_restorable(&unmapped));

    /* ---------- 回退目标 = z 序最上 ---------- */
    assert(woodland_focus_topmost(&server) == &a);

    /* 把 a 变成不可聚焦，回退应落到 b */
    a.minimized = true;
    assert(woodland_focus_topmost(&server) == &b);
    a.minimized = false;

    /* ---------- 循环选择 ---------- */
    /* 当前 a：往前 → b，往回 → b（只有 a、b 可聚焦，两个窗口来回切） */
    assert(woodland_focus_pick(&server, &a, true) == &b);
    assert(woodland_focus_pick(&server, &a, false) == &b);
    assert(woodland_focus_pick(&server, &b, true) == &a);
    assert(woodland_focus_pick(&server, &b, false) == &a);

    /* from 不在候选集里（最小化/别的桌面）：往前给第一个，往回给最后一个 */
    assert(woodland_focus_pick(&server, &c, true) == &a);
    assert(woodland_focus_pick(&server, &c, false) == &b);
    assert(woodland_focus_pick(&server, &d, true) == &a);
    assert(woodland_focus_pick(&server, NULL, true) == &a);

    /* 三个可聚焦时按 z 序轮转：a → b → c' */
    struct woodland_view f = { .server = &server, .mapped = true, .workspace = &ws1 };
    wl_list_insert(server.views.prev, &f.link);   /* 追加到表尾 */
    assert(woodland_focus_pick(&server, &b, true) == &f);
    assert(woodland_focus_pick(&server, &f, true) == &a);
    assert(woodland_focus_pick(&server, &a, false) == &f);

    /* 全部不可聚焦：没有目标 */
    a.minimized = true;
    b.minimized = true;
    f.minimized = true;
    assert(woodland_focus_topmost(&server) == NULL);
    assert(woodland_focus_pick(&server, &a, true) == NULL);
    assert(woodland_focus_pick(&server, NULL, false) == NULL);
    /* ---------- wants_focus 期望模型判定 ---------- */
    assert(woodland_view_wants_focus(&a) == WOODLAND_VIEW_WANTS_FOCUS_ALWAYS);
    assert(woodland_view_wants_focus(NULL) == WOODLAND_VIEW_WANTS_FOCUS_NEVER);
    woodland_seat_focus_surface(&server, NULL);

#if WOODLAND_HAVE_XWAYLAND
    struct wlr_xwayland_surface xsurf_empty_type = {
        .window_type_len = 0,
    };
    struct woodland_view xview = {
        .server = &server,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &xsurf_empty_type,
        .mapped = true,
        .workspace = &ws1,
    };
    assert(woodland_view_wants_focus(&xview) == WOODLAND_VIEW_WANTS_FOCUS_ALWAYS);
    assert(woodland_view_focusable(&xview) == true);

    /* ---------- 强制激活：切工作区 + 取消最小化 + 模态穿透 ---------- */
    struct woodland_server f_srv = {0};
    wl_list_init(&f_srv.views);
    wl_list_init(&f_srv.workspaces);
    wl_signal_init(&f_srv.view_changed);
    wl_signal_init(&f_srv.workspace_changed);
    f_srv.scene = wlr_scene_create();
    assert(f_srv.scene != NULL);

    struct woodland_workspace f_ws1 = {
        .server = &f_srv,
        .name = "1",
        .tree = wlr_scene_tree_create(&f_srv.scene->tree),
    };
    struct woodland_workspace f_ws2 = {
        .server = &f_srv,
        .name = "2",
        .tree = wlr_scene_tree_create(&f_srv.scene->tree),
    };
    wlr_scene_node_set_enabled(&f_ws2.tree->node, false);
    f_srv.active_workspace = &f_ws1;

    struct wlr_surface s_other = {0};
    struct wlr_surface s_parent = {0};
    struct wlr_surface s_modal = {0};
    struct wlr_xwayland_surface xs_other = { .surface = &s_other };
    struct wlr_xwayland_surface xs_parent = { .surface = &s_parent };
    struct wlr_xwayland_surface xs_modal = { .surface = &s_modal, .modal = true };

    struct woodland_view v_other = {
        .id = 1,
        .server = &f_srv,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &xs_other,
        .mapped = true,
        .workspace = &f_ws1,
        .scene_tree = wlr_scene_tree_create(f_ws1.tree),
    };
    struct woodland_view v_parent = {
        .id = 2,
        .server = &f_srv,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &xs_parent,
        .mapped = true,
        .minimized = true,
        .workspace = &f_ws2,
        .scene_tree = wlr_scene_tree_create(f_ws2.tree),
    };
    wlr_scene_node_set_enabled(&v_parent.scene_tree->node, false);

    /* 模态子窗口故意放在 ws1 且处于最小化，验证强制激活父窗口能一并对齐并恢复 */
    struct woodland_view v_modal = {
        .id = 3,
        .server = &f_srv,
        .type = WOODLAND_VIEW_XWAYLAND,
        .xwayland_surface = &xs_modal,
        .parent = &v_parent,
        .mapped = true,
        .minimized = true,
        .workspace = &f_ws1,
        .scene_tree = wlr_scene_tree_create(f_ws1.tree),
    };
    wlr_scene_node_set_enabled(&v_modal.scene_tree->node, false);

    wl_list_insert(&f_srv.views, &v_modal.link);
    wl_list_insert(&f_srv.views, &v_parent.link);
    wl_list_insert(&f_srv.views, &v_other.link);
    f_srv.focused_view = &v_other;
    v_other.activated = true;

    assert(woodland_focus_force_activate(&v_parent) == true);
    assert(f_srv.active_workspace == &f_ws2);
    assert(!v_parent.minimized);
    assert(!v_modal.minimized);
    assert(v_modal.workspace == &f_ws2);
    assert(v_modal.scene_tree->node.parent == f_ws2.tree);
    assert(f_srv.focused_view == &v_modal);
    assert(v_modal.activated);
    assert(!v_other.activated);

    /* 直接强制激活模态窗口时，若父窗口已最小化也应一并恢复 */
    v_parent.minimized = true;
    v_modal.minimized = true;
    wlr_scene_node_set_enabled(&v_parent.scene_tree->node, false);
    wlr_scene_node_set_enabled(&v_modal.scene_tree->node, false);
    f_srv.active_workspace = &f_ws1;
    wlr_scene_node_set_enabled(&f_ws2.tree->node, false);
    wlr_scene_node_set_enabled(&f_ws1.tree->node, true);

    assert(woodland_focus_force_activate(&v_modal) == true);
    assert(f_srv.active_workspace == &f_ws2);
    assert(!v_parent.minimized);
    assert(!v_modal.minimized);
    assert(f_srv.focused_view == &v_modal);

    wlr_scene_node_destroy(&f_srv.scene->tree.node);
#endif


    return 0;
}
