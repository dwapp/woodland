// SPDX-License-Identifier: MIT
#ifndef WOODLAND_UTIL_LOG_H
#define WOODLAND_UTIL_LOG_H

#include <wlr/util/log.h>

/**
 * 初始化 woodland 日志系统（转发 wlr_log）。
 *
 * verbosity: 日志级别阈值（WLR_SILENT/WLR_ERROR/WLR_INFO/WLR_DEBUG）。
 */
void woodland_log_init(enum wlr_log_importance verbosity);

/** 返回当前日志级别阈值。 */
enum wlr_log_importance woodland_log_get_verbosity(void);

/** 记录一条日志（宏转发 wlr_log，保留文件名/行号前缀）。 */
#define woodland_log wlr_log

#endif
