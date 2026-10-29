// SPDX-License-Identifier: GPL-2.0-only
/*
 * 不变量检查单测。
 *
 * 造一批"状态不自洽"的假视图，断言对应那条检查失败、且失败信息指向
 * 正确的视图。这是自检模块的价值所在：把 PR #8 评审里"两扇窗口标题栏
 * 同时是激活色"那类问题变成一条可断言的规则。
 *
 * 假窗口要给到 xdg_toplevel 和一个哑 surface：焦点检查要求"焦点视图
 * 能收键盘"，没有 surface 会正当地判失败。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "selfcheck.h"
#include "ssd.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

struct fake_view {
    struct woodland_view view;
    struct wlr_xdg_toplevel toplevel;
    struct wlr_xdg_surface xdg_surface;   /* toplevel->base 指向它 */
    struct wlr_surface surface;
};

static void fake_init(struct fake_view *fake, struct woodland_server *server,
                      uint32_t id, struct woodland_workspace *ws) {
    memset(fake, 0, sizeof(*fake));
    fake->view.server = server;
    fake->view.id = id;
    fake->view.type = WOODLAND_VIEW_XDG;
    fake->view.xdg_toplevel = &fake->toplevel;
    /* base 是指针（wlr_xdg_toplevel.base），必须指到一个真实存在的结构上 */
    fake->toplevel.base = &fake->xdg_surface;
    fake->xdg_surface.surface = &fake->surface;
    fake->view.scene_tree = wlr_scene_tree_create(ws->tree);
    fake->view.mapped = true;
    fake->view.workspace = ws;
}

/* 同时改两处：我们的记录、xdg configure 里的 activated 位 */
static void fake_set_activated(struct fake_view *fake, bool on) {
    fake->view.activated = on;
    fake->toplevel.scheduled.activated = on;
}

static const struct woodland_check *find(const struct woodland_check *checks,
                                         int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(checks[i].name, name) == 0) {
            return &checks[i];
        }
    }
    assert(!"check not found");
    return NULL;
}

static int run(struct woodland_server *server,
               struct woodland_check *checks) {
    return woodland_selfcheck_collect(server, checks, WOODLAND_SELFCHECK_MAX);
}

static void assert_all_ok(struct woodland_server *server, const char *stage) {
    struct woodland_check out[WOODLAND_SELFCHECK_MAX];
    int n = run(server, out);
    for (int i = 0; i < n; i++) {
        if (!out[i].ok) {
            fprintf(stderr, "unexpected failure at %s: %s (expected %s, got %s)\n",
                    stage, out[i].name, out[i].expected, out[i].actual);
            assert(0);
        }
    }
}

int main(void) {
    struct woodland_server server = {0};
    wl_list_init(&server.views);
    wl_list_init(&server.workspaces);
    server.scene = wlr_scene_create();
    assert(server.scene);

    struct woodland_workspace ws1 = {.server = &server, .name = "1"};
    struct woodland_workspace ws2 = {.server = &server, .name = "2"};
    ws1.tree = wlr_scene_tree_create(&server.scene->tree);
    ws2.tree = wlr_scene_tree_create(&server.scene->tree);
    assert(ws1.tree && ws2.tree);
    server.active_workspace = &ws1;

    /* ---------- 空状态：全部通过 ---------- */
    struct woodland_check out[WOODLAND_SELFCHECK_MAX];
    int n = run(&server, out);
    assert(n == 8);
    assert_all_ok(&server, "empty");

    /* ---------- 一个正常聚焦的视图：全部通过 ---------- */
    struct fake_view a;
    fake_init(&a, &server, 1, &ws1);
    wl_list_insert(&server.views, &a.view.link);
    server.focused_view = &a.view;
    fake_set_activated(&a, true);
    assert_all_ok(&server, "single focused view");

    /* 焦点还在、但激活态被清 → activated_unique 失败（"标题栏不变色"） */
    fake_set_activated(&a, false);
    n = run(&server, out);
    assert(!find(out, n, "activated_unique")->ok);
    fake_set_activated(&a, true);

    /* ---------- 两扇窗口同时激活（PR #8 那个 bug 的形状）---------- */
    struct fake_view b;
    fake_init(&b, &server, 2, &ws1);
    fake_set_activated(&b, true);
    wl_list_insert(server.views.next, &b.view.link);   /* b 在最上 */

    n = run(&server, out);
    const struct woodland_check *act = find(out, n, "activated_unique");
    assert(!act->ok);
    assert(strcmp(act->actual, "2 activated views") == 0);

    fake_set_activated(&b, false);
    assert_all_ok(&server, "back to single active");

    /* ---------- 焦点指向不可聚焦的视图 ---------- */
    server.focused_view = &b.view;
    fake_set_activated(&a, false);
    fake_set_activated(&b, true);
    b.view.minimized = true;
    n = run(&server, out);
    const struct woodland_check *focus = find(out, n, "focused_view_valid");
    assert(!focus->ok);
    assert(strstr(focus->actual, "not focusable"));

    /* ---------- 场景树挂到别的工作区 ---------- */
    b.view.minimized = false;
    wlr_scene_node_reparent(&b.view.scene_tree->node, ws2.tree);
    n = run(&server, out);
    const struct woodland_check *ws = find(out, n, "mapped_in_workspace");
    assert(!ws->ok);
    assert(strstr(ws->actual, "scene_tree not under workspace tree"));
    wlr_scene_node_reparent(&b.view.scene_tree->node, ws1.tree);
    assert_all_ok(&server, "reparented back");

    /* ---------- 链表里留着已卸载的视图 ---------- */
    server.focused_view = NULL;
    fake_set_activated(&b, false);
    b.view.mapped = false;
    n = run(&server, out);
    assert(!find(out, n, "views_list_intact")->ok);
    /* mapped=0 的视图不再参与工作区检查，那条仍然成立 */
    assert(find(out, n, "mapped_in_workspace")->ok);
    b.view.mapped = true;
    assert_all_ok(&server, "remapped");

    /* ---------- xdg configure 的 activated 与状态不符 ---------- */
    fake_set_activated(&b, false);
    b.toplevel.scheduled.activated = true;   /* configure 说激活了，记录说没激活 */
    n = run(&server, out);
    const struct woodland_check *xdg = find(out, n, "xdg_activated_matches");
    assert(!xdg->ok);
    assert(strstr(xdg->actual, "view#2"));

    b.toplevel.scheduled.activated = false;
    assert_all_ok(&server, "xdg configure synced");

    /* ---------- JSON 输出：ok/failures 与失败项字段 ---------- */
    struct woodland_json json;
    woodland_json_init(&json);
    fake_set_activated(&a, true);            /* 焦点为空却激活 → 至少一条失败 */
    int failures = woodland_selfcheck_run(&server, &json);
    assert(failures >= 1);
    const char *text = woodland_json_data(&json);
    assert(strstr(text, "\"ok\":false"));
    assert(strstr(text, "\"failures\":"));
    assert(strstr(text, "\"expected\":"));
    assert(strstr(text, "\"actual\":"));
    woodland_json_finish(&json);
    fake_set_activated(&a, false);

    /* 修好之后 ok:true、没有 expected/actual 噪音 */
    woodland_json_init(&json);
    failures = woodland_selfcheck_run(&server, &json);
    assert(failures == 0);
    text = woodland_json_data(&json);
    assert(strstr(text, "\"ok\":true"));
    assert(strstr(text, "\"expected\"") == NULL);
    woodland_json_finish(&json);

    return 0;
}
