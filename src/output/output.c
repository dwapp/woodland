// SPDX-License-Identifier: MIT
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "output.h"
#include "woodland.h"

bool woodland_output_init(struct woodland_server *server) {
    wl_list_init(&server->outputs);

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) {
        return false;
    }

    server->scene_layout =
        wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_layout) {
        return false;
    }

    return true;
}

void woodland_output_finish(struct woodland_server *server) {
    if (server->scene_layout) {
        /* 自动随 output_layout 销毁；销毁 output_layout 前先置空自身引用 */
        server->scene_layout = NULL;
    }
    if (server->output_layout) {
        wlr_output_layout_destroy(server->output_layout);
        server->output_layout = NULL;
    }
}
