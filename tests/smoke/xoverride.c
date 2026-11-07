// SPDX-License-Identifier: GPL-2.0-only
/*
 * 测试用最小 X11 客户端：创建一个 override-redirect 窗口并 map。
 *
 * 用来验证 compositor 的 unmanaged 路径（菜单/提示类窗口）：不生成
 * woodland_view、不画 SSD、不参与焦点，位置由 X 侧坐标决定。
 *
 * 用法: xoverride [x] [y] [w] [h]，窗口底色为纯红，map 后打印 READY 并阻塞。
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>

int main(int argc, char *argv[]) {
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

    int x = argc > 1 ? atoi(argv[1]) : 200;
    int y = argc > 2 ? atoi(argv[2]) : 200;
    int w = argc > 3 ? atoi(argv[3]) : 160;
    int h = argc > 4 ? atoi(argv[4]) : 120;

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

    if (argc > 1 && strcmp(argv[1], "--test-toggle") == 0) {
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
