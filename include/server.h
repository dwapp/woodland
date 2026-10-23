// SPDX-License-Identifier: MIT
#ifndef WOODLAND_SERVER_H
#define WOODLAND_SERVER_H

#include <stdbool.h>

struct woodland_server;

bool woodland_server_init(struct woodland_server *server);
bool woodland_server_start(struct woodland_server *server);
void woodland_server_run(struct woodland_server *server);
void woodland_server_finish(struct woodland_server *server);

#endif
