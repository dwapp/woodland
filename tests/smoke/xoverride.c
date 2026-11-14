// SPDX-License-Identifier: GPL-2.0-only
/*
 * 测试用最小 X11 客户端：创建一个 override-redirect 窗口并 map。
 *
 * 用来验证 compositor 的 unmanaged 路径（菜单/提示类窗口）：不生成
 * woodland_view、不画 SSD、不参与焦点，位置由 X 侧坐标决定。
 *
 * 用法: xoverride [x] [y] [w] [h]，窗口底色为纯红，map 后打印窗口 id 并阻塞。
 *       xoverride --managed-window [x y w h] 另外建一个**受管理**的 X11 顶层窗口
 *       （不带 override_redirect），供 treeland-xwindow-control 冒烟验证 managed
 *       分支；--test-toggle 做 override_redirect 的双向迁移回归。
 *       三个开关可以和几何任意顺序给，加 --dry-run 只解析参数、不连 X。
 */
#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>

enum probe_mode {
    PROBE_OVERRIDE, /* 默认：override-redirect（unmanaged 路径） */
    PROBE_MANAGED,  /* 普通顶层窗口（走 woodland_view + SSD） */
    PROBE_TOGGLE,   /* override_redirect 双向迁移回归 */
};

static const char *mode_name(enum probe_mode mode)
{
    switch (mode) {
    case PROBE_MANAGED:
        return "managed";
    case PROBE_TOGGLE:
        return "toggle";
    default:
        return "override-redirect";
    }
}

int main(int argc, char *argv[]) {
    /*
     * 参数解析放在连 X 之前。
     *
     * 原来这里是 `int x = argc > 1 ? atoi(argv[1]) : 200;`，而 argv[1] 是
     * "--managed-window" / "--test-toggle" 时 atoi 得 0 —— 于是带开关的那两种
     * 模式里 x 恒为 0（窗口贴在屏幕最左边），而"再往后传几何"会整体错位一格
     * （x 吃 flag、y 吃 x……）。开关和几何分开解析，顺序也就不重要了。
     */
    enum probe_mode mode = PROBE_OVERRIDE;
    int pos[4] = {200, 200, 160, 120};
    int npos = 0;
    bool dry_run = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--managed-window") == 0) {
            mode = PROBE_MANAGED;
        } else if (strcmp(argv[i], "--test-toggle") == 0) {
            mode = PROBE_TOGGLE;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (npos < 4) {
            pos[npos++] = atoi(argv[i]);
        } else {
            fprintf(stderr, "用法: xoverride [--managed-window|--test-toggle]"
                            " [--dry-run] [x y w h]\n");
            return 2;
        }
    }

    int x = pos[0], y = pos[1], w = pos[2], h = pos[3];
    if (w <= 0 || h <= 0) {
        /* 0 宽/高会让 CreateWindow 直接 BadValue，窗口根本不存在，
         * 而调用方还在等一个永远不会出现的 view —— 宁可在本地就炸掉 */
        fprintf(stderr, "窗口尺寸必须为正（拿到 %dx%d）\n", w, h);
        return 2;
    }

    if (dry_run) {
        printf("dry-run: mode=%s x=%d y=%d w=%d h=%d\n", mode_name(mode), x, y, w, h);
        return 0;
    }

    int screen_num = 0;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "cannot connect to X display\n");
        return 1;
    }

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&iter);
    }
    xcb_screen_t *screen = iter.data;

    /* 纯红底色，便于截图后按像素断言 */
    uint32_t red = screen->white_pixel;
    xcb_alloc_color_cookie_t cookie = xcb_alloc_color(
        conn, screen->default_colormap, 0xffff, 0x0000, 0x0000);
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(conn, cookie, NULL);
    if (reply) {
        red = reply->pixel;
        free(reply);
    }

    xcb_window_t win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;

    if (mode == PROBE_MANAGED) {
        /* 普通顶层窗口：合成器会建 woodland_view / SSD，用于按 wid 摆位的
         * managed 分支。打印的 0x.. 就是协议要的 X11 window id。 */
        uint32_t val_managed[] = {red, 0};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          mask, val_managed);
        xcb_map_window(conn, win);
        xcb_flush(conn);
        printf("managed window 0x%x at %d,%d %dx%d\n", win, x, y, w, h);
        fflush(stdout);
    } else if (mode == PROBE_TOGGLE) {
        /* 先以 managed (override_redirect = 0) 创建并 map */
        uint32_t val_managed[] = {red, 0};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          mask, val_managed);
        xcb_map_window(conn, win);
        xcb_flush(conn);
        usleep(50000);

        /* 动态切成 override-redirect = 1（触发 xwl_set_override_redirect 迁移） */
        uint32_t val_or[] = {1};
        xcb_change_window_attributes(conn, win, XCB_CW_OVERRIDE_REDIRECT, val_or);
        xcb_flush(conn);
        usleep(50000);

        /* 再切回 managed = 0（触发 unmanaged_set_override_redirect 迁移） */
        uint32_t val_back[] = {0};
        xcb_change_window_attributes(conn, win, XCB_CW_OVERRIDE_REDIRECT, val_back);
        xcb_flush(conn);
        usleep(50000);

        printf("TOGGLE_OK\n");
        fflush(stdout);
    } else {
        uint32_t values[] = {red, 1};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          mask, values);
        xcb_map_window(conn, win);
        xcb_flush(conn);

        printf("override-redirect window 0x%x at %d,%d %dx%d\n", win, x, y, w, h);
        fflush(stdout);
    }
    /* 挂住等 SIGTERM（冒烟脚本负责收尾） */
    pause();
    xcb_disconnect(conn);
    return 0;
}
