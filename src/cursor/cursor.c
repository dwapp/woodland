// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "cursor.h"
#include "woodland.h"

bool woodland_cursor_init(struct woodland_server *server)
{
    server->cursor = wlr_cursor_create();
    if (!server->cursor) {
        return false;
    }

    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

    const char *theme = getenv("XCURSOR_THEME");
    const char *size_str = getenv("XCURSOR_SIZE");
    int size = 24;
    if (size_str) {
        char *end = NULL;
        long parsed = strtol(size_str, &end, 10);
        if (end != size_str && *end == '\0' && parsed > 0) {
            size = (int)parsed;
        }
    }

    server->xcursor_manager = wlr_xcursor_manager_create(theme, size);
    if (!server->xcursor_manager) {
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
        return false;
    }

    if (!wlr_xcursor_manager_load(server->xcursor_manager, 1)) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = NULL;
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
        return false;
    }

    if (!woodland_pointer_constraints_init(server)) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = NULL;
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
        return false;
    }

    return true;
}

void woodland_cursor_finish(struct woodland_server *server)
{
    woodland_pointer_constraints_finish(server);
    if (server->xcursor_manager) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = NULL;
    }
    if (server->cursor) {
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
    }
}

void woodland_cursor_set(struct woodland_server *server, const char *name)
{
    wlr_cursor_unset_image(server->cursor);
    wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}

/* 建一个 manager 并确保它至少能加载 1x 一套；失败时自己回收干净 */
static struct wlr_xcursor_manager *manager_create(const char *theme, int size)
{
    struct wlr_xcursor_manager *mgr = wlr_xcursor_manager_create(theme, size);
    if (!mgr) {
        return NULL;
    }
    if (!wlr_xcursor_manager_load(mgr, 1)) {
        wlr_xcursor_manager_destroy(mgr);
        return NULL;
    }
    return mgr;
}

bool woodland_cursor_apply_theme(struct woodland_server *server, const char *theme, int size,
                                 const char **effective)
{
    if (effective) {
        *effective = NULL;
    }
    if (!server || !server->cursor) {
        return false;
    }
    if (size <= 0) {
        size = WOODLAND_CURSOR_DEFAULT_SIZE;
    }
    if (theme && !*theme) {
        theme = NULL;
    }

    /*
     * 先建新的、成了再拆旧的：直接 destroy 再 create 的话，主题名拼错
     * （create 不校验主题是否存在，load 才会失败）就会把光标管理器弄成
     * NULL，之后每一次 woodland_cursor_set 都是空指针解引用。
     */
    struct wlr_xcursor_manager *mgr = manager_create(theme, size);
    const char *used = theme;
    if (!mgr && theme) {
        /* 协议原文：请求的主题没装上就退到默认主题，不是"什么都不做" */
        wlr_log(WLR_INFO, "cursor: theme '%s' unavailable, falling back to default", theme);
        mgr = manager_create(NULL, size);
        used = NULL;
    }
    if (!mgr) {
        wlr_log(WLR_ERROR, "cursor: cannot create xcursor manager (size=%d)", size);
        return false;
    }

    if (server->xcursor_manager) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
    }
    server->xcursor_manager = mgr;
    /* 光标形状可能正停在 resize 上，重设成默认箭头由调用方决定；这里只
     * 重设一次，保证新主题立刻可见 */
    woodland_cursor_set(server, "default");
    if (effective) {
        *effective = used ? used : "default";
    }
    return true;
}

const char *woodland_cursor_resize_name(uint32_t edges)
{
    if (edges == (WLR_EDGE_TOP | WLR_EDGE_LEFT)) {
        return "nw-resize";
    } else if (edges == (WLR_EDGE_TOP | WLR_EDGE_RIGHT)) {
        return "ne-resize";
    } else if (edges == (WLR_EDGE_BOTTOM | WLR_EDGE_LEFT)) {
        return "sw-resize";
    } else if (edges == (WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT)) {
        return "se-resize";
    } else if (edges & WLR_EDGE_TOP) {
        return "n-resize";
    } else if (edges & WLR_EDGE_BOTTOM) {
        return "s-resize";
    } else if (edges & WLR_EDGE_LEFT) {
        return "w-resize";
    } else if (edges & WLR_EDGE_RIGHT) {
        return "e-resize";
    }
    return "default";
}

const char *woodland_cursor_resize_fallback_name(uint32_t edges)
{
    if (edges == (WLR_EDGE_TOP | WLR_EDGE_LEFT)) {
        return "top_left_corner";
    } else if (edges == (WLR_EDGE_TOP | WLR_EDGE_RIGHT)) {
        return "top_right_corner";
    } else if (edges == (WLR_EDGE_BOTTOM | WLR_EDGE_LEFT)) {
        return "bottom_left_corner";
    } else if (edges == (WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT)) {
        return "bottom_right_corner";
    } else if (edges & WLR_EDGE_TOP) {
        return "top_side";
    } else if (edges & WLR_EDGE_BOTTOM) {
        return "bottom_side";
    } else if (edges & WLR_EDGE_LEFT) {
        return "left_side";
    } else if (edges & WLR_EDGE_RIGHT) {
        return "right_side";
    }
    return NULL;
}

void woodland_cursor_set_resize(struct woodland_server *server, uint32_t edges)
{
    if (!server || !server->cursor || !server->xcursor_manager) {
        return;
    }
    const char *primary = woodland_cursor_resize_name(edges);
    const char *fallback = woodland_cursor_resize_fallback_name(edges);
    const char *name = primary;
    if (fallback && !wlr_xcursor_manager_get_xcursor(server->xcursor_manager, primary, 1.0f)) {
        name = fallback;
    }
    wlr_cursor_unset_image(server->cursor);
    wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}
