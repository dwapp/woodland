// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include "ssd.h"
#include "view.h"
#include "woodland.h"

/* 用栈上伪造的 view/xdg 结构测 SSD 纯几何逻辑，不依赖真实 client 连接 */
int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.scene = wlr_scene_create();
    assert(server.scene);

    struct wlr_xdg_surface fake_base = {0};
    fake_base.geometry = (struct wlr_box){ .x = 0, .y = 0, .width = 400, .height = 300 };
    struct wlr_xdg_toplevel fake_toplevel = { .base = &fake_base };

    struct woodland_view view = {0};
    view.server = &server;
    view.xdg_toplevel = &fake_toplevel;
    view.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    assert(view.scene_tree);
    wlr_scene_node_set_position(&view.scene_tree->node, 100, 100);

    /* create：默认非激活 + 内容尺寸 400x300 */
    woodland_ssd_create(&view);
    assert(view.ssd != NULL);

    /* titlebar 命中：布局坐标 (150, 88) → 相对 (50, -12) 在标题栏内（高 26） */
    uint32_t edges = 0;
    enum woodland_ssd_part part =
        woodland_ssd_at(&view, 150, 88, &edges);
    assert(part == WOODLAND_SSD_TITLEBAR);

    /* 按钮：宽 26、间距 0，自右向左 close/max/iconify，占 [400-78, 400) */
    /* 关闭按钮：相对 x∈[374,400) */
    part = woodland_ssd_at(&view, 100 + 380, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_CLOSE);
    /* 最大化：相对 x∈[348,374) */
    part = woodland_ssd_at(&view, 100 + 360, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_MAXIMIZE);
    /* 最小化：相对 x∈[322,348) */
    part = woodland_ssd_at(&view, 100 + 330, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_MINIMIZE);

    /* 左边框命中：相对 x∈[-1,0)（边框宽 1） */
    part = woodland_ssd_at(&view, 100 - 0.5, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);

    /* 顶部边框命中：相对 y∈[-27,-26) */
    part = woodland_ssd_at(&view, 100 + 200, 100 - 26.5, &edges);
    assert(part == WOODLAND_SSD_BORDER_TOP);
    assert(edges == WLR_EDGE_TOP);

    /* 内容区不命中 SSD */
    part = woodland_ssd_at(&view, 100 + 200, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_NONE);

    /* activate 切换后颜色变化（用 set_active 不崩溃即可验证；颜色非空常量） */
    woodland_ssd_set_active(&view, true);
    woodland_ssd_set_active(&view, false);

    /* destroy：view->ssd 置空，scene 树级联释放 */
    woodland_ssd_destroy(&view);
    assert(view.ssd == NULL);

    wlr_scene_node_destroy(&view.scene_tree->node);
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;
    return 0;
}
