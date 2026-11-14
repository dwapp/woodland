// SPDX-License-Identifier: GPL-2.0-only
/*
 * util/env 的回归：桌面/会话名判定与"环境变量关掉某功能"的词表。
 *
 * 这些函数本身只是字符串比较，但 woodland_session_is_dde() 决定合成器
 * 要不要在会话里拉 woodland-shortcutd——判错的后果是 DDE 的全局快捷键被
 * 抢走（对方报 occupied，用户只看到"快捷键失效"）。所以把见过的取值形态
 * 全钉一遍，包括三个会话变量各自单独出现的组合。
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "util/env.h"

static void test_desktop_is_dde(void)
{
    /* DDE 的桌面名在历史版本里两种写法都出现过，会话文件还可能带附加项 */
    assert(woodland_desktop_is_dde("DDE"));
    assert(woodland_desktop_is_dde("dde"));
    assert(woodland_desktop_is_dde("Deepin"));
    assert(woodland_desktop_is_dde("deepin"));
    assert(woodland_desktop_is_dde("deepin-wayland"));
    assert(woodland_desktop_is_dde("DDE:GNOME"));
    assert(woodland_desktop_is_dde("GNOME:DDE"));
    assert(woodland_desktop_is_dde("  DDE  "));

    /* 这几条是最要紧的：woodland 自己的会话文件就是 woodland:wlroots，
     * 判成 DDE 就等于自己的默认快捷键永远起不来 */
    assert(!woodland_desktop_is_dde("woodland:wlroots"));
    assert(!woodland_desktop_is_dde("GNOME"));
    assert(!woodland_desktop_is_dde("KDE"));
    assert(!woodland_desktop_is_dde("LXQt"));
    assert(!woodland_desktop_is_dde("sway"));
    /* 只是长得像 */
    assert(!woodland_desktop_is_dde("ddex"));
    assert(!woodland_desktop_is_dde("xde"));
    assert(!woodland_desktop_is_dde("deep"));
    assert(!woodland_desktop_is_dde(""));
    assert(!woodland_desktop_is_dde(NULL));
}

static void test_session_is_dde(void)
{
    static const char *const vars[] = {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP",
                                       "DESKTOP_SESSION"};
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        unsetenv(vars[i]);
    }
    /* 三个都没设：woodland 自己的会话，快捷键归我们 */
    assert(!woodland_session_is_dde());

    /* 三个来源各自都要能认出来：显示管理器（尤其 lightdm 的 deepin 会话）
     * 不一定把 XDG_CURRENT_DESKTOP 设上，漏一个就会去抢 dde-daemon 的位置 */
    setenv("XDG_CURRENT_DESKTOP", "Deepin", 1);
    assert(woodland_session_is_dde());
    unsetenv("XDG_CURRENT_DESKTOP");

    setenv("XDG_SESSION_DESKTOP", "dde", 1);
    assert(woodland_session_is_dde());
    unsetenv("XDG_SESSION_DESKTOP");

    setenv("DESKTOP_SESSION", "deepin", 1);
    assert(woodland_session_is_dde());
    unsetenv("DESKTOP_SESSION");

    /* 三个都在、但不是 DDE：不许误判（否则默认键位永远起不来） */
    setenv("XDG_CURRENT_DESKTOP", "woodland:wlroots", 1);
    setenv("XDG_SESSION_DESKTOP", "wlroots", 1);
    setenv("DESKTOP_SESSION", "lxqt", 1);
    assert(!woodland_session_is_dde());

    /* 空串等于没设 */
    setenv("DESKTOP_SESSION", "", 1);
    assert(!woodland_session_is_dde());

    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        unsetenv(vars[i]);
    }
}

static void test_env_is_off(void)
{
    assert(woodland_env_is_off(""));
    assert(woodland_env_is_off("0"));
    assert(woodland_env_is_off("off"));
    assert(woodland_env_is_off("OFF"));
    assert(woodland_env_is_off("false"));
    assert(woodland_env_is_off("No"));
    assert(woodland_env_is_off("none"));

    /* 未设置是"没意见"，不是"关掉" */
    assert(!woodland_env_is_off(NULL));
    assert(!woodland_env_is_off("1"));
    assert(!woodland_env_is_off("on"));
    /* 路径形式的覆盖值不能被当成开关 */
    assert(!woodland_env_is_off("/usr/bin/woodland-shortcutd"));
}

int main(void)
{
    test_desktop_is_dde();
    test_session_is_dde();
    test_env_is_off();
    printf("env unit passed\n");
    return 0;
}
