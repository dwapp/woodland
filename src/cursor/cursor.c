// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "cursor.h"
#include "woodland.h"

bool woodland_cursor_init(struct woodland_server *server) {
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

    return true;
}

void woodland_cursor_finish(struct woodland_server *server) {
    if (server->xcursor_manager) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = NULL;
    }
    if (server->cursor) {
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
    }
}

void woodland_cursor_set(struct woodland_server *server, const char *name) {
    wlr_cursor_unset_image(server->cursor);
    wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}
