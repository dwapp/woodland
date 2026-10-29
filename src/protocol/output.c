// SPDX-License-Identifier: GPL-2.0-only
/*
 * 输出管理族：output-management-v1（wdisplays / kanshi 读写显示器配置）
 * 与 output-power-management-v1（DPMS 开关）。
 *
 * 两个协议是同一套状态机的前后门：一个改配置，一个改电源，都要把结果
 * 写回 wlr_output_layout 并保证客户端看到的配置是真实生效的那份。
 *
 * 位置不在 wlr_output_state 里，得单独写回布局——这是本文件最容易踩
 * 的一处，详见 output_apply_config()。
 */
#include <stdlib.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/util/log.h>

#include "output.h"
#include "protocol_internal.h"
#include "woodland.h"

struct woodland_proto_output {
    struct woodland_proto *owner;
    struct woodland_server *server;

    struct wlr_output_manager_v1 *manager;
    struct wl_listener apply;
    struct wl_listener test;

    struct wlr_output_power_manager_v1 *power;
    struct wl_listener power_set_mode;

    struct wl_listener layout_change;

    /* apply 期间压住配置广播，否则客户端分不清哪份是对自己请求的回应 */
    bool applying;
};

/*
 * 配置广播：把当前所有输出的状态打包成一份 configuration 发给客户端
 * （kanshi / wdisplays 这类工具靠它知道"现在是什么样"）。
 *
 * 必须在输出集合或布局变化后重发，否则客户端看到的还是上次的快照。
 * apply 期间不能发——那时客户端正在等自己那份 config 的结果，中间插
 * 一份新配置会让它以为配置被拒。
 */
void woodland_proto_output_broadcast(struct woodland_proto *proto) {
    struct woodland_proto_output *output = proto ? proto->output : NULL;
    if (!output || !output->manager || output->applying) {
        return;
    }
    struct woodland_server *server = output->server;
    if (!server->output_layout) {
        return;
    }

    struct wlr_output_configuration_v1 *config =
        wlr_output_configuration_v1_create();
    if (!config) {
        return;
    }
    struct woodland_output *out;
    wl_list_for_each(out, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, out->wlr_output);
        if (!head) {
            continue;
        }
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }
    wlr_output_manager_v1_set_configuration(output->manager, config);
}

static void output_handle_layout_change(struct wl_listener *listener,
                                        void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, layout_change);
    woodland_proto_output_broadcast(output->owner);
    (void)data;
}

/* apply 与 test 的差别只有一条：test 不许落盘。 */
static bool output_apply_config(struct woodland_proto_output *output,
                                struct wlr_output_configuration_v1 *config,
                                bool commit) {
    struct woodland_server *server = output->server;
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output *wlr_output = head->state.output;

        if (commit) {
            struct wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_head_v1_state_apply(&head->state, &state);
            if (!wlr_output_commit_state(wlr_output, &state)) {
                wlr_log(WLR_ERROR, "output-management: commit failed for %s",
                        wlr_output->name);
                ok = false;
            }
            wlr_output_state_finish(&state);
        }

        /* 位置不在 output_state 里，得单独写回布局；
         * 禁用时从布局摘除，否则 workarea 会留一块黑洞。 */
        if (head->state.enabled) {
            wlr_output_layout_add(server->output_layout, wlr_output,
                                  head->state.x, head->state.y);
        } else {
            wlr_output_layout_remove(server->output_layout, wlr_output);
        }
    }
    return ok;
}

static void output_handle_apply(struct wl_listener *listener, void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, apply);
    struct wlr_output_configuration_v1 *config = data;

    /* 压住广播：apply 过程中布局变化会触发 layout-change，
     * 那时插一份新配置会让客户端分不清哪份是它要的结果 */
    output->applying = true;
    bool ok = output_apply_config(output, config, true);
    output->applying = false;

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    /* 落盘后再补一次广播，客户端拿到的才是真实生效的状态 */
    woodland_proto_output_broadcast(output->owner);
}

static void output_handle_test(struct wl_listener *listener, void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, test);
    struct wlr_output_configuration_v1 *config = data;

    /* 没有副作用可验：把状态翻译一遍不报错就算通过 */
    bool ok = output_apply_config(output, config, false);
    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
}

static void output_handle_power_set_mode(struct wl_listener *listener,
                                         void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, power_set_mode);
    struct wlr_output_power_v1_set_mode_event *event = data;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state,
        event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    if (!wlr_output_commit_state(event->output, &state)) {
        wlr_log(WLR_ERROR, "output-power: failed to commit state for %s",
                event->output->name);
    }
    wlr_output_state_finish(&state);
}

bool woodland_proto_output_init(struct woodland_proto *proto) {
    struct woodland_server *server = proto->server;

    struct woodland_proto_output *output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "cannot allocate output protocol state");
        return false;
    }
    output->owner = proto;
    output->server = server;
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->output = output;

    /* 输出电源：DPMS 开关 */
    output->power = wlr_output_power_manager_v1_create(server->display);
    if (!output->power) {
        wlr_log(WLR_ERROR, "failed to create output-power-management");
        return false;
    }
    output->power_set_mode.notify = output_handle_power_set_mode;
    wl_signal_add(&output->power->events.set_mode, &output->power_set_mode);

    /* 输出配置：wdisplays / kanshi 这类工具靠它读写显示器设置 */
    output->manager = wlr_output_manager_v1_create(server->display);
    if (!output->manager) {
        wlr_log(WLR_ERROR, "failed to create output-management");
        return false;
    }
    output->apply.notify = output_handle_apply;
    wl_signal_add(&output->manager->events.apply, &output->apply);
    output->test.notify = output_handle_test;
    wl_signal_add(&output->manager->events.test, &output->test);
    output->layout_change.notify = output_handle_layout_change;
    wl_signal_add(&server->output_layout->events.change, &output->layout_change);

    return true;
}

void woodland_proto_output_finish(struct woodland_proto *proto) {
    struct woodland_proto_output *output = proto->output;
    if (!output) {
        return;
    }
    if (output->power_set_mode.link.prev) {
        wl_list_remove(&output->power_set_mode.link);
    }
    if (output->apply.link.prev) {
        wl_list_remove(&output->apply.link);
    }
    if (output->test.link.prev) {
        wl_list_remove(&output->test.link);
    }
    if (output->layout_change.link.prev) {
        wl_list_remove(&output->layout_change.link);
    }
    free(output);
    proto->output = NULL;
}
