// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "woodland.h"

#define WOODLAND_LAYER_SHELL_VERSION 4

bool woodland_layer_init(struct woodland_server *server) {
    server->layer_shell = wlr_layer_shell_v1_create(
        server->display, WOODLAND_LAYER_SHELL_VERSION);
    if (!server->layer_shell) {
        wlr_log(WLR_ERROR, "failed to create layer shell");
        return false;
    }
    return true;
}

void woodland_layer_finish(struct woodland_server *server) {
    /* manager 随 display 销毁，不单独 destroy；此处仅置空 */
    server->layer_shell = NULL;
    server->focused_layer = NULL;
}
