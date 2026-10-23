// SPDX-License-Identifier: MIT
#ifndef WOODLAND_OUTPUT_H
#define WOODLAND_OUTPUT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct woodland_server;

struct woodland_output {
    struct wl_list link;               // woodland_server.outputs
    struct woodland_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame;
    struct wl_listener destroy;
};

bool woodland_output_init(struct woodland_server *server);
void woodland_output_finish(struct woodland_server *server);

#endif
