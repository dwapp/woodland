// SPDX-License-Identifier: GPL-2.0-only
/*
 * effect 纯函数单测：圆角半径的取值优先级、四角怎么摊、blur region 怎么裁。
 *
 * 不建 compositor、不碰 GPU——这些函数把"协议/配置里的意图"翻译成"要画多大
 * 半径、糊哪一块"，默认构建与 effects 构建行为必须一致，差别只在最后画不画。
 * 所以这里两种构建都要通过（渲染部分在 effect.c 里被 #if 编掉了）。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <pixman.h>

#include "effect.h"
#include "scene.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

static void clear_corners(struct woodland_effect_corners *c)
{
    memset(c, 0xA5, sizeof(*c)); /* 脏填充：函数漏写某个角会被断言抓到 */
}

int main(void)
{
    struct woodland_server server = {0};
    /* 堆上分配：woodland_config_destroy 连结构本身一起释放，栈上的不行 */
    struct woodland_config *config = woodland_config_create_default();
    assert(config != NULL);
    config->ssd.titlebar_height = 26;
    config->ssd.border_width = 1;
    server.config = config;

    struct woodland_view view = {0};
    view.server = &server;

    /* ---- 1) 半径来源：默认 0，配置生效 ---- */
    assert(config->effects.corner_radius == 0);
    assert(woodland_effect_corner_radius(&view) == 0);
    config->effects.corner_radius = 12;
    assert(woodland_effect_corner_radius(&view) == 12);

    /* 空视图 / 无配置也不能崩，按 0 处理 */
    assert(woodland_effect_corner_radius(NULL) == 0);
    struct woodland_view orphan = {0};
    assert(woodland_effect_corner_radius(&orphan) == 0);

    /* ---- 2) 客户端覆盖 > 配置 ---- */
    view.ssd_overrides.corner_radius_set = true;
    view.ssd_overrides.corner_radius = 4;
    assert(woodland_effect_corner_radius(&view) == 4);

    /* 覆盖值也要 clamp：上界按 SSD 几何上界收，负数归 0 */
    view.ssd_overrides.corner_radius = WOODLAND_SSD_MAX_SIZE + 500;
    assert(woodland_effect_corner_radius(&view) == WOODLAND_SSD_MAX_SIZE);
    view.ssd_overrides.corner_radius = -7;
    assert(woodland_effect_corner_radius(&view) == 0);

    /* 配置值同样 clamp（配置解析里也拦一次，这里守的是函数自己的契约） */
    view.ssd_overrides.corner_radius_set = false;
    config->effects.corner_radius = 5000;
    assert(woodland_effect_corner_radius(&view) == WOODLAND_SSD_MAX_SIZE);
    config->effects.corner_radius = -1;
    assert(woodland_effect_corner_radius(&view) == 0);

    /* ---- 3) 四角：有标题栏只圆下角，上角归 SSD ---- */
    config->effects.corner_radius = 10;
    struct woodland_effect_corners c;
    clear_corners(&c);
    woodland_effect_corners(&view, &c);
    assert(c.top_left == 0 && c.top_right == 0);
    assert(c.bottom_left == 10 && c.bottom_right == 10);

    /* ---- 4) 无标题栏（协议 hide 或配置高度为 0）：四角都圆 ---- */
    view.ssd_overrides.titlebar_hidden = true;
    clear_corners(&c);
    woodland_effect_corners(&view, &c);
    assert(c.top_left == 10 && c.top_right == 10);
    assert(c.bottom_left == 10 && c.bottom_right == 10);

    view.ssd_overrides.titlebar_hidden = false;
    config->ssd.titlebar_height = 0;
    clear_corners(&c);
    woodland_effect_corners(&view, &c);
    assert(c.top_left == 10 && c.bottom_right == 10);
    config->ssd.titlebar_height = 26;

    /* ---- 5) 最大化 / 全屏：一律直角（圆角会在屏幕边缘漏出后面） ---- */
    view.maximized = true;
    clear_corners(&c);
    woodland_effect_corners(&view, &c);
    assert(c.top_left == 0 && c.top_right == 0 && c.bottom_left == 0 && c.bottom_right == 0);
    view.maximized = false;

    view.fullscreen = true;
    clear_corners(&c);
    woodland_effect_corners(&view, &c);
    assert(c.top_left == 0 && c.bottom_left == 0);
    view.fullscreen = false;

    /* ---- 6) blur region：空区域 / 非法尺寸不成立 ---- */
    struct wlr_box box = {0};
    pixman_region32_t region;
    pixman_region32_init(&region);
    assert(!woodland_effect_blur_box(&region, 400, 300, &box));
    assert(!woodland_effect_blur_box(NULL, 400, 300, &box));
    pixman_region32_init_rect(&region, 0, 0, 10, 10);
    assert(!woodland_effect_blur_box(&region, 0, 300, &box));
    assert(!woodland_effect_blur_box(&region, 400, 0, &box));

    /* ---- 7) 正常区域原样取包围盒 ---- */
    pixman_region32_clear(&region);
    pixman_region32_init_rect(&region, 50, 40, 200, 100);
    assert(woodland_effect_blur_box(&region, 400, 300, &box));
    assert(box.x == 50 && box.y == 40 && box.width == 200 && box.height == 100);

    /* ---- 8) 越界：裁到 surface 尺寸 ---- */
    pixman_region32_clear(&region);
    pixman_region32_init_rect(&region, 350, 250, 200, 200);
    assert(woodland_effect_blur_box(&region, 400, 300, &box));
    assert(box.x == 350 && box.y == 250 && box.width == 50 && box.height == 50);

    /* 完全落在 surface 外：不成立，不是"裁成 0 宽" */
    pixman_region32_clear(&region);
    pixman_region32_init_rect(&region, 500, 400, 50, 50);
    assert(!woodland_effect_blur_box(&region, 400, 300, &box));

    /* 负坐标一侧同样裁 */
    pixman_region32_clear(&region);
    pixman_region32_init_rect(&region, -20, -20, 100, 100);
    assert(woodland_effect_blur_box(&region, 400, 300, &box));
    assert(box.x == 0 && box.y == 0 && box.width == 80 && box.height == 80);

    /* ---- 9) 提交状态：设区域 → blur=true，空区域 → 撤掉 ---- */
    pixman_region32_clear(&region);
    pixman_region32_init_rect(&region, 10, 20, 100, 50);
    assert(!woodland_effect_blur_visible(&view));
    woodland_effect_set_blur_region(&view, &region, 400, 300);
    assert(woodland_effect_blur_visible(&view));
    assert(view.effect.blur_region.x == 10 && view.effect.blur_region.width == 100);

    /* 空区域 = 客户端撤掉模糊 */
    pixman_region32_clear(&region);
    woodland_effect_set_blur_region(&view, &region, 400, 300);
    assert(!woodland_effect_blur_visible(&view));
    assert(view.effect.blur_region.width == 0 && view.effect.blur_region.height == 0);

    /* region 传 NULL 等价于空（协议里 set_blur_region(NULL) 的语义） */
    woodland_effect_set_blur_region(&view, NULL, 400, 300);
    assert(!woodland_effect_blur_visible(&view));

    pixman_region32_fini(&region);
    woodland_config_destroy(config);

    printf("test_effect: ok\n");
    return 0;
}
