// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_CURSOR_H
#define WOODLAND_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_server;
struct wlr_surface;
struct wlr_pointer_constraint_v1;

bool woodland_cursor_init(struct woodland_server *server);
void woodland_cursor_finish(struct woodland_server *server);
void woodland_cursor_set(struct woodland_server *server, const char *name);
void woodland_cursor_set_resize(struct woodland_server *server, uint32_t edges);
const char *woodland_cursor_resize_name(uint32_t edges);
const char *woodland_cursor_resize_fallback_name(uint32_t edges);

/* 指针约束管理（zwp_pointer_constraints_v1） */
bool woodland_pointer_constraints_init(struct woodland_server *server);
void woodland_pointer_constraints_finish(struct woodland_server *server);
void woodland_pointer_constraint_recheck(struct woodland_server *server,
                                         struct wlr_surface *surface);
void woodland_pointer_constraint_set(struct woodland_server *server,
                                     struct wlr_pointer_constraint_v1 *constraint);
void woodland_pointer_constraint_apply(struct woodland_server *server,
                                       double *dx, double *dy);
#endif
