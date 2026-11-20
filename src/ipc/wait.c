// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scene.h"
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>

#include "ipc_internal.h"
#include "multitask_view.h"
#include "output.h"
#include "placement.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "window_menu.h"
#include "window_switcher.h"
#include "woodland.h"
#include "workspace.h"

#define WAIT_DEFAULT_TIMEOUT_MS 5000
#define WAIT_MAX_TIMEOUT_MS 60000
#define WAIT_STR_MAX 128

enum wait_kind {
    WAIT_VIEW_MAPPED = 0,
    WAIT_VIEW_UNMAPPED,
    WAIT_VIEW_COUNT,
    WAIT_FOCUS,
    WAIT_WORKSPACE,
    WAIT_MULTITASK,
    WAIT_WINDOW_MENU,
    WAIT_WINDOW_SWITCHER,
    WAIT_SNAP_PREVIEW,
    WAIT_OUTPUT_COUNT,
    WAIT_PIXEL,
    WAIT_TRACE,
};

struct wait_view_filter {
    bool has_id;
    long id;
    bool has_title;
    char title[WAIT_STR_MAX];
    bool has_app_id;
    char app_id[WAIT_STR_MAX];
    bool has_kind;
    enum woodland_view_type kind;
    bool has_workspace;
    char workspace[32];
    bool has_activated;
    bool activated;
    bool has_focused;
    bool focused;
    bool has_maximized;
    bool maximized;
    bool has_fullscreen;
    bool fullscreen;
    bool has_minimized;
    bool minimized;
    bool has_ssd;
    bool ssd;
};

struct woodland_ipc_wait {
    enum wait_kind kind;
    const char *cond_name;
    int timeout_ms;
    uint64_t start_ms;

    struct wait_view_filter view;

    bool has_count;
    int count;
    bool has_min;
    int min;
    bool has_max;
    int max;
    bool mapped_only;

    bool has_layer;
    char layer[64];
    bool has_none;
    bool none;

    bool has_name;
    char name[64];

    bool has_active;
    bool active;
    bool has_card_count;
    int card_count;
    bool has_thumb_count;
    int thumb_count;

    bool has_x;
    int x;
    bool has_y;
    int y;
    bool has_hex;
    uint32_t hex_rgb;
    int tolerance;

    bool has_what;
    char what[32];
    bool has_detail;
    char detail[WAIT_STR_MAX];
    bool has_obj;
    uint32_t obj;
};

/* ==================== 共用解析助手 ==================== */

const char *woodland_ipc_next_token(const char *p, char *tok, size_t cap)
{
    if (!p || !tok || cap == 0) {
        return NULL;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return NULL;
    }

    size_t n = 0;
    bool in_quotes = false;
    while (*p != '\0') {
        if (!in_quotes && (*p == ' ' || *p == '\t')) {
            break;
        }
        if (*p == '"') {
            in_quotes = !in_quotes;
            p++;
            continue;
        }
        if (in_quotes && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
            if (n + 1 < cap) {
                tok[n++] = p[1];
            }
            p += 2;
            continue;
        }
        if (n + 1 < cap) {
            tok[n++] = *p;
        }
        p++;
    }
    tok[n] = '\0';
    return p;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool woodland_ipc_parse_hex_rgb(const char *s, uint32_t *rgb)
{
    if (!s || !rgb) {
        return false;
    }
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6) {
        return false;
    }
    uint32_t val = 0;
    for (int i = 0; i < 6; i++) {
        int d = hex_val(s[i]);
        if (d < 0) {
            return false;
        }
        val = (val << 4) | (uint32_t)d;
    }
    *rgb = val;
    return true;
}

static bool parse_bool_str(const char *s, bool *out)
{
    if (!s) {
        return false;
    }
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int_str(const char *s, int *out)
{
    if (!s || !s[0]) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool parse_u32_str(const char *s, uint32_t *out)
{
    if (!s || !s[0] || s[0] == '-') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

uint64_t woodland_ipc_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ==================== 条件解析 ==================== */

static bool parse_view_kv(struct wait_view_filter *f, const char *k, const char *v,
                          bool allow_full_attrs, char *err, size_t err_cap)
{
    if (strcmp(k, "id") == 0) {
        int iv = 0;
        if (!parse_int_str(v, &iv) || iv <= 0) {
            snprintf(err, err_cap, "invalid view id");
            return false;
        }
        f->has_id = true;
        f->id = iv;
        return true;
    }
    if (strcmp(k, "title") == 0) {
        snprintf(f->title, sizeof(f->title), "%s", v);
        f->has_title = true;
        return true;
    }
    if (strcmp(k, "app_id") == 0) {
        snprintf(f->app_id, sizeof(f->app_id), "%s", v);
        f->has_app_id = true;
        return true;
    }
    if (strcmp(k, "kind") == 0) {
        if (strcmp(v, "xdg") == 0) {
            f->kind = WOODLAND_VIEW_XDG;
        } else if (strcmp(v, "x11") == 0) {
            f->kind = WOODLAND_VIEW_XWAYLAND;
        } else {
            snprintf(err, err_cap, "kind must be xdg or x11");
            return false;
        }
        f->has_kind = true;
        return true;
    }
    if (!allow_full_attrs) {
        snprintf(err, err_cap, "unknown parameter '%s'", k);
        return false;
    }
    if (strcmp(k, "workspace") == 0) {
        snprintf(f->workspace, sizeof(f->workspace), "%s", v);
        f->has_workspace = true;
        return true;
    }
    if (strcmp(k, "activated") == 0) {
        if (!parse_bool_str(v, &f->activated)) {
            snprintf(err, err_cap, "activated must be 0|1|true|false");
            return false;
        }
        f->has_activated = true;
        return true;
    }
    if (strcmp(k, "focused") == 0) {
        if (!parse_bool_str(v, &f->focused)) {
            snprintf(err, err_cap, "focused must be 0|1|true|false");
            return false;
        }
        f->has_focused = true;
        return true;
    }
    if (strcmp(k, "maximized") == 0) {
        if (!parse_bool_str(v, &f->maximized)) {
            snprintf(err, err_cap, "maximized must be 0|1|true|false");
            return false;
        }
        f->has_maximized = true;
        return true;
    }
    if (strcmp(k, "fullscreen") == 0) {
        if (!parse_bool_str(v, &f->fullscreen)) {
            snprintf(err, err_cap, "fullscreen must be 0|1|true|false");
            return false;
        }
        f->has_fullscreen = true;
        return true;
    }
    if (strcmp(k, "minimized") == 0) {
        if (!parse_bool_str(v, &f->minimized)) {
            snprintf(err, err_cap, "minimized must be 0|1|true|false");
            return false;
        }
        f->has_minimized = true;
        return true;
    }
    if (strcmp(k, "ssd") == 0) {
        if (!parse_bool_str(v, &f->ssd)) {
            snprintf(err, err_cap, "ssd must be 0|1|true|false");
            return false;
        }
        f->has_ssd = true;
        return true;
    }
    snprintf(err, err_cap, "unknown parameter '%s'", k);
    return false;
}

static bool parse_condition_name(const char *name, enum wait_kind *out_kind,
                                 const char **out_canonical)
{
    static const struct {
        const char *name;
        enum wait_kind kind;
    } table[] = {
        {"view_mapped", WAIT_VIEW_MAPPED},
        {"view_unmapped", WAIT_VIEW_UNMAPPED},
        {"view_count", WAIT_VIEW_COUNT},
        {"focus", WAIT_FOCUS},
        {"workspace", WAIT_WORKSPACE},
        {"multitask", WAIT_MULTITASK},
        {"window_menu", WAIT_WINDOW_MENU},
        {"window_switcher", WAIT_WINDOW_SWITCHER},
        {"snap_preview", WAIT_SNAP_PREVIEW},
        {"output_count", WAIT_OUTPUT_COUNT},
        {"pixel", WAIT_PIXEL},
        {"trace", WAIT_TRACE},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out_kind = table[i].kind;
            *out_canonical = table[i].name;
            return true;
        }
    }
    return false;
}

bool woodland_ipc_wait_parse(const char *args, struct woodland_ipc_wait **out_wait,
                             char *err, size_t err_cap)
{
    *out_wait = NULL;
    char cond_tok[64];
    const char *p = woodland_ipc_next_token(args, cond_tok, sizeof(cond_tok));
    if (!p) {
        snprintf(err, err_cap, "usage: wait <condition> [key=val...] [timeout=MS]");
        return false;
    }

    enum wait_kind kind = WAIT_VIEW_MAPPED;
    const char *canonical = NULL;
    if (!parse_condition_name(cond_tok, &kind, &canonical)) {
        snprintf(err, err_cap, "unknown wait condition '%s'", cond_tok);
        return false;
    }

    struct woodland_ipc_wait *w = calloc(1, sizeof(*w));
    if (!w) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    w->kind = kind;
    w->cond_name = canonical;
    w->timeout_ms = WAIT_DEFAULT_TIMEOUT_MS;
    w->tolerance = 2;
    w->start_ms = woodland_ipc_monotonic_ms();

    char tok[192];
    while ((p = woodland_ipc_next_token(p, tok, sizeof(tok))) != NULL) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
            snprintf(err, err_cap, "expected key=value parameter, got '%s'", tok);
            free(w);
            return false;
        }
        *eq = '\0';
        const char *k = tok;
        const char *v = eq + 1;

        if (strcmp(k, "timeout") == 0) {
            int t = 0;
            if (!parse_int_str(v, &t) || t < 1 || t > WAIT_MAX_TIMEOUT_MS) {
                snprintf(err, err_cap, "timeout must be 1..%d ms", WAIT_MAX_TIMEOUT_MS);
                free(w);
                return false;
            }
            w->timeout_ms = t;
            continue;
        }

        switch (w->kind) {
        case WAIT_VIEW_MAPPED:
            if (!parse_view_kv(&w->view, k, v, true, err, err_cap)) {
                free(w);
                return false;
            }
            break;
        case WAIT_VIEW_UNMAPPED:
            if (!parse_view_kv(&w->view, k, v, false, err, err_cap)) {
                free(w);
                return false;
            }
            break;
        case WAIT_VIEW_COUNT:
            if (strcmp(k, "count") == 0) {
                if (!parse_int_str(v, &w->count) || w->count < 0) {
                    snprintf(err, err_cap, "count must be >= 0");
                    free(w);
                    return false;
                }
                w->has_count = true;
            } else if (strcmp(k, "min") == 0) {
                if (!parse_int_str(v, &w->min) || w->min < 0) {
                    snprintf(err, err_cap, "min must be >= 0");
                    free(w);
                    return false;
                }
                w->has_min = true;
            } else if (strcmp(k, "max") == 0) {
                if (!parse_int_str(v, &w->max) || w->max < 0) {
                    snprintf(err, err_cap, "max must be >= 0");
                    free(w);
                    return false;
                }
                w->has_max = true;
            } else if (strcmp(k, "mapped") == 0) {
                if (!parse_bool_str(v, &w->mapped_only)) {
                    snprintf(err, err_cap, "mapped must be 0|1|true|false");
                    free(w);
                    return false;
                }
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_FOCUS:
            if (strcmp(k, "view") == 0) {
                int vid = 0;
                if (!parse_int_str(v, &vid) || vid <= 0) {
                    snprintf(err, err_cap, "invalid view id");
                    free(w);
                    return false;
                }
                w->view.has_id = true;
                w->view.id = vid;
            } else if (strcmp(k, "title") == 0) {
                snprintf(w->view.title, sizeof(w->view.title), "%s", v);
                w->view.has_title = true;
            } else if (strcmp(k, "app_id") == 0) {
                snprintf(w->view.app_id, sizeof(w->view.app_id), "%s", v);
                w->view.has_app_id = true;
            } else if (strcmp(k, "layer") == 0) {
                snprintf(w->layer, sizeof(w->layer), "%s", v);
                w->has_layer = true;
            } else if (strcmp(k, "none") == 0) {
                if (!parse_bool_str(v, &w->none)) {
                    snprintf(err, err_cap, "none must be 0|1|true|false");
                    free(w);
                    return false;
                }
                w->has_none = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_WORKSPACE:
            if (strcmp(k, "name") == 0) {
                snprintf(w->name, sizeof(w->name), "%s", v);
                w->has_name = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_MULTITASK:
            if (strcmp(k, "active") == 0) {
                if (!parse_bool_str(v, &w->active)) {
                    snprintf(err, err_cap, "active must be 0|1|true|false");
                    free(w);
                    return false;
                }
                w->has_active = true;
            } else if (strcmp(k, "card_count") == 0) {
                if (!parse_int_str(v, &w->card_count) || w->card_count < 0) {
                    snprintf(err, err_cap, "card_count must be >= 0");
                    free(w);
                    return false;
                }
                w->has_card_count = true;
            } else if (strcmp(k, "thumb_count") == 0) {
                if (!parse_int_str(v, &w->thumb_count) || w->thumb_count < 0) {
                    snprintf(err, err_cap, "thumb_count must be >= 0");
                    free(w);
                    return false;
                }
                w->has_thumb_count = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_WINDOW_MENU:
        case WAIT_WINDOW_SWITCHER:
        case WAIT_SNAP_PREVIEW:
            if (strcmp(k, "active") == 0) {
                if (!parse_bool_str(v, &w->active)) {
                    snprintf(err, err_cap, "active must be 0|1|true|false");
                    free(w);
                    return false;
                }
                w->has_active = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_OUTPUT_COUNT:
            if (strcmp(k, "count") == 0) {
                if (!parse_int_str(v, &w->count) || w->count < 0) {
                    snprintf(err, err_cap, "count must be >= 0");
                    free(w);
                    return false;
                }
                w->has_count = true;
            } else if (strcmp(k, "min") == 0) {
                if (!parse_int_str(v, &w->min) || w->min < 0) {
                    snprintf(err, err_cap, "min must be >= 0");
                    free(w);
                    return false;
                }
                w->has_min = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_PIXEL:
            if (strcmp(k, "x") == 0) {
                if (!parse_int_str(v, &w->x)) {
                    snprintf(err, err_cap, "invalid x coordinate");
                    free(w);
                    return false;
                }
                w->has_x = true;
            } else if (strcmp(k, "y") == 0) {
                if (!parse_int_str(v, &w->y)) {
                    snprintf(err, err_cap, "invalid y coordinate");
                    free(w);
                    return false;
                }
                w->has_y = true;
            } else if (strcmp(k, "hex") == 0) {
                if (!woodland_ipc_parse_hex_rgb(v, &w->hex_rgb)) {
                    snprintf(err, err_cap, "invalid hex color (want #rrggbb)");
                    free(w);
                    return false;
                }
                w->has_hex = true;
            } else if (strcmp(k, "tolerance") == 0) {
                if (!parse_int_str(v, &w->tolerance) || w->tolerance < 0 || w->tolerance > 255) {
                    snprintf(err, err_cap, "tolerance must be 0..255");
                    free(w);
                    return false;
                }
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        case WAIT_TRACE:
            if (strcmp(k, "what") == 0) {
                snprintf(w->what, sizeof(w->what), "%s", v);
                w->has_what = true;
            } else if (strcmp(k, "detail") == 0) {
                snprintf(w->detail, sizeof(w->detail), "%s", v);
                w->has_detail = true;
            } else if (strcmp(k, "obj") == 0) {
                if (!parse_u32_str(v, &w->obj)) {
                    snprintf(err, err_cap, "obj must be an unsigned integer");
                    free(w);
                    return false;
                }
                w->has_obj = true;
            } else {
                snprintf(err, err_cap, "unknown parameter '%s'", k);
                free(w);
                return false;
            }
            break;
        }
    }

    /* 必填参数校验 */
    switch (w->kind) {
    case WAIT_VIEW_COUNT:
        if (!w->has_count && !w->has_min && !w->has_max) {
            snprintf(err, err_cap, "view_count requires count, min, or max");
            free(w);
            return false;
        }
        if (w->has_min && w->has_max && w->min > w->max) {
            snprintf(err, err_cap, "min cannot exceed max");
            free(w);
            return false;
        }
        break;
    case WAIT_FOCUS:
        if (w->has_none && w->none &&
            (w->view.has_id || w->view.has_title || w->view.has_app_id || w->has_layer)) {
            snprintf(err, err_cap, "focus none=1 cannot be combined with target filters");
            free(w);
            return false;
        }
        break;
    case WAIT_WORKSPACE:
        if (!w->has_name || !w->name[0]) {
            snprintf(err, err_cap, "workspace requires name=<workspace>");
            free(w);
            return false;
        }
        break;
    case WAIT_MULTITASK:
    case WAIT_WINDOW_MENU:
    case WAIT_WINDOW_SWITCHER:
    case WAIT_SNAP_PREVIEW:
        if (!w->has_active) {
            snprintf(err, err_cap, "%s requires active=0|1", w->cond_name);
            free(w);
            return false;
        }
        break;
    case WAIT_OUTPUT_COUNT:
        if (!w->has_count && !w->has_min) {
            snprintf(err, err_cap, "output_count requires count or min");
            free(w);
            return false;
        }
        break;
    case WAIT_PIXEL:
        if (!w->has_x || !w->has_y || !w->has_hex) {
            snprintf(err, err_cap, "pixel requires x=N y=N hex=#rrggbb");
            free(w);
            return false;
        }
        break;
    case WAIT_TRACE:
        if (!w->has_what || !w->what[0]) {
            snprintf(err, err_cap, "trace requires what=<tag>");
            free(w);
            return false;
        }
        break;
    default:
        break;
    }

    *out_wait = w;
    return true;
}

bool woodland_ipc_wait_validate_runtime(const struct woodland_server *server,
                                        const struct woodland_ipc_wait *wait,
                                        char *err, size_t err_cap)
{
    if (!wait) {
        snprintf(err, err_cap, "invalid wait state");
        return false;
    }
    switch (wait->kind) {
    case WAIT_TRACE:
        if (!woodland_trace_enabled()) {
            snprintf(err, err_cap, "trace is disabled (start with --trace)");
            return false;
        }
        break;
    case WAIT_PIXEL:
        if (!server || !server->scene || !server->renderer || !server->output_layout) {
            snprintf(err, err_cap, "renderer or output layout unavailable");
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

void woodland_ipc_wait_free(struct woodland_ipc_wait *wait)
{
    free(wait);
}

int woodland_ipc_wait_timeout_ms(const struct woodland_ipc_wait *wait)
{
    return wait ? wait->timeout_ms : WAIT_DEFAULT_TIMEOUT_MS;
}

uint64_t woodland_ipc_wait_start_ms(const struct woodland_ipc_wait *wait)
{
    return wait ? wait->start_ms : 0;
}

/* ==================== 条件求值 ==================== */

static bool view_matches_filter(const struct woodland_server *server,
                                const struct woodland_view *view,
                                const struct wait_view_filter *f)
{
    if (f->has_id && (long)view->id != f->id) {
        return false;
    }
    if (f->has_title && (!view->title || strcmp(view->title, f->title) != 0)) {
        return false;
    }
    if (f->has_app_id && (!view->app_id || strcmp(view->app_id, f->app_id) != 0)) {
        return false;
    }
    if (f->has_kind && view->type != f->kind) {
        return false;
    }
    if (f->has_workspace &&
        (!view->workspace || !view->workspace->name ||
         strcmp(view->workspace->name, f->workspace) != 0)) {
        return false;
    }
    if (f->has_activated && view->activated != f->activated) {
        return false;
    }
    if (f->has_focused && (server->focused_view == view) != f->focused) {
        return false;
    }
    if (f->has_maximized && view->maximized != f->maximized) {
        return false;
    }
    if (f->has_fullscreen && view->fullscreen != f->fullscreen) {
        return false;
    }
    if (f->has_minimized && view->minimized != f->minimized) {
        return false;
    }
    if (f->has_ssd && (view->ssd != NULL) != f->ssd) {
        return false;
    }
    return true;
}

static void write_wait_match_header(struct woodland_json_obj *obj,
                                    const struct woodland_ipc_wait *wait,
                                    uint64_t elapsed_ms)
{
    woodland_json_obj_open(obj, obj->json);
    woodland_json_obj_bool(obj, "ok", true);
    woodland_json_obj_str(obj, "cmd", "wait");
    woodland_json_obj_str(obj, "condition", wait->cond_name);
    woodland_json_obj_bool(obj, "matched", true);
    woodland_json_obj_int(obj, "elapsed_ms", (long long)elapsed_ms);
}

static void write_wait_view_summary(struct woodland_json_obj *parent,
                                    const struct woodland_view *view)
{
    woodland_json_obj_key(parent, "view");
    struct woodland_json_obj vobj;
    woodland_json_obj_open(&vobj, parent->json);
    woodland_json_obj_int(&vobj, "id", view->id);
    woodland_json_obj_str(&vobj, "title", view->title);
    woodland_json_obj_str(&vobj, "app_id", view->app_id);
    struct wlr_box outer = {0};
    woodland_view_outer_box(view, &outer);
    int vals[4] = {outer.x, outer.y, outer.width, outer.height};
    woodland_json_obj_key(&vobj, "outer");
    woodland_json_int_array(parent->json, vals, 4);
    woodland_json_obj_close(&vobj);
}

bool woodland_ipc_wait_eval(struct woodland_server *server,
                            const struct woodland_ipc_wait *wait,
                            uint64_t elapsed_ms, struct woodland_json *out,
                            bool *matched)
{
    *matched = false;
    if (!server || !wait || !out) {
        return false;
    }

    struct woodland_json_obj obj = {.json = out, .first = true};

    switch (wait->kind) {
    case WAIT_VIEW_MAPPED: {
        struct woodland_view *found = NULL;
        if (server->views.next) {
            struct woodland_view *view;
            wl_list_for_each(view, &server->views, link) {
                if (!view->mapped) {
                    continue;
                }
                if (view_matches_filter(server, view, &wait->view)) {
                    found = view;
                    break;
                }
            }
        }
        if (!found) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        write_wait_view_summary(&obj, found);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_VIEW_UNMAPPED: {
        if (server->views.next) {
            struct woodland_view *view;
            wl_list_for_each(view, &server->views, link) {
                if (!view->mapped) {
                    continue;
                }
                if (view_matches_filter(server, view, &wait->view)) {
                    return true;
                }
            }
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_VIEW_COUNT: {
        int n = 0;
        if (server->views.next) {
            struct woodland_view *view;
            wl_list_for_each(view, &server->views, link) {
                if (!wait->mapped_only || view->mapped) {
                    n++;
                }
            }
        }
        if (wait->has_count && n != wait->count) {
            return true;
        }
        if (wait->has_min && n < wait->min) {
            return true;
        }
        if (wait->has_max && n > wait->max) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_int(&obj, "count", n);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_FOCUS: {
        struct woodland_view *fv = server->focused_view;
        struct wlr_layer_surface_v1 *fl = server->focused_layer;
        if (wait->has_none && wait->none) {
            if (fv || fl) {
                return true;
            }
        } else {
            if (wait->has_none && !wait->none && !fv && !fl) {
                return true;
            }
            if (!wait->view.has_id && !wait->view.has_title && !wait->view.has_app_id &&
                !wait->has_layer && !wait->has_none) {
                if (!fv && !fl) {
                    return true;
                }
            }
            if (wait->has_layer) {
                if (!fl || !fl->namespace || strcmp(fl->namespace, wait->layer) != 0) {
                    return true;
                }
            }
            if (wait->view.has_id || wait->view.has_title || wait->view.has_app_id) {
                if (!fv || !view_matches_filter(server, fv, &wait->view)) {
                    return true;
                }
            }
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_key(&obj, "view");
        if (fv) {
            woodland_json_int(out, fv->id);
        } else {
            woodland_json_raw(out, "null");
        }
        woodland_json_obj_str(&obj, "title", fv ? fv->title : NULL);
        woodland_json_obj_str(&obj, "layer", fl ? fl->namespace : NULL);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_WORKSPACE:
        if (!server->active_workspace || !server->active_workspace->name ||
            strcmp(server->active_workspace->name, wait->name) != 0) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_str(&obj, "workspace", server->active_workspace->name);
        woodland_json_obj_close(&obj);
        return true;
    case WAIT_MULTITASK: {
        bool active = woodland_multitask_view_is_active(server);
        if (active != wait->active) {
            return true;
        }
        int cards = server->multitask_view ? server->multitask_view->card_count : 0;
        int thumbs = server->multitask_view ? server->multitask_view->thumb_count : 0;
        if (wait->has_card_count && cards != wait->card_count) {
            return true;
        }
        if (wait->has_thumb_count && thumbs != wait->thumb_count) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_bool(&obj, "active", active);
        woodland_json_obj_int(&obj, "card_count", cards);
        woodland_json_obj_int(&obj, "thumb_count", thumbs);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_WINDOW_MENU: {
        bool active = woodland_window_menu_is_active(server);
        if (active != wait->active) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_bool(&obj, "active", active);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_WINDOW_SWITCHER: {
        bool active = woodland_window_switcher_is_active(server);
        if (active != wait->active) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_bool(&obj, "active", active);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_SNAP_PREVIEW: {
        const struct woodland_snap_preview *sp = &server->snap_preview;
        bool active = (sp->edges != 0 && sp->tree && sp->tree->node.enabled);
        if (active != wait->active) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_bool(&obj, "active", active);
        if (active) {
            woodland_json_obj_int(&obj, "edges", (long long)sp->edges);
        }
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_OUTPUT_COUNT: {
        int n = 0;
        if (server->outputs.next) {
            struct woodland_output *wo;
            wl_list_for_each(wo, &server->outputs, link) {
                if (wo->wlr_output && wo->wlr_output->enabled) {
                    n++;
                }
            }
        }
        if (wait->has_count && n != wait->count) {
            return true;
        }
        if (wait->has_min && n < wait->min) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_int(&obj, "count", n);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_PIXEL: {
        uint8_t rgba[4] = {0};
        char err[128];
        if (!woodland_ipc_sample_pixel(server, wait->x, wait->y, rgba, err, sizeof(err))) {
            return true;
        }
        int delta = woodland_ipc_rgb_delta(rgba, wait->hex_rgb);
        if (delta > wait->tolerance) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_int(&obj, "x", wait->x);
        woodland_json_obj_int(&obj, "y", wait->y);
        int rgba_vals[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
        woodland_json_obj_key(&obj, "rgba");
        woodland_json_int_array(out, rgba_vals, 4);
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", rgba[0], rgba[1], rgba[2]);
        woodland_json_obj_str(&obj, "hex", hex);
        woodland_json_obj_int(&obj, "channel_delta", delta);
        woodland_json_obj_key(&obj, "source");
        woodland_ipc_write_pixel_source(server, wait->x, wait->y, rgba, out,
                                        NULL, NULL, NULL, NULL, NULL);
        woodland_json_obj_close(&obj);
        return true;
    }
    case WAIT_TRACE:
        if (!woodland_trace_has_match(wait->what, wait->obj, wait->has_obj,
                                      wait->has_detail ? wait->detail : NULL)) {
            return true;
        }
        *matched = true;
        write_wait_match_header(&obj, wait, elapsed_ms);
        woodland_json_obj_str(&obj, "what", wait->what);
        if (wait->has_obj) {
            woodland_json_obj_int(&obj, "obj", wait->obj);
        }
        if (wait->has_detail) {
            woodland_json_obj_str(&obj, "detail", wait->detail);
        }
        woodland_json_obj_close(&obj);
        return true;
    }

    return true;
}

void woodland_ipc_wait_write_timeout(const struct woodland_ipc_wait *wait,
                                     uint64_t elapsed_ms, struct woodland_json *out)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "timeout waiting for %s", wait->cond_name);
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", false);
    woodland_json_obj_str(&obj, "cmd", "wait");
    woodland_json_obj_str(&obj, "condition", wait->cond_name);
    woodland_json_obj_bool(&obj, "matched", false);
    woodland_json_obj_int(&obj, "elapsed_ms", (long long)elapsed_ms);
    woodland_json_obj_int(&obj, "timeout_ms", wait->timeout_ms);
    woodland_json_obj_str(&obj, "error", msg);
    woodland_json_obj_close(&obj);
}

bool woodland_ipc_cmd_wait_sync(struct woodland_server *server, const char *args,
                                struct woodland_json *out)
{
    struct woodland_ipc_wait *wait = NULL;
    char err[128];
    if (!woodland_ipc_wait_parse(args, &wait, err, sizeof(err))) {
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_bool(&obj, "ok", false);
        woodland_json_obj_str(&obj, "cmd", "wait");
        woodland_json_obj_str(&obj, "error", err);
        woodland_json_obj_close(&obj);
        return false;
    }
    if (!woodland_ipc_wait_validate_runtime(server, wait, err, sizeof(err))) {
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_bool(&obj, "ok", false);
        woodland_json_obj_str(&obj, "cmd", "wait");
        woodland_json_obj_str(&obj, "error", err);
        woodland_json_obj_close(&obj);
        woodland_ipc_wait_free(wait);
        return false;
    }

    bool matched = false;
    woodland_ipc_wait_eval(server, wait, 0, out, &matched);
    if (!matched) {
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_bool(&obj, "ok", false);
        woodland_json_obj_str(&obj, "cmd", "wait");
        woodland_json_obj_str(&obj, "condition", wait->cond_name);
        woodland_json_obj_bool(&obj, "matched", false);
        woodland_json_obj_int(&obj, "elapsed_ms", 0);
        woodland_json_obj_str(&obj, "error", "condition not met");
        woodland_json_obj_close(&obj);
    }
    woodland_ipc_wait_free(wait);
    return matched;
}
