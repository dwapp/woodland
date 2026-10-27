// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_CAPTURE_H
#define WOODLAND_CAPTURE_H

#include <stdbool.h>

struct woodland_server;
struct woodland_view;

/**
 * 创建截图/录屏协议：
 * - zwlr_screencopy_manager_v1（grim 等老客户端用）
 * - ext_image_copy_capture_manager_v1（新的截图协议本体）
 * - ext_output_image_capture_source_manager_v1（按输出取源）
 * - ext_foreign_toplevel_image_capture_source_manager_v1（按窗口取源）
 *
 * 只有「按窗口取源」需要合成器参与：客户端请求某个 toplevel 的源时，
 * 由 handle_toplevel_capture_request() 建场景节点捕获源并 accept。
 */
bool woodland_capture_init(struct woodland_server *server);

/** 注销监听器；global 随 display 销毁。 */
void woodland_capture_finish(struct woodland_server *server);

/** 视图销毁时调用：断开捕获源监听（源本身随 scene 节点释放）。 */
void woodland_capture_view_finish(struct woodland_view *view);

#endif
