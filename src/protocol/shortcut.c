// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-shortcut-manager-unstable-v3：全局快捷键与一次性快捷键捕获。
 *
 * 和 treeland-compositor-action 不同，这个协议**是有状态的**：客户端要先
 * acquire 拿到独占，注册一批带名字的绑定，之后由合成器在按键到来时执行
 * 对应 action，或给 action=notify 的绑定回一个 activated 事件。所以这里的
 * 主体是一张 binding 表 + 一个 owner 指针 + 一个 pending capture 对象。
 *
 * 按能力分档（与 include/protocol.h 的档位表一致）：
 *
 *   协议语义完整：acquire 独占（occupied / not_acquired）、bind_key /
 *                 bind_swipe_gesture / bind_hold_gesture / unbind 的四种
 *                 bind_failure（name_conflict / duplicate_binding /
 *                 invalid_argument / internal_error）、bind_key 的
 *                 upsert-on-trigger、capture_next_shortcut 的
 *                 invalid_surface / busy / not_active、capture 对象的
 *                 destroy 取消、activated 事件。
 *   真接线：      action 0（notify）、1..14（工作区切换）、15..20（窗口
 *                 最大化/还原/最小化/移动/缩放/关闭）、22..23（窗口移到
 *                 相邻工作区）、32..39（贴边平铺）。键盘事件由 seat 转发
 *                 到 woodland_protocol_shortcuts_key() 后匹配 QKeySequence
 *                 PortableText。
 *   仅登记不触发：swipe / hold 绑定能注册、能查重、能 unbind，也能在 wctl
 *                 里看到，但 woodland 没有触摸手势识别状态机，不会 Trigger
 *                 对应 action。协议允许「不支持的手势」存在吗？不允许——
 *                 所以每次注册都会在 trace 里明写 gesture_unwired，不假装
 *                 绑定生效。
 *   置空但可观测：21、24..31、40..45（窗口菜单、show-desktop、多任务、
 *                 任务切换、缩放、FPS、锁屏、关机菜单）。没有对应 shell
 *                 状态机，不改状态、不发事件，但记 trace / 日志 / wctl。
 *
 * 部分实现（明确写出来，别当成安全边界）：
 *   - 协议要求 privileged client。woodland 没有 DDE privilege 服务，与
 *     compositor_action.c 一样只校验对端 uid 与合成器相同，挡住跨用户连接。
 *   - 「用户 session 切换时延迟 bind/unbind」没有实现：woodland 是单用户
 *     session 合成器，绑定一律立即生效。
 *   - 裸 Meta（Super 单独按下）只能被 capture 捕获，不能 bind_key：协议
 *     capture_next_shortcut 的 validity 规则明文包含「the Meta (Super) key
 *     pressed alone」，而 bind_key 引用同一套规则，严格说 bind_key 也应
 *     接受 "Meta"。woodland 故意不做：全局注册一个裸 Super 绑定会吃掉
 *     整个桌面生态里大量以 Super 开头的组合键（Super 按下即命中、还拦
 *     不住后续修饰键），收益配不上复杂度。capture 语义不变，bind 对
 *     "Meta" 报 invalid_argument——这是记录在案的偏差，不是疏漏。
 *   - keyboard-shortcuts-inhibit 期间由 seat 跳过本层（见 seat.c），抑制器
 *     优先于全局快捷键，和内置 Alt+ 键位一致。
 *
 * 生命周期：wl_global_destroy() 不会带走已 bind 的 wl_resource。finish 必须
 * 先销毁所有 capture / manager resource（destructor 摘链并清 owner / pending
 * 指针），再释放 binding 表和族状态；否则活着的 resource 上还留着指向已
 * 释放状态的 user_data。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "focus.h"
#include "placement.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "scene.h"
#include "trace.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

#include "treeland-shortcut-manager-unstable-v3-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define SC_VERSION 1
#define SC_ACTION_MAX TREELAND_SHORTCUT_MANAGER_V3_ACTION_SHUTDOWN_MENU
#define SC_FLAG_KEY_PRESS   TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_KEY_PRESS
#define SC_FLAG_KEY_RELEASE TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_KEY_RELEASE
#define SC_FLAG_REPEAT      TREELAND_SHORTCUT_MANAGER_V3_KEYBIND_FLAG_REPEAT
#define SC_FLAG_MASK        (SC_FLAG_KEY_PRESS | SC_FLAG_KEY_RELEASE | SC_FLAG_REPEAT)

/* 结果名是稳定字符串：wctl 与冒烟脚本按名字断言，不写成一句人话 */
static const char *const parse_names[] = {
    [WOODLAND_SC_PARSE_OK] = "ok",
    [WOODLAND_SC_PARSE_EMPTY] = "empty",
    [WOODLAND_SC_PARSE_SEQUENCE] = "sequence",
    [WOODLAND_SC_PARSE_DUPLICATE_MOD] = "duplicate_mod",
    [WOODLAND_SC_PARSE_BAD_TOKEN] = "bad_token",
    [WOODLAND_SC_PARSE_NO_KEY] = "no_key",
    [WOODLAND_SC_PARSE_TOO_LONG] = "too_long",
};

/* 一次请求/激活的处置结果 */
static const char *const outcome_names[] = {
    "none", "bound", "upsert", "unbound", "name_conflict", "duplicate_binding",
    "invalid_argument", "internal_error", "activated", "no_owner", "switched",
    "no_workspace", "no_view", "view_action", "tiled", "moved_workspace",
    "unsupported", "invalid", "gesture_unwired", "captured", "busy",
    "not_active", "aborted", "interrupted", "not_acquired", "occupied",
};

enum shortcut_outcome {
    OUTCOME_NONE = 0,
    OUTCOME_BOUND,
    OUTCOME_UPSERT,
    OUTCOME_UNBOUND,
    OUTCOME_NAME_CONFLICT,
    OUTCOME_DUPLICATE_BINDING,
    OUTCOME_INVALID_ARGUMENT,
    OUTCOME_INTERNAL_ERROR,
    OUTCOME_ACTIVATED,
    OUTCOME_NO_OWNER,
    OUTCOME_SWITCHED,
    OUTCOME_NO_WORKSPACE,
    OUTCOME_NO_VIEW,
    OUTCOME_VIEW_ACTION,
    OUTCOME_TILED,
    OUTCOME_MOVED_WORKSPACE,
    OUTCOME_UNSUPPORTED,
    OUTCOME_INVALID,
    OUTCOME_GESTURE_UNWIRED,
    OUTCOME_CAPTURED,
    OUTCOME_BUSY,
    OUTCOME_NOT_ACTIVE,
    OUTCOME_ABORTED,
    OUTCOME_INTERRUPTED,
    OUTCOME_NOT_ACQUIRED,
    OUTCOME_OCCUPIED,
};

static const char *outcome_name(enum shortcut_outcome outcome)
{
    if ((size_t)outcome >= sizeof(outcome_names) / sizeof(outcome_names[0])) {
        return "invalid";
    }
    return outcome_names[outcome];
}

/* ==================== 纯逻辑：PortableText ==================== */

/*
 * Qt 的 PortableText 键名与 XKB keysym 名不完全一致（Del/Esc/PgUp/Enter
 * 等）。这里只补 Qt 常用别名，其余直接交给 xkb_keysym_from_name()。
 * xkb_keysym_from_name() 用 CASE_INSENSITIVE，客户端写 "ctrl+a" 也能收。
 */
struct key_alias {
    const char *qt;
    const char *xkb;
};

static const struct key_alias key_aliases[] = {
    {"del", "Delete"},        {"delete", "Delete"},
    {"esc", "Escape"},        {"escape", "Escape"},
    {"ins", "Insert"},        {"insert", "Insert"},
    {"pgup", "Prior"},         {"pageup", "Prior"},
    {"pgdown", "Next"},       {"pagedown", "Next"},
    {"enter", "Return"},      {"return", "Return"},
    {"backspace", "BackSpace"},
    {"space", "space"},        {"spacebar", "space"},
    {"print", "Print"},        {"prtsc", "Print"},
    {"pause", "Pause"},        {"help", "Help"},
    {"menu", "Menu"},          {"cancel", "Cancel"},
    {"home", "Home"},          {"end", "End"},
    {"up", "Up"},             {"down", "Down"},
    {"left", "Left"},          {"right", "Right"},
    {"tab", "Tab"},
};

static bool ascii_iequal(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool token_is_modifier(const char *token, uint32_t *bit)
{
    if (ascii_iequal(token, "ctrl") || ascii_iequal(token, "control")) {
        *bit = WOODLAND_SC_MOD_CTRL;
        return true;
    }
    if (ascii_iequal(token, "alt")) {
        *bit = WOODLAND_SC_MOD_ALT;
        return true;
    }
    if (ascii_iequal(token, "shift")) {
        *bit = WOODLAND_SC_MOD_SHIFT;
        return true;
    }
    if (ascii_iequal(token, "meta") || ascii_iequal(token, "super") ||
        ascii_iequal(token, "win")) {
        *bit = WOODLAND_SC_MOD_META;
        return true;
    }
    return false;
}

/* sym 是不是纯修饰键；bit 为 0 表示「是修饰键但不参与我们的四位掩码」 */
static bool sym_is_modifier(uint32_t sym, uint32_t *bit)
{
    *bit = 0;
    switch (sym) {
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        *bit = WOODLAND_SC_MOD_CTRL;
        return true;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
        *bit = WOODLAND_SC_MOD_ALT;
        return true;
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        *bit = WOODLAND_SC_MOD_SHIFT;
        return true;
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        *bit = WOODLAND_SC_MOD_META;
        return true;
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Scroll_Lock:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_ISO_Level3_Shift:
        return true;
    default:
        return false;
    }
}

bool woodland_shortcut_key_format(uint32_t mods, uint32_t sym, char *buf, size_t len)
{
    if (!buf || len == 0 || sym == XKB_KEY_NoSymbol) {
        return false;
    }
    char name[64];
    /* Qt 把 Ctrl+a 写成 Ctrl+A：单字母一律取大写，其余键名原样 */
    if (xkb_keysym_get_name(xkb_keysym_to_upper(sym), name, sizeof(name)) < 0) {
        return false;
    }
    int n = snprintf(buf, len, "%s%s%s%s%s",
                     (mods & WOODLAND_SC_MOD_CTRL) ? "Ctrl+" : "",
                     (mods & WOODLAND_SC_MOD_ALT) ? "Alt+" : "",
                     (mods & WOODLAND_SC_MOD_SHIFT) ? "Shift+" : "",
                     (mods & WOODLAND_SC_MOD_META) ? "Meta+" : "",
                     name);
    return n >= 0 && (size_t)n < len;
}

const char *woodland_shortcut_parse_name(enum woodland_shortcut_parse_result result)
{
    if ((size_t)result >= sizeof(parse_names) / sizeof(parse_names[0])) {
        return "unknown";
    }
    return parse_names[result];
}

enum woodland_shortcut_parse_result
woodland_shortcut_key_parse(const char *text, struct woodland_shortcut_key *out)
{
    if (!out) {
        return WOODLAND_SC_PARSE_EMPTY;
    }
    memset(out, 0, sizeof(*out));
    if (!text || !*text) {
        return WOODLAND_SC_PARSE_EMPTY;
    }
    /* 协议只收单个组合；Qt 的多段序列用 ", " 分隔，遇到就明确拒绝 */
    if (strchr(text, ',') != NULL) {
        return WOODLAND_SC_PARSE_SEQUENCE;
    }

    char buf[128];
    if (strlen(text) >= sizeof(buf)) {
        return WOODLAND_SC_PARSE_TOO_LONG;
    }
    memcpy(buf, text, strlen(text) + 1);

    uint32_t mods = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, "+", &saveptr);
    const char *last = NULL;
    while (token) {
        uint32_t bit = 0;
        char *next = strtok_r(NULL, "+", &saveptr);
        if (!next) {
            last = token;
            break;
        }
        if (!token_is_modifier(token, &bit)) {
            return WOODLAND_SC_PARSE_BAD_TOKEN;
        }
        if (mods & bit) {
            return WOODLAND_SC_PARSE_DUPLICATE_MOD;
        }
        mods |= bit;
        token = next;
    }
    if (!last || !*last) {
        return WOODLAND_SC_PARSE_NO_KEY;
    }
    /*
     * 单独一个修饰键不是可匹配的组合，裸 Meta 也一样。协议按
     * capture_next_shortcut 的 validity 规则要求 bind_key 接受 "Meta"，
     * woodland 有意不收这个组合（理由见文件头「部分实现」），因此统一
     * 走 NO_KEY；capture 里裸 Meta 的路径是独立的 held_mods 特判。
     */
    uint32_t ignored_bit = 0;
    if (token_is_modifier(last, &ignored_bit)) {
        return WOODLAND_SC_PARSE_NO_KEY;
    }

    const char *xkb_name = last;
    for (size_t i = 0; i < sizeof(key_aliases) / sizeof(key_aliases[0]); i++) {
        if (ascii_iequal(last, key_aliases[i].qt)) {
            xkb_name = key_aliases[i].xkb;
            break;
        }
    }
    uint32_t sym = xkb_keysym_from_name(xkb_name, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol || sym_is_modifier(sym, &ignored_bit)) {
        return WOODLAND_SC_PARSE_BAD_TOKEN;
    }

    out->mods = mods;
    out->sym = sym;
    if (!woodland_shortcut_key_format(mods, sym, out->text, sizeof(out->text))) {
        return WOODLAND_SC_PARSE_TOO_LONG;
    }
    return WOODLAND_SC_PARSE_OK;
}

bool woodland_shortcut_key_is_valid(uint32_t mods, uint32_t sym)
{
    if (sym == XKB_KEY_NoSymbol) {
        return false;
    }
    uint32_t bit = 0;
    if (sym_is_modifier(sym, &bit)) {
        return false;
    }
    uint32_t s = xkb_keysym_to_lower(sym);

    /* 非修饰键 + Ctrl/Alt/Meta 一律有效 */
    if (mods & (WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT | WOODLAND_SC_MOD_META)) {
        return true;
    }
    /* Shift 只有配空格/Esc/Tab/导航键才算快捷键 */
    if (mods & WOODLAND_SC_MOD_SHIFT) {
        switch (s) {
        case XKB_KEY_space:
        case XKB_KEY_Escape:
        case XKB_KEY_Tab:
        case XKB_KEY_Home:
        case XKB_KEY_End:
        case XKB_KEY_Prior:
        case XKB_KEY_Next:
        case XKB_KEY_Up:
        case XKB_KEY_Down:
        case XKB_KEY_Left:
        case XKB_KEY_Right:
            return true;
        default:
            return false;
        }
    }
    if (s >= XKB_KEY_F1 && s <= XKB_KEY_F35) {
        return true;
    }
    switch (s) {
    case XKB_KEY_Delete:
    case XKB_KEY_BackSpace:
    case XKB_KEY_Pause:
    case XKB_KEY_Print:
    case XKB_KEY_Insert:
    case XKB_KEY_Help:
    case XKB_KEY_Menu:
    case XKB_KEY_Cancel:
        return true;
    default:
        break;
    }
    /* 媒体 / 浏览器 / 亮度 / 启动键都是 XF86* keysym */
    char name[64];
    if (xkb_keysym_get_name(s, name, sizeof(name)) > 0 &&
        strncmp(name, "XF86", 4) == 0) {
        return true;
    }
    return false;
}

bool woodland_shortcut_key_matches(const struct woodland_shortcut_key *key,
                                    uint32_t mods, uint32_t sym)
{
    if (!key || key->sym == XKB_KEY_NoSymbol) {
        return false;
    }
    return key->mods == mods &&
           xkb_keysym_to_lower(key->sym) == xkb_keysym_to_lower(sym);
}

enum woodland_shortcut_action_class
woodland_shortcut_action_classify(uint32_t action)
{
    if (action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_NOTIFY) {
        return WOODLAND_SC_ACTION_NOTIFY;
    }
    if (action >= TREELAND_SHORTCUT_MANAGER_V3_ACTION_WORKSPACE_1 &&
        action <= TREELAND_SHORTCUT_MANAGER_V3_ACTION_WORKSPACE_12) {
        return WOODLAND_SC_ACTION_WORKSPACE;
    }
    if (action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_PREV_WORKSPACE) {
        return WOODLAND_SC_ACTION_WORKSPACE_PREV;
    }
    if (action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_NEXT_WORKSPACE) {
        return WOODLAND_SC_ACTION_WORKSPACE_NEXT;
    }
    if (action >= TREELAND_SHORTCUT_MANAGER_V3_ACTION_MAXIMIZE &&
        action <= TREELAND_SHORTCUT_MANAGER_V3_ACTION_CLOSE_WINDOW) {
        return WOODLAND_SC_ACTION_VIEW;
    }
    if (action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_MOVE_WINDOW_TO_PREV_WORKSPACE ||
        action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_MOVE_WINDOW_TO_NEXT_WORKSPACE) {
        return WOODLAND_SC_ACTION_MOVE_WORKSPACE;
    }
    if (action >= TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_LEFT &&
        action <= TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_BOTTOM_RIGHT) {
        return WOODLAND_SC_ACTION_TILE;
    }
    if (action <= SC_ACTION_MAX) {
        return WOODLAND_SC_ACTION_UNSUPPORTED;
    }
    return WOODLAND_SC_ACTION_INVALID;
}

/* ==================== 族状态 ==================== */

enum shortcut_kind {
    SC_KIND_KEY = 0,
    SC_KIND_SWIPE,
    SC_KIND_HOLD,
};

static const char *const kind_names[] = {"key", "swipe", "hold"};

struct shortcut_binding {
    struct wl_list link;
    enum shortcut_kind kind;
    char *name;
    char *key_text;                        /* SC_KIND_KEY：客户端原文 */
    struct woodland_shortcut_key key;      /* SC_KIND_KEY：规范化结果 */
    uint32_t flags;
    uint32_t finger;
    uint32_t direction;
    uint32_t action;
};

struct shortcut_manager {
    struct wl_list link;
    struct woodland_proto_shortcut *sc;
    struct wl_resource *resource;
};

struct shortcut_capture {
    struct wl_list link;
    struct woodland_proto_shortcut *sc;
    struct wl_resource *resource;
    struct wlr_surface *surface;
    struct wlr_seat *seat;
    bool terminal;         /* 已经发过 captured / failed，对象变 inert */
    uint32_t held_mods;    /* 捕获期间按住的修饰键 */
};

struct woodland_proto_shortcut {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list managers;       /* struct shortcut_manager.link */
    struct wl_list captures;       /* struct shortcut_capture.link */
    struct wl_list bindings;       /* struct shortcut_binding.link */
    struct wl_resource *owner;     /* acquire 成功的那个 manager resource */
    struct shortcut_capture *capture;   /* 当前 pending 的 capture，最多一个 */
    struct wl_listener focus_change;

    /* captured 之后要吞掉的 key_release，协议要求这段时间也算 busy */
    uint32_t drain_keycode;
    /* wlroots 的 key event 不带 repeat 标志：同键连续 press 推断为 repeat */
    uint32_t last_keycode;
    bool last_pressed;

    /* 只读诊断状态 */
    uint32_t bind_count;
    uint32_t unbind_count;
    uint32_t capture_count;
    uint32_t activation_count;
    uint32_t last_action;
    enum shortcut_outcome last_outcome;
    char last_activation[64];
};

static void shortcut_binding_free(struct shortcut_binding *binding)
{
    if (!binding) {
        return;
    }
    /* 自环（wl_list_init 过）表示还没入链，wl_list_remove 对它是安全的 */
    wl_list_remove(&binding->link);
    free(binding->name);
    free(binding->key_text);
    free(binding);
}

static struct shortcut_binding *find_binding_by_name(struct woodland_proto_shortcut *sc,
                                                      const char *name)
{
    struct shortcut_binding *binding;
    wl_list_for_each(binding, &sc->bindings, link)
    {
        if (strcmp(binding->name, name) == 0) {
            return binding;
        }
    }
    return NULL;
}

static void shortcut_record(struct woodland_proto_shortcut *sc,
                            enum shortcut_outcome outcome, uint32_t action,
                            const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void shortcut_record(struct woodland_proto_shortcut *sc,
                            enum shortcut_outcome outcome, uint32_t action,
                            const char *fmt, ...)
{
    if (action != UINT32_MAX) {
        sc->last_action = action;
    }
    sc->last_outcome = outcome;

    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    woodland_trace_add("shortcut", 0, "%s -> %s", detail, outcome_name(outcome));
    wlr_log(WLR_INFO, "shortcut-manager: %s -> %s", detail, outcome_name(outcome));
}

static void shortcut_send_bind_failure(struct woodland_proto_shortcut *sc,
                                       const char *name, uint32_t error)
{
    if (!sc->owner) {
        /* 协议没规定 owner 消失后往哪发；记一条日志，不要给别的客户端发 */
        wlr_log(WLR_INFO, "shortcut-manager: bind_failure(%s, %u) with no owner",
                name ? name : "", error);
        return;
    }
    treeland_shortcut_manager_v3_send_bind_failure(sc->owner, name, error);
}

static enum shortcut_outcome bind_error_outcome(uint32_t error)
{
    switch (error) {
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_NAME_CONFLICT:
        return OUTCOME_NAME_CONFLICT;
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_DUPLICATE_BINDING:
        return OUTCOME_DUPLICATE_BINDING;
    case TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INTERNAL_ERROR:
        return OUTCOME_INTERNAL_ERROR;
    default:
        return OUTCOME_INVALID_ARGUMENT;
    }
}

/* ==================== 动作执行 ==================== */

static struct woodland_view *shortcut_active_view(struct woodland_server *server)
{
    struct woodland_view *view = woodland_focus_current(server);
    return (view && view->mapped) ? view : NULL;
}

static void shortcut_move_view_to_workspace(struct woodland_server *server,
                                             struct woodland_view *view,
                                             struct woodland_workspace *target)
{
    if (!view || !target || view->workspace == target) {
        return;
    }
    /* 先把 scene tree 挪到目标工作区的树下面，再切工作区（会 disable 旧树） */
    view->workspace = target;
    wlr_scene_node_reparent(&view->scene_tree->node, target->tree);
    woodland_workspace_switch_focus(server, target, false);
    woodland_focus_view(view, true);
}

static uint32_t shortcut_tile_edges(uint32_t action)
{
    switch (action) {
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_LEFT:
        return WLR_EDGE_LEFT;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_RIGHT:
        return WLR_EDGE_RIGHT;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_TOP:
        return WLR_EDGE_TOP;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_BOTTOM:
        return WLR_EDGE_BOTTOM;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_TOP_LEFT:
        return WLR_EDGE_TOP | WLR_EDGE_LEFT;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_TOP_RIGHT:
        return WLR_EDGE_TOP | WLR_EDGE_RIGHT;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_BOTTOM_LEFT:
        return WLR_EDGE_BOTTOM | WLR_EDGE_LEFT;
    case TREELAND_SHORTCUT_MANAGER_V3_ACTION_TILE_BOTTOM_RIGHT:
        return WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
    default:
        return 0;
    }
}

static enum shortcut_outcome shortcut_run_action(struct woodland_proto_shortcut *sc,
                                                  struct shortcut_binding *binding,
                                                  uint32_t trigger_flags)
{
    struct woodland_server *server = sc->server;
    uint32_t action = binding->action;
    enum shortcut_outcome outcome = OUTCOME_INVALID;
    char detail[192];

    switch (woodland_shortcut_action_classify(action)) {
    case WOODLAND_SC_ACTION_NOTIFY:
        if (sc->owner) {
            treeland_shortcut_manager_v3_send_activated(sc->owner, binding->name,
                                                       trigger_flags);
            sc->activation_count++;
            snprintf(sc->last_activation, sizeof(sc->last_activation), "%s", binding->name);
            outcome = OUTCOME_ACTIVATED;
        } else {
            outcome = OUTCOME_NO_OWNER;
        }
        break;
    case WOODLAND_SC_ACTION_WORKSPACE: {
        char name[16];
        snprintf(name, sizeof(name), "%u", action);
        struct woodland_workspace *target = woodland_workspace_by_name(server, name);
        if (target) {
            woodland_workspace_switch(server, target);
            outcome = OUTCOME_SWITCHED;
        } else {
            outcome = OUTCOME_NO_WORKSPACE;
        }
        break;
    }
    case WOODLAND_SC_ACTION_WORKSPACE_PREV:
    case WOODLAND_SC_ACTION_WORKSPACE_NEXT: {
        bool next = action == TREELAND_SHORTCUT_MANAGER_V3_ACTION_NEXT_WORKSPACE;
        struct woodland_workspace *target =
            woodland_protocol_action_adjacent_workspace(server, next);
        if (target) {
            woodland_workspace_switch(server, target);
            outcome = OUTCOME_SWITCHED;
        } else {
            outcome = OUTCOME_NO_WORKSPACE;
        }
        break;
    }
    case WOODLAND_SC_ACTION_VIEW: {
        struct woodland_view *view = shortcut_active_view(server);
        if (!view) {
            outcome = OUTCOME_NO_VIEW;
            break;
        }
        switch (action) {
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_MAXIMIZE:
            woodland_view_set_maximized(view, true);
            break;
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_CANCEL_MAXIMIZE:
            woodland_view_set_maximized(view, false);
            break;
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_MINIMIZE:
            woodland_view_set_minimized(view, true);
            break;
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_MOVE_WINDOW:
            woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
            break;
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_RESIZE_WINDOW:
            woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE,
                                            WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
            break;
        case TREELAND_SHORTCUT_MANAGER_V3_ACTION_CLOSE_WINDOW:
            woodland_view_close(view);
            break;
        default:
            break;
        }
        outcome = OUTCOME_VIEW_ACTION;
        break;
    }
    case WOODLAND_SC_ACTION_MOVE_WORKSPACE: {
        struct woodland_view *view = shortcut_active_view(server);
        bool next = action ==
                    TREELAND_SHORTCUT_MANAGER_V3_ACTION_MOVE_WINDOW_TO_NEXT_WORKSPACE;
        struct woodland_workspace *target =
            woodland_protocol_action_adjacent_workspace(server, next);
        if (!view) {
            outcome = OUTCOME_NO_VIEW;
        } else if (!target) {
            outcome = OUTCOME_NO_WORKSPACE;
        } else {
            shortcut_move_view_to_workspace(server, view, target);
            outcome = OUTCOME_MOVED_WORKSPACE;
        }
        break;
    }
    case WOODLAND_SC_ACTION_TILE: {
        struct woodland_view *view = shortcut_active_view(server);
        if (!view) {
            outcome = OUTCOME_NO_VIEW;
        } else if (woodland_view_snap_to(view, shortcut_tile_edges(action))) {
            outcome = OUTCOME_TILED;
        } else {
            /* 已最大化/全屏或没有可用工作区时贴不了，属于"不能执行" */
            outcome = OUTCOME_NO_VIEW;
        }
        break;
    }
    case WOODLAND_SC_ACTION_UNSUPPORTED:
        outcome = OUTCOME_UNSUPPORTED;
        break;
    case WOODLAND_SC_ACTION_INVALID:
    default:
        outcome = OUTCOME_INVALID;
        break;
    }

    snprintf(detail, sizeof(detail), "activate name=%s kind=%s action=%u flags=%u",
             binding->name, kind_names[binding->kind], action, trigger_flags);
    shortcut_record(sc, outcome, action, "%s", detail);
    return outcome;
}

/* ==================== bind / manager 请求 ==================== */

static bool client_allowed(struct wl_client *client)
{
    pid_t pid = 0;
    uid_t uid = (uid_t)-1;
    gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    (void)pid;
    (void)gid;
    if (uid == (uid_t)-1) {
        /* 部分 fd 取不到 SO_PEERCRED：in-process 客户端也走这条，放行 */
        wlr_log(WLR_INFO, "shortcut-manager: no peer credentials, allowing bind");
        return true;
    }
    return uid == getuid();
}

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    if (!manager) {
        return;
    }
    struct woodland_proto_shortcut *sc = manager->sc;
    wl_list_remove(&manager->link);
    if (sc->owner == resource) {
        sc->owner = NULL;
        woodland_trace_add("shortcut", 0, "owner released (manager destroyed)");
    }
    free(manager);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    /* 协议：destroy 隐式释放独占，但**不删除**已注册的绑定 */
    wl_resource_destroy(resource);
}

static void manager_handle_acquire(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc) {
        return;
    }
    if (sc->owner) {
        shortcut_record(sc, OUTCOME_OCCUPIED, UINT32_MAX, "acquire denied");
        wl_resource_post_error(resource, TREELAND_SHORTCUT_MANAGER_V3_ERROR_OCCUPIED,
                                "the shortcut manager is already held by another client");
        return;
    }
    sc->owner = resource;
    shortcut_record(sc, OUTCOME_BOUND, UINT32_MAX, "acquire");
}

static bool require_owner(struct woodland_proto_shortcut *sc, struct wl_resource *resource)
{
    if (sc->owner == resource) {
        return true;
    }
    shortcut_record(sc, OUTCOME_NOT_ACQUIRED, UINT32_MAX, "request without acquire");
    wl_resource_post_error(resource, TREELAND_SHORTCUT_MANAGER_V3_ERROR_NOT_ACQUIRED,
                            "the client has not acquired the shortcut manager");
    return false;
}

/* 返回 bind_error 码（0 表示成功）；成功时把新绑定挂进 sc->bindings */
static uint32_t shortcut_bind_common(struct woodland_proto_shortcut *sc,
                                     const char *name, uint32_t action,
                                     enum shortcut_kind kind, uint32_t finger,
                                     uint32_t direction, uint32_t flags,
                                     const struct woodland_shortcut_key *key,
                                     const char *key_text, bool *upserted)
{
    if (upserted) {
        *upserted = false;
    }
    if (!name || !*name) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
    }
    if (action > SC_ACTION_MAX) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
    }
    if (find_binding_by_name(sc, name)) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_NAME_CONFLICT;
    }
    if (kind != SC_KIND_KEY && finger == 0) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
    }
    if (kind == SC_KIND_SWIPE &&
        (direction < TREELAND_SHORTCUT_MANAGER_V3_DIRECTION_DOWN ||
         direction > TREELAND_SHORTCUT_MANAGER_V3_DIRECTION_RIGHT)) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
    }

    struct shortcut_binding *binding;
    /* 重复 trigger-action 组合：key 是 upsert，手势是 duplicate_binding */
    wl_list_for_each(binding, &sc->bindings, link)
    {
        if (binding->action != action || binding->kind != kind) {
            continue;
        }
        bool same = false;
        if (kind == SC_KIND_KEY) {
            same = key && woodland_shortcut_key_matches(&binding->key,
                                                         key->mods, key->sym);
        } else if (kind == SC_KIND_SWIPE) {
            same = binding->finger == finger && binding->direction == direction;
        } else {
            same = binding->finger == finger;
        }
        if (!same) {
            continue;
        }
        if (kind == SC_KIND_KEY) {
            binding->flags = flags;
            if (upserted) {
                *upserted = true;
            }
            shortcut_record(sc, OUTCOME_UPSERT, action,
                            "bind_key name=%s key=%s upsert flags=%u on %s",
                            name, key->text, flags, binding->name);
            return 0;
        }
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_DUPLICATE_BINDING;
    }

    binding = calloc(1, sizeof(*binding));
    if (!binding) {
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INTERNAL_ERROR;
    }
    wl_list_init(&binding->link);
    binding->name = strdup(name);
    if (key_text) {
        binding->key_text = strdup(key_text);
    }
    if (!binding->name || (key_text && !binding->key_text)) {
        shortcut_binding_free(binding);
        return TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INTERNAL_ERROR;
    }
    binding->kind = kind;
    binding->flags = flags;
    binding->finger = finger;
    binding->direction = direction;
    binding->action = action;
    if (key) {
        binding->key = *key;
    }
    wl_list_insert(&sc->bindings, &binding->link);
    return 0;
}

static void manager_handle_bind_key(struct wl_client *client, struct wl_resource *resource,
                                    const char *name, const char *key, uint32_t flags,
                                    uint32_t action)
{
    (void)client;
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc || !require_owner(sc, resource)) {
        return;
    }
    sc->bind_count++;

    struct woodland_shortcut_key parsed;
    enum woodland_shortcut_parse_result pr =
        woodland_shortcut_key_parse(key, &parsed);
    uint32_t err = 0;
    if (pr != WOODLAND_SC_PARSE_OK) {
        err = TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
        shortcut_record(sc, OUTCOME_INVALID_ARGUMENT, action,
                        "bind_key name=%s key=%s parse=%s",
                        name ? name : "", key ? key : "", woodland_shortcut_parse_name(pr));
    } else if (flags == 0 || (flags & ~SC_FLAG_MASK) != 0) {
        /* 三种 trigger 位全不设的绑定永远不会命中，按 out-of-range 拒掉 */
        err = TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
        shortcut_record(sc, OUTCOME_INVALID_ARGUMENT, action,
                        "bind_key name=%s flags=%u out of range", name, flags);
    } else if (!woodland_shortcut_key_is_valid(parsed.mods, parsed.sym)) {
        err = TREELAND_SHORTCUT_MANAGER_V3_BIND_ERROR_INVALID_ARGUMENT;
        shortcut_record(sc, OUTCOME_INVALID_ARGUMENT, action,
                        "bind_key name=%s key=%s not a valid shortcut", name, parsed.text);
    } else {
        bool upserted = false;
        err = shortcut_bind_common(sc, name, action, SC_KIND_KEY, 0, 0, flags,
                                   &parsed, key, &upserted);
        /* 别用 sc->last_outcome 判 upsert：上一条请求可能也是 upsert */
        if (err == 0 && !upserted) {
            shortcut_record(sc, OUTCOME_BOUND, action,
                            "bind_key name=%s key=%s flags=%u", name, parsed.text, flags);
        }
    }
    if (err) {
        shortcut_record(sc, bind_error_outcome(err), action,
                        "bind rejected name=%s error=%u", name ? name : "", err);
        shortcut_send_bind_failure(sc, name ? name : "", err);
    }
}

static void manager_handle_bind_swipe(struct wl_client *client, struct wl_resource *resource,
                                      const char *name, uint32_t finger,
                                      uint32_t direction, uint32_t action)
{
    (void)client;
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc || !require_owner(sc, resource)) {
        return;
    }
    sc->bind_count++;
    uint32_t err = shortcut_bind_common(sc, name, action, SC_KIND_SWIPE, finger,
                                         direction, 0, NULL, NULL, NULL);
    if (err == 0) {
        /* 注册成功但没有手势识别状态机：明写"登记了但不会触发" */
        shortcut_record(sc, OUTCOME_GESTURE_UNWIRED, action,
                        "bind_swipe_gesture name=%s finger=%u direction=%u",
                        name, finger, direction);
    } else {
        shortcut_record(sc, bind_error_outcome(err), action,
                        "bind rejected name=%s error=%u", name ? name : "", err);
        shortcut_send_bind_failure(sc, name ? name : "", err);
    }
}

static void manager_handle_bind_hold(struct wl_client *client, struct wl_resource *resource,
                                      const char *name, uint32_t finger, uint32_t action)
{
    (void)client;
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc || !require_owner(sc, resource)) {
        return;
    }
    sc->bind_count++;
    uint32_t err = shortcut_bind_common(sc, name, action, SC_KIND_HOLD, finger,
                                         0, 0, NULL, NULL, NULL);
    if (err == 0) {
        shortcut_record(sc, OUTCOME_GESTURE_UNWIRED, action,
                        "bind_hold_gesture name=%s finger=%u", name, finger);
    } else {
        shortcut_record(sc, bind_error_outcome(err), action,
                        "bind rejected name=%s error=%u", name ? name : "", err);
        shortcut_send_bind_failure(sc, name ? name : "", err);
    }
}

static void manager_handle_unbind(struct wl_client *client, struct wl_resource *resource,
                                   const char *name)
{
    (void)client;
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc || !require_owner(sc, resource)) {
        return;
    }
    sc->unbind_count++;
    struct shortcut_binding *binding = name ? find_binding_by_name(sc, name) : NULL;
    if (binding) {
        shortcut_binding_free(binding);
        shortcut_record(sc, OUTCOME_UNBOUND, UINT32_MAX, "unbind name=%s", name);
    } else {
        /* 协议：找不到名字就静默忽略，不发 bind_failure */
        shortcut_record(sc, OUTCOME_NONE, UINT32_MAX, "unbind name=%s not found",
                        name ? name : "");
    }
}

/* ==================== capture 对象 ==================== */

static void capture_resource_destroy(struct wl_resource *resource)
{
    struct shortcut_capture *capture = wl_resource_get_user_data(resource);
    if (!capture) {
        return;
    }
    struct woodland_proto_shortcut *sc = capture->sc;
    wl_list_remove(&capture->link);
    if (sc->capture == capture) {
        sc->capture = NULL;
        woodland_trace_add("shortcut", 0, "capture cancelled by client");
    }
    free(capture);
}

static void capture_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct treeland_shortcut_capture_v3_interface capture_impl = {
    .destroy = capture_handle_destroy,
};

static void capture_terminal(struct woodland_proto_shortcut *sc,
                             struct shortcut_capture *capture,
                             enum shortcut_outcome outcome,
                             uint32_t failed_reason, const char *key)
{
    if (!capture || capture->terminal) {
        return;
    }
    capture->terminal = true;
    if (sc->capture == capture) {
        sc->capture = NULL;
    }
    if (outcome == OUTCOME_CAPTURED) {
        treeland_shortcut_capture_v3_send_captured(capture->resource, key);
        shortcut_record(sc, outcome, UINT32_MAX, "capture captured key=%s", key);
    } else {
        treeland_shortcut_capture_v3_send_failed(capture->resource, failed_reason);
        shortcut_record(sc, outcome, UINT32_MAX, "capture failed reason=%u",
                        failed_reason);
    }
}

static void manager_handle_capture_next_shortcut(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 struct wl_resource *surface_resource,
                                                 struct wl_resource *seat_resource,
                                                 uint32_t id)
{
    struct shortcut_manager *manager = wl_resource_get_user_data(resource);
    struct woodland_proto_shortcut *sc = manager ? manager->sc : NULL;
    if (!sc) {
        return;
    }
    /* 先校验 surface 归属：不属于本客户端就是协议错误，不创建 capture 对象 */
    if (!surface_resource || wl_resource_get_client(surface_resource) != client) {
        shortcut_record(sc, OUTCOME_INVALID_ARGUMENT, UINT32_MAX,
                        "capture_next_shortcut invalid_surface");
        wl_resource_post_error(resource,
                                TREELAND_SHORTCUT_MANAGER_V3_ERROR_INVALID_SURFACE,
                                "the surface is not owned by the requesting client");
        return;
    }

    struct wl_resource *capture_resource = wl_resource_create(
        client, &treeland_shortcut_capture_v3_interface,
        wl_resource_get_version(resource), id);
    if (!capture_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct shortcut_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        wl_resource_destroy(capture_resource);
        wl_client_post_no_memory(client);
        return;
    }
    capture->sc = sc;
    capture->resource = capture_resource;
    wl_resource_set_implementation(capture_resource, &capture_impl, capture,
                                    capture_resource_destroy);
    wl_list_insert(&sc->captures, &capture->link);
    sc->capture_count++;

    /* busy：已有 pending capture，或还在吞上一次 captured 的 key_release */
    if (sc->capture || sc->drain_keycode != 0) {
        capture_terminal(sc, capture, OUTCOME_BUSY,
                         TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_BUSY, NULL);
        return;
    }

    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    struct wlr_seat *seat = sc->server->seat;
    if (seat_resource) {
        struct wlr_seat_client *seat_client = wlr_seat_client_from_resource(seat_resource);
        seat = seat_client ? seat_client->seat : NULL;
    }
    /*
     * woodland 只有 seat0：其它 seat 的 keyboard focus 不在我们的事件路径上，
     * 收了也拦截不到输入，按 not_active 明确拒绝而不是留下一个永不结束的
     * capture。surface 必须有该 seat 的键盘焦点。
     */
    if (!surface || !seat || seat != sc->server->seat ||
        seat->keyboard_state.focused_surface != surface) {
        capture_terminal(sc, capture, OUTCOME_NOT_ACTIVE,
                         TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_NOT_ACTIVE, NULL);
        return;
    }

    capture->surface = surface;
    capture->seat = seat;
    sc->capture = capture;
    shortcut_record(sc, OUTCOME_NONE, UINT32_MAX, "capture pending");
}

static const struct treeland_shortcut_manager_v3_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .acquire = manager_handle_acquire,
    .bind_key = manager_handle_bind_key,
    .bind_swipe_gesture = manager_handle_bind_swipe,
    .bind_hold_gesture = manager_handle_bind_hold,
    .unbind = manager_handle_unbind,
    .capture_next_shortcut = manager_handle_capture_next_shortcut,
};

static void shortcut_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_shortcut *sc = data;
    if (!client_allowed(client)) {
        wl_client_post_implementation_error(client,
            "treeland_shortcut_manager_v3: bind rejected for a different user");
        return;
    }
    if (version > SC_VERSION) {
        version = SC_VERSION;
    }
    struct wl_resource *resource = wl_resource_create(
        client, &treeland_shortcut_manager_v3_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct shortcut_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    manager->sc = sc;
    manager->resource = resource;
    wl_resource_set_implementation(resource, &manager_impl, manager,
                                    manager_resource_destroy);
    wl_list_insert(&sc->managers, &manager->link);
}

/* ==================== 键盘 / 指针事件转发 ==================== */

static uint32_t wlr_mods_to_shortcut_mods(uint32_t wlr_mods)
{
    uint32_t mods = 0;
    if (wlr_mods & WLR_MODIFIER_CTRL) {
        mods |= WOODLAND_SC_MOD_CTRL;
    }
    if (wlr_mods & WLR_MODIFIER_ALT) {
        mods |= WOODLAND_SC_MOD_ALT;
    }
    if (wlr_mods & WLR_MODIFIER_SHIFT) {
        mods |= WOODLAND_SC_MOD_SHIFT;
    }
    if (wlr_mods & WLR_MODIFIER_LOGO) {
        mods |= WOODLAND_SC_MOD_META;
    }
    return mods;
}

static bool shortcut_capture_key(struct woodland_proto_shortcut *sc,
                                 struct shortcut_capture *capture,
                                 uint32_t mods, uint32_t sym,
                                 const struct wlr_keyboard_key_event *event,
                                 bool repeat)
{
    struct wlr_seat *seat = sc->server->seat;
    bool press = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

    /* 焦点丢了：协议要求发 aborted，并让当前事件继续发给新焦点 */
    if (seat->keyboard_state.focused_surface != capture->surface) {
        capture_terminal(sc, capture, OUTCOME_ABORTED,
                         TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_ABORTED, NULL);
        return false;
    }
    if (repeat) {
        return true; /* auto-repeat 静默消费 */
    }

    uint32_t mod_bit = 0;
    if (sym_is_modifier(sym, &mod_bit) && press) {
        capture->held_mods |= mod_bit;
        return true; /* 纯修饰键按下静默消费 */
    }
    if (sym_is_modifier(sym, &mod_bit)) {
        capture->held_mods &= ~mod_bit;
        /* 只按了 Meta 再松开：协议规定报 captured("Meta") */
        if (mod_bit == WOODLAND_SC_MOD_META && capture->held_mods == 0) {
            capture_terminal(sc, capture, OUTCOME_CAPTURED, 0, "Meta");
        } else {
            capture_terminal(sc, capture, OUTCOME_INTERRUPTED,
                             TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_INTERRUPTED,
                             NULL);
        }
        return true;
    }
    if (!press) {
        return true; /* 非修饰键的 release 属于被吞掉的那一次输入 */
    }

    if (woodland_shortcut_key_is_valid(mods, sym)) {
        char text[64];
        if (!woodland_shortcut_key_format(mods, sym, text, sizeof(text))) {
            capture_terminal(sc, capture, OUTCOME_INTERRUPTED,
                             TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_INTERRUPTED,
                             NULL);
        } else {
            capture_terminal(sc, capture, OUTCOME_CAPTURED, 0, text);
            /* 协议：吃掉最后一次 captured 键的 key_release，期间新 capture 报 busy */
            sc->drain_keycode = event->keycode;
        }
    } else {
        capture_terminal(sc, capture, OUTCOME_INTERRUPTED,
                         TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_INTERRUPTED, NULL);
    }
    return true;
}

bool woodland_proto_shortcut_key(struct woodland_proto *proto,
                                 struct wlr_keyboard *keyboard,
                                 const struct wlr_keyboard_key_event *event)
{
    struct woodland_proto_shortcut *sc = proto ? proto->shortcut : NULL;
    if (!sc || !keyboard || !event) {
        return false;
    }

    bool press = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    bool repeat = press && sc->last_pressed && sc->last_keycode == event->keycode;
    sc->last_pressed = press;
    sc->last_keycode = event->keycode;

    /*
     * captured 之后那一次 key 的收尾要吞掉，不能再去触发已注册的绑定。
     * 协议只说 consume the key_release，但同键的 auto-repeat 也在 drain
     * 窗口内：它发生在 release 之前，不一起吞掉就会穿透去匹配绑定（若
     * 恰好有个带 REPEAT flag 的同组合绑定就会误触发）。
     */
    if (sc->drain_keycode == event->keycode) {
        if (!press) {
            sc->drain_keycode = 0;
        }
        return true;
    }

    uint32_t mods = wlr_mods_to_shortcut_mods(wlr_keyboard_get_modifiers(keyboard));
    uint32_t sym = XKB_KEY_NoSymbol;
    if (keyboard->xkb_state) {
        const xkb_keysym_t *syms = NULL;
        int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, event->keycode + 8, &syms);
        if (nsyms > 0 && syms) {
            sym = (uint32_t)syms[0];
        }
    }

    /* capture 的优先级高于已注册绑定 */
    if (sc->capture && !sc->capture->terminal) {
        return shortcut_capture_key(sc, sc->capture, mods, sym, event, repeat);
    }

    uint32_t trigger_flags = press ? SC_FLAG_KEY_PRESS : SC_FLAG_KEY_RELEASE;
    if (repeat) {
        trigger_flags |= SC_FLAG_REPEAT;
    }
    struct shortcut_binding *binding;
    wl_list_for_each(binding, &sc->bindings, link)
    {
        if (binding->kind != SC_KIND_KEY) {
            continue; /* 手势绑定没有识别状态机，不会由键盘事件触发 */
        }
        if (!woodland_shortcut_key_matches(&binding->key, mods, sym)) {
            continue;
        }
        if (repeat && !(binding->flags & SC_FLAG_REPEAT)) {
            continue;
        }
        if (!(binding->flags & trigger_flags & SC_FLAG_MASK)) {
            continue;
        }
        shortcut_run_action(sc, binding, trigger_flags);
        return true;
    }
    return false;
}

bool woodland_proto_shortcut_pointer(struct woodland_proto *proto)
{
    struct woodland_proto_shortcut *sc = proto ? proto->shortcut : NULL;
    if (!sc || !sc->capture || sc->capture->terminal) {
        return false;
    }
    /* 指针 button / wheel 终止捕获，事件本身被消费 */
    capture_terminal(sc, sc->capture, OUTCOME_INTERRUPTED,
                     TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_INTERRUPTED, NULL);
    return true;
}

static void shortcut_handle_focus_change(struct wl_listener *listener, void *data)
{
    struct woodland_proto_shortcut *sc =
        wl_container_of(listener, sc, focus_change);
    (void)data;
    /*
     * 焦点一旦变化，上一次 captured 遗留的 drain 窗口就不再可靠：release
     * 可能永远不来（客户端断连 / 设备拔掉），留着会让之后所有 capture
     * 永久报 busy。这里和 capture 的 aborted 一起清理。
     */
    if (sc->drain_keycode != 0) {
        woodland_trace_add("shortcut", 0, "drain cleared on focus change");
        sc->drain_keycode = 0;
    }
    if (!sc->capture || sc->capture->terminal) {
        return;
    }
    if (sc->server->seat->keyboard_state.focused_surface != sc->capture->surface) {
        capture_terminal(sc, sc->capture, OUTCOME_ABORTED,
                         TREELAND_SHORTCUT_CAPTURE_V3_FAILED_REASON_ABORTED, NULL);
    }
}

/* ==================== 编排层入口 / 只读状态 ==================== */

bool woodland_proto_shortcut_init(struct woodland_proto *proto)
{
    struct woodland_proto_shortcut *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        wlr_log(WLR_ERROR, "cannot allocate shortcut-manager protocol state");
        return false;
    }
    sc->server = proto->server;
    wl_list_init(&sc->managers);
    wl_list_init(&sc->captures);
    wl_list_init(&sc->bindings);
    sc->last_outcome = OUTCOME_NONE;
    /* 先挂上去：init 中途失败时 finish 也能回收 */
    proto->shortcut = sc;
    /* 键盘焦点一变就要判 capture 的 aborted，挂 seat 而不是 focus.c 的每个出口 */
    woodland_listener_connect(&sc->focus_change,
                               &sc->server->seat->keyboard_state.events.focus_change,
                               shortcut_handle_focus_change, "shortcut.focus_change");
    /* 指针事件从 shell/cursor.c 进来，用 server 上的钩子跨层，不反向链接 */
    sc->server->shortcut_pointer_hook = woodland_protocol_shortcuts_pointer;

    sc->global = wl_global_create(sc->server->display,
                                  &treeland_shortcut_manager_v3_interface,
                                  SC_VERSION, sc, shortcut_bind);
    if (!sc->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-shortcut-manager-v3 global");
        return false;
    }
    wlr_log(WLR_INFO, "treeland-shortcut-manager-v3: key bindings wired, "
                      "gesture bindings recorded but not triggered");
    return true;
}

void woodland_proto_shortcut_finish(struct woodland_proto *proto)
{
    struct woodland_proto_shortcut *sc = proto->shortcut;
    if (!sc) {
        return;
    }
    if (sc->global) {
        wl_global_destroy(sc->global);
        sc->global = NULL;
    }
    sc->server->shortcut_pointer_hook = NULL;
    wl_list_remove(&sc->focus_change.link);
    /*
     * global 销毁不会带走已 bind 的 resource。先销毁所有 capture / manager
     * resource（destructor 摘链并清 owner / pending 指针），再释放 binding
     * 与族状态——否则 live resource 的 user_data 会变成悬空指针。
     */
    struct shortcut_capture *capture, *capture_tmp;
    wl_list_for_each_safe(capture, capture_tmp, &sc->captures, link)
    {
        wl_resource_destroy(capture->resource);
    }
    struct shortcut_manager *manager, *manager_tmp;
    wl_list_for_each_safe(manager, manager_tmp, &sc->managers, link)
    {
        wl_resource_destroy(manager->resource);
    }
    struct shortcut_binding *binding, *binding_tmp;
    wl_list_for_each_safe(binding, binding_tmp, &sc->bindings, link)
    {
        shortcut_binding_free(binding);
    }
    free(sc);
    proto->shortcut = NULL;
}

void woodland_protocol_shortcut_status(struct woodland_server *server,
                                       struct woodland_json *out)
{
    struct woodland_proto_shortcut *sc =
        (server && server->proto) ? server->proto->shortcut : NULL;
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "available", sc != NULL);
    woodland_json_obj_bool(&obj, "acquired", sc && sc->owner != NULL);
    woodland_json_obj_int(&obj, "manager_count",
                          sc ? (long long)wl_list_length(&sc->managers) : 0);
    woodland_json_obj_int(&obj, "binding_count",
                          sc ? (long long)wl_list_length(&sc->bindings) : 0);
    woodland_json_obj_int(&obj, "capture_count", sc ? sc->capture_count : 0);
    woodland_json_obj_bool(&obj, "capture_pending", sc && sc->capture != NULL);
    woodland_json_obj_int(&obj, "activation_count", sc ? sc->activation_count : 0);
    woodland_json_obj_int(&obj, "last_action", sc ? sc->last_action : 0);
    woodland_json_obj_str(&obj, "last_outcome", sc ? outcome_name(sc->last_outcome) : NULL);
    woodland_json_obj_str(&obj, "last_activation",
                          sc && sc->last_activation[0] ? sc->last_activation : NULL);

    woodland_json_obj_key(&obj, "bindings");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    if (sc) {
        struct shortcut_binding *binding;
        wl_list_for_each(binding, &sc->bindings, link)
        {
            woodland_json_arr_sep(&arr);
            struct woodland_json_obj b;
            woodland_json_obj_open(&b, out);
            woodland_json_obj_str(&b, "name", binding->name);
            woodland_json_obj_str(&b, "kind", kind_names[binding->kind]);
            woodland_json_obj_str(&b, "key", binding->key_text);
            woodland_json_obj_int(&b, "flags", binding->flags);
            woodland_json_obj_int(&b, "finger", binding->finger);
            woodland_json_obj_int(&b, "direction", binding->direction);
            woodland_json_obj_int(&b, "action", binding->action);
            woodland_json_obj_close(&b);
        }
    }
    woodland_json_arr_close(&arr);
    woodland_json_obj_close(&obj);
}
