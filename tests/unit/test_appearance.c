// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-appearance / -manager 的纯逻辑回归：set_* 分档表、越界裁剪、
 * 字体描述串的拆分合成。
 *
 * 这里不建 display、不连客户端：bind / 推送 / 广播这些协议交互由
 * smoke-appearance 覆盖。分档表是 wctl 的 recorded_only 与协议实现的共同
 * 事实源，所以钉住它的条数比钉住某一项的字符串更有意义——上游加一条 set_*
 * 而这边忘了跟进，这条断言会先炸。
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cursor.h"
#include "protocol.h"
#include "ssd.h"
#include "text_buffer.h"

/* 两张 XML 的 set_* 请求总数（treeland-appearance-manager-unstable-v1） */
#define PROTOCOL_SET_REQUEST_COUNT 11

static void test_settings_table(void)
{
    size_t count = 0;
    const struct woodland_appearance_setting *table = woodland_appearance_settings(&count);
    assert(table != NULL);
    assert(count == PROTOCOL_SET_REQUEST_COUNT);

    /*
     * 全表过一遍：
     *   - 名字以 set_ 开头（是 request 名，不是 event 名）；
     *   - 名字唯一（wctl 与 find 都靠它定位）；
     *   - APPLIED 必须有渲染消费方，RECORDED 必须没有——"说生效了"和
     *     "说只记录"不能自相矛盾。
     */
    size_t applied = 0;
    size_t recorded = 0;
    for (size_t i = 0; i < count; i++) {
        assert(table[i].request != NULL);
        assert(strncmp(table[i].request, "set_", 4) == 0);
        for (size_t j = i + 1; j < count; j++) {
            assert(strcmp(table[i].request, table[j].request) != 0);
        }
        if (table[i].effect == WOODLAND_APPEARANCE_APPLIED) {
            assert(table[i].consumer != NULL);
            applied++;
        } else {
            assert(table[i].effect == WOODLAND_APPEARANCE_RECORDED);
            assert(table[i].consumer == NULL);
            recorded++;
        }
    }
    /* 只有等宽字体与窗口不透明度是"记了不画"的，多一个少一个都要问清楚 */
    assert(applied + recorded == PROTOCOL_SET_REQUEST_COUNT);
    assert(recorded == 2);
    assert(applied == 9);

    /* 按名字查：命中给条目，未知给 NULL */
    const struct woodland_appearance_setting *cursor =
        woodland_appearance_setting_find("set_cursor_theme");
    assert(cursor && cursor->effect == WOODLAND_APPEARANCE_APPLIED);
    const struct woodland_appearance_setting *opacity =
        woodland_appearance_setting_find("set_window_opacity");
    assert(opacity && opacity->effect == WOODLAND_APPEARANCE_RECORDED);
    assert(woodland_appearance_setting_find("cursor_theme") == NULL); /* event 名不算 */
    assert(woodland_appearance_setting_find("") == NULL);
    assert(woodland_appearance_setting_find(NULL) == NULL);
}

static void test_clamp(void)
{
    /* 光标：8..256，0 与超大值都收到端点（协议要求 clamp，不是报错） */
    assert(woodland_appearance_clamp_cursor_size(0) == WOODLAND_CURSOR_MIN_SIZE);
    assert(woodland_appearance_clamp_cursor_size(1) == WOODLAND_CURSOR_MIN_SIZE);
    assert(woodland_appearance_clamp_cursor_size(WOODLAND_CURSOR_MIN_SIZE) ==
           WOODLAND_CURSOR_MIN_SIZE);
    assert(woodland_appearance_clamp_cursor_size(24) == 24);
    assert(woodland_appearance_clamp_cursor_size(WOODLAND_CURSOR_MAX_SIZE) ==
           WOODLAND_CURSOR_MAX_SIZE);
    assert(woodland_appearance_clamp_cursor_size(WOODLAND_CURSOR_MAX_SIZE + 1) ==
           WOODLAND_CURSOR_MAX_SIZE);
    assert(woodland_appearance_clamp_cursor_size(UINT32_MAX) == WOODLAND_CURSOR_MAX_SIZE);

    /* 标题栏：0 是合法值（隐藏标题栏），上界 200 */
    assert(woodland_appearance_clamp_titlebar_height(0) == 0);
    assert(woodland_appearance_clamp_titlebar_height(26) == 26);
    assert(woodland_appearance_clamp_titlebar_height(200) == 200);
    assert(woodland_appearance_clamp_titlebar_height(201) == 200);
    assert(woodland_appearance_clamp_titlebar_height(UINT32_MAX) == 200);

    /* 圆角：0..WOODLAND_SSD_MAX_SIZE（1000），与 treeland 的上界一致 */
    assert(woodland_appearance_clamp_corner_radius(0) == 0);
    assert(woodland_appearance_clamp_corner_radius(12) == 12);
    assert(woodland_appearance_clamp_corner_radius(WOODLAND_SSD_MAX_SIZE) ==
           WOODLAND_SSD_MAX_SIZE);
    assert(woodland_appearance_clamp_corner_radius(WOODLAND_SSD_MAX_SIZE + 1) ==
           WOODLAND_SSD_MAX_SIZE);

    /* 字号 points*10：5pt..40pt。105 = 10.5pt 这种中间值必须原样过 */
    assert(woodland_appearance_clamp_font_size(0) == 50);
    assert(woodland_appearance_clamp_font_size(105) == 105);
    assert(woodland_appearance_clamp_font_size(400) == 400);
    assert(woodland_appearance_clamp_font_size(401) == 400);
    assert(woodland_appearance_clamp_font_size(UINT32_MAX) == 400);
}

/* 浮点比较：点数只保留一位小数，容差 1e-6 足够 */
static int near(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

static void test_font_description(void)
{
    char family[128] = {0};
    double size = 0.0;

    /* 默认字体串：拆出 family 与点数，和 [decorations] font 的写法一致 */
    assert(woodland_font_split("Sans 10", family, sizeof(family), &size));
    assert(strcmp(family, "Sans") == 0);
    assert(near(size, 10.0));

    /*
     * 字重不是族名的一部分：pango 把 "Bold" 解析成 weight，族名只到
     * "DejaVu Sans"。所以协议侧的 set_font_size 不能"拼 family + size"，
     * 否则字重就丢了——这正是 woodland_font_with 存在的理由。
     */
    assert(woodland_font_split("DejaVu Sans Bold 10.5", family, sizeof(family), &size));
    assert(strcmp(family, "DejaVu Sans") == 0);
    assert(near(size, 10.5));

    /* 空/NULL：没有 family，点数按 0 返回（调用方各自回落到 10pt） */
    assert(!woodland_font_split("", family, sizeof(family), &size));
    assert(family[0] == '\0');
    assert(near(size, 0.0));
    assert(!woodland_font_split(NULL, family, sizeof(family), &size));

    char buf[128];
    /* 只换族名：字号与字重都留着（noto 有没有这条字重是 fontconfig 的事，
     * 描述串本身必须原样带上） */
    assert(woodland_font_with("DejaVu Sans Bold 10.5", "Noto Sans", 0.0, buf, sizeof(buf)));
    assert(strcmp(buf, "Noto Sans Bold 10.5") == 0);
    /* 只换字号：族名与字重不动 */
    assert(woodland_font_with("DejaVu Sans Bold 10.5", NULL, 12.5, buf, sizeof(buf)));
    assert(strcmp(buf, "DejaVu Sans Bold 12.5") == 0);
    /* 都不改：输出 pango 规范化后的原串（整点数不补 ".0"） */
    assert(woodland_font_with("Sans 10", NULL, 0.0, buf, sizeof(buf)));
    assert(strcmp(buf, "Sans 10") == 0);
    /* NULL 描述串从内置默认起步，不会拼出空族名 */
    assert(woodland_font_with(NULL, "Noto Sans", 0.0, buf, sizeof(buf)));
    assert(strcmp(buf, "Noto Sans 10") == 0);
    /* 缓冲不足要拒绝，不能写出半截字符串 */
    assert(!woodland_font_with("Sans 10", "Noto Sans", 0.0, buf, 4));
    assert(!woodland_font_with("Sans 10", "Noto Sans", 0.0, NULL, sizeof(buf)));

    /* 从族名与点数起步也能得到可用描述串（无字重时只写 family + size） */
    assert(woodland_font_split("DejaVu Sans Bold 12.5", family, sizeof(family), &size));
    assert(woodland_font_with(NULL, family, size, buf, sizeof(buf)));
    assert(strcmp(buf, "DejaVu Sans 12.5") == 0);
}

int main(void)
{
    test_settings_table();
    test_clamp();
    test_font_description();
    printf("test_appearance: all assertions passed\n");
    return 0;
}
