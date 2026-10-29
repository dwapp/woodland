// SPDX-License-Identifier: GPL-2.0-only
#include "util/json.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_INITIAL_CAP 256
/* 单次 fmt 的栈上缓冲：够放下任何一条字段，超了按 JSON 规范截断也无妨 */
#define JSON_FMT_MAX 512

static void json_append(struct woodland_json *json, const char *text,
                        size_t len) {
    if (!json || json->oom || len == 0) {
        return;
    }
    if (json->len + len + 1 > json->cap) {
        size_t cap = json->cap ? json->cap : JSON_INITIAL_CAP;
        while (cap < json->len + len + 1) {
            cap *= 2;
        }
        char *data = realloc(json->data, cap);
        if (!data) {
            json->oom = true;
            return;
        }
        json->data = data;
        json->cap = cap;
    }
    memcpy(json->data + json->len, text, len);
    json->len += len;
    json->data[json->len] = '\0';
}

void woodland_json_init(struct woodland_json *json) {
    if (!json) {
        return;
    }
    *json = (struct woodland_json){0};
    /* 先分配一块：没有输出的命令也能拿到 ""，省掉各处判空 */
    json->data = malloc(JSON_INITIAL_CAP);
    if (!json->data) {
        json->oom = true;
        return;
    }
    json->data[0] = '\0';
    json->cap = JSON_INITIAL_CAP;
}

void woodland_json_finish(struct woodland_json *json) {
    if (!json) {
        return;
    }
    free(json->data);
    *json = (struct woodland_json){0};
}

const char *woodland_json_data(const struct woodland_json *json) {
    return (json && json->data) ? json->data : "";
}

size_t woodland_json_len(const struct woodland_json *json) {
    return json ? json->len : 0;
}

bool woodland_json_ok(const struct woodland_json *json) {
    return json && !json->oom;
}

void woodland_json_raw(struct woodland_json *json, const char *text) {
    if (!text) {
        return;
    }
    json_append(json, text, strlen(text));
}

void woodland_json_fmt(struct woodland_json *json, const char *fmt, ...) {
    if (!json || !fmt) {
        return;
    }
    char buf[JSON_FMT_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    json_append(json, buf, len);
}

/*
 * 按 UTF-8 解码一个字符，返回消耗的字节数；非法序列返回 0。
 *
 * 只看结构合法性（首字节长度前缀 + 续字节 10xxxxxx），不校验码点范围：
 * 目标是"让 jq 能解析"，过度校正反而会把合法但少见的字符吃掉。
 */
static size_t utf8_decode(const unsigned char *s, uint32_t *out) {
    unsigned char c = s[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *out = cp;
    return len;
}

static void json_append_utf8(struct woodland_json *json, uint32_t cp) {
    char buf[4];
    if (cp < 0x80) {
        buf[0] = (char)cp;
        json_append(json, buf, 1);
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        json_append(json, buf, 2);
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        json_append(json, buf, 3);
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        json_append(json, buf, 4);
    }
}

void woodland_json_string(struct woodland_json *json, const char *text) {
    if (!json || json->oom) {
        return;
    }
    if (!text) {
        json_append(json, "null", 4);
        return;
    }
    json_append(json, "\"", 1);
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            json_append(json, esc, 2);
            s++;
        } else if (c == '\n' || c == '\t' || c == '\r') {
            char esc[2] = {'\\', c == '\n' ? 'n' : (c == '\t' ? 't' : 'r')};
            json_append(json, esc, 2);
            s++;
        } else if (c < 0x20 || c == 0x7F) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            json_append(json, esc, 6);
            s++;
        } else if (c < 0x80) {
            json_append(json, (const char *)s, 1);
            s++;
        } else {
            uint32_t cp = 0;
            size_t len = utf8_decode(s, &cp);
            if (len == 0) {
                /* 非法字节：替换成 U+FFFD，保证输出仍是合法 UTF-8 */
                json_append_utf8(json, 0xFFFD);
                s++;
            } else {
                json_append(json, (const char *)s, len);
                s += len;
            }
        }
    }
    json_append(json, "\"", 1);
}

void woodland_json_key(struct woodland_json *json, const char *key) {
    woodland_json_string(json, key);
    json_append(json, ":", 1);
}

void woodland_json_int(struct woodland_json *json, long long value) {
    woodland_json_fmt(json, "%lld", value);
}

void woodland_json_bool(struct woodland_json *json, bool value) {
    json_append(json, value ? "true" : "false", value ? 4 : 5);
}

void woodland_json_int_array(struct woodland_json *json, const int *values,
                             int count) {
    if (!json || json->oom) {
        return;
    }
    json_append(json, "[", 1);
    for (int i = 0; values && i < count; i++) {
        if (i > 0) {
            json_append(json, ",", 1);
        }
        woodland_json_int(json, values[i]);
    }
    json_append(json, "]", 1);
}
