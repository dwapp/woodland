// SPDX-License-Identifier: GPL-2.0-only
/*
 * util/listener 账本单测：注册顺序、emit 计数、dump 形状、上限溢出。
 *
 * 顺序是这份账本存在的全部理由（监听器按注册序调用，顺序就是行为），
 * 所以断言的主线就是 seq：先注册的一定 seq 小，dump 一定按 seq 升序。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "util/json.h"
#include "util/listener.h"

static int callback_ran = 0;

static void on_event(struct wl_listener *listener, void *data) {
    callback_ran++;
    woodland_listener_count(listener);
    (void)data;
}

/* 摘掉一个监听（模拟销毁路径），账本条目留着——remove 不抹账 */
static void disconnect(struct wl_listener *listener) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

int main(void) {
    struct wl_signal sig;
    wl_signal_init(&sig);
    struct wl_listener a, b, c;

    /* ---------- 注册顺序即 seq ---------- */
    woodland_listener_connect(&a, &sig, on_event, "test.alpha");
    woodland_listener_connect(&b, &sig, on_event, "test.beta");
    woodland_listener_connect(&c, &sig, on_event, "test.gamma");
    assert(woodland_listener_total() == 3);

    /* emit 一次，三个都要跑，顺序 a → b → c */
    int order[3] = {0};
    int *o = order;
    (void)o;
    wl_signal_emit_mutable(&sig, NULL);
    assert(callback_ran == 3);

    /* ---------- dump 形状：seq 升序、名字对 ---------- */
    struct woodland_json j;
    woodland_json_init(&j);
    woodland_listener_dump(&j);
    assert(woodland_json_ok(&j));
    const char *data = woodland_json_data(&j);
    const char *expect[] = {
        "\"cmd\":\"get_listeners\"",
        "\"seq\":0", "\"name\":\"test.alpha\"",
        "\"seq\":1", "\"name\":\"test.beta\"",
        "\"seq\":2", "\"name\":\"test.gamma\"",
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        if (strstr(data, expect[i]) == NULL) {
            fprintf(stderr, "missing %s in: %s\n", expect[i], data);
            assert(0);
        }
    }
    /* emits 已被 count 记到 1 */
    assert(strstr(data, "\"emits\":1") != NULL);
    woodland_json_finish(&j);

    /* ---------- emit 计数是每监听器独立 ---------- */
    wl_signal_emit_mutable(&sig, NULL);
    woodland_json_init(&j);
    woodland_listener_dump(&j);
    data = woodland_json_data(&j);
    /* 两次 emit，每个 listener 各 2 次 */
    assert(strstr(data, "\"emits\":2") != NULL);
    assert(strstr(data, "\"emits\":1") == NULL);
    woodland_json_finish(&j);

    /* ---------- count 传 NULL 不炸 ---------- */
    woodland_listener_count(NULL);

    /* ---------- 注册数上限：溢出记数而不是崩 ---------- */
    struct wl_signal dummy;
    wl_signal_init(&dummy);
    static struct wl_listener spare[WOODLAND_LISTENER_MAX + 8];
    for (int i = 0; i < WOODLAND_LISTENER_MAX + 8; i++) {
        spare[i].notify = on_event;
        wl_list_init(&spare[i].link);
    }
    /* 先摘掉前面的再灌满——真实世界里 destroy 路径本来就会摘 */
    disconnect(&a);
    disconnect(&b);
    disconnect(&c);
    for (int i = 0; i < WOODLAND_LISTENER_MAX + 8; i++) {
        woodland_listener_connect(&spare[i], &dummy, on_event,
                                  "test.flood");
    }
    /* 3 + (MAX+8) 次注册，表只能记 MAX 条 */
    assert(woodland_listener_total() == WOODLAND_LISTENER_MAX);
    woodland_json_init(&j);
    woodland_listener_dump(&j);
    data = woodland_json_data(&j);
    /* 溢出条数必须出现在 dump 里：非 0 就是泄漏信号 */
    char overflow[64];
    snprintf(overflow, sizeof(overflow), "\"overflow\":%d",
             3 + WOODLAND_LISTENER_MAX + 8 - WOODLAND_LISTENER_MAX);
    assert(strstr(data, overflow) != NULL);
    woodland_json_finish(&j);

    printf("listener tests OK\n");
    return 0;
}
