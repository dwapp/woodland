// SPDX-License-Identifier: MIT
#include "server.h"
#include "util/log.h"
#include "woodland.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    woodland_log_init(WLR_INFO);

    struct woodland_server server = {0};
    if (!woodland_server_init(&server)) {
        woodland_server_finish(&server);
        return 1;
    }
    if (!woodland_server_start(&server)) {
        woodland_server_finish(&server);
        return 1;
    }
    woodland_server_run(&server);
    woodland_server_finish(&server);
    return 0;
}
