// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WORKSPACE_H
#define WOODLAND_WORKSPACE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include "scene.h"

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

/**
 * 切换工作区并处理键盘焦点。
 *
 * restore_focus=true：切完把焦点交给新工作区 z 序最上的窗口（用户按
 * Alt+N 的正常语义）；false：只清焦点，调用方稍后会自己设焦点
 * （聚焦其它工作区的窗口时用，避免焦点先落到别人身上再被抢走）。
 */
void woodland_workspace_switch_focus(struct woodland_server *server,
                                     struct woodland_workspace *target,
                                     bool restore_focus);

#endif
