// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_UTIL_JSON_H
#define WOODLAND_UTIL_JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 极简 JSON 写入器：只负责"拼字符串"，不做语法树。
 *
 * 存在的理由是把调试接口的输出集中到一处，避免每个命令各写一遍
 * printf 转义。刻意不引 JSON 库：需求只有"把合成器状态写成一行"，
 * 手写一百多行比多一个依赖划算，也让 `wctl | jq` 之外的脚本
 * （grep/sed）照样能用。
 *
 * 用法：
 *     struct woodland_json j;
 *     woodland_json_init(&j);
 *     woodland_json_raw(&j, "{\"ok\":true,\"title\":");
 *     woodland_json_string(&j, view->title);   // NULL → null
 *     woodland_json_raw(&j, "}");
 *     puts(woodland_json_data(&j));
 *     woodland_json_finish(&j);
 *
 * 分配失败不会中断调用序列：内部置 oom 标记后继续空转，
 * woodland_json_ok() 返回 false，调用方据此回一个错误响应。
 */
struct woodland_json {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
};

void woodland_json_init(struct woodland_json *json);
void woodland_json_finish(struct woodland_json *json);

/** 缓冲区内容（始终以 NUL 结尾；未初始化时返回 ""）。 */
const char *woodland_json_data(const struct woodland_json *json);
size_t woodland_json_len(const struct woodland_json *json);
bool woodland_json_ok(const struct woodland_json *json);

/** 追加原样文本（调用方保证是合法 JSON 片段）。 */
void woodland_json_raw(struct woodland_json *json, const char *text);

/** 追加格式化文本（内部 vsnprintf，超长会被截断）。 */
void woodland_json_fmt(struct woodland_json *json, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * 追加一个 JSON 字符串字面量（含引号）。
 *
 * NULL 写成 null。非法 UTF-8 字节替换成 U+FFFD——标题/class 来自客户端，
 * 不能假设是干净 UTF-8，否则整个响应在 jq 里直接解析失败。
 */
void woodland_json_string(struct woodland_json *json, const char *text);

/** 追加 `"key":`（key 同样转义）。 */
void woodland_json_key(struct woodland_json *json, const char *key);

void woodland_json_int(struct woodland_json *json, long long value);
void woodland_json_bool(struct woodland_json *json, bool value);

/** 追加整数数组，如 [1,2,3]。values 为 NULL 或 n<=0 时写 []。 */
void woodland_json_int_array(struct woodland_json *json, const int *values,
                             int count);

#endif
