// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WINDOW_MENU_H
#define WOODLAND_WINDOW_MENU_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct woodland_server;
struct woodland_view;
struct woodland_output;
struct woodland_json;
struct wlr_scene_tree;
struct wlr_scene_buffer;
#if WOODLAND_HAVE_EFFECTS
struct wlr_scene_shadow;
#endif

#define WOODLAND_MENU_ITEM_HEIGHT 30
#define WOODLAND_MENU_SEPARATOR_HEIGHT 9
#define WOODLAND_MENU_PADDING_Y 6
#define WOODLAND_MENU_PADDING_X 4
#define WOODLAND_MENU_MAX_ITEMS 32

enum window_menu_action {
    WINDOW_MENU_ACTION_NONE = 0,
    WINDOW_MENU_ACTION_MAXIMIZE,
    WINDOW_MENU_ACTION_MINIMIZE,
    WINDOW_MENU_ACTION_MOVE,
    WINDOW_MENU_ACTION_RESIZE,
    WINDOW_MENU_ACTION_FULLSCREEN,
    WINDOW_MENU_ACTION_WORKSPACE,
    WINDOW_MENU_ACTION_OUTPUT,
    WINDOW_MENU_ACTION_CLOSE,
};

struct window_menu_item {
    char text[64];
    char shortcut[16];
    bool is_separator;
    bool enabled;
    bool checked;
    bool has_submenu;
    enum window_menu_action action;
    void *action_data;
    char accel_key;
};

struct window_menu_level {
    int x;
    int y;
    int width;
    int height;
    struct wlr_scene_buffer *scene_buffer;
#if WOODLAND_HAVE_EFFECTS
    struct wlr_scene_shadow *shadow;
#endif
    struct window_menu_item items[WOODLAND_MENU_MAX_ITEMS];
    int item_count;
    int hovered_index;
};

struct woodland_window_menu {
    struct woodland_server *server;
    struct woodland_view *view;
    bool active;
    struct wlr_scene_tree *tree;
    struct window_menu_level root;
    struct window_menu_level sub;
    int active_sub_item; /* 主菜单中展开子菜单的项索引，-1 表示无 */
    bool sub_has_focus;  /* 键盘焦点是否在子菜单内 */
    float scale;
    struct woodland_output *output;
};

/**
 * 初始化右键窗口菜单（由 shell_init 调用）。
 */
bool woodland_window_menu_init(struct woodland_server *server);

/**
 * 释放右键窗口菜单（由 shell_finish 调用）。
 */
void woodland_window_menu_finish(struct woodland_server *server);

/**
 * 菜单当前是否处于显示激活状态。
 */
bool woodland_window_menu_is_active(const struct woodland_server *server);

/**
 * 在布局坐标 (lx, ly) 处弹出指定视图的窗口菜单。
 */
void woodland_window_menu_show(struct woodland_view *view, double lx, double ly);

/**
 * 隐藏当前窗口菜单。
 */
void woodland_window_menu_hide(struct woodland_server *server);

/**
 * 若当前弹出的菜单属于指定视图，将其隐藏。
 */
void woodland_window_menu_hide_for_view(struct woodland_view *view);

/**
 * 处理光标按下/释放事件。返回 true 表示事件被菜单消费。
 */
bool woodland_window_menu_handle_button(struct woodland_server *server, uint32_t button,
                                       uint32_t state, double lx, double ly);

/**
 * 处理光标移动事件，更新高亮项或展开子菜单。
 */
void woodland_window_menu_handle_motion(struct woodland_server *server, double lx, double ly);

/**
 * 处理键盘按键事件（导航、确认、快捷键等）。返回 true 表示按键被消费。
 */
bool woodland_window_menu_handle_key(struct woodland_server *server, xkb_keysym_t sym);

/**
 * 导出窗口菜单状态（IPC / wctl get_tree 用）。
 */
void woodland_window_menu_status(struct woodland_server *server, struct woodland_json *out);

#endif /* WOODLAND_WINDOW_MENU_H */
