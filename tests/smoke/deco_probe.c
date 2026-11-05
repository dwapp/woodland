// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-decoration-unstable-v1 的客户端探针。
 *
 * 它自己不做断言——断言交给 wctl（run_deco_smoke.sh 里 jq 查 get_tree 的
 * deco/margins 字段）。探针只负责一件事：按命令行把"一个真实的客户端会做
 * 什么"做出来，然后撑住窗口不关，让脚本有东西可查。
 *
 * 为什么不用 weston-simple-shm：它不建装饰上下文，也不协商 xdg-decoration，
 * 而这个协议的全部行为都挂在"客户端主动接管装饰"上。
 *
 * 用法：
 *   deco_probe [操作...] [--hold 毫秒]
 *
 * 操作按顺序执行（都在 map 之前，正好覆盖"上下文早于 SSD 创建"的路径）：
 *   border <w> <r> <g> <b> <a>   set_border
 *   titlebar <0|1>               set_titlebar_mode（0=show 1=hide）
 *   radius <n>                   set_corner_radius
 *   shadow <r> <dx> <dy> <r> <g> <b> <a>   set_shadow
 *   dup                          再建一个 context（预期被 already_used 打掉）
 *
 * ext-background-effect-v1 相关：
 *   blur <x> <y> <w> <h>         get_background_effect + set_blur_region
 *   --caps                       打印 CAPS <flags>（bit0 = blur）
 *   --alpha <0-255>              buffer 用 ARGB8888，按该 alpha 预乘
 *   --pattern                    buffer 画彩条（不画纯灰：纯色模糊前后一样，看不出效果）
 *   --pad <n>                    边缘 n 像素全透明（客户端"声明得比画得大"的样子）
 *   --layer                      不建顶层窗口，改建一个 layer surface（面板/Dock 的形态）
 *
 * 退出码：0 = 正常；1 = 环境/协商失败；3 = 收到了协议错误（dup 用例）。
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "treeland-decoration-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define WIDTH 400
#define HEIGHT 300
#define DEFAULT_HOLD_MS 2000

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *xdg_deco_manager;
static struct treeland_decoration_manager_v1 *deco_manager;
static struct ext_background_effect_manager_v1 *bg_effect_manager;
static uint32_t bg_caps;
static int want_caps;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface_handle;
static struct zxdg_toplevel_decoration_v1 *xdg_deco;
static struct treeland_decoration_context_v1 *deco_ctx;
static struct ext_background_effect_surface_v1 *bg_effect;
static int configured;
/* buffer 外观：alpha < 255 才能看出"后面被糊了"，pattern 让模糊前后有区别 */
static uint32_t buffer_alpha = 255;
static int buffer_pattern;
/* 边缘留多少像素完全不画（模拟 CSD 客户端的阴影预留区：surface 比可视区大） */
static int buffer_pad;

static const struct ext_background_effect_manager_v1_listener bg_listener;

/* ==================== 事件 ==================== */

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface_, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface_, serial);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel_, int32_t width,
                               int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel_;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel_)
{
    (void)data;
    (void)toplevel_;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel_, int32_t w,
                                      int32_t h)
{
    (void)data;
    (void)toplevel_;
    (void)w;
    (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel_, struct wl_array *c)
{
    (void)data;
    (void)toplevel_;
    (void)c;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

/*
 * layer surface 的 configure：必须 ack 之后才能上 buffer。
 * 尺寸用我们 set_size 申请的值，合成器可能改（这里不关心，照着 ack 即可）。
 */
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    (void)width;
    (void)height;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    configured = 1;
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)data;
    (void)layer_surface;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
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
        xdg_deco_manager =
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, treeland_decoration_manager_v1_interface.name) == 0) {
        deco_manager = wl_registry_bind(registry, name, &treeland_decoration_manager_v1_interface,
                                        version < 1 ? version : 1);
    } else if (strcmp(interface, ext_background_effect_manager_v1_interface.name) == 0) {
        bg_effect_manager = wl_registry_bind(registry, name,
                                             &ext_background_effect_manager_v1_interface,
                                             version < 1 ? version : 1);
        ext_background_effect_manager_v1_add_listener(bg_effect_manager, &bg_listener, NULL);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                                       version < 4 ? version : 4);
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

static void bg_capabilities(void *data, struct ext_background_effect_manager_v1 *manager,
                            uint32_t flags)
{
    (void)data;
    (void)manager;
    bg_caps = flags;
}

static const struct ext_background_effect_manager_v1_listener bg_listener = {
    .capabilities = bg_capabilities,
};

/* ==================== 工具 ==================== */

/*
 * 等 ms 毫秒，期间持续分发事件。
 *
 * 必须用 prepare_read/read_events 这一对：dispatch_pending 只处理已经落在
 * 缓冲区里的事件，不会去读 socket（同 idle_probe 的取舍）。
 *
 * 返回 0 正常；-1 表示连接上出了协议错误（dup 用例靠这个判定）。
 */
static int pump(int ms)
{
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
                if (wl_display_read_events(display) != 0) {
                    return -1;
                }
            } else {
                wl_display_cancel_read(display);
            }
        }
        /* 注意：dispatch_pending 返回的是"分发了几个事件"，不是 0/1 */
        if (wl_display_dispatch_pending(display) < 0) {
            return -1;
        }
        if (wl_display_get_error(display) != 0) {
            return -1;
        }
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&tick, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed =
            (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms) {
            return 0;
        }
    }
}

static struct wl_buffer *create_buffer(void)
{
    int stride = WIDTH * 4;
    int size = stride * HEIGHT;
    int fd = memfd_create("deco-probe", MFD_CLOEXEC);
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
    /* 纯灰：内容本身不重要，重要的是有一块真 buffer 让窗口 map 上来；
     * pattern 画彩条（模糊前后差别明显），alpha 用来做半透明窗口 */
    uint32_t *pixels = data;
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            if (buffer_pad > 0 &&
                (x < buffer_pad || y < buffer_pad || x >= WIDTH - buffer_pad ||
                 y >= HEIGHT - buffer_pad)) {
                /* 整像素 0（预乘 ARGB 的全透明）：客户端声明了区域但没画这块 */
                pixels[y * WIDTH + x] = 0;
                continue;
            }
            uint32_t r = 0x80, g = 0x80, b = 0x80;
            if (buffer_pattern) {
                if (((x / 40) + (y / 40)) % 2 == 0) {
                    r = 0xe0;
                    g = 0x38;
                    b = 0x38;
                } else {
                    r = 0x30;
                    g = 0x60;
                    b = 0xe0;
                }
                if ((x / 80) % 3 == 1) {
                    g = 0xd0;
                }
            }
            uint32_t a = buffer_alpha;
            /* 预乘：wl_shm 的 ARGB8888 是 premultiplied */
            r = r * a / 255;
            g = g * a / 255;
            b = b * a / 255;
            pixels[y * WIDTH + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    munmap(data, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, WIDTH, HEIGHT, stride,
        buffer_alpha < 255 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void print_protocol_error(void)
{
    const struct wl_interface *iface = NULL;
    uint32_t id = 0;
    /*
     * wl_display_get_error() 给的是 libwayland 的 errno 风格错误（协议错误
     * 恒为 EPROTO=71），真正想断言的是协议里的 error code——它来自
     * wl_display_get_protocol_error() 的返回值。
     */
    uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
    (void)id;
    printf("PROTOCOL_ERROR %u %s\n", code, iface ? iface->name : "?");
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    int hold_ms = DEFAULT_HOLD_MS;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "cannot connect to compositor\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    /*
     * 第二轮是必须的，不是保险：bind 请求是在第一轮**分发**全局事件时才排进
     * 发送队列的，而第一轮等的那次 sync 早在 bind 之前就发出去了。所以第一轮
     * 返回时服务端还没看见 bind，更没来得及发 capabilities——这时读 bg_caps
     * 永远是 0，只有开了模糊的配置会露馅（关掉时期望值恰好也是 0）。
     */
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "missing wl_compositor / wl_shm / xdg_wm_base\n");
        return 1;
    }
    if (!deco_manager) {
        /* 合成器没接这个协议：交给调用方 skip，不算失败 */
        fprintf(stderr, "treeland_decoration_manager_v1 not advertised\n");
        return 77;
    }

    /*
     * --layer：换成 layer surface。面板/Dock 这类客户端没有顶层窗口对象，
     * 但一样能用 ext-background-effect——合成器得按 surface 分辨该把 blur
     * 节点挂在哪棵树上，这条路径只能靠真客户端走一遍。
     * 参数要在建 surface 之前就知道，所以先扫一遍。
     */
    int use_layer = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--layer") == 0) {
            use_layer = 1;
        }
    }

    surface = wl_compositor_create_surface(compositor);
    if (use_layer) {
        if (!layer_shell) {
            fprintf(stderr, "zwlr_layer_shell_v1 not advertised\n");
            return 77;
        }
        layer_surface_handle = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "deco-probe-layer");
        zwlr_layer_surface_v1_add_listener(layer_surface_handle, &layer_surface_listener, NULL);
        zwlr_layer_surface_v1_set_anchor(layer_surface_handle,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_size(layer_surface_handle, WIDTH, HEIGHT);
        /* 不要 exclusive zone、不要键盘：纯背景块 */
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_handle, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_handle, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    } else {
        xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
        xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
        toplevel = xdg_surface_get_toplevel(xdg_surface);
        xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
        xdg_toplevel_set_title(toplevel, "deco-probe");

        /*
         * 先谈 SSD：协议要求客户端先经 xdg-decoration 协商服务端装饰，这些
         * set_* 才有意义。woodland 默认就是 SSD，这里仍然走一遍——把"默认
         * 能work"和"协商后能work"混为一谈，换个合成器就露馅。
         */
        if (xdg_deco_manager) {
            xdg_deco =
                zxdg_decoration_manager_v1_get_toplevel_decoration(xdg_deco_manager, toplevel);
            zxdg_toplevel_decoration_v1_set_mode(
                xdg_deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    /* 上下文建在 map 之前：覆盖值要在 SSD 创建那一刻就读到 */
    int want_dup = 0;
    int want_inert_error = 0;
    int no_context = 0;
    int drop_context = 0;
    int drop_effect = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-context") == 0) {
            no_context = 1;
        } else if (strcmp(argv[i], "--drop-context") == 0) {
            drop_context = 1;
        } else if (strcmp(argv[i], "--drop-effect") == 0) {
            drop_effect = 1;
        } else if (strcmp(argv[i], "--caps") == 0) {
            want_caps = 1;
        } else if (strcmp(argv[i], "--layer") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--alpha") == 0 && i + 1 < argc) {
            buffer_alpha = (uint32_t)atoi(argv[++i]);
            if (buffer_alpha > 255) {
                buffer_alpha = 255;
            }
        } else if (strcmp(argv[i], "--pattern") == 0) {
            buffer_pattern = 1;
        } else if (strcmp(argv[i], "--pad") == 0 && i + 1 < argc) {
            buffer_pad = atoi(argv[++i]);
            if (buffer_pad < 0) {
                buffer_pad = 0;
            }
            if (buffer_pad > WIDTH / 4) {
                buffer_pad = WIDTH / 4;
            }
        }
    }
    if (use_layer) {
        no_context = 1; /* layer surface 没有装饰可谈 */
    }
    if (!no_context) {
        deco_ctx = treeland_decoration_manager_v1_get_decoration_context(deco_manager, surface);
        if (!deco_ctx) {
            fprintf(stderr, "get_decoration_context returned NULL\n");
            return 1;
        }
    }

    /*
     * 能力位只跟 manager 有关，跟窗口/装饰上下文无关，所以解析完参数就打，
     * 脚本据此决定"模糊到底该不该生效"的期望值。bit0 = blur。
     */
    if (want_caps) {
        printf("CAPS %u\n", bg_caps);
        fflush(stdout);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-context") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--drop-context") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--drop-effect") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--caps") == 0 || strcmp(argv[i], "--pattern") == 0 ||
                   strcmp(argv[i], "--layer") == 0) {
            /* 已在上面处理 */
        } else if (strcmp(argv[i], "--alpha") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--pad") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "blur") == 0 && i + 4 < argc) {
            /*
             * 背景模糊：不依赖装饰上下文（协议是独立的 surface 扩展），所以
             * 这一支排在 !deco_ctx 的检查之前。
             */
            int bx = atoi(argv[++i]);
            int by = atoi(argv[++i]);
            int bw = atoi(argv[++i]);
            int bh = atoi(argv[++i]);
            if (!bg_effect_manager) {
                fprintf(stderr, "ext_background_effect_manager_v1 not advertised\n");
                return 77;
            }
            bg_effect =
                ext_background_effect_manager_v1_get_background_effect(bg_effect_manager, surface);
            struct wl_region *region = wl_compositor_create_region(compositor);
            wl_region_add(region, bx, by, bw, bh);
            ext_background_effect_surface_v1_set_blur_region(bg_effect, region);
            wl_region_destroy(region);
        } else if (!deco_ctx) {
            fprintf(stderr, "operation %s needs a decoration context\n", argv[i]);
            return 1;
        } else if (strcmp(argv[i], "border") == 0 && i + 5 < argc) {
            uint32_t w = (uint32_t)atoi(argv[++i]);
            uint32_t r = (uint32_t)atoi(argv[++i]);
            uint32_t g = (uint32_t)atoi(argv[++i]);
            uint32_t b = (uint32_t)atoi(argv[++i]);
            uint32_t a = (uint32_t)atoi(argv[++i]);
            treeland_decoration_context_v1_set_border(deco_ctx, w, r, g, b, a);
        } else if (strcmp(argv[i], "titlebar") == 0 && i + 1 < argc) {
            treeland_decoration_context_v1_set_titlebar_mode(deco_ctx,
                                                             (uint32_t)atoi(argv[++i]));
        } else if (strcmp(argv[i], "radius") == 0 && i + 1 < argc) {
            treeland_decoration_context_v1_set_corner_radius(deco_ctx, (uint32_t)atoi(argv[++i]));
        } else if (strcmp(argv[i], "shadow") == 0 && i + 7 < argc) {
            uint32_t radius = (uint32_t)atoi(argv[++i]);
            int32_t dx = (int32_t)atoi(argv[++i]);
            int32_t dy = (int32_t)atoi(argv[++i]);
            uint32_t r = (uint32_t)atoi(argv[++i]);
            uint32_t g = (uint32_t)atoi(argv[++i]);
            uint32_t b = (uint32_t)atoi(argv[++i]);
            uint32_t a = (uint32_t)atoi(argv[++i]);
            treeland_decoration_context_v1_set_shadow(deco_ctx, radius, dx, dy, r, g, b, a);
        } else if (strcmp(argv[i], "dup") == 0) {
            want_dup = 1;
        } else if (strcmp(argv[i], "inert-error") == 0) {
            want_inert_error = 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    /* 第二个 context：合成器必须回 already_used 并断开我们 */
    if (want_dup) {
        treeland_decoration_manager_v1_get_decoration_context(deco_manager, surface);
        wl_display_flush(display);
        if (pump(1000) == 0) {
            fprintf(stderr, "FAIL: duplicate context was accepted\n");
            return 1;
        }
        print_protocol_error();
        return 3;
    }
    /* surface 销毁后设 blur region：合成器必须报 surface_destroyed 协议错误 */
    if (want_inert_error) {
        if (!bg_effect_manager) {
            fprintf(stderr, "ext_background_effect_manager_v1 not advertised\n");
            return 1;
        }
        struct wl_surface *bare = wl_compositor_create_surface(compositor);
        struct ext_background_effect_surface_v1 *bare_bg =
            ext_background_effect_manager_v1_get_background_effect(bg_effect_manager, bare);
        wl_surface_destroy(bare);
        ext_background_effect_surface_v1_set_blur_region(bare_bg, NULL);
        wl_display_flush(display);
        if (pump(1000) == 0) {
            fprintf(stderr, "FAIL: set_blur_region on destroyed surface was accepted\n");
            return 1;
        }
        print_protocol_error();
        return 3;
    }


    /* 等 configure → ack → 上 buffer：这一步之后窗口才算 map 了 */
    wl_surface_commit(surface);
    int waited = 0;
    while (!configured && waited < 3000) {
        if (pump(50) != 0) {
            print_protocol_error();
            return 3;
        }
        waited += 50;
    }
    if (!configured) {
        fprintf(stderr, "no surface configure in 3s (%s)\n",
                use_layer ? "zwlr_layer_surface_v1" : "xdg_surface");
        return 1;
    }
    struct wl_buffer *buffer = create_buffer();
    if (!buffer) {
        fprintf(stderr, "cannot create shm buffer\n");
        return 1;
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);

    /* 让合成器处理完 map，再让脚本开始查状态 */
    if (pump(200) != 0) {
        print_protocol_error();
        return 3;
    }
    printf("READY\n");
    fflush(stdout);

    /*
     * 销毁 context：协议允许合成器把属性恢复默认。脚本用这一刻断言
     * "边框回来了、标题栏仍然是隐藏的"（我们和 treeland 一样不恢复标题栏）。
     */
    if (drop_context && deco_ctx) {
        treeland_decoration_context_v1_destroy(deco_ctx);
        deco_ctx = NULL;
        pump(150);
        printf("REVERTED\n");
        fflush(stdout);
    }

    /*
     * 撤掉背景效果对象：协议说区域"在下一次提交时移除"。这里不提交新的
     * buffer（没必要），合成器在对象销毁时就该把 blur 摘掉。
     */
    if (drop_effect && bg_effect) {
        ext_background_effect_surface_v1_destroy(bg_effect);
        bg_effect = NULL;
        pump(150);
        printf("EFFECT_DROPPED\n");
        fflush(stdout);
    }

    int rc = pump(hold_ms);
    if (rc != 0) {
        print_protocol_error();
        return 3;
    }

    if (deco_ctx) {
        treeland_decoration_context_v1_destroy(deco_ctx);
    }
    if (bg_effect) {
        ext_background_effect_surface_v1_destroy(bg_effect);
    }
    wl_buffer_destroy(buffer);
    if (layer_surface_handle) {
        zwlr_layer_surface_v1_destroy(layer_surface_handle);
    }
    if (toplevel) {
        xdg_toplevel_destroy(toplevel);
    }
    if (xdg_surface) {
        xdg_surface_destroy(xdg_surface);
    }
    wl_surface_destroy(surface);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return 0;
}
