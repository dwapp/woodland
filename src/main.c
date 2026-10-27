// SPDX-License-Identifier: GPL-2.0-only
#include <getopt.h>
#include <stdio.h>

#include "placement.h"
#include "server.h"
#include "util/log.h"
#include "woodland.h"

static void usage(const char *name) {
    printf("Usage: %s [-s startup command] [-p placement policy]\n"
           "\n"
           "  -s  command run after the compositor is up\n"
           "  -p  new window placement: cascade (default), center, cursor, autoplace\n",
           name);
}

int main(int argc, char *argv[]) {
    woodland_log_init(WLR_INFO);

    char *startup_cmd = NULL;
    enum woodland_placement_policy placement = WOODLAND_PLACE_CASCADE;

    int c;
    while ((c = getopt(argc, argv, "s:p:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        case 'p':
            if (!woodland_placement_policy_parse(optarg, &placement)) {
                printf("unknown placement policy: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 0;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return 0;
    }

    struct woodland_server server = {0};
    server.startup_cmd = startup_cmd;
    server.placement_policy = placement;
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
