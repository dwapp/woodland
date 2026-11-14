// SPDX-License-Identifier: GPL-2.0-only
/*
 * util/env 的回归：XDG_CURRENT_DESKTOP 判定与"环境变量关掉某功能"的词表。
 *
 * 这两个函数本身只是字符串比较，但 woodland_desktop_is_dde() 决定合成器
 * 要不要在会话里拉 woodland-shortcutd——判错的后果是 DDE 的全局快捷键被
 * 抢走（对方报 occupied，用户只看到"快捷键失效"）。所以把见过的取值形态
 * 全钉一遍。
 */
#include <assert.h>
#include <stdio.h>

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
    test_env_is_off();
    printf("env unit passed\n");
    return 0;
}
