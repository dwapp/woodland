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
#endif


    return 0;
}
