// SPDX-License-Identifier: GPL-2.0-only
/*
 * woodland-shortcutd 的配置解析与默认键位表。见 include/shortcutd_config.h。
 *
 * 为什么复用 src/config/toml.c 而不是自己写行解析：合成器的配置文件已经是
 * TOML，用户不该为了快捷键再学第二种格式；那份解析器只依赖 libc，直接
 * 编进 client 的静态库即可，不用把 wlroots 拖进来。
 *
 * 为什么不在这里校验键位合法性：QKeySequence PortableText 的解析 + validity
 * 规则在合成器侧（src/protocol/shortcut.c）只有一份，客户端再实现一遍就是
 * 两套事实源。这里只保证"形状对"（key 非空、action 已知、command 与 action
 * 二选一），语义交给 bind_failure 回答。
 */
#include "shortcutd_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "toml.h"

/* ==================== 小工具 ==================== */

static bool streq_icase(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static char *xstrdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* ==================== action 名表 ==================== */

/* 名字取自协议 XML 的 entry name，不做别名：脚本/wctl 输出能一一对上 */
static const char *const action_names[WSD_ACTION_MAX + 1] = {
    [WSD_ACTION_NOTIFY] = "notify",
    [WSD_ACTION_WORKSPACE_1] = "workspace_1",
    [WSD_ACTION_WORKSPACE_2] = "workspace_2",
    [WSD_ACTION_WORKSPACE_3] = "workspace_3",
    [WSD_ACTION_WORKSPACE_4] = "workspace_4",
    [WSD_ACTION_WORKSPACE_5] = "workspace_5",
    [WSD_ACTION_WORKSPACE_6] = "workspace_6",
    [WSD_ACTION_WORKSPACE_7] = "workspace_7",
    [WSD_ACTION_WORKSPACE_8] = "workspace_8",
    [WSD_ACTION_WORKSPACE_9] = "workspace_9",
    [WSD_ACTION_WORKSPACE_10] = "workspace_10",
    [WSD_ACTION_WORKSPACE_11] = "workspace_11",
    [WSD_ACTION_WORKSPACE_12] = "workspace_12",
    [WSD_ACTION_PREV_WORKSPACE] = "prev_workspace",
    [WSD_ACTION_NEXT_WORKSPACE] = "next_workspace",
    [WSD_ACTION_MAXIMIZE] = "maximize",
    [WSD_ACTION_CANCEL_MAXIMIZE] = "cancel_maximize",
    [WSD_ACTION_MINIMIZE] = "minimize",
    [WSD_ACTION_MOVE_WINDOW] = "move_window",
    [WSD_ACTION_RESIZE_WINDOW] = "resize_window",
    [WSD_ACTION_CLOSE_WINDOW] = "close_window",
    [WSD_ACTION_SHOW_WINDOW_MENU] = "show_window_menu",
    [WSD_ACTION_MOVE_WINDOW_TO_PREV_WORKSPACE] = "move_window_to_prev_workspace",
    [WSD_ACTION_MOVE_WINDOW_TO_NEXT_WORKSPACE] = "move_window_to_next_workspace",
    [WSD_ACTION_SHOW_DESKTOP] = "show_desktop",
    [WSD_ACTION_OPEN_MULTITASK_VIEW] = "open_multitask_view",
    [WSD_ACTION_CLOSE_MULTITASK_VIEW] = "close_multitask_view",
    [WSD_ACTION_TOGGLE_MULTITASK_VIEW] = "toggle_multitask_view",
    [WSD_ACTION_TASKSWITCH_NEXT] = "taskswitch_next",
    [WSD_ACTION_TASKSWITCH_PREV] = "taskswitch_prev",
    [WSD_ACTION_TASKSWITCH_SAMEAPP_NEXT] = "taskswitch_sameapp_next",
    [WSD_ACTION_TASKSWITCH_SAMEAPP_PREV] = "taskswitch_sameapp_prev",
    [WSD_ACTION_TILE_LEFT] = "tile_left",
    [WSD_ACTION_TILE_RIGHT] = "tile_right",
    [WSD_ACTION_TILE_TOP] = "tile_top",
    [WSD_ACTION_TILE_BOTTOM] = "tile_bottom",
    [WSD_ACTION_TILE_TOP_LEFT] = "tile_top_left",
    [WSD_ACTION_TILE_TOP_RIGHT] = "tile_top_right",
    [WSD_ACTION_TILE_BOTTOM_LEFT] = "tile_bottom_left",
    [WSD_ACTION_TILE_BOTTOM_RIGHT] = "tile_bottom_right",
    [WSD_ACTION_ZOOM_IN] = "zoom_in",
    [WSD_ACTION_ZOOM_OUT] = "zoom_out",
    [WSD_ACTION_ZOOM_RESET] = "zoom_reset",
    [WSD_ACTION_TOGGLE_FPS_DISPLAY] = "toggle_fps_display",
    [WSD_ACTION_LOCKSCREEN] = "lockscreen",
    [WSD_ACTION_SHUTDOWN_MENU] = "shutdown_menu",
};

const char *wsd_action_name(uint32_t action)
{
    if (action > WSD_ACTION_MAX) {
        return NULL;
    }
    return action_names[action];
}

bool wsd_action_from_name(const char *name, uint32_t *action)
{
    if (!name || !*name) {
        return false;
    }
    for (uint32_t i = 0; i <= WSD_ACTION_MAX; i++) {
        if (action_names[i] && streq_icase(name, action_names[i])) {
            if (action) {
                *action = i;
            }
            return true;
        }
    }
    return false;
}

/* ==================== flags ==================== */

bool wsd_flags_from_string(const char *text, uint32_t *flags)
{
    if (!text || !*text || !flags) {
        return false;
    }
    char buf[128];
    if (strlen(text) >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, text, strlen(text) + 1);

    uint32_t out = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, "|,+\t ", &saveptr); tok; tok = strtok_r(NULL, "|,+\t ", &saveptr)) {
        if (streq_icase(tok, "press") || streq_icase(tok, "key_press")) {
            out |= WSD_FLAG_PRESS;
        } else if (streq_icase(tok, "release") || streq_icase(tok, "key_release")) {
            out |= WSD_FLAG_RELEASE;
        } else if (streq_icase(tok, "repeat")) {
            out |= WSD_FLAG_REPEAT;
        } else {
            return false;
        }
    }
    if (out == 0) {
        return false;
    }
    *flags = out;
    return true;
}

/* ==================== PATH 探测 ==================== */

bool wsd_command_available(const char *command)
{
    if (!command) {
        return false;
    }
    while (*command && isspace((unsigned char)*command)) {
        command++;
    }
    char bin[PATH_MAX];
    size_t j = 0;
    while (command[j] && !isspace((unsigned char)command[j]) && j + 1 < sizeof(bin)) {
        bin[j] = command[j];
        j++;
    }
    bin[j] = '\0';
    if (j == 0) {
        return false;
    }
    /* 带斜杠的命令按路径直接判，和 execvp 的语义一致 */
    if (strchr(bin, '/')) {
        return access(bin, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char candidate[PATH_MAX];
    const char *p = path;
    while (true) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        int n;
        if (len == 0) {
            /* POSIX：PATH 里的空项表示当前目录 */
            n = snprintf(candidate, sizeof(candidate), "./%s", bin);
        } else {
            n = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, p, bin);
        }
        if (n > 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0) {
            return true;
        }
        if (!sep) {
            break;
        }
        p = sep + 1;
    }
    return false;
}

/* ==================== 配置容器 ==================== */

void wsd_config_init(struct wsd_config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
}

void wsd_config_clear(struct wsd_config *cfg)
{
    if (!cfg) {
        return;
    }
    for (size_t i = 0; i < cfg->count; i++) {
        free(cfg->bindings[i].name);
        free(cfg->bindings[i].key);
        free(cfg->bindings[i].command);
    }
    free(cfg->bindings);
    bool enabled = cfg->enabled;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = enabled;
}

static bool name_taken(const struct wsd_config *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->bindings[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * 名字默认就是 key 文本（wctl get_tree 里一眼能认出是哪条键位）。
 * 同一组合写两遍（比如一次 command、一次 action）时追加 #N 保证唯一——
 * 合成器对重名报 name_conflict，与其让第二条静默失败不如自己先区分开。
 */
static char *make_unique_name(const struct wsd_config *cfg, const char *key)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", key);
    for (int n = 2; name_taken(cfg, buf) && n < 1000; n++) {
        snprintf(buf, sizeof(buf), "%s#%d", key, n);
    }
    return xstrdup(buf);
}

bool wsd_config_add(struct wsd_config *cfg, const char *key, uint32_t action,
                    uint32_t flags, const char *command)
{
    if (!cfg || !key || !*key) {
        return false;
    }
    if (action > WSD_ACTION_MAX) {
        return false;
    }
    if (flags == 0 || (flags & ~WSD_FLAG_MASK) != 0) {
        return false;
    }
    if (command && action != WSD_ACTION_NOTIFY) {
        return false;
    }

    if (cfg->count == cfg->capacity) {
        size_t capacity = cfg->capacity ? cfg->capacity * 2 : 16;
        struct wsd_binding *grown =
            realloc(cfg->bindings, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        cfg->bindings = grown;
        cfg->capacity = capacity;
    }

    struct wsd_binding *binding = &cfg->bindings[cfg->count];
    memset(binding, 0, sizeof(*binding));
    binding->name = make_unique_name(cfg, key);
    binding->key = xstrdup(key);
    binding->command = xstrdup(command);
    if (!binding->name || !binding->key || (command && !binding->command)) {
        free(binding->name);
        free(binding->key);
        free(binding->command);
        memset(binding, 0, sizeof(*binding));
        return false;
    }
    binding->flags = flags;
    binding->action = action;
    cfg->count++;
    return true;
}

/* ==================== 内置默认键位 ==================== */

/*
 * 每条默认键位给一串候选命令，按顺序挑第一个在 PATH 里存在的。
 * 目的很实际：woodland 自己只认 Alt+Tab / Alt+F1..F9，非 DDE 环境下连
 * "Super+Return 开终端"都没人接；这里把最常见的几条补上，同时不硬绑定
 * 某个终端或音量后端。
 */
static const char *const default_terminal_cmds[] = {
    "x-terminal-emulator", "foot", "alacritty", "kitty", "wezterm",
    "qterminal", "xfce4-terminal", "konsole", "gnome-terminal", "xterm",
    NULL,
};

static const char *const default_filemanager_cmds[] = {
    "dde-file-manager", "nautilus", "dolphin", "pcmanfm-qt",
    "thunar", "pcmanfm", "nemo",
    NULL,
};

static const char *const default_volume_up_cmds[] = {
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+",
    "pactl set-sink-volume @DEFAULT_SINK@ +5%",
    NULL,
};

static const char *const default_volume_down_cmds[] = {
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-",
    "pactl set-sink-volume @DEFAULT_SINK@ -5%",
    NULL,
};

static const char *const default_volume_mute_cmds[] = {
    "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle",
    "pactl set-sink-mute @DEFAULT_SINK@ toggle",
    NULL,
};

static const char *const default_brightness_up_cmds[] = {
    "brightnessctl set 5%+",
    NULL,
};

static const char *const default_brightness_down_cmds[] = {
    "brightnessctl set 5%-",
    NULL,
};

static const char *const default_player_play_cmds[] = {
    "playerctl play-pause",
    NULL,
};

static const char *const default_player_next_cmds[] = {
    "playerctl next",
    NULL,
};

static const char *const default_player_prev_cmds[] = {
    "playerctl previous",
    NULL,
};

struct wsd_default {
    const char *key;
    uint32_t flags;
    uint32_t action; /* commands 为 NULL 时生效 */
    const char *const *commands;
};

static const struct wsd_default wsd_defaults[] = {
    /* 窗口/工作区：woodland 内置键位里没有的部分，交给合成器动作 */
    {"Meta+Q", WSD_FLAG_PRESS, WSD_ACTION_CLOSE_WINDOW, NULL},
    {"Meta+Up", WSD_FLAG_PRESS, WSD_ACTION_MAXIMIZE, NULL},
    {"Meta+Down", WSD_FLAG_PRESS, WSD_ACTION_CANCEL_MAXIMIZE, NULL},
    {"Meta+Left", WSD_FLAG_PRESS, WSD_ACTION_TILE_LEFT, NULL},
    {"Meta+Right", WSD_FLAG_PRESS, WSD_ACTION_TILE_RIGHT, NULL},
    /* 启动器：非 DDE 环境的基本可用性就靠这两条 */
    {"Meta+Return", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_terminal_cmds},
    {"Meta+E", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_filemanager_cmds},
    /* 音量/亮度按住要连续变化，所以要 repeat */
    {"XF86AudioRaiseVolume", WSD_FLAG_PRESS | WSD_FLAG_REPEAT, WSD_ACTION_NOTIFY,
     default_volume_up_cmds},
    {"XF86AudioLowerVolume", WSD_FLAG_PRESS | WSD_FLAG_REPEAT, WSD_ACTION_NOTIFY,
     default_volume_down_cmds},
    {"XF86AudioMute", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_volume_mute_cmds},
    {"XF86MonBrightnessUp", WSD_FLAG_PRESS | WSD_FLAG_REPEAT, WSD_ACTION_NOTIFY,
     default_brightness_up_cmds},
    {"XF86MonBrightnessDown", WSD_FLAG_PRESS | WSD_FLAG_REPEAT, WSD_ACTION_NOTIFY,
     default_brightness_down_cmds},
    {"XF86AudioPlay", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_player_play_cmds},
    {"XF86AudioNext", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_player_next_cmds},
    {"XF86AudioPrev", WSD_FLAG_PRESS, WSD_ACTION_NOTIFY, default_player_prev_cmds},
};

static const char *pick_command(const char *const *commands)
{
    for (size_t i = 0; commands && commands[i]; i++) {
        if (wsd_command_available(commands[i])) {
            return commands[i];
        }
    }
    return NULL;
}

void wsd_config_load_defaults(struct wsd_config *cfg)
{
    if (!cfg) {
        return;
    }
    for (size_t i = 0; i < sizeof(wsd_defaults) / sizeof(wsd_defaults[0]); i++) {
        const struct wsd_default *def = &wsd_defaults[i];
        const char *command = def->commands ? pick_command(def->commands) : NULL;
        if (def->commands && !command) {
            /* 一个候选都没有：注册一条按下去没反应的键位只会让人困惑 */
            continue;
        }
        if (!wsd_config_add(cfg, def->key, def->action, def->flags, command)) {
            continue;
        }
        cfg->bindings[cfg->count - 1].builtin_default = true;
    }
}

/* ==================== 读配置文件 ==================== */

static char *read_file(FILE *f, char *err, size_t errlen)
{
    size_t capacity = 4096;
    size_t len = 0;
    char *text = malloc(capacity);
    if (!text) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    while (!feof(f)) {
        if (len + 1024 + 1 > capacity) {
            size_t grown = capacity * 2;
            char *bigger = realloc(text, grown);
            if (!bigger) {
                free(text);
                snprintf(err, errlen, "out of memory");
                return NULL;
            }
            text = bigger;
            capacity = grown;
        }
        size_t n = fread(text + len, 1, capacity - len - 1, f);
        len += n;
        if (ferror(f)) {
            snprintf(err, errlen, "read error: %s", strerror(errno));
            free(text);
            return NULL;
        }
    }
    text[len] = '\0';
    return text;
}

static void entry_warn(size_t index, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void entry_warn(size_t index, const char *key, const char *fmt, ...)
{
    fprintf(stderr, "woodland-shortcutd: [[shortcut]] #%zu (key=%s): ", index,
            (key && *key) ? key : "-");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void load_entry(struct wsd_config *cfg, const struct toml_table *tbl, size_t index)
{
    const char *key = toml_get_string(tbl, "key", NULL);
    const char *command = toml_get_string(tbl, "command", NULL);
    const char *action_text = toml_get_string(tbl, "action", NULL);
    const char *flags_text = toml_get_string(tbl, "flags", NULL);

    if (!key || !*key) {
        entry_warn(index, NULL, "缺少 key，整条跳过");
        return;
    }
    if (!command && !action_text) {
        entry_warn(index, key, "command 与 action 都没有，整条跳过");
        return;
    }
    if (command && action_text) {
        entry_warn(index, key, "command 与 action 只能二选一，整条跳过");
        return;
    }

    uint32_t flags = WSD_FLAG_PRESS;
    if (flags_text && !wsd_flags_from_string(flags_text, &flags)) {
        entry_warn(index, key, "flags=\"%s\" 不认识（press/release/repeat 用 | 组合），整条跳过",
                   flags_text);
        return;
    }

    uint32_t action = WSD_ACTION_NOTIFY;
    if (action_text && !wsd_action_from_name(action_text, &action)) {
        entry_warn(index, key, "action=\"%s\" 不是已知动作（见 --help 的 action 清单），整条跳过",
                   action_text);
        return;
    }
    if (!command && action == WSD_ACTION_NOTIFY) {
        entry_warn(index, key, "action=notify 要靠 command 才有意义，整条跳过");
        return;
    }
    if (command && !*command) {
        entry_warn(index, key, "command 是空串，整条跳过");
        return;
    }
    if (command && !wsd_command_available(command)) {
        /* 只警告不跳过：命令可能在这之后才装好，注册着让用户先看到 wctl 里有它 */
        entry_warn(index, key, "command 的第一个词不在 PATH 里（仍然注册）：%s", command);
    }

    if (!wsd_config_add(cfg, key, action, flags, command)) {
        entry_warn(index, key, "内存不足或字段非法，整条跳过");
    }
}

enum wsd_load_result wsd_config_load_file(struct wsd_config *cfg, const char *path,
                                          char *err, size_t errlen)
{
    char errbuf[256] = {0};
    if (!err) {
        err = errbuf;
        errlen = sizeof(errbuf);
    }
    if (!cfg || !path) {
        snprintf(err, errlen, "no path");
        return WSD_LOAD_ERROR;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            return WSD_LOAD_MISSING;
        }
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return WSD_LOAD_ERROR;
    }
    char *text = read_file(f, err, errlen);
    fclose(f);
    if (!text) {
        return WSD_LOAD_ERROR;
    }

    char parse_err[256] = {0};
    struct toml_doc *doc = toml_parse(text, parse_err, sizeof(parse_err));
    free(text);
    if (!doc) {
        snprintf(err, errlen, "%s: %s", path, parse_err);
        return WSD_LOAD_ERROR;
    }

    const struct toml_table *general = toml_find_table(doc, "general");
    if (general) {
        cfg->enabled = toml_get_bool(general, "enabled", cfg->enabled);
    }

    size_t index = 0;
    const struct toml_table *tbl = toml_find_table(doc, "shortcut");
    while (tbl) {
        load_entry(cfg, tbl, ++index);
        tbl = toml_next_table(tbl, "shortcut");
    }
    toml_free(doc);
    return WSD_LOAD_OK;
}

/* ==================== 路径 / 输出 ==================== */

const char *wsd_config_default_path(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return NULL;
    }
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home && *config_home) {
        snprintf(buf, len, "%s/woodland/shortcuts.toml", config_home);
        return buf;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, len, "%s/.config/woodland/shortcuts.toml", home);
        return buf;
    }
    snprintf(buf, len, "woodland/shortcuts.toml");
    return buf;
}

static const char *flags_to_string(uint32_t flags, char *buf, size_t len)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } parts[] = {
        {WSD_FLAG_PRESS, "press"},
        {WSD_FLAG_RELEASE, "release"},
        {WSD_FLAG_REPEAT, "repeat"},
    };
    if (!buf || len == 0) {
        return "";
    }
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (!(flags & parts[i].bit)) {
            continue;
        }
        int n = snprintf(buf + used, len - used, "%s%s", used ? "|" : "", parts[i].name);
        if (n < 0 || (size_t)n >= len - used) {
            break;
        }
        used += (size_t)n;
    }
    return buf;
}

void wsd_config_dump(const struct wsd_config *cfg, FILE *out)
{
    if (!cfg || !out) {
        return;
    }
    fprintf(out, "enabled=%s bindings=%zu\n", cfg->enabled ? "true" : "false", cfg->count);
    for (size_t i = 0; i < cfg->count; i++) {
        const struct wsd_binding *b = &cfg->bindings[i];
        char flags[64];
        flags_to_string(b->flags, flags, sizeof(flags));
        if (b->command) {
            fprintf(out, "  %-24s command  %-8s %s%s\n", b->key, flags, b->command,
                    b->builtin_default ? "   (default)" : "");
        } else {
            const char *name = wsd_action_name(b->action);
            fprintf(out, "  %-24s action   %-8s %s%s\n", b->key, flags,
                    name ? name : "?", b->builtin_default ? "   (default)" : "");
        }
    }
}

void wsd_print_sample_config(FILE *out)
{
    if (!out) {
        return;
    }
    fprintf(out,
            "# woodland-shortcutd 配置：$XDG_CONFIG_HOME/woodland/shortcuts.toml\n"
            "#\n"
            "# 一条 [[shortcut]] 对应合成器里的一次 bind_key：\n"
            "#   key     必填，QKeySequence PortableText（\"Ctrl+Alt+T\"、\"XF86AudioMute\"）\n"
            "#   command 可选，命中后在本地执行（合成器回 activated 事件），走 /bin/sh -c\n"
            "#   action  可选，交给合成器执行的内建动作（tile_left / close_window / ...）\n"
            "#   flags   可选，默认 press；press|release|repeat 可组合\n"
            "#\n"
            "# command 与 action 二选一。key 的合法性由合成器校验，写错了会在日志里\n"
            "# 看到 bind_failure。下面是内置默认键位：命令类条目按 PATH 挑选可用程序，\n"
            "# 一个都找不到的会以注释形式列出。\n"
            "\n"
            "[general]\n"
            "enabled = true\n");

    for (size_t i = 0; i < sizeof(wsd_defaults) / sizeof(wsd_defaults[0]); i++) {
        const struct wsd_default *def = &wsd_defaults[i];
        const char *command = def->commands ? pick_command(def->commands) : NULL;
        char flags[64];
        flags_to_string(def->flags, flags, sizeof(flags));
        bool missing = def->commands && !command;

        fprintf(out, "\n");
        if (missing) {
            fprintf(out, "# 未找到可用程序（候选：%s）\n", def->commands[0]);
            fprintf(out, "# [[shortcut]]\n# key = \"%s\"\n", def->key);
            if (def->commands) {
                fprintf(out, "# command = \"%s\"\n", def->commands[0]);
            }
            continue;
        }
        fprintf(out, "[[shortcut]]\nkey = \"%s\"\n", def->key);
        if (command) {
            fprintf(out, "command = \"%s\"\n", command);
        } else {
            fprintf(out, "action = \"%s\"\n", wsd_action_name(def->action));
        }
        fprintf(out, "flags = \"%s\"\n", flags);
    }
}
