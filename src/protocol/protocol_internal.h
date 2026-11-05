// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PROTOCOL_INTERNAL_H
#define WOODLAND_PROTOCOL_INTERNAL_H

#include <stdbool.h>

struct woodland_server;
struct wlr_input_device;
struct woodland_proto_idle;
struct woodland_proto_output;
struct woodland_proto_input;
struct woodland_proto_window;
struct woodland_proto_drm_lease;
struct woodland_proto_decoration;
struct woodland_proto_bg_effect;
struct wlr_output;
struct wlr_viewporter;
struct wlr_presentation;
struct wlr_linux_drm_syncobj_manager_v1;

/*
 * 协议层内部聚合状态（只在 src/protocol 内部可见）。
 *
 * 每个协议族一个不透明指针，结构体定义留在各自的 .c 里：编排层只负责
 * 按序建立、逆序回收，不需要知道某个族持有哪些监听器与私有状态。
 *
 * 这样加一个协议只动一个文件——不用往 woodland.h 里加字段，也不会因为
 * 漏掉 finish 里的一行 wl_list_remove 而留下悬挂监听器。
 */
struct woodland_proto {
    struct woodland_server *server;

    /*
     * 无状态的全局：创建即完成，没有监听器也没有生命周期需要跟踪，
     * 所以不值得为它们各自开一族，就近挂在编排层。
     */
    struct wlr_viewporter *viewporter;
    struct wlr_presentation *presentation;
    struct wlr_linux_drm_syncobj_manager_v1 *linux_drm_syncobj;

    /* 有状态的五族，混在一起会互相拖累得看不出边界 */
    struct woodland_proto_idle *idle;      /* idle.c   */
    struct woodland_proto_output *output;  /* output.c */
    struct woodland_proto_input *input;    /* input.c  */
    struct woodland_proto_window *window;  /* window.c */
    struct woodland_proto_drm_lease *drm_lease;   /* drm_lease.c */
    struct woodland_proto_decoration *decoration; /* decoration.c */
    struct woodland_proto_bg_effect *bg_effect;   /* background_effect.c */
};

/* 空闲与抑制族（idle.c）：idle-notify / idle-inhibit / keyboard-shortcuts-inhibit */
bool woodland_proto_idle_init(struct woodland_proto *proto);
void woodland_proto_idle_finish(struct woodland_proto *proto);
void woodland_proto_idle_notify_activity(struct woodland_proto *proto);
bool woodland_proto_idle_shortcuts_inhibited(struct woodland_proto *proto);

/* 输出管理族（output.c）：output-management / output-power-management */
bool woodland_proto_output_init(struct woodland_proto *proto);
void woodland_proto_output_finish(struct woodland_proto *proto);
/** 把当前输出集合打包成 configuration 广播给客户端。 */
void woodland_proto_output_broadcast(struct woodland_proto *proto);

/* 输入与选区族（input.c）：relative-pointer / pointer-gestures / primary-selection */
bool woodland_proto_input_init(struct woodland_proto *proto);
void woodland_proto_input_finish(struct woodland_proto *proto);
void woodland_proto_input_attach_device(struct woodland_proto *proto,
                                        struct wlr_input_device *device);

/* 窗口族（window.c）：xdg-activation / xdg-toplevel-tag */
bool woodland_proto_window_init(struct woodland_proto *proto);
void woodland_proto_window_finish(struct woodland_proto *proto);

/* DRM 租借族（drm_lease.c）：wp_drm_lease_device_v1 */
bool woodland_proto_drm_lease_init(struct woodland_proto *proto);
void woodland_proto_drm_lease_finish(struct woodland_proto *proto);
void woodland_proto_drm_lease_offer_output(struct woodland_proto *proto,
                                           struct wlr_output *output);
void woodland_proto_drm_lease_withdraw_output(struct woodland_proto *proto,
                                              struct wlr_output *output);

/* 装饰族（decoration.c）：treeland-decoration（按窗口的 SSD 定制） */
bool woodland_proto_decoration_init(struct woodland_proto *proto);
void woodland_proto_decoration_finish(struct woodland_proto *proto);

/* 背景效果族（background_effect.c）：ext-background-effect-v1 */
bool woodland_proto_bg_effect_init(struct woodland_proto *proto);
void woodland_proto_bg_effect_finish(struct woodland_proto *proto);
void woodland_proto_bg_effect_refresh(struct woodland_proto *proto);

#endif
