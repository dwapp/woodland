// SPDX-License-Identifier: GPL-2.0-only
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>

#include "placement.h"
#include "server.h"
#include "trace.h"
#include "util/log.h"
#include "woodland.h"

static void usage(const char *name)
{
    printf("Usage: %s [-s startup command] [-c config file] [-p placement policy] [--self-check]"
           " [--trace] [--no-shortcut-client]\n"
           "\n"
           "  -s  command run after the compositor is up\n"
           "  -c  path to configuration file (default: $XDG_CONFIG_HOME/woodland/config.toml)\n"
           "  -p  new window placement: cascade (default), center, cursor, autoplace\n"
           "  --self-check  run invariant checks after every state change\n"
           "                (failures are logged, not fatal)\n"
           "  --trace       keep the last 4096 state-change decisions in a ring\n"
           "                buffer and expose them as `wctl get_trace`\n"
           "  --no-shortcut-client  do not autostart woodland-shortcutd\n"
           "                (in non-DDE sessions it normally registers the default\n"
           "                 global shortcuts; DDE sessions never start it)\n"
           "\n"
           "The debug socket path is exported as WOODLAND_IPC; query it with\n"
           "`wctl get_tree | jq` or `nc -U $WOODLAND_IPC`.\n",
           name);
}

int main(int argc, char *argv[])
{
    woodland_log_init(WLR_INFO);

    char *startup_cmd = NULL;
    char *config_path = NULL;
    bool self_check = false;
    bool trace_enabled = false;
    bool no_shortcut_client = false;
    bool placement_specified = false;
    enum woodland_placement_policy placement = WOODLAND_PLACE_CASCADE;
    enum {
        OPT_SELF_CHECK = 256,
        OPT_TRACE,
        OPT_NO_SHORTCUT_CLIENT,
    };
    static const struct option long_options[] = {
        {"self-check", no_argument, NULL, OPT_SELF_CHECK},
        {"trace", no_argument, NULL, OPT_TRACE},
        {"no-shortcut-client", no_argument, NULL, OPT_NO_SHORTCUT_CLIENT},
        {"config", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:p:c:h", long_options, NULL)) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'p':
            if (!woodland_placement_policy_parse(optarg, &placement)) {
                printf("unknown placement policy: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            placement_specified = true;
            break;
        case OPT_SELF_CHECK:
            self_check = true;
            break;
        case OPT_TRACE:
            trace_enabled = true;
            break;
        case OPT_NO_SHORTCUT_CLIENT:
            no_shortcut_client = true;
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
    server.config_path = config_path;
    server.placement_policy = placement;
    server.placement_specified = placement_specified;
    server.self_check = self_check;
    server.no_shortcut_client = no_shortcut_client;
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
