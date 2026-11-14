// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SHORTCUTD_CONFIG_H
#define WOODLAND_SHORTCUTD_CONFIG_H

/*
 * woodland-shortcutd（以下简称 wsd）的配置模型。
 *
 * 一条 [[shortcut]] 对应一次 treeland_shortcut_manager_v3.bind_key，语义分两类：
 *
 *   command = "foot"          注册时 action=notify，合成器按键命中后回 activated，
 *                             wsd 自己 fork/exec 这条命令。这是非 DDE 环境下
 *                             "Super+Return 开终端" 这类键位的落点。
 *   action  = "tile_left"     直接交给合成器执行内建动作（贴边、切工作区、
 *                             最大化……），wsd 不参与执行。
 *
 * 两者互斥。注意"单条命令执行失败"是运行期行为，"键位能不能注册上"由合成器
 * 校验（QKeySequence PortableText + validity 规则），wsd 不重复实现那份解析器，
 * 只把 bind_failure 记成日志——合成器是唯一事实源。
 *
 * 命名：wsd = woodland shortcut daemon。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * keybind_flag：与 treeland-shortcut-manager-v3 的枚举逐位同值。
 * 这里不 include 生成的协议头，是为了让配置模块能被单元测试独立链接；
 * src/shortcutd/main.c 里有 _Static_assert 对着协议头复验，改协议时编译期就炸。
 */
#define WSD_FLAG_PRESS   0x1u
#define WSD_FLAG_RELEASE 0x2u
#define WSD_FLAG_REPEAT  0x4u
#define WSD_FLAG_MASK    (WSD_FLAG_PRESS | WSD_FLAG_RELEASE | WSD_FLAG_REPEAT)

/* treeland-shortcut-manager-v3 的 action 枚举（同样靠 _Static_assert 对齐） */
enum wsd_action {
    WSD_ACTION_NOTIFY = 0,
    WSD_ACTION_WORKSPACE_1 = 1,
    WSD_ACTION_WORKSPACE_2 = 2,
    WSD_ACTION_WORKSPACE_3 = 3,
    WSD_ACTION_WORKSPACE_4 = 4,
    WSD_ACTION_WORKSPACE_5 = 5,
    WSD_ACTION_WORKSPACE_6 = 6,
    WSD_ACTION_WORKSPACE_7 = 7,
    WSD_ACTION_WORKSPACE_8 = 8,
    WSD_ACTION_WORKSPACE_9 = 9,
    WSD_ACTION_WORKSPACE_10 = 10,
    WSD_ACTION_WORKSPACE_11 = 11,
    WSD_ACTION_WORKSPACE_12 = 12,
    WSD_ACTION_PREV_WORKSPACE = 13,
    WSD_ACTION_NEXT_WORKSPACE = 14,
    WSD_ACTION_MAXIMIZE = 15,
    WSD_ACTION_CANCEL_MAXIMIZE = 16,
    WSD_ACTION_MINIMIZE = 17,
    WSD_ACTION_MOVE_WINDOW = 18,
    WSD_ACTION_RESIZE_WINDOW = 19,
    WSD_ACTION_CLOSE_WINDOW = 20,
    WSD_ACTION_SHOW_WINDOW_MENU = 21,
    WSD_ACTION_MOVE_WINDOW_TO_PREV_WORKSPACE = 22,
    WSD_ACTION_MOVE_WINDOW_TO_NEXT_WORKSPACE = 23,
    WSD_ACTION_SHOW_DESKTOP = 24,
    WSD_ACTION_OPEN_MULTITASK_VIEW = 25,
    WSD_ACTION_CLOSE_MULTITASK_VIEW = 26,
    WSD_ACTION_TOGGLE_MULTITASK_VIEW = 27,
    WSD_ACTION_TASKSWITCH_NEXT = 28,
    WSD_ACTION_TASKSWITCH_PREV = 29,
    WSD_ACTION_TASKSWITCH_SAMEAPP_NEXT = 30,
    WSD_ACTION_TASKSWITCH_SAMEAPP_PREV = 31,
    WSD_ACTION_TILE_LEFT = 32,
    WSD_ACTION_TILE_RIGHT = 33,
    WSD_ACTION_TILE_TOP = 34,
    WSD_ACTION_TILE_BOTTOM = 35,
    WSD_ACTION_TILE_TOP_LEFT = 36,
    WSD_ACTION_TILE_TOP_RIGHT = 37,
    WSD_ACTION_TILE_BOTTOM_LEFT = 38,
    WSD_ACTION_TILE_BOTTOM_RIGHT = 39,
    WSD_ACTION_ZOOM_IN = 40,
    WSD_ACTION_ZOOM_OUT = 41,
    WSD_ACTION_ZOOM_RESET = 42,
    WSD_ACTION_TOGGLE_FPS_DISPLAY = 43,
    WSD_ACTION_LOCKSCREEN = 44,
    WSD_ACTION_SHUTDOWN_MENU = 45,
};

#define WSD_ACTION_MAX WSD_ACTION_SHUTDOWN_MENU

struct wsd_binding {
    char *name;    /* 注册给合成器的唯一名字，就是 key 文本（配置层保证 key 唯一） */
    char *key;     /* QKeySequence PortableText */
    uint32_t flags;
    uint32_t action;
    char *command; /* NULL 表示不 spawn（action 交给合成器） */
    bool builtin_default;
};

struct wsd_config {
    bool enabled; /* [general] enabled，默认 true */
    struct wsd_binding *bindings;
    size_t count;
    size_t capacity;
};

enum wsd_load_result {
    WSD_LOAD_OK = 0,
    WSD_LOAD_MISSING, /* 文件不存在：调用方决定要不要填默认表 */
    WSD_LOAD_ERROR,   /* 读失败或 TOML 语法错误，err 里有人话 */
};

void wsd_config_init(struct wsd_config *cfg);
void wsd_config_clear(struct wsd_config *cfg);

/* $XDG_CONFIG_HOME/woodland/shortcuts.toml，回退 $HOME/.config/... */
const char *wsd_config_default_path(char *buf, size_t len);

/*
 * 读配置文件。单条 [[shortcut]] 语义有错（缺 key、action 不认识、command 与
 * action 同时给）时跳过该条并往 stderr 记一行，不影响其余条目——一次拼错
 * 不该让整套键位失效。整份文件的语法错误才算 WSD_LOAD_ERROR。
 */
enum wsd_load_result wsd_config_load_file(struct wsd_config *cfg, const char *path,
                                          char *err, size_t errlen);

/* 内置默认键位：命令类条目按 PATH 探测挑第一个可用程序，全都不可用就跳过 */
void wsd_config_load_defaults(struct wsd_config *cfg);

/*
 * 这个 key 是否已经在表里。比较形式忽略大小写与空白：合成器侧的解析是
 * 大小写不敏感的（`meta+left` 与 `Meta+Left` 是同一个组合），而一条组合在
 * 合成器里只能有一条生效——同 action 会被 bind_key 的 upsert 吞掉（并且
 * 保留先注册的 name），不同 action 只有表里靠前的那条会命中。所以重复的
 * key 必须在配置层就挡掉，否则表现为"第二条静默不生效"。
 */
bool wsd_config_has_key(const struct wsd_config *cfg, const char *key);

/*
 * 追加一条绑定。name 由 key 派生（就是 key 文本，配置层保证 key 唯一）。
 * 参数非法、key 与已有条目重复、或内存不足返回 false。
 */
bool wsd_config_add(struct wsd_config *cfg, const char *key, uint32_t action,
                    uint32_t flags, const char *command);

bool wsd_action_from_name(const char *name, uint32_t *action);
const char *wsd_action_name(uint32_t action);

/* "press" / "press|repeat" / "release,repeat"；空串或未知词返回 false */
bool wsd_flags_from_string(const char *text, uint32_t *flags);

/* 命令的第一个词是否能在 PATH 里找到（绝对/相对路径则直接看可执行位） */
bool wsd_command_available(const char *command);

/* --print-default-config：输出可直接当配置文件用的示例（= 内置默认键位） */
void wsd_print_sample_config(FILE *out);

/* --list：把当前生效的绑定打成一行一条，方便和 wctl get_tree 对照 */
void wsd_config_dump(const struct wsd_config *cfg, FILE *out);

#endif /* WOODLAND_SHORTCUTD_CONFIG_H */
