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

#include <wlr/backend.h>
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

/*
 * 一份 config 的 heads → backend 级 state 数组。
 *
 * 走 backend 而不是逐个 wlr_output_commit_state()，是因为 DRM 有 backend
 * 级约束（同一 CRTC 资源、带宽预算）：逐输出提交时每个输出单独看都合理，
 * 合起来不一定能成立。wlr_backend_test()/commit() 会把整组一起看。
 *
 * 调用方负责 states_finish()（每个 base 是 init 过的，含 buffer 引用）。
 */
static struct wlr_backend_output_state *config_build_states(
        struct wlr_output_configuration_v1 *config, size_t *len) {
    return wlr_output_configuration_v1_build_state(config, len);
}

static void states_finish(struct wlr_backend_output_state *states,
                          size_t len) {
    for (size_t i = 0; i < len; i++) {
        wlr_output_state_finish(&states[i].base);
    }
    free(states);
}

/* 布局不进 output_state：位置是合成器自己的概念，wlr_output_configuration_head_v1
 * 的 x/y 只在合成器侧有效，所以 commit 成功之后单独写回。 */
static void apply_layout(struct woodland_server *server,
                         struct wlr_output_configuration_v1 *config) {
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        if (head->state.enabled) {
            wlr_output_layout_add(server->output_layout, head->state.output,
                                  head->state.x, head->state.y);
        } else {
            /* 禁用要从布局摘除，否则 workarea 会留一块黑洞 */
            wlr_output_layout_remove(server->output_layout, head->state.output);
        }
    }
}

/*
 * test 与 apply 的唯一差别：test 不许产生任何副作用。
 *
 * 所以 test 路径完全不碰 output_layout —— 早先的实现把"翻译成 state"和
 * "写回布局"放在同一个循环里，`output_apply_config(commit=false)` 于是
 * 真的挪动了输出位置、真的把输出从布局里摘掉（但 wlr_output->enabled 没变，
 * 状态就此不一致），而且从不调用 wlr_output_test_state()，任何配置都报成功。
 * 现在 test 只做 backend_test，布局一个字都不动。
 */
static void output_handle_test(struct wl_listener *listener, void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, test);
    struct wlr_output_configuration_v1 *config = data;

    size_t len = 0;
    struct wlr_backend_output_state *states =
        config_build_states(config, &len);
    /* 空配置（没有 head）当成无条件成功：calloc(0) 是否返回 NULL 依实现
     * 而定，别让它决定一次 test 的结论 */
    bool ok = len == 0
        || (states != NULL
            && wlr_backend_test(output->server->backend, states, len));
    states_finish(states, len);

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
}

/*
 * apply：先整体 test、再整体 commit、最后才写布局。
 *
 * 顺序是刻意的。协议在 failed 事件里要求"撤销本次 apply 的修改"，而逐
 * 输出 commit 一旦中途失败，前面的已经生效、后面的没有，撤销要靠给每个
 * 输出拍快照再逐一恢复（mode、scale、buffer 都要），代价和出错面都很大。
 * 把 test 提到前面，让"不支持"在零副作用时就被拦下；真正 commit 时
 * backend 已经确认过整组可行，部分失败只剩驱动侧竞态。
 *
 * 布局放在 commit 成功之后：commit 失败时布局必须保持原样，否则会出现
 * "布局说输出在 (0,0) 且启用，wlr_output 却说没启用"的错位。
 */
static void output_handle_apply(struct wl_listener *listener, void *data) {
    struct woodland_proto_output *output =
        wl_container_of(listener, output, apply);
    struct wlr_output_configuration_v1 *config = data;
    struct woodland_server *server = output->server;

    size_t len = 0;
    struct wlr_backend_output_state *states =
        config_build_states(config, &len);

    bool ok = len == 0;
    if (len > 0 && states != NULL) {
        /* 压住广播：test/commit 期间布局可能变化，中间插一份新配置
         * 会让客户端分不清哪份是它要的结果 */
        output->applying = true;
        ok = wlr_backend_test(server->backend, states, len)
             && wlr_backend_commit(server->backend, states, len);
        output->applying = false;
    }
    if (!ok) {
        wlr_log(WLR_ERROR, "output-management: apply rejected");
    } else if (len > 0) {
        apply_layout(server, config);
    }
    states_finish(states, len);

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    /* 落盘后再补一次广播，客户端拿到的才是真实生效的状态 */
    woodland_proto_output_broadcast(output->owner);
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
        /* TODO: 协议要求失败时回 failed 事件，但 wlroots 的 set_mode 信号
         * 只带 wlr_output，拿不到 wlr_output_power_v1 对象，没法回。
         * 需要自己跟踪 power 对象（像 idle 那样挂 destroy 监听）才能补上，
         * 暂时只记日志——别把这里当成已完整实现的反馈路径。 */
        wlr_log(WLR_ERROR, "output-power: failed to commit state for %s",
                event->output->name);
    } else {
        /* DPMS 只改 enabled，不触发 layout change，所以布局驱动的广播
         * 不会发；不主动补一次，output-management 客户端看到的 enabled
         * 和 serial 会一直停在旧值 */
        woodland_proto_output_broadcast(output->owner);
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
