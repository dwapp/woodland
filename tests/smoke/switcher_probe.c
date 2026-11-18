// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alt+Tab 切换器的端到端探针：三个真窗口 + 一块虚拟键盘。
 *
 * 为什么要走到这一层：单测（tests/unit/test_window_switcher.c）只证明"选谁"，
 * 而这条链路上真正容易断的是**按键怎么到切换器**——Alt 的修饰位是不是被认到、
 * Tab 的 press 有没有被全局键位截走、松开 Alt 有没有被当成普通 release 下发给
 * 客户端。这些只有发真按键才能验。
 *
 * 做法：
 *   1. 一个客户端开三个 xdg_toplevel（最后 map 的拿焦点，z 序 [c, b, a]）；
 *   2. 用 zwp_virtual_keyboard_v1 发 Alt↓ Tab↓ Tab↑ → 断言面板打开、高亮第二个；
 *   3. 松 Alt → 断言焦点真的换到了高亮那个窗口；
 *   4. 再来一轮 → 断言焦点按**改写过的 z 序**走（3 个窗口才能证明方向，2 个是对称的）。
 *   5. 面板开着时最小化一个候选 → 断言那一行还在（只是标了"已最小化"），
 *      提交后窗口被恢复并拿到焦点；
 *   6. 先最小化、后开面板 → 断言它照样出现在列表里、能被 Tab 走到、提交后恢复。
 *
 * 状态一律从调试接口读（WOODLAND_IPC / get_focus / get_tree），断言写在本进程里：
 * 探针自己退出码就是结论，shell 那层只负责起停合成器和看现场。
 *
 * 虚拟键盘必须自己发 modifiers 事件：wlr 的 virtual-keyboard 后端把 key 事件
 * 的 update_state 置成 false，xkb 状态只由 modifiers 请求驱动。少发这一条，
 * 合成器那边修饰位恒为 0，Alt+Tab 根本进不到键位表。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "switcher-xdg-shell-client-protocol.h"
#include "switcher-virtual-keyboard-unstable-v1-client-protocol.h"

#define WIN_WIDTH 220
#define WIN_HEIGHT 160

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_keyboard *wl_kb;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct zwp_virtual_keyboard_v1 *vk;

/*
 * 客户端视角下的按键配对账本（盯 seat 层的吞键位图）：
 *   - client_held[kc]：当前焦点表面按 enter / leave / key 维护的"按下未松开"位图；
 *   - orphan_releases：收到 release 时 client_held[kc] 为 false 的次数（孤儿 release）；
 *   - swallowed_leaks：Tab / Escape 这两颗被切换器消费的键漏进客户端的次数。
 */
static bool client_held[256];
static int orphan_releases;
static int swallowed_leaks;

static int failures;
/* --hold-ms N：面板打开后多停 N 毫秒再松 Alt（截图/人工观察用，默认 0）。
 * 两个停点：第一轮刚打开（看列表本身）、第三轮最小化之后（看"已最小化"那行）。 */
static int hold_ms;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            fprintf(stderr, "ok   " __VA_ARGS__);                                                   \
            fputc('\n', stderr);                                                                    \
        } else {                                                                                   \
            fprintf(stderr, "FAIL " __VA_ARGS__);                                                   \
            fputc('\n', stderr);                                                                    \
            failures++;                                                                             \
        }                                                                                          \
    } while (0)

/* ==================== 窗口 ==================== */

struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    bool configured;
};

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
    struct window *win = data;
    xdg_surface_ack_configure(surf, serial);
    win->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)w;
    (void)h;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd,
                      uint32_t size)
{
    (void)data;
    (void)kb;
    (void)format;
    (void)size;
    if (fd >= 0) {
        close(fd);
    }
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)kb;
    (void)serial;
    (void)surface;
    memset(client_held, 0, sizeof(client_held));
    uint32_t *k;
    wl_array_for_each(k, keys) {
        if (*k < 256) {
            client_held[*k] = true;
        }
        if (*k == KEY_TAB || *k == KEY_ESC) {
            swallowed_leaks++;
        }
    }
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface)
{
    (void)data;
    (void)kb;
    (void)serial;
    (void)surface;
    memset(client_held, 0, sizeof(client_held));
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state)
{
    (void)data;
    (void)kb;
    (void)serial;
    (void)time;
    if (key == KEY_TAB || key == KEY_ESC) {
        swallowed_leaks++;
    }
    if (key >= 256) {
        return;
    }
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        client_held[key] = true;
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (!client_held[key]) {
            orphan_releases++;
        }
        client_held[key] = false;
    }
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                         uint32_t group)
{
    (void)data;
    (void)kb;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data;
    (void)kb;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

static int client_held_count(void)
{
    int n = 0;
    for (size_t i = 0; i < sizeof(client_held) / sizeof(client_held[0]); i++) {
        if (client_held[i]) {
            n++;
        }
    }
    return n;
}

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl_kb) {
        wl_kb = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(wl_kb, &kb_listener, NULL);
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
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        vk_mgr = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
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
    int stride = WIN_WIDTH * 4;
    int size = stride * WIN_HEIGHT;
    int fd = memfd_create("switcher-probe", MFD_CLOEXEC);
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
    for (int i = 0; i < WIN_WIDTH * WIN_HEIGHT; i++) {
        data[i] = 0xFF445566;
    }
    munmap(data, (size_t)size);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, WIN_WIDTH, WIN_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

/* 建一个窗口并把它的首个 buffer 提交上去（map 才会发生） */
static bool create_window(struct window *win, const char *title)
{
    win->surface = wl_compositor_create_surface(compositor);
    if (!win->surface) {
        return false;
    }
    win->xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
    win->toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(win->toplevel, title);
    xdg_toplevel_set_app_id(win->toplevel, "org.woodland.switcher-probe");
    wl_surface_commit(win->surface);

    while (!win->configured) {
        if (wl_display_dispatch(display) < 0) {
            return false;
        }
    }

    win->buffer = create_buffer();
    if (!win->buffer) {
        return false;
    }
    wl_surface_attach(win->surface, win->buffer, 0, 0);
    wl_surface_damage_buffer(win->surface, 0, 0, WIN_WIDTH, WIN_HEIGHT);
    wl_surface_commit(win->surface);
    return true;
}

/* ==================== 虚拟键盘 ==================== */

static void vk_send_keymap(struct xkb_keymap *keymap)
{
    char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) {
        return;
    }
    size_t len = strlen(str) + 1;
    int fd = memfd_create("switcher-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        free(str);
        return;
    }
    if (ftruncate(fd, (off_t)len) == 0) {
        void *data = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data != MAP_FAILED) {
            memcpy(data, str, len);
            munmap(data, len);
            /* fd 的所有权交给合成器？不——协议是"传一份可读的 fd"，
             * 发完之后本地这份由我们自己关 */
            zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
        }
    }
    close(fd);
    free(str);
}

/* 推一次事件并等合成器处理完：roundtrip 之后按键一定已经过 seat 的键盘钩子 */
static void pump(void)
{
    wl_display_flush(display);
    wl_display_roundtrip(display);
}

static void send_mods(uint32_t depressed)
{
    zwp_virtual_keyboard_v1_modifiers(vk, depressed, 0, 0, 0);
    pump();
}

static void send_key(uint32_t evdev, bool pressed)
{
    zwp_virtual_keyboard_v1_key(vk, 0, evdev,
                                pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                        : WL_KEYBOARD_KEY_STATE_RELEASED);
    pump();
}

/* ==================== 调试接口 ==================== */

static char ipc_buf[1 << 16];

/* 查一条只读命令，结果放进 ipc_buf（一行 JSON，含换行） */
static const char *ipc_query(const char *cmd)
{
    const char *path = getenv("WOODLAND_IPC");
    if (!path || !*path) {
        return NULL;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }
    char line[64];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, (size_t)n) != n) {
        close(fd);
        return NULL;
    }
    size_t used = 0;
    ipc_buf[0] = '\0';
    while (used + 1 < sizeof(ipc_buf)) {
        ssize_t r = read(fd, ipc_buf + used, sizeof(ipc_buf) - 1 - used);
        if (r <= 0) {
            break;
        }
        used += (size_t)r;
        ipc_buf[used] = '\0';
        if (memchr(ipc_buf, '\n', used)) {
            break;
        }
    }
    close(fd);
    return used > 0 ? ipc_buf : NULL;
}

/* "key":123 → 123；找不到返回 -999 */
static int json_int_in(const char *json, const char *key)
{
    if (!json) {
        return -999;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return -999;
    }
    return atoi(p + strlen(pat));
}

static int json_bool_in(const char *json, const char *key)
{
    if (!json) {
        return -1;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    return strncmp(p, "true", 4) == 0 ? 1 : 0;
}

/* 把 "window_switcher":{...} 这个子对象切出来，避免撞上 window_menu 的同名键 */
static const char *switcher_object(const char *tree)
{
    static char slice[512];
    if (!tree) {
        return NULL;
    }
    const char *p = strstr(tree, "\"window_switcher\":");
    if (!p) {
        return NULL;
    }
    const char *open = strchr(p, '{');
    const char *close = open ? strchr(open, '}') : NULL;
    if (!open || !close || (size_t)(close - open) >= sizeof(slice)) {
        return NULL;
    }
    size_t len = (size_t)(close - open) + 1;
    memcpy(slice, open, len);
    slice[len] = '\0';
    return slice;
}

/* 面板当前高亮那个窗口的 id（0 = 没有高亮） */
static int switcher_selected(void)
{
    return json_int_in(switcher_object(ipc_query("get_tree")), "selected");
}

static int focused_view(void)
{
    const char *json = ipc_query("get_focus");
    return json_int_in(json, "view");
}

/*
 * 在 [start, end) 这段对象 JSON 里找顶层键 `"key":` 的值起点。
 *
 * 只在深度 == 1（该对象自身的顶层字段）时匹配，跳过所有嵌套的 `{...}` /
 * `[...]`（如 ssd、deco、foreign_toplevel、outer）及字符串字面量，不依赖
 * append_view_details() 的字段输出顺序。
 */
static const char *json_top_field(const char *start, const char *end, const char *key)
{
    char pat[64];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pat_len <= 0 || (size_t)pat_len >= sizeof(pat)) {
        return NULL;
    }
    int depth = 0;
    bool in_str = false;
    for (const char *p = start; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < end) {
                p++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            if (depth == 1 && (size_t)(end - p) >= (size_t)pat_len &&
                memcmp(p, pat, (size_t)pat_len) == 0) {
                return p + pat_len;
            }
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return NULL;
}

/*
 * 读某个视图的顶层布尔字段（这里只用于 minimized）。
 *
 * 按括号深度逐个切出 `"views":[...]` 里的视图对象，在对象自身的顶层字段里
 * 查 `"id"` 与目标 key，不依赖字段先后顺序，也不会误匹配嵌套对象里的同名键。
 */
static int view_bool_field(int id, const char *key)
{
    const char *tree = ipc_query("get_tree");
    if (!tree) {
        return -1;
    }
    const char *views = strstr(tree, "\"views\":[");
    if (!views) {
        return -1;
    }
    const char *p = strchr(views, '[');
    if (!p) {
        return -1;
    }
    p++;

    int depth = 0;
    bool in_str = false;
    const char *obj_start = NULL;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && *(p + 1)) {
                p++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) {
                obj_start = p;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                const char *obj_end = p + 1;
                const char *id_val = json_top_field(obj_start, obj_end, "id");
                if (id_val && atoi(id_val) == id) {
                    const char *val = json_top_field(obj_start, obj_end, key);
                    if (!val) {
                        return -1;
                    }
                    return strncmp(val, "true", 4) == 0 ? 1 : 0;
                }
                obj_start = NULL;
            }
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (depth == 0) {
                break;
            }
            depth--;
        }
    }
    return -1;
}

/* 该视图此刻是否最小化；读不到返回 -1（和服务端的 false 区分开） */
static int view_minimized(int id)
{
    return view_bool_field(id, "minimized");
}

/* ==================== 主流程 ==================== */

/*
 * 自检里的 switcher_snapshot 检查专门盯"快照里的每一项都还在 server->views
 * 里、且仍然 restorable"。面板开着的那一刻是最有价值的采样点：快照非空、
 * 高亮有效、每一项都要对得上。漏挂 view_unmapped 监听时这里会红，而面板
 * 外观上看不出来（最多少一行）。
 */
static void expect_self_check_ok(const char *when)
{
    const char *json = ipc_query("self_check");
    if (!json) {
        CHECK(false, "self_check answered (%s)", when);
        return;
    }
    CHECK(json_bool_in(json, "ok") == 1, "self_check ok %s", when);
    /*
     * 还要确认这条检查**真的在跑**：通过的检查项只写 name、不写
     * expected/actual（自检的约定是"绿了就别吵"），所以这里只能查名字。
     * 两个分支（面板开着 / 已收起）的语义由 tests/unit/test_selfcheck.c
     * 钉住——它故意漏发 view_unmapped，断言 active 分支会红。
     */
    CHECK(strstr(json, "\"name\":\"switcher_snapshot\"") != NULL,
          "self_check covers the switcher snapshot (%s)", when);
}

static void expect_switcher(bool active, int count, int index)
{
    const char *obj = switcher_object(ipc_query("get_tree"));
    if (!obj) {
        CHECK(false, "get_tree has no window_switcher object");
        return;
    }
    CHECK(json_bool_in(obj, "active") == (active ? 1 : 0), "switcher.active == %s", 
          active ? "true" : "false");
    CHECK(json_int_in(obj, "count") == count, "switcher.count == %d (got %d)", count,
          json_int_in(obj, "count"));
    CHECK(json_int_in(obj, "index") == index, "switcher.index == %d (got %d)", index,
          json_int_in(obj, "index"));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        }
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "FAIL cannot connect to display\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base || !seat || !vk_mgr) {
        fprintf(stderr, "FAIL missing globals (compositor/shm/xdg/seat/virtual-keyboard)\n");
        return 1;
    }

    /*
     * 三个窗口而不是两个：两个窗口时"正着切"和"反着切"是同一件事，方向
     * 弄反了测试照样绿。三个才能钉住"z 序表头最上、往表尾走"这个语义。
     */
    struct window win_a = {0};
    struct window win_b = {0};
    struct window win_c = {0};
    struct window *created[] = {&win_a, &win_b, &win_c};
    const char *titles[] = {"switcher-a", "switcher-b", "switcher-c"};
    int ids[3] = {0, 0, 0};
    for (size_t i = 0; i < sizeof(created) / sizeof(created[0]); i++) {
        if (!create_window(created[i], titles[i])) {
            fprintf(stderr, "FAIL cannot create windows\n");
            return 1;
        }
        /* 每 map 一个就记下它的 id：最新 map 的拿焦点。后面全按 id 断言顺序 */
        pump();
        pump();
        ids[i] = focused_view();
    }
    for (int i = 0; i < 3; i++) {
        pump();
    }
    usleep(150 * 1000);
    pump();

    int id_a = ids[0], id_b = ids[1], id_c = ids[2];
    CHECK(id_a > 0 && id_b > 0 && id_c > 0 && id_a != id_b && id_b != id_c && id_a != id_c,
          "three windows mapped with distinct ids (a=%d, b=%d, c=%d)", id_a, id_b, id_c);
    if (id_a <= 0 || id_b <= 0 || id_c <= 0) {
        return 1;
    }
    CHECK(focused_view() == id_c, "the last mapped window has focus (id=%d)", id_c);
    expect_switcher(false, 0, -1);

    /* ---------- 虚拟键盘 + 键位 ---------- */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS)
                                    : NULL;
    if (!keymap) {
        fprintf(stderr, "FAIL cannot build an xkb keymap\n");
        return 1;
    }
    vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);
    if (!vk) {
        fprintf(stderr, "FAIL cannot create virtual keyboard\n");
        return 1;
    }
    vk_send_keymap(keymap);
    pump();

    xkb_mod_index_t alt_idx = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
    if (alt_idx == XKB_MOD_INVALID) {
        fprintf(stderr, "FAIL keymap has no Alt modifier\n");
        return 1;
    }
    uint32_t alt_mask = 1u << alt_idx;

    /* ---------- 第一轮：z 序 [c, b, a]，Alt+Tab 落 b，再一格到 a ---------- */
    send_mods(alt_mask);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);

    expect_switcher(true, 3, 1);
    int selected = switcher_selected();
    CHECK(selected == id_b,
          "first Tab lands on the window below the focused one (selected=%d, expected=%d)",
          selected, id_b);
    expect_self_check_ok("while the panel is open");

    if (hold_ms > 0) {
        /* 面板就此停住：截图 / 人工确认列表长什么样 */
        usleep((useconds_t)hold_ms * 1000);
        pump();
        expect_switcher(true, 3, 1);
    }

    /* 再按一格：b → a。继续往 z 序**表尾**走，而不是弹回当前窗口 */
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);
    expect_switcher(true, 3, 2);
    selected = switcher_selected();
    CHECK(selected == id_a, "second Tab walks further down the z-order (selected=%d, expected=%d)",
          selected, id_a);

    /* ---------- 松开 Alt：提交，焦点真的换过去 ---------- */
    send_key(KEY_LEFTALT, false);
    send_mods(0);
    CHECK(focused_view() == id_a,
          "Alt release commits the highlight (focus=%d, expected=%d)", focused_view(), id_a);
    expect_switcher(false, 0, -1);
    expect_self_check_ok("after committing");

    /* ---------- 第二轮：提交把 a 提到表头，z 序变 [a, c, b]，一格该落到 c ---------- */
    send_mods(alt_mask);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);
    expect_switcher(true, 3, 1);
    selected = switcher_selected();
    CHECK(selected == id_c,
          "the raise after commit rewrote the order (selected=%d, expected=%d)", selected, id_c);
    send_key(KEY_LEFTALT, false);
    send_mods(0);
    CHECK(focused_view() == id_c, "second round commits c (focus=%d, expected=%d)", focused_view(),
          id_c);

    /* ---------- 第三轮：面板开着时最小化**高亮那个**候选 ---------- */
    /*
     * 最小化只发 view_changed、不发 view_unmapped（woodland_view_set_minimized），
     * 所以这一轮是"候选集被改写"那条路径唯一的真客户端验证。断言的是**反的**：
     * 这一行要还在——Alt+Tab 就是把最小化窗口叫回来的入口，摘掉等于再也切不回来。
     * 需要"它确实变了"的证据就看面板计数不变、minimized 翻转、以及 trace 里的
     * min n=3 / restore。
     */
    send_mods(alt_mask);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);
    expect_switcher(true, 3, 1);
    CHECK(switcher_selected() == id_a, "third round highlights a again");

    xdg_toplevel_set_minimized(win_a.toplevel);
    pump();
    pump();
    expect_switcher(true, 3, 1);
    CHECK(switcher_selected() == id_a,
          "a minimized candidate keeps its row and the highlight (selected=%d, expected=%d)",
          switcher_selected(), id_a);
    CHECK(view_minimized(id_a) == 1, "the compositor reports a as minimized");
    expect_self_check_ok("with a minimized candidate in the list");

    if (hold_ms > 0) {
        /* 面板停在"有一行是最小化的"这个状态：截图看压暗 + 后缀 */
        usleep((useconds_t)hold_ms * 1000);
        pump();
        expect_switcher(true, 3, 1);
    }

    /*
     * 这里不验"客户端自己取消最小化"：Wayland stable xdg-shell 有意只提供
     * `set_minimized`、不提供 `unset_minimized`（恢复由合成器或任务栏触发）。
     * 所以 `unmin` 分支由单测覆盖（直接改字段再补发 view_changed），
     * 端到端只钉"提交时由合成器恢复"这一条。
     */
    send_key(KEY_LEFTALT, false);
    send_mods(0);
    CHECK(focused_view() == id_a,
          "commit restores the minimized window and focuses it (focus=%d, expected=%d)",
          focused_view(), id_a);
    CHECK(view_minimized(id_a) == 0, "the committed window is no longer minimized");
    expect_switcher(false, 0, -1);

    /* ---------- 第四轮：先最小化、后开面板（收集路径） ---------- */
    /*
     * 第三轮验的是"面板开着的时候最小化"（view_changed 分支），这一轮验收集
     * 那一刻就已经最小化的窗口——那是 switcher_collect 用 restorable 而不是
     * focusable 的关键分支。两处任一处写错，列表里就少一行。
     *
     * 专挑**没有焦点**的 b 下手：最小化一个非焦点窗口不会触发焦点回退，z 序
     * 也不动，所以"按两下 Tab 落到谁"是确定的（a → c → b）。换成最小化 a
     * 就要先算焦点落到哪、还要算 fallback 有没有 raise，等于把断言建在一堆
     * 与本题无关的推论上。
     */
    xdg_toplevel_set_minimized(win_b.toplevel);
    pump();
    pump();
    CHECK(view_minimized(id_b) == 1, "round 4: b starts minimized");
    CHECK(focused_view() == id_a,
          "minimizing an unfocused window does not move focus (focus=%d, expected=%d)",
          focused_view(), id_a);

    send_mods(alt_mask);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);
    expect_switcher(true, 3, 1);
    CHECK(switcher_selected() == id_c, "round 4: first Tab still walks to c (selected=%d)",
          switcher_selected());
    send_key(KEY_TAB, true);
    send_key(KEY_TAB, false);
    expect_switcher(true, 3, 2);
    CHECK(switcher_selected() == id_b,
          "a window minimized before begin is still reachable (selected=%d, expected=%d)",
          switcher_selected(), id_b);
    expect_self_check_ok("with a window minimized before begin");

    send_key(KEY_LEFTALT, false);
    send_mods(0);
    CHECK(focused_view() == id_b,
          "commit brings the pre-minimized window back (focus=%d, expected=%d)", focused_view(),
          id_b);
    CHECK(view_minimized(id_b) == 0, "round 4: b is restored");
    expect_switcher(false, 0, -1);

    /* ---------- 第五轮：按键 press/release 配对（先按住的 Shift 与晚到的 Tab/Esc release） ---------- */
    /*
     * 钉住 seat 层吞键位图的两条对称边界：
     *   1. 面板打开前已按下的 Shift，在面板打开后松开，release 必须放行给客户端
     *      （否则 Escape 取消后焦点没变，客户端里的 Shift 会一直卡在按下）；
     *   2. 面板打开期间被吞掉 press 的 Tab / Escape，即使在面板因 Escape 或松开
     *      Alt 关闭之后才松开，晚到的 release 也必须被吞掉（不能漏成孤儿 release）。
     */
    xkb_mod_index_t shift_idx = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    uint32_t shift_mask = (shift_idx != XKB_MOD_INVALID) ? (1u << shift_idx) : 0;

    send_mods(alt_mask | shift_mask);
    send_key(KEY_LEFTSHIFT, true);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true); /* 按住 Tab 不松，打开面板（反向预选 index 2） */
    expect_switcher(true, 3, 2);

    /* 面板开着时先松开 Shift：它的 press 发给了客户端，release 必须配对放行 */
    send_key(KEY_LEFTSHIFT, false);
    send_mods(alt_mask);
    CHECK(!client_held[KEY_LEFTSHIFT],
          "Shift pressed before switcher opened is released on client when released mid-switch");

    /* 按 Escape 取消面板（此时 Tab 仍按着）：面板立刻关闭，随后才松开 Escape、Tab、Alt */
    send_key(KEY_ESC, true);
    expect_switcher(false, 0, -1);
    send_key(KEY_ESC, false);
    send_key(KEY_TAB, false);
    send_key(KEY_LEFTALT, false);
    send_mods(0);
    CHECK(focused_view() == id_b,
          "Escape cancels switcher without changing focus (focus=%d, expected=%d)",
          focused_view(), id_b);

    /* 再验"先松 Alt 提交切焦点、后松 Tab"：新焦点窗口的 enter 与后续都不该收到孤儿 Tab↑ */
    send_mods(alt_mask);
    send_key(KEY_LEFTALT, true);
    send_key(KEY_TAB, true);
    expect_switcher(true, 3, 1);
    send_key(KEY_LEFTALT, false);
    send_mods(0);
    expect_switcher(false, 0, -1);
    send_key(KEY_TAB, false);

    CHECK(orphan_releases == 0,
          "no orphan key release leaked to the client (got %d)", orphan_releases);
    CHECK(client_held_count() == 0,
          "no key left stuck in pressed state on the client (got %d)", client_held_count());
    CHECK(swallowed_leaks == 0,
          "swallowed Tab/Escape keys never leaked to the client (got %d)", swallowed_leaks);

    /*
     * 收尾要干净：ASan 构建里 LeakSanitizer 会在 exit 时报掉没释放的 wl_proxy
     * 并让整条用例判 FAIL——而协议断言其实全过了，看着像功能坏了。
     *
     * 别指望 wl_display_disconnect() 兜底：它只关 fd、释放 display 自己的记账，
     * 客户端自己 bind / create 出来的 proxy 还得自己 destroy（实测不管的话
     * 稳定漏 9 个）。顺序按依赖倒着来：先窗口的对象，再全局。
     */
    struct window *wins[] = {&win_a, &win_b, &win_c};
    for (size_t i = 0; i < sizeof(wins) / sizeof(wins[0]); i++) {
        if (wins[i]->toplevel) {
            xdg_toplevel_destroy(wins[i]->toplevel);
        }
        if (wins[i]->xdg_surface) {
            xdg_surface_destroy(wins[i]->xdg_surface);
        }
        if (wins[i]->surface) {
            wl_surface_destroy(wins[i]->surface);
        }
        if (wins[i]->buffer) {
            wl_buffer_destroy(wins[i]->buffer);
        }
    }
    zwp_virtual_keyboard_v1_destroy(vk);
    zwp_virtual_keyboard_manager_v1_destroy(vk_mgr);
    if (wl_kb) {
        wl_keyboard_release(wl_kb);
    }
    wl_seat_release(seat);
    xdg_wm_base_destroy(wm_base);
    wl_shm_destroy(shm);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);

    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    wl_display_flush(display);
    wl_display_disconnect(display);

    if (failures == 0) {
        fprintf(stderr, "PASS\n");
        return 0;
    }
    fprintf(stderr, "FAIL %d check(s) failed\n", failures);
    return 1;
}
