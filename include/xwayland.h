// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_XWAYLAND_H
#define WOODLAND_XWAYLAND_H

#include <stdbool.h>

struct woodland_server;

/**
 * 初始化 Xwayland 支持。
 *
 * 创建 lazy 模式的 wlr_xwayland（首个 X 客户端连接时才真正拉起 Xwayland 进程），
 * 接上 seat，并监听 new_surface 生成 woodland_view（override-redirect 的走
 * unmanaged 路径：无装饰、不参与焦点和工作区）。
 *
 * 必须在 seat / shell 就绪之后调用（server_init 中位于 shell_init 之后）。
 */
bool woodland_xwayland_init(struct woodland_server *server);

/** 摘掉监听器（display 销毁会连带释放 wlr_xwayland）。 */
void woodland_xwayland_finish(struct woodland_server *server);

#endif /* WOODLAND_XWAYLAND_H */
