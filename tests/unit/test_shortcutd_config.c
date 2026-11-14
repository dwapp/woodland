// SPDX-License-Identifier: GPL-2.0-only
/*
 * woodland-shortcutd 的配置层回归：action/flags 名表、条目增删、TOML 读取、
 * 默认键位的 PATH 探测。
 *
 * 不连 display、不起合成器：wsd 与合成器的交界是 bind_key/bind_failure，
 * 那部分由 smoke-shortcutd 覆盖；这里盯的是"用户写了什么、我们打算注册什么"，
 * 这层错了会表现为"某条键位静默失效"，比连不上合成器更难查。
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shortcutd_config.h"

static char *write_temp(const char *content)
{
    char tmpl[] = "/tmp/wsd-config-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
    char *path = strdup(tmpl);
    assert(path);
    return path;
}

static const struct wsd_binding *find_key(const struct wsd_config *cfg, const char *key)
{
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->bindings[i].key, key) == 0) {
            return &cfg->bindings[i];
        }
    }
    return NULL;
}

static void test_action_names(void)
{
    /* 0..45 每个动作都要有名字，且能从名字解析回同一个值： --help、配置文件、
     * wctl 输出三处显示的名字必须是同一套 */
    for (uint32_t i = 0; i <= WSD_ACTION_MAX; i++) {
        const char *name = wsd_action_name(i);
        assert(name && *name);
        uint32_t back = 12345;
        assert(wsd_action_from_name(name, &back));
        assert(back == i);
    }
    assert(wsd_action_name(WSD_ACTION_MAX + 1) == NULL);

    uint32_t action = 0;
    assert(wsd_action_from_name("TILE_LEFT", &action));
    assert(action == WSD_ACTION_TILE_LEFT);
    assert(!wsd_action_from_name("tile_zigzag", &action));
    assert(!wsd_action_from_name("", &action));
    assert(!wsd_action_from_name(NULL, &action));
    /* 未知名字不许把出参改坏 */
    action = 77;
    assert(!wsd_action_from_name("nope", &action));
    assert(action == 77);
}

static void test_flags(void)
{
    uint32_t flags = 0;
    assert(wsd_flags_from_string("press", &flags) && flags == WSD_FLAG_PRESS);
    assert(wsd_flags_from_string("press|repeat", &flags) &&
           flags == (WSD_FLAG_PRESS | WSD_FLAG_REPEAT));
    assert(wsd_flags_from_string("release, repeat", &flags) &&
           flags == (WSD_FLAG_RELEASE | WSD_FLAG_REPEAT));
    assert(wsd_flags_from_string("key_press", &flags) && flags == WSD_FLAG_PRESS);
    assert(!wsd_flags_from_string("press|hold", &flags));
    assert(!wsd_flags_from_string("", &flags));
    assert(!wsd_flags_from_string(NULL, &flags));
}

static void test_add_and_names(void)
{
    struct wsd_config cfg;
    wsd_config_init(&cfg);
    assert(cfg.enabled);

    assert(wsd_config_add(&cfg, "Meta+A", WSD_ACTION_NOTIFY, WSD_FLAG_PRESS, "true"));
    assert(cfg.count == 1);
    /* 名字就是 key 文本：合成器把 name 当唯一键，配置层保证 key 唯一 */
    assert(strcmp(cfg.bindings[0].name, "Meta+A") == 0);
    assert(cfg.bindings[0].command && strcmp(cfg.bindings[0].command, "true") == 0);

    /*
     * 同一个组合只留第一条。合成器侧一条组合只能有一条生效：同 action 会被
     * bind_key 的 upsert 吞掉（还保留先注册的 name），不同 action 只有表里
     * 靠前的那条会命中。以前这里给重名加 "#2" 让两条都注册进去，结果是用户
     * 看到"注册成功"但第二条永远不生效——比直接拒绝更糟。大小写/空白不同
     * 也要认成同一条：合成器的解析对这两者不敏感。
     */
    assert(!wsd_config_add(&cfg, "Meta+A", WSD_ACTION_CLOSE_WINDOW, WSD_FLAG_PRESS, NULL));
    assert(!wsd_config_add(&cfg, "meta+a", WSD_ACTION_NOTIFY, WSD_FLAG_PRESS, "true"));
    assert(!wsd_config_add(&cfg, " Meta + A ", WSD_ACTION_NOTIFY, WSD_FLAG_PRESS, "true"));
    assert(cfg.count == 1);
    assert(wsd_config_has_key(&cfg, "META+A"));
    assert(!wsd_config_has_key(&cfg, "Meta+B"));

    /* 非法输入一律拒绝，别把半条绑定塞进表里 */
    assert(!wsd_config_add(&cfg, "", WSD_ACTION_NOTIFY, WSD_FLAG_PRESS, "true"));
    assert(!wsd_config_add(&cfg, NULL, WSD_ACTION_NOTIFY, WSD_FLAG_PRESS, "true"));
    assert(!wsd_config_add(&cfg, "Meta+B", WSD_ACTION_NOTIFY, 0, "true"));
    assert(!wsd_config_add(&cfg, "Meta+B", WSD_ACTION_NOTIFY, 0x8, "true"));
    assert(!wsd_config_add(&cfg, "Meta+B", WSD_ACTION_MAX + 1, WSD_FLAG_PRESS, NULL));
    /* command 只能配 notify：带 command 又给具体动作等于两条语义打架 */
    assert(!wsd_config_add(&cfg, "Meta+B", WSD_ACTION_TILE_LEFT, WSD_FLAG_PRESS, "true"));
    assert(cfg.count == 1);

    wsd_config_clear(&cfg);
    assert(cfg.count == 0 && cfg.bindings == NULL);
}

static void test_load_file(void)
{
    const char *content =
        "[general]\n"
        "enabled = true\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+T\"\n"
        "command = \"foot -e tmux\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Super+X\"\n"
        "action = \"tile_right\"\n"
        "flags = \"press|repeat\"\n";
    char *path = write_temp(content);

    struct wsd_config cfg;
    wsd_config_init(&cfg);
    char err[256] = {0};
    assert(wsd_config_load_file(&cfg, path, err, sizeof(err)) == WSD_LOAD_OK);
    assert(cfg.enabled);
    assert(cfg.count == 2);
    assert(strcmp(cfg.bindings[0].key, "Ctrl+Alt+T") == 0);
    assert(strcmp(cfg.bindings[0].command, "foot -e tmux") == 0);
    assert(cfg.bindings[0].action == WSD_ACTION_NOTIFY);
    assert(cfg.bindings[0].flags == WSD_FLAG_PRESS);
    assert(strcmp(cfg.bindings[1].key, "Super+X") == 0);
    assert(cfg.bindings[1].command == NULL);
    assert(cfg.bindings[1].action == WSD_ACTION_TILE_RIGHT);
    assert(cfg.bindings[1].flags == (WSD_FLAG_PRESS | WSD_FLAG_REPEAT));

    wsd_config_clear(&cfg);
    unlink(path);
    free(path);
}

static void test_load_skips_bad_entries(void)
{
    /* 单条写错只丢那一条：一次拼错不该让整套键位失效 */
    const char *content =
        "[[shortcut]]\n"
        "key = \"F5\"\n"
        "action = \"tile_zigzag\"\n"
        "\n"
        "[[shortcut]]\n"
        "command = \"foot\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+K\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+J\"\n"
        "action = \"close_window\"\n"
        "command = \"foot\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+H\"\n"
        "action = \"notify\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+G\"\n"
        "action = \"close_window\"\n"
        "flags = \"later\"\n"
        "\n"
        "[[shortcut]]\n"
        "key = \"Ctrl+Alt+F\"\n"
        "action = \"close_window\"\n"
        "\n"
        /* 与上一条同一个组合（大小写不同、action 不同）：整条跳过，
         * 保留先出现的那条的 key 原文与 action */
        "[[shortcut]]\n"
        "key = \"ctrl+alt+f\"\n"
        "action = \"maximize\"\n";
    char *path = write_temp(content);

    struct wsd_config cfg;
    wsd_config_init(&cfg);
    char err[256] = {0};
    assert(wsd_config_load_file(&cfg, path, err, sizeof(err)) == WSD_LOAD_OK);
    assert(cfg.count == 1);
    assert(strcmp(cfg.bindings[0].key, "Ctrl+Alt+F") == 0);
    /* 重复的那条被跳过，先出现的那条原样保留 */
    assert(cfg.bindings[0].action == WSD_ACTION_CLOSE_WINDOW);

    wsd_config_clear(&cfg);
    unlink(path);
    free(path);
}

static void test_load_disabled_and_missing(void)
{
    char *path = write_temp("[general]\nenabled = false\n");
    struct wsd_config cfg;
    wsd_config_init(&cfg);
    char err[256] = {0};
    assert(wsd_config_load_file(&cfg, path, err, sizeof(err)) == WSD_LOAD_OK);
    assert(!cfg.enabled);
    wsd_config_clear(&cfg);
    unlink(path);
    free(path);

    /* 文件不存在是"没配过"，不是错误：调用方据此填默认键位 */
    wsd_config_init(&cfg);
    assert(wsd_config_load_file(&cfg, "/nonexistent/wsd/shortcuts.toml", err, sizeof(err)) ==
           WSD_LOAD_MISSING);
    wsd_config_clear(&cfg);

    /* 语法错误才是错误，且必须带上行号 */
    path = write_temp("this line has no equals sign\n");
    wsd_config_init(&cfg);
    err[0] = '\0';
    assert(wsd_config_load_file(&cfg, path, err, sizeof(err)) == WSD_LOAD_ERROR);
    assert(strstr(err, "line 1") != NULL);
    wsd_config_clear(&cfg);
    unlink(path);
    free(path);
}

static void test_command_available(void)
{
    assert(wsd_command_available("sh"));
    assert(wsd_command_available("sh -c true"));
    assert(!wsd_command_available("definitely-not-a-command-xyz"));
    assert(!wsd_command_available(""));
    assert(!wsd_command_available(NULL));
    /* 带斜杠的按路径判，不走 PATH */
    assert(wsd_command_available("/bin/sh"));
    assert(!wsd_command_available("/nonexistent/bin/sh"));
}

static void test_defaults_use_path(void)
{
    char old_path[4096];
    const char *saved = getenv("PATH");
    snprintf(old_path, sizeof(old_path), "%s", saved ? saved : "");

    char dir[] = "/tmp/wsd-path-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char foot[512];
    snprintf(foot, sizeof(foot), "%s/foot", dir);
    FILE *f = fopen(foot, "w");
    assert(f);
    fputs("#!/bin/sh\n", f);
    fclose(f);
    assert(chmod(foot, 0755) == 0);
    char wpctl[512];
    snprintf(wpctl, sizeof(wpctl), "%s/wpctl", dir);
    f = fopen(wpctl, "w");
    assert(f);
    fputs("#!/bin/sh\n", f);
    fclose(f);
    assert(chmod(wpctl, 0755) == 0);

    assert(setenv("PATH", dir, 1) == 0);

    struct wsd_config cfg;
    wsd_config_init(&cfg);
    wsd_config_load_defaults(&cfg);

    const struct wsd_binding *term = find_key(&cfg, "Meta+Return");
    assert(term && term->command && strcmp(term->command, "foot") == 0);
    assert(term->builtin_default);

    /* 音量：wpctl 存在就用 wpctl，不退回 pactl */
    const struct wsd_binding *vol = find_key(&cfg, "XF86AudioRaiseVolume");
    assert(vol && vol->command && strstr(vol->command, "wpctl") != NULL);
    assert(vol->flags == (WSD_FLAG_PRESS | WSD_FLAG_REPEAT));

    /* 亮度：brightnessctl 不存在，这条就不注册（按下去没反应的键位更糟） */
    assert(find_key(&cfg, "XF86MonBrightnessUp") == NULL);
    /* playerctl 也不在，媒体键同理 */
    assert(find_key(&cfg, "XF86AudioPlay") == NULL);
    /* action 类不依赖 PATH，永远在 */
    const struct wsd_binding *tile = find_key(&cfg, "Meta+Left");
    assert(tile && tile->command == NULL && tile->action == WSD_ACTION_TILE_LEFT);

    wsd_config_clear(&cfg);
    assert(setenv("PATH", old_path, 1) == 0);
    unlink(foot);
    unlink(wpctl);
    rmdir(dir);
}

static void test_sample_roundtrip(void)
{
    /* --print-default-config 的输出必须能原样喂回 -c：用户复制的第一份配置
     * 就该是可用的 */
    char tmpl[] = "/tmp/wsd-sample-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    wsd_print_sample_config(f);
    fclose(f);

    struct wsd_config cfg;
    wsd_config_init(&cfg);
    char err[256] = {0};
    assert(wsd_config_load_file(&cfg, tmpl, err, sizeof(err)) == WSD_LOAD_OK);
    assert(cfg.count > 0);
    /* 示例是"内置默认键位"，所以 action 类条目一定在里面 */
    assert(find_key(&cfg, "Meta+Left") != NULL);

    wsd_config_clear(&cfg);
    unlink(tmpl);
}

static void test_default_path(void)
{
    char old_config_home[1024];
    const char *saved = getenv("XDG_CONFIG_HOME");
    snprintf(old_config_home, sizeof(old_config_home), "%s", saved ? saved : "");

    assert(setenv("XDG_CONFIG_HOME", "/tmp/wsd-xdg", 1) == 0);
    char buf[512];
    const char *path = wsd_config_default_path(buf, sizeof(buf));
    assert(path && strcmp(path, "/tmp/wsd-xdg/woodland/shortcuts.toml") == 0);

    /* 没设 XDG_CONFIG_HOME 时退回 ~/.config */
    unsetenv("XDG_CONFIG_HOME");
    assert(setenv("HOME", "/tmp/wsd-home", 1) == 0);
    path = wsd_config_default_path(buf, sizeof(buf));
    assert(path && strcmp(path, "/tmp/wsd-home/.config/woodland/shortcuts.toml") == 0);

    if (saved) {
        assert(setenv("XDG_CONFIG_HOME", old_config_home, 1) == 0);
    }
}

int main(void)
{
    test_action_names();
    test_flags();
    test_add_and_names();
    test_load_file();
    test_load_skips_bad_entries();
    test_load_disabled_and_missing();
    test_command_available();
    test_defaults_use_path();
    test_sample_roundtrip();
    test_default_path();
    printf("shortcutd_config unit passed\n");
    return 0;
}
