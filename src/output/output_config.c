// SPDX-License-Identifier: GPL-2.0-only
#include "output_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "output.h"
#include "util/json.h"
#include "woodland.h"

#define OUTPUTS_CONFIG_MAX (1024u * 1024u)

/* ==================== 路径与目录工具 ==================== */

static bool config_dir(char *buf, size_t size)
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

static bool config_path(char *buf, size_t size)
{
    char dir[512];
    if (!config_dir(dir, sizeof(dir))) {
        return false;
    }
    return snprintf(buf, size, "%s/outputs.json", dir) < (int)size;
}

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
    return true;
}

/* ==================== 设备标识工具 ==================== */

static bool get_output_identifier(const struct wlr_output *wlr, char *buf, size_t size)
{
    if (wlr && wlr->model && wlr->model[0] && wlr->serial && wlr->serial[0]) {
        snprintf(buf, size, "%s:%s", wlr->model, wlr->serial);
        return true;
    }
    return false;
}

/* ==================== JSON 极简流式解析器 ==================== */

struct json_cursor {
    const char *p;
};

static void json_ws(struct json_cursor *cur)
{
    while (*cur->p == ' ' || *cur->p == '\t' || *cur->p == '\n' || *cur->p == '\r') {
        cur->p++;
    }
}

static bool json_eat(struct json_cursor *cur, char c)
{
    json_ws(cur);
    if (*cur->p != c) {
        return false;
    }
    cur->p++;
    return true;
}

static bool json_parse_string(struct json_cursor *cur, char **out)
{
    json_ws(cur);
    if (*cur->p != '"') {
        return false;
    }
    cur->p++;

    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return false;
    }
    buf[0] = '\0';

    while (*cur->p && *cur->p != '"') {
        char ch = *cur->p++;
        if (ch == '\\') {
            if (!*cur->p) {
                free(buf);
                return false;
            }
            char esc = *cur->p++;
            switch (esc) {
            case '"':  ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            default:   ch = esc; break;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                return false;
            }
            buf = grown;
        }
        buf[len++] = ch;
        buf[len] = '\0';
    }

    if (*cur->p != '"') {
        free(buf);
        return false;
    }
    cur->p++;
    *out = buf;
    return true;
}

static bool json_parse_int(struct json_cursor *cur, int32_t *out)
{
    json_ws(cur);
    char *end = NULL;
    long val = strtol(cur->p, &end, 10);
    if (end == cur->p) {
        return false;
    }
    cur->p = end;
    *out = (int32_t)val;
    return true;
}

static bool json_parse_double(struct json_cursor *cur, double *out)
{
    json_ws(cur);
    char *end = NULL;
    double val = strtod(cur->p, &end);
    if (end == cur->p) {
        return false;
    }
    cur->p = end;
    *out = val;
    return true;
}

static bool json_parse_bool(struct json_cursor *cur, bool *out)
{
    json_ws(cur);
    if (strncmp(cur->p, "true", 4) == 0) {
        cur->p += 4;
        *out = true;
        return true;
    }
    if (strncmp(cur->p, "false", 5) == 0) {
        cur->p += 5;
        *out = false;
        return true;
    }
    return false;
}

static bool json_skip_value(struct json_cursor *cur)
{
    json_ws(cur);
    switch (*cur->p) {
    case '"': {
        char *dummy = NULL;
        bool ok = json_parse_string(cur, &dummy);
        free(dummy);
        return ok;
    }
    case '{': {
        cur->p++;
        json_ws(cur);
        if (*cur->p == '}') {
            cur->p++;
            return true;
        }
        for (;;) {
            char *key = NULL;
            if (!json_parse_string(cur, &key) || !json_eat(cur, ':')) {
                free(key);
                return false;
            }
            free(key);
            if (!json_skip_value(cur)) {
                return false;
            }
            json_ws(cur);
            if (*cur->p == ',') {
                cur->p++;
                continue;
            }
            if (*cur->p == '}') {
                cur->p++;
                return true;
            }
            return false;
        }
    }
    case '[': {
        cur->p++;
        json_ws(cur);
        if (*cur->p == ']') {
            cur->p++;
            return true;
        }
        for (;;) {
            if (!json_skip_value(cur)) {
                return false;
            }
            json_ws(cur);
            if (*cur->p == ',') {
                cur->p++;
                continue;
            }
            if (*cur->p == ']') {
                cur->p++;
                return true;
            }
            return false;
        }
    }
    default: {
        while (*cur->p && *cur->p != ',' && *cur->p != '}' && *cur->p != ']' &&
               *cur->p != ' ' && *cur->p != '\t' && *cur->p != '\n' && *cur->p != '\r') {
            cur->p++;
        }
        return true;
    }
    }
}

/* ==================== 配置读取与解析 ==================== */

void woodland_saved_outputs_finish(struct woodland_saved_outputs *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].output_id);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

const struct woodland_saved_output *
woodland_saved_outputs_find_by_name(const struct woodland_saved_outputs *list, const char *name)
{
    if (!list || !name) {
        return NULL;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].name && strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

const struct woodland_saved_output *
woodland_saved_outputs_find(const struct woodland_saved_outputs *list,
                            const struct wlr_output *wlr)
{
    if (!list || !wlr) {
        return NULL;
    }
    char id_buf[128];
    if (get_output_identifier(wlr, id_buf, sizeof(id_buf))) {
        for (size_t i = 0; i < list->count; i++) {
            if (list->items[i].output_id && strcmp(list->items[i].output_id, id_buf) == 0) {
                return &list->items[i];
            }
        }
    }
    return woodland_saved_outputs_find_by_name(list, wlr->name);
}

static bool parse_output_object(struct json_cursor *cur, struct woodland_saved_output *item)
{
    if (!json_eat(cur, '{')) {
        return false;
    }
    memset(item, 0, sizeof(*item));
    item->enabled = true;
    item->scale = 1.0;

    json_ws(cur);
    if (*cur->p == '}') {
        cur->p++;
        return item->name != NULL;
    }

    for (;;) {
        char *key = NULL;
        if (!json_parse_string(cur, &key) || !json_eat(cur, ':')) {
            free(key);
            return false;
        }

        bool ok = true;
        if (strcmp(key, "name") == 0) {
            free(item->name);
            ok = json_parse_string(cur, &item->name);
        } else if (strcmp(key, "id") == 0) {
            free(item->output_id);
            ok = json_parse_string(cur, &item->output_id);
        } else if (strcmp(key, "enabled") == 0) {
            ok = json_parse_bool(cur, &item->enabled);
        } else if (strcmp(key, "x") == 0) {
            ok = json_parse_int(cur, &item->x);
        } else if (strcmp(key, "y") == 0) {
            ok = json_parse_int(cur, &item->y);
        } else if (strcmp(key, "width") == 0) {
            ok = json_parse_int(cur, &item->width);
        } else if (strcmp(key, "height") == 0) {
            ok = json_parse_int(cur, &item->height);
        } else if (strcmp(key, "refresh") == 0) {
            ok = json_parse_int(cur, &item->refresh);
        } else if (strcmp(key, "scale") == 0) {
            ok = json_parse_double(cur, &item->scale);
        } else if (strcmp(key, "transform") == 0) {
            int32_t tf = 0;
            ok = json_parse_int(cur, &tf);
            item->transform = (enum wl_output_transform)tf;
        } else if (strcmp(key, "adaptive_sync") == 0) {
            ok = json_parse_bool(cur, &item->adaptive_sync);
        } else {
            ok = json_skip_value(cur);
        }
        free(key);
        if (!ok) {
            return false;
        }

        json_ws(cur);
        if (*cur->p == ',') {
            cur->p++;
            continue;
        }
        if (*cur->p == '}') {
            cur->p++;
            break;
        }
        return false;
    }

    return item->name != NULL;
}

bool woodland_saved_outputs_load(struct woodland_saved_outputs *list)
{
    if (!list) {
        return false;
    }
    memset(list, 0, sizeof(*list));

    char path[512];
    if (!config_path(path, sizeof(path))) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || (unsigned long)sz > OUTPUTS_CONFIG_MAX) {
        fclose(f);
        return false;
    }

    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    struct json_cursor cur = {.p = buf};
    if (!json_eat(&cur, '[')) {
        free(buf);
        return false;
    }

    size_t cap = 4;
    list->items = calloc(cap, sizeof(struct woodland_saved_output));
    if (!list->items) {
        free(buf);
        return false;
    }

    json_ws(&cur);
    if (*cur.p != ']') {
        for (;;) {
            struct woodland_saved_output item = {0};
            if (!parse_output_object(&cur, &item)) {
                free(item.name);
                free(item.output_id);
                break;
            }

            if (list->count >= cap) {
                cap *= 2;
                struct woodland_saved_output *grown =
                    realloc(list->items, cap * sizeof(struct woodland_saved_output));
                if (!grown) {
                    free(item.name);
                    free(item.output_id);
                    break;
                }
                list->items = grown;
            }
            list->items[list->count++] = item;

            json_ws(&cur);
            if (*cur.p == ',') {
                cur.p++;
                continue;
            }
            if (*cur.p == ']') {
                break;
            }
            break;
        }
    }

    free(buf);
    return true;
}

/* ==================== 持久化写出与合并 ==================== */

static void upsert_saved_output(struct woodland_saved_outputs *list,
                                const struct woodland_saved_output *entry,
                                bool preserve_position)
{
    for (size_t i = 0; i < list->count; i++) {
        bool match = false;
        if (entry->output_id && list->items[i].output_id &&
            strcmp(list->items[i].output_id, entry->output_id) == 0) {
            match = true;
        } else if (entry->name && list->items[i].name &&
                   strcmp(list->items[i].name, entry->name) == 0) {
            match = true;
        }

        if (match) {
            int32_t old_x = list->items[i].x;
            int32_t old_y = list->items[i].y;
            free(list->items[i].name);
            free(list->items[i].output_id);
            list->items[i] = *entry;
            list->items[i].name = strdup(entry->name ? entry->name : "");
            list->items[i].output_id = entry->output_id ? strdup(entry->output_id) : NULL;
            if (preserve_position) {
                list->items[i].x = old_x;
                list->items[i].y = old_y;
            }
            return;
        }
    }

    struct woodland_saved_output *grown =
        realloc(list->items, (list->count + 1) * sizeof(struct woodland_saved_output));
    if (!grown) {
        return;
    }
    list->items = grown;
    list->items[list->count] = *entry;
    list->items[list->count].name = strdup(entry->name ? entry->name : "");
    list->items[list->count].output_id = entry->output_id ? strdup(entry->output_id) : NULL;
    list->count++;
}

void woodland_output_config_save(struct woodland_server *server)
{
    if (!server || !server->output_layout || !server->outputs.next) {
        return;
    }

    /* 统计当前启用的输出数量，若仅有 1 块屏幕在线，保护原有多屏相对坐标不被覆盖 */
    size_t enabled_count = 0;
    struct woodland_output *it;
    wl_list_for_each(it, &server->outputs, link) {
        if (it->wlr_output && it->wlr_output->enabled) {
            enabled_count++;
        }
    }
    bool preserve_position = (enabled_count == 1);

    /* 1. 先读现有 outputs.json，以支持多屏配置合并更新（不丢离线屏） */
    struct woodland_saved_outputs list = {0};
    woodland_saved_outputs_load(&list);

    /* 2. 遍历当前在线的各输出，更新入 list */
    struct woodland_output *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output *wlr = out->wlr_output;
        if (!wlr || !wlr->name) {
            continue;
        }

        struct woodland_saved_output entry = {0};
        entry.name = wlr->name;
        char id_buf[128];
        if (get_output_identifier(wlr, id_buf, sizeof(id_buf))) {
            entry.output_id = id_buf;
        }
        entry.enabled = wlr->enabled;

        struct wlr_box box = {0};
        wlr_output_layout_get_box(server->output_layout, wlr, &box);
        entry.x = box.x;
        entry.y = box.y;

        if (wlr->current_mode) {
            entry.width = wlr->current_mode->width;
            entry.height = wlr->current_mode->height;
            entry.refresh = wlr->current_mode->refresh;
        } else {
            entry.width = wlr->width;
            entry.height = wlr->height;
            entry.refresh = wlr->refresh;
        }

        entry.scale = wlr->scale;
        entry.transform = wlr->transform;
        entry.adaptive_sync = (wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);

        upsert_saved_output(&list, &entry, preserve_position);
    }

    /* 3. 使用 util/json.h 序列化 */
    struct woodland_json json;
    woodland_json_init(&json);
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, &json);

    for (size_t i = 0; i < list.count; i++) {
        woodland_json_arr_sep(&arr);
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, &json);
        woodland_json_obj_str(&obj, "name", list.items[i].name);
        if (list.items[i].output_id) {
            woodland_json_obj_str(&obj, "id", list.items[i].output_id);
        }
        woodland_json_obj_bool(&obj, "enabled", list.items[i].enabled);
        woodland_json_obj_int(&obj, "x", list.items[i].x);
        woodland_json_obj_int(&obj, "y", list.items[i].y);
        woodland_json_obj_int(&obj, "width", list.items[i].width);
        woodland_json_obj_int(&obj, "height", list.items[i].height);
        woodland_json_obj_int(&obj, "refresh", list.items[i].refresh);

        woodland_json_obj_key(&obj, "scale");
        woodland_json_fmt(&json, "%.3f", list.items[i].scale);

        woodland_json_obj_int(&obj, "transform", (int)list.items[i].transform);
        woodland_json_obj_bool(&obj, "adaptive_sync", list.items[i].adaptive_sync);
        woodland_json_obj_close(&obj);
    }
    woodland_json_arr_close(&arr);
    woodland_json_raw(&json, "\n");

    /* 4. 原子落盘：写临时文件再 rename */
    char path[512];
    if (config_path(path, sizeof(path)) && mkdir_parents(path)) {
        char tmp_path[540];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        FILE *tf = fopen(tmp_path, "wb");
        if (tf) {
            const char *data = woodland_json_data(&json);
            size_t len = woodland_json_len(&json);
            if (fwrite(data, 1, len, tf) == len) {
                fflush(tf);
                fclose(tf);
                if (rename(tmp_path, path) == 0) {
                    wlr_log(WLR_INFO, "output configuration persisted to %s (%zu outputs)",
                            path, list.count);
                } else {
                    wlr_log(WLR_ERROR, "failed to rename %s to %s: %s", tmp_path, path,
                            strerror(errno));
                    unlink(tmp_path);
                }
            } else {
                fclose(tf);
                unlink(tmp_path);
            }
        }
    }

    woodland_json_finish(&json);
    woodland_saved_outputs_finish(&list);
}

/* ==================== 启动与热插拔配置恢复 ==================== */

bool woodland_output_config_restore(struct woodland_output *output,
                                    struct wlr_output_state *state,
                                    int32_t *out_x, int32_t *out_y)
{
    if (!output || !output->wlr_output || !state) {
        return false;
    }

    struct woodland_saved_outputs list = {0};
    if (!woodland_saved_outputs_load(&list)) {
        return false;
    }

    const struct woodland_saved_output *saved =
        woodland_saved_outputs_find(&list, output->wlr_output);
    if (!saved) {
        woodland_saved_outputs_finish(&list);
        return false;
    }

    wlr_output_state_set_enabled(state, saved->enabled);

    /* 匹配分辨率与刷新率 */
    if (saved->width > 0 && saved->height > 0) {
        struct wlr_output_mode *best_mode = NULL;
        int best_diff = INT_MAX;
        struct wlr_output_mode *m;
        wl_list_for_each(m, &output->wlr_output->modes, link) {
            if (m->width == saved->width && m->height == saved->height) {
                int diff = abs(m->refresh - saved->refresh);
                if (diff < best_diff) {
                    best_diff = diff;
                    best_mode = m;
                }
            }
        }

        if (best_mode) {
            wlr_output_state_set_mode(state, best_mode);
        } else if (wl_list_empty(&output->wlr_output->modes)) {
            /* headless / 虚拟屏幕等无预置 mode 的情况 */
            wlr_output_state_set_custom_mode(state, saved->width, saved->height, saved->refresh);
        } else {
            /* 预置 modes 里没找到该分辨率，回退 preferred */
            struct wlr_output_mode *pref = wlr_output_preferred_mode(output->wlr_output);
            if (pref) {
                wlr_output_state_set_mode(state, pref);
            }
        }
    }

    if (saved->scale > 0.0) {
        wlr_output_state_set_scale(state, saved->scale);
    }
    wlr_output_state_set_transform(state, saved->transform);
    wlr_output_state_set_adaptive_sync_enabled(state, saved->adaptive_sync);

    if (out_x) {
        *out_x = saved->x;
    }
    if (out_y) {
        *out_y = saved->y;
    }

    wlr_log(WLR_INFO, "output %s (id: %s): restored configuration %dx%d@%dmHz, scale %.2f at (%d,%d)",
            output->wlr_output->name, saved->output_id ? saved->output_id : "none",
            saved->width, saved->height, saved->refresh,
            saved->scale, saved->x, saved->y);

    woodland_saved_outputs_finish(&list);
    return true;
}
