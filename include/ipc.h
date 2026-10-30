// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_IPC_H
#define WOODLAND_IPC_H

#include <stdbool.h>

struct woodland_server;
struct woodland_json;

/*
 * 调试接口：unix socket 上的行分隔 JSON。
 *
 * 为什么不是自定义二进制协议、也不引 IPC 框架：
 * 这个接口的用户是调试的人和调试的 AI，`nc -U $WOODLAND_IPC` 手敲一句、
 * `wctl get_tree | jq`、`grep '"focused":true'` 全部直接可用，
 * 比任何带长度头的协议都省事。代价是没有类型校验，接受。
 *
 * 一次连接可以连发多条命令（一行一条），服务端一行一条回应。
 */

/**
 * 建立 socket 并注册到事件循环。
 *
 * wayland_socket 是 wl_display_add_socket_auto() 返回的名字（如
 * "wayland-0"），用来拼出 $XDG_RUNTIME_DIR/woodland-wayland-0.sock，
 * 并把路径导出到环境变量 WOODLAND_IPC 供客户端/脚本取用。
 *
 * 失败只记日志并返回 false：调试接口不可用不应该拖垮合成器。
 */
bool woodland_ipc_init(struct woodland_server *server,
                       const char *wayland_socket);
void woodland_ipc_finish(struct woodland_server *server);

/** socket 路径；未初始化时返回 ""。 */
const char *woodland_ipc_path(const struct woodland_server *server);

/**
 * 执行一条命令，把 JSON 响应写进 out（调用方已 init）。
 * 返回响应里的 ok 字段。
 *
 * 契约：只要 server/line/out 非空，返回时 out 里一定是一段**完整**的
 * JSON——包括拼装中途内存失败的情况（此时换成一条 ok=false 的错误响应）。
 * 传输层因此可以直接把 out 发出去，不需要再判断缓冲区是否写坏了。
 */
bool woodland_ipc_dispatch(struct woodland_server *server, const char *line,
                           struct woodland_json *out);

#endif
