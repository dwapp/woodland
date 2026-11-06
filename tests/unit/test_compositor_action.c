// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-compositor-action 的纯逻辑回归：动作分档、工作区名、相邻工作区、
 * 以及诊断状态在无协议层时的安全默认值。
 *
 * 这里不建 display、不连客户端：协议交互（bind / trigger / destroy）由
 * smoke-compositor-action 覆盖。栈上伪造 workspaces 链表的做法同
 * test_ssd.c / test_workspace.c。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>

#include "protocol.h"
#include "woodland.h"
#include "workspace.h"

static void test_classify(void)
{
    /*
     * 值来自 protocols/treeland/treeland-compositor-action-unstable-v1.xml：
     * 1..12 指定工作区、13/14 前后切换、15..30 是暂置空的 shell 动作。
     * 全值域扫一遍，比只测几个边界值更容易发现上游改枚举后的漂移。
     */
    for (uint32_t value = 1; value <= 30; value++) {
        enum woodland_protocol_action_class expected;
        if (value <= 12) {
            expected = WOODLAND_PROTO_ACTION_WORKSPACE;
        } else if (value == 13) {
            expected = WOODLAND_PROTO_ACTION_WORKSPACE_PREV;
        } else if (value == 14) {
            expected = WOODLAND_PROTO_ACTION_WORKSPACE_NEXT;
        } else {
            expected = WOODLAND_PROTO_ACTION_UNSUPPORTED;
        }
        assert(woodland_protocol_action_classify(value) == expected);
    }
    assert(woodland_protocol_action_classify(0) == WOODLAND_PROTO_ACTION_INVALID);
    assert(woodland_protocol_action_classify(31) == WOODLAND_PROTO_ACTION_INVALID);
    assert(woodland_protocol_action_classify(999) == WOODLAND_PROTO_ACTION_INVALID);
    assert(woodland_protocol_action_classify(UINT32_MAX) ==
           WOODLAND_PROTO_ACTION_INVALID);
}

static void test_workspace_name(void)
{
    char buf[16];
    assert(woodland_protocol_action_workspace_name(1, buf, sizeof(buf)));
    assert(strcmp(buf, "1") == 0);
    assert(woodland_protocol_action_workspace_name(10, buf, sizeof(buf)));
    assert(strcmp(buf, "10") == 0);
    assert(woodland_protocol_action_workspace_name(12, buf, sizeof(buf)));
    assert(strcmp(buf, "12") == 0);

    /* 不是指定工作区的动作、以及枚举外的值都不该产出名字 */
    assert(!woodland_protocol_action_workspace_name(13, buf, sizeof(buf)));
    assert(!woodland_protocol_action_workspace_name(15, buf, sizeof(buf)));
    assert(!woodland_protocol_action_workspace_name(0, buf, sizeof(buf)));
    assert(!woodland_protocol_action_workspace_name(31, buf, sizeof(buf)));

    /* 缓冲不够"10\0"三个字节时必须失败，不能写出截断的名字 */
    char tiny[2];
    assert(!woodland_protocol_action_workspace_name(10, tiny, sizeof(tiny)));
    assert(!woodland_protocol_action_workspace_name(1, NULL, 8));
    assert(!woodland_protocol_action_workspace_name(1, buf, 0));
}

static void test_adjacent_workspace(void)
{
    struct woodland_server server = {0};
    wl_list_init(&server.workspaces);

    /* 空链表或没有 active_workspace 时不能返回野指针 */
    assert(woodland_protocol_action_adjacent_workspace(NULL, true) == NULL);
    assert(woodland_protocol_action_adjacent_workspace(&server, true) == NULL);

    struct woodland_workspace ws1 = {0}, ws2 = {0}, ws3 = {0};
    wl_list_insert(server.workspaces.prev, &ws1.link);
    wl_list_insert(server.workspaces.prev, &ws2.link);
    wl_list_insert(server.workspaces.prev, &ws3.link);

    /* 边界不绕回：第一个没有 prev，最后一个没有 next */
    server.active_workspace = &ws1;
    assert(woodland_protocol_action_adjacent_workspace(&server, false) == NULL);
    assert(woodland_protocol_action_adjacent_workspace(&server, true) == &ws2);

    server.active_workspace = &ws2;
    assert(woodland_protocol_action_adjacent_workspace(&server, false) == &ws1);
    assert(woodland_protocol_action_adjacent_workspace(&server, true) == &ws3);

    server.active_workspace = &ws3;
    assert(woodland_protocol_action_adjacent_workspace(&server, false) == &ws2);
    assert(woodland_protocol_action_adjacent_workspace(&server, true) == NULL);
}

static void test_status_defaults(void)
{
    bool available = true;
    uint32_t count = 99, last = 99;
    const char *outcome = "stale";

    /* 没有 server 时不能读空指针，且要把所有出参归零 */
    woodland_protocol_compositor_action_status(NULL, &available, &count, &last,
                                                &outcome);
    assert(!available);
    assert(count == 0);
    assert(last == 0);
    assert(outcome == NULL);

    /* server 存在但协议层还没建起来（proto == NULL）同样安全 */
    struct woodland_server server = {0};
    available = true;
    count = 99;
    last = 99;
    outcome = "stale";
    woodland_protocol_compositor_action_status(&server, &available, &count, &last,
                                                &outcome);
    assert(!available);
    assert(count == 0);
    assert(last == 0);
    assert(outcome == NULL);

    /* 全部出参都是可选的 */
    woodland_protocol_compositor_action_status(&server, NULL, NULL, NULL, NULL);
}

int main(void)
{
    test_classify();
    test_workspace_name();
    test_adjacent_workspace();
    test_status_defaults();
    printf("compositor_action unit passed\n");
    return 0;
}
