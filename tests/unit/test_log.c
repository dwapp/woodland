#include <assert.h>
#include <wlr/util/log.h>
#include "util/log.h"

int main(void) {
    woodland_log_init(WLR_DEBUG);
    assert(woodland_log_get_verbosity() == WLR_DEBUG);

    woodland_log_init(WLR_ERROR);
    assert(woodland_log_get_verbosity() == WLR_ERROR);

    /* 日志调用不崩溃、返回 0 */
    woodland_log(WLR_ERROR, "test log line %d", 42);
    return 0;
}
