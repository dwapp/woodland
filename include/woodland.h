// SPDX-License-Identifier: MIT
#ifndef WOODLAND_WOODLAND_H
#define WOODLAND_WOODLAND_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>

struct woodland_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wl_list outputs;               // struct woodland_output.link 链表头
    struct wl_listener new_output;        // backend->events.new_output 监听器
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor_manager;
    struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_listener request_set_shape;
};

#endif
