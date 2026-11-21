// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "treeland-wine-window-management-unstable-v1-client-protocol.h"
#include "treeland-wine-window-state-unstable-v1-client-protocol.h"
#include "wine-xdg-shell-client-protocol.h"

struct probe_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm_base;
    struct treeland_wine_window_manager_v1 *wm_manager;
    struct treeland_wine_window_state_manager_v1 *ws_manager;

    /* Window Control Events */
    uint32_t last_window_id;
    int window_id_count;
    int32_t last_pos_x;
    int32_t last_pos_y;
    uint32_t last_pos_serial;
    int configure_position_count;
    uint32_t last_topmost;
    int configure_stacking_count;

    /* Window State Events */
    uint32_t last_ws_state;
    int ws_state_count;
    uint32_t last_activate_denied_serial;
    int activate_denied_count;
};

static void handle_window_id(void *data,
                             struct treeland_wine_window_control_v1 *control,
                             uint32_t id)
{
    (void)control;
    struct probe_state *st = data;
    st->last_window_id = id;
    st->window_id_count++;
}

static void handle_configure_position(void *data,
                                      struct treeland_wine_window_control_v1 *control,
                                      int32_t x,
                                      int32_t y,
                                      uint32_t serial)
{
    (void)control;
    struct probe_state *st = data;
    st->last_pos_x = x;
    st->last_pos_y = y;
    st->last_pos_serial = serial;
    st->configure_position_count++;
}

static void handle_configure_stacking(void *data,
                                      struct treeland_wine_window_control_v1 *control,
                                      uint32_t topmost)
{
    (void)control;
    struct probe_state *st = data;
    st->last_topmost = topmost;
    st->configure_stacking_count++;
}

static const struct treeland_wine_window_control_v1_listener control_listener = {
    .window_id = handle_window_id,
    .configure_position = handle_configure_position,
    .configure_stacking = handle_configure_stacking,
};

static void handle_state_changed(void *data,
                                 struct treeland_wine_window_state_v1 *state,
                                 uint32_t ws_state)
{
    (void)state;
    struct probe_state *st = data;
    st->last_ws_state = ws_state;
    st->ws_state_count++;
}

static void handle_activate_denied(void *data,
                                   struct treeland_wine_window_state_v1 *state,
                                   uint32_t serial)
{
    (void)state;
    struct probe_state *st = data;
    st->last_activate_denied_serial = serial;
    st->activate_denied_count++;
}

static const struct treeland_wine_window_state_v1_listener state_listener = {
    .state_changed = handle_state_changed,
    .activate_denied = handle_activate_denied,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    struct probe_state *st = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        st->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!st->seat) {
            st->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(st->wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, treeland_wine_window_manager_v1_interface.name) == 0) {
        uint32_t bind_ver = version < 1 ? version : 1;
        st->wm_manager = wl_registry_bind(registry, name,
                                          &treeland_wine_window_manager_v1_interface, bind_ver);
    } else if (strcmp(interface, treeland_wine_window_state_manager_v1_interface.name) == 0) {
        uint32_t bind_ver = version < 1 ? version : 1;
        st->ws_manager = wl_registry_bind(registry, name,
                                          &treeland_wine_window_state_manager_v1_interface, bind_ver);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm, int width, int height)
{
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("shm-buffer", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }

    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    /* Fill solid red */
    for (int i = 0; i < width * height; i++) {
        data[i] = 0xFFFF0000;
    }
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                  int32_t width, int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static int run_bind(struct probe_state *st)
{
    if (!st->wm_manager) {
        fprintf(stderr, "treeland_wine_window_manager_v1 not found\n");
        return 1;
    }
    if (!st->ws_manager) {
        fprintf(stderr, "treeland_wine_window_state_manager_v1 not found\n");
        return 1;
    }

    uint32_t wm_ver = treeland_wine_window_manager_v1_get_version(st->wm_manager);
    uint32_t ws_ver = treeland_wine_window_state_manager_v1_get_version(st->ws_manager);
    if (wm_ver != 1 || ws_ver != 1) {
        fprintf(stderr, "version mismatch: wm=%u ws=%u\n", wm_ver, ws_ver);
        return 1;
    }

    treeland_wine_window_manager_v1_destroy(st->wm_manager);
    st->wm_manager = NULL;
    treeland_wine_window_state_manager_v1_destroy(st->ws_manager);
    st->ws_manager = NULL;
    return 0;
}

static int run_wm_test(struct probe_state *st)
{
    if (!st->wm_manager || !st->compositor || !st->shm || !st->wm_base) {
        fprintf(stderr, "missing globals for wm test\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(st->compositor);
    struct xdg_surface *xdg_surf = xdg_wm_base_get_xdg_surface(st->wm_base, surface);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);
    xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "wine_wm_test_window");

    struct treeland_wine_window_control_v1 *control =
        treeland_wine_window_manager_v1_get_window_control(st->wm_manager, toplevel);
    treeland_wine_window_control_v1_add_listener(control, &control_listener, st);

    /* Initial commit without buffer to trigger configure */
    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    /* Attach buffer and commit to map */
    struct wl_buffer *buffer = create_shm_buffer(st->shm, 200, 150);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    /* Verify initial sequence: window_id -> configure_position -> configure_stacking */
    if (st->window_id_count < 1 || st->last_window_id == 0) {
        fprintf(stderr, "failed to receive window_id\n");
        return 1;
    }
    if (st->configure_position_count < 1 || st->last_pos_serial != 0) {
        fprintf(stderr, "initial configure_position failed\n");
        return 1;
    }
    if (st->configure_stacking_count < 1 || st->last_topmost != 0) {
        fprintf(stderr, "initial configure_stacking failed\n");
        return 1;
    }

    /* Test set_position */
    treeland_wine_window_control_v1_set_position(control, 150, 120, 1);
    wl_display_roundtrip(st->display);
    if (st->last_pos_serial != 1 || st->last_pos_x != 150 || st->last_pos_y != 120) {
        fprintf(stderr, "set_position failed: serial=%u pos=(%d,%d)\n",
                st->last_pos_serial, st->last_pos_x, st->last_pos_y);
        return 1;
    }

    /* Test set_z_order topmost */
    treeland_wine_window_control_v1_set_z_order(control,
        TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_TOPMOST, 0);
    wl_display_roundtrip(st->display);
    if (st->last_topmost != 1) {
        fprintf(stderr, "set_z_order topmost failed: topmost=%u\n", st->last_topmost);
        return 1;
    }

    /* Test set_z_order notopmost */
    treeland_wine_window_control_v1_set_z_order(control,
        TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_NOTOPMOST, 0);
    wl_display_roundtrip(st->display);
    if (st->last_topmost != 0) {
        fprintf(stderr, "set_z_order notopmost failed: topmost=%u\n", st->last_topmost);
        return 1;
    }

    /* Test set_z_order bottom */
    treeland_wine_window_control_v1_set_z_order(control,
        TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_BOTTOM, 0);
    wl_display_roundtrip(st->display);
    if (st->last_topmost != 0) {
        fprintf(stderr, "set_z_order bottom failed: topmost=%u\n", st->last_topmost);
        return 1;
    }

    treeland_wine_window_control_v1_destroy(control);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    return 0;
}

static int run_ws_test(struct probe_state *st)
{
    if (!st->ws_manager || !st->compositor || !st->shm || !st->wm_base) {
        fprintf(stderr, "missing globals for ws test\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(st->compositor);
    struct xdg_surface *xdg_surf = xdg_wm_base_get_xdg_surface(st->wm_base, surface);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);
    xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "wine_ws_test_window");

    struct treeland_wine_window_state_v1 *ws =
        treeland_wine_window_state_manager_v1_get_window_state(st->ws_manager, toplevel);
    treeland_wine_window_state_v1_add_listener(ws, &state_listener, st);

    /* Initial commit without buffer to trigger configure */
    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    /* Attach buffer and commit to map */
    struct wl_buffer *buffer = create_shm_buffer(st->shm, 200, 150);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    /* Check initial state_changed event */
    if (st->ws_state_count < 1 || st->last_ws_state != 0) {
        fprintf(stderr, "initial state_changed failed: count=%d state=%u\n",
                st->ws_state_count, st->last_ws_state);
        return 1;
    }

    /* Test set_attention */
    st->ws_state_count = 0;
    treeland_wine_window_state_v1_set_attention(ws, 0, 0);
    wl_display_roundtrip(st->display);
    if (st->ws_state_count < 1 || !(st->last_ws_state & TREELAND_WINE_WINDOW_STATE_V1_STATE_ATTENTION)) {
        fprintf(stderr, "set_attention failed: state=%u\n", st->last_ws_state);
        return 1;
    }

    /* Test clear_attention */
    st->ws_state_count = 0;
    treeland_wine_window_state_v1_clear_attention(ws);
    wl_display_roundtrip(st->display);
    if (st->ws_state_count < 1 || (st->last_ws_state & TREELAND_WINE_WINDOW_STATE_V1_STATE_ATTENTION)) {
        fprintf(stderr, "clear_attention failed: state=%u\n", st->last_ws_state);
        return 1;
    }

    /* Test unminimize */
    xdg_toplevel_set_minimized(toplevel);
    wl_display_roundtrip(st->display);
    treeland_wine_window_state_v1_unminimize(ws);
    wl_display_roundtrip(st->display);
    if (st->last_ws_state & TREELAND_WINE_WINDOW_STATE_V1_STATE_MINIMIZED) {
        fprintf(stderr, "unminimize failed: state=%u\n", st->last_ws_state);
        return 1;
    }

    /* Test activate */
    treeland_wine_window_state_v1_activate(ws, 42,
        TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_USER_REQUEST,
        st->seat);
    wl_display_roundtrip(st->display);

    /* Test activate with RESTORE on minimized window */
    xdg_toplevel_set_minimized(toplevel);
    wl_display_roundtrip(st->display);
    treeland_wine_window_state_v1_activate(ws, 43,
        TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_RESTORE,
        st->seat);
    wl_display_roundtrip(st->display);
    if (st->last_ws_state & TREELAND_WINE_WINDOW_STATE_V1_STATE_MINIMIZED) {
        fprintf(stderr, "activate(RESTORE) failed to restore minimized window: state=%u\n",
                st->last_ws_state);
        return 1;
    }

    treeland_wine_window_state_v1_destroy(ws);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    return 0;
}

static int run_insert_after_test(struct probe_state *st)
{
    if (!st->wm_manager || !st->compositor || !st->shm || !st->wm_base) {
        return 1;
    }

    /* Create window A */
    struct wl_surface *surf_a = wl_compositor_create_surface(st->compositor);
    struct xdg_surface *xdg_a = xdg_wm_base_get_xdg_surface(st->wm_base, surf_a);
    struct xdg_toplevel *top_a = xdg_surface_get_toplevel(xdg_a);
    xdg_surface_add_listener(xdg_a, &xdg_surface_listener, NULL);
    xdg_toplevel_add_listener(top_a, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(top_a, "wine_window_a");
    struct treeland_wine_window_control_v1 *ctrl_a =
        treeland_wine_window_manager_v1_get_window_control(st->wm_manager, top_a);
    treeland_wine_window_control_v1_add_listener(ctrl_a, &control_listener, st);
    wl_surface_commit(surf_a);
    wl_display_roundtrip(st->display);
    struct wl_buffer *buf_a = create_shm_buffer(st->shm, 100, 100);
    wl_surface_attach(surf_a, buf_a, 0, 0);
    wl_surface_commit(surf_a);
    wl_display_roundtrip(st->display);
    uint32_t id_a = st->last_window_id;

    /* Create window B */
    struct wl_surface *surf_b = wl_compositor_create_surface(st->compositor);
    struct xdg_surface *xdg_b = xdg_wm_base_get_xdg_surface(st->wm_base, surf_b);
    struct xdg_toplevel *top_b = xdg_surface_get_toplevel(xdg_b);
    xdg_surface_add_listener(xdg_b, &xdg_surface_listener, NULL);
    xdg_toplevel_add_listener(top_b, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(top_b, "wine_window_b");
    struct treeland_wine_window_control_v1 *ctrl_b =
        treeland_wine_window_manager_v1_get_window_control(st->wm_manager, top_b);
    treeland_wine_window_control_v1_add_listener(ctrl_b, &control_listener, st);
    wl_surface_commit(surf_b);
    wl_display_roundtrip(st->display);
    struct wl_buffer *buf_b = create_shm_buffer(st->shm, 100, 100);
    wl_surface_attach(surf_b, buf_b, 0, 0);
    wl_surface_commit(surf_b);
    wl_display_roundtrip(st->display);

    /* Insert B after A (place B below A) */
    treeland_wine_window_control_v1_set_z_order(ctrl_b,
        TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_INSERT_AFTER, id_a);
    wl_display_roundtrip(st->display);

    treeland_wine_window_control_v1_destroy(ctrl_a);
    treeland_wine_window_control_v1_destroy(ctrl_b);
    xdg_toplevel_destroy(top_a);
    xdg_toplevel_destroy(top_b);
    xdg_surface_destroy(xdg_a);
    xdg_surface_destroy(xdg_b);
    wl_surface_destroy(surf_a);
    wl_surface_destroy(surf_b);
    wl_buffer_destroy(buf_a);
    wl_buffer_destroy(buf_b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--bind | --wm | --ws | --insert-after]\n", argv[0]);
        return 1;
    }

    struct probe_state st = {0};
    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "failed to connect to Wayland display\n");
        return 1;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(st.display);

    int ret = 0;
    if (strcmp(argv[1], "--bind") == 0) {
        ret = run_bind(&st);
    } else if (strcmp(argv[1], "--wm") == 0) {
        ret = run_wm_test(&st);
    } else if (strcmp(argv[1], "--ws") == 0) {
        ret = run_ws_test(&st);
    } else if (strcmp(argv[1], "--insert-after") == 0) {
        ret = run_insert_after_test(&st);
    } else {
        fprintf(stderr, "unknown arg: %s\n", argv[1]);
        ret = 1;
    }

    if (st.wm_manager) {
        treeland_wine_window_manager_v1_destroy(st.wm_manager);
    }
    if (st.ws_manager) {
        treeland_wine_window_state_manager_v1_destroy(st.ws_manager);
    }
    if (st.wm_base) {
        xdg_wm_base_destroy(st.wm_base);
    }
    if (st.seat) {
        wl_seat_destroy(st.seat);
    }
    if (st.shm) {
        wl_shm_destroy(st.shm);
    }
    if (st.compositor) {
        wl_compositor_destroy(st.compositor);
    }
    if (st.registry) {
        wl_registry_destroy(st.registry);
    }
    wl_display_disconnect(st.display);
    return ret;
}
