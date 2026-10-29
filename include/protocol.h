// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PROTOCOL_H
#define WOODLAND_PROTOCOL_H

#include <stdbool.h>

struct woodland_server;
struct wlr_input_device;
struct woodland_view;

/*
 * 上游协议接线（src/protocol）。
 *
 * 这一层的职责只有"把 wlroots 的 manager 挂上 display，并把事件接到
 * 已有的窗口管理逻辑上"——不实现协议本身，也不碰渲染与几何。
 *
 * 分成三档，档位决定了它是否真的改变行为：
 *
 *   语义完整：viewporter、presentation-time、primary-selection、
 *             idle-notify、idle-inhibit、keyboard-shortcuts-inhibit、
 *             output-power-management、output-management、
 *             relative-pointer、pointer-gestures、xdg-activation
 *             —— 客户端请求会真的影响合成器行为。
 *   仅记录：  xdg-toplevel-tag（存下来、经 wctl 可见，但暂不参与窗口规则）。
 *   暂不接线：security-context（无沙箱校验时声明"我是沙箱"反而是假安全）、
 *             tearing-control（要改 scene 的 commit 路径传 tearing 位，
 *             属单独一版）、content-type（scene 不消费）、
 *             session-lock / pointer-constraints / xdg-foreign / drm-lease
 *             （各自需要一套独立状态机，见里程碑文档）。
 */
bool woodland_protocol_init(struct woodland_server *server);
void woodland_protocol_finish(struct woodland_server *server);

/**
 * 把新出现的输入设备交给协议层：手势协议要按设备挂监听。
 *
 * 由 seat 在设备接入时调用（协议层不该自己去抢 backend 的 new_input）。
 */
void woodland_protocol_attach_input(struct woodland_server *server,
                                    struct wlr_input_device *device);

/** 有输入事件时调用，用来给 idle-notify 续命。 */
void woodland_protocol_notify_activity(struct woodland_server *server);

/**
 * 焦点变化后同步 keyboard-shortcuts-inhibit 的激活态。
 *
 * 协议规定：抑制器只在它所属 surface 拿到键盘焦点时生效。调用点只有
 * 协议层自己（挂在 seat 的 keyboard focus_change 上），对外公开是为了
 * 让将来引入别处焦点来源时有统一入口。
 */
void woodland_protocol_sync_shortcuts(struct woodland_server *server);

/** 当前焦点 surface 是否持有生效中的快捷键抑制器（有则跳过合成器键位）。 */
bool woodland_protocol_shortcuts_inhibited(struct woodland_server *server);

#endif
