// SPDX-License-Identifier: GPL-2.0-only
#include <getopt.h>
#include <stdio.h>

#include "server.h"
#include "util/log.h"
#include "woodland.h"

int main(int argc, char *argv[]) {
    woodland_log_init(WLR_INFO);

    char *startup_cmd = NULL;
    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        default:
            printf("Usage: %s [-s startup command]\n", argv[0]);
            return 0;
        }
    }
    if (optind < argc) {
        printf("Usage: %s [-s startup command]\n", argv[0]);
        return 0;
    }

    struct woodland_server server = {0};
    server.startup_cmd = startup_cmd;
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
