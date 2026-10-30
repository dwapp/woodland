// SPDX-License-Identifier: GPL-2.0-only
/*
 * 决策轨迹单测：开关语义、环的覆盖顺序、字段截断、JSON 形状。
 *
 * 轨迹本身是调试工具，最怕"工具自己坏了还看不出来"——所以这里不做
 * 模糊断言，全部数序列号：绕圈之后第一个 entry 的 seq 必须正好等于
 * total - capacity，这条能同时抓住"下一个写哪"和"最旧在哪"两处 off-by-one。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "trace.h"
#include "util/json.h"

/* 把一次 dump 追加进缓冲区，返回 data 指针（调用方负责 finish） */
static const char *dump(struct woodland_json *json) {
    woodland_json_init(json);
    woodland_trace_dump(json);
    return woodland_json_data(json);
}

static void test_disabled(void) {
    woodland_trace_init(false);
    assert(!woodland_trace_enabled());

    /* 关着的时候写多少都不留痕：这是"零成本"的契约 */
    for (int i = 0; i < 100; i++) {
        woodland_trace_add("focus", (uint32_t)i, "n=%d", i);
    }
    assert(woodland_trace_count() == 0);
    assert(woodland_trace_total() == 0);

    struct woodland_json j;
    const char *data = dump(&j);
    assert(woodland_json_ok(&j));
    if (strstr(data, "\"enabled\":false") == NULL
        || strstr(data, "\"count\":0") == NULL
        || strstr(data, "\"entries\":[]") == NULL) {
        fprintf(stderr, "unexpected dump while disabled: %s\n", data);
        assert(0);
    }
    woodland_json_finish(&j);
    woodland_trace_finish();
}

static void test_basic(void) {
    woodland_trace_init(true);
    assert(woodland_trace_enabled());

    woodland_trace_add("map", 7, "ws=1 parent=0");
    woodland_trace_add("place", 7, "policy=cascade %dx%d at %d,%d",
                       250, 250, 1, 27);
    woodland_trace_add("focus", 7, "prev=0 raise=1");

    assert(woodland_trace_count() == 3);
    assert(woodland_trace_total() == 3);

    struct woodland_json j;
    const char *data = dump(&j);
    assert(woodland_json_ok(&j));

    const char *expect[] = {
        "\"seq\":0", "\"seq\":1", "\"seq\":2",
        "\"what\":\"map\"", "\"what\":\"place\"", "\"what\":\"focus\"",
        "\"obj\":7", "policy=cascade 250x250 at 1,27",
        "\"dropped\":0",
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        if (strstr(data, expect[i]) == NULL) {
            fprintf(stderr, "missing %s in: %s\n", expect[i], data);
            assert(0);
        }
    }
    woodland_json_finish(&j);
    woodland_trace_finish();
}

static void test_wrap(void) {
    woodland_trace_init(true);
    /* 容量是 4096（见 trace.c）；写 4100 条，正好覆盖最旧的 4 条 */
    const uint32_t n = 4100;
    for (uint32_t i = 0; i < n; i++) {
        woodland_trace_add("focus", i, "i=%u", i);
    }

    uint32_t capacity = woodland_trace_count();
    assert(capacity > 0 && capacity < n);
    assert(woodland_trace_total() == n);
    assert(capacity == 4096);

    struct woodland_json j;
    const char *data = dump(&j);
    /* 覆盖了 n - capacity 条，最旧的 seq 必须正好是它 */
    char dropped[64];
    snprintf(dropped, sizeof(dropped), "\"dropped\":%u", n - capacity);
    assert(strstr(data, dropped) != NULL);

    char oldest[64];
    snprintf(oldest, sizeof(oldest), "\"seq\":%u,", n - capacity);
    assert(strstr(data, oldest) != NULL);

    char newest[64];
    snprintf(newest, sizeof(newest), "\"seq\":%u,", n - 1);
    assert(strstr(data, newest) != NULL);

    /* 被覆盖掉的那条不能再出现 */
    assert(strstr(data, "\"seq\":0,") == NULL);
    woodland_json_finish(&j);
    woodland_trace_finish();
}

static void test_truncation(void) {
    woodland_trace_init(true);
    /* what 上限 16、detail 上限 96：超长必须截断而不是溢出 */
    woodland_trace_add("a-very-long-event-name-that-nobody-should-use", 1, "%s",
                       "0123456789012345678901234567890123456789"
                       "0123456789012345678901234567890123456789"
                       "0123456789012345678901234567890123456789");
    assert(woodland_trace_count() == 1);

    struct woodland_json j;
    const char *data = dump(&j);
    assert(strstr(data, "\"what\":\"a-very-long-eve\"") != NULL);
    /* detail 被 vsnprintf 截在 95 个字符，不会出现第 96 个 '9' */
    assert(strstr(data, "0123456789012345678901234567890123456789"
                        "0123456789012345678901234567890123456789"
                        "0123456789012345678901234") == NULL);
    woodland_json_finish(&j);
    woodland_trace_finish();
}

static void test_reinit_clears(void) {
    woodland_trace_init(true);
    woodland_trace_add("map", 1, NULL);
    assert(woodland_trace_total() == 1);

    /* 重新 init 相当于"清空重来"，不能把旧条数带过来 */
    woodland_trace_init(true);
    assert(woodland_trace_total() == 0);
    assert(woodland_trace_count() == 0);
    woodland_trace_finish();
}

int main(void) {
    test_disabled();
    test_basic();
    test_wrap();
    test_truncation();
    test_reinit_clears();
    printf("trace tests OK\n");
    return 0;
}
