// SPDX-License-Identifier: GPL-2.0-only
/*
 * 空闲与抑制族：idle-notify、idle-inhibit、keyboard-shortcuts-inhibit。
 *
 * 三个协议是一件事的两面：客户端声明"我在忙"或"别打扰我"，合成器据此
 * 停掉空闲计时、或把键位让给客户端。放在一起是因为它们共享同一个前提
 * ——当前键盘焦点落在哪个 surface 上，以及"有抑制器就要重算"的聚合。
 *
 * 这里不实现协议：抑制器对象归 wlroots 持有，本文件只跟踪它们的生命
 * 周期，并把状态翻译成合成器行为。
 */
#include <stdlib.h>

#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "protocol_internal.h"
#include "woodland.h"

/*
 * 抑制器由 wlroots 持有，但它只提供"新增"事件，不提供"全部清空"。
 * 每来一个抑制器挂一个销毁监听，任一销毁就重算一次聚合状态——
 * 否则最后一个抑制器消失后 idle 会一直停着。聚合只数自己的链表，
 * 原因见 idle_refresh()。
 */
struct woodland_idle_inhibitor {
    struct woodland_proto_idle *idle;
    struct wl_list link;                  // woodland_proto_idle.inhibitors
    struct wl_listener destroy;
};

struct woodland_proto_idle {
    struct woodland_server *server;

    /* idle-notify：客户端订阅空闲，合成器负责续命 */
    struct wlr_idle_notifier_v1 *notifier;

    /* idle-inhibit：抑制器聚合 */
    struct wlr_idle_inhibit_manager_v1 *inhibit;
    struct wl_listener new_inhibitor;
    struct wl_list inhibitors;            // struct woodland_idle_inhibitor.link

    /* keyboard-shortcuts-inhibit：激活态要跟着键盘焦点走 */
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts;
    struct wl_listener new_shortcut;
    struct wl_listener focus_change;
};

/*
 * 聚合看的是**我们自己**的链表，而不是 wlroots 的 manager->inhibitors。
 *
 * wlroots 在 wlr_idle_inhibit_v1.c 里是"先 emit destroy、再从 manager 链表
 * 摘除"（idle_inhibitor_v1_destroy），所以 destroy 回调执行时 manager 里还
 * 留着正在销毁的那一个。按 manager 判空的话，最后一个抑制器关掉后仍被算作
 * 非空，idle 从此再也恢复不了——屏幕永不熄灭，且没有任何事件能纠正它。
 *
 * destroy 回调在调用本函数前已经把自己从 inhibitors 摘掉了，所以这里读到
 * 的一定是"还活着"的那些，与 wlroots 的内部顺序无关。
 */
static void idle_refresh(struct woodland_proto_idle *idle) {
    if (!idle->notifier) {
        return;
    }
    bool inhibited = !wl_list_empty(&idle->inhibitors);
    wlr_idle_notifier_v1_set_inhibited(idle->notifier, inhibited);
}

static void idle_inhibitor_handle_destroy(struct wl_listener *listener,
                                          void *data) {
    struct woodland_idle_inhibitor *inhibitor =
        wl_container_of(listener, inhibitor, destroy);
    struct woodland_proto_idle *idle = inhibitor->idle;
    wl_list_remove(&inhibitor->destroy.link);
    wl_list_remove(&inhibitor->link);
    free(inhibitor);
    idle_refresh(idle);
    (void)data;
}

static void idle_handle_new_inhibitor(struct wl_listener *listener,
                                      void *data) {
    struct woodland_proto_idle *idle =
        wl_container_of(listener, idle, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;

    struct woodland_idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor) {
        /* 跟踪不了销毁就等于没有这个抑制器：宁可让 idle 正常恢复计时，
         * 也不要留一个永远回收不掉的条目把屏幕钉死 */
        wlr_log(WLR_ERROR, "cannot track idle inhibitor, ignoring it");
        idle_refresh(idle);
        return;
    }
    inhibitor->idle = idle;
    inhibitor->destroy.notify = idle_inhibitor_handle_destroy;
    wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
    wl_list_insert(&idle->inhibitors, &inhibitor->link);
    idle_refresh(idle);
}

/*
 * 协议规定：抑制器只在它所属 surface 拿到键盘焦点时生效。所以激活态
 * 不能只按"有没有人声明"来定，必须跟着焦点走——焦点从 A 换到 B 时，
 * A 的抑制器要立刻失效，否则 B 里的 Alt+Tab 会被 A 吞掉。
 */
static void shortcuts_sync(struct woodland_proto_idle *idle) {
    if (!idle->shortcuts) {
        return;
    }
    struct wlr_surface *focused = idle->server->seat
        ? idle->server->seat->keyboard_state.focused_surface : NULL;

    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &idle->shortcuts->inhibitors, link) {
        bool want = focused && inhibitor->surface == focused;
        if (want && !inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
        } else if (!want && inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inhibitor);
        }
    }
}

bool woodland_proto_idle_shortcuts_inhibited(struct woodland_proto *proto) {
    struct woodland_proto_idle *idle = proto ? proto->idle : NULL;
    if (!idle || !idle->shortcuts || !idle->server->seat) {
        return false;
    }
    struct wlr_surface *focused = idle->server->seat->keyboard_state.focused_surface;
    if (!focused) {
        return false;
    }
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &idle->shortcuts->inhibitors, link) {
        if (inhibitor->active && inhibitor->surface == focused) {
            return true;
        }
    }
    return false;
}

static void shortcuts_handle_new_inhibitor(struct wl_listener *listener,
                                           void *data) {
    struct woodland_proto_idle *idle =
        wl_container_of(listener, idle, new_shortcut);
    /* 新声明的不一定属于当前焦点 surface：交给统一的同步逻辑判断 */
    shortcuts_sync(idle);
    (void)data;
}

/*
 * 焦点变化是抑制器唯一的失效时机，所以挂在 seat 的 keyboard focus_change
 * 上，而不是在 focus.c 的每个出口各插一次调用——键盘焦点不止由 focus.c
 * 下发（layer.c 也直接 notify_enter），漏一处就会留下"旧窗口还在吞键"。
 */
static void shortcuts_handle_focus_change(struct wl_listener *listener,
                                          void *data) {
    struct woodland_proto_idle *idle =
        wl_container_of(listener, idle, focus_change);
    shortcuts_sync(idle);
    (void)data;
}

void woodland_proto_idle_notify_activity(struct woodland_proto *proto) {
    struct woodland_proto_idle *idle = proto ? proto->idle : NULL;
    if (idle && idle->notifier) {
        wlr_idle_notifier_v1_notify_activity(idle->notifier, idle->server->seat);
    }
}

bool woodland_proto_idle_init(struct woodland_proto *proto) {
    struct woodland_server *server = proto->server;

    struct woodland_proto_idle *idle = calloc(1, sizeof(*idle));
    if (!idle) {
        wlr_log(WLR_ERROR, "cannot allocate idle protocol state");
        return false;
    }
    idle->server = server;
    wl_list_init(&idle->inhibitors);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->idle = idle;

    /* idle：通知器与抑制器是两件事，缺一个就别各自半残 */
    idle->notifier = wlr_idle_notifier_v1_create(server->display);
    idle->inhibit = wlr_idle_inhibit_v1_create(server->display);
    if (!idle->notifier || !idle->inhibit) {
        wlr_log(WLR_ERROR, "failed to create idle protocols");
        return false;
    }
    idle->new_inhibitor.notify = idle_handle_new_inhibitor;
    wl_signal_add(&idle->inhibit->events.new_inhibitor, &idle->new_inhibitor);

    idle->shortcuts = wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
    if (!idle->shortcuts) {
        wlr_log(WLR_ERROR, "failed to create keyboard-shortcuts-inhibit");
        return false;
    }
    idle->new_shortcut.notify = shortcuts_handle_new_inhibitor;
    wl_signal_add(&idle->shortcuts->events.new_inhibitor, &idle->new_shortcut);
    idle->focus_change.notify = shortcuts_handle_focus_change;
    wl_signal_add(&server->seat->keyboard_state.events.focus_change,
                  &idle->focus_change);

    return true;
}

void woodland_proto_idle_finish(struct woodland_proto *proto) {
    struct woodland_proto_idle *idle = proto->idle;
    if (!idle) {
        return;
    }

    /* 抑制器本身归 wlroots，退出了就没人替我们释放包装结构 */
    struct woodland_idle_inhibitor *inhibitor, *tmp;
    wl_list_for_each_safe(inhibitor, tmp, &idle->inhibitors, link) {
        wl_list_remove(&inhibitor->destroy.link);
        wl_list_remove(&inhibitor->link);
        free(inhibitor);
    }

    if (idle->new_inhibitor.link.prev) {
        wl_list_remove(&idle->new_inhibitor.link);
    }
    if (idle->new_shortcut.link.prev) {
        wl_list_remove(&idle->new_shortcut.link);
    }
    if (idle->focus_change.link.prev) {
        wl_list_remove(&idle->focus_change.link);
    }

    free(idle);
    proto->idle = NULL;
}
