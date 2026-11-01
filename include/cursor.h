// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_CURSOR_H
#define WOODLAND_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_server;

bool woodland_cursor_init(struct woodland_server *server);
void woodland_cursor_finish(struct woodland_server *server);
void woodland_cursor_set(struct woodland_server *server, const char *name);
void woodland_cursor_set_resize(struct woodland_server *server, uint32_t edges);

#endif
