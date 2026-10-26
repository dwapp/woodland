// SPDX-License-Identifier: GPL-2.0-only
#include "util/log.h"

static enum wlr_log_importance verbosity = WLR_SILENT;

void woodland_log_init(enum wlr_log_importance v) {
    verbosity = v;
    wlr_log_init(v, NULL);
}

enum wlr_log_importance woodland_log_get_verbosity(void) {
    return verbosity;
}
