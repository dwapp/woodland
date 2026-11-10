// SPDX-License-Identifier: GPL-2.0-only
/*
 * wl_display 全局接口过滤器分发器：见 include/util/global_filter.h。
 *
 * 通过 wl_display_add_destroy_listener() 将注册表绑定到对应的 wl_display
 * 生命周期上，既避免静态全局变量跨 display 污染，也能在异常路径跳过
 * 模块 finish() 时随 display 自动清理。
 */
#include "util/global_filter.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

struct global_filter_entry {
    struct wl_list link;
    char *name;
    wl_display_global_filter_func_t filter;
    void *data;
};

struct global_filter_registry {
    struct wl_display *display;
    struct wl_list entries; // struct global_filter_entry.link
    size_t count;
    struct wl_listener display_destroy;
};

static void registry_destroy(struct global_filter_registry *reg, bool clear_display_filter)
{
    if (!reg) {
        return;
    }
    if (clear_display_filter && reg->display) {
        wl_display_set_global_filter(reg->display, NULL, NULL);
    }
    struct global_filter_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &reg->entries, link) {
        wl_list_remove(&entry->link);
        free(entry->name);
        free(entry);
    }
    if (reg->display_destroy.link.prev) {
        wl_list_remove(&reg->display_destroy.link);
    }
    free(reg);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct global_filter_registry *reg = wl_container_of(listener, reg, display_destroy);
    registry_destroy(reg, false);
}

static struct global_filter_registry *find_registry(struct wl_display *display)
{
    if (!display) {
        return NULL;
    }
    struct wl_listener *listener =
        wl_display_get_destroy_listener(display, handle_display_destroy);
    if (!listener) {
        return NULL;
    }
    struct global_filter_registry *reg = wl_container_of(listener, reg, display_destroy);
    return reg;
}

static bool dispatch_global_filter(const struct wl_client *client,
                                   const struct wl_global *global,
                                   void *data)
{
    struct global_filter_registry *reg = data;
    if (!reg) {
        return true;
    }
    struct global_filter_entry *entry;
    wl_list_for_each(entry, &reg->entries, link) {
        if (entry->filter && !entry->filter(client, global, entry->data)) {
            return false;
        }
    }
    return true;
}

bool woodland_global_filter_add(struct wl_display *display,
                                const char *name,
                                wl_display_global_filter_func_t filter,
                                void *data)
{
    if (!display || !name || !name[0] || !filter) {
        wlr_log(WLR_ERROR, "global_filter: invalid arguments for '%s'", name ? name : "(null)");
        return false;
    }

    struct global_filter_registry *reg = find_registry(display);
    bool created_reg = false;
    if (reg) {
        struct global_filter_entry *existing;
        wl_list_for_each(existing, &reg->entries, link) {
            if (strcmp(existing->name, name) == 0) {
                wlr_log(WLR_ERROR, "global_filter: duplicate registration '%s'", name);
                return false;
            }
        }
        if (reg->count >= WOODLAND_GLOBAL_FILTER_MAX) {
            wlr_log(WLR_ERROR, "global_filter: limit (%d) reached while adding '%s'",
                    WOODLAND_GLOBAL_FILTER_MAX, name);
            return false;
        }
    } else {
        reg = calloc(1, sizeof(*reg));
        if (!reg) {
            wlr_log(WLR_ERROR, "global_filter: cannot allocate registry");
            return false;
        }
        reg->display = display;
        wl_list_init(&reg->entries);
        reg->display_destroy.notify = handle_display_destroy;
        wl_display_add_destroy_listener(display, &reg->display_destroy);
        wl_display_set_global_filter(display, dispatch_global_filter, reg);
        created_reg = true;
    }

    struct global_filter_entry *entry = calloc(1, sizeof(*entry));
    char *name_copy = strdup(name);
    if (!entry || !name_copy) {
        wlr_log(WLR_ERROR, "global_filter: cannot allocate entry '%s'", name);
        free(entry);
        free(name_copy);
        if (created_reg) {
            registry_destroy(reg, true);
        }
        return false;
    }

    entry->name = name_copy;
    entry->filter = filter;
    entry->data = data;
    wl_list_insert(reg->entries.prev, &entry->link);
    reg->count++;
    return true;
}

void woodland_global_filter_remove(struct wl_display *display, const char *name)
{
    if (!display || !name || !name[0]) {
        return;
    }
    struct global_filter_registry *reg = find_registry(display);
    if (!reg) {
        return;
    }

    struct global_filter_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &reg->entries, link) {
        if (strcmp(entry->name, name) == 0) {
            wl_list_remove(&entry->link);
            free(entry->name);
            free(entry);
            if (reg->count > 0) {
                reg->count--;
            }
            break;
        }
    }

    if (wl_list_empty(&reg->entries)) {
        registry_destroy(reg, true);
    }
}

size_t woodland_global_filter_count(struct wl_display *display)
{
    struct global_filter_registry *reg = find_registry(display);
    return reg ? reg->count : 0;
}

bool woodland_global_filter_has(struct wl_display *display, const char *name)
{
    if (!display || !name || !name[0]) {
        return false;
    }
    struct global_filter_registry *reg = find_registry(display);
    if (!reg) {
        return false;
    }
    struct global_filter_entry *entry;
    wl_list_for_each(entry, &reg->entries, link) {
        if (strcmp(entry->name, name) == 0) {
            return true;
        }
    }
    return false;
}
