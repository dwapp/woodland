// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PROTOCOL_H
#define WOODLAND_PROTOCOL_H

#include <stdbool.h>

struct woodland_server;
struct wlr_input_device;
struct wlr_output;

/*
 * 上游协议接线（src/protocol）。
 *
 * 这一层的职责只有"把 wlroots 的 manager 挂上 display，并把事件接到
 * 已有的窗口管理逻辑上"——不实现协议本身，也不碰渲染与几何。
 *
 * 内部按关注点分了五个文件，各自的取舍写在文件头：
 *   idle.c    空闲与抑制：idle-notify / idle-inhibit / shortcuts-inhibit
 *   output.c  输出管理：output-management / output-power-management
 *   input.c   输入与选区：relative-pointer / pointer-gestures / primary-selection
 *   window.c  窗口：xdg-activation / xdg-toplevel-tag
 *   decoration.c 窗口装饰：treeland-decoration（按窗口的 SSD 定制）
 *   background_effect.c 背景模糊：ext-background-effect-v1
 *   protocol.c 只管按序建立、逆序回收，以及下面这几个转发入口。
 *
 * 按语义完备程度分三档，档位决定了它是否真的改变行为：
 *
 *   语义完整：viewporter、presentation-time、primary-selection、
 *             idle-notify、idle-inhibit、keyboard-shortcuts-inhibit、
 *             output-power-management、output-management、gamma-control、
 *             drm-lease、relative-pointer、pointer-constraints、pointer-gestures、
 *             linux-drm-syncobj、xdg-activation、
 *             treeland-decoration 的边框/标题栏/圆角、
 *             ext-background-effect（capability 随 [effects] 运行期变化）
 *             —— 客户端请求会真的影响合成器行为。
 *   仅记录：  xdg-toplevel-tag（存下来、经 wctl 可见，但暂不参与窗口规则）；
 *             treeland-decoration 的阴影（要一层模糊 buffer，还没做）。
 *   暂不接线：security-context（无沙箱校验时声明"我是沙箱"反而是假安全）、
 *             tearing-control（要改 scene 的 commit 路径传 tearing 位，
 *             属单独一版）、content-type（scene 不消费）、
 *             session-lock / xdg-foreign
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

/** 当前焦点 surface 是否持有生效中的快捷键抑制器（有则跳过合成器键位）。 */
bool woodland_protocol_shortcuts_inhibited(struct woodland_server *server);

/**
 * 输出集合或状态变了，让协议层补一次 output-management 配置广播。
 *
 * 布局变化会自动触发广播（协议层监听 output_layout），但有些变化不经过
 * 布局：DPMS 开关、以及 headless 输出 commit 时没有 preferred mode 可供
 * 重算布局。这些路径需要显式通知一声。
 */
void woodland_protocol_output_changed(struct woodland_server *server);


/**
 * 配置热重载之后，把新的模糊能力位广播给已连接的客户端。
 *
 * ext-background-effect 的 capability 在 bind 时发一次，客户端之后不会再问；
 * 所以 [effects] 开关变了要主动给所有已绑定的 manager 补一条（协议允许重复
 * 发送，以最后一条为准）。
 */
void woodland_protocol_refresh_background_effect(struct woodland_server *server);

/** 将 DRM 输出提议给 DRM lease 管理器 */
void woodland_protocol_offer_drm_lease(struct woodland_server *server,
                                       struct wlr_output *output);

/** 从 DRM lease 管理器撤回 DRM 输出提议 */
void woodland_protocol_withdraw_drm_lease(struct woodland_server *server,
                                          struct wlr_output *output);
#endif
