// SPDX-License-Identifier: GPL-2.0-only
/*
 * wp-security-context-v1 冒烟探针。
 *
 * 流程：
 *   1. 作为沙箱引擎（父客户端）接入主 Wayland 显示，校验普通客户端能正常见到
 *      wp_security_context_manager_v1 及特权全局（且看不到 xwayland_shell_v1）；
 *   2. 创建监听 socket 与 close_fd，通过 wp_security_context_manager_v1 注册沙箱上下文；
 *   3. 作为沙箱客户端连接该监听 socket；
 *   4. 沙箱客户端验证全局接口过滤（必须看到 zwp_keyboard_shortcuts_inhibit_manager_v1，
 *      必须屏蔽 wp_security_context_manager_v1 与其他全部特权接口）；
 *   5. 沙箱客户端创建一个 xdg toplevel 窗口，输出 SANDBOX_READY 供脚本断言；
 *   6. 保持运行直到被终止。
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>

#include "sec-ctx-client-protocol.h"
#include "sec-ctx-xdg-shell-client-protocol.h"

#define WIDTH 200
#define HEIGHT 200

static struct wp_security_context_manager_v1 *parent_sec_ctx_manager;
static bool parent_saw_layer_shell;
static bool parent_saw_screencopy;
static bool parent_saw_data_control;
static bool parent_saw_output_mgr;
static bool parent_saw_session_lock;
static bool parent_saw_ftl_list;
static bool parent_saw_workspace_mgr;
static bool parent_saw_shortcuts_inhibit;
static bool parent_saw_treeland_ftl;
static bool parent_saw_treeland_shortcut;
static bool parent_saw_xwayland_shell;

static void parent_registry_global(void *data, struct wl_registry *reg, uint32_t name,
                                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wp_security_context_manager_v1_interface.name) == 0) {
        parent_sec_ctx_manager =
            wl_registry_bind(reg, name, &wp_security_context_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        parent_saw_layer_shell = true;
    } else if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0) {
        parent_saw_screencopy = true;
    } else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        parent_saw_data_control = true;
    } else if (strcmp(interface, "zwlr_output_manager_v1") == 0) {
        parent_saw_output_mgr = true;
    } else if (strcmp(interface, "ext_session_lock_manager_v1") == 0) {
        parent_saw_session_lock = true;
    } else if (strcmp(interface, "ext_foreign_toplevel_list_v1") == 0) {
        parent_saw_ftl_list = true;
    } else if (strcmp(interface, "ext_workspace_manager_v1") == 0) {
        parent_saw_workspace_mgr = true;
    } else if (strcmp(interface, "zwp_keyboard_shortcuts_inhibit_manager_v1") == 0) {
        parent_saw_shortcuts_inhibit = true;
    } else if (strcmp(interface, "treeland_foreign_toplevel_manager_v3") == 0) {
        parent_saw_treeland_ftl = true;
    } else if (strcmp(interface, "treeland_shortcut_manager_v3") == 0) {
        parent_saw_treeland_shortcut = true;
    } else if (strcmp(interface, "xwayland_shell_v1") == 0) {
        parent_saw_xwayland_shell = true;
    }
}

static void parent_registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener parent_registry_listener = {
    .global = parent_registry_global,
    .global_remove = parent_registry_global_remove,
};

/* ==================== 沙箱客户端状态 ==================== */

static struct wl_compositor *child_compositor;
static struct wl_shm *child_shm;
static struct xdg_wm_base *child_wm_base;

static bool child_saw_sec_ctx;
static bool child_saw_layer_shell;
static bool child_saw_screencopy;
static bool child_saw_data_control;
static bool child_saw_output_mgr;
static bool child_saw_session_lock;
static bool child_saw_ftl_list;
static bool child_saw_workspace_mgr;
static bool child_saw_treeland_ftl;
static bool child_saw_treeland_shortcut;
static bool child_saw_xwayland_shell;
static bool child_saw_shortcuts_inhibit;
static bool child_configured;

static void child_registry_global(void *data, struct wl_registry *reg, uint32_t name,
                                  const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, "wp_security_context_manager_v1") == 0) {
        child_saw_sec_ctx = true;
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        child_saw_layer_shell = true;
    } else if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0) {
        child_saw_screencopy = true;
    } else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        child_saw_data_control = true;
    } else if (strcmp(interface, "zwlr_output_manager_v1") == 0) {
        child_saw_output_mgr = true;
    } else if (strcmp(interface, "ext_session_lock_manager_v1") == 0) {
        child_saw_session_lock = true;
    } else if (strcmp(interface, "ext_foreign_toplevel_list_v1") == 0) {
        child_saw_ftl_list = true;
    } else if (strcmp(interface, "ext_workspace_manager_v1") == 0) {
        child_saw_workspace_mgr = true;
    } else if (strcmp(interface, "treeland_foreign_toplevel_manager_v3") == 0) {
        child_saw_treeland_ftl = true;
    } else if (strcmp(interface, "treeland_shortcut_manager_v3") == 0) {
        child_saw_treeland_shortcut = true;
    } else if (strcmp(interface, "xwayland_shell_v1") == 0) {
        child_saw_xwayland_shell = true;
    } else if (strcmp(interface, "zwp_keyboard_shortcuts_inhibit_manager_v1") == 0) {
        child_saw_shortcuts_inhibit = true;
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        child_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        child_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        child_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    }
}

static void child_registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener child_registry_listener = {
    .global = child_registry_global,
    .global_remove = child_registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    child_configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)data;
    (void)t;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm)
{
    int stride = WIDTH * 4;
    int size = stride * HEIGHT;
    int fd = memfd_create("sec-ctx-probe-buf", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memset(data, 0x40, (size_t)size);
    munmap(data, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

int main(void)
{
    /* 1. 父客户端接入 */
    struct wl_display *parent_display = wl_display_connect(NULL);
    if (!parent_display) {
        fprintf(stderr, "failed to connect to parent display\n");
        return 1;
    }

    struct wl_registry *parent_registry = wl_display_get_registry(parent_display);
    wl_registry_add_listener(parent_registry, &parent_registry_listener, NULL);
    wl_display_roundtrip(parent_display);
    wl_display_roundtrip(parent_display);

    if (!parent_sec_ctx_manager || !parent_saw_layer_shell || !parent_saw_screencopy ||
        !parent_saw_data_control || !parent_saw_output_mgr || !parent_saw_session_lock ||
        !parent_saw_ftl_list || !parent_saw_workspace_mgr || !parent_saw_shortcuts_inhibit ||
        !parent_saw_treeland_ftl || !parent_saw_treeland_shortcut) {
        fprintf(stderr, "FAIL: normal parent client missing expected privileged globals\n");
        return 1;
    }
    if (parent_saw_xwayland_shell) {
        fprintf(stderr, "FAIL: normal parent client unexpectedly saw xwayland_shell_v1\n");
        return 1;
    }

    /* 2. 创建监听 socket 并注册沙箱安全上下文 */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "sec-ctx-smoke-%d", getpid());
    socklen_t addr_len = (socklen_t)(sizeof(sa_family_t) + 1 + strlen(addr.sun_path + 1));
    if (bind(listen_fd, (struct sockaddr *)&addr, addr_len) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 1) != 0) {
        perror("listen");
        return 1;
    }

    int close_fds[2];
    if (pipe2(close_fds, O_CLOEXEC) != 0) {
        perror("pipe2");
        return 1;
    }

    struct wp_security_context_v1 *sec_ctx =
        wp_security_context_manager_v1_create_listener(parent_sec_ctx_manager, listen_fd, close_fds[0]);
    if (!sec_ctx) {
        fprintf(stderr, "failed to create security context listener\n");
        return 1;
    }
    wp_security_context_v1_set_sandbox_engine(sec_ctx, "org.flatpak");
    wp_security_context_v1_set_app_id(sec_ctx, "org.example.SecCtxSmoke");
    wp_security_context_v1_set_instance_id(sec_ctx, "smoke-42");
    wp_security_context_v1_commit(sec_ctx);
    wp_security_context_v1_destroy(sec_ctx);
    wl_display_roundtrip(parent_display);

    /* 3. 沙箱客户端连接 */
    int child_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (child_fd < 0) {
        perror("child socket");
        return 1;
    }
    if (connect(child_fd, (struct sockaddr *)&addr, addr_len) != 0) {
        perror("child connect");
        return 1;
    }

    struct wl_display *child_display = wl_display_connect_to_fd(child_fd);
    if (!child_display) {
        fprintf(stderr, "failed to connect child display to fd\n");
        return 1;
    }

    struct wl_registry *child_registry = wl_display_get_registry(child_display);
    wl_registry_add_listener(child_registry, &child_registry_listener, NULL);
    wl_display_roundtrip(child_display);
    wl_display_roundtrip(child_display);

    /* 4. 校验过滤规则 */
    if (child_saw_sec_ctx || child_saw_layer_shell || child_saw_screencopy ||
        child_saw_data_control || child_saw_output_mgr || child_saw_session_lock ||
        child_saw_ftl_list || child_saw_workspace_mgr || child_saw_treeland_ftl ||
        child_saw_treeland_shortcut || child_saw_xwayland_shell) {
        fprintf(stderr,
                "FAIL: sandboxed client saw privileged globals "
                "(sec=%d layer=%d screen=%d data=%d out=%d lock=%d ftl=%d ws=%d tftl=%d tsc=%d xwl=%d)\n",
                child_saw_sec_ctx, child_saw_layer_shell, child_saw_screencopy,
                child_saw_data_control, child_saw_output_mgr, child_saw_session_lock,
                child_saw_ftl_list, child_saw_workspace_mgr, child_saw_treeland_ftl,
                child_saw_treeland_shortcut, child_saw_xwayland_shell);
        return 2;
    }
    if (!child_compositor || !child_shm || !child_wm_base || !child_saw_shortcuts_inhibit) {
        fprintf(stderr,
                "FAIL: sandboxed client missing allowed globals "
                "(comp=%d shm=%d wm_base=%d shortcuts_inhibit=%d)\n",
                child_compositor != NULL, child_shm != NULL, child_wm_base != NULL,
                child_saw_shortcuts_inhibit);
        return 2;
    }

    /* 5. 创建 xdg toplevel 窗口 */
    xdg_wm_base_add_listener(child_wm_base, &wm_base_listener, NULL);
    struct wl_surface *surface = wl_compositor_create_surface(child_compositor);
    struct xdg_surface *xdg_surf = xdg_wm_base_get_xdg_surface(child_wm_base, surface);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Sandboxed Window");
    xdg_toplevel_set_app_id(toplevel, "org.example.SecCtxSmoke");

    wl_surface_commit(surface);
    wl_display_roundtrip(child_display);

    while (!child_configured) {
        if (wl_display_dispatch(child_display) < 0) {
            break;
        }
    }

    struct wl_buffer *buf = create_shm_buffer(child_shm);
    if (!buf) {
        fprintf(stderr, "failed to create shm buffer\n");
        return 1;
    }
    wl_surface_attach(surface, buf, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    wl_display_roundtrip(child_display);

    /* 6. 通知测试脚本就绪 */
    printf("SANDBOX_READY\n");
    fflush(stdout);

    /* 保持运行并处理事件循环，直到收到 SIGTERM */
    struct pollfd pfd = {
        .fd = wl_display_get_fd(child_display),
        .events = POLLIN,
    };
    while (true) {
        while (wl_display_prepare_read(child_display) != 0) {
            if (wl_display_dispatch_pending(child_display) < 0) {
                goto out;
            }
        }
        wl_display_flush(child_display);
        if (poll(&pfd, 1, 2000) <= 0) {
            wl_display_cancel_read(child_display);
            break;
        }
        if (wl_display_read_events(child_display) < 0) {
            break;
        }
        if (wl_display_dispatch_pending(child_display) < 0) {
            break;
        }
    }

out:
    close(close_fds[1]);
    wl_buffer_destroy(buf);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surf);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(child_wm_base);
    wl_shm_destroy(child_shm);
    wl_compositor_destroy(child_compositor);
    wl_registry_destroy(child_registry);
    wl_display_disconnect(child_display);

    wp_security_context_manager_v1_destroy(parent_sec_ctx_manager);
    wl_registry_destroy(parent_registry);
    wl_display_disconnect(parent_display);

    return 0;
}
