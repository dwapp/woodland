// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_LAYER_H
#define WOODLAND_LAYER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct woodland_server;
struct woodland_output;
struct wlr_layer_surface_v1;
struct wlr_xdg_popup;

struct woodland_layer_surface {
    struct woodland_server *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer_surface;
    bool mapped;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener surface_commit;
    struct wl_listener output_destroy;
    struct wl_listener node_destroy;
    struct wl_listener new_popup;
};

struct woodland_layer_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener commit;
    struct wl_listener destroy;
};

bool woodland_layer_init(struct woodland_server *server);
void woodland_layer_finish(struct woodland_server *server);

void woodland_layer_arrange(struct woodland_server *server,
                            struct woodland_output *output);

void woodland_layer_try_focus(struct woodland_server *server,
                              struct wlr_layer_surface_v1 *layer_surface);

struct wlr_layer_surface_v1 *woodland_layer_surface_at(
    struct woodland_server *server, double lx, double ly,
    struct wlr_surface **surface, double *sx, double *sy);

#endif
