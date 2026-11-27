// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-appearance-manager 设置的持久化。
 *
 * 存成普通 JSON 文件（与 wallpaper.json 同一个取舍：不引 JSON 库，手写一个
 * 只认"这个固定形状"的解析器）：`$XDG_CONFIG_HOME/woodland/appearance.json`，
 * 单一扁平对象，键与协议 set_* 一一对应（11 项）。
 *
 * 为什么是独立文件而不是写回 config.toml：
 *   config.toml 由 toml.c 解析，只读、无 TOML 序列化器；写回要么重写整个
 *   文件（丢掉注释与排版），要么实现一套 TOML 写器。而 appearance 的 11 项
 *   是自成一体的"用户级外观状态"，独立 JSON 与 wallpaper.json 同构，读写都
 *   是几十行，符合"单一权威文件"的心智模型。
 *
 * 权限模型与 wallpaper 一致：单用户 session 合成器，配置目录 0755、文件按
 * 默认 umask 写，不额外加密。文件缺失或解析失败都回退到当前内存值，不让
 * 合成器因此起不来。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "config.h"
#include "cursor.h"
#include "ssd.h"
#include "util/json.h"
#include "woodland_config.h"

/* 一份 11 项配置远小于 4KB，64KB 是防御性上限 */
#define APPEARANCE_CONFIG_MAX (64u * 1024u)

/* ==================== 路径 ==================== */

static bool appearance_dir(char *buf, size_t size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        return snprintf(buf, size, "%s/woodland", xdg) < (int)size;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') {
        return false;
    }
    return snprintf(buf, size, "%s/.config/woodland", home) < (int)size;
}

static bool appearance_path(char *buf, size_t size)
{
    char dir[512];
    if (!appearance_dir(dir, sizeof(dir))) {
        return false;
    }
    return snprintf(buf, size, "%s/appearance.json", dir) < (int)size;
}

/* mkdir -p（与 wallpaper_config.c 同一套） */
static bool mkdir_parents(const char *path)
{
    char buf[512];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        return false;
    }
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

/* ==================== 极简 JSON 解析（只认本文件的固定形状） ==================== */

struct ap_json {
    const char *p;
};

static void ap_ws(struct ap_json *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') {
        j->p++;
    }
}

static bool ap_eat(struct ap_json *j, char c)
{
    ap_ws(j);
    if (*j->p != c) {
        return false;
    }
    j->p++;
    return true;
}

static bool ap_buf_put(char **buf, size_t *cap, size_t *len, const char *bytes, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t want = (*len + n + 1) * 2;
        char *grown = realloc(*buf, want);
        if (!grown) {
            return false;
        }
        *buf = grown;
        *cap = want;
    }
    memcpy(*buf + *len, bytes, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static bool ap_hex4(const char *p, unsigned *out)
{
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = value;
    return true;
}

/* 字符串：处理转义（含 \uXXXX 与代理对），产出 UTF-8 */
static bool ap_parse_string(struct ap_json *j, char **out)
{
    ap_ws(j);
    if (*j->p != '"') {
        return false;
    }
    j->p++;

    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return false;
    }
    buf[0] = '\0';

    while (*j->p && *j->p != '"') {
        char bytes[4];
        size_t n = 1;
        bytes[0] = *j->p++;

        if (bytes[0] == '\\') {
            if (*j->p == '\0') {
                goto fail;
            }
            char esc = *j->p++;
            switch (esc) {
            case '"':  bytes[0] = '"';  break;
            case '\\': bytes[0] = '\\'; break;
            case '/':  bytes[0] = '/';  break;
            case 'b':  bytes[0] = '\b'; break;
            case 'f':  bytes[0] = '\f'; break;
            case 'n':  bytes[0] = '\n'; break;
            case 'r':  bytes[0] = '\r'; break;
            case 't':  bytes[0] = '\t'; break;
            case 'u': {
                unsigned cp;
                if (!ap_hex4(j->p, &cp)) {
                    goto fail;
                }
                j->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (j->p[0] == '\\' && j->p[1] == 'u' && ap_hex4(j->p + 2, &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        j->p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                if (cp < 0x80) {
                    bytes[0] = (char)cp;
                    n = 1;
                } else if (cp < 0x800) {
                    bytes[0] = (char)(0xC0 | (cp >> 6));
                    bytes[1] = (char)(0x80 | (cp & 0x3F));
                    n = 2;
                } else if (cp < 0x10000) {
                    bytes[0] = (char)(0xE0 | (cp >> 12));
                    bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    bytes[2] = (char)(0x80 | (cp & 0x3F));
                    n = 3;
                } else {
                    bytes[0] = (char)(0xF0 | (cp >> 18));
                    bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    bytes[3] = (char)(0x80 | (cp & 0x3F));
                    n = 4;
                }
                break;
            }
            default:
                goto fail;
            }
        }

        if (!ap_buf_put(&buf, &cap, &len, bytes, n)) {
            goto fail;
        }
    }
    if (*j->p != '"') {
        goto fail;
    }
    j->p++;
    *out = buf;
    return true;

fail:
    free(buf);
    return false;
}

/* null / true / false 字面量：先 consume 再回填值 */
static bool ap_parse_literal(struct ap_json *j, const char *lit)
{
    ap_ws(j);
    size_t n = strlen(lit);
    if (strncmp(j->p, lit, n) != 0) {
        return false;
    }
    j->p += n;
    return true;
}

static bool ap_parse_null(struct ap_json *j)
{
    return ap_parse_literal(j, "null");
}

static bool ap_parse_bool(struct ap_json *j, bool *out)
{
    ap_ws(j);
    if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        *out = true;
        return true;
    }
    if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        *out = false;
        return true;
    }
    return false;
}

static bool ap_parse_number(struct ap_json *j, double *out)
{
    ap_ws(j);
    char *end = NULL;
    double value = strtod(j->p, &end);
    if (end == j->p) {
        return false;
    }
    j->p = end;
    *out = value;
    return true;
}

/* 跳过任意值（未知字段将来出现时旧配置仍能读）。当前解析器在
 * ap_parse_config 里对每个键都按类型解析后交给 ap_apply_field（未知键直接
 * 忽略），所以这个函数暂时用不到；将来若改成流式解析再补。 */

/*
 * 把解析出的 JSON 值覆写到 config。
 *
 * 语义与协议 set_* 严格对齐：字符串 NULL 或空串回落到"未设/默认"，
 * 越界值 clamp 到协议同样的范围（复用 appearance.c 的 clamp 会成环依赖，
 * 这里只写 config 层自己掌握的边界）。
 */
static void ap_apply_field(struct woodland_config *cfg, const char *key,
                           const char *str, bool has_str, double num, bool is_bool,
                           bool bool_val, bool is_null)
{
    if (strcmp(key, "cursor_theme") == 0) {
        if (!is_null && has_str && str[0]) {
            char *copy = strdup(str);
            if (copy) {
                free(cfg->cursor.theme);
                cfg->cursor.theme = copy;
            }
        }
    } else if (strcmp(key, "cursor_size") == 0) {
        if (!is_null && !is_bool && !has_str) {
            long v = (long)num;
            if (v < WOODLAND_CURSOR_MIN_SIZE) {
                v = WOODLAND_CURSOR_MIN_SIZE;
            }
            if (v > WOODLAND_CURSOR_MAX_SIZE) {
                v = WOODLAND_CURSOR_MAX_SIZE;
            }
            cfg->cursor.size = (int32_t)v;
        }
    } else if (strcmp(key, "font") == 0) {
        if (!is_null && has_str && str[0]) {
            char *copy = strdup(str);
            if (copy) {
                free(cfg->ssd.font);
                cfg->ssd.font = copy;
            }
        }
    } else if (strcmp(key, "titlebar_height") == 0) {
        if (!is_null && !is_bool && !has_str) {
            long v = (long)num;
            if (v < 0) {
                v = 0;
            }
            if (v > 200) {
                v = 200;
            }
            cfg->ssd.titlebar_height = (int32_t)v;
        }
    } else if (strcmp(key, "corner_radius") == 0) {
        if (!is_null && !is_bool && !has_str) {
            long v = (long)num;
            if (v < 0) {
                v = 0;
            }
            if (v > WOODLAND_SSD_MAX_SIZE) {
                v = WOODLAND_SSD_MAX_SIZE;
            }
            cfg->effects.corner_radius = (int32_t)v;
        }
    } else if (strcmp(key, "accent_rgb") == 0) {
        if (!is_null && !is_bool && !has_str) {
            long v = (long)num;
            if (v < 0) {
                v = 0;
            }
            if (v > 0xFFFFFF) {
                v = 0xFFFFFF;
            }
            /* 只存值，不翻转 accent_set：accent_set 才是权威（文件里写在前面，
             * 消费方都先判 accent_set 才用 rgb）。旧文件只有 rgb 没有 set 时
             * 这个值也是死的，不会误触发。 */
            cfg->appearance.accent_rgb = (uint32_t)v;
        }
    } else if (strcmp(key, "accent_set") == 0) {
        if (!is_null && is_bool) {
            cfg->appearance.accent_set = bool_val;
        }
    } else if (strcmp(key, "color_scheme_set") == 0) {
        if (!is_null && is_bool) {
            cfg->appearance.color_scheme_set = bool_val;
        }
    } else if (strcmp(key, "dark") == 0) {
        if (!is_null && is_bool) {
            cfg->appearance.dark = bool_val;
        }
    } else if (strcmp(key, "icon_theme") == 0) {
        if (is_null || (has_str && !str[0])) {
            free(cfg->appearance.icon_theme);
            cfg->appearance.icon_theme = NULL;
        } else if (has_str) {
            char *copy = strdup(str);
            if (copy) {
                free(cfg->appearance.icon_theme);
                cfg->appearance.icon_theme = copy;
            }
        }
    } else if (strcmp(key, "monospace_font") == 0) {
        if (is_null || (has_str && !str[0])) {
            free(cfg->appearance.monospace_font);
            cfg->appearance.monospace_font = NULL;
        } else if (has_str) {
            char *copy = strdup(str);
            if (copy) {
                free(cfg->appearance.monospace_font);
                cfg->appearance.monospace_font = copy;
            }
        }
    } else if (strcmp(key, "window_opacity") == 0) {
        if (!is_null && !is_bool && !has_str) {
            if (num < 0.0) {
                num = 0.0;
            }
            if (num > 1.0) {
                num = 1.0;
            }
            /* 只存值，不翻转 opacity_set：opacity_set 才是权威（文件里写在
             * window_opacity 之前，消费方都先判 opacity_set 才用值）。否则
             * 未设过透明度时 save 写出的 0.000 会在重载时把 opacity_set 翻成
             * true，观测端从默认 1.0 错成 0.0。 */
            cfg->appearance.window_opacity = num;
        }
    } else if (strcmp(key, "opacity_set") == 0) {
        if (!is_null && is_bool) {
            cfg->appearance.opacity_set = bool_val;
        }
    }
    /* 未知键忽略：将来上游加字段时旧文件仍能读 */
}

/*
 * 解析顶层 `{ ... }`，逐键覆写 config。
 * 只覆盖出现的键，其余保持现状——这样部分损坏/部分缺失的文件也尽量恢复。
 */
static bool ap_parse_config(const char *text, struct woodland_config *cfg)
{
    struct ap_json j = {text};
    if (!ap_eat(&j, '{')) {
        return false;
    }
    ap_ws(&j);
    if (*j.p == '}') {
        j.p++;
        ap_ws(&j);
        return *j.p == '\0';
    }
    for (;;) {
        char *key = NULL;
        if (!ap_parse_string(&j, &key) || !ap_eat(&j, ':')) {
            free(key);
            return false;
        }

        /* 判断值类型：字符串 / 数字 / 布尔 / null */
        ap_ws(&j);
        if (*j.p == '"') {
            char *str = NULL;
            if (!ap_parse_string(&j, &str)) {
                free(key);
                return false;
            }
            ap_apply_field(cfg, key, str, true, 0.0, false, false, false);
            free(str);
        } else if (*j.p == 't' || *j.p == 'f') {
            bool b = false;
            if (!ap_parse_bool(&j, &b)) {
                free(key);
                return false;
            }
            ap_apply_field(cfg, key, NULL, false, 0.0, true, b, false);
        } else if (*j.p == 'n') {
            if (!ap_parse_null(&j)) {
                free(key);
                return false;
            }
            ap_apply_field(cfg, key, NULL, false, 0.0, false, false, true);
        } else {
            double num = 0.0;
            if (!ap_parse_number(&j, &num)) {
                free(key);
                return false;
            }
            ap_apply_field(cfg, key, NULL, false, num, false, false, false);
        }

        free(key);
        ap_ws(&j);
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p == '}') {
            j.p++;
            break;
        }
        return false;
    }
    ap_ws(&j);
    return *j.p == '\0';
}

/* ==================== 写入 ==================== */

static void json_write(const struct woodland_config *cfg, struct woodland_json *out)
{
    struct woodland_json_obj o;
    woodland_json_obj_open(&o, out);
    woodland_json_obj_str(&o, "cursor_theme", cfg->cursor.theme);
    woodland_json_obj_int(&o, "cursor_size", cfg->cursor.size);
    woodland_json_obj_str(&o, "font", cfg->ssd.font);
    woodland_json_obj_str(&o, "monospace_font", cfg->appearance.monospace_font);
    woodland_json_obj_str(&o, "icon_theme", cfg->appearance.icon_theme);
    woodland_json_obj_bool(&o, "accent_set", cfg->appearance.accent_set);
    woodland_json_obj_int(&o, "accent_rgb", (long long)cfg->appearance.accent_rgb);
    woodland_json_obj_bool(&o, "opacity_set", cfg->appearance.opacity_set);
    woodland_json_obj_key(&o, "window_opacity");
    woodland_json_fmt(out, "%.3f", cfg->appearance.window_opacity);
    woodland_json_obj_bool(&o, "color_scheme_set", cfg->appearance.color_scheme_set);
    woodland_json_obj_bool(&o, "dark", cfg->appearance.dark);
    woodland_json_obj_int(&o, "titlebar_height", cfg->ssd.titlebar_height);
    woodland_json_obj_int(&o, "corner_radius", cfg->effects.corner_radius);
    woodland_json_obj_close(&o);
}

/* ==================== 公开入口 ==================== */

void woodland_config_appearance_overlay(struct woodland_config *config)
{
    if (!config) {
        return;
    }
    char path[600];
    if (!appearance_path(path, sizeof(path))) {
        return;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        /* 文件不存在：首次运行，用默认值 */
        return;
    }
    char *text = calloc(1, APPEARANCE_CONFIG_MAX);
    if (!text) {
        fclose(file);
        return;
    }
    size_t len = fread(text, 1, APPEARANCE_CONFIG_MAX - 1, file);
    fclose(file);
    text[len] = '\0';

    if (len == 0 || !ap_parse_config(text, config)) {
        wlr_log(WLR_ERROR, "appearance config %s is malformed, keeping current values", path);
    } else {
        wlr_log(WLR_INFO, "loaded appearance config: %s", path);
    }
    free(text);
}

bool woodland_config_appearance_save(const struct woodland_config *config)
{
    if (!config) {
        return false;
    }
    char path[600];
    char dir[512];
    if (!appearance_path(path, sizeof(path)) || !appearance_dir(dir, sizeof(dir))) {
        return false;
    }
    if (!mkdir_parents(dir)) {
        wlr_log(WLR_ERROR, "cannot create config dir %s", dir);
        return false;
    }

    /* 先写临时文件再 rename：配置只有几十字节，但写到一半被杀会留下半截
     * JSON，下次启动解析失败就回到默认外观。rename 是原子的。 */
    char tmp[700];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return false;
    }
    FILE *file = fopen(tmp, "w");
    if (!file) {
        wlr_log(WLR_ERROR, "cannot write %s: %s", tmp, strerror(errno));
        return false;
    }

    struct woodland_json json;
    woodland_json_init(&json);
    json_write(config, &json);
    bool ok = woodland_json_ok(&json) &&
              fwrite(woodland_json_data(&json), 1, woodland_json_len(&json), file) ==
                  woodland_json_len(&json);
    woodland_json_finish(&json);
    if (fputs("\n", file) == EOF) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok || rename(tmp, path) != 0) {
        wlr_log(WLR_ERROR, "cannot save appearance config to %s", path);
        unlink(tmp);
        return false;
    }
    return true;
}
