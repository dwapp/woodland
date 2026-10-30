// SPDX-License-Identifier: GPL-2.0-only
/*
 * wl_listener 的账本：见 include/util/listener.h。
 *
 * 和 trace 一样是诊断汇（sink）：file-static 状态，不往调用点穿
 * server 指针；只依赖 util/json，业务模块谁都能链，不会成环。
 */
#include "util/listener.h"

#include <string.h>

#include "util/json.h"

struct listener_slot {
    struct wl_listener *listener;
    const char *name;
    uint64_t emits;
};

static struct {
    struct listener_slot slots[WOODLAND_LISTENER_MAX];
    uint32_t count;    /* 已用槽位数；同时也是下一个 seq */
    uint32_t overflow; /* 表满了还在注册：注册了但记不下 */
} ledger;

void woodland_listener_connect(struct wl_listener *listener,
                               struct wl_signal *signal,
                               wl_notify_func_t notify, const char *name) {
    listener->notify = notify;
    wl_signal_add(signal, listener);

    if (ledger.count >= WOODLAND_LISTENER_MAX) {
        ledger.overflow++;
        return;
    }
    struct listener_slot *slot = &ledger.slots[ledger.count++];
    slot->listener = listener;
    slot->name = name;
    slot->emits = 0;
}

void woodland_listener_count(struct wl_listener *listener) {
    if (!listener) {
        return;
    }
    /* 线性查找：emit 是高频路径……但只在排障时才插 WL_COUNT，
     * 常规构建里没有任何调用点，别为不存在的路径做哈希表 */
    for (uint32_t i = 0; i < ledger.count; i++) {
        if (ledger.slots[i].listener == listener) {
            ledger.slots[i].emits++;
            return;
        }
    }
}

uint32_t woodland_listener_total(void) {
    return ledger.count;
}

void woodland_listener_dump(struct woodland_json *out) {
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", true);
    woodland_json_obj_str(&obj, "cmd", "get_listeners");
    woodland_json_obj_int(&obj, "count", ledger.count);
    /* 注册了但表记不下：非 0 就是 listener 泄漏的信号 */
    woodland_json_obj_int(&obj, "overflow", ledger.overflow);

    woodland_json_obj_key(&obj, "listeners");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    for (uint32_t i = 0; i < ledger.count; i++) {
        const struct listener_slot *slot = &ledger.slots[i];
        woodland_json_arr_sep(&arr);

        struct woodland_json_obj entry;
        woodland_json_obj_open(&entry, out);
        /* seq 就是注册序号：监听器按它决定调用顺序，所以顺序错没错
         * 直接看这两个数 */
        woodland_json_obj_int(&entry, "seq", i);
        woodland_json_obj_str(&entry, "name", slot->name);
        woodland_json_obj_int(&entry, "emits", (long long)slot->emits);
        woodland_json_obj_close(&entry);
    }
    woodland_json_arr_close(&arr);
    woodland_json_obj_close(&obj);
}
