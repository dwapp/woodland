// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SEAT_H
#define WOODLAND_SEAT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

struct woodland_server;
struct wlr_surface;

struct woodland_keyboard {
    struct wl_list link;               // woodland_server.keyboards
    struct woodland_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

bool woodland_seat_init(struct woodland_server *server);
void woodland_seat_finish(struct woodland_server *server);

/** 设置键盘焦点到指定 surface（NULL 表示清除焦点）。 */
void woodland_seat_focus_surface(struct woodland_server *server,
                                 struct wlr_surface *surface);

#endif
