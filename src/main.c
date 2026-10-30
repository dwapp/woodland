// SPDX-License-Identifier: GPL-2.0-only
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>

#include "placement.h"
#include "server.h"
#include "trace.h"
#include "util/log.h"
#include "woodland.h"

static void usage(const char *name) {
    printf("Usage: %s [-s startup command] [-p placement policy] [--self-check]"
           " [--trace]\n"
           "\n"
           "  -s  command run after the compositor is up\n"
           "  -p  new window placement: cascade (default), center, cursor, autoplace\n"
           "  --self-check  run invariant checks after every state change\n"
           "                (failures are logged, not fatal)\n"
           "  --trace       keep the last 4096 state-change decisions in a ring\n"
           "                buffer and expose them as `wctl get_trace`\n"
           "\n"
           "The debug socket path is exported as WOODLAND_IPC; query it with\n"
           "`wctl get_tree | jq` or `nc -U $WOODLAND_IPC`.\n",
           name);
}

int main(int argc, char *argv[]) {
    woodland_log_init(WLR_INFO);

    char *startup_cmd = NULL;
    bool self_check = false;
    bool trace_enabled = false;
    enum woodland_placement_policy placement = WOODLAND_PLACE_CASCADE;

    enum {
        OPT_SELF_CHECK = 256,
        OPT_TRACE,
    };
    static const struct option long_options[] = {
        {"self-check", no_argument, NULL, OPT_SELF_CHECK},
        {"trace", no_argument, NULL, OPT_TRACE},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:p:h", long_options, NULL)) != -1) {
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
        case OPT_SELF_CHECK:
            self_check = true;
            break;
        case OPT_TRACE:
            trace_enabled = true;
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

    /* 轨迹要在 server_init 之前就绪：初始化过程本身也会写轨迹 */
    woodland_trace_init(trace_enabled);

    struct woodland_server server = {0};
    server.startup_cmd = startup_cmd;
    server.placement_policy = placement;
    server.self_check = self_check;
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
    woodland_trace_finish();
    return 0;
}
