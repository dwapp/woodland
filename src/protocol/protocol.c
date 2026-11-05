// SPDX-License-Identifier: GPL-2.0-only
/*
 * 上游协议接线的总装层。
 *
 * 这一层只做三件事：
 *   1. 按依赖顺序建立各协议族，失败时让 finish 逆序回收；
 *   2. 把散在别处（seat / 测试）的调用转发给对应的族；
 *   3. 持有两个"创建即完成"的全局（viewporter、presentation-time）——
 *      它们没有监听器也没有私有状态，不值得各自开一族。
 *
 * 具体的协议行为按关注点分在七个文件里：
 *   idle.c       空闲与抑制（idle-notify / idle-inhibit / shortcuts-inhibit）
 *   output.c     输出管理（output-management / output-power-management）
 *   input.c      输入与选区（relative-pointer / pointer-gestures / primary-selection）
 *   window.c     窗口（xdg-activation / xdg-toplevel-tag）
 *   decoration.c 窗口装饰（treeland-decoration：按窗口的 SSD 定制）
 *   background_effect.c 背景模糊（ext-background-effect-v1）
 *   wallpaper.c  壁纸（treeland-wallpaper-shell / -manager / -notifier）
 * 分层的原因不是"文件太大了"：这几块的状态机各不相干，混在一个文件里时
 * init 的失败回滚和 finish 的逆序拆除要横跨十几组监听器，漏一行就是悬挂
 * 指针；拆开之后每族的建立与回收在同一个文件里，加协议只动一处。
 */
#include <stdlib.h>

#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

#include "protocol_internal.h"
#include "protocol.h"
#include "woodland.h"

bool woodland_protocol_init(struct woodland_server *server) {
    /*
     * 前置条件写全一点：协议层要挂壁纸族，而壁纸族监听的是
     * cursor / seat / output_layout / workspace（server->active_workspace
     * 就是"workspace 层已经建好"的标志）。少了任何一个，监听器会挂到
     * 没初始化的 wl_signal 上——那是段错误，不是启动失败。
     */
    if (!server->cursor || !server->seat || !server->output_layout ||
        !server->active_workspace) {
        wlr_log(WLR_ERROR,
                "protocol layer needs cursor/seat/output/workspace first");
        return false;
    }

    struct woodland_proto *proto = calloc(1, sizeof(*proto));
    if (!proto) {
        wlr_log(WLR_ERROR, "cannot allocate protocol state");
        return false;
    }
    proto->server = server;
    /* 先挂上去：后面任何一步失败，finish 都能顺着它回收 */
    server->proto = proto;

    /* 无状态的一组：客户端能不能用的分水岭在这里 */
    proto->viewporter = wlr_viewporter_create(server->display);
    if (!proto->viewporter) {
        wlr_log(WLR_ERROR, "failed to create viewporter");
        return false;
    }

    /* 帧时序：scene 在把 surface 提交到输出时自己会上报 feedback
     * （types/scene/surface.c），所以这里创建完就够，无需自己算 */
    proto->presentation = wlr_presentation_create(server->display,
                                                  server->backend, 1);
    if (!proto->presentation) {
        wlr_log(WLR_ERROR, "failed to create presentation-time");
        return false;
    }

    /* linux-drm-syncobj-v1：显式同步。同步点与释放围栏由 wlr_scene 在
     * commit 与 output_sample 时原生消费（types/scene/surface.c），这里
     * 创建即完成。门控条件与 labwc 对齐：硬件与后端都支持 timeline 且
     * 能拿到 DRM fd 才暴露；不支持时 create 本身也会因 eventfd 检查失败
     * 而返回 NULL，不是错误，静默跳过即可。 */
    int drm_fd = wlr_renderer_get_drm_fd(server->renderer);
    if (server->renderer->features.timeline && server->backend->features.timeline &&
            drm_fd >= 0) {
        proto->linux_drm_syncobj =
            wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd);
        if (proto->linux_drm_syncobj) {
            wlr_log(WLR_INFO, "linux-drm-syncobj-v1 enabled (explicit sync)");
        }
    }

    /*
     * 顺序有讲究：idle 要挂 seat 的键盘焦点信号，output 要挂 output_layout，
     * input 要挂 cursor 与 seat，window 谁都不依赖——按"依赖少的多"排在
     * 前面，失败时只需回收已经建起来的。
     */
    if (!woodland_proto_idle_init(proto)) {
        return false;
    }
    if (!woodland_proto_output_init(proto)) {
        return false;
    }
    if (!woodland_proto_input_init(proto)) {
        return false;
    }
    if (!woodland_proto_window_init(proto)) {
        return false;
    }
    if (!woodland_proto_drm_lease_init(proto)) {
        return false;
    }
    if (!woodland_proto_decoration_init(proto)) {
        return false;
    }
    if (!woodland_proto_bg_effect_init(proto)) {
        return false;
    }
    /* 壁纸放在最后：它要 server->scene、server->outputs、server->session_lock
     * 都已就位 */
    if (!woodland_proto_wallpaper_init(proto)) {
        return false;
    }

    /* 已有输出可能早于本模块建好，补一次广播让客户端看到初始状态 */
    woodland_proto_output_broadcast(proto);
    return true;
}

void woodland_protocol_refresh_background_effect(struct woodland_server *server)
{
    if (server && server->proto) {
        woodland_proto_bg_effect_refresh(server->proto);
    }
}

void woodland_protocol_finish(struct woodland_server *server)
{
    struct woodland_proto *proto = server->proto;
    if (!proto) {
        return;
    }

    /* 逆序拆：与 init 对称，后建的先摘 */
    woodland_proto_wallpaper_finish(proto);
    woodland_proto_bg_effect_finish(proto);
    woodland_proto_decoration_finish(proto);
    woodland_proto_drm_lease_finish(proto);
    woodland_proto_window_finish(proto);
    woodland_proto_input_finish(proto);
    woodland_proto_output_finish(proto);
    woodland_proto_idle_finish(proto);

    /* viewporter / presentation 归 display 所有，这里不用管 */
    free(proto);
    server->proto = NULL;
}

void woodland_protocol_attach_input(struct woodland_server *server,
                                    struct wlr_input_device *device) {
    woodland_proto_input_attach_device(server->proto, device);
}

void woodland_protocol_notify_activity(struct woodland_server *server) {
    woodland_proto_idle_notify_activity(server->proto);
}

bool woodland_protocol_shortcuts_inhibited(struct woodland_server *server) {
    return woodland_proto_idle_shortcuts_inhibited(server->proto);
}

void woodland_protocol_output_changed(struct woodland_server *server) {
    if (!server) {
        return;
    }
    woodland_proto_output_broadcast(server->proto);
}

void woodland_protocol_offer_drm_lease(struct woodland_server *server,
                                       struct wlr_output *output) {
    if (!server || !server->proto) {
        return;
    }
    woodland_proto_drm_lease_offer_output(server->proto, output);
}

void woodland_protocol_withdraw_drm_lease(struct woodland_server *server,
                                          struct wlr_output *output) {
    if (!server || !server->proto) {
        return;
    }
    woodland_proto_drm_lease_withdraw_output(server->proto, output);
}

void woodland_protocol_output_added(struct woodland_server *server,
                                    struct wlr_output *output) {
    if (!server || !server->proto) {
        return;
    }
    woodland_proto_wallpaper_output_added(server->proto, output);
}
