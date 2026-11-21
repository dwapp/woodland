// SPDX-License-Identifier: GPL-2.0-only
/*
 * 环境变量探测。见 include/util/env.h。
 *
 * 这两个判断都不碰任何全局状态，纯字符串比较，所以能被单元测试直接覆盖——
 * 而它们决定的却是"要不要在别人家里接管全局快捷键"这种有后果的事。
 */
#include "util/env.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

struct woodland_remote_parent_addon {
    struct wlr_addon addon;
    struct wlr_surface *parent;
};

static void remote_parent_addon_destroy(struct wlr_addon *addon)
{
    struct woodland_remote_parent_addon *entry = wl_container_of(addon, entry, addon);
    wlr_addon_finish(&entry->addon);
    free(entry);
}

static const struct wlr_addon_interface remote_parent_addon_impl = {
    .name = "woodland_remote_subsurface_parent",
    .destroy = remote_parent_addon_destroy,
};

void woodland_remote_subsurface_set_parent(struct wlr_surface *child, struct wlr_surface *parent)
{
    if (!child) {
        return;
    }
    struct wlr_addon *found =
        wlr_addon_find(&child->addons, child, &remote_parent_addon_impl);
    if (!parent) {
        if (found) {
            remote_parent_addon_destroy(found);
        }
        return;
    }
    if (found) {
        struct woodland_remote_parent_addon *entry = wl_container_of(found, entry, addon);
        entry->parent = parent;
        return;
    }
    struct woodland_remote_parent_addon *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return;
    }
    entry->parent = parent;
    wlr_addon_init(&entry->addon, &child->addons, child, &remote_parent_addon_impl);
}

struct wlr_surface *woodland_remote_subsurface_parent(struct wlr_surface *surface)
{
    if (!surface || !surface->addons.WLR_PRIVATE.addons.next) {
        return NULL;
    }
    struct wlr_addon *found =
        wlr_addon_find(&surface->addons, surface, &remote_parent_addon_impl);
    if (!found) {
        return NULL;
    }
    struct woodland_remote_parent_addon *entry = wl_container_of(found, entry, addon);
    return entry->parent;
}

struct wlr_surface *woodland_surface_get_root_surface(struct wlr_surface *surface)
{
    if (!surface) {
        return NULL;
    }
    struct wlr_surface *curr = wlr_surface_get_root_surface(surface);
    for (int depth = 0; curr && depth < 32; depth++) {
        struct wlr_surface *remote_parent = woodland_remote_subsurface_parent(curr);
        if (!remote_parent || remote_parent == curr) {
            break;
        }
        curr = wlr_surface_get_root_surface(remote_parent);
    }
    return curr;
}

/* token 是 XDG_CURRENT_DESKTOP 里 ':' 之间的一段（已去空白） */
static bool token_is_dde(const char *token, size_t len)
{
    if (len == 3 && strncasecmp(token, "dde", 3) == 0) {
        return true;
    }
    /* deepin / deepin-wayland / Deepin 都算：DDE 的桌面名历史上两种写法都有 */
    if (len >= 6 && strncasecmp(token, "deepin", 6) == 0) {
        return true;
    }
    return false;
}

bool woodland_desktop_is_dde(const char *current_desktop)
{
    if (!current_desktop || !*current_desktop) {
        return false;
    }
    const char *p = current_desktop;
    while (true) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            len--;
        }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
            len--;
        }
        if (token_is_dde(p, len)) {
            return true;
        }
        if (!sep) {
            break;
        }
        p = sep + 1;
    }
    return false;
}

bool woodland_session_is_dde(void)
{
    static const char *const vars[] = {
        "XDG_CURRENT_DESKTOP",
        "XDG_SESSION_DESKTOP",
        "DESKTOP_SESSION",
        NULL,
    };
    for (size_t i = 0; vars[i]; i++) {
        if (woodland_desktop_is_dde(getenv(vars[i]))) {
            return true;
        }
    }
    return false;
}

bool woodland_env_is_off(const char *value)
{
    if (!value) {
        return false;
    }
    if (!*value) {
        return true;
    }
    static const char *const off_words[] = {"0", "false", "no", "off", "none", NULL};
    for (size_t i = 0; off_words[i]; i++) {
        if (strcasecmp(value, off_words[i]) == 0) {
            return true;
        }
    }
    return false;
}
