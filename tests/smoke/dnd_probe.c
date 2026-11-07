// SPDX-License-Identifier: GPL-2.0-only
/*
 * 拖拽探针：请求一次 start_drag（serial 故意无效），断言合成器回了
 * wl_data_source.cancelled。
 *
 * 为什么这条断言值得写：缺了 request_start_drag 的监听器时，合成器对
 * start_drag **一声不吭**——源既不 cancelled 也不 dnd_finished，客户端就
 * 挂在那里，正是"拖放静默失败"的形状。所以"收到 cancelled"是"这条路
 * 走通了"的最小证据。
 *
 * 真拖拽没法在这里跑：start_drag 的 serial 必须来自一次真实的 pointer
 * button press（合成器用 wlr_seat_validate_pointer_grab_serial 校验
 * seat->pointer_state 里的 grab serial），headless 后端不产生指针事件。
 * reject 与 accept 只差那一行校验结果，所以这里覆盖到的是"接线通了"+
 * "拒绝时源被销毁"，accept 之后的图标/投放走真机手测清单
 * （docs/superpowers/specs/2026-11-08-clipboard-dnd-design.md §8.3）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;

static int cancelled;
static int dropped;
static int finished;

static void source_target(void *data, struct wl_data_source *source, const char *mime_type) {
    (void)data;
    (void)source;
    (void)mime_type;
}

static void source_send(void *data, struct wl_data_source *source, const char *mime_type,
                        int32_t fd) {
    (void)data;
    (void)source;
    (void)mime_type;
    /* 我们没打算真送数据（这次拖拽必被拒），关掉 fd 就行 */
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
    (void)data;
    (void)source;
    cancelled = 1;
}

static void source_drop_performed(void *data, struct wl_data_source *source) {
    (void)data;
    (void)source;
    dropped = 1;
}

static void source_finished(void *data, struct wl_data_source *source) {
    (void)data;
    (void)source;
    finished = 1;
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action) {
    (void)data;
    (void)source;
    (void)action;
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_drop_performed,
    .dnd_finished = source_finished,
    .action = source_action,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, v);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t v = version < 7 ? version : 7;
        seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        uint32_t v = version < 3 ? version : 3;
        ddm = wl_registry_bind(registry, name, &wl_data_device_manager_interface, v);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "dnd_probe: cannot connect to the compositor\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    /*
     * 两次 roundtrip：第一次只拿到 global 列表，registry 回调里发的 bind
     * 还在出站缓冲里，而同一批事件里的 sync 已经把 done 置上了，dispatch
     * 立刻返回。第二次开头那次 flush 才把 bind 真正发出去——见 AGENTS.md。
     */
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "dnd_probe: roundtrip failed\n");
        return 1;
    }
    if (!compositor || !seat || !ddm) {
        fprintf(stderr, "dnd_probe: missing wl_compositor / wl_seat / "
                        "wl_data_device_manager\n");
        return 1;
    }

    /*
     * start_drag 的 origin 是非空参数（协议签名 ?oo?ou），必须给一个 surface。
     * 随便建一个没有角色的 wl_surface 就够——origin 只用于"拖拽从哪块表面
     * 发起"，我们这次一定被拒，不会走到用它算几何的分支。
     */
    struct wl_surface *origin = wl_compositor_create_surface(compositor);

    ddev = wl_data_device_manager_get_data_device(ddm, seat);
    struct wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_add_listener(source, &source_listener, NULL);

    /*
     * serial 用 0xdeadbeef：合成器发过的 serial 是递增的小整数，这个值
     * 一定不匹配，走的就是"校验失败 → 销毁源"那条分支。
     */
    wl_data_device_start_drag(ddev, source, origin, NULL, 0xdeadbeefu);
    wl_display_flush(display);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (cancelled) {
        printf("dnd_probe: start_drag rejected with cancelled\n");
    } else if (dropped || finished) {
        fprintf(stderr, "dnd_probe: compositor accepted a drag with a bogus serial\n");
        return 1;
    } else {
        fprintf(stderr,
                "dnd_probe: start_drag went unanswered (no cancelled/dnd_finished) — "
                "request_start_drag is probably not handled\n");
        return 1;
    }

    wl_data_source_destroy(source);
    wl_surface_destroy(origin);
    wl_data_device_destroy(ddev);
    /* 每个 bind 出来的 proxy 都自己还：ASan 构建下漏一个就是一条 leak */
    wl_data_device_manager_destroy(ddm);
    wl_seat_destroy(seat);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
