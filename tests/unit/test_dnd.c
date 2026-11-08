// SPDX-License-Identifier: GPL-2.0-only
/*
 * 拖拽状态机单测。
 *
 * 为什么不在 headless 里跑一次完整拖拽：start_drag 的 serial 必须来自一次
 * **真实的** pointer button press（wlr_seat_validate_pointer_grab_serial 查的
 * 是 seat->pointer_state 里的 grab serial），headless 后端不产生指针事件，
 * 客户端也就拿不到可用 serial。真拖拽只能真机手测（清单见
 * docs/clipboard-dnd.md §8.3）。
 *
 * 这里覆盖状态机本身：图标树的开关与置顶、客户端不给图标、未 start 就
 * destroy、重复 destroy、以及"命中图标自己"的判定。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>

#include "scene.h"

#include "dnd.h"
#include "woodland.h"

/* node 是不是 scene 根的最后一个孩子（= 最上层） */
static bool is_top_most_child(struct woodland_server *server, struct wlr_scene_node *node)
{
    if (wl_list_empty(&server->scene->tree.children)) {
        return false;
    }
    struct wl_list *last = server->scene->tree.children.prev;
    struct wlr_scene_node *top = wl_container_of(last, top, link);
    return top == node;
}

/* 造一个假拖拽：状态机只读 drag->icon / grab_type / dropped，够用 */
static void fake_drag_init(struct wlr_drag *drag, enum wlr_drag_grab_type type)
{
    memset(drag, 0, sizeof(*drag));
    wl_signal_init(&drag->events.destroy);
    drag->grab_type = type;
}

int main(void)
{
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend);
    /*
     * 刻意不建 renderer/allocator：场景树与 seat 都不需要它们，而建一套
     * EGL 上下文会在退出时被 LeakSanitizer 记成 Mesa 的泄漏（和本模块无关，
     * 但会把 build-asan 的 dnd 测试染红）。
     */
    server.scene = wlr_scene_create();
    assert(server.scene);
    server.seat = wlr_seat_create(server.display, "seat0");
    assert(server.seat);

    assert(woodland_dnd_init(&server));
    struct woodland_dnd *dnd = &server.dnd;
    assert(dnd->icons != NULL);
    /* 空闲时图标树必须关着：不渲染、也不参与 hit-test */
    assert(!dnd->icons->node.enabled);
    assert(!woodland_dnd_active(&server));

    struct wlr_drag drag;
    fake_drag_init(&drag, WLR_DRAG_GRAB_KEYBOARD_POINTER);

    woodland_dnd_handle_start(&server, &drag);
    assert(woodland_dnd_active(&server));
    assert(dnd->drag == &drag);
    assert(dnd->icons->node.enabled);
    /* 置顶：不置顶会被 layer-shell 的 panel 盖住（labwc #1936） */
    assert(is_top_most_child(&server, &dnd->icons->node));
    /* 客户端没给图标是合法状态：只置 active，不建节点 */
    assert(dnd->icon.tree == NULL);

    /* 拖拽期间不许再插进来一个（否则同一棵树上会叠两层图标） */
    struct wlr_drag other;
    fake_drag_init(&other, WLR_DRAG_GRAB_KEYBOARD_POINTER);
    woodland_dnd_handle_start(&server, &other);
    assert(dnd->drag == &drag);

    /* 没 start 过的 drag 被销毁：不能把当前拖拽带走 */
    woodland_dnd_handle_destroy(&server, &other);
    assert(dnd->drag == &drag);
    assert(dnd->icons->node.enabled);

    /* 命中测试：图标子树里的节点一律当成"图标自己"，不能当投放目标 */
    struct wlr_scene_tree *inside = wlr_scene_tree_create(dnd->icons);
    assert(inside != NULL);
    assert(woodland_dnd_node_is_icon(dnd, &inside->node));
    struct wlr_scene_tree *outside = wlr_scene_tree_create(&server.scene->tree);
    assert(outside != NULL);
    assert(!woodland_dnd_node_is_icon(dnd, &outside->node));
    assert(!woodland_dnd_node_is_icon(dnd, NULL));

    woodland_dnd_handle_destroy(&server, &drag);
    assert(!woodland_dnd_active(&server));
    assert(dnd->icons->node.enabled == false);

    /* 重复 destroy（wlr_drag 有 cancel + destroy 两条路）必须幂等 */
    woodland_dnd_handle_destroy(&server, &drag);
    assert(dnd->drag == NULL);

    woodland_dnd_finish(&server);
    assert(dnd->icons == NULL);

    /* 收尾：scene 与 display 都自己建的，自己拆干净 */
    wlr_scene_node_destroy(&server.scene->tree.node);
    wl_display_destroy(server.display);

    printf("test_dnd: ok\n");
    return 0;
}
