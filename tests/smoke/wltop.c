// SPDX-License-Identifier: GPL-2.0-only
/*
 * wltop - list Wayland toplevels, for testing and debugging woodland.
 *
 * A small, dependency-light replacement for `lswt` in the smoke tests. It
 * binds ext-foreign-toplevel-list-v1 and prints a one-shot snapshot of every
 * mapped toplevel: app_id, title and the ext protocol identifier (the same
 * token `grim -T` consumes).
 *
 * Why a snapshot instead of a live watcher: the smoke scripts call the tool
 * repeatedly in a polling loop. Each invocation connects, grabs the current
 * list and exits, so there is never a half-filled snapshot to parse and no
 * long-lived process to clean up.
 *
 * Output modes:
 *   default            human-readable table, for interactive debugging
 *   -j / --json        JSON array compatible with `lswt -j`; each object has
 *                      app_id / title / identifier. This is the mode the
 *                      capture and xwayland smoke scripts consume.
 *
 * Useful options:
 *   -1 / --one         exit after the first toplevel (cheap "is anything up")
 *   --wait-for APP_ID  block until a toplevel with this app_id shows up;
 *                      implies re-polling every 100ms until --timeout
 *   -t / --timeout MS  overall deadline for --wait-for (default 5000)
 *   -q / --quiet       suppress the header in table mode
 *
 * Exit codes:
 *   0  at least one toplevel matched (or, without --wait-for, snapshot taken)
 *   1  no toplevel / timed out waiting / runtime error
 *   77 ext-foreign-toplevel-list-v1 is not advertised (treat as SKIP)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#define DEFAULT_TIMEOUT_MS 5000

struct toplevel {
    struct wl_list link;
    struct ext_foreign_toplevel_handle_v1 *handle;
    char *app_id;
    char *title;
    char *identifier;
    bool done;
    bool closed;
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct ext_foreign_toplevel_list_v1 *list;
static struct wl_list toplevels; /* struct toplevel.link */
static bool list_done;

static bool json_mode;
static bool one_shot;
static bool quiet;
static const char *wait_app_id;
static int timeout_ms = DEFAULT_TIMEOUT_MS;

/* ==================== helpers ==================== */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* JSON string escaping: quote + backslash + control chars. The identifier is
 * printable ASCII per protocol, but escape it anyway for safety. */
static void print_json_string(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p < 0x20) {
                printf("\\u%04x", *p);
            } else {
                putchar(*p);
            }
        }
    }
    putchar('"');
}

/* ==================== handle events ==================== */

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *title)
{
    struct toplevel *t = data;
    (void)h;
    free(t->title);
    t->title = strdup(title ? title : "");
}

static void handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
                          const char *app_id)
{
    struct toplevel *t = data;
    (void)h;
    free(t->app_id);
    t->app_id = strdup(app_id ? app_id : "");
}

static void handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h,
                              const char *identifier)
{
    struct toplevel *t = data;
    (void)h;
    free(t->identifier);
    t->identifier = strdup(identifier ? identifier : "");
}

static void handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *h)
{
    struct toplevel *t = data;
    (void)h;
    t->done = true;
}

static void handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h)
{
    struct toplevel *t = data;
    (void)h;
    t->closed = true;
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = handle_closed,
    .done = handle_done,
    .title = handle_title,
    .app_id = handle_app_id,
    .identifier = handle_identifier,
};

/* ==================== list events ==================== */

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
                          struct ext_foreign_toplevel_handle_v1 *new_handle)
{
    (void)data;
    (void)l;
    struct toplevel *t = calloc(1, sizeof(*t));
    if (!t) {
        return;
    }
    t->handle = new_handle;
    wl_list_insert(&toplevels, &t->link);
    ext_foreign_toplevel_handle_v1_add_listener(new_handle, &handle_listener, t);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l)
{
    (void)data;
    (void)l;
    list_done = true;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

/* ==================== registry ==================== */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        /* Never bind a version newer than the generated bindings understand;
         * otherwise the compositor could send events we cannot demarshal. */
        uint32_t supported = (uint32_t)ext_foreign_toplevel_list_v1_interface.version;
        uint32_t v = version < supported ? version : supported;
        if (v < 1) {
            v = 1;
        }
        list = wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, v);
        ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ==================== state ==================== */

static void toplevels_clear(void)
{
    struct toplevel *t, *tmp;
    wl_list_for_each_safe(t, tmp, &toplevels, link)
    {
        wl_list_remove(&t->link);
        if (t->handle) {
            ext_foreign_toplevel_handle_v1_destroy(t->handle);
        }
        free(t->app_id);
        free(t->title);
        free(t->identifier);
        free(t);
    }
}

static bool matches_wait(const struct toplevel *t)
{
    return t->done && !t->closed && wait_app_id && t->app_id &&
           strcmp(t->app_id, wait_app_id) == 0;
}

/* A toplevel is part of the printable snapshot only when its properties are
 * complete. With --wait-for, restrict output to the requested app_id so the
 * printed result agrees with the exit status (a miss prints an empty array). */
static bool should_print(const struct toplevel *t)
{
    if (!t->done || t->closed) {
        return false;
    }
    if (wait_app_id && (!t->app_id || strcmp(t->app_id, wait_app_id) != 0)) {
        return false;
    }
    return true;
}

static bool any_printable(void)
{
    struct toplevel *t;
    wl_list_for_each(t, &toplevels, link)
    {
        if (should_print(t)) {
            return true;
        }
    }
    return false;
}

static bool any_match(void)
{
    struct toplevel *t;
    wl_list_for_each(t, &toplevels, link)
    {
        if (matches_wait(t)) {
            return true;
        }
    }
    return false;
}

/* Drive the event loop until the initial `finished` burst has been pumped
 * (wlroots sends every handle's details + `done` before `finished`, so this is
 * a complete snapshot) and, in --wait-for mode, the target has shown up — or
 * the deadline passes. Using `finished` rather than "any handle ready" avoids
 * emitting a partial snapshot on compositors that deliver events in batches.
 * Returns 0 on success, -1 on error. */
static int settle(long deadline_ms)
{
    for (;;) {
        if (wl_display_prepare_read(display) == 0) {
            wl_display_flush(display);
            struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
            long remaining = deadline_ms - now_ms();
            if (remaining < 0) {
                remaining = 0;
            }
            int r = poll(&pfd, 1, remaining > 100 ? 100 : (int)remaining);
            if (r > 0) {
                if (wl_display_read_events(display) != 0) {
                    return -1;
                }
            } else {
                wl_display_cancel_read(display);
            }
        }
        if (wl_display_dispatch_pending(display) < 0 || wl_display_get_error(display) != 0) {
            return -1;
        }
        if ((list_done && (!wait_app_id || any_match())) || now_ms() >= deadline_ms) {
            return 0;
        }
    }
}

static void print_snapshot(void)
{
    struct toplevel *t;
    if (json_mode) {
        putchar('[');
        bool first = true;
        wl_list_for_each(t, &toplevels, link)
        {
            if (!should_print(t)) {
                continue;
            }
            printf("%s\n  {\n    \"app_id\": ", first ? "" : ",");
            print_json_string(t->app_id);
            printf(",\n    \"title\": ");
            print_json_string(t->title);
            printf(",\n    \"identifier\": ");
            print_json_string(t->identifier);
            printf("\n  }");
            first = false;
            if (one_shot) {
                break;
            }
        }
        printf("\n]\n");
    } else {
        if (!quiet) {
            printf("%-28s %-32s %s\n", "APP_ID", "TITLE", "IDENTIFIER");
            printf("%-28s %-32s %s\n", "------", "-----", "----------");
        }
        wl_list_for_each(t, &toplevels, link)
        {
            if (!should_print(t)) {
                continue;
            }
            printf("%-28.28s %-32.32s %s\n", t->app_id ? t->app_id : "-",
                   t->title ? t->title : "", t->identifier ? t->identifier : "-");
            if (one_shot) {
                break;
            }
        }
    }
    fflush(stdout);
}

/* ==================== main ==================== */

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0) {
            json_mode = true;
        } else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--one") == 0) {
            one_shot = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--wait-for") == 0 && i + 1 < argc) {
            wait_app_id = argv[++i];
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) &&
                   i + 1 < argc) {
            const char *arg = argv[++i];
            char *end;
            errno = 0;
            long ms = strtol(arg, &end, 10);
            if (errno != 0 || end == arg || *end != '\0' || ms < 0 || ms > 3600000) {
                fprintf(stderr, "wltop: invalid timeout: %s\n", arg);
                return 1;
            }
            timeout_ms = (int)ms;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            puts("usage: wltop [-j|--json] [-1|--one] [-q|--quiet]\n"
                 "             [--wait-for APP_ID] [-t|--timeout MS]\n"
                 "list Wayland toplevels (app_id, title, identifier) from\n"
                 "ext-foreign-toplevel-list-v1. Exit 0=ok, 1=none/timeout, 77=unsupported.");
            return 0;
        } else {
            fprintf(stderr, "wltop: unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    wl_list_init(&toplevels);

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "wltop: cannot connect to compositor\n");
        return 1;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    /* First roundtrip delivers globals; the second delivers the initial
     * toplevel burst queued in response to our bind (same two-roundtrip rule
     * the other probes in this tree use). */
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!list) {
        fprintf(stderr, "wltop: ext-foreign-toplevel-list-v1 not advertised\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 77;
    }

    long deadline = now_ms() + (wait_app_id ? timeout_ms : 500);
    if (settle(deadline) != 0) {
        fprintf(stderr, "wltop: display error while collecting toplevels\n");
        toplevels_clear();
        ext_foreign_toplevel_list_v1_destroy(list);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }

    /* Exit status agrees with what print_snapshot() emits: success means at
     * least one toplevel was actually printed (the requested one under
     * --wait-for). */
    int rc = any_printable() ? 0 : 1;
    print_snapshot();

    toplevels_clear();
    ext_foreign_toplevel_list_v1_destroy(list);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return rc;
}
