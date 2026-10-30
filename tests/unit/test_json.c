// SPDX-License-Identifier: GPL-2.0-only
/*
 * util/json 写入器单测：转义、NULL、UTF-8 清洗、整数数组。
 *
 * 这些细节决定了 `wctl get_tree | jq` 能不能解析——标题里一个引号
 * 没转义，整个响应就是坏 JSON，调试工具直接不可用。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "util/json.h"

/* 断言结果里恰好有 n 个 U+FFFD（EF BF BD），且没有其它内容 */
static void assert_fffd(const struct woodland_json *j, int n) {
    const char *data = woodland_json_data(j);
    size_t len = strlen(data);
    int count = 0;
    for (const char *p = data; (p = strstr(p, "\xEF\xBF\xBD")) != NULL; p += 3) {
        count++;
    }
    if (count != n || len != (size_t)(2 + 3 * n)) {
        fprintf(stderr, "expected %d U+FFFD, got %d (len %zu): %s\n",
                n, count, len, data);
        assert(0);
    }
}

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

    /*
     * 结构合法但码点非法的序列也要拦下。
     *
     * 这些序列直接放过去的后果不是"显示成怪字符"，而是响应本身解析
     * 失败：过长编码 0xC0 0x80 解出来是 0x00，写进 JSON 字符串就是裸
     * NUL；代理对 0xED 0xA0 0x80 则是孤立半个字符。两者都会让 jq 报错，
     * 和"清洗标题里脏字节"的初衷正好相反。
     */
    /* 过长编码的 NUL：两字节 → 2 个 U+FFFD */
    woodland_json_string(&j, "\xC0\x80");
    assert_fffd(&j, 2);
    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* UTF-16 代理区（U+D800）：三字节 → 3 个 U+FFFD */
    woodland_json_string(&j, "\xED\xA0\x80");
    assert_fffd(&j, 3);
    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* 超出 U+10FFFF（0xF5 开头）：四字节 → 4 个 U+FFFD */
    woodland_json_string(&j, "\xF5\x80\x80\x80");
    assert_fffd(&j, 4);
    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* 合法的四字节字符不能被误伤 */
    woodland_json_string(&j, "\xF0\x9F\x98\x80");   /* U+1F600 */
    assert(strcmp(woodland_json_data(&j), "\"\xF0\x9F\x98\x80\"") == 0);

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

    /* ---------- 对象/数组写入器：逗号自动管理 ---------- */
    woodland_json_init(&j);

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, &j);
    woodland_json_obj_bool(&obj, "ok", true);
    woodland_json_obj_str(&obj, "cmd", "ping");
    woodland_json_obj_int(&obj, "pid", 42);
    woodland_json_obj_key(&obj, "tags");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, &j);
    woodland_json_arr_sep(&arr);
    woodland_json_string(&j, "a");
    woodland_json_arr_sep(&arr);
    woodland_json_string(&j, "b");
    woodland_json_arr_close(&arr);
    woodland_json_obj_close(&obj);
    /* 首字段无前置逗号、末字段无尾逗号、数组元素分隔恰好——
     * 这三处任何一处错了，客户端 jq 解析直接失败 */
    assert(strcmp(woodland_json_data(&j),
                  "{\"ok\":true,\"cmd\":\"ping\",\"pid\":42,"
                  "\"tags\":[\"a\",\"b\"]}") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* 空数组 / 空对象：sep 一次都没调时就是 [] / {} */
    struct woodland_json_arr empty_arr;
    woodland_json_arr_open(&empty_arr, &j);
    woodland_json_arr_close(&empty_arr);
    struct woodland_json_obj empty_obj;
    woodland_json_obj_open(&empty_obj, &j);
    woodland_json_obj_close(&empty_obj);
    assert(strcmp(woodland_json_data(&j), "[]{}") == 0);

    woodland_json_finish(&j);
    woodland_json_init(&j);

    /* first=false：接着手写前缀继续写（get_focus/self_check 的用法），
     * 第一个字段要补前置逗号 */
    woodland_json_raw(&j, "{\"cmd\":\"get_focus\"");
    struct woodland_json_obj cont = {.json = &j, .first = false};
    woodland_json_obj_bool(&cont, "seat", false);
    woodland_json_obj_str(&cont, "title", NULL);
    woodland_json_obj_close(&cont);
    assert(strcmp(woodland_json_data(&j),
                  "{\"cmd\":\"get_focus\",\"seat\":false,\"title\":null}") == 0);

    woodland_json_finish(&j);

    return 0;
}
