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

#include "treeland-dde-shell-unstable-v2-client-protocol.h"
#include "dde-shell-xdg-shell-client-protocol.h"

struct probe_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct treeland_dde_shell_manager_v2 *manager;

    uint32_t last_role;
    uint32_t role_event_count;
    bool role_received;
};

static void role_callback(void *data,
                          struct treeland_dde_shell_surface_v2 *surface,
                          uint32_t role)
{
    (void)surface;
    struct probe_state *st = data;
    st->last_role = role;
    st->role_event_count++;
    st->role_received = true;
}

static const struct treeland_dde_shell_surface_v2_listener surface_listener = {
    .role = role_callback,
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
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!st->output) {
            st->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(st->wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, treeland_dde_shell_manager_v2_interface.name) == 0) {
        uint32_t bind_ver = version < 2 ? version : 2;
        st->manager = wl_registry_bind(registry, name,
                                       &treeland_dde_shell_manager_v2_interface, bind_ver);
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
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memset(data, 0x80, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(data, size);
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
    if (!st->manager) {
        fprintf(stderr, "treeland_dde_shell_manager_v2 not found\n");
        return 1;
    }
    uint32_t ver = treeland_dde_shell_manager_v2_get_version(st->manager);
    if (ver != 2) {
        fprintf(stderr, "expected version 2, got %u\n", ver);
        return 1;
    }
    treeland_dde_shell_manager_v2_destroy(st->manager);
    st->manager = NULL;
    return wl_display_roundtrip(st->display) < 0 ? 1 : 0;
}

static int run_lifecycle(struct probe_state *st)
{
    if (!st->manager || !st->compositor) {
        fprintf(stderr, "manager or compositor missing\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(st->compositor);
    if (!surface) {
        return 1;
    }
    struct treeland_dde_shell_surface_v2 *shell_surf =
        treeland_dde_shell_manager_v2_get_shell_surface(st->manager, surface);
    if (!shell_surf) {
        wl_surface_destroy(surface);
        return 1;
    }
    treeland_dde_shell_surface_v2_add_listener(shell_surf, &surface_listener, st);

    /* 1. Roundtrip to get initial role event (OVERLAY = 1) */
    st->role_received = false;
    if (wl_display_roundtrip(st->display) < 0 || !st->role_received || st->last_role != 1) {
        fprintf(stderr, "initial role event failed (count=%u role=%u)\n",
                st->role_event_count, st->last_role);
        return 1;
    }

    /* 2. set_role(0) NORMAL -> should receive role event 0 */
    st->role_received = false;
    treeland_dde_shell_surface_v2_set_role(shell_surf, 0);
    if (wl_display_roundtrip(st->display) < 0 || !st->role_received || st->last_role != 0) {
        fprintf(stderr, "set_role normal failed\n");
        return 1;
    }

    /* 3. set_role(2) ALWAYS_ON_TOP -> should receive role event 2 */
    st->role_received = false;
    treeland_dde_shell_surface_v2_set_role(shell_surf, 2);
    if (wl_display_roundtrip(st->display) < 0 || !st->role_received || st->last_role != 2) {
        fprintf(stderr, "set_role always_on_top failed\n");
        return 1;
    }

    /* 4. set_role(2) again -> should NOT receive duplicate event */
    uint32_t cur_count = st->role_event_count;
    treeland_dde_shell_surface_v2_set_role(shell_surf, 2);
    if (wl_display_roundtrip(st->display) < 0 || st->role_event_count != cur_count) {
        fprintf(stderr, "duplicate role event fired\n");
        return 1;
    }

    /* 5. Position hints */
    treeland_dde_shell_surface_v2_set_position_hint(shell_surf, st->output, 42, 24);
    treeland_dde_shell_surface_v2_set_position_hint(shell_surf, NULL, 10, 20);
    treeland_dde_shell_surface_v2_set_cursor_placement_hint(shell_surf, 15, -30);
    treeland_dde_shell_surface_v2_set_skip_flags(shell_surf, 0x5);
    treeland_dde_shell_surface_v2_set_accept_keyboard_focus(shell_surf, 0);

    if (wl_display_roundtrip(st->display) < 0) {
        fprintf(stderr, "hints roundtrip failed\n");
        return 1;
    }

    /* 6. Destroy shell surface */
    treeland_dde_shell_surface_v2_destroy(shell_surf);
    if (wl_display_roundtrip(st->display) < 0) {
        fprintf(stderr, "destroy roundtrip failed\n");
        return 1;
    }

    /* 7. Recreate on same surface */
    shell_surf = treeland_dde_shell_manager_v2_get_shell_surface(st->manager, surface);
    if (!shell_surf) {
        fprintf(stderr, "recreate failed\n");
        return 1;
    }
    treeland_dde_shell_surface_v2_add_listener(shell_surf, &surface_listener, st);
    st->role_received = false;
    if (wl_display_roundtrip(st->display) < 0 || !st->role_received || st->last_role != 1) {
        fprintf(stderr, "recreated role event failed\n");
        return 1;
    }

    treeland_dde_shell_surface_v2_destroy(shell_surf);
    wl_surface_destroy(surface);
    wl_display_roundtrip(st->display);
    return 0;
}

static int run_duplicate_error(struct probe_state *st)
{
    if (!st->manager || !st->compositor) {
        return 1;
    }
    struct wl_surface *surface = wl_compositor_create_surface(st->compositor);
    if (!surface) {
        return 1;
    }
    struct treeland_dde_shell_surface_v2 *surf1 =
        treeland_dde_shell_manager_v2_get_shell_surface(st->manager, surface);
    if (!surf1) {
        wl_surface_destroy(surface);
        return 1;
    }
    wl_display_roundtrip(st->display);

    /* Second get_shell_surface on same surface must trigger protocol error */
    treeland_dde_shell_manager_v2_get_shell_surface(st->manager, surface);
    int res = wl_display_roundtrip(st->display);
    /* Expect connection to be severed with error */
    if (res < 0) {
        printf("duplicate_error: severed as expected\n");
        return 0;
    }
    fprintf(stderr, "expected protocol error did not terminate connection\n");
    return 1;
}

static int run_mapped_window(struct probe_state *st, uint32_t role, int x, int y,
                             uint32_t skip_flags, bool accept_focus, int duration_sec)
{
    if (!st->manager || !st->compositor || !st->shm || !st->wm_base) {
        fprintf(stderr, "missing prerequisites for mapped window\n");
        return 1;
    }

    struct wl_surface *surface = wl_compositor_create_surface(st->compositor);
    struct treeland_dde_shell_surface_v2 *shell_surf =
        treeland_dde_shell_manager_v2_get_shell_surface(st->manager, surface);
    treeland_dde_shell_surface_v2_add_listener(shell_surf, &surface_listener, st);

    treeland_dde_shell_surface_v2_set_role(shell_surf, role);
    if (x != 0 || y != 0) {
        treeland_dde_shell_surface_v2_set_position_hint(shell_surf, NULL, x, y);
    }
    if (skip_flags != 0) {
        treeland_dde_shell_surface_v2_set_skip_flags(shell_surf, skip_flags);
    }
    if (!accept_focus) {
        treeland_dde_shell_surface_v2_set_accept_keyboard_focus(shell_surf, 0);
    }

    struct xdg_surface *xdg_surf = xdg_wm_base_get_xdg_surface(st->wm_base, surface);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "dde_shell_window");
    xdg_toplevel_set_app_id(toplevel, "dde.shell.probe");

    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    struct wl_buffer *buffer = create_shm_buffer(st->shm, 120, 80);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(st->display);

    if (duration_sec > 0) {
        sleep((unsigned int)duration_sec);
    }

    wl_buffer_destroy(buffer);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    treeland_dde_shell_surface_v2_destroy(shell_surf);
    wl_surface_destroy(surface);
    wl_display_roundtrip(st->display);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <--bind|--lifecycle|--duplicate-error|--mapped role x y skip accept duration>\n",
                argv[0]);
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
    wl_display_roundtrip(st.display);

    int rc = 0;
    if (strcmp(argv[1], "--bind") == 0) {
        rc = run_bind(&st);
    } else if (strcmp(argv[1], "--lifecycle") == 0) {
        rc = run_lifecycle(&st);
    } else if (strcmp(argv[1], "--duplicate-error") == 0) {
        rc = run_duplicate_error(&st);
    } else if (strcmp(argv[1], "--mapped") == 0) {
        uint32_t role = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
        int x = argc > 3 ? atoi(argv[3]) : 0;
        int y = argc > 4 ? atoi(argv[4]) : 0;
        uint32_t skip = argc > 5 ? (uint32_t)atoi(argv[5]) : 0;
        bool accept = argc > 6 ? (atoi(argv[6]) != 0) : true;
        int duration = argc > 7 ? atoi(argv[7]) : 1;
        rc = run_mapped_window(&st, role, x, y, skip, accept, duration);
    } else {
        fprintf(stderr, "unknown option: %s\n", argv[1]);
        rc = 1;
    }

    if (st.manager) {
        treeland_dde_shell_manager_v2_destroy(st.manager);
    }
    if (st.wm_base) {
        xdg_wm_base_destroy(st.wm_base);
    }
    if (st.output) {
        wl_output_destroy(st.output);
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
    if (st.display) {
        wl_display_disconnect(st.display);
    }
    return rc;
}
