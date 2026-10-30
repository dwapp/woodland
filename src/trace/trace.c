// SPDX-License-Identifier: GPL-2.0-only
/*
 * 决策轨迹：定长环形缓冲。
 *
 * 只在 --trace 下工作，写满覆盖最旧。不分配、不加锁、不去重：调用点要
 * 便宜到"随手加一行不心疼"，否则没人会在修 bug 之前就把它加对地方。
 *
 * 依赖方向：trace → util/json，一路向内。业务模块（shell/output/protocol/
 * ipc）都只**调用**它，它不认识任何一个，所以怎么加都不会成环。
 */
#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wlr/util/log.h>

#include "util/json.h"

/* 写满就覆盖最旧的。4096 × 136B ≈ 545KB，且只在 --trace 时才分配 */
#define TRACE_CAPACITY 4096
/* what 是短标签，超过这个长度说明用错了：轨迹不是日志 */
#define TRACE_WHAT_MAX 16
/* detail 够放"policy=autoplace area=1280x720 at 100,80"这种，再长就该去查状态 */
#define TRACE_DETAIL_MAX 96

struct trace_entry {
    uint32_t seq;    /* 从 0 开始的全局序号，跨覆盖单调递增 */
    uint32_t obj;    /* 对象 id（view id 等），0 表示"没有具体对象" */
    int64_t ms;      /* 相对 init 的毫秒数，比绝对时间戳好读 */
    char what[TRACE_WHAT_MAX];
    char detail[TRACE_DETAIL_MAX];
};

static struct {
    bool enabled;
    struct trace_entry *entries;  /* NULL 表示关闭 */
    uint32_t next;                /* 下一条写哪个槽 */
    uint32_t total;               /* 累计写入条数，不是缓冲里的条数 */
    int64_t origin_ms;
} trace;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void woodland_trace_init(bool enabled) {
    woodland_trace_finish();
    if (!enabled) {
        return;
    }

    trace.entries = calloc(TRACE_CAPACITY, sizeof(*trace.entries));
    if (!trace.entries) {
        /* 记不下来就算了：这条路径本身不能把合成器拖垮 */
        wlr_log(WLR_ERROR, "trace: out of memory, --trace stays off");
        return;
    }
    trace.next = 0;
    trace.total = 0;
    trace.origin_ms = now_ms();
    trace.enabled = true;
    wlr_log(WLR_INFO, "trace: recording up to %d entries", TRACE_CAPACITY);
}

void woodland_trace_finish(void) {
    free(trace.entries);
    trace.entries = NULL;
    trace.next = 0;
    trace.total = 0;
    trace.origin_ms = 0;
    trace.enabled = false;
}

bool woodland_trace_enabled(void) {
    return trace.enabled;
}

void woodland_trace_add(const char *what, uint32_t obj, const char *fmt, ...) {
    if (!trace.enabled) {
        return;
    }

    struct trace_entry *entry = &trace.entries[trace.next];
    entry->seq = trace.total;
    entry->obj = obj;
    entry->ms = now_ms() - trace.origin_ms;
    /* what 是编译期字面量；这里用 snprintf 只是防"标签写太长把邻居踩了" */
    snprintf(entry->what, sizeof(entry->what), "%s", what ? what : "-");

    if (fmt && fmt[0]) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(entry->detail, sizeof(entry->detail), fmt, args);
        va_end(args);
    } else {
        entry->detail[0] = '\0';
    }

    trace.next = (trace.next + 1) % TRACE_CAPACITY;
    trace.total++;
}

uint32_t woodland_trace_count(void) {
    return trace.total < TRACE_CAPACITY ? trace.total : TRACE_CAPACITY;
}

uint32_t woodland_trace_total(void) {
    return trace.total;
}

void woodland_trace_dump(struct woodland_json *out) {
    uint32_t count = woodland_trace_count();

    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_trace\",\"enabled\":");
    woodland_json_bool(out, trace.enabled);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "capacity");
    woodland_json_int(out, TRACE_CAPACITY);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "count");
    woodland_json_int(out, count);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "total");
    woodland_json_int(out, trace.total);
    woodland_json_raw(out, ",");
    /* 被覆盖掉的条数：total 减缓冲里还剩的，就是"你来得太晚"那些 */
    woodland_json_key(out, "dropped");
    woodland_json_int(out, trace.total - count);
    woodland_json_raw(out, ",\"entries\":[");

    /* 缓冲里最旧的一条：还没绕圈时是 0，绕圈后就是 next（下一步要覆盖的槽） */
    uint32_t oldest = trace.total < TRACE_CAPACITY ? 0 : trace.next;
    for (uint32_t i = 0; i < count; i++) {
        const struct trace_entry *entry =
            &trace.entries[(oldest + i) % TRACE_CAPACITY];
        if (i > 0) {
            woodland_json_raw(out, ",");
        }
        woodland_json_raw(out, "{\"seq\":");
        woodland_json_int(out, entry->seq);
        woodland_json_raw(out, ",");
        woodland_json_key(out, "ms");
        woodland_json_int(out, entry->ms);
        woodland_json_raw(out, ",");
        woodland_json_key(out, "obj");
        woodland_json_int(out, entry->obj);
        woodland_json_raw(out, ",");
        woodland_json_key(out, "what");
        woodland_json_string(out, entry->what);
        woodland_json_raw(out, ",");
        woodland_json_key(out, "detail");
        woodland_json_string(out, entry->detail);
        woodland_json_raw(out, "}");
    }
    woodland_json_raw(out, "]}");
}
