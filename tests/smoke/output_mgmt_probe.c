// SPDX-License-Identifier: GPL-2.0-only
/*
 * output-management 事件顺序探针。
 *
 * 协议在 zwlr_output_configuration_v1.succeeded 的描述里写明：
 *   "If the current configuration has changed, events to describe the
 *    changes will be sent followed by a wlr_output_manager.done event."
 * 也就是说 succeeded 必须先到，随后才允许出现新一轮 configuration + done。
 *
 * 合成器如果在 apply 里先写回布局（wlr_output_layout_add/remove 会触发
 * layout change，而广播挂在那个信号上）、再去回 succeeded，客户端看到的
 * 就是反过来的顺序：先 configuration + done，后 succeeded。wdisplays /
 * kanshi 会把那份配置当成另一轮变更。
 *
 * 退出码：0 顺序正确；1 顺序错误；77 环境不支持（无 output / 无 mode）。
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "wlr-output-management-unstable-v1-client-protocol.h"

#define MAX_EVENTS 8
#define WAIT_MS 2000

static const char *log_events[MAX_EVENTS];
static int n_events;

static struct zwlr_output_manager_v1 *manager;
static struct zwlr_output_head_v1 *head;
static struct zwlr_output_mode_v1 *mode;
static int32_t head_x, head_y;
static uint32_t serial;
static int got_configuration;

static void record(const char *event) {
    if (n_events < MAX_EVENTS) {
        log_events[n_events++] = event;
    }
}

static void pump(struct wl_display *display, int ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfd = {
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
        };
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(display) < 0) {
                return;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms) {
            return;
        }
    }
}

static void mode_handle_size(void *data, struct zwlr_output_mode_v1 *m,
                             int32_t width, int32_t height) {
    (void)data; (void)m; (void)width; (void)height;
}

static void mode_handle_refresh(void *data, struct zwlr_output_mode_v1 *m,
                                int32_t refresh) {
    (void)data; (void)m; (void)refresh;
}

static void mode_handle_preferred(void *data, struct zwlr_output_mode_v1 *m) {
    (void)data; (void)m;
}

static void mode_handle_finished(void *data, struct zwlr_output_mode_v1 *m) {
    (void)data;
    zwlr_output_mode_v1_destroy(m);
    if (mode == m) {
        mode = NULL;
    }
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_handle_size,
    .refresh = mode_handle_refresh,
    .preferred = mode_handle_preferred,
    .finished = mode_handle_finished,
};

static void head_handle_name(void *data, struct zwlr_output_head_v1 *h,
                             const char *name) {
    (void)data; (void)h; (void)name;
}

static void head_handle_description(void *data, struct zwlr_output_head_v1 *h,
                                    const char *description) {
    (void)data; (void)h; (void)description;
}

static void head_handle_physical_size(void *data, struct zwlr_output_head_v1 *h,
                                      int32_t width, int32_t height) {
    (void)data; (void)h; (void)width; (void)height;
}

static void head_handle_mode(void *data, struct zwlr_output_head_v1 *h,
                             struct zwlr_output_mode_v1 *m) {
    (void)data; (void)h;
    zwlr_output_mode_v1_add_listener(m, &mode_listener, NULL);
    /* 取第一个 mode：headless 只有一个 */
    if (!mode) {
        mode = m;
    }
}

static void head_handle_enabled(void *data, struct zwlr_output_head_v1 *h,
                                int32_t enabled) {
    (void)data; (void)h; (void)enabled;
}

static void head_handle_current_mode(void *data, struct zwlr_output_head_v1 *h,
                                     struct zwlr_output_mode_v1 *m) {
    (void)data; (void)h;
    if (!mode) {
        mode = m;
    }
}

static void head_handle_position(void *data, struct zwlr_output_head_v1 *h,
                                 int32_t x, int32_t y) {
    (void)data; (void)h;
    head_x = x;
    head_y = y;
}

static void head_handle_transform(void *data, struct zwlr_output_head_v1 *h,
                                  int32_t transform) {
    (void)data; (void)h; (void)transform;
}

static void head_handle_scale(void *data, struct zwlr_output_head_v1 *h,
                              wl_fixed_t scale) {
    (void)data; (void)h; (void)scale;
}

static void head_handle_finished(void *data, struct zwlr_output_head_v1 *h) {
    (void)data;
    zwlr_output_head_v1_destroy(h);
    if (head == h) {
        head = NULL;
    }
}

static void head_handle_make(void *data, struct zwlr_output_head_v1 *h,
                             const char *make) {
    (void)data; (void)h; (void)make;
}

static void head_handle_model(void *data, struct zwlr_output_head_v1 *h,
                              const char *model) {
    (void)data; (void)h; (void)model;
}

static void head_handle_serial_number(void *data, struct zwlr_output_head_v1 *h,
                                      const char *serial_number) {
    (void)data; (void)h; (void)serial_number;
}

const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_handle_name,
    .description = head_handle_description,
    .physical_size = head_handle_physical_size,
    .mode = head_handle_mode,
    .enabled = head_handle_enabled,
    .current_mode = head_handle_current_mode,
    .position = head_handle_position,
    .transform = head_handle_transform,
    .scale = head_handle_scale,
    .finished = head_handle_finished,
    .make = head_handle_make,
    .model = head_handle_model,
    .serial_number = head_handle_serial_number,
};

static void manager_handle_head(void *data, struct zwlr_output_manager_v1 *m,
                                struct zwlr_output_head_v1 *h) {
    (void)data; (void)m;
    if (!head) {
        head = h;
        zwlr_output_head_v1_add_listener(h, &head_listener, NULL);
    }
}

static void manager_handle_done(void *data, struct zwlr_output_manager_v1 *m,
                                uint32_t s) {
    (void)data; (void)m;
    serial = s;
    got_configuration++;
    record("configuration");
}

static void manager_handle_finished(void *data,
                                    struct zwlr_output_manager_v1 *m) {
    (void)data; (void)m;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_handle_head,
    .done = manager_handle_done,
    .finished = manager_handle_finished,
};

static void config_handle_succeeded(void *data,
                                    struct zwlr_output_configuration_v1 *c) {
    (void)data; (void)c;
    record("succeeded");
}

static void config_handle_failed(void *data,
                                 struct zwlr_output_configuration_v1 *c) {
    (void)data; (void)c;
    record("failed");
}

static void config_handle_cancelled(void *data,
                                    struct zwlr_output_configuration_v1 *c) {
    (void)data; (void)c;
    record("cancelled");
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_handle_succeeded,
    .failed = config_handle_failed,
    .cancelled = config_handle_cancelled,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
    (void)data; (void)version;
    if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(registry, name,
                                   &zwlr_output_manager_v1_interface, 1);
        wl_proxy_add_listener((struct wl_proxy *)manager,
                             (void (**)(void))&manager_listener, NULL);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *r,
                                          uint32_t name) {
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to compositor\n");
        return 125;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!manager) {
        fprintf(stderr, "compositor does not advertise output-management\n");
        return 77;
    }
    if (!head || !mode) {
        fprintf(stderr, "no head with a mode, skipping\n");
        return 77;
    }

    struct zwlr_output_configuration_v1 *config =
        zwlr_output_manager_v1_create_configuration(manager, serial);
    struct zwlr_output_configuration_head_v1 *config_head =
        zwlr_output_configuration_v1_enable_head(config, head);
    zwlr_output_configuration_head_v1_set_mode(config_head, mode);
    zwlr_output_configuration_head_v1_set_scale(config_head,
                                                wl_fixed_from_double(1.0));
    zwlr_output_configuration_head_v1_set_position(config_head, head_x + 100,
                                                   head_y);
    zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);

    /* 从这里开始只记录本轮的回应 */
    n_events = 0;
    got_configuration = 0;
    zwlr_output_configuration_v1_apply(config);
    wl_display_flush(display);

    /* 等到 succeeded/failed 之后又收到一份新配置，或者超时 */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pump(display, 20);
        int responded = 0;
        for (int i = 0; i < n_events; i++) {
            if (strcmp(log_events[i], "configuration") != 0) {
                responded = 1;
            }
        }
        if (responded && got_configuration > 0) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= WAIT_MS) {
            break;
        }
    }

    printf("apply 之后收到的事件顺序:");
    for (int i = 0; i < n_events; i++) {
        printf(" %s", log_events[i]);
    }
    printf("\n");

    int rc = 0;
    if (n_events == 0) {
        fprintf(stderr, "FAIL: apply 之后没有收到任何事件\n");
        rc = 1;
    } else if (strcmp(log_events[0], "configuration") == 0) {
        fprintf(stderr,
                "FAIL: 本次 apply 的回应之前先来了 configuration + done；\n"
                "      客户端会把它当成另一轮配置变更（协议要求 succeeded 在前）\n");
        rc = 1;
    } else if (strcmp(log_events[0], "succeeded") != 0) {
        fprintf(stderr, "FAIL: apply 失败（%s），无法验证顺序\n", log_events[0]);
        rc = 1;
    } else {
        printf("ok: succeeded 先于 configuration\n");
    }

    /* 逐个释放，ASan 构建下探针自己也不许漏 */
    zwlr_output_configuration_head_v1_destroy(config_head);
    zwlr_output_configuration_v1_destroy(config);
    if (mode) {
        zwlr_output_mode_v1_destroy(mode);
    }
    if (head) {
        zwlr_output_head_v1_destroy(head);
    }
    zwlr_output_manager_v1_destroy(manager);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return rc;
}
