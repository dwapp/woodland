// SPDX-License-Identifier: GPL-2.0-only
/*
 * idle-inhibit 的聚合探针。
 *
 * 验证的是"最后一个抑制器销毁后 idle 必须恢复"——wlroots 在
 * wlr_idle_inhibit_v1.c 里先 emit destroy 再从 manager 链表摘除，所以按
 * manager 判空的实现会永久停在抑制态（屏幕永不熄灭，且没有事件能纠正）。
 *
 * 两个阶段各等 700ms，idle 超时 300ms：
 *   阶段 1（持有 inhibitor）→ idled 次数必须为 0
 *   阶段 2（destroy inhibitor）→ idled 次数必须 >= 1
 */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

#define IDLE_TIMEOUT_MS 300
#define WAIT_MS 700

static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static struct ext_idle_notifier_v1 *idle_notifier;
static int idled_count;

static void notifier_idled(void *data, struct ext_idle_notification_v1 *n) {
    (void)data;
    (void)n;
    idled_count++;
}

static void notifier_resumed(void *data, struct ext_idle_notification_v1 *n) {
    (void)data;
    (void)n;
}

static const struct ext_idle_notification_v1_listener notification_listener = {
    .idled = notifier_idled,
    .resumed = notifier_resumed,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
    } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        inhibit_manager = wl_registry_bind(registry, name,
            &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        idle_notifier = wl_registry_bind(registry, name,
            &ext_idle_notifier_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/*
 * 等 ms 毫秒，期间持续分发事件。
 *
 * 必须用 prepare_read/read_events 这一对：dispatch_pending 只处理已经落在
 * 缓冲区里的事件，不会去读 socket，那样永远看不到新来的 idled。
 */
static void pump(struct wl_display *display, int ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (wl_display_prepare_read(display) == 0) {
            wl_display_flush(display);
            struct pollfd pfd = {
                .fd = wl_display_get_fd(display),
                .events = POLLIN,
            };
            if (poll(&pfd, 1, 20) > 0) {
                wl_display_read_events(display);
            } else {
                wl_display_cancel_read(display);
            }
        }
        wl_display_dispatch_pending(display);
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&tick, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms) {
            break;
        }
    }
}

static void cleanup(struct wl_display *display, struct wl_registry *registry,
                    struct wl_surface *surface,
                    struct ext_idle_notification_v1 *notification) {
    /* ASan 构建里本程序也被插桩：不把代理挨个销毁就是 96 字节泄漏，
     * 会让 smoke 测试在 LeakSanitizer 下假失败 */
    if (notification) {
        ext_idle_notification_v1_destroy(notification);
    }
    if (surface) {
        wl_surface_destroy(surface);
    }
    if (inhibit_manager) {
        zwp_idle_inhibit_manager_v1_destroy(inhibit_manager);
    }
    if (idle_notifier) {
        ext_idle_notifier_v1_destroy(idle_notifier);
    }
    if (seat) {
        wl_seat_destroy(seat);
    }
    if (compositor) {
        wl_compositor_destroy(compositor);
    }
    if (registry) {
        wl_registry_destroy(registry);
    }
    wl_display_flush(display);
    wl_display_disconnect(display);
}

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to compositor\n");
        return 125;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !seat || !inhibit_manager || !idle_notifier) {
        /* 合成器没接这些协议时不该算失败，交给调用方 skip */
        fprintf(stderr, "missing globals (compositor/seat/inhibit/idle-notify)\n");
        cleanup(display, registry, NULL, NULL);
        return 77;
    }

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct zwp_idle_inhibitor_v1 *inhibitor =
        zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
    struct ext_idle_notification_v1 *notification =
        ext_idle_notifier_v1_get_idle_notification(idle_notifier,
                                                   IDLE_TIMEOUT_MS, seat);
    ext_idle_notification_v1_add_listener(notification,
                                          &notification_listener, NULL);
    wl_display_roundtrip(display);

    pump(display, WAIT_MS);
    int during = idled_count;
    if (during != 0) {
        fprintf(stderr, "FAIL: 持有 inhibitor 期间仍收到 %d 次 idled\n", during);
        cleanup(display, registry, surface, notification);
        return 1;
    }
    printf("ok: 持有 inhibitor 期间未 idle\n");

    zwp_idle_inhibitor_v1_destroy(inhibitor);
    wl_display_flush(display);
    pump(display, WAIT_MS);
    int after = idled_count - during;
    if (after < 1) {
        fprintf(stderr, "FAIL: 销毁最后一个 inhibitor 后 %dms 内没有 idled"
                        "（idle 被永久抑制）\n", WAIT_MS);
        cleanup(display, registry, surface, notification);
        return 1;
    }
    printf("ok: 销毁 inhibitor 后恢复 idle（idled %d 次）\n", after);

    cleanup(display, registry, surface, notification);
    return 0;
}
