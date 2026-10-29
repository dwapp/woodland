// SPDX-License-Identifier: GPL-2.0-only
/*
 * util/json 写入器单测：转义、NULL、UTF-8 清洗、整数数组。
 *
 * 这些细节决定了 `wctl get_tree | jq` 能不能解析——标题里一个引号
 * 没转义，整个响应就是坏 JSON，调试工具直接不可用。
 */
#include <assert.h>
#include <string.h>

#include "util/json.h"

int main(void) {
    /* ---------- 基本字符串与转义 ---------- */
    struct woodland_json j;
    woodland_json_init(&j);
    assert(woodland_json_ok(&j));

    woodland_json_string(&j, "plain");
    assert(strcmp(woodland_json_data(&j), "\"plain\"") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    woodland_json_string(&j, "say \"hi\"\\done");
    assert(strcmp(woodland_json_data(&j), "\"say \\\"hi\\\"\\\\done\"") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* 换行/制表/回车走短转义，其它控制字符走 \u00XX */
    woodland_json_string(&j, "a\nb\tc\rd\001" "e");
    assert(strcmp(woodland_json_data(&j),
                  "\"a\\nb\\tc\\rd\\u0001e\"") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* NULL 写成 null（title/app_id 经常没有） */
    woodland_json_string(&j, NULL);
    assert(strcmp(woodland_json_data(&j), "null") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* ---------- UTF-8：合法原样、非法替换成 U+FFFD ---------- */
    woodland_json_string(&j, "窗口");
    assert(strcmp(woodland_json_data(&j), "\"窗口\"") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* 孤立续字节 + 截断的多字节序列，都不该原样输出 */
    woodland_json_string(&j, "\x80\xE7\xAA");
    const char *data = woodland_json_data(&j);
    /* 两个坏字节 → 两个 U+FFFD（EF BF BD） */
    assert(strcmp(data, "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"") == 0
           || strcmp(data, "\"\xEF\xBF\xBD\xEF\xBF\xBD\"") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* ---------- 数值、布尔、数组、键 ---------- */
    woodland_json_raw(&j, "{");
    woodland_json_key(&j, "n");
    woodland_json_int(&j, -42);
    woodland_json_raw(&j, ",");
    woodland_json_key(&j, "b");
    woodland_json_bool(&j, true);
    woodland_json_raw(&j, ",");
    woodland_json_key(&j, "box");
    int values[4] = {0, 27, 800, 600};
    woodland_json_int_array(&j, values, 4);
    woodland_json_raw(&j, "}");
    assert(strcmp(woodland_json_data(&j),
                  "{\"n\":-42,\"b\":true,\"box\":[0,27,800,600]}") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* NULL/0 元素的数组写成 []，不产生 "null" */
    woodland_json_int_array(&j, NULL, 0);
    assert(strcmp(woodland_json_data(&j), "[]") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* ---------- 调用序列上的便利性 ---------- */
    /* 空缓冲区也返回 ""，调用方不必判空 */
    assert(strcmp(woodland_json_data(&j), "") == 0);
    assert(woodland_json_len(&j) == 0);

    /* fmt 超长会被截断，但仍然是合法字符串（不会越界写） */
    woodland_json_fmt(&j, "%0*d", 2000, 7);
    assert(woodland_json_len(&j) > 0 && woodland_json_len(&j) < 600);

    woodland_json_finish(&j);
    /* finish 之后可以重复 init（每次命令一个缓冲区） */
    woodland_json_init(&j);
    woodland_json_raw(&j, "[]");
    assert(strcmp(woodland_json_data(&j), "[]") == 0);
    woodland_json_finish(&j);

    return 0;
}
