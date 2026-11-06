// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PROTOCOL_H
#define WOODLAND_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct woodland_server;
struct woodland_workspace;
struct wlr_input_device;
struct wlr_output;

/*
 * 上游协议接线（src/protocol）。
 *
 * 这一层的职责只有"把 wlroots 的 manager 挂上 display，并把事件接到
 * 已有的窗口管理逻辑上"——不实现协议本身，也不碰渲染与几何。
 *
 * 内部按关注点分了七个文件，各自的取舍写在文件头：
 *   idle.c    空闲与抑制：idle-notify / idle-inhibit / shortcuts-inhibit
 *   output.c  输出管理：output-management / output-power-management
 *   input.c   输入与选区：relative-pointer / pointer-gestures / primary-selection
 *   window.c  窗口：xdg-activation / xdg-toplevel-tag
 *   decoration.c 窗口装饰：treeland-decoration（按窗口的 SSD 定制）
 *   background_effect.c 背景模糊：ext-background-effect-v1
 *   wallpaper.c 壁纸：treeland-wallpaper-shell / -manager / -notifier
 *   compositor_action.c shell 动作：treeland-compositor-action（工作区切换）
 *   protocol.c 只管按序建立、逆序回收，以及下面这几个转发入口。
 *
 * 按语义完备程度分档，档位决定了它是否真的改变行为：
 *
 *   语义完整：viewporter、presentation-time、primary-selection、
 *             idle-notify、idle-inhibit、keyboard-shortcuts-inhibit、
 *             output-power-management、output-management、gamma-control、
 *             drm-lease、relative-pointer、pointer-constraints、pointer-gestures、
 *             linux-drm-syncobj、xdg-activation、
 *             treeland-decoration 的边框/标题栏/圆角、
 *             ext-background-effect（capability 随 [effects] 运行期变化）
 *             —— 客户端请求会真的影响合成器行为。
 *   委派实现：treeland-wallpaper-shell / -manager / -notifier
 *             —— 配置、场景托管、播放控制都在合成器里，但**解码不在**：
 *             图片/视频由外部 treeland-wallpaper-factory 渲染（见设计规格）。
 *   部分实现：treeland-compositor-action 的 workspace_1..12 / prev / next
 *             —— 真接 woodland_workspace_switch()；其余 15..30 号动作
 *             （show-desktop、多任务、缩放、FPS、锁屏、关机菜单、用户切换）
 *             置空但可观测（trace / wctl / 日志），不假装生效。bind 的
 *             "非特权客户端"判定只做到同 uid，woodland 没有 DDE 的
 *             privileged 服务，取舍写在 compositor_action.c 文件头。
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

/**
 * 新输出的 scene 图元（scene_output + 4 棵 layer tree）建好之后调用。
 *
 * 必须在新输出的 handle_new_output 之后跑：图层树用 lower_to_bottom() 建立，
 * 会把壁纸基线顶到 layer-shell 的 background 之上。这是"壁纸永远在
 * layer-shell 下面"的唯一保证，热插拔路径（不只是启动）也要覆盖。
 */
void woodland_protocol_output_added(struct woodland_server *server,
                                    struct wlr_output *output);

/* ==================== treeland-compositor-action ==================== */

/*
 * 前三个是纯函数（不碰 display、不连客户端）：动作表、工作区名、相邻
 * 工作区都能在栈上验证，不必为了测它们起一个合成器；最后一个是只读
 * getter。协议实现与单元测试共用同一份推导，免得测试自己抄一遍 1..12
 * 的映射。
 */
enum woodland_protocol_action_class {
    WOODLAND_PROTO_ACTION_INVALID,       /* 枚举之外的值（0、>30） */
    WOODLAND_PROTO_ACTION_WORKSPACE,    /* workspace_1..12 */
    WOODLAND_PROTO_ACTION_WORKSPACE_PREV,
    WOODLAND_PROTO_ACTION_WORKSPACE_NEXT,
    WOODLAND_PROTO_ACTION_UNSUPPORTED,  /* 15..30：woodland 暂无对应状态机 */
};

enum woodland_protocol_action_class
woodland_protocol_action_classify(uint32_t action);

/** workspace_N -> 名字 "1".."12"；其它动作或缓冲不足返回 false。 */
bool woodland_protocol_action_workspace_name(uint32_t action, char *buf, size_t len);

/**
 * 当前工作区在 server->workspaces 上的前/后一个；边界不绕回（没有上一个
 * 就属于"不能执行"，协议要求忽略）。server 可以是栈上只填了 workspaces
 * 链表的假结构。
 */
struct woodland_workspace *woodland_protocol_action_adjacent_workspace(
    struct woodland_server *server, bool next);

/**
 * 最近一次 trigger 的处置结果，给 wctl get_tree 用。出参都可为 NULL；
 * 协议层没建起来时 available=false、计数为 0、last_outcome=NULL。
 */
void woodland_protocol_compositor_action_status(struct woodland_server *server,
                                                bool *available,
                                                uint32_t *trigger_count,
                                                uint32_t *last_action,
                                                const char **last_outcome);
#endif
