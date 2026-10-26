// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>

#include "workspace.h"
#include "woodland.h"

#define WOODLAND_WORKSPACE_COUNT 10

bool woodland_workspace_init(struct woodland_server *server) {
    wl_list_init(&server->workspaces);

    for (int i = 1; i <= WOODLAND_WORKSPACE_COUNT; i++) {
        struct woodland_workspace *ws = calloc(1, sizeof(*ws));
        if (!ws) {
            woodland_workspace_finish(server);
            return false;
        }
        ws->server = server;
        char name[8];
        snprintf(name, sizeof(name), "%d", i);
        ws->name = strdup(name);
        if (!ws->name) {
            free(ws);
            woodland_workspace_finish(server);
            return false;
        }
        ws->tree = wlr_scene_tree_create(&server->scene->tree);
        if (!ws->tree) {
            free(ws->name);
            free(ws);
            woodland_workspace_finish(server);
            return false;
        }
        /* 仅第一个工作区初始可见 */
        wlr_scene_node_set_enabled(&ws->tree->node, i == 1);
        /* 追加到链表末尾（wl_list_insert 是头部插入，故用 prev 实现 append） */
        wl_list_insert(server->workspaces.prev, &ws->link);
    }

    struct woodland_workspace *first;
    first = wl_container_of(server->workspaces.next, first, link);
    server->active_workspace = first;
    return true;
}

struct woodland_workspace *woodland_workspace_by_name(
        struct woodland_server *server, const char *name) {
    struct woodland_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (strcmp(ws->name, name) == 0) {
            return ws;
        }
    }
    return NULL;
}

void woodland_workspace_finish(struct woodland_server *server) {
    struct woodland_workspace *ws, *tmp;
    wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
        wl_list_remove(&ws->link);
        wlr_scene_node_destroy(&ws->tree->node);
        free(ws->name);
        free(ws);
    }
    server->active_workspace = NULL;
}
