// SPDX-License-Identifier: GPL-2.0-only
/*
 * 壁纸配置：内存模型、读写、默认值、源路径校验。
 *
 * 配置存成**普通 JSON 文件**（不用 dconfig）：`$XDG_CONFIG_HOME/woodland/
 * wallpaper.json`，结构与 treeland 的 WallpaperOutputConfig 同构——一个
 * output 一项，项里带锁屏壁纸和每个 workspace 的桌面壁纸。
 *
 * 为什么要"每个 workspace 一份"而不是"每个 output 一份"：treeland 的模型
 * 就是这样的（它按 workspace 记，切工作区时才能报 changed），我们照抄是为了
 * 同一份文件两边都能读。**但写入策略仍是 treeland 的**：desktop 角色一次性
 * 写满所有 workspace，所以实际上各 workspace 的值相同——想按工作区设不同
 * 壁纸，需要协议扩展，v1 不做。
 *
 * 这里不做解码（解码在 treeland-wallpaper-factory），校验只到路径级。
 */
/* realpath 在 glibc 里要 _XOPEN_SOURCE >= 500 才声明（工程只给了
 * _POSIX_C_SOURCE，那是另一套特性宏） */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "util/json.h"
#include "wallpaper_internal.h"

/* 最大配置文件尺寸：一份 10 workspace 的配置不到 2KB，1MB 是防御性上限 */
#define WALLPAPER_CONFIG_MAX (1024u * 1024u)

/* ==================== 配置文件路径 ==================== */

static bool config_dir(char *buf, size_t size) {
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

static bool config_path(char *buf, size_t size) {
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) {
        return false;
    }
    return snprintf(buf, size, "%s/wallpaper.json", dir) < (int)size;
}

/* mkdir -p：配置目录多半还不存在（woodland 是唯一写它的人） */
static bool mkdir_parents(const char *path) {
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

/* ==================== 内存模型 ==================== */

static char *dup_or_default(const char *value) {
    return strdup(value ? value : "");
}

void woodland_wallpaper_config_init(struct woodland_wallpaper_config *config) {
    memset(config, 0, sizeof(*config));
}

static void output_finish(struct woodland_wallpaper_output *output) {
    free(output->name);
    free(output->lockscreen);
    for (size_t i = 0; i < output->n_workspaces; i++) {
        free(output->workspaces[i].desktop);
    }
    free(output->workspaces);
    memset(output, 0, sizeof(*output));
}

void woodland_wallpaper_config_finish(struct woodland_wallpaper_config *config) {
    for (size_t i = 0; i < config->n_outputs; i++) {
        output_finish(&config->outputs[i]);
    }
    free(config->outputs);
    memset(config, 0, sizeof(*config));
}

struct woodland_wallpaper_output *
woodland_wallpaper_config_find_output(struct woodland_wallpaper_config *config,
                                      const char *name) {
    if (!config || !name) {
        return NULL;
    }
    for (size_t i = 0; i < config->n_outputs; i++) {
        if (config->outputs[i].name && strcmp(config->outputs[i].name, name) == 0) {
            return &config->outputs[i];
        }
    }
    return NULL;
}

static struct woodland_wallpaper_output *
config_append_output(struct woodland_wallpaper_config *config, const char *name) {    if (config->n_outputs == config->cap_outputs) {
        size_t cap = config->cap_outputs ? config->cap_outputs * 2 : 4;
        struct woodland_wallpaper_output *grown =
            realloc(config->outputs, cap * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        config->outputs = grown;
        config->cap_outputs = cap;
    }
    struct woodland_wallpaper_output *output = &config->outputs[config->n_outputs];
    memset(output, 0, sizeof(*output));
    output->name = strdup(name);
    if (!output->name) {
        return NULL;
    }
    config->n_outputs++;
    return output;
}

static bool workspace_append(struct woodland_wallpaper_output *output, int index,
                             const char *desktop, int type) {
    struct woodland_wallpaper_workspace *grown =
        realloc(output->workspaces, (output->n_workspaces + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    output->workspaces = grown;
    struct woodland_wallpaper_workspace *ws = &output->workspaces[output->n_workspaces];
    memset(ws, 0, sizeof(*ws));
    ws->index = index;
    ws->desktop = dup_or_default(desktop);
    ws->desktop_type = type;
    if (!ws->desktop) {
        return false;
    }
    output->n_workspaces++;
    return true;
}

const char *woodland_wallpaper_config_desktop(const struct woodland_wallpaper_output *output,
                                              int index, int *type) {
    if (!output) {
        return NULL;
    }
    for (size_t i = 0; i < output->n_workspaces; i++) {
        if (output->workspaces[i].index == index) {
            if (type) {
                *type = output->workspaces[i].desktop_type;
            }
            return output->workspaces[i].desktop;
        }
    }
    /* 兜底：workspace 列表还没对齐时用第一个，别让客户端收不到 changed */
    if (output->n_workspaces > 0) {
        if (type) {
            *type = output->workspaces[0].desktop_type;
        }
        return output->workspaces[0].desktop;
    }
    return NULL;
}

struct woodland_wallpaper_output *
woodland_wallpaper_config_ensure_output(struct woodland_wallpaper_config *config,
                                        const char *name, size_t n_workspaces,
                                        bool *created) {
    if (created) {
        *created = false;
    }
    struct woodland_wallpaper_output *output =
        woodland_wallpaper_config_find_output(config, name);
    if (output) {
        size_t before = output->n_workspaces;
        for (size_t i = before; i < n_workspaces; i++) {
            const char *ref = before > 0 ? output->workspaces[0].desktop
                                         : WOODLAND_WALLPAPER_DEFAULT;
            int type = before > 0 ? output->workspaces[0].desktop_type
                                  : WOODLAND_WALLPAPER_IMAGE;
            if (!workspace_append(output, (int)i, ref, type)) {
                return output;
            }
        }
        if (created) {
            *created = output->n_workspaces != before;
        }
        return output;
    }

    /* 新输出：照 treeland 的做法克隆"最后一份配置"，让热插拔的显示器
     * 和主屏一致，而不是突然换成默认图 */
    const struct woodland_wallpaper_output *ref =
        config->n_outputs > 0 ? &config->outputs[config->n_outputs - 1] : NULL;

    output = config_append_output(config, name);
    if (!output) {
        return NULL;
    }
    output->lockscreen = dup_or_default(
        ref && ref->lockscreen ? ref->lockscreen : WOODLAND_WALLPAPER_DEFAULT);
    output->lockscreen_type = ref ? ref->lockscreen_type : WOODLAND_WALLPAPER_IMAGE;

    for (size_t i = 0; i < n_workspaces; i++) {
        const struct woodland_wallpaper_workspace *ref_ws =
            ref && ref->n_workspaces > 0 ? &ref->workspaces[0] : NULL;
        if (!workspace_append(output, (int)i,
                              ref_ws ? ref_ws->desktop : WOODLAND_WALLPAPER_DEFAULT,
                              ref_ws ? ref_ws->desktop_type
                                     : WOODLAND_WALLPAPER_IMAGE)) {
            break;
        }
    }
    if (created) {
        *created = true;
    }
    return output;
}

void woodland_wallpaper_config_remove_output(struct woodland_wallpaper_config *config,
                                             const char *name) {
    for (size_t i = 0; i < config->n_outputs; i++) {
        if (!config->outputs[i].name || strcmp(config->outputs[i].name, name) != 0) {
            continue;
        }
        output_finish(&config->outputs[i]);
        /* 后面的元素前移，保持数组连续 */
        memmove(&config->outputs[i], &config->outputs[i + 1],
                (config->n_outputs - i - 1) * sizeof(*config->outputs));
        config->n_outputs--;
        return;
    }
}

bool woodland_wallpaper_config_set_source(struct woodland_wallpaper_config *config,
                                          const char *output_name,
                                          enum woodland_wallpaper_role role,
                                          enum woodland_wallpaper_type type,
                                          const char *path) {
    struct woodland_wallpaper_output *output =
        woodland_wallpaper_config_find_output(config, output_name);
    if (!output) {
        return false;
    }

    bool changed = false;
    if ((role & WOODLAND_WALLPAPER_LOCKSCREEN) &&
        (!output->lockscreen || strcmp(output->lockscreen, path) != 0 ||
         output->lockscreen_type != (int)type)) {
        char *copy = strdup(path);
        if (copy) {
            free(output->lockscreen);
            output->lockscreen = copy;
            output->lockscreen_type = (int)type;
            changed = true;
        }
    }
    if (role & WOODLAND_WALLPAPER_DESKTOP) {
        for (size_t i = 0; i < output->n_workspaces; i++) {
            if (output->workspaces[i].desktop &&
                strcmp(output->workspaces[i].desktop, path) == 0 &&
                output->workspaces[i].desktop_type == (int)type) {
                continue;
            }
            char *copy = strdup(path);
            if (!copy) {
                continue;
            }
            free(output->workspaces[i].desktop);
            output->workspaces[i].desktop = copy;
            output->workspaces[i].desktop_type = (int)type;
            changed = true;
        }
    }
    return changed;
}

/* ==================== JSON 读写 ==================== */

/*
 * 自己写一个只认"配置形状"的解析器，而不是引 JSON 库：
 * 需求只有"把这一份数组读回来"，形状固定（数组 → 对象 → 字符串/整数/数组），
 * 一百多行比多一个依赖划算——和 util/json.c 只有写入器是同一个取舍。
 *
 * 不认识的值一律 skip_value 跳过，这样将来上游加字段（treeland 的
 * workspaceId / 颜色等）不会让旧配置读不出来。
 */
struct wp_json {
    const char *p;
};

static bool json_skip_value(struct wp_json *j);

static void json_ws(struct wp_json *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') {
        j->p++;
    }
}

static bool json_eat(struct wp_json *j, char c) {
    json_ws(j);
    if (*j->p != c) {
        return false;
    }
    j->p++;
    return true;
}

static bool json_accept_str(struct wp_json *j, const char *literal) {
    size_t n = strlen(literal);
    if (strncmp(j->p, literal, n) != 0) {
        return false;
    }
    j->p += n;
    return true;
}

static bool buf_put(char **buf, size_t *cap, size_t *len, const char *bytes, size_t n) {
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

static bool json_hex4(const char *p, unsigned *out) {
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
static bool json_parse_string(struct wp_json *j, char **out) {
    json_ws(j);
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
                if (!json_hex4(j->p, &cp)) {
                    goto fail;
                }
                j->p += 4;
                /* 非 BMP：高代理 + 低代理拼成一个码点，否则替换成 U+FFFD */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (j->p[0] == '\\' && j->p[1] == 'u' && json_hex4(j->p + 2, &lo) &&
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

        if (!buf_put(&buf, &cap, &len, bytes, n)) {
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

static bool json_parse_int(struct wp_json *j, int *out) {
    json_ws(j);
    char *end = NULL;
    long value = strtol(j->p, &end, 10);
    if (end == j->p) {
        return false;
    }
    j->p = end;
    *out = (int)value;
    return true;
}

static bool json_skip_value(struct wp_json *j) {
    json_ws(j);
    switch (*j->p) {
    case '"': {
        char *ignored = NULL;
        bool ok = json_parse_string(j, &ignored);
        free(ignored);
        return ok;
    }
    case '{':
        j->p++;
        json_ws(j);
        if (*j->p == '}') {
            j->p++;
            return true;
        }
        for (;;) {
            char *key = NULL;
            if (!json_parse_string(j, &key) || !json_eat(j, ':')) {
                free(key);
                return false;
            }
            free(key);
            if (!json_skip_value(j)) {
                return false;
            }
            json_ws(j);
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == '}') {
                j->p++;
                return true;
            }
            return false;
        }
    case '[':
        j->p++;
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        for (;;) {
            if (!json_skip_value(j)) {
                return false;
            }
            json_ws(j);
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == ']') {
                j->p++;
                return true;
            }
            return false;
        }
    case 't':
        return json_accept_str(j, "true");
    case 'f':
        return json_accept_str(j, "false");
    case 'n':
        return json_accept_str(j, "null");
    default: {
        char *end = NULL;
        (void)strtod(j->p, &end);
        if (end == j->p) {
            return false;
        }
        j->p = end;
        return true;
    }
    }
}

struct parsed_ws {
    int index;
    char *desktop;
    int type;
};

static void parsed_ws_finish(struct parsed_ws *ws, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(ws[i].desktop);
    }
    free(ws);
}

static bool json_parse_workspaces(struct wp_json *j, struct parsed_ws **out,
                                  size_t *count) {
    *out = NULL;
    *count = 0;
    if (!json_eat(j, '[')) {
        return false;
    }
    json_ws(j);
    if (*j->p == ']') {
        j->p++;
        return true;
    }

    size_t cap = 0;
    struct parsed_ws ws = {0, NULL, WOODLAND_WALLPAPER_IMAGE};
    for (;;) {
        ws = (struct parsed_ws){0, NULL, WOODLAND_WALLPAPER_IMAGE};
        if (!json_eat(j, '{')) {
            goto fail;
        }
        json_ws(j);
        if (*j->p != '}') {
            for (;;) {
                char *key = NULL;
                if (!json_parse_string(j, &key) || !json_eat(j, ':')) {
                    free(key);
                    goto fail;
                }
                bool ok;
                if (strcmp(key, "workspaceIndex") == 0) {
                    ok = json_parse_int(j, &ws.index);
                } else if (strcmp(key, "desktopWallpaper") == 0) {
                    char *value = NULL;
                    ok = json_parse_string(j, &value);
                    free(ws.desktop);
                    ws.desktop = value;
                } else if (strcmp(key, "desktopWallpapertype") == 0) {
                    ok = json_parse_int(j, &ws.type);
                } else {
                    ok = json_skip_value(j);
                }
                free(key);
                if (!ok) {
                    goto fail;
                }
                json_ws(j);
                if (*j->p == ',') {
                    j->p++;
                    continue;
                }
                if (*j->p == '}') {
                    break;
                }
                goto fail;
            }
        }
        j->p++; // '}'

        if (*count == cap) {
            size_t want = cap ? cap * 2 : 4;
            struct parsed_ws *grown = realloc(*out, want * sizeof(*grown));
            if (!grown) {
                goto fail;
            }
            *out = grown;
            cap = want;
        }
        (*out)[(*count)++] = ws;
        ws.desktop = NULL;

        json_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        goto fail;
    }

fail:
    free(ws.desktop);
    parsed_ws_finish(*out, *count);
    *out = NULL;
    *count = 0;
    return false;
}

static bool json_parse_output(struct wp_json *j,
                              struct woodland_wallpaper_config *config) {
    char *name = NULL;
    char *lockscreen = NULL;
    int lockscreen_type = WOODLAND_WALLPAPER_IMAGE;
    struct parsed_ws *workspaces = NULL;
    size_t n_workspaces = 0;

    if (!json_eat(j, '{')) {
        return false;
    }
    json_ws(j);
    if (*j->p != '}') {
        for (;;) {
            char *key = NULL;
            if (!json_parse_string(j, &key) || !json_eat(j, ':')) {
                free(key);
                goto fail;
            }
            bool ok;
            if (strcmp(key, "outputName") == 0) {
                char *value = NULL;
                ok = json_parse_string(j, &value);
                free(name);
                name = value;
            } else if (strcmp(key, "lockScreenWallpaper") == 0) {
                char *value = NULL;
                ok = json_parse_string(j, &value);
                free(lockscreen);
                lockscreen = value;
            } else if (strcmp(key, "lockScreenWallpapertype") == 0) {
                ok = json_parse_int(j, &lockscreen_type);
            } else if (strcmp(key, "workspaces") == 0) {
                parsed_ws_finish(workspaces, n_workspaces);
                ok = json_parse_workspaces(j, &workspaces, &n_workspaces);
            } else {
                ok = json_skip_value(j);
            }
            free(key);
            if (!ok) {
                goto fail;
            }
            json_ws(j);
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == '}') {
                break;
            }
            goto fail;
        }
    }
    j->p++; // '}'

    /* 没有 outputName 的项没法定位，当成配置文件损坏 */
    if (!name || name[0] == '\0') {
        goto fail;
    }

    struct woodland_wallpaper_output *output = config_append_output(config, name);
    if (!output) {
        goto fail;
    }
    output->lockscreen = dup_or_default(lockscreen);
    output->lockscreen_type = lockscreen_type;
    if (!output->lockscreen) {
        goto fail;
    }
    for (size_t i = 0; i < n_workspaces; i++) {
        workspace_append(output, workspaces[i].index, workspaces[i].desktop,
                         workspaces[i].type);
    }

    free(name);
    free(lockscreen);
    parsed_ws_finish(workspaces, n_workspaces);
    return true;

fail:
    free(name);
    free(lockscreen);
    parsed_ws_finish(workspaces, n_workspaces);
    return false;
}

static bool json_parse_config(const char *text,
                              struct woodland_wallpaper_config *config) {
    struct wp_json j = {text};
    if (!json_eat(&j, '[')) {
        return false;
    }
    json_ws(&j);
    if (*j.p == ']') {
        j.p++;
        json_ws(&j);
        return *j.p == '\0';
    }
    for (;;) {
        if (!json_parse_output(&j, config)) {
            return false;
        }
        json_ws(&j);
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p == ']') {
            j.p++;
            break;
        }
        return false;
    }
    json_ws(&j);
    return *j.p == '\0';
}

static void json_write(const struct woodland_wallpaper_config *config,
                       struct woodland_json *out) {
    struct woodland_json_arr outputs;
    woodland_json_arr_open(&outputs, out);
    for (size_t i = 0; i < config->n_outputs; i++) {
        const struct woodland_wallpaper_output *output = &config->outputs[i];
        woodland_json_arr_sep(&outputs);

        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_str(&obj, "outputName", output->name);
        woodland_json_obj_str(&obj, "lockScreenWallpaper", output->lockscreen);
        /* 键名是 treeland 的 typo，抄它才能共用配置，别"顺手修正" */
        woodland_json_obj_int(&obj, "lockScreenWallpapertype", output->lockscreen_type);
        woodland_json_obj_key(&obj, "workspaces");

        struct woodland_json_arr workspaces;
        woodland_json_arr_open(&workspaces, out);
        for (size_t k = 0; k < output->n_workspaces; k++) {
            const struct woodland_wallpaper_workspace *ws = &output->workspaces[k];
            woodland_json_arr_sep(&workspaces);
            struct woodland_json_obj ws_obj;
            woodland_json_obj_open(&ws_obj, out);
            woodland_json_obj_int(&ws_obj, "workspaceIndex", ws->index);
            woodland_json_obj_str(&ws_obj, "desktopWallpaper", ws->desktop);
            woodland_json_obj_int(&ws_obj, "desktopWallpapertype", ws->desktop_type);
            woodland_json_obj_close(&ws_obj);
        }
        woodland_json_arr_close(&workspaces);

        woodland_json_obj_close(&obj);
    }
    woodland_json_arr_close(&outputs);
}

bool woodland_wallpaper_config_load(struct woodland_wallpaper_config *config) {
    char path[600];
    if (!config_path(path, sizeof(path))) {
        wlr_log(WLR_DEBUG, "no config dir for wallpaper, using defaults");
        return false;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        wlr_log(WLR_DEBUG, "no wallpaper config at %s", path);
        return false;
    }
    char *text = calloc(1, WALLPAPER_CONFIG_MAX);
    if (!text) {
        fclose(file);
        return false;
    }
    size_t len = fread(text, 1, WALLPAPER_CONFIG_MAX - 1, file);
    fclose(file);
    text[len] = '\0';

    /* 空文件（上次写到一半被杀）按"没有配置"处理，不要因为它覆盖默认值 */
    struct woodland_wallpaper_config parsed;
    woodland_wallpaper_config_init(&parsed);
    if (len == 0 || !json_parse_config(text, &parsed)) {
        wlr_log(WLR_ERROR, "wallpaper config %s is malformed, falling back to defaults",
                path);
        woodland_wallpaper_config_finish(&parsed);
        free(text);
        return false;
    }
    free(text);

    woodland_wallpaper_config_finish(config);
    *config = parsed;
    wlr_log(WLR_INFO, "loaded wallpaper config: %zu outputs", config->n_outputs);
    return true;
}

bool woodland_wallpaper_config_save(const struct woodland_wallpaper_config *config) {
    char path[600];
    char dir[512];
    if (!config_path(path, sizeof(path)) || !config_dir(dir, sizeof(dir))) {
        return false;
    }
    if (!mkdir_parents(dir)) {
        wlr_log(WLR_ERROR, "cannot create config dir %s", dir);
        return false;
    }

    /*
     * 先写临时文件再 rename：配置只有几 KB，但写到一半被杀会留下半截 JSON，
     * 下次启动解析失败就把用户设的壁纸全丢了。rename 是原子的。
     */
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
        wlr_log(WLR_ERROR, "cannot save wallpaper config to %s", path);
        unlink(tmp);
        return false;
    }
    return true;
}

/* ==================== 源校验 ==================== */

static bool path_under(const char *root, const char *path) {
    size_t n = strlen(root);
    if (n == 0 || strncmp(path, root, n) != 0) {
        return false;
    }
    return path[n] == '/' || path[n] == '\0';
}

/*
 * 白名单根目录。
 *
 * realpath 归一之后再比，是为了让根目录自己是符号链接时也能匹配（有的发行版
 * 把 /usr/share/wallpapers 链到别处），同时也避免路径穿越
 * （`/usr/share/wallpapers/../../../etc/passwd` 归一后落在白名单外）。
 */
static size_t source_roots(char roots[][PATH_MAX], size_t max) {
    static const char *fixed[] = {
        "/usr/share/wallpapers",
        "/var/cache/wallpapers",
        "/usr/share/backgrounds",
    };
    size_t n = 0;
    char buf[PATH_MAX];
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && n < max; i++) {
        if (realpath(fixed[i], buf)) {
            snprintf(roots[n++], PATH_MAX, "%s", buf);
        }
    }

    /* 用户自己的壁纸目录：dde 允许把自选图片放进这里 */
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (data_home && data_home[0] == '/') {
        snprintf(buf, sizeof(buf), "%s/wallpapers", data_home);
    } else if (home && home[0] == '/') {
        snprintf(buf, sizeof(buf), "%s/.local/share/wallpapers", home);
    } else {
        buf[0] = '\0';
    }
    if (buf[0] && n < max && realpath(buf, roots[n])) {
        n++;
    }
    return n;
}

enum woodland_wallpaper_source woodland_wallpaper_source_check(const char *path) {
    if (!path || path[0] != '/') {
        return WOODLAND_WALLPAPER_SOURCE_INVALID;
    }

    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        /* 打不开是权限问题；不存在 / 路径本身有问题都算非法源 */
        return errno == EACCES ? WOODLAND_WALLPAPER_SOURCE_PERMISSION
                               : WOODLAND_WALLPAPER_SOURCE_INVALID;
    }
    if (access(resolved, R_OK) != 0) {
        return errno == EACCES ? WOODLAND_WALLPAPER_SOURCE_PERMISSION
                               : WOODLAND_WALLPAPER_SOURCE_INVALID;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        return WOODLAND_WALLPAPER_SOURCE_INVALID;
    }

    char roots[6][PATH_MAX];
    size_t n = source_roots(roots, 6);
    for (size_t i = 0; i < n; i++) {
        if (path_under(roots[i], resolved)) {
            return WOODLAND_WALLPAPER_SOURCE_OK;
        }
    }
    wlr_log(WLR_INFO, "wallpaper source %s is outside the allowed roots", resolved);
    return WOODLAND_WALLPAPER_SOURCE_INVALID;
}
