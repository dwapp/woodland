// SPDX-License-Identifier: GPL-2.0-only
/*
 * 上游协议接线（见 include/protocol.h 里对三档语义的说明）。
 *
 * 为什么单独成模块，而不是继续往 server.c 里堆：
 *   - server.c 管的是"生命周期骨架"，十几个协议塞进去会把真正重要的
 *     初始化顺序淹掉；
 *   - 其中五六个需要自己持有监听器与私有状态（idle 抑制器、手势设备、
 *     输出配置），只有放在一起才能保证 init/finish 严格对称。
 *
 * 这一层不实现协议本身，也不碰渲染和几何：所有行为都落到已有的
 * focus / output / seat 逻辑上。
 */
#include <stdlib.h>

#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>

#include "focus.h"
#include "output.h"
#include "protocol.h"
#include "view.h"
#include "woodland.h"

/* ==================== idle-notify / idle-inhibit ==================== */

/*
 * 抑制器由 wlroots 持有，但它只提供"新增"事件，不提供"全部清空"。
 * 每来一个抑制器挂一个销毁监听，任一销毁就重算一次聚合状态——
 * 否则最后一个抑制器消失后 idle 会一直停着。
 */
struct woodland_idle_inhibitor {
    struct woodland_server *server;
    struct wl_list link;                  // woodland_server.idle_inhibitors
    struct wl_listener destroy;
};

static void idle_refresh(struct woodland_server *server) {
    if (!server->idle_notifier || !server->idle_inhibit) {
        return;
    }
    bool inhibited = !wl_list_empty(&server->idle_inhibit->inhibitors);
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
}

static void idle_inhibitor_handle_destroy(struct wl_listener *listener,
                                          void *data) {
    struct woodland_idle_inhibitor *inhibitor =
        wl_container_of(listener, inhibitor, destroy);
    struct woodland_server *server = inhibitor->server;
    wl_list_remove(&inhibitor->destroy.link);
    wl_list_remove(&inhibitor->link);
    free(inhibitor);
    idle_refresh(server);
    (void)data;
}

static void idle_handle_new_inhibitor(struct wl_listener *listener,
                                      void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;

    struct woodland_idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor) {
        /* 记不住销毁也没关系：idle 只是停不下来，不影响可用性 */
        wlr_log(WLR_ERROR, "cannot track idle inhibitor, idle stays inhibited");
        idle_refresh(server);
        return;
    }
    inhibitor->server = server;
    inhibitor->destroy.notify = idle_inhibitor_handle_destroy;
    wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
    wl_list_insert(&server->idle_inhibitors, &inhibitor->link);
    idle_refresh(server);
}

void woodland_protocol_notify_activity(struct woodland_server *server) {
    if (server->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier,
                                             server->seat);
    }
}

/* ==================== keyboard-shortcuts-inhibit ==================== */

/*
 * 协议规定：抑制器只在它所属 surface 拿到键盘焦点时生效。所以激活态
 * 不能只按"有没有人声明"来定，必须跟着焦点走——焦点从 A 换到 B 时，
 * A 的抑制器要立刻失效，否则 B 里的 Alt+Tab 会被 A 吞掉。
 */
void woodland_protocol_sync_shortcuts(struct woodland_server *server) {
    if (!server->shortcuts_inhibit) {
        return;
    }
    struct wlr_surface *focused = server->seat
        ? server->seat->keyboard_state.focused_surface : NULL;

    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &server->shortcuts_inhibit->inhibitors, link) {
        bool want = focused && inhibitor->surface == focused;
        if (want && !inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
        } else if (!want && inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inhibitor);
        }
    }
}

bool woodland_protocol_shortcuts_inhibited(struct woodland_server *server) {
    if (!server->shortcuts_inhibit || !server->seat) {
        return false;
    }
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) {
        return false;
    }
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &server->shortcuts_inhibit->inhibitors, link) {
        if (inhibitor->active && inhibitor->surface == focused) {
            return true;
        }
    }
    return false;
}

static void shortcuts_handle_new_inhibitor(struct wl_listener *listener,
                                           void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_shortcuts_inhibitor);
    /* 新声明的不一定属于当前焦点 surface：交给统一的同步逻辑判断 */
    woodland_protocol_sync_shortcuts(server);
    (void)data;
}

/*
 * 焦点变化是抑制器唯一的失效时机，所以挂在 seat 的 keyboard focus_change
 * 上，而不是在 focus.c 的每个出口各插一次调用——键盘焦点不止由 focus.c
 * 下发（layer.c 也直接 notify_enter），漏一处就会留下"旧窗口还在吞键"。
 */
static void shortcuts_handle_focus_change(struct wl_listener *listener,
                                          void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, shortcuts_focus_change);
    woodland_protocol_sync_shortcuts(server);
    (void)data;
}

/* ==================== output-power-management ==================== */

static void output_power_handle_set_mode(struct wl_listener *listener,
                                         void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, output_power_set_mode);
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
    (void)server;
}

/* ==================== output-management-v1 ==================== */

/*
 * 配置广播：把当前所有输出的状态打包成一份 configuration 发给客户端
 * （kanshi / wdisplays 这类工具靠它知道"现在是什么样"）。
 *
 * 必须在输出集合或布局变化后重发，否则客户端看到的还是上次的快照。
 * apply 期间不能发——那时客户端正在等自己那份 config 的结果，中间插
 * 一份新配置会让它以为配置被拒。
 */
static void output_config_send(struct woodland_server *server) {
    if (!server->output_manager || !server->output_layout
        || server->output_applying) {
        return;
    }
    struct wlr_output_configuration_v1 *config =
        wlr_output_configuration_v1_create();
    if (!config) {
        return;
    }
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, output->wlr_output);
        if (!head) {
            continue;
        }
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, output->wlr_output,
                                  &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }
    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void output_layout_handle_change(struct wl_listener *listener,
                                        void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, output_layout_change);
    output_config_send(server);
    (void)data;
}

/* apply 与 test 的差别只有一条：test 不许落盘。 */
static bool output_apply_config(struct woodland_server *server,
                                struct wlr_output_configuration_v1 *config,
                                bool commit) {
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output *output = head->state.output;

        if (commit) {
            struct wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_head_v1_state_apply(&head->state, &state);
            if (!wlr_output_commit_state(output, &state)) {
                wlr_log(WLR_ERROR, "output-management: commit failed for %s",
                        output->name);
                ok = false;
            }
            wlr_output_state_finish(&state);
        }

        /* 位置不在 output_state 里，得单独写回布局；
         * 禁用时从布局摘除，否则 workarea 会留一块黑洞。 */
        if (head->state.enabled) {
            wlr_output_layout_add(server->output_layout, output,
                                  head->state.x, head->state.y);
        } else {
            wlr_output_layout_remove(server->output_layout, output);
        }
    }
    return ok;
}

static void output_manager_handle_apply(struct wl_listener *listener,
                                        void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, output_manager_apply);
    struct wlr_output_configuration_v1 *config = data;

    /* 压住广播：apply 过程中布局变化会触发 layout-change，
     * 那时插一份新配置会让客户端分不清哪份是它要的结果 */
    server->output_applying = true;
    bool ok = output_apply_config(server, config, true);
    server->output_applying = false;

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    /* 落盘后再补一次广播，客户端拿到的才是真实生效的状态 */
    output_config_send(server);
}

static void output_manager_handle_test(struct wl_listener *listener,
                                       void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, output_manager_test);
    struct wlr_output_configuration_v1 *config = data;

    /* 没有副作用可验：把状态翻译一遍不报错就算通过 */
    bool ok = output_apply_config(server, config, false);
    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);
}

/* ==================== relative-pointer / 活动通知 ==================== */

/*
 * 相对指针与 idle 活动都靠光标事件驱动，所以监听器直接挂在 protocol
 * 这一层：shell.c 里的光标回调已经够长了，不该再往里塞协议细节。
 */
static void proto_cursor_motion(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, proto_cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    woodland_protocol_notify_activity(server);
    if (server->relative_pointer && server->seat) {
        /* 协议要微秒；wlr 事件给的是毫秒 */
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer, server->seat,
            (uint64_t)event->time_msec * 1000,
            event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
    }
}

static void proto_cursor_motion_absolute(struct wl_listener *listener,
                                         void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, proto_cursor_motion_absolute);
    woodland_protocol_notify_activity(server);
    (void)data;
}

static void proto_cursor_button(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, proto_cursor_button);
    woodland_protocol_notify_activity(server);
    (void)data;
}

static void proto_cursor_axis(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, proto_cursor_axis);
    woodland_protocol_notify_activity(server);
    (void)data;
}

/* ==================== pointer-gestures ==================== */

/*
 * 手势是 per-device 的（wlr_pointer 上挂信号），所以要为每个指针设备
 * 持有一组监听器。设备销毁时跟着释放，finish 时兜底清理剩下的。
 */
struct woodland_gesture_pointer {
    struct woodland_server *server;
    struct wl_list link;                  // woodland_server.gesture_pointers
    struct wl_listener destroy;
    struct wl_listener swipe_begin;
    struct wl_listener swipe_update;
    struct wl_listener swipe_end;
    struct wl_listener pinch_begin;
    struct wl_listener pinch_update;
    struct wl_listener pinch_end;
    struct wl_listener hold_begin;
    struct wl_listener hold_end;
};

static void gesture_swipe_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_begin);
    struct wlr_pointer_swipe_begin_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_begin(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_swipe_update(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_update);
    struct wlr_pointer_swipe_update_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_update(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->dx, event->dy);
    }
}

static void gesture_swipe_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_end);
    struct wlr_pointer_swipe_end_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_end(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->cancelled);
    }
}

static void gesture_pinch_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_begin);
    struct wlr_pointer_pinch_begin_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_pinch_begin(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_pinch_update(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_update);
    struct wlr_pointer_pinch_update_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_pinch_update(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->dx, event->dy,
            event->scale, event->rotation);
    }
}

static void gesture_pinch_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_end);
    struct wlr_pointer_pinch_end_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_pinch_end(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->cancelled);
    }
}

static void gesture_hold_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, hold_begin);
    struct wlr_pointer_hold_begin_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_hold_begin(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_hold_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, hold_end);
    struct wlr_pointer_hold_end_event *event = data;
    if (gp->server->pointer_gestures) {
        wlr_pointer_gestures_v1_send_hold_end(gp->server->pointer_gestures,
            gp->server->seat, event->time_msec, event->cancelled);
    }
}

static void gesture_pointer_destroy(struct woodland_gesture_pointer *gp) {
    wl_list_remove(&gp->destroy.link);
    wl_list_remove(&gp->swipe_begin.link);
    wl_list_remove(&gp->swipe_update.link);
    wl_list_remove(&gp->swipe_end.link);
    wl_list_remove(&gp->pinch_begin.link);
    wl_list_remove(&gp->pinch_update.link);
    wl_list_remove(&gp->pinch_end.link);
    wl_list_remove(&gp->hold_begin.link);
    wl_list_remove(&gp->hold_end.link);
    wl_list_remove(&gp->link);
    free(gp);
}

static void gesture_handle_destroy(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, destroy);
    gesture_pointer_destroy(gp);
    (void)data;
}

void woodland_protocol_attach_input(struct woodland_server *server,
                                    struct wlr_input_device *device) {
    if (!server->pointer_gestures) {
        return;
    }
    struct wlr_pointer *pointer = wlr_pointer_from_input_device(device);
    if (!pointer) {
        return;
    }

    struct woodland_gesture_pointer *gp = calloc(1, sizeof(*gp));
    if (!gp) {
        wlr_log(WLR_ERROR, "cannot forward gestures: out of memory");
        return;
    }
    gp->server = server;

    gp->swipe_begin.notify = gesture_swipe_begin;
    gp->swipe_update.notify = gesture_swipe_update;
    gp->swipe_end.notify = gesture_swipe_end;
    gp->pinch_begin.notify = gesture_pinch_begin;
    gp->pinch_update.notify = gesture_pinch_update;
    gp->pinch_end.notify = gesture_pinch_end;
    gp->hold_begin.notify = gesture_hold_begin;
    gp->hold_end.notify = gesture_hold_end;

    wl_signal_add(&pointer->events.swipe_begin, &gp->swipe_begin);
    wl_signal_add(&pointer->events.swipe_update, &gp->swipe_update);
    wl_signal_add(&pointer->events.swipe_end, &gp->swipe_end);
    wl_signal_add(&pointer->events.pinch_begin, &gp->pinch_begin);
    wl_signal_add(&pointer->events.pinch_update, &gp->pinch_update);
    wl_signal_add(&pointer->events.pinch_end, &gp->pinch_end);
    wl_signal_add(&pointer->events.hold_begin, &gp->hold_begin);
    wl_signal_add(&pointer->events.hold_end, &gp->hold_end);

    gp->destroy.notify = gesture_handle_destroy;
    wl_signal_add(&device->events.destroy, &gp->destroy);

    wl_list_insert(&server->gesture_pointers, &gp->link);
}

/* ==================== xdg-activation ==================== */

/*
 * 这里只处理"已经存在的窗口被要求置顶/聚焦"（点通知栏、从浏览器拉起
 * 已开的窗口）。启动器拉起新窗口的情形不需要处理：map 路径本来就会
 * 聚焦新窗口。
 *
 * token 里的 app_id 提示没被消费——要按 app_id 找窗口得等窗口规则
 * （M6）落地，现在硬猜只能猜错。
 */
static void activation_handle_request(struct wl_listener *listener,
                                      void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, activation_request);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;

    struct woodland_view *view = woodland_view_from_surface(event->surface);
    if (!view) {
        /* layer surface / 尚未建视图的 surface：不抢焦点 */
        return;
    }
    woodland_focus_view(view, true);
}

/* ==================== xdg-toplevel-tag ==================== */

static void toplevel_tag_handle_set(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, toplevel_tag_set);
    struct wlr_xdg_toplevel_tag_manager_v1_set_tag_event *event = data;

    struct woodland_view *view =
        woodland_view_from_surface(event->toplevel->base->surface);
    if (!view) {
        return;
    }
    /* 只记录，不参与规则匹配：窗口规则还没做，存下来至少能在 wctl 里看到 */
    woodland_view_set_tag(view, event->tag);
    (void)server;
}

/* ==================== primary-selection ==================== */

static void seat_handle_request_primary(struct wl_listener *listener,
                                        void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* ==================== init / finish ==================== */

static bool protocol_init_globals(struct woodland_server *server) {
    struct wl_display *display = server->display;

    /* 纯创建即完成的几个：客户端能不能用的分水岭在这里 */
    server->viewporter = wlr_viewporter_create(display);
    if (!server->viewporter) {
        wlr_log(WLR_ERROR, "failed to create viewporter");
        return false;
    }

    /* 帧时序：scene 在把 surface 提交到输出时自己会上报 feedback
     * （types/scene/surface.c），所以这里创建完就够，无需自己算 */
    server->presentation = wlr_presentation_create(display, server->backend, 1);
    if (!server->presentation) {
        wlr_log(WLR_ERROR, "failed to create presentation-time");
        return false;
    }

    server->primary_selection = wlr_primary_selection_v1_device_manager_create(
        display);
    if (!server->primary_selection) {
        wlr_log(WLR_ERROR, "failed to create primary-selection");
        return false;
    }

    server->relative_pointer =
        wlr_relative_pointer_manager_v1_create(display);
    if (!server->relative_pointer) {
        wlr_log(WLR_ERROR, "failed to create relative-pointer");
        return false;
    }

    server->pointer_gestures = wlr_pointer_gestures_v1_create(display);
    if (!server->pointer_gestures) {
        wlr_log(WLR_ERROR, "failed to create pointer-gestures");
        return false;
    }

    server->xdg_activation = wlr_xdg_activation_v1_create(display);
    if (!server->xdg_activation) {
        wlr_log(WLR_ERROR, "failed to create xdg-activation");
        return false;
    }
    server->activation_request.notify = activation_handle_request;
    wl_signal_add(&server->xdg_activation->events.request_activate,
                  &server->activation_request);

    server->toplevel_tag = wlr_xdg_toplevel_tag_manager_v1_create(display, 1);
    if (!server->toplevel_tag) {
        wlr_log(WLR_ERROR, "failed to create xdg-toplevel-tag");
        return false;
    }
    server->toplevel_tag_set.notify = toplevel_tag_handle_set;
    wl_signal_add(&server->toplevel_tag->events.set_tag,
                  &server->toplevel_tag_set);

    /* idle：通知器与抑制器是两件事，缺一个就别各自半残 */
    server->idle_notifier = wlr_idle_notifier_v1_create(display);
    server->idle_inhibit = wlr_idle_inhibit_v1_create(display);
    if (!server->idle_notifier || !server->idle_inhibit) {
        wlr_log(WLR_ERROR, "failed to create idle protocols");
        return false;
    }
    server->new_idle_inhibitor.notify = idle_handle_new_inhibitor;
    wl_signal_add(&server->idle_inhibit->events.new_inhibitor,
                  &server->new_idle_inhibitor);

    server->shortcuts_inhibit =
        wlr_keyboard_shortcuts_inhibit_v1_create(display);
    if (!server->shortcuts_inhibit) {
        wlr_log(WLR_ERROR, "failed to create keyboard-shortcuts-inhibit");
        return false;
    }
    server->new_shortcuts_inhibitor.notify = shortcuts_handle_new_inhibitor;
    wl_signal_add(&server->shortcuts_inhibit->events.new_inhibitor,
                  &server->new_shortcuts_inhibitor);
    server->shortcuts_focus_change.notify = shortcuts_handle_focus_change;
    wl_signal_add(&server->seat->keyboard_state.events.focus_change,
                  &server->shortcuts_focus_change);

    /* 输出电源：DPMS 开关 */
    server->output_power = wlr_output_power_manager_v1_create(display);
    if (!server->output_power) {
        wlr_log(WLR_ERROR, "failed to create output-power-management");
        return false;
    }
    server->output_power_set_mode.notify = output_power_handle_set_mode;
    wl_signal_add(&server->output_power->events.set_mode,
                  &server->output_power_set_mode);

    /* 输出配置：wdisplays / kanshi 这类工具靠它读写显示器设置 */
    server->output_manager = wlr_output_manager_v1_create(display);
    if (!server->output_manager) {
        wlr_log(WLR_ERROR, "failed to create output-management");
        return false;
    }
    server->output_manager_apply.notify = output_manager_handle_apply;
    wl_signal_add(&server->output_manager->events.apply,
                  &server->output_manager_apply);
    server->output_manager_test.notify = output_manager_handle_test;
    wl_signal_add(&server->output_manager->events.test,
                  &server->output_manager_test);
    server->output_layout_change.notify = output_layout_handle_change;
    wl_signal_add(&server->output_layout->events.change,
                  &server->output_layout_change);

    return true;
}

static void protocol_init_input(struct woodland_server *server) {
    server->proto_cursor_motion.notify = proto_cursor_motion;
    wl_signal_add(&server->cursor->events.motion,
                  &server->proto_cursor_motion);
    server->proto_cursor_motion_absolute.notify = proto_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
                  &server->proto_cursor_motion_absolute);
    server->proto_cursor_button.notify = proto_cursor_button;
    wl_signal_add(&server->cursor->events.button,
                  &server->proto_cursor_button);
    server->proto_cursor_axis.notify = proto_cursor_axis;
    wl_signal_add(&server->cursor->events.axis,
                  &server->proto_cursor_axis);

    server->request_set_primary_selection.notify =
        seat_handle_request_primary;
    wl_signal_add(&server->seat->events.request_set_primary_selection,
                  &server->request_set_primary_selection);
}

bool woodland_protocol_init(struct woodland_server *server) {
    if (!server->cursor || !server->seat || !server->output_layout) {
        wlr_log(WLR_ERROR, "protocol layer needs cursor/seat/output first");
        return false;
    }
    if (!protocol_init_globals(server)) {
        return false;
    }
    protocol_init_input(server);

    /* 已有输出可能早于本模块建好，补一次广播让客户端看到初始状态 */
    output_config_send(server);
    return true;
}

void woodland_protocol_finish(struct woodland_server *server) {
    /* 手势设备：正常的设备销毁会自己摘，这里收尾的是"设备还在但合成器
     * 要退"的情况，否则 backend 销毁时回调会打到已释放的 server 上 */
    struct woodland_gesture_pointer *gp, *tmp;
    wl_list_for_each_safe(gp, tmp, &server->gesture_pointers, link) {
        gesture_pointer_destroy(gp);
    }

    /* 抑制器本身归 wlroots，退出了就没人替我们释放包装结构 */
    struct woodland_idle_inhibitor *inhibitor, *inhibitor_tmp;
    wl_list_for_each_safe(inhibitor, inhibitor_tmp, &server->idle_inhibitors,
                          link) {
        wl_list_remove(&inhibitor->destroy.link);
        wl_list_remove(&inhibitor->link);
        free(inhibitor);
    }

    if (server->proto_cursor_motion.link.prev) {
        wl_list_remove(&server->proto_cursor_motion.link);
    }
    if (server->proto_cursor_motion_absolute.link.prev) {
        wl_list_remove(&server->proto_cursor_motion_absolute.link);
    }
    if (server->proto_cursor_button.link.prev) {
        wl_list_remove(&server->proto_cursor_button.link);
    }
    if (server->proto_cursor_axis.link.prev) {
        wl_list_remove(&server->proto_cursor_axis.link);
    }
    if (server->request_set_primary_selection.link.prev) {
        wl_list_remove(&server->request_set_primary_selection.link);
    }
    if (server->new_idle_inhibitor.link.prev) {
        wl_list_remove(&server->new_idle_inhibitor.link);
    }
    if (server->new_shortcuts_inhibitor.link.prev) {
        wl_list_remove(&server->new_shortcuts_inhibitor.link);
    }
    if (server->shortcuts_focus_change.link.prev) {
        wl_list_remove(&server->shortcuts_focus_change.link);
    }
    if (server->output_power_set_mode.link.prev) {
        wl_list_remove(&server->output_power_set_mode.link);
    }
    if (server->output_manager_apply.link.prev) {
        wl_list_remove(&server->output_manager_apply.link);
    }
    if (server->output_manager_test.link.prev) {
        wl_list_remove(&server->output_manager_test.link);
    }
    if (server->output_layout_change.link.prev) {
        wl_list_remove(&server->output_layout_change.link);
    }
    if (server->activation_request.link.prev) {
        wl_list_remove(&server->activation_request.link);
    }
    if (server->toplevel_tag_set.link.prev) {
        wl_list_remove(&server->toplevel_tag_set.link);
    }
}
