// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "cursor.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "output.h"
#include "seat.h"
#include "session_lock.h"
#include "view.h"
#include "woodland.h"

struct client_state {
    struct ext_session_lock_manager_v1 *manager;
    struct wl_compositor *compositor;
    struct wl_output *output;
    struct wl_shm *shm;
    struct ext_session_lock_v1 *lock;
    struct ext_session_lock_surface_v1 *lock_surface;
    bool locked;
    bool finished;
    uint32_t configure_width;
    uint32_t configure_height;
    uint32_t configure_serial;
};

static void lock_handle_locked(void *data, struct ext_session_lock_v1 *lock)
{
    struct client_state *state = data;
    state->locked = true;
    (void)lock;
}

static void lock_handle_finished(void *data, struct ext_session_lock_v1 *lock)
{
    struct client_state *state = data;
    state->finished = true;
    (void)lock;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_handle_locked,
    .finished = lock_handle_finished,
};

static void surface_handle_configure(void *data, struct ext_session_lock_surface_v1 *surface,
                                     uint32_t serial, uint32_t width, uint32_t height)
{
    struct client_state *state = data;
    state->configure_serial = serial;
    state->configure_width = width;
    state->configure_height = height;
    ext_session_lock_surface_v1_ack_configure(surface, serial);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
    .configure = surface_handle_configure,
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        state->manager =
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        state->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    (void)version;
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static void sync_callback(void *data, struct wl_callback *cb, uint32_t serial)
{
    bool *done = data;
    *done = true;
    wl_callback_destroy(cb);
    (void)serial;
}
static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("test-shm", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_callback,
};

static void pump_roundtrip(struct woodland_server *server, struct wl_display *client_disp)
{
    bool done = false;
    struct wl_callback *cb = wl_display_sync(client_disp);
    assert(cb != NULL);
    wl_callback_add_listener(cb, &sync_listener, &done);
    while (!done) {
        assert(wl_display_get_error(client_disp) == 0);
        wl_display_flush(client_disp);
        wl_event_loop_dispatch(server->event_loop, 0);
        wl_display_flush_clients(server->display);
        wl_display_dispatch(client_disp);
    }
}

int main(void)
{
    setenv("WLR_RENDERER", "pixman", 1);

    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display != NULL);

    server.event_loop = wl_display_get_event_loop(server.display);
    assert(server.event_loop != NULL);

    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend != NULL);

    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer != NULL);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    assert(server.compositor != NULL);
    wlr_renderer_init_wl_shm(server.renderer, server.display);
    assert(server.allocator != NULL);

    server.scene = wlr_scene_create();
    assert(server.scene != NULL);

    assert(woodland_output_init(&server));
    assert(woodland_cursor_init(&server));
    assert(woodland_seat_init(&server));
    assert(woodland_session_lock_init(&server));
    assert(woodland_shell_init(&server));
    woodland_cursor_init_events(&server);

    /* 添加一个无头输出 */
    struct wlr_output *wlr_out = wlr_headless_add_output(server.backend, 1280, 720);
    assert(wlr_out != NULL);

    /* 触发 new_output 事件以构建 woodland_output */
    wl_signal_emit_mutable(&server.backend->events.new_output, wlr_out);
    assert(!wl_list_empty(&server.outputs));

    /* 1. 断言协议初始化与初始未锁定状态 */
    assert(server.session_lock != NULL);
    assert(server.session_lock->wlr_manager != NULL);
    assert(server.session_lock->wlr_manager->global != NULL);
    assert(!woodland_session_lock_is_locked(&server));

    /* 2. 创建物理键盘 */
    struct wlr_keyboard keyboard = {0};
    wlr_keyboard_init(&keyboard, NULL, "test-kb");
    woodland_seat_new_keyboard(&server, &keyboard.base);
    assert(!wl_list_empty(&server.keyboards));

    /* 3. 客户端 1 连接并请求加锁 */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *client = wl_client_create(server.display, fds[0]);
    assert(client != NULL);
    struct wl_display *client_display = wl_display_connect_to_fd(fds[1]);
    assert(client_display != NULL);

    struct client_state c1 = {0};
    struct wl_registry *reg = wl_display_get_registry(client_display);
    wl_registry_add_listener(reg, &registry_listener, &c1);
    pump_roundtrip(&server, client_display);

    assert(c1.manager != NULL);
    assert(c1.compositor != NULL);
    assert(c1.output != NULL);

    c1.lock = ext_session_lock_manager_v1_lock(c1.manager);
    assert(c1.lock != NULL);
    ext_session_lock_v1_add_listener(c1.lock, &lock_listener, &c1);

    pump_roundtrip(&server, client_display);

    /* 验证服务端进入锁定态，客户端收到 locked 事件，且屏幕已被遮罩 */
    assert(woodland_session_lock_is_locked(&server));
    assert(c1.locked == true);
    assert(!wl_list_empty(&server.session_lock->lock_outputs));
    assert(server.seat->pointer_state.focused_surface == NULL);
    assert(server.cursor_mode == WOODLAND_CURSOR_PASSTHROUGH);

    /* 4. 客户端 1 创建锁屏表面并配置尺寸 */
    struct wl_surface *surf = wl_compositor_create_surface(c1.compositor);
    assert(surf != NULL);
    c1.lock_surface = ext_session_lock_v1_get_lock_surface(c1.lock, surf, c1.output);
    assert(c1.lock_surface != NULL);
    ext_session_lock_surface_v1_add_listener(c1.lock_surface, &surface_listener, &c1);

    pump_roundtrip(&server, client_display);

    assert(c1.configure_width == 1280);
    assert(c1.configure_height == 720);

    /* 绑定并提交真实 buffer 使其完成映射 */
    struct wl_buffer *buf = create_shm_buffer(c1.shm, c1.configure_width, c1.configure_height);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_commit(surf);
    pump_roundtrip(&server, client_display);

    /* 验证映射后焦点赋予锁屏表面 */
    assert(server.session_lock->focused_surface != NULL);
    struct woodland_session_lock_output *first_lo =
        wl_container_of(server.session_lock->lock_outputs.next, first_lo, link);
    assert(server.session_lock->focused_surface == first_lo->surface->surface);

    /* 5. 命中测试与锁定态光标/按键分发 */
    struct wlr_surface *hit_surf = NULL;
    double sx = 0, sy = 0;
    assert(woodland_session_lock_surface_at(&server, 100, 100, &hit_surf, &sx, &sy) == true);
    assert(hit_surf == server.session_lock->focused_surface);
    assert(sx == 100 && sy == 100);

    /* 模拟普通按键下发 */
    struct wlr_keyboard_key_event key_ev = {
        .time_msec = 1000,
        .keycode = 30, // KEY_A
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
    };
    wl_signal_emit_mutable(&keyboard.events.key, &key_ev);

    /* 6. 在已锁定情况下，第二个客户端尝试加锁应被拒绝 */
    int fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds2) == 0);
    struct wl_client *client2 = wl_client_create(server.display, fds2[0]);
    assert(client2 != NULL);
    struct wl_display *client2_display = wl_display_connect_to_fd(fds2[1]);
    assert(client2_display != NULL);

    struct client_state c2 = {0};
    struct wl_registry *reg2 = wl_display_get_registry(client2_display);
    wl_registry_add_listener(reg2, &registry_listener, &c2);
    pump_roundtrip(&server, client2_display);

    assert(c2.manager != NULL);
    c2.lock = ext_session_lock_manager_v1_lock(c2.manager);
    ext_session_lock_v1_add_listener(c2.lock, &lock_listener, &c2);
    pump_roundtrip(&server, client2_display);

    /* 第二个客户端应收到 finished 事件 */
    assert(c2.finished == true);
    ext_session_lock_v1_destroy(c2.lock);
    ext_session_lock_manager_v1_destroy(c2.manager);
    wl_compositor_destroy(c2.compositor);
    wl_output_destroy(c2.output);
    wl_registry_destroy(reg2);
    if (c2.shm)
        wl_shm_destroy(c2.shm);
    pump_roundtrip(&server, client2_display);
    wl_display_disconnect(client2_display);
    wl_client_destroy(client2);

    /* 7. 客户端 1 执行解锁 */
    ext_session_lock_surface_v1_destroy(c1.lock_surface);
    wl_buffer_destroy(buf);
    wl_surface_destroy(surf);
    ext_session_lock_v1_unlock_and_destroy(c1.lock);
    pump_roundtrip(&server, client_display);

    /* 验证会话恢复未锁定状态，锁屏遮罩已销毁 */
    assert(!woodland_session_lock_is_locked(&server));
    assert(server.session_lock->lock == NULL);
    assert(wl_list_empty(&server.session_lock->lock_outputs));

    /* 8. 测试遗弃锁（Abandoned Lock）保护与接管：
     * 客户端锁定后若崩溃/直接断开，合成器必须持续保持黑屏锁定状态，直至新锁屏客户端接管并解锁 */
    c1.lock = ext_session_lock_manager_v1_lock(c1.manager);
    c1.locked = false;
    ext_session_lock_v1_add_listener(c1.lock, &lock_listener, &c1);
    pump_roundtrip(&server, client_display);
    assert(woodland_session_lock_is_locked(&server));

    /* 客户端 1 异常退出（直接断开连接，模拟进程崩溃/挂掉） */
    wl_proxy_destroy((struct wl_proxy *)c1.lock);
    ext_session_lock_manager_v1_destroy(c1.manager);
    wl_compositor_destroy(c1.compositor);
    wl_output_destroy(c1.output);
    wl_registry_destroy(reg);
    if (c1.shm)
        wl_shm_destroy(c1.shm);
    wl_display_disconnect(client_display);
    wl_client_destroy(client);
    wl_event_loop_dispatch(server.event_loop, 0);
    assert(woodland_session_lock_is_locked(&server));
    assert(server.session_lock->lock == NULL);
    assert(!wl_list_empty(&server.session_lock->lock_outputs));

    /* 新客户端接入接管遗弃锁 */
    int fds3[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds3) == 0);
    struct wl_client *client3 = wl_client_create(server.display, fds3[0]);
    assert(client3 != NULL);
    struct wl_display *client3_display = wl_display_connect_to_fd(fds3[1]);
    assert(client3_display != NULL);

    struct client_state c3 = {0};
    struct wl_registry *reg3 = wl_display_get_registry(client3_display);
    wl_registry_add_listener(reg3, &registry_listener, &c3);
    pump_roundtrip(&server, client3_display);

    c3.lock = ext_session_lock_manager_v1_lock(c3.manager);
    ext_session_lock_v1_add_listener(c3.lock, &lock_listener, &c3);
    pump_roundtrip(&server, client3_display);

    assert(c3.locked == true);
    assert(woodland_session_lock_is_locked(&server));

    /* 测试带有已映射锁屏表面的会话在 shutdown 时直接调用 woodland_session_lock_finish 不崩溃 */
    struct wl_surface *surf3 = wl_compositor_create_surface(c3.compositor);
    c3.lock_surface = ext_session_lock_v1_get_lock_surface(c3.lock, surf3, c3.output);
    ext_session_lock_surface_v1_add_listener(c3.lock_surface, &surface_listener, &c3);
    pump_roundtrip(&server, client3_display);
    struct wl_buffer *buf3 = create_shm_buffer(c3.shm, c3.configure_width, c3.configure_height);
    wl_surface_attach(surf3, buf3, 0, 0);
    wl_surface_commit(surf3);
    pump_roundtrip(&server, client3_display);
    assert(server.session_lock->focused_surface != NULL);

    woodland_session_lock_finish(&server);
    assert(server.session_lock == NULL);
    wl_proxy_destroy((struct wl_proxy *)c3.lock_surface);
    wl_buffer_destroy(buf3);
    wl_surface_destroy(surf3);
    wl_proxy_destroy((struct wl_proxy *)c3.lock);
    /* 9. 清理所有客户端与服务端对象 */
    ext_session_lock_manager_v1_destroy(c3.manager);
    wl_compositor_destroy(c3.compositor);
    wl_output_destroy(c3.output);
    wl_registry_destroy(reg3);
    if (c3.shm)
        wl_shm_destroy(c3.shm);
    wl_display_disconnect(client3_display);
    wl_client_destroy(client3);

    wlr_keyboard_finish(&keyboard);

    woodland_cursor_finish_events(&server);
    woodland_shell_finish(&server);
    woodland_seat_finish(&server);
    woodland_cursor_finish(&server);
    woodland_output_finish(&server);

    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_allocator_destroy(server.allocator);
    server.allocator = NULL;
    wlr_renderer_destroy(server.renderer);
    server.renderer = NULL;
    wlr_backend_destroy(server.backend);
    server.backend = NULL;
    wl_display_destroy(server.display);
    server.display = NULL;

    return 0;
}
