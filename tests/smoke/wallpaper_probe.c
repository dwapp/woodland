// SPDX-License-Identifier: GPL-2.0-only
/*
 * 壁纸协议探针：一个最小客户端，把 manager / notifier / shell 三条链路各走一遍。
 *
 * 断言全在客户端做（事件计数 + 错误码），脚本只负责起合成器、给它一个临时
 * 的 XDG_CONFIG_HOME / XDG_DATA_HOME，再看退出码和落盘文件。这样失败信息
 * 是"哪个事件不对"，而不是"grep 没找到某行"。
 *
 * 覆盖的场景：
 *   - bind notifier 后收到已有源（add 重放）
 *   - get_treeland_wallpaper 后收到 lockscreen + desktop 两条 changed
 *   - set_image_source 合法路径 → notifier 多一条 add
 *   - 非法路径 → failed(invalid_source)
 *   - 重复设置同一个源 → failed(already_used)
 *   - set_video_source → 类型按请求判定（video=1）
 *   - shell：赋角色 + attach buffer + commit + ready，不崩且能正常销毁
 *   - 可见性：当前工作区那张 play，其它源 pause
 *   - --workspace：切工作区后可见的那张跟着换（见 run_workspace_mode）
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-util.h>

#include "ext-workspace-v1-client-protocol.h"
#include "treeland-wallpaper-manager-unstable-v1-client-protocol.h"
#include "treeland-wallpaper-shell-unstable-v1-client-protocol.h"

#define ROLE_DESKTOP 0x1u
#define ROLE_LOCKSCREEN 0x2u

#define ERROR_ALREADY_USED 0u
#define ERROR_INVALID_SOURCE 1u

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_output *output;

static struct treeland_wallpaper_manager_v1 *manager;
static struct treeland_wallpaper_notifier_v1 *notifier;
static struct treeland_wallpaper_shell_v1 *shell;

static int adds;
static int removes;
static int changed_desktop;
static int changed_lockscreen;
static int failures;
static uint32_t last_error;
static char changed_desktop_path[512];

/*
 * 每个壁纸 surface 自己的播放状态计数。
 *
 * 合成器会给**看不见**的那些发 pause（切工作区时更是成对出现 pause/play），
 * 所以不能用一个全局计数器——分不清是哪一张收到的。
 */
struct surface_stats {
    int played;
    int paused;
};

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
}

static void expect(int condition, const char *what) {
    if (!condition) {
        fail(what);
    }
}

static void roundtrip(void) {
    if (wl_display_roundtrip(display) < 0) {
        fail("roundtrip failed");
    }
}

/* ==================== 监听器 ==================== */

static void notifier_handle_add(void *data, struct treeland_wallpaper_notifier_v1 *obj,
                                uint32_t source_type, const char *file_source) {
    (void)data;
    (void)obj;
    adds++;
    printf("add %u %s\n", source_type, file_source);
}

static void notifier_handle_remove(void *data, struct treeland_wallpaper_notifier_v1 *obj,
                                   const char *file_source) {
    (void)data;
    (void)obj;
    removes++;
    printf("remove %s\n", file_source);
}

static const struct treeland_wallpaper_notifier_v1_listener notifier_listener = {
    .add = notifier_handle_add,
    .remove = notifier_handle_remove,
};

static void wallpaper_handle_failed(void *data, struct treeland_wallpaper_v1 *obj,
                                    const char *file_source, uint32_t error) {
    (void)data;
    (void)obj;
    failures++;
    last_error = error;
    printf("failed %s %u\n", file_source, error);
}

static void wallpaper_handle_changed(void *data, struct treeland_wallpaper_v1 *obj,
                                     uint32_t role, uint32_t source_type,
                                     const char *file_source) {
    (void)data;
    (void)obj;
    if (role & ROLE_LOCKSCREEN) {
        changed_lockscreen++;
    }
    if (role & ROLE_DESKTOP) {
        changed_desktop++;
        snprintf(changed_desktop_path, sizeof(changed_desktop_path), "%s", file_source);
    }
    printf("changed %u %u %s\n", role, source_type, file_source);
}

static const struct treeland_wallpaper_v1_listener wallpaper_listener = {
    .failed = wallpaper_handle_failed,
    .changed = wallpaper_handle_changed,
};

static void surface_handle_position(void *data, struct treeland_wallpaper_surface_v1 *obj,
                                    wl_fixed_t position) {
    (void)data;
    (void)obj;
    (void)position;
}

static void surface_handle_pause(void *data, struct treeland_wallpaper_surface_v1 *obj) {
    (void)obj;
    struct surface_stats *stats = data;
    if (stats) {
        stats->paused++;
    }
}

static void surface_handle_set_playback_rate(void *data,
                                             struct treeland_wallpaper_surface_v1 *obj,
                                             wl_fixed_t rate) {
    (void)data;
    (void)obj;
    (void)rate;
}

static void surface_handle_play(void *data, struct treeland_wallpaper_surface_v1 *obj) {
    (void)obj;
    struct surface_stats *stats = data;
    if (stats) {
        stats->played++;
    }
}

static void surface_handle_slow_down(void *data, struct treeland_wallpaper_surface_v1 *obj,
                                     uint32_t duration) {
    (void)data;
    (void)obj;
    (void)duration;
}

static const struct treeland_wallpaper_surface_v1_listener surface_listener = {
    .position = surface_handle_position,
    .pause = surface_handle_pause,
    .set_playback_rate = surface_handle_set_playback_rate,
    .play = surface_handle_play,
    .slow_down = surface_handle_slow_down,
};

/* ==================== ext-workspace ==================== */

/*
 * 只为了做一件事：请合成器切到指定工作区，好验证"壁纸跟着工作区走"。
 *
 * 走协议而不是发键盘事件：headless 下没有键盘设备，ext-workspace 的
 * activate 是唯一能程序化触发切换的路子（v1 的 activate 不带 serial）。
 */
#define WS_MAX 16

struct probe_ws {
    struct ext_workspace_handle_v1 *handle;
    char name[32];
};

static struct ext_workspace_manager_v1 *ws_manager;
static struct probe_ws ws_list[WS_MAX];
static int ws_count;
static bool ws_done;

static void ws_handle_id(void *data, struct ext_workspace_handle_v1 *handle, const char *id) {
    (void)data;
    (void)handle;
    (void)id;
}

static void ws_handle_name(void *data, struct ext_workspace_handle_v1 *handle, const char *name) {
    (void)handle;
    struct probe_ws *ws = data;
    snprintf(ws->name, sizeof(ws->name), "%s", name ? name : "");
}

static void ws_handle_coordinates(void *data, struct ext_workspace_handle_v1 *handle,
                                 struct wl_array *coordinates) {
    (void)data;
    (void)handle;
    (void)coordinates;
}

static void ws_handle_state(void *data, struct ext_workspace_handle_v1 *handle, uint32_t state) {
    (void)data;
    (void)handle;
    (void)state;
}

static void ws_handle_capabilities(void *data, struct ext_workspace_handle_v1 *handle,
                                  uint32_t capabilities) {
    (void)data;
    (void)handle;
    (void)capabilities;
}

static void ws_handle_removed(void *data, struct ext_workspace_handle_v1 *handle) {
    (void)data;
    (void)handle;
}

/* 生成代码不给 NULL 回调做保护（这个 wayland-scanner 版本），全填上 */
static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
    .id = ws_handle_id,
    .name = ws_handle_name,
    .coordinates = ws_handle_coordinates,
    .state = ws_handle_state,
    .capabilities = ws_handle_capabilities,
    .removed = ws_handle_removed,
};

static void ws_manager_handle_workspace(void *data, struct ext_workspace_manager_v1 *manager,
                                        struct ext_workspace_handle_v1 *handle) {
    (void)data;
    (void)manager;
    if (ws_count >= WS_MAX) {
        return;
    }
    struct probe_ws *ws = &ws_list[ws_count++];
    ws->handle = handle;
    ext_workspace_handle_v1_add_listener(handle, &ws_handle_listener, ws);
}

static void ws_manager_handle_group(void *data, struct ext_workspace_manager_v1 *manager,
                                    struct ext_workspace_group_handle_v1 *group) {
    (void)data;
    (void)manager;
    /*
     * 组用不上（工作区名字从 manager 的 workspace 事件来），但**必须**收掉：
     * 它是事件里 new_id 出来的代理，客户端自己负责销毁。留着不只是 LeakSanitizer
     * 会报 96 字节（libwayland 在 read_events 里给新代理分配的闭包），
     * 还会让"泄漏"看起来像合成器的问题。
     */
    ext_workspace_group_handle_v1_destroy(group);
}

static void ws_manager_handle_done(void *data, struct ext_workspace_manager_v1 *manager) {
    (void)data;
    (void)manager;
    ws_done = true;
}

static void ws_manager_handle_finished(void *data, struct ext_workspace_manager_v1 *manager) {
    (void)data;
    (void)manager;
}

static const struct ext_workspace_manager_v1_listener ws_manager_listener = {
    .workspace_group = ws_manager_handle_group,
    .workspace = ws_manager_handle_workspace,
    .done = ws_manager_handle_done,
    .finished = ws_manager_handle_finished,
};

/* ==================== registry ==================== */

static void registry_handle_global(void *data, struct wl_registry *reg, uint32_t name,
                                   const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                      version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!output) {
            output = wl_registry_bind(reg, name, &wl_output_interface, version);
        }
    } else if (strcmp(interface, treeland_wallpaper_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(reg, name, &treeland_wallpaper_manager_v1_interface,
                                   version);
    } else if (strcmp(interface, treeland_wallpaper_notifier_v1_interface.name) == 0) {
        notifier = wl_registry_bind(reg, name, &treeland_wallpaper_notifier_v1_interface,
                                    version);
        treeland_wallpaper_notifier_v1_add_listener(notifier, &notifier_listener, NULL);
    } else if (strcmp(interface, treeland_wallpaper_shell_v1_interface.name) == 0) {
        shell = wl_registry_bind(reg, name, &treeland_wallpaper_shell_v1_interface, version);
    } else if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
        ws_manager = wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface, 1);
        ext_workspace_manager_v1_add_listener(ws_manager, &ws_manager_listener, NULL);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *reg,
                                          uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ==================== shm buffer ==================== */

static struct wl_buffer *create_buffer(int width, int height) {
    int stride = width * 4;
    int size = stride * height;

    char path[] = "/tmp/woodland-wallpaper-shm-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fail("mkstemp");
    }
    unlink(path);
    if (ftruncate(fd, size) < 0) {
        fail("ftruncate");
    }
    void *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        fail("mmap");
    }
    memset(pixels, 0x20, (size_t)size);
    munmap(pixels, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    if (!pool) {
        fail("wl_shm_create_pool");
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                         WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buffer) {
        fail("wl_shm_pool_create_buffer");
    }
    return buffer;
}

/* ==================== 数据目录 ==================== */

/*
 * 源校验有白名单（/usr/share/wallpapers、/var/cache/wallpapers、
 * $XDG_DATA_HOME/wallpapers）。探针自己在 XDG_DATA_HOME 里造一个文件，
 * 这样既不写系统目录，又确实走了"合法源"那条路径。
 */
static void make_source(const char *name, char *out, size_t out_size) {
    const char *data_home = getenv("XDG_DATA_HOME");
    if (!data_home || data_home[0] != '/') {
        fail("XDG_DATA_HOME must be set by the smoke script");
    }
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/wallpapers", data_home);
    mkdir(data_home, 0755); // 已存在返回 -1，不关心
    mkdir(dir, 0755);

    snprintf(out, out_size, "%s/%s", dir, name);
    FILE *file = fopen(out, "w");
    if (!file) {
        fail("cannot create source file");
    }
    fputs("not really a wallpaper: the compositor never decodes it\n", file);
    fclose(file);
}

/* ==================== 主流程 ==================== */

static void cleanup(void);

static void connect_compositor(void) {
    display = wl_display_connect(NULL);
    if (!display) {
        fail("cannot connect to the compositor");
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    roundtrip();
    roundtrip(); // 第二轮拿齐 output 等需要额外往返的全局

    expect(manager && notifier && shell, "wallpaper globals are missing");
    expect(compositor && shm && output, "compositor/shm/output globals are missing");
}

/*
 * 工作区联动模式。
 *
 * 前提：脚本先把配置改成"工作区 1 和工作区 2 各一张不同的图"，再启动一份
 * 干净的合成器（默认停在工作区 1）。探针造两个壁纸 surface，各对应一张图，
 * 然后请合成器切到工作区 2，看可见性有没有跟着换边。
 */
static int run_workspace_mode(const char *first_source, const char *second_source) {
    connect_compositor();
    expect(ws_manager != NULL, "ext-workspace manager is missing");

    for (int i = 0; i < 10 && !ws_done; i++) {
        roundtrip();
    }
    expect(ws_done, "ext-workspace manager never sent done");
    expect(ws_count >= 2, "expected at least two workspaces");

    struct treeland_wallpaper_v1 *wallpaper =
        treeland_wallpaper_manager_v1_get_treeland_wallpaper(manager, output, NULL);
    expect(wallpaper != NULL, "get_treeland_wallpaper returned NULL");
    treeland_wallpaper_v1_add_listener(wallpaper, &wallpaper_listener, NULL);
    roundtrip();

    struct wl_buffer *buffer = create_buffer(8, 8);
    struct surface_stats first = {0};
    struct surface_stats second = {0};

    struct wl_surface *first_wl_surface = wl_compositor_create_surface(compositor);
    struct treeland_wallpaper_surface_v1 *first_surface =
        treeland_wallpaper_shell_v1_get_treeland_wallpaper_surface(shell, first_wl_surface,
                                                                  first_source);
    expect(first_surface != NULL, "cannot create the first wallpaper surface");
    treeland_wallpaper_surface_v1_add_listener(first_surface, &surface_listener, &first);
    wl_surface_attach(first_wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(first_wl_surface, 0, 0, 8, 8);
    wl_surface_commit(first_wl_surface);
    treeland_wallpaper_surface_v1_ready(first_surface);

    struct wl_surface *second_wl_surface = wl_compositor_create_surface(compositor);
    struct treeland_wallpaper_surface_v1 *second_surface =
        treeland_wallpaper_shell_v1_get_treeland_wallpaper_surface(shell, second_wl_surface,
                                                                  second_source);
    expect(second_surface != NULL, "cannot create the second wallpaper surface");
    treeland_wallpaper_surface_v1_add_listener(second_surface, &surface_listener, &second);
    wl_surface_attach(second_wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(second_wl_surface, 0, 0, 8, 8);
    wl_surface_commit(second_wl_surface);
    treeland_wallpaper_surface_v1_ready(second_surface);

    roundtrip();
    roundtrip();
    expect(first.played == 1,
           "wallpaper of the current workspace should be playing");
    expect(first.paused == 0, "current workspace wallpaper should not be paused");
    expect(second.played == 0,
           "wallpaper of another workspace should not start playing");
    expect(second.paused == 1, "wallpaper of another workspace should be paused");

    /* 切到工作区 2：可见的那张必须换边 */
    struct probe_ws *target = NULL;
    for (int i = 0; i < ws_count; i++) {
        if (strcmp(ws_list[i].name, "2") == 0) {
            target = &ws_list[i];
            break;
        }
    }
    expect(target != NULL, "workspace 2 not found");
    ext_workspace_handle_v1_activate(target->handle);
    ext_workspace_manager_v1_commit(ws_manager);
    roundtrip();
    roundtrip();
    expect(first.paused == 1, "leaving the workspace should pause its wallpaper");
    expect(second.played == 1, "entering the workspace should play its wallpaper");

    treeland_wallpaper_surface_v1_destroy(first_surface);
    wl_surface_destroy(first_wl_surface);
    treeland_wallpaper_surface_v1_destroy(second_surface);
    wl_surface_destroy(second_wl_surface);
    wl_buffer_destroy(buffer);
    treeland_wallpaper_v1_destroy(wallpaper);
    treeland_wallpaper_manager_v1_destroy(manager);
    treeland_wallpaper_notifier_v1_destroy(notifier);
    treeland_wallpaper_shell_v1_destroy(shell);
    roundtrip();

    cleanup();
    printf("wallpaper probe: workspace switch OK (played=%d/%d paused=%d/%d)\n",
           first.played, second.played, first.paused, second.paused);
    return 0;
}

/* ==================== 清理 ==================== */

/*
 * ASan 构建里探针自己也被插桩：代理不逐个销毁就是 96 字节泄漏
 * （libwayland-client 的 display 内部结构），会让冒烟在 LeakSanitizer 下
 * 假失败。见 tests/smoke/idle_probe.c 的 cleanup 注释。
 */
static void cleanup(void) {
    for (int i = 0; i < ws_count; i++) {
        if (ws_list[i].handle) {
            ext_workspace_handle_v1_destroy(ws_list[i].handle);
        }
    }
    if (ws_manager) {
        ext_workspace_manager_v1_destroy(ws_manager);
    }
    if (compositor) {
        wl_compositor_destroy(compositor);
    }
    if (shm) {
        wl_shm_destroy(shm);
    }
    if (output) {
        wl_output_destroy(output);
    }
    if (registry) {
        wl_registry_destroy(registry);
    }
    if (display) {
        wl_display_disconnect(display);
    }
}

/*
 * 三种跑法：
 *   wallpaper_probe
 *       完整链路（默认壁纸 → 设图 → 各种失败 → 视频 → shell → 可见性）。
 *   wallpaper_probe --expect-desktop <path>
 *       只验证"重启后从配置文件读回来的桌面壁纸就是 <path>"。第二次启动时
 *       配置里已经有上次设的图，再跑完整链路会撞 already_used，所以拆开。
 *   wallpaper_probe --workspace <当前工作区的图> <另一个工作区的图>
 *       验证"壁纸跟着工作区走"（见 run_workspace_mode）。
 */
int main(int argc, char **argv) {
    const char *expect_desktop =
        (argc >= 3 && strcmp(argv[1], "--expect-desktop") == 0) ? argv[2] : NULL;
    bool workspace_mode = (argc >= 4 && strcmp(argv[1], "--workspace") == 0);

    /* 行缓冲：断言失败/ASan 中止时，已经收到的事件不该留在缓冲区里丢掉 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (workspace_mode) {
        return run_workspace_mode(argv[2], argv[3]);
    }

    connect_compositor();

    /* bind notifier 时会重放已有源（默认壁纸或上次设的图），至少一条 */
    expect(adds >= 1, "notifier did not replay existing sources");

    struct treeland_wallpaper_v1 *wallpaper =
        treeland_wallpaper_manager_v1_get_treeland_wallpaper(manager, output, NULL);
    expect(wallpaper != NULL, "get_treeland_wallpaper returned NULL");
    treeland_wallpaper_v1_add_listener(wallpaper, &wallpaper_listener, NULL);
    roundtrip();
    expect(changed_lockscreen == 1,
           "expected one lockscreen changed on wallpaper creation");
    expect(changed_desktop == 1, "expected one desktop changed on wallpaper creation");

    if (expect_desktop) {
        expect(strcmp(changed_desktop_path, expect_desktop) == 0,
               "desktop wallpaper restored from config does not match");
        treeland_wallpaper_v1_destroy(wallpaper);
        treeland_wallpaper_manager_v1_destroy(manager);
        treeland_wallpaper_notifier_v1_destroy(notifier);
        treeland_wallpaper_shell_v1_destroy(shell);
        roundtrip();
        cleanup();
        printf("wallpaper probe: restored %s\n", expect_desktop);
        return 0;
    }

    char image[512];
    char video[512];
    char missing[512];
    make_source("smoke-image.jpg", image, sizeof(image));
    make_source("smoke-video.mp4", video, sizeof(video));
    snprintf(missing, sizeof(missing), "%s/wallpapers/does-not-exist.jpg",
             getenv("XDG_DATA_HOME"));

    /* 1. 合法图片：新增一条 notifier add，不报错 */
    int adds_before = adds;
    int failures_before = failures;
    treeland_wallpaper_v1_set_image_source(wallpaper, image, ROLE_DESKTOP);
    roundtrip();
    expect(failures == failures_before, "valid image source was rejected");
    expect(adds == adds_before + 1, "valid image source was not announced to notifier");

    /* 2. 不存在的文件 → invalid_source */
    failures_before = failures;
    treeland_wallpaper_v1_set_image_source(wallpaper, missing, ROLE_DESKTOP);
    roundtrip();
    expect(failures == failures_before + 1, "missing file was not reported as failed");
    expect(last_error == ERROR_INVALID_SOURCE, "missing file error code is not invalid_source");

    /* 3. 白名单外的路径（存在但不在允许目录）→ invalid_source */
    failures_before = failures;
    treeland_wallpaper_v1_set_image_source(wallpaper, "/etc/passwd", ROLE_DESKTOP);
    roundtrip();
    expect(failures == failures_before + 1, "path outside the allowlist was accepted");
    expect(last_error == ERROR_INVALID_SOURCE, "allowlist error code is not invalid_source");

    /* 4. 同一张图再设一次 → already_used（非致命） */
    failures_before = failures;
    treeland_wallpaper_v1_set_image_source(wallpaper, image, ROLE_DESKTOP);
    roundtrip();
    expect(failures == failures_before + 1, "duplicate source was not reported");
    expect(last_error == ERROR_ALREADY_USED, "duplicate source error code is not already_used");

    /* 5. 视频源：类型由请求决定（1 = video） */
    adds_before = adds;
    failures_before = failures;
    treeland_wallpaper_v1_set_video_source(wallpaper, video, ROLE_LOCKSCREEN);
    roundtrip();
    expect(failures == failures_before, "valid video source was rejected");
    expect(adds == adds_before + 1, "valid video source was not announced to notifier");

    /* 6. shell：赋角色 + 首帧 + ready */
    struct wl_surface *wl_surface = wl_compositor_create_surface(compositor);
    expect(wl_surface != NULL, "cannot create wl_surface");
    struct treeland_wallpaper_surface_v1 *wallpaper_surface =
        treeland_wallpaper_shell_v1_get_treeland_wallpaper_surface(shell, wl_surface, image);
    expect(wallpaper_surface != NULL, "cannot create wallpaper surface");
    struct surface_stats desktop_stats = {0};
    treeland_wallpaper_surface_v1_add_listener(wallpaper_surface, &surface_listener,
                                              &desktop_stats);

    struct wl_buffer *buffer = create_buffer(8, 8);
    wl_surface_attach(wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(wl_surface, 0, 0, 8, 8);
    wl_surface_commit(wl_surface);
    treeland_wallpaper_surface_v1_ready(wallpaper_surface);
    roundtrip();
    /*
     * 未锁屏、且这张源正是当前工作区的桌面壁纸：ready 之后应当收到 play。
     * 反过来"别的源"要收 pause，那是 --workspace 模式覆盖的事——这里只
     * 确认可见性没把该显示的那张也误关掉。
     */
    expect(desktop_stats.played == 1, "expected play after ready while unlocked");
    expect(desktop_stats.paused == 0, "the visible wallpaper should not be paused");

    /* 清理顺序按协议：先角色对象再 wl_surface */
    treeland_wallpaper_surface_v1_destroy(wallpaper_surface);
    wl_surface_destroy(wl_surface);
    wl_buffer_destroy(buffer);
    treeland_wallpaper_v1_destroy(wallpaper);
    treeland_wallpaper_manager_v1_destroy(manager);
    treeland_wallpaper_notifier_v1_destroy(notifier);
    treeland_wallpaper_shell_v1_destroy(shell);
    roundtrip();

    cleanup();
    printf("wallpaper probe: OK (adds=%d removes=%d changed=%d/%d failures=%d)\n", adds,
           removes, changed_lockscreen, changed_desktop, failures);
    return 0;
}
