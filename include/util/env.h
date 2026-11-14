// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_UTIL_ENV_H
#define WOODLAND_UTIL_ENV_H

/*
 * 环境探测的小工具。放在 util 而不是各自的模块里，是因为它们的判断依据
 * 只有环境变量本身（无副作用、可单测），而调用方散在 server / 未来的客户端
 * 里。合成器启动 woodland-shortcutd 的时机就取决于这里的判定。
 */

#include <stdbool.h>

/*
 * XDG_CURRENT_DESKTOP 是否声明了 DDE / deepin 会话。
 *
 * 取值按 XDG 规范是冒号分隔的列表（"Deepin"、"DDE:GNOME"、
 * "woodland:wlroots" 都可能出现），所以不能直接 strcmp：
 *   - 任一 token 等于 "dde"（大小写不敏感）→ true
 *   - 任一 token 以 "deepin" 开头（deepin / deepin-wayland）→ true
 * 空串/NULL → false。
 */
bool woodland_desktop_is_dde(const char *current_desktop);

/*
 * 当前会话是不是 DDE（于是全局快捷键归桌面自己管，合成器不许插一个客户端
 * 进去抢独占）。依次看 XDG_CURRENT_DESKTOP / XDG_SESSION_DESKTOP /
 * DESKTOP_SESSION：显示管理器（尤其 lightdm 的 deepin 会话与老式 .desktop
 * 会话）不一定把三个都设上，只看一个会在"漏设"的会话里误判——而误判的
 * 代价是 dde-daemon 拿不到 manager，整套 DDE 快捷键失效，用户只会看到
 * "快捷键突然不好使"。宁可多认几个变量（误判成 DDE 的代价只是我们不启动
 * 客户端，用户可以显式开回来）。
 */
bool woodland_session_is_dde(void);

/*
 * 环境变量值是否表示"关闭"：空串、0、false、no、off、none（大小写不敏感）。
 * NULL（未设置）返回 false——未设置是"没意见"，不是"关掉"。
 */
bool woodland_env_is_off(const char *value);

#endif /* WOODLAND_UTIL_ENV_H */
