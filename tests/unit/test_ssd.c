// SPDX-License-Identifier: GPL-2.0-only
#include "icon.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

/* 用栈上伪造的 view/xdg 结构测 SSD 纯几何逻辑，不依赖真实 client 连接 */
int main(void)
{
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.scene = wlr_scene_create();
    assert(server.scene);

    struct wlr_xdg_surface fake_base = {0};
    fake_base.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 400, .height = 300};
    struct wlr_xdg_toplevel fake_toplevel = {.base = &fake_base};

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
    enum woodland_ssd_part part = woodland_ssd_at(&view, 150, 88, &edges);
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

    /* 边框必须包住标题栏：标题栏高度处的左边框（相对 y∈[-26,0)）也要命中，
     * 且四角不留空隙（顶边框横向跨满到 x=-1 / x=400） */
    part = woodland_ssd_at(&view, 100 - 0.5, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);
    part = woodland_ssd_at(&view, 100 + 400.5, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BORDER_RIGHT);
    assert(edges == WLR_EDGE_RIGHT);
    part = woodland_ssd_at(&view, 100 - 0.5, 100 - 26.5, &edges);
    assert(part == WOODLAND_SSD_BORDER_TOP);
    assert(edges == WLR_EDGE_TOP);
    part = woodland_ssd_at(&view, 100 - 0.5, 100 + 300.5, &edges);
    assert(part == WOODLAND_SSD_BORDER_BOTTOM);
    assert(edges == WLR_EDGE_BOTTOM);


    /* 抓取外边距（8px）：在 1px 细边框之外（如 -5px、+5px）依然能灵敏命中 */
    part = woodland_ssd_at(&view, 100 - 5.0, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);

    part = woodland_ssd_at(&view, 100 + 405.0, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_BORDER_RIGHT);
    assert(edges == WLR_EDGE_RIGHT);

    part = woodland_ssd_at(&view, 100 + 200, 100 + 305.0, &edges);
    assert(part == WOODLAND_SSD_BORDER_BOTTOM);
    assert(edges == WLR_EDGE_BOTTOM);

    part = woodland_ssd_at(&view, 100 + 200, 100 - 30.0, &edges);
    assert(part == WOODLAND_SSD_BORDER_TOP);
    assert(edges == WLR_EDGE_TOP);

    /* 超出抓取外边距（> 8px）则不命中 */
    part = woodland_ssd_at(&view, 100 - 15.0, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_NONE);
    /* 内容区不命中 SSD */
    part = woodland_ssd_at(&view, 100 + 200, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_NONE);

    /* activate 切换后颜色变化（用 set_active 不崩溃即可验证；颜色非空常量） */
    woodland_ssd_set_active(&view, true);
    woodland_ssd_set_active(&view, false);

    /* 按钮 hover/按下态：状态切换不崩溃，且与命中区域一一对应 */
    assert(woodland_ssd_at(&view, 100 + 380, 100 - 12, &edges) == WOODLAND_SSD_BUTTON_CLOSE);
    woodland_ssd_set_hover(&view, WOODLAND_SSD_BUTTON_CLOSE);
    woodland_ssd_set_pressed(&view, WOODLAND_SSD_BUTTON_CLOSE);
    woodland_ssd_set_pressed(&view, WOODLAND_SSD_NONE);
    woodland_ssd_set_hover(&view, WOODLAND_SSD_NONE);
    /* 标题栏/边框不是按钮：传 NONE 清除高亮 */
    woodland_ssd_set_hover(&view, WOODLAND_SSD_NONE);

    /* 外边距：普通窗口 = 边框 1 + 标题栏 26（顶 27） */
    struct woodland_ssd_margins margins = {0};
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == WOODLAND_SSD_BORDER_W);
    assert(margins.right == WOODLAND_SSD_BORDER_W);
    assert(margins.bottom == WOODLAND_SSD_BORDER_W);
    assert(margins.top == WOODLAND_SSD_TITLEBAR_H + WOODLAND_SSD_BORDER_W);

    /* 最大化：无边框，只留标题栏 */
    view.maximized = true;
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 0 && margins.right == 0 && margins.bottom == 0);
    assert(margins.top == WOODLAND_SSD_TITLEBAR_H);
    view.maximized = false;

    /* 全屏：装饰全隐，无任何外边距 */
    view.fullscreen = true;
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 0 && margins.top == 0 && margins.right == 0 && margins.bottom == 0);
    view.fullscreen = false;

    /* CSD（客户端自绘）：同样无边距，否则窗口会被挪出空档 */
    view.deco_csd = true;
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 0 && margins.top == 0 && margins.right == 0 && margins.bottom == 0);
    view.deco_csd = false;

    /* destroy：view->ssd 置空，scene 树级联释放 */
    woodland_ssd_destroy(&view);
    assert(view.ssd == NULL);

    /* .desktop 图标解析单测 */
    char tmp_desktop[] = "/tmp/woodland_test_desktop_XXXXXX";
    int fd = mkstemp(tmp_desktop);
    assert(fd >= 0);
    const char *content = "[Desktop Entry]\nName=Test App\nIcon=org.test.App\nType=Application\n";
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);
    char *parsed_icon = woodland_icon_from_desktop_file(tmp_desktop);
    assert(parsed_icon != NULL);
    assert(strcmp(parsed_icon, "org.test.App") == 0);
    free(parsed_icon);
    unlink(tmp_desktop);

    /* 不存在的图标路径查询不崩溃 */
    char *nonexistent = woodland_icon_find_path("nonexistent.app.xyz.123");
    assert(nonexistent == NULL);

    woodland_ssd_finish();

    wlr_scene_node_destroy(&view.scene_tree->node);
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;
    return 0;
}
