// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-foreign-toplevel-manager-v3 的纯逻辑回归：state 数组编码。
 *
 * 只测这一个函数，因为它是全族唯一"算错了不会崩、只会让 Dock 显示错"的
 * 地方：状态数组少一位、顺序反了、或者在 cap 不足时越界写，客户端都会
 * 安静地按错误的位图渲染图标。其余部分（bind / handle / 请求回灌 /
 * dock preview 的错误码）由 smoke-foreign-toplevel 走真实协议覆盖。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "protocol.h"

/* 值与 XML 里 state 枚举一致：0..4。写死字面量，上游改枚举就该在这里炸 */
#define STATE_MAXIMIZED 0u
#define STATE_MINIMIZED 1u
#define STATE_ACTIVATED 2u
#define STATE_FULLSCREEN 3u
#define STATE_ATTENTION 4u

static void test_empty(void)
{
    struct woodland_toplevel_state st = {0};
    uint32_t out[5] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 0);
    /* 一个都不置位时不能碰缓冲：客户端拿到空数组才是"普通窗口" */
    for (int i = 0; i < 5; i++) {
        assert(out[i] == 0xffffffff);
    }
}

static void test_single_flags(void)
{
    struct woodland_toplevel_state st = {0};
    uint32_t out[5];

    st.maximized = true;
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 1);
    assert(out[0] == STATE_MAXIMIZED);
    st.maximized = false;

    st.minimized = true;
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 1);
    assert(out[0] == STATE_MINIMIZED);
    st.minimized = false;

    st.activated = true;
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 1);
    assert(out[0] == STATE_ACTIVATED);
    st.activated = false;

    st.fullscreen = true;
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 1);
    assert(out[0] == STATE_FULLSCREEN);
    st.fullscreen = false;

    st.attention = true;
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 1);
    assert(out[0] == STATE_ATTENTION);
}

static void test_all_flags_in_order(void)
{
    struct woodland_toplevel_state st = {
        .maximized = true,
        .minimized = true,
        .activated = true,
        .fullscreen = true,
        .attention = true,
    };
    uint32_t out[5] = {0};
    /* 顺序固定成枚举升序：客户端按值置位，顺序不影响语义，但反过来会让
     * 任何按位置断言的脚本（包括本仓库的冒烟）失去意义 */
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 5);
    assert(out[0] == STATE_MAXIMIZED);
    assert(out[1] == STATE_MINIMIZED);
    assert(out[2] == STATE_ACTIVATED);
    assert(out[3] == STATE_FULLSCREEN);
    assert(out[4] == STATE_ATTENTION);
}

static void test_partial(void)
{
    struct woodland_toplevel_state st = {
        .activated = true,
        .maximized = true,
    };
    uint32_t out[5] = {0};
    assert(woodland_protocol_toplevel_state_encode(&st, out, 5) == 2);
    /* 跳过未置位的项，不留 0 占位：数组里出现 maximized(0) 与"没有状态"
     * 在客户端看来是两回事 */
    assert(out[0] == STATE_MAXIMIZED);
    assert(out[1] == STATE_ACTIVATED);
}

static void test_capacity_clamp(void)
{
    struct woodland_toplevel_state st = {
        .maximized = true,
        .minimized = true,
        .activated = true,
    };
    uint32_t out[3] = {0, 0, 0};
    /* cap 不够就截断并返回 cap：宁少发一位，也不能越界写 */
    assert(woodland_protocol_toplevel_state_encode(&st, out, 2) == 2);
    assert(out[0] == STATE_MAXIMIZED);
    assert(out[1] == STATE_MINIMIZED);
    assert(out[2] == 0);

    uint32_t none[1] = {7};
    assert(woodland_protocol_toplevel_state_encode(&st, none, 0) == 0);
    assert(none[0] == 7);
}

static void test_null_guard(void)
{
    struct woodland_toplevel_state st = {.activated = true};
    uint32_t out[5] = {0};
    assert(woodland_protocol_toplevel_state_encode(NULL, out, 5) == 0);
    assert(woodland_protocol_toplevel_state_encode(&st, NULL, 5) == 0);
    assert(out[0] == 0);
}

int main(void)
{
    test_empty();
    test_single_flags();
    test_all_flags_in_order();
    test_partial();
    test_capacity_clamp();
    test_null_guard();
    printf("foreign_toplevel: all assertions passed\n");
    return 0;
}
