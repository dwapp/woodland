// SPDX-License-Identifier: GPL-2.0-only
#include "icon.h"
#include "output.h"
#include "ssd.h"
#include "text_buffer.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

/* 用栈上伪造的 view/xdg 结构测 SSD 纯几何逻辑，不依赖真实 client 连接 */

/*
 * 扫一遍"按钮区"里的 buffer 节点，检查 dest 是不是方形、有没有溢出标题栏。
 *
 * 不去戳 view->ssd（内部结构对测试不可见），而是遍历场景树：真实节点的
 * dst_width / dst_height 就是最终画出来的尺寸，比断言中间变量更硬。
 */
struct glyph_scan {
    struct woodland_view *view;
    int zone_x;  /* 按钮区左边界（绝对坐标），0 表示整条标题栏 */
    int count;   /* 按钮区里的 buffer 节点数 */
    int enabled; /* 其中处于可见状态的 */
    int square;  /* 其中 dest 宽高相等的 */
    int spill;   /* 其中"顶端 + 高度"越过内容原点的（即画到了客户端内容上） */
};

/* 注意：wlr_scene_node_for_each_buffer 会整棵跳过 enabled=false 的子树，
 * 所以这里只能数到"看得见的节点"——想断言某东西被藏起来，只能断言它不在。 */
static void scan_glyphs(struct wlr_scene_buffer *buffer, int x, int y, void *data)
{
    struct glyph_scan *scan = data;
    int oy = scan->view->scene_tree->node.y;
    /* 按钮区 = 内容右端三个按钮宽。标题文字和应用图标都在左侧，不会命中 */
    if (x < scan->zone_x || y >= oy) {
        return;
    }
    scan->count++;
    if (buffer->node.enabled) {
        scan->enabled++;
    }
    if (buffer->dst_width == buffer->dst_height) {
        scan->square++;
    }
    if (y + buffer->dst_height > oy) {
        scan->spill++;
    }
}

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

    /* titlebar 命中：布局坐标 (150, 88) → 相对 (50, -12) 在标题栏内（高 32） */
    uint32_t edges = 0;
    enum woodland_ssd_part part = woodland_ssd_at(&view, 150, 88, &edges);
    assert(part == WOODLAND_SSD_TITLEBAR);

    /* 按钮：宽 32、间距 0，自右向左 close/max/iconify，占 [400-96, 400) */
    /* 关闭按钮：相对 x∈[368,400) */
    part = woodland_ssd_at(&view, 100 + 380, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_CLOSE);
    /* 最大化：相对 x∈[336,368) */
    part = woodland_ssd_at(&view, 100 + 360, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_MAXIMIZE);
    /* 最小化：相对 x∈[304,336) */
    part = woodland_ssd_at(&view, 100 + 330, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BUTTON_MINIMIZE);

    /* 左边框命中：相对 x∈[-1,0)（边框宽 1） */
    part = woodland_ssd_at(&view, 100 - 0.5, 100 + 150, &edges);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);

    /* 顶部边框命中：相对 y∈[-(th+1), -th) */
    part = woodland_ssd_at(&view, 100 + 200, 100 - (WOODLAND_SSD_TITLEBAR_H + 0.5), &edges);
    assert(part == WOODLAND_SSD_BORDER_TOP);
    assert(edges == WLR_EDGE_TOP);

    /* 边框必须包住标题栏：标题栏高度处的左边框（相对 y∈[-th,0)）也要命中，
     * 且四角不留空隙（顶边框横向跨满到 x=-1 / x=400） */
    part = woodland_ssd_at(&view, 100 - 0.5, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BORDER_LEFT);
    assert(edges == WLR_EDGE_LEFT);
    part = woodland_ssd_at(&view, 100 + 400.5, 100 - 12, &edges);
    assert(part == WOODLAND_SSD_BORDER_RIGHT);
    assert(edges == WLR_EDGE_RIGHT);
    part = woodland_ssd_at(&view, 100 - 0.5, 100 - (WOODLAND_SSD_TITLEBAR_H + 0.5), &edges);
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

    part = woodland_ssd_at(&view, 100 + 200, 100 - (WOODLAND_SSD_TITLEBAR_H + 4.0), &edges);
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

    /* 外边距：普通窗口 = 边框 1 + 标题栏 32（顶 33） */
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

    /* ==================== treeland-decoration 的覆盖值 ==================== */

    /*
     * 标题栏 hide：边距的 top 只剩边框，且原来标题栏那一带不再是 SSD 区域
     * （否则用户还能拖一条看不见的标题栏）。这是"半 CSD 半 SSD"的基础。
     */
    view.ssd_overrides.titlebar_set = true;
    view.ssd_overrides.titlebar_hidden = true;
    woodland_ssd_deco_changed(&view);
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.top == WOODLAND_SSD_BORDER_W);
    assert(margins.left == WOODLAND_SSD_BORDER_W);
    assert(margins.bottom == WOODLAND_SSD_BORDER_W);
    assert(margins.right == WOODLAND_SSD_BORDER_W);
    /* 内容原点上方 12px 原本是标题栏（可拖动），隐藏后不该再命中 */
    assert(woodland_ssd_at(&view, 100 + 200, 100 - 12, &edges) == WOODLAND_SSD_NONE);
    /* 标题栏隐藏不影响边框：顶部 1px 仍然可抓 */
    assert(woodland_ssd_at(&view, 100 + 200, 100 - 0.5, &edges) == WOODLAND_SSD_BORDER_TOP);
    assert(edges == WLR_EDGE_TOP);

    /* 边框覆盖：宽度进边距（顶部仍无标题栏），颜色随覆盖值走 */
    view.ssd_overrides.border_set = true;
    view.ssd_overrides.border_width = 4;
    view.ssd_overrides.border_rgba = 0xff0000ff; /* r=255 g=0 b=0 a=255 */
    woodland_ssd_deco_changed(&view);
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 4 && margins.right == 4 && margins.bottom == 4);
    assert(margins.top == 4);
    /* 4px 边框：相对 x ∈ [-4,0) 命中左边缘 */
    assert(woodland_ssd_at(&view, 100 - 2, 100 + 150, &edges) == WOODLAND_SSD_BORDER_LEFT);
    /* 边框之外的 5px 处不再命中（抓取宽度 8 仍然向外扩，这里验证的是
     * 边框本身的几何已经跟着覆盖值走） */
    assert(woodland_ssd_at(&view, 100 + 200, 100 + 150, &edges) == WOODLAND_SSD_NONE);

    /* 边框 0：不画边框，边距归零（隐藏标题栏 + 零边框 = 完全没有装饰） */
    view.ssd_overrides.border_width = 0;
    woodland_ssd_deco_changed(&view);
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 0 && margins.top == 0 && margins.right == 0 && margins.bottom == 0);

    /* 恢复标题栏 + 不覆盖边框：回到配置默认（32 + 1） */
    view.ssd_overrides.titlebar_hidden = false;
    view.ssd_overrides.border_set = false;
    woodland_ssd_deco_changed(&view);
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.top == WOODLAND_SSD_TITLEBAR_H + WOODLAND_SSD_BORDER_W);
    assert(margins.left == WOODLAND_SSD_BORDER_W);
    /* 标题栏回来，命中区域一并恢复 */
    assert(woodland_ssd_at(&view, 100 + 200, 100 - 12, &edges) == WOODLAND_SSD_TITLEBAR);

    /* 最大化 + 隐藏标题栏：什么都不剩（原来是"只留标题栏"） */
    view.ssd_overrides.titlebar_hidden = true;
    view.maximized = true;
    woodland_ssd_deco_changed(&view);
    woodland_ssd_get_margins(&view, &margins);
    assert(margins.left == 0 && margins.top == 0 && margins.right == 0 && margins.bottom == 0);
    view.maximized = false;
    view.ssd_overrides.titlebar_hidden = false;
    woodland_ssd_deco_changed(&view);

    /* 按钮图标 buffer 物理尺寸验证：1x 为 size, 2x 为 size * 2，防止孤儿 px=1 回归 */
    struct wlr_buffer *btn_1x = woodland_button_icon_buffer(WOODLAND_SSD_ICON_CLOSE, 26, false, 1.0f);
    assert(btn_1x != NULL);
    assert(btn_1x->width == 26);
    assert(btn_1x->height == 26);
    wlr_buffer_drop(btn_1x);

    struct wlr_buffer *btn_2x = woodland_button_icon_buffer(WOODLAND_SSD_ICON_CLOSE, 26, false, 2.0f);
    assert(btn_2x != NULL);
    assert(btn_2x->width == 52);
    assert(btn_2x->height == 52);
    wlr_buffer_drop(btn_2x);

    /*
     * 按钮符号必须是方形，且不能溢出格子。
     *
     * 宽度来自 [decorations] button_width，高度跟着标题栏走：默认 32/32 两者
     * 恰好相等，把"按 (bwn, bh) 设 dest"的问题整个掩盖住了。这里把标题栏压到
     * 20 逼出不等——不取短边的话，符号要么被纵向压扁，要么仍按 32 高画而溢出
     * 到内容区（y 从 -20 一直到 +12）。
     */
    struct woodland_config tight_cfg = {0};
    tight_cfg.ssd.enabled = true;
    tight_cfg.ssd.titlebar_height = 20;
    tight_cfg.ssd.button_width = WOODLAND_SSD_BUTTON_W;
    server.config = &tight_cfg;

    struct woodland_view tight = {0};
    tight.server = &server;
    tight.xdg_toplevel = &fake_toplevel;
    tight.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    assert(tight.scene_tree);
    wlr_scene_node_set_position(&tight.scene_tree->node, 600, 100);
    woodland_ssd_create(&tight);
    assert(tight.ssd != NULL);

    struct glyph_scan scan = {.view = &tight,
                              .zone_x = 600 + 400 - 3 * WOODLAND_SSD_BUTTON_W};
    wlr_scene_node_for_each_buffer(&tight.scene_tree->node, scan_glyphs, &scan);
    assert(scan.count == 3);           /* close / max / min 三个符号 */
    assert(scan.enabled == scan.count); /* 默认宽度下当然要画出来 */
    assert(scan.square == scan.count); /* dest 必须是方形，不能被压扁 */
    assert(scan.spill == 0);           /* 不能越过内容原点画到客户端内容上 */

    woodland_ssd_destroy(&tight);
    wlr_scene_node_destroy(&tight.scene_tree->node);
    server.config = NULL;

    /*
     * 窄窗口：放不下"图标 + 标题 + 三个按钮"时整组按钮收起，右侧那一带仍是
     * 标题栏（能拖，点不到按钮）。不收的话 32+32+32 三个格子会压到左侧应用
     * 图标上，两个图形叠在一起谁也点不准。
     */
    struct wlr_xdg_surface narrow_base = {0};
    narrow_base.geometry = (struct wlr_box){.x = 0, .y = 0, .width = 100, .height = 200};
    struct wlr_xdg_toplevel narrow_toplevel = {.base = &narrow_base};

    struct woodland_view narrow = {0};
    narrow.server = &server;
    narrow.xdg_toplevel = &narrow_toplevel;
    narrow.scene_tree = wlr_scene_tree_create(&server.scene->tree);
    assert(narrow.scene_tree);
    wlr_scene_node_set_position(&narrow.scene_tree->node, 900, 100);
    woodland_ssd_create(&narrow);
    assert(narrow.ssd != NULL);

    /* 相对 (95, -10) 落在最右那个按钮格子内；按钮收起后必须退回标题栏 */
    assert(woodland_ssd_at(&narrow, 900 + 95, 100 - 10, &edges) == WOODLAND_SSD_TITLEBAR);
    assert(woodland_ssd_at(&narrow, 900 + 50, 100 - 10, &edges) == WOODLAND_SSD_TITLEBAR);

    /* 光不响应点击还不够：节点本身也得关掉，否则图标还画在标题栏上。
     * 这一条是给 ssd_apply_state 的——它跑在 ssd_layout 之后，早先那句
     * set_enabled(button, titlebar) 会把布局刚收起的按钮重新打开。
     *
     * 遍历器会跳过隐藏子树，所以"按钮不在"表现为数不到它们：右侧 96px 那一带
     * 里只剩应用图标和标题文字（这个 view 两个都可能是空的），三个符号一个都
     * 不能冒出来。 */
    struct glyph_scan narrow_scan = {.view = &narrow,
                                     .zone_x = 900 + 100 - 3 * WOODLAND_SSD_BUTTON_W};
    wlr_scene_node_for_each_buffer(&narrow.scene_tree->node, scan_glyphs, &narrow_scan);
    assert(narrow_scan.count <= 2);

    woodland_ssd_destroy(&narrow);
    wlr_scene_node_destroy(&narrow.scene_tree->node);

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

    /*
     * SSD 调色板自报。
     *
     * 像素断言（tests/smoke/run_xwayland_smoke.sh）的期望值从这里来，所以内置
     * 默认值在这里钉住：色板是有意的设计决定，改它就该连这条测试一起改，
     * 而不是让一条像素用例在 CI 上莫名变红。三层来源里这里覆盖两档——
     * 内置浅色板、内置深色板，以及"用户显式配色时深色板不接管"。
     */
    struct woodland_config pal_cfg = {0};
    server.config = &pal_cfg;

    struct woodland_ssd_palette pal = {0};
    woodland_ssd_get_palette(&view, &pal);
    assert(!pal.dark);
    assert(pal.titlebar_active == 0xF4F4F4);
    assert(pal.titlebar_inactive == 0xF9F9F9);
    assert(pal.border_active == 0xCCCCCC);
    assert(pal.border_inactive == 0xCCCCCC);

    pal_cfg.appearance.color_scheme_set = true;
    pal_cfg.appearance.dark = true;
    woodland_ssd_get_palette(&view, &pal);
    assert(pal.dark);
    assert(pal.titlebar_active == 0x292929);
    assert(pal.titlebar_inactive == 0x202020);
    assert(pal.border_active == 0x525252);
    assert(pal.border_inactive == 0x525252);

    /* 显式配过 SSD 颜色的人，不该被深色方案盖掉（同 window_menu 的规矩） */
    pal_cfg.ssd.colors_specified = true;
    woodland_ssd_get_palette(&view, &pal);
    assert(!pal.dark);
    assert(pal.titlebar_active == 0xF4F4F4);

    /*
     * 客端覆盖边框：报的必须是"作者写的颜色"，不能被预乘拉暗。
     * 这条带 alpha（0x80）的用例是 rgba_float_to_rgb 里除回 alpha 那一步的
     * 唯一实际覆盖——不除的话 #3366CC 会报成 #1A3366 那种暗色，
     * 而且 8 位量化之后肉眼基本看不出来是错的。
     */
    view.ssd_overrides.border_set = true;
    view.ssd_overrides.border_rgba = 0x3366CC80;
    woodland_ssd_get_palette(&view, &pal);
    assert(pal.border_active == 0x3366CC);
    assert(pal.border_inactive == 0x3366CC);
    /* 覆盖的只是边框，标题栏不受影响 */
    assert(pal.titlebar_active == 0xF4F4F4);
    view.ssd_overrides.border_set = false;

    server.config = NULL;

    woodland_ssd_finish();

    wlr_scene_node_destroy(&view.scene_tree->node);
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;
    return 0;
}
