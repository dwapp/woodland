// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_XWAYLAND_OUTPUTS_H
#define WOODLAND_XWAYLAND_OUTPUTS_H

/*
 * 只给 Xwayland 看的 xdg-output global（native_resolution 的核心）。
 *
 * Xwayland 从 xdg-output 得知输出位置与大小，据此建 X 屏幕和 RandR 模式。
 * 这里单独建一个 zxdg_output_manager_v1：其中每个输出按 X 坐标下的矩形
 * （物理尺寸，见 xwayland_scale.h）发布；wlroots 自带的那个仍给其他客户端，
 * 二者经 global filter 互相隔离（woodland_xwl_outputs_advertise）。
 */

#include <stdbool.h>

struct wl_client;
struct wl_global;
struct woodland_server;
struct woodland_xwl_outputs;

/** 创建 global；失败返回 NULL。调用前 server->xwl_map.enabled 须已定。 */
struct woodland_xwl_outputs *woodland_xwl_outputs_create(struct woodland_server *server);

/** 销毁 global；已绑定的资源仅停止收事件。幂等判空由调用方负责。 */
void woodland_xwl_outputs_destroy(struct woodland_xwl_outputs *outputs);

/**
 * global filter：本 global 只给 Xwayland，wlroots 的 xdg-output global 给其他
 * 所有客户端。返回 true 表示允许 client 看到 global。非 xdg-output 的 global
 * 一律返回 true。
 */
bool woodland_xwl_outputs_advertise(const struct woodland_server *server,
                                    const struct wl_client *client,
                                    const struct wl_global *global,
                                    const struct wl_client *xwayland);

/** 按当前布局重算映射并向 Xwayland 重发有变化的输出；返回映射是否变了。 */
bool woodland_xwl_outputs_update(struct woodland_server *server);

#endif /* WOODLAND_XWAYLAND_OUTPUTS_H */
