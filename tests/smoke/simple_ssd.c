// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "simple-ssd-xdg-decoration-unstable-v1-client-protocol.h"
#include "simple-ssd-xdg-shell-client-protocol.h"

#define WIDTH 250
#define HEIGHT 250

static const char *opt_title = "simple-ssd";
static bool opt_log_keys = false;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct zxdg_toplevel_decoration_v1 *toplevel_deco;
static int configured = 0;

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surf, serial);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel_, int32_t w, int32_t h,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel_;
    (void)w;
    (void)h;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel_)
{
    (void)data;
    (void)toplevel_;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
    (void)data;
    (void)deco;
    (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                       int32_t fd, uint32_t size)
{
    (void)data;
    (void)k;
    (void)format;
    (void)size;
    if (fd >= 0) {
        close(fd);
    }
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *surf, struct wl_array *keys)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)surf;
    (void)keys;
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *surf)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)surf;
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)time;
    printf("KEY %u %u\n", key, state);
    fflush(stdout);
}

static void kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                          uint32_t mods_depressed, uint32_t mods_latched,
                          uint32_t mods_locked, uint32_t group)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void kbd_repeat_info(void *data, struct wl_keyboard *k,
                            int32_t rate, int32_t delay)
{
    (void)data;
    (void)k;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
        wl_keyboard_release(keyboard);
        keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
    (void)data;
    (void)s;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        deco_mgr = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (opt_log_keys && strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t v = version < 5 ? version : 5;
        seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
        wl_seat_add_listener(seat, &seat_listener, NULL);
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

static struct wl_buffer *create_buffer(void)
{
    int stride = WIDTH * 4;
    int size = stride * HEIGHT;
    int fd = memfd_create("simple-ssd", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        data[i] = 0xFF333333; /* 灰色背景 */
    }
    munmap(data, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log-keys") == 0) {
            opt_log_keys = true;
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            opt_title = argv[++i];
        }
    }
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "missing compositor / shm / wm_base\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, opt_title);
    xdg_toplevel_set_app_id(xdg_toplevel, "org.woodland.simple-ssd");

    if (deco_mgr) {
        toplevel_deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr, xdg_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(toplevel_deco, &deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(toplevel_deco,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(surface);

    while (!configured) {
        if (wl_display_dispatch(display) < 0) {
            return 1;
        }
    }

    struct wl_buffer *buffer = create_buffer();
    if (!buffer) {
        fprintf(stderr, "cannot create buffer\n");
        return 1;
    }

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    wl_display_flush(display);

    while (wl_display_dispatch(display) >= 0) {
    }

    return 0;
}
