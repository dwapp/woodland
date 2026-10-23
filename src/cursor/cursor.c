// SPDX-License-Identifier: MIT
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
    (void)server;
    (void)name;
}
