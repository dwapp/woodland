// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-shortcut-manager-v3 的纯逻辑回归：QKeySequence PortableText
 * 的解析/格式化、有效快捷键判定、按键匹配、动作分档，以及无协议层时
 * wctl 状态的安全默认值。
 *
 * 这里不建 display、不连客户端：协议交互（acquire / bind / capture / 协议
 * 错误）由 smoke-shortcut 覆盖。协议实现与测试共用同一份推导，避免各自
 * 抄一遍 0..45 的动作表或 Qt 的键名别名。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "protocol.h"
#include "util/json.h"
#include "woodland.h"

static void test_parse(void)
{
    struct woodland_shortcut_key key;

    assert(woodland_shortcut_key_parse("Ctrl+A", &key) == WOODLAND_SC_PARSE_OK);
    assert(key.mods == WOODLAND_SC_MOD_CTRL);
    /* CASE_INSENSITIVE 查 "A" 拿到小写 a；text 才规范化成 Qt 的大写形式 */
    assert(key.sym == (uint32_t)XKB_KEY_a);
    assert(strcmp(key.text, "Ctrl+A") == 0);

    assert(woodland_shortcut_key_parse("Ctrl+Alt+Delete", &key) == WOODLAND_SC_PARSE_OK);
    assert(key.mods == (WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT));
    assert(key.sym == (uint32_t)XKB_KEY_Delete);
    assert(strcmp(key.text, "Ctrl+Alt+Delete") == 0);

    /* Qt 别名 + 大小写不敏感：ctrl+shift+pgup 规范成 Ctrl+Shift+Prior */
    assert(woodland_shortcut_key_parse("ctrl+shift+pgup", &key) == WOODLAND_SC_PARSE_OK);
    assert(key.mods == (WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_SHIFT));
    assert(key.sym == (uint32_t)XKB_KEY_Prior);
    assert(strcmp(key.text, "Ctrl+Shift+Prior") == 0);

    assert(woodland_shortcut_key_parse("Meta+Tab", &key) == WOODLAND_SC_PARSE_OK);
    assert(key.mods == WOODLAND_SC_MOD_META);
    assert(key.sym == (uint32_t)XKB_KEY_Tab);

    assert(woodland_shortcut_key_parse("F1", &key) == WOODLAND_SC_PARSE_OK);
    assert(key.mods == 0);
    assert(key.sym == (uint32_t)XKB_KEY_F1);
    assert(strcmp(key.text, "F1") == 0);

    /* 每一类失败都有稳定名字，冒烟脚本按它断言 */
    assert(woodland_shortcut_key_parse(NULL, &key) == WOODLAND_SC_PARSE_EMPTY);
    assert(woodland_shortcut_key_parse("", &key) == WOODLAND_SC_PARSE_EMPTY);
    assert(woodland_shortcut_key_parse("Ctrl+X, Ctrl+C", &key) ==
           WOODLAND_SC_PARSE_SEQUENCE);
    assert(woodland_shortcut_key_parse("Ctrl+Ctrl+A", &key) ==
           WOODLAND_SC_PARSE_DUPLICATE_MOD);
    assert(woodland_shortcut_key_parse("Ctrl+NoSuchKeyHere", &key) ==
           WOODLAND_SC_PARSE_BAD_TOKEN);
    assert(woodland_shortcut_key_parse("Ctrl", &key) == WOODLAND_SC_PARSE_NO_KEY);
    assert(woodland_shortcut_key_parse("Ctrl+Alt", &key) == WOODLAND_SC_PARSE_NO_KEY);
    assert(woodland_shortcut_key_parse("Meta", &key) == WOODLAND_SC_PARSE_NO_KEY);

    char very_long[256];
    memset(very_long, 'a', sizeof(very_long) - 1);
    very_long[sizeof(very_long) - 1] = '\0';
    assert(woodland_shortcut_key_parse(very_long, &key) == WOODLAND_SC_PARSE_TOO_LONG);
    assert(woodland_shortcut_key_parse("Ctrl+A", NULL) ==
           WOODLAND_SC_PARSE_EMPTY);

    assert(strcmp(woodland_shortcut_parse_name(WOODLAND_SC_PARSE_OK), "ok") == 0);
    assert(strcmp(woodland_shortcut_parse_name(WOODLAND_SC_PARSE_SEQUENCE),
                  "sequence") == 0);
    assert(strcmp(woodland_shortcut_parse_name((enum woodland_shortcut_parse_result)999),
                  "unknown") == 0);
}

static void test_format(void)
{
    char buf[64];
    assert(woodland_shortcut_key_format(WOODLAND_SC_MOD_CTRL, XKB_KEY_a,
                                        buf, sizeof(buf)));
    assert(strcmp(buf, "Ctrl+A") == 0);

    uint32_t all = WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT |
                   WOODLAND_SC_MOD_SHIFT | WOODLAND_SC_MOD_META;
    assert(woodland_shortcut_key_format(all, XKB_KEY_Delete, buf, sizeof(buf)));
    assert(strcmp(buf, "Ctrl+Alt+Shift+Meta+Delete") == 0);

    assert(woodland_shortcut_key_format(0, XKB_KEY_F12, buf, sizeof(buf)));
    assert(strcmp(buf, "F12") == 0);

    char tiny[4];
    assert(!woodland_shortcut_key_format(WOODLAND_SC_MOD_CTRL, XKB_KEY_a,
                                         tiny, sizeof(tiny)));
    assert(!woodland_shortcut_key_format(0, XKB_KEY_NoSymbol, buf, sizeof(buf)));
    assert(!woodland_shortcut_key_format(0, XKB_KEY_a, NULL, 8));
    assert(!woodland_shortcut_key_format(0, XKB_KEY_a, buf, 0));
}

static void test_is_valid(void)
{
    /* 非修饰键 + Ctrl / Alt / Meta */
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_CTRL, XKB_KEY_a));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_ALT, XKB_KEY_F4));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_META, XKB_KEY_Tab));

    /* 单独功能键与独立特殊键 */
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_F1));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_F35));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Delete));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_BackSpace));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Pause));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Print));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Insert));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Help));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Menu));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_Cancel));

    /* Shift 只在配空格 / Esc / Tab / 导航键时有效 */
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_Tab));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_space));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_Escape));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_Prior));
    assert(woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_Left));
    assert(!woodland_shortcut_key_is_valid(WOODLAND_SC_MOD_SHIFT, XKB_KEY_a));

    /* 媒体 / 亮度 / 启动键 */
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_XF86AudioRaiseVolume));
    assert(woodland_shortcut_key_is_valid(0, XKB_KEY_XF86MonBrightnessUp));

    /* 协议明确排除：未修饰的字母数字、单独 Escape、修饰键本身 */
    assert(!woodland_shortcut_key_is_valid(0, XKB_KEY_a));
    assert(!woodland_shortcut_key_is_valid(0, XKB_KEY_1));
    assert(!woodland_shortcut_key_is_valid(0, XKB_KEY_Escape));
    assert(!woodland_shortcut_key_is_valid(0, XKB_KEY_Super_L));
    assert(!woodland_shortcut_key_is_valid(0, XKB_KEY_NoSymbol));
}

static void test_matches(void)
{
    struct woodland_shortcut_key key;
    assert(woodland_shortcut_key_parse("Ctrl+Alt+Delete", &key) ==
           WOODLAND_SC_PARSE_OK);
    assert(woodland_shortcut_key_matches(&key,
           WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT, XKB_KEY_Delete));
    /* 少了/多了修饰键都不能命中，否则全局键位会互相抢 */
    assert(!woodland_shortcut_key_matches(&key, WOODLAND_SC_MOD_CTRL, XKB_KEY_Delete));
    assert(!woodland_shortcut_key_matches(&key,
           WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT | WOODLAND_SC_MOD_SHIFT,
           XKB_KEY_Delete));
    assert(!woodland_shortcut_key_matches(&key,
           WOODLAND_SC_MOD_CTRL | WOODLAND_SC_MOD_ALT, XKB_KEY_Insert));

    /* Ctrl+A 绑定：xkb 事件给的是小写 a，比较要大小写不敏感 */
    assert(woodland_shortcut_key_parse("Ctrl+A", &key) == WOODLAND_SC_PARSE_OK);
    assert(woodland_shortcut_key_matches(&key, WOODLAND_SC_MOD_CTRL, XKB_KEY_a));
    assert(woodland_shortcut_key_matches(&key, WOODLAND_SC_MOD_CTRL, XKB_KEY_A));
    assert(!woodland_shortcut_key_matches(NULL, WOODLAND_SC_MOD_CTRL, XKB_KEY_a));
}

static void test_classify(void)
{
    /* 值来自 protocols/treeland/treeland-shortcut-manager-unstable-v3.xml */
    assert(woodland_shortcut_action_classify(0) == WOODLAND_SC_ACTION_NOTIFY);
    for (uint32_t value = 1; value <= 12; value++) {
        assert(woodland_shortcut_action_classify(value) ==
               WOODLAND_SC_ACTION_WORKSPACE);
    }
    assert(woodland_shortcut_action_classify(13) ==
           WOODLAND_SC_ACTION_WORKSPACE_PREV);
    assert(woodland_shortcut_action_classify(14) ==
           WOODLAND_SC_ACTION_WORKSPACE_NEXT);
    for (uint32_t value = 15; value <= 20; value++) {
        assert(woodland_shortcut_action_classify(value) == WOODLAND_SC_ACTION_VIEW);
    }
    assert(woodland_shortcut_action_classify(21) ==
           WOODLAND_SC_ACTION_UNSUPPORTED);
    assert(woodland_shortcut_action_classify(22) ==
           WOODLAND_SC_ACTION_MOVE_WORKSPACE);
    assert(woodland_shortcut_action_classify(23) ==
           WOODLAND_SC_ACTION_MOVE_WORKSPACE);
    for (uint32_t value = 24; value <= 31; value++) {
        assert(woodland_shortcut_action_classify(value) ==
               WOODLAND_SC_ACTION_UNSUPPORTED);
    }
    for (uint32_t value = 32; value <= 39; value++) {
        assert(woodland_shortcut_action_classify(value) == WOODLAND_SC_ACTION_TILE);
    }
    for (uint32_t value = 40; value <= 45; value++) {
        assert(woodland_shortcut_action_classify(value) ==
               WOODLAND_SC_ACTION_UNSUPPORTED);
    }
    assert(woodland_shortcut_action_classify(46) == WOODLAND_SC_ACTION_INVALID);
    assert(woodland_shortcut_action_classify(999) == WOODLAND_SC_ACTION_INVALID);
    assert(woodland_shortcut_action_classify(UINT32_MAX) ==
           WOODLAND_SC_ACTION_INVALID);
}

static void test_status_defaults(void)
{
    /* 没有协议层时也要写出一份可用的 JSON 对象，不能崩或留半个对象 */
    struct woodland_json out;
    woodland_json_init(&out);
    woodland_protocol_shortcut_status(NULL, &out);
    assert(woodland_json_ok(&out));
    assert(strstr(woodland_json_data(&out), "\"available\":false") != NULL);
    assert(strstr(woodland_json_data(&out), "\"bindings\":[]") != NULL);
    woodland_json_finish(&out);

    struct woodland_server server = {0};
    woodland_json_init(&out);
    woodland_protocol_shortcut_status(&server, &out);
    assert(woodland_json_ok(&out));
    assert(strstr(woodland_json_data(&out), "\"acquired\":false") != NULL);
    woodland_json_finish(&out);
}

int main(void)
{
    test_parse();
    test_format();
    test_is_valid();
    test_matches();
    test_classify();
    test_status_defaults();
    printf("shortcut unit passed\n");
    return 0;
}
