// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_ext_workspace_v1.h>
#include "scene.h"
#include <wlr/util/log.h>

#include "focus.h"
#include "trace.h"
#include "workspace.h"
#include "woodland.h"

#define WOODLAND_WORKSPACE_COUNT 10
#define EXT_WORKSPACE_VERSION 1

static void handle_ext_workspace_commit(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, ext_workspace_commit);
    struct wlr_ext_workspace_v1_commit_event *event = data;

    struct wlr_ext_workspace_v1_request *req;
    wl_list_for_each(req, event->requests, link) {
        if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE &&
            req->activate.workspace != NULL) {
            struct woodland_workspace *ws = req->activate.workspace->data;
            woodland_workspace_switch(server, ws);
            wlr_log(WLR_INFO, "activating workspace %s", ws->name);
        }
    }
}

bool woodland_workspace_init(struct woodland_server *server) {
    wl_list_init(&server->workspaces);

    server->ext_workspace_manager =
        wlr_ext_workspace_manager_v1_create(server->display, EXT_WORKSPACE_VERSION);
    if (!server->ext_workspace_manager) {
        wlr_log(WLR_ERROR, "failed to create ext-workspace manager");
        return false;
    }

    server->ext_workspace_group = wlr_ext_workspace_group_handle_v1_create(
        server->ext_workspace_manager, 0);

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

        ws->ext_workspace = wlr_ext_workspace_handle_v1_create(
            server->ext_workspace_manager, NULL,
            EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
        if (!ws->ext_workspace) {
            wlr_log(WLR_ERROR, "failed to create ext-workspace handle");
            free(ws->name);
            free(ws);
            woodland_workspace_finish(server);
            return false;
        }
        ws->ext_workspace->data = ws;
        wlr_ext_workspace_handle_v1_set_group(ws->ext_workspace,
            server->ext_workspace_group);
        wlr_ext_workspace_handle_v1_set_name(ws->ext_workspace, name);
        wlr_ext_workspace_handle_v1_set_active(ws->ext_workspace, i == 1);

        /* 追加到链表末尾（wl_list_insert 是头部插入，故用 prev 实现 append） */
        wl_list_insert(server->workspaces.prev, &ws->link);
    }

    struct woodland_workspace *first;
    first = wl_container_of(server->workspaces.next, first, link);
    server->active_workspace = first;

    woodland_listener_connect(&server->ext_workspace_commit, &server->ext_workspace_manager->events.commit,
                          handle_ext_workspace_commit, "workspace.commit");

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

void woodland_workspace_switch_focus(struct woodland_server *server,
                                     struct woodland_workspace *target,
                                     bool restore_focus) {
    if (target == NULL || target == server->active_workspace) {
        return;
    }

    /* 先清旧焦点，再切 active_workspace。
     *
     * 顺序反过来会漏掉清外观：切换之后旧焦点视图已经不在新工作区，
     * focusable 为假，而 headless 下没有键盘设备可以反查，焦点就"找不到
     * 是谁"了——它的 activated / SSD / foreign-toplevel 会一直亮着，
     * 等调用方设上新焦点，就有两个窗口同时处于激活态。 */
    if (!restore_focus && server->seat && server->focused_layer == NULL) {
        /* 调用方马上要设焦点：先清掉旧工作区的键盘焦点，避免按键
         * 继续发给已隐藏的窗口。 */
        woodland_focus_none(server);
    }

    struct woodland_workspace *from = server->active_workspace;
    wlr_scene_node_set_enabled(&server->active_workspace->tree->node, false);
    wlr_ext_workspace_handle_v1_set_active(
        server->active_workspace->ext_workspace, false);
    wlr_scene_node_set_enabled(&target->tree->node, true);
    wlr_ext_workspace_handle_v1_set_active(target->ext_workspace, true);
    server->active_workspace = target;
    woodland_trace_add("workspace", 0, "%s -> %s restore=%d",
                       from ? from->name : "-", target->name,
                       restore_focus ? 1 : 0);

    if (restore_focus) {
        /* 焦点回退会挑新工作区 z 序最上的窗口；若焦点在常驻的 layer
         * surface 上则保持不动（见 woodland_focus_fallback）。 */
        woodland_focus_fallback(server);
    }
}

void woodland_workspace_switch(struct woodland_server *server,
                               struct woodland_workspace *target) {
    woodland_workspace_switch_focus(server, target, true);
}

void woodland_workspace_finish(struct woodland_server *server) {
    if (server->ext_workspace_manager) {
        wl_list_remove(&server->ext_workspace_commit.link);
    }
    struct woodland_workspace *ws, *tmp;
    wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
        wl_list_remove(&ws->link);
        if (ws->ext_workspace) {
            wlr_ext_workspace_handle_v1_destroy(ws->ext_workspace);
        }
        wlr_scene_node_destroy(&ws->tree->node);
        free(ws->name);
        free(ws);
    }
    if (server->ext_workspace_group) {
        wlr_ext_workspace_group_handle_v1_destroy(server->ext_workspace_group);
        server->ext_workspace_group = NULL;
    }
    /* manager 随 display 销毁，不单独 destroy */
    server->ext_workspace_manager = NULL;
    server->active_workspace = NULL;
}
