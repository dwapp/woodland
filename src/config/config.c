// SPDX-License-Identifier: GPL-2.0-only
#include "toml.h"
#include "util/json.h"
#include "util/log.h"
#include "woodland_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static uint32_t parse_color(const char *hex, uint32_t def)
{
    if (!hex || *hex != '#') {
        return def;
    }
    hex++;
    size_t len = strlen(hex);
    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (len == 6) {
        if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    } else if (len == 8) {
        if (sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return def;
}

static enum wl_output_transform parse_transform(const char *name)
{
    if (!name) {
        return WL_OUTPUT_TRANSFORM_NORMAL;
    }
    if (strcmp(name, "90") == 0)
        return WL_OUTPUT_TRANSFORM_90;
    if (strcmp(name, "180") == 0)
        return WL_OUTPUT_TRANSFORM_180;
    if (strcmp(name, "270") == 0)
        return WL_OUTPUT_TRANSFORM_270;
    if (strcmp(name, "flipped") == 0)
        return WL_OUTPUT_TRANSFORM_FLIPPED;
    if (strcmp(name, "flipped-90") == 0)
        return WL_OUTPUT_TRANSFORM_FLIPPED_90;
    if (strcmp(name, "flipped-180") == 0)
        return WL_OUTPUT_TRANSFORM_FLIPPED_180;
    if (strcmp(name, "flipped-270") == 0)
        return WL_OUTPUT_TRANSFORM_FLIPPED_270;
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

void woodland_config_default_init(struct woodland_config *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));

    /* [general] */
    config->general.placement_policy = WOODLAND_PLACE_CASCADE;
    config->general.placement_specified = false;
    config->general.workspace_count = 10;
    config->general.focus_follows_mouse = false;
    config->general.snap_distance = WOODLAND_SNAP_DISTANCE;

    /* [decorations] */
    config->ssd.enabled = true;
    config->ssd.titlebar_height = WOODLAND_SSD_TITLEBAR_H;
    config->ssd.border_width = WOODLAND_SSD_BORDER_W;
    config->ssd.button_width = WOODLAND_SSD_BUTTON_W;
    config->ssd.font = strdup("sans 10");
    config->ssd.active_title_color = 0xFFE1DEDB;    /* #e1dedb */
    config->ssd.inactive_title_color = 0xFFF7F6F4;  /* #f7f6f4 */
    config->ssd.active_border_color = 0xFFAAAAAA;   /* #aaaaaa */
    config->ssd.inactive_border_color = 0xFFAAAAAA; /* #aaaaaa */

    /* [input.keyboard] */
    config->keyboard.layout = NULL;
    config->keyboard.variant = NULL;
    config->keyboard.options = NULL;
    config->keyboard.model = NULL;
    config->keyboard.repeat_rate = 25;
    config->keyboard.repeat_delay = 600;

    /* [input.touchpad] */
    config->touchpad.tap_to_click = false;
    config->touchpad.natural_scroll = false;
    config->touchpad.accel_speed = 0.0;
    config->touchpad.accel_profile = strdup("adaptive");

    /* [cursor] */
    config->cursor.theme = strdup("default");
    config->cursor.size = 24;

    /* [effects] */
    config->effects.enabled = false;
    config->effects.blur = false;
    config->effects.corner_radius = 0;
    config->effects.shadow = false;

    /* [[output]] */
    wl_list_init(&config->output_rules);
}

struct woodland_config *woodland_config_create_default(void)
{
    struct woodland_config *config = calloc(1, sizeof(*config));
    if (!config) {
        return NULL;
    }
    woodland_config_default_init(config);
    return config;
}

static void free_output_rules(struct wl_list *list)
{
    struct woodland_config_output_rule *rule, *tmp;
    wl_list_for_each_safe(rule, tmp, list, link)
    {
        wl_list_remove(&rule->link);
        free(rule->name);
        free(rule->mode);
        free(rule);
    }
}

void woodland_config_destroy(struct woodland_config *config)
{
    if (!config) {
        return;
    }
    free(config->loaded_path);
    free(config->ssd.font);
    free(config->keyboard.layout);
    free(config->keyboard.variant);
    free(config->keyboard.options);
    free(config->keyboard.model);
    free(config->touchpad.accel_profile);
    free(config->cursor.theme);
    free_output_rules(&config->output_rules);
    free(config);
}

static void parse_output_table(const struct toml_table *tbl, struct woodland_config *cfg)
{
    const char *name = toml_get_string(tbl, "name", NULL);
    if (!name) {
        return;
    }

    struct woodland_config_output_rule *rule = calloc(1, sizeof(*rule));
    if (!rule) {
        return;
    }
    rule->name = strdup(name);
    const char *mode_str = toml_get_string(tbl, "mode", "preferred");
    rule->mode = strdup(mode_str);
    rule->scale = toml_get_float(tbl, "scale", 1.0);
    if (rule->scale <= 0.0) {
        rule->scale = 1.0;
    }
    rule->enabled = toml_get_bool(tbl, "enabled", true);
    rule->adaptive_sync = toml_get_bool(tbl, "adaptive_sync", false);
    rule->transform = parse_transform(toml_get_string(tbl, "transform", "normal"));

    /* position: [x, y] 或 "auto" */
    const struct toml_value *pos_val = toml_get(tbl, "position");
    if (pos_val && pos_val->type == TOML_ARRAY && pos_val->arr.count >= 2) {
        if (pos_val->arr.items[0].type == TOML_INT && pos_val->arr.items[1].type == TOML_INT) {
            rule->x = (int32_t)pos_val->arr.items[0].i;
            rule->y = (int32_t)pos_val->arr.items[1].i;
            rule->auto_position = false;
        } else {
            rule->auto_position = true;
        }
    } else {
        rule->auto_position = true;
    }

    wl_list_insert(cfg->output_rules.prev, &rule->link);
}

struct woodland_config *woodland_config_parse_string(const char *content, size_t len,
                                                     const char *source_name)
{
    struct woodland_config *cfg = woodland_config_create_default();
    if (!cfg) {
        return NULL;
    }

    if (!content || len == 0 || !*content) {
        return cfg;
    }

    char *bounded = strndup(content, len);
    if (!bounded) {
        return cfg;
    }

    char errbuf[256] = {0};
    struct toml_doc *doc = toml_parse(bounded, errbuf, sizeof(errbuf));
    free(bounded);
    if (!doc) {
        wlr_log(WLR_ERROR, "config: syntax error in %s: %s (using defaults)",
                source_name ? source_name : "<memory>", errbuf);
        return cfg;
    }

    /* 1. [general] */
    const struct toml_table *gen = toml_find_table(doc, "general");
    if (!gen) {
        gen = doc->root;
    }
    if (gen) {
        const char *policy_str = toml_get_string(gen, "placement_policy", NULL);
        if (policy_str) {
            enum woodland_placement_policy p;
            if (woodland_placement_policy_parse(policy_str, &p)) {
                cfg->general.placement_policy = p;
                cfg->general.placement_specified = true;
            }
        }
        int64_t ws = toml_get_int(gen, "workspace_count", cfg->general.workspace_count);
        if (ws >= 1 && ws <= 12) {
            cfg->general.workspace_count = (uint32_t)ws;
        }
        const char *focus_str = toml_get_string(gen, "focus_policy", NULL);
        if (focus_str) {
            cfg->general.focus_follows_mouse = (strcmp(focus_str, "sloppy") == 0);
        }
        int64_t snap = toml_get_int(gen, "snap_distance", cfg->general.snap_distance);
        if (snap >= 0) {
            cfg->general.snap_distance = (int32_t)snap;
        }
    }

    /* 2. [decorations] */
    const struct toml_table *ssd = toml_find_table(doc, "decorations");
    if (ssd) {
        cfg->ssd.enabled = toml_get_bool(ssd, "enabled", cfg->ssd.enabled);
        int64_t th = toml_get_int(ssd, "titlebar_height", cfg->ssd.titlebar_height);
        if (th > 0 && th < 200) {
            cfg->ssd.titlebar_height = (int32_t)th;
        }
        int64_t bw = toml_get_int(ssd, "border_width", cfg->ssd.border_width);
        if (bw >= 0 && bw < 50) {
            cfg->ssd.border_width = (int32_t)bw;
        }
        int64_t btn_w = toml_get_int(ssd, "button_width", cfg->ssd.button_width);
        if (btn_w > 0 && btn_w < 200) {
            cfg->ssd.button_width = (int32_t)btn_w;
        }
        const char *font_str = toml_get_string(ssd, "font", NULL);
        if (font_str) {
            free(cfg->ssd.font);
            cfg->ssd.font = strdup(font_str);
        }
        cfg->ssd.active_title_color = parse_color(toml_get_string(ssd, "active_title_color", NULL),
                                                  cfg->ssd.active_title_color);
        cfg->ssd.inactive_title_color = parse_color(
            toml_get_string(ssd, "inactive_title_color", NULL), cfg->ssd.inactive_title_color);
        cfg->ssd.active_border_color = parse_color(
            toml_get_string(ssd, "active_border_color", NULL), cfg->ssd.active_border_color);
        cfg->ssd.inactive_border_color = parse_color(
            toml_get_string(ssd, "inactive_border_color", NULL), cfg->ssd.inactive_border_color);
    }

    /* 3. [input.keyboard] 或 [keyboard] */
    const struct toml_table *kb = toml_find_table(doc, "input.keyboard");
    if (!kb) {
        kb = toml_find_table(doc, "keyboard");
    }
    if (kb) {
        const char *layout = toml_get_string(kb, "layout", NULL);
        if (layout) {
            free(cfg->keyboard.layout);
            cfg->keyboard.layout = strdup(layout);
        }
        const char *variant = toml_get_string(kb, "variant", NULL);
        if (variant) {
            free(cfg->keyboard.variant);
            cfg->keyboard.variant = strdup(variant);
        }
        const char *options = toml_get_string(kb, "options", NULL);
        if (options) {
            free(cfg->keyboard.options);
            cfg->keyboard.options = strdup(options);
        }
        const char *model = toml_get_string(kb, "model", NULL);
        if (model) {
            free(cfg->keyboard.model);
            cfg->keyboard.model = strdup(model);
        }
        int64_t rate = toml_get_int(kb, "repeat_rate", cfg->keyboard.repeat_rate);
        if (rate > 0) {
            cfg->keyboard.repeat_rate = (int32_t)rate;
        }
        int64_t delay = toml_get_int(kb, "repeat_delay", cfg->keyboard.repeat_delay);
        if (delay > 0) {
            cfg->keyboard.repeat_delay = (int32_t)delay;
        }
    }

    /* 4. [input.touchpad] 或 [touchpad] */
    const struct toml_table *tp = toml_find_table(doc, "input.touchpad");
    if (!tp) {
        tp = toml_find_table(doc, "touchpad");
    }
    if (tp) {
        cfg->touchpad.tap_to_click = toml_get_bool(tp, "tap_to_click", cfg->touchpad.tap_to_click);
        cfg->touchpad.natural_scroll =
            toml_get_bool(tp, "natural_scroll", cfg->touchpad.natural_scroll);
        cfg->touchpad.accel_speed = toml_get_float(tp, "accel_speed", cfg->touchpad.accel_speed);
        if (cfg->touchpad.accel_speed < -1.0)
            cfg->touchpad.accel_speed = -1.0;
        if (cfg->touchpad.accel_speed > 1.0)
            cfg->touchpad.accel_speed = 1.0;
        const char *prof = toml_get_string(tp, "accel_profile", NULL);
        if (prof) {
            free(cfg->touchpad.accel_profile);
            cfg->touchpad.accel_profile = strdup(prof);
        }
    }

    /* 5. [cursor] */
    const struct toml_table *cur = toml_find_table(doc, "cursor");
    if (cur) {
        const char *theme = toml_get_string(cur, "theme", NULL);
        if (theme) {
            free(cfg->cursor.theme);
            cfg->cursor.theme = strdup(theme);
        }
        int64_t size = toml_get_int(cur, "size", cfg->cursor.size);
        if (size > 0 && size < 512) {
            cfg->cursor.size = (int32_t)size;
        }
    }

    /* 6. [effects] */
    const struct toml_table *fx = toml_find_table(doc, "effects");
    if (fx) {
        cfg->effects.enabled = toml_get_bool(fx, "enabled", cfg->effects.enabled);
        cfg->effects.blur = toml_get_bool(fx, "blur", cfg->effects.blur);
        int64_t cr = toml_get_int(fx, "corner_radius", cfg->effects.corner_radius);
        if (cr >= 0) {
            cfg->effects.corner_radius = (int32_t)cr;
        }
        cfg->effects.shadow = toml_get_bool(fx, "shadow", cfg->effects.shadow);
    }

    /* 7. [[output]] 数组表 */
    for (const struct toml_table *t = toml_find_table(doc, "output"); t;
         t = toml_next_table(t, "output")) {
        parse_output_table(t, cfg);
    }

    toml_free(doc);
    return cfg;
}

static char *resolve_config_path(const char *path_override)
{
    if (path_override) {
        if (access(path_override, R_OK) == 0) {
            return strdup(path_override);
        }
        return NULL;
    }

    char candidate[1024];

    /* 1. $XDG_CONFIG_HOME/woodland/config.toml */
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(candidate, sizeof(candidate), "%s/woodland/config.toml", xdg_config);
        if (access(candidate, R_OK) == 0) {
            return strdup(candidate);
        }
    }

    /* 2. ~/.config/woodland/config.toml */
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(candidate, sizeof(candidate), "%s/.config/woodland/config.toml", home);
        if (access(candidate, R_OK) == 0) {
            return strdup(candidate);
        }
    }

    /* 3. $XDG_CONFIG_DIRS/woodland/config.toml */
    const char *xdg_dirs = getenv("XDG_CONFIG_DIRS");
    if (xdg_dirs && *xdg_dirs) {
        char *dirs = strdup(xdg_dirs);
        if (dirs) {
            char *saveptr = NULL;
            char *dir = strtok_r(dirs, ":", &saveptr);
            while (dir) {
                snprintf(candidate, sizeof(candidate), "%s/woodland/config.toml", dir);
                if (access(candidate, R_OK) == 0) {
                    free(dirs);
                    return strdup(candidate);
                }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(dirs);
        }
    }

    /* 4. /etc/xdg/woodland/config.toml */
    if (access("/etc/xdg/woodland/config.toml", R_OK) == 0) {
        return strdup("/etc/xdg/woodland/config.toml");
    }

    return NULL;
}

struct woodland_config *woodland_config_load(const char *path_override)
{
    char *path = resolve_config_path(path_override);
    if (!path) {
        wlr_log(WLR_INFO, "config: no config file found, using defaults");
        return woodland_config_create_default();
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        wlr_log(WLR_ERROR, "config: failed to open %s: %s (using defaults)", path, strerror(errno));
        free(path);
        return woodland_config_create_default();
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        free(path);
        return woodland_config_create_default();
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        free(path);
        return woodland_config_create_default();
    }
    rewind(f);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        free(path);
        return woodland_config_create_default();
    }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    struct woodland_config *cfg = woodland_config_parse_string(buf, read_bytes, path);
    free(buf);
    if (cfg) {
        cfg->loaded_path = path;
    } else {
        free(path);
    }
    return cfg;
}

void woodland_config_dump_json(const struct woodland_config *config, struct woodland_json *out)
{
    if (!config || !out) {
        return;
    }

    struct woodland_json_obj root;
    woodland_json_obj_open(&root, out);

    woodland_json_obj_str(&root, "loaded_path", config->loaded_path);

    /* general */
    woodland_json_obj_key(&root, "general");
    struct woodland_json_obj gen;
    woodland_json_obj_open(&gen, out);
    woodland_json_obj_str(&gen, "placement_policy",
                          woodland_placement_policy_name(config->general.placement_policy));
    woodland_json_obj_int(&gen, "workspace_count", config->general.workspace_count);
    woodland_json_obj_bool(&gen, "focus_follows_mouse", config->general.focus_follows_mouse);
    woodland_json_obj_int(&gen, "snap_distance", config->general.snap_distance);
    woodland_json_obj_close(&gen);

    /* decorations */
    woodland_json_obj_key(&root, "decorations");
    struct woodland_json_obj ssd;
    woodland_json_obj_open(&ssd, out);
    woodland_json_obj_bool(&ssd, "enabled", config->ssd.enabled);
    woodland_json_obj_int(&ssd, "titlebar_height", config->ssd.titlebar_height);
    woodland_json_obj_int(&ssd, "border_width", config->ssd.border_width);
    woodland_json_obj_int(&ssd, "button_width", config->ssd.button_width);
    woodland_json_obj_str(&ssd, "font", config->ssd.font);
    woodland_json_obj_int(&ssd, "active_title_color", config->ssd.active_title_color);
    woodland_json_obj_int(&ssd, "inactive_title_color", config->ssd.inactive_title_color);
    woodland_json_obj_int(&ssd, "active_border_color", config->ssd.active_border_color);
    woodland_json_obj_int(&ssd, "inactive_border_color", config->ssd.inactive_border_color);
    woodland_json_obj_close(&ssd);

    /* keyboard */
    woodland_json_obj_key(&root, "keyboard");
    struct woodland_json_obj kb;
    woodland_json_obj_open(&kb, out);
    woodland_json_obj_str(&kb, "layout", config->keyboard.layout);
    woodland_json_obj_str(&kb, "variant", config->keyboard.variant);
    woodland_json_obj_str(&kb, "options", config->keyboard.options);
    woodland_json_obj_str(&kb, "model", config->keyboard.model);
    woodland_json_obj_int(&kb, "repeat_rate", config->keyboard.repeat_rate);
    woodland_json_obj_int(&kb, "repeat_delay", config->keyboard.repeat_delay);
    woodland_json_obj_close(&kb);

    /* touchpad */
    woodland_json_obj_key(&root, "touchpad");
    struct woodland_json_obj tp;
    woodland_json_obj_open(&tp, out);
    woodland_json_obj_bool(&tp, "tap_to_click", config->touchpad.tap_to_click);
    woodland_json_obj_bool(&tp, "natural_scroll", config->touchpad.natural_scroll);
    woodland_json_obj_int(&tp, "accel_speed_milli",
                          (long long)(config->touchpad.accel_speed * 1000.0));
    woodland_json_obj_str(&tp, "accel_profile", config->touchpad.accel_profile);
    woodland_json_obj_close(&tp);

    /* cursor */
    woodland_json_obj_key(&root, "cursor");
    struct woodland_json_obj cur;
    woodland_json_obj_open(&cur, out);
    woodland_json_obj_str(&cur, "theme", config->cursor.theme);
    woodland_json_obj_int(&cur, "size", config->cursor.size);
    woodland_json_obj_close(&cur);

    /* effects */
    woodland_json_obj_key(&root, "effects");
    struct woodland_json_obj fx;
    woodland_json_obj_open(&fx, out);
    woodland_json_obj_bool(&fx, "enabled", config->effects.enabled);
    woodland_json_obj_bool(&fx, "blur", config->effects.blur);
    woodland_json_obj_int(&fx, "corner_radius", config->effects.corner_radius);
    woodland_json_obj_bool(&fx, "shadow", config->effects.shadow);
    woodland_json_obj_close(&fx);

    /* outputs */
    woodland_json_obj_key(&root, "outputs");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    struct woodland_config_output_rule *rule;
    wl_list_for_each(rule, &config->output_rules, link)
    {
        woodland_json_arr_sep(&arr);
        struct woodland_json_obj ro;
        woodland_json_obj_open(&ro, out);
        woodland_json_obj_str(&ro, "name", rule->name);
        woodland_json_obj_str(&ro, "mode", rule->mode);
        woodland_json_obj_int(&ro, "scale_milli", (long long)(rule->scale * 1000.0));
        woodland_json_obj_bool(&ro, "auto_position", rule->auto_position);
        woodland_json_obj_int(&ro, "x", rule->x);
        woodland_json_obj_int(&ro, "y", rule->y);
        woodland_json_obj_bool(&ro, "enabled", rule->enabled);
        woodland_json_obj_close(&ro);
    }
    woodland_json_arr_close(&arr);
    woodland_json_obj_close(&root);
}
