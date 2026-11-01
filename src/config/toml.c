// SPDX-License-Identifier: GPL-2.0-only
#include "toml.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *errbuf, size_t errbuf_len, const char *fmt, int line, const char *msg)
{
    if (errbuf && errbuf_len > 0) {
        snprintf(errbuf, errbuf_len, "line %d: %s", line, msg ? msg : fmt);
    }
}

static char *skip_ws(char *p)
{
    while (*p && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static void trim_right(char *p)
{
    size_t len = strlen(p);
    while (len > 0 &&
           (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n')) {
        p[--len] = '\0';
    }
}

static void toml_free_value(struct toml_value *v)
{
    if (!v) {
        return;
    }
    if (v->type == TOML_STRING) {
        free(v->s);
        v->s = NULL;
    } else if (v->type == TOML_ARRAY) {
        for (size_t i = 0; i < v->arr.count; i++) {
            toml_free_value(&v->arr.items[i]);
        }
        free(v->arr.items);
        v->arr.items = NULL;
        v->arr.count = 0;
    }
}

static void toml_free_nodes(struct toml_node *node)
{
    while (node) {
        struct toml_node *next = node->next;
        free(node->key);
        toml_free_value(&node->val);
        free(node);
        node = next;
    }
}

static void toml_free_table(struct toml_table *tbl)
{
    if (!tbl) {
        return;
    }
    free(tbl->name);
    toml_free_nodes(tbl->nodes);
    free(tbl);
}

void toml_free(struct toml_doc *doc)
{
    if (!doc) {
        return;
    }
    toml_free_table(doc->root);
    struct toml_table *t = doc->tables;
    while (t) {
        struct toml_table *next = t->next;
        toml_free_table(t);
        t = next;
    }
    free(doc);
}

static struct toml_table *toml_create_table(const char *name, bool is_array_elem)
{
    struct toml_table *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    if (name) {
        t->name = strdup(name);
    }
    t->is_array_elem = is_array_elem;
    return t;
}

struct toml_table *toml_find_table(const struct toml_doc *doc, const char *name)
{
    if (!doc || !name) {
        return NULL;
    }
    for (struct toml_table *t = doc->tables; t; t = t->next) {
        if (t->name && strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

struct toml_table *toml_next_table(const struct toml_table *tbl, const char *name)
{
    if (!tbl || !name) {
        return NULL;
    }
    for (struct toml_table *t = tbl->next; t; t = t->next) {
        if (t->name && strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

const struct toml_value *toml_get(const struct toml_table *tbl, const char *key)
{
    if (!tbl || !key) {
        return NULL;
    }
    for (struct toml_node *n = tbl->nodes; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            return &n->val;
        }
    }
    return NULL;
}

const char *toml_get_string(const struct toml_table *tbl, const char *key, const char *def)
{
    const struct toml_value *v = toml_get(tbl, key);
    return (v && v->type == TOML_STRING) ? v->s : def;
}

int64_t toml_get_int(const struct toml_table *tbl, const char *key, int64_t def)
{
    const struct toml_value *v = toml_get(tbl, key);
    return (v && v->type == TOML_INT) ? v->i : def;
}

double toml_get_float(const struct toml_table *tbl, const char *key, double def)
{
    const struct toml_value *v = toml_get(tbl, key);
    if (!v) {
        return def;
    }
    if (v->type == TOML_FLOAT) {
        return v->d;
    }
    if (v->type == TOML_INT) {
        return (double)v->i;
    }
    return def;
}

bool toml_get_bool(const struct toml_table *tbl, const char *key, bool def)
{
    const struct toml_value *v = toml_get(tbl, key);
    return (v && v->type == TOML_BOOL) ? v->b : def;
}

/* 解析双引号转义字符串 */
static char *parse_quoted_string(const char *src, const char **endpos)
{
    if (*src != '"') {
        return NULL;
    }
    src++; // 跳过起始 '"'
    size_t cap = 32;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    while (*src && *src != '"') {
        char ch = *src++;
        if (ch == '\\') {
            if (!*src) {
                free(buf);
                return NULL;
            }
            char esc = *src++;
            switch (esc) {
            case '"':
                ch = '"';
                break;
            case '\\':
                ch = '\\';
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 't':
                ch = '\t';
                break;
            case 'r':
                ch = '\r';
                break;
            default:
                free(buf);
                return NULL;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        buf[len++] = ch;
    }

    if (*src != '"') {
        free(buf);
        return NULL; // 未闭合字符串
    }
    src++; // 跳过结束 '"'
    buf[len] = '\0';
    if (endpos) {
        *endpos = src;
    }
    return buf;
}

/* 解析字面量单引号字符串 */
static char *parse_literal_string(const char *src, const char **endpos)
{
    if (*src != '\'') {
        return NULL;
    }
    src++;
    const char *start = src;
    while (*src && *src != '\'') {
        src++;
    }
    if (*src != '\'') {
        return NULL;
    }
    size_t len = (size_t)(src - start);
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    src++;
    if (endpos) {
        *endpos = src;
    }
    return buf;
}

/* 单个值解析 */
static bool parse_value(const char *src, struct toml_value *out, const char **endpos)
{
    while (*src == ' ' || *src == '\t') {
        src++;
    }
    if (!*src) {
        return false;
    }

    /* 1. 字符串 */
    if (*src == '"') {
        char *s = parse_quoted_string(src, endpos);
        if (!s) {
            return false;
        }
        out->type = TOML_STRING;
        out->s = s;
        return true;
    }
    if (*src == '\'') {
        char *s = parse_literal_string(src, endpos);
        if (!s) {
            return false;
        }
        out->type = TOML_STRING;
        out->s = s;
        return true;
    }

    /* 2. 数组 [item, item] */
    if (*src == '[') {
        src++; // 跳过 '['
        size_t cap = 4;
        size_t count = 0;
        struct toml_value *items = malloc(cap * sizeof(*items));
        if (!items) {
            return false;
        }

        while (true) {
            while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
                src++;
            }
            if (*src == ']') {
                src++;
                break;
            }
            if (!*src) {
                for (size_t i = 0; i < count; i++) {
                    toml_free_value(&items[i]);
                }
                free(items);
                return false;
            }
            struct toml_value val = {0};
            const char *next_pos = NULL;
            if (!parse_value(src, &val, &next_pos)) {
                for (size_t i = 0; i < count; i++) {
                    toml_free_value(&items[i]);
                }
                free(items);
                return false;
            }
            if (count >= cap) {
                cap *= 2;
                struct toml_value *ni = realloc(items, cap * sizeof(*ni));
                if (!ni) {
                    toml_free_value(&val);
                    for (size_t i = 0; i < count; i++) {
                        toml_free_value(&items[i]);
                    }
                    free(items);
                    return false;
                }
                items = ni;
            }
            items[count++] = val;
            src = next_pos;
            while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
                src++;
            }
            if (*src == ',') {
                src++;
            } else if (*src == ']') {
                src++;
                break;
            } else {
                for (size_t i = 0; i < count; i++) {
                    toml_free_value(&items[i]);
                }
                free(items);
                return false;
            }
        }

        out->type = TOML_ARRAY;
        out->arr.items = items;
        out->arr.count = count;
        if (endpos) {
            *endpos = src;
        }
        return true;
    }

    /* 3. 布尔值 */
    if (strncmp(src, "true", 4) == 0 && (!src[4] || isspace((unsigned char)src[4]) ||
                                         src[4] == ',' || src[4] == ']' || src[4] == '#')) {
        out->type = TOML_BOOL;
        out->b = true;
        if (endpos) {
            *endpos = src + 4;
        }
        return true;
    }
    if (strncmp(src, "false", 5) == 0 && (!src[5] || isspace((unsigned char)src[5]) ||
                                          src[5] == ',' || src[5] == ']' || src[5] == '#')) {
        out->type = TOML_BOOL;
        out->b = false;
        if (endpos) {
            *endpos = src + 5;
        }
        return true;
    }

    /* 4. 数值 (整数或浮点数) */
    char *end = NULL;
    errno = 0;
    /* 检查是否含小数点或科学计数法 */
    const char *p = src;
    bool is_float = false;
    if (*p == '+' || *p == '-') {
        p++;
    }
    while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E')) {
        if (*p == '.' || *p == 'e' || *p == 'E') {
            is_float = true;
        }
        p++;
    }

    if (is_float) {
        double d = strtod(src, &end);
        if (end != src && (end == p || !*end || isspace((unsigned char)*end) || *end == ',' ||
                           *end == ']' || *end == '#')) {
            out->type = TOML_FLOAT;
            out->d = d;
            if (endpos) {
                *endpos = end;
            }
            return true;
        }
    } else {
        int64_t i = strtoll(src, &end, 0);
        if (end != src && (end == p || !*end || isspace((unsigned char)*end) || *end == ',' ||
                           *end == ']' || *end == '#')) {
            out->type = TOML_INT;
            out->i = i;
            if (endpos) {
                *endpos = end;
            }
            return true;
        }
    }

    return false;
}

struct toml_doc *toml_parse(const char *src, char *errbuf, size_t errbuf_len)
{
    if (!src) {
        set_err(errbuf, errbuf_len, NULL, 0, "source is null");
        return NULL;
    }

    struct toml_doc *doc = calloc(1, sizeof(*doc));
    if (!doc) {
        set_err(errbuf, errbuf_len, NULL, 0, "out of memory");
        return NULL;
    }
    doc->root = toml_create_table(NULL, false);
    if (!doc->root) {
        toml_free(doc);
        return NULL;
    }

    struct toml_table *current_tbl = doc->root;

    /* 复制一份用于按行切割处理 */
    char *content = strdup(src);
    if (!content) {
        toml_free(doc);
        return NULL;
    }

    char *saveptr = NULL;
    char *line = strtok_r(content, "\n", &saveptr);
    int line_no = 0;

    while (line) {
        line_no++;
        char *p = skip_ws(line);

        /* 忽略注释和空行 */
        if (!*p || *p == '#') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* 1. 数组表 [[name]] */
        if (p[0] == '[' && p[1] == '[') {
            char *close_bracket = strstr(p + 2, "]]");
            if (!close_bracket) {
                set_err(errbuf, errbuf_len, NULL, line_no,
                        "malformed table array header (missing ']]')");
                free(content);
                toml_free(doc);
                return NULL;
            }
            char *after = skip_ws(close_bracket + 2);
            if (*after && *after != '#') {
                set_err(errbuf, errbuf_len, NULL, line_no,
                        "unexpected characters after table header");
                free(content);
                toml_free(doc);
                return NULL;
            }
            *close_bracket = '\0';
            char *tbl_name = skip_ws(p + 2);
            trim_right(tbl_name);
            if (!*tbl_name) {
                set_err(errbuf, errbuf_len, NULL, line_no, "empty table array name");
                free(content);
                toml_free(doc);
                return NULL;
            }

            struct toml_table *new_tbl = toml_create_table(tbl_name, true);
            if (!new_tbl) {
                free(content);
                toml_free(doc);
                return NULL;
            }
            /* 追加到 doc->tables 尾部 */
            if (!doc->tables) {
                doc->tables = new_tbl;
            } else {
                struct toml_table *tail = doc->tables;
                while (tail->next) {
                    tail = tail->next;
                }
                tail->next = new_tbl;
            }
            current_tbl = new_tbl;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* 2. 普通表 [name] */
        if (p[0] == '[') {
            char *close_bracket = strchr(p + 1, ']');
            if (!close_bracket) {
                set_err(errbuf, errbuf_len, NULL, line_no, "malformed table header (missing ']')");
                free(content);
                toml_free(doc);
                return NULL;
            }
            char *after = skip_ws(close_bracket + 1);
            if (*after && *after != '#') {
                set_err(errbuf, errbuf_len, NULL, line_no,
                        "unexpected characters after table header");
                free(content);
                toml_free(doc);
                return NULL;
            }
            *close_bracket = '\0';
            char *tbl_name = skip_ws(p + 1);
            trim_right(tbl_name);
            if (!*tbl_name) {
                set_err(errbuf, errbuf_len, NULL, line_no, "empty table name");
                free(content);
                toml_free(doc);
                return NULL;
            }

            /* 如果表已存在则复用，否则新建追加 */
            struct toml_table *existing = toml_find_table(doc, tbl_name);
            if (existing && !existing->is_array_elem) {
                current_tbl = existing;
            } else {
                struct toml_table *new_tbl = toml_create_table(tbl_name, false);
                if (!new_tbl) {
                    free(content);
                    toml_free(doc);
                    return NULL;
                }
                if (!doc->tables) {
                    doc->tables = new_tbl;
                } else {
                    struct toml_table *tail = doc->tables;
                    while (tail->next) {
                        tail = tail->next;
                    }
                    tail->next = new_tbl;
                }
                current_tbl = new_tbl;
            }
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* 3. 键值对 key = value */
        char *eq = strchr(p, '=');
        if (!eq) {
            set_err(errbuf, errbuf_len, NULL, line_no, "syntax error: expected '='");
            free(content);
            toml_free(doc);
            return NULL;
        }

        *eq = '\0';
        char *key = skip_ws(p);
        trim_right(key);
        if (!*key) {
            set_err(errbuf, errbuf_len, NULL, line_no, "empty key name");
            free(content);
            toml_free(doc);
            return NULL;
        }

        char *val_str = skip_ws(eq + 1);
        struct toml_value val = {0};
        const char *endpos = NULL;
        if (!parse_value(val_str, &val, &endpos)) {
            set_err(errbuf, errbuf_len, NULL, line_no, "malformed value expression");
            free(content);
            toml_free(doc);
            return NULL;
        }

        /* 校验值后是否有未闭合非法字符（除了空白或行尾注释） */
        if (endpos) {
            const char *trailing = skip_ws((char *)endpos);
            if (*trailing && *trailing != '#') {
                set_err(errbuf, errbuf_len, NULL, line_no, "unexpected characters after value");
                toml_free_value(&val);
                free(content);
                toml_free(doc);
                return NULL;
            }
        }

        /* 挂载节点到当前表 */
        struct toml_node *node = calloc(1, sizeof(*node));
        if (!node) {
            toml_free_value(&val);
            free(content);
            toml_free(doc);
            return NULL;
        }
        node->key = strdup(key);
        node->val = val;

        /* 追加到 current_tbl->nodes 链表 */
        if (!current_tbl->nodes) {
            current_tbl->nodes = node;
        } else {
            struct toml_node *tail = current_tbl->nodes;
            while (tail->next) {
                tail = tail->next;
            }
            tail->next = node;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(content);
    return doc;
}
