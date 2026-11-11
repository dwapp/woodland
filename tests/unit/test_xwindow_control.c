// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-xwindow-control-v1 的纯逻辑回归：fixed 取整、目标坐标推导、
 * 处置结果的稳定字符串、以及 wctl 状态在"协议层没建起来"时的安全默认值。
 *
 * 这里不建 display、不连客户端、也不起 Xwayland：协议交互（bind / destroy /
 * callback.done）由 smoke-xwindow-control 覆盖，真正的摆位由它的 X11 分支覆盖。
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wayland-util.h>

#include "protocol.h"
#include "util/json.h"
#include "woodland.h"

static void test_outcome_names(void)
{
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_NONE), "none") == 0);
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_MOVED), "moved") == 0);
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_NO_XWAYLAND),
                  "no_xwayland") == 0);
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_INVALID_ANCHOR),
                  "invalid_anchor") == 0);
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_NO_WINDOW),
                  "no_window") == 0);
    assert(strcmp(woodland_xwindow_outcome_name(WOODLAND_XWINDOW_CALLBACK_FAILED),
                  "callback_failed") == 0);
    /* 枚举外的值不能越界读表 */
    assert(strcmp(woodland_xwindow_outcome_name((enum woodland_xwindow_outcome)999),
                  "unknown") == 0);
}

static void test_round_fixed(void)
{
    assert(woodland_xwindow_control_round_fixed(0) == 0);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_int(10)) == 10);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_int(-10)) == -10);

    /* 四舍五入、远离 0：和 treeland 用 double 平移后的观感一致 */
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(1.5)) == 2);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(-1.5)) == -2);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(1.49)) == 1);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(-1.49)) == -1);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(0.49)) == 0);
    assert(woodland_xwindow_control_round_fixed(wl_fixed_from_double(-0.49)) == 0);

    /* 24.8 定点的极值转 int 不会溢出 */
    assert(woodland_xwindow_control_round_fixed(INT32_MAX) == 8388608);
    assert(woodland_xwindow_control_round_fixed(INT32_MIN) == -8388608);
}

static void test_target(void)
{
    int x = 0, y = 0;
    woodland_xwindow_control_target(100, 200, wl_fixed_from_int(30),
                                    wl_fixed_from_int(-40), &x, &y);
    assert(x == 130 && y == 160);

    /* 分数偏移按取整后的值算，且 x/y 各自独立 */
    woodland_xwindow_control_target(0, 0, wl_fixed_from_double(2.6),
                                    wl_fixed_from_double(-2.6), &x, &y);
    assert(x == 3 && y == -3);

    /* 出参可为 NULL，不能写空指针 */
    woodland_xwindow_control_target(1, 2, 0, 0, NULL, NULL);
    woodland_xwindow_control_target(1, 2, 0, 0, &x, NULL);
    assert(x == 1);
    woodland_xwindow_control_target(1, 2, 0, 0, NULL, &y);
    assert(y == 2);

    /* 大偏移不能让 int 加法 UB：饱和到 INT32_MAX / INT32_MIN */
    woodland_xwindow_control_target(INT32_MAX - 1, INT32_MIN + 1,
                                    wl_fixed_from_int(1000000),
                                    wl_fixed_from_int(-1000000), &x, &y);
    assert(x == INT32_MAX && y == INT32_MIN);
}

static void test_status_defaults(void)
{
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_xwindow_control_status(NULL, &json);
    assert(strcmp(woodland_json_data(&json), "{\"available\":false}") == 0);
    woodland_json_finish(&json);

    /* server 存在但协议层没建起来，同样只有 available:false */
    struct woodland_server server = {0};
    woodland_json_init(&json);
    woodland_protocol_xwindow_control_status(&server, &json);
    assert(strcmp(woodland_json_data(&json), "{\"available\":false}") == 0);
    woodland_json_finish(&json);
}

int main(void)
{
    test_outcome_names();
    test_round_fixed();
    test_target();
    test_status_defaults();
    printf("xwindow_control unit tests passed\n");
    return 0;
}
