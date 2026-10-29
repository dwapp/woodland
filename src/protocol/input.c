// SPDX-License-Identifier: GPL-2.0-only
/*
 * 输入与选区族：relative-pointer、pointer-gestures、primary-selection。
 *
 * 这三块共享同一个位置——都是"合成器收到的事件要原样转给客户端"，而且
 * 全部挂在 seat / cursor / 指针设备上：
 *   - relative-pointer 与 idle 活动续命挂在 wlr_cursor 的 motion 上；
 *   - pointer-gestures 是 per-device 的（wlr_pointer 上挂信号）；
 *   - primary-selection 是 seat 的选区请求，转发即完成。
 *
 * 手势的监听器按设备分配，设备销毁时跟着释放；finish 时兜底清理剩下的，
 * 否则 backend 销毁时的回调会打到已释放的 server 上。
 */
#include <stdlib.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "protocol_internal.h"
#include "woodland.h"

struct woodland_gesture_pointer {
    struct woodland_proto_input *input;
    struct wl_list link;                  // woodland_proto_input.pointers
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

struct woodland_proto_input {
    struct woodland_proto *owner;
    struct woodland_server *server;

    struct wlr_relative_pointer_manager_v1 *relative_pointer;

    struct wlr_pointer_gestures_v1 *gestures;
    struct wl_list pointers;              // struct woodland_gesture_pointer.link

    struct wlr_primary_selection_v1_device_manager *selection;
    struct wl_listener request_set_selection;

    /* 光标事件上的钩子：相对指针与 idle 活动（shell.c 已有一组，不共用） */
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
};

/* ==================== 光标钩子 ==================== */

/*
 * 相对指针与 idle 活动都靠光标事件驱动，所以监听器挂在协议层：shell.c
 * 里的光标回调已经够长了，不该再往里塞协议细节。
 */
static void input_handle_cursor_motion(struct wl_listener *listener,
                                       void *data) {
    struct woodland_proto_input *input =
        wl_container_of(listener, input, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    woodland_proto_idle_notify_activity(input->owner);
    if (input->relative_pointer && input->server->seat) {
        /* 协议要微秒；wlr 事件给的是毫秒 */
        wlr_relative_pointer_manager_v1_send_relative_motion(
            input->relative_pointer, input->server->seat,
            (uint64_t)event->time_msec * 1000,
            event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
    }
}

static void input_handle_cursor_motion_absolute(struct wl_listener *listener,
                                                void *data) {
    struct woodland_proto_input *input =
        wl_container_of(listener, input, cursor_motion_absolute);
    woodland_proto_idle_notify_activity(input->owner);
    (void)data;
}

static void input_handle_cursor_button(struct wl_listener *listener,
                                       void *data) {
    struct woodland_proto_input *input =
        wl_container_of(listener, input, cursor_button);
    woodland_proto_idle_notify_activity(input->owner);
    (void)data;
}

static void input_handle_cursor_axis(struct wl_listener *listener,
                                     void *data) {
    struct woodland_proto_input *input =
        wl_container_of(listener, input, cursor_axis);
    woodland_proto_idle_notify_activity(input->owner);
    (void)data;
}

/* ==================== pointer-gestures ==================== */

static void gesture_swipe_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_begin);
    struct wlr_pointer_swipe_begin_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_swipe_begin(input->gestures,
            input->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_swipe_update(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_update);
    struct wlr_pointer_swipe_update_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_swipe_update(input->gestures,
            input->server->seat, event->time_msec, event->dx, event->dy);
    }
}

static void gesture_swipe_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, swipe_end);
    struct wlr_pointer_swipe_end_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_swipe_end(input->gestures,
            input->server->seat, event->time_msec, event->cancelled);
    }
}

static void gesture_pinch_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_begin);
    struct wlr_pointer_pinch_begin_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_pinch_begin(input->gestures,
            input->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_pinch_update(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_update);
    struct wlr_pointer_pinch_update_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_pinch_update(input->gestures,
            input->server->seat, event->time_msec, event->dx, event->dy,
            event->scale, event->rotation);
    }
}

static void gesture_pinch_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, pinch_end);
    struct wlr_pointer_pinch_end_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_pinch_end(input->gestures,
            input->server->seat, event->time_msec, event->cancelled);
    }
}

static void gesture_hold_begin(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, hold_begin);
    struct wlr_pointer_hold_begin_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_hold_begin(input->gestures,
            input->server->seat, event->time_msec, event->fingers);
    }
}

static void gesture_hold_end(struct wl_listener *listener, void *data) {
    struct woodland_gesture_pointer *gp =
        wl_container_of(listener, gp, hold_end);
    struct wlr_pointer_hold_end_event *event = data;
    struct woodland_proto_input *input = gp->input;
    woodland_proto_idle_notify_activity(input->owner);
    if (input->gestures) {
        wlr_pointer_gestures_v1_send_hold_end(input->gestures,
            input->server->seat, event->time_msec, event->cancelled);
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

void woodland_proto_input_attach_device(struct woodland_proto *proto,
                                        struct wlr_input_device *device) {
    struct woodland_proto_input *input = proto ? proto->input : NULL;
    if (!input || !input->gestures) {
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
    gp->input = input;

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

    wl_list_insert(&input->pointers, &gp->link);
}

/* ==================== primary-selection ==================== */

static void input_handle_request_selection(struct wl_listener *listener,
                                           void *data) {
    struct woodland_proto_input *input =
        wl_container_of(listener, input, request_set_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(input->server->seat, event->source,
                                   event->serial);
}

/* ==================== init / finish ==================== */

bool woodland_proto_input_init(struct woodland_proto *proto) {
    struct woodland_server *server = proto->server;

    struct woodland_proto_input *input = calloc(1, sizeof(*input));
    if (!input) {
        wlr_log(WLR_ERROR, "cannot allocate input protocol state");
        return false;
    }
    input->owner = proto;
    input->server = server;
    wl_list_init(&input->pointers);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->input = input;

    input->relative_pointer = wlr_relative_pointer_manager_v1_create(
        server->display);
    if (!input->relative_pointer) {
        wlr_log(WLR_ERROR, "failed to create relative-pointer");
        return false;
    }

    input->gestures = wlr_pointer_gestures_v1_create(server->display);
    if (!input->gestures) {
        wlr_log(WLR_ERROR, "failed to create pointer-gestures");
        return false;
    }

    input->selection = wlr_primary_selection_v1_device_manager_create(
        server->display);
    if (!input->selection) {
        wlr_log(WLR_ERROR, "failed to create primary-selection");
        return false;
    }
    input->request_set_selection.notify = input_handle_request_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection,
                  &input->request_set_selection);

    input->cursor_motion.notify = input_handle_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &input->cursor_motion);
    input->cursor_motion_absolute.notify = input_handle_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
                  &input->cursor_motion_absolute);
    input->cursor_button.notify = input_handle_cursor_button;
    wl_signal_add(&server->cursor->events.button, &input->cursor_button);
    input->cursor_axis.notify = input_handle_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &input->cursor_axis);

    return true;
}

void woodland_proto_input_finish(struct woodland_proto *proto) {
    struct woodland_proto_input *input = proto->input;
    if (!input) {
        return;
    }

    /* 手势设备：正常的设备销毁会自己摘，这里收尾的是"设备还在但合成器
     * 要退"的情况 */
    struct woodland_gesture_pointer *gp, *tmp;
    wl_list_for_each_safe(gp, tmp, &input->pointers, link) {
        gesture_pointer_destroy(gp);
    }

    if (input->request_set_selection.link.prev) {
        wl_list_remove(&input->request_set_selection.link);
    }
    if (input->cursor_motion.link.prev) {
        wl_list_remove(&input->cursor_motion.link);
    }
    if (input->cursor_motion_absolute.link.prev) {
        wl_list_remove(&input->cursor_motion_absolute.link);
    }
    if (input->cursor_button.link.prev) {
        wl_list_remove(&input->cursor_button.link);
    }
    if (input->cursor_axis.link.prev) {
        wl_list_remove(&input->cursor_axis.link);
    }

    free(input);
    proto->input = NULL;
}
