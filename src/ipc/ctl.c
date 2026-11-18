// SPDX-License-Identifier: GPL-2.0-only
/*
 * 调试接口的写命令：聚焦 / 移动 / 改尺寸 / 关闭 / 切工作区 / 注入指针与按键。
 *
 * 只在 --debug-ctl 下生效。默认调试 socket 保持只读：写命令要和正常输入路径
 * 共用状态，一个 bug 就会变成"合成器被调试工具搞坏了"，所以必须显式打开。
 *
 * 设计取舍：
 *   - 全部走合成器自己的入口（woodland_focus_view / woodland_view_set_* /
 *     cursor 的 button 信号），不绕过去直接改字段——AI 驱动出来的状态和
 *     真人操作出来的状态必须是同一种。
 *   - key 只送给当前键盘焦点的客户端，不经过合成器的快捷键表：它回答
 *     "客户端收到按键会怎样"，不是"快捷键绑定对不对"。
 *   - 响应一律带 ok 与 cmd；失败带 error，参数错误不会改任何状态。
 */
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scene.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>

#include "focus.h"
#include "ipc.h"
#include "ssd.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

/* ==================== 文档（并入 help） ==================== */

struct ctl_cmd_doc {
    const char *name;
    const char *usage;
    const char *desc;
};

static const struct ctl_cmd_doc ctl_docs[] = {
    {"focus_view", "focus_view <view_id>", "give keyboard focus to a view (raises it, switches workspace)"},
    {"close", "close <view_id>", "ask a view to close (same path as the titlebar button)"},
    {"move", "move <view_id> <x> <y>",
     "move the OUTER frame's top-left to layout coords x,y (no snap, no clamp)"},
    {"resize", "resize <view_id> <w> <h>", "request a content size of w x h (client may refuse)"},
    {"maximize", "maximize <view_id> <0|1>", "set maximized state"},
    {"fullscreen", "fullscreen <view_id> <0|1>", "set fullscreen state"},
    {"minimize", "minimize <view_id> <0|1>", "set minimized state"},
    {"workspace", "workspace <name>", "switch to workspace by name (\"1\"..\"10\")"},
    {"pointer_move", "pointer_move <x> <y>",
     "warp the cursor to layout coords and run the normal motion path"},
    {"pointer_button", "pointer_button <left|right|middle> <press|release|click>",
     "emit a button through the real cursor handler (click = press + release)"},
    {"inject_key", "inject_key <linux_keycode> <press|release|tap>",
     "send a key to the focused client only (bypasses compositor shortcuts)"},
};

#define CTL_CMD_COUNT (sizeof(ctl_docs) / sizeof(ctl_docs[0]))

void woodland_ipc_ctl_help(const struct woodland_server *server, struct woodland_json *out,
                           struct woodland_json_arr *cmds)
{
    if (!server->debug_ctl) {
        return;
    }
    for (size_t i = 0; i < CTL_CMD_COUNT; i++) {
        woodland_json_arr_sep(cmds);
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_str(&obj, "name", ctl_docs[i].name);
        woodland_json_obj_str(&obj, "desc", ctl_docs[i].desc);
        woodland_json_obj_str(&obj, "usage", ctl_docs[i].usage);
        woodland_json_obj_bool(&obj, "write", true);
        woodland_json_obj_key(&obj, "fields");
        woodland_json_raw(out, "[]");
        woodland_json_obj_close(&obj);
    }
}

/* ==================== 小工具 ==================== */

static void respond(struct woodland_json *out, const char *cmd, bool ok, const char *error)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", ok);
    woodland_json_obj_str(&obj, "cmd", cmd);
    if (error) {
        woodland_json_obj_str(&obj, "error", error);
    }
    woodland_json_obj_close(&obj);
}

/* 取下一个空白分隔的词；没有则返回 NULL。tok 的容量由调用方保证 */
static const char *next_token(const char *p, char *tok, size_t cap)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return NULL;
    }
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t') {
        if (n + 1 < cap) {
            tok[n] = p[n];
        }
        n++;
    }
    tok[n < cap ? n : cap - 1] = '\0';
    return p + n;
}

static bool parse_long(const char *s, long *v)
{
    char *end = NULL;
    errno = 0;
    long r = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *v = r;
    return true;
}

/* 解析 n 个整数参数；多给或少给都算错，避免"悄悄忽略多余参数" */
static bool parse_ints(const char *args, long *vals, int n)
{
    char tok[32];
    for (int i = 0; i < n; i++) {
        args = next_token(args, tok, sizeof(tok));
        if (!args || !parse_long(tok, &vals[i])) {
            return false;
        }
    }
    return next_token(args, tok, sizeof(tok)) == NULL;
}

static struct woodland_view *find_view(struct woodland_server *server, long id)
{
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if ((long)view->id == id) {
            return view;
        }
    }
    return NULL;
}

static uint32_t now_msec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ==================== 命令实现 ==================== */

static bool ctl_view_cmd(struct woodland_server *server, const char *cmd, const char *args,
                         struct woodland_json *out)
{
    int nvals = 1;
    if (strcmp(cmd, "move") == 0 || strcmp(cmd, "resize") == 0) {
        nvals = 3;
    } else if (strcmp(cmd, "maximize") == 0 || strcmp(cmd, "fullscreen") == 0 ||
               strcmp(cmd, "minimize") == 0) {
        nvals = 2;
    }
    long v[3] = {0};
    if (!parse_ints(args, v, nvals)) {
        respond(out, cmd, false, "bad arguments (see `help` for usage)");
        return false;
    }
    struct woodland_view *view = find_view(server, v[0]);
    if (!view) {
        respond(out, cmd, false, "no such view");
        return false;
    }
    if (nvals == 2 && v[1] != 0 && v[1] != 1) {
        respond(out, cmd, false, "state must be 0 or 1");
        return false;
    }

    if (strcmp(cmd, "focus_view") == 0) {
        if (!woodland_view_focusable(view)) {
            respond(out, cmd, false, "view is not focusable");
            return false;
        }
        woodland_focus_view(view, true);
    } else if (strcmp(cmd, "close") == 0) {
        woodland_view_close(view);
    } else if (strcmp(cmd, "move") == 0) {
        if (!view->mapped || !view->scene_tree) {
            respond(out, cmd, false, "view is not mapped");
            return false;
        }
        struct woodland_ssd_margins m = {0};
        woodland_ssd_get_margins(view, &m);
        wlr_scene_node_set_position(&view->scene_tree->node, (int)v[1] + m.left, (int)v[2] + m.top);
        woodland_view_update_outputs(view);
    } else if (strcmp(cmd, "resize") == 0) {
        if (v[1] <= 0 || v[2] <= 0 || v[1] > 16384 || v[2] > 16384) {
            respond(out, cmd, false, "size out of range (1..16384)");
            return false;
        }
        woodland_view_request_size(view, (int)v[1], (int)v[2]);
    } else if (strcmp(cmd, "maximize") == 0) {
        woodland_view_set_maximized(view, v[1] == 1);
    } else if (strcmp(cmd, "fullscreen") == 0) {
        woodland_view_set_fullscreen(view, v[1] == 1, NULL);
    } else {
        woodland_view_set_minimized(view, v[1] == 1);
    }
    woodland_trace_add("action", view->id, "debug-ctl %s", cmd);
    respond(out, cmd, true, NULL);
    return true;
}

static bool ctl_workspace(struct woodland_server *server, const char *args,
                          struct woodland_json *out)
{
    char name[32];
    const char *rest = next_token(args, name, sizeof(name));
    char extra[8];
    if (!rest || next_token(rest, extra, sizeof(extra))) {
        respond(out, "workspace", false, "usage: workspace <name>");
        return false;
    }
    struct woodland_workspace *ws = woodland_workspace_by_name(server, name);
    if (!ws) {
        respond(out, "workspace", false, "no such workspace");
        return false;
    }
    woodland_workspace_switch_focus(server, ws, true);
    woodland_trace_add("action", 0, "debug-ctl workspace %s", name);
    respond(out, "workspace", true, NULL);
    return true;
}

static bool ctl_pointer_move(struct woodland_server *server, const char *args,
                             struct woodland_json *out)
{
    long v[2];
    if (!parse_ints(args, v, 2)) {
        respond(out, "pointer_move", false, "usage: pointer_move <x> <y>");
        return false;
    }
    /* NULL device：绝对坐标 warp，不依赖任何物理指针设备 */
    wlr_cursor_warp(server->cursor, NULL, (double)v[0], (double)v[1]);
    woodland_cursor_process_motion(server, now_msec());
    respond(out, "pointer_move", true, NULL);
    return true;
}

static void emit_button(struct woodland_server *server, uint32_t button, bool pressed)
{
    struct wlr_pointer_button_event ev = {
        .pointer = NULL,
        .time_msec = now_msec(),
        .button = button,
        .state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
    };
    /* 走 cursor 的 button 信号：点击聚焦/标题栏按钮/菜单都是这条路上的监听器 */
    wl_signal_emit_mutable(&server->cursor->events.button, &ev);
}

static bool ctl_pointer_button(struct woodland_server *server, const char *args,
                               struct woodland_json *out)
{
    char which[16], action[16], extra[8];
    const char *rest = next_token(args, which, sizeof(which));
    rest = rest ? next_token(rest, action, sizeof(action)) : NULL;
    if (!rest || next_token(rest, extra, sizeof(extra))) {
        respond(out, "pointer_button", false, "usage: pointer_button <left|right|middle> <press|release|click>");
        return false;
    }
    uint32_t button;
    if (strcmp(which, "left") == 0) {
        button = BTN_LEFT;
    } else if (strcmp(which, "right") == 0) {
        button = BTN_RIGHT;
    } else if (strcmp(which, "middle") == 0) {
        button = BTN_MIDDLE;
    } else {
        respond(out, "pointer_button", false, "button must be left|right|middle");
        return false;
    }
    if (strcmp(action, "press") == 0) {
        emit_button(server, button, true);
    } else if (strcmp(action, "release") == 0) {
        emit_button(server, button, false);
    } else if (strcmp(action, "click") == 0) {
        emit_button(server, button, true);
        emit_button(server, button, false);
    } else {
        respond(out, "pointer_button", false, "action must be press|release|click");
        return false;
    }
    respond(out, "pointer_button", true, NULL);
    return true;
}

static bool ctl_inject_key(struct woodland_server *server, const char *args,
                           struct woodland_json *out)
{
    char code[16], action[16], extra[8];
    const char *rest = next_token(args, code, sizeof(code));
    rest = rest ? next_token(rest, action, sizeof(action)) : NULL;
    long keycode;
    if (!rest || next_token(rest, extra, sizeof(extra)) || !parse_long(code, &keycode) ||
        keycode <= 0 || keycode > 0x2ff) {
        respond(out, "inject_key", false, "usage: inject_key <linux_keycode> <press|release|tap>");
        return false;
    }
    if (!server->seat || !server->seat->keyboard_state.focused_surface) {
        respond(out, "inject_key", false, "no keyboard focus");
        return false;
    }
    bool press = strcmp(action, "press") == 0 || strcmp(action, "tap") == 0;
    bool release = strcmp(action, "release") == 0 || strcmp(action, "tap") == 0;
    if (!press && !release) {
        respond(out, "inject_key", false, "action must be press|release|tap");
        return false;
    }
    if (press) {
        wlr_seat_keyboard_notify_key(server->seat, now_msec(), (uint32_t)keycode,
                                     WL_KEYBOARD_KEY_STATE_PRESSED);
    }
    if (release) {
        wlr_seat_keyboard_notify_key(server->seat, now_msec(), (uint32_t)keycode,
                                     WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    respond(out, "inject_key", true, NULL);
    return true;
}

/* ==================== 入口 ==================== */

bool woodland_ipc_ctl_dispatch(struct woodland_server *server, const char *cmd, const char *args,
                               struct woodland_json *out, bool *ok)
{
    bool known = false;
    for (size_t i = 0; i < CTL_CMD_COUNT; i++) {
        if (strcmp(cmd, ctl_docs[i].name) == 0) {
            known = true;
            break;
        }
    }
    if (!known) {
        return false;
    }
    if (!server->debug_ctl) {
        respond(out, cmd, false, "write commands disabled: start woodland with --debug-ctl");
        *ok = false;
        return true;
    }

    if (strcmp(cmd, "workspace") == 0) {
        *ok = ctl_workspace(server, args, out);
    } else if (strcmp(cmd, "pointer_move") == 0) {
        *ok = ctl_pointer_move(server, args, out);
    } else if (strcmp(cmd, "pointer_button") == 0) {
        *ok = ctl_pointer_button(server, args, out);
    } else if (strcmp(cmd, "inject_key") == 0) {
        *ok = ctl_inject_key(server, args, out);
    } else {
        *ok = ctl_view_cmd(server, cmd, args, out);
    }
    return true;
}
