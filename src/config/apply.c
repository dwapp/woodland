// SPDX-License-Identifier: GPL-2.0-only
#include "cursor.h"
#include "seat.h"
#include "ssd.h"
#include "toml.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

static bool str_diff(const char *a, const char *b)
{
    if ((a != NULL) != (b != NULL)) {
        return true;
    }
    return a && b && strcmp(a, b) != 0;
}

static void apply_keyboard_config(struct woodland_server *server,
                                  const struct woodland_config_keyboard *kb_cfg)
{
    if (!server || !kb_cfg || !server->keyboards.next) {
        return;
    }
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        return;
    }

    struct xkb_rule_names rules = {
        .rules = NULL,
        .model = kb_cfg->model,
        .layout = kb_cfg->layout,
        .variant = kb_cfg->variant,
        .options = kb_cfg->options,
    };

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) {
        struct woodland_keyboard *kb;
        wl_list_for_each(kb, &server->keyboards, link)
        {
            wlr_keyboard_set_keymap(kb->wlr_keyboard, keymap);
            wlr_keyboard_set_repeat_info(kb->wlr_keyboard, kb_cfg->repeat_rate,
                                         kb_cfg->repeat_delay);
        }
        xkb_keymap_unref(keymap);
    } else {
        wlr_log(WLR_ERROR, "config: failed to compile xkb keymap for layout=%s",
                kb_cfg->layout ? kb_cfg->layout : "default");
    }
    xkb_context_unref(ctx);
}

static void apply_ssd_config(struct woodland_server *server,
                             const struct woodland_config_decorations *ssd_cfg)
{
    if (!server || !server->views.next) {
        return;
    }
    struct woodland_view *view, *tmp;
    wl_list_for_each_safe(view, tmp, &server->views, link)
    {
        if (!ssd_cfg->enabled) {
            if (view->ssd) {
                woodland_ssd_destroy(view);
            }
        } else {
            if (!view->ssd && !view->deco_csd && view->mapped) {
                woodland_ssd_create(view);
            } else if (view->ssd) {
                woodland_ssd_update(view);
            }
        }
    }
}

static void apply_cursor_config(struct woodland_server *server,
                                const struct woodland_config_cursor *cur_cfg,
                                const struct woodland_config_cursor *old_cur_cfg)
{
    if (!server || !cur_cfg || !server->cursor || !server->xcursor_manager) {
        return;
    }
    bool theme_changed = !old_cur_cfg || str_diff(cur_cfg->theme, old_cur_cfg->theme);

    if (theme_changed && server->xcursor_manager) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = wlr_xcursor_manager_create(cur_cfg->theme, cur_cfg->size);
        if (server->xcursor_manager) {
            wlr_xcursor_manager_load(server->xcursor_manager, 1);
        }
    } else if (server->xcursor_manager) {
        wlr_xcursor_manager_load(server->xcursor_manager, cur_cfg->size);
    }
    woodland_cursor_set(server, "default");
}

void woodland_config_apply(struct woodland_server *server, struct woodland_config *new_config)
{
    if (!server || !new_config) {
        return;
    }

    struct woodland_config *old_config = server->config;

    /* 1. general */
    if (!server->placement_specified) {
        server->placement_policy = new_config->general.placement_policy;
    }

    /* 2. keyboard */
    bool kb_changed = !old_config ||
                      (new_config->keyboard.repeat_rate != old_config->keyboard.repeat_rate) ||
                      (new_config->keyboard.repeat_delay != old_config->keyboard.repeat_delay) ||
                      str_diff(new_config->keyboard.layout, old_config->keyboard.layout) ||
                      str_diff(new_config->keyboard.variant, old_config->keyboard.variant) ||
                      str_diff(new_config->keyboard.options, old_config->keyboard.options) ||
                      str_diff(new_config->keyboard.model, old_config->keyboard.model);
    if (kb_changed) {
        apply_keyboard_config(server, &new_config->keyboard);
    }

    /* 3. ssd */
    bool ssd_changed =
        !old_config || (new_config->ssd.enabled != old_config->ssd.enabled) ||
        (new_config->ssd.titlebar_height != old_config->ssd.titlebar_height) ||
        (new_config->ssd.border_width != old_config->ssd.border_width) ||
        (new_config->ssd.button_width != old_config->ssd.button_width) ||
        (new_config->ssd.active_title_color != old_config->ssd.active_title_color) ||
        (new_config->ssd.inactive_title_color != old_config->ssd.inactive_title_color) ||
        (new_config->ssd.active_border_color != old_config->ssd.active_border_color) ||
        (new_config->ssd.inactive_border_color != old_config->ssd.inactive_border_color);

    if (ssd_changed) {
        apply_ssd_config(server, &new_config->ssd);
    }

    /* 4. cursor */
    bool cur_changed = !old_config || (new_config->cursor.size != old_config->cursor.size) ||
                       str_diff(new_config->cursor.theme, old_config->cursor.theme);

    if (cur_changed) {
        apply_cursor_config(server, &new_config->cursor, old_config ? &old_config->cursor : NULL);
    }

    /* 5. 记入决策轨迹 */
    woodland_trace_add("cfg_apply", 0, "path=%s placement=%s kb_changed=%d ssd_changed=%d",
                       new_config->loaded_path ? new_config->loaded_path : "defaults",
                       woodland_placement_policy_name(server->placement_policy), kb_changed,
                       ssd_changed);

    /* 6. 替换配置指针并销毁旧配置 */
    server->config = new_config;
    if (old_config) {
        woodland_config_destroy(old_config);
    }
}

bool woodland_config_reload(struct woodland_server *server)
{
    if (!server) {
        return false;
    }

    const char *path = server->config ? server->config->loaded_path : NULL;
    if (!path || access(path, R_OK) != 0) {
        wlr_log(WLR_INFO, "config: reload: no valid file path to reload");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        wlr_log(WLR_ERROR, "config: reload failed: cannot open %s: %s", path, strerror(errno));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return false;
    }
    rewind(f);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* Phase 1: 语法与结构校验（若有语法错误，拒绝更新并保护当前状态） */
    char errbuf[256] = {0};
    struct toml_doc *doc = toml_parse(buf, errbuf, sizeof(errbuf));
    if (!doc) {
        wlr_log(WLR_ERROR, "config: reload failed: syntax error in %s: %s (keeping current config)",
                path, errbuf);
        free(buf);
        return false;
    }
    toml_free(doc);

    /* Phase 2: 解析并热应用 */
    struct woodland_config *new_cfg = woodland_config_parse_string(buf, read_bytes, path);
    free(buf);
    if (!new_cfg) {
        return false;
    }
    new_cfg->loaded_path = strdup(path);

    woodland_config_apply(server, new_cfg);
    wlr_log(WLR_INFO, "config: reloaded successfully from %s", path);
    return true;
}
