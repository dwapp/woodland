// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TOML_H
#define WOODLAND_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum toml_type {
    TOML_STRING,
    TOML_INT,
    TOML_FLOAT,
    TOML_BOOL,
    TOML_ARRAY,
    TOML_TABLE,
};

struct toml_table;

struct toml_value {
    enum toml_type type;
    union {
        char *s;
        int64_t i;
        double d;
        bool b;
        struct {
            struct toml_value *items;
            size_t count;
        } arr;
        struct toml_table *tbl;
    };
};

struct toml_node {
    char *key;
    struct toml_value val;
    struct toml_node *next;
};

struct toml_table {
    char *name;         /* 表名，如 "general"、"decorations" */
    bool is_array_elem; /* 是否由 [[table]] 产生 */
    struct toml_node *nodes;
    struct toml_table *next;
};

struct toml_doc {
    struct toml_table *root;   /* 顶层无 section 键值表 */
    struct toml_table *tables; /* 所有显式 section 表链表 */
};

/**
 * 解析 TOML 字符串。解析成功返回 doc；失败返回 NULL 并在 errbuf 输出错误（若提供）。
 */
struct toml_doc *toml_parse(const char *src, char *errbuf, size_t errbuf_len);

/**
 * 释放 TOML 文档及全部节点。
 */
void toml_free(struct toml_doc *doc);

/**
 * 按名称查找首个 section 表（如 "general" 或 "decorations"）。
 */
struct toml_table *toml_find_table(const struct toml_doc *doc, const char *name);

/**
 * 查找同名的下一个表（用于 [[output]] 等数组表）。
 */
struct toml_table *toml_next_table(const struct toml_table *tbl, const char *name);

/**
 * 从表中获取指定 key 的值节点。
 */
const struct toml_value *toml_get(const struct toml_table *tbl, const char *key);

/**
 * 基础类型查询便捷函数（若未找到或类型不符，返回 def）。
 */
const char *toml_get_string(const struct toml_table *tbl, const char *key, const char *def);
int64_t toml_get_int(const struct toml_table *tbl, const char *key, int64_t def);
double toml_get_float(const struct toml_table *tbl, const char *key, double def);
bool toml_get_bool(const struct toml_table *tbl, const char *key, bool def);

#endif /* WOODLAND_TOML_H */
