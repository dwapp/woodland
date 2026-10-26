// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WORKSPACE_H
#define WOODLAND_WORKSPACE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct woodland_server;
struct wlr_ext_workspace_handle_v1;

struct woodland_workspace {
    struct wl_list link;                 // woodland_server.workspaces
    struct woodland_server *server;
    char *name;                          // "1".."10"
    struct wlr_scene_tree *tree;
    struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

bool woodland_workspace_init(struct woodland_server *server);
void woodland_workspace_finish(struct woodland_server *server);
struct woodland_workspace *woodland_workspace_by_name(
    struct woodland_server *server, const char *name);
void woodland_workspace_switch(struct woodland_server *server,
                               struct woodland_workspace *target);

#endif
