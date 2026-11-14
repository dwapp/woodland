// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SHORTCUTD_SPAWN_H
#define WOODLAND_SHORTCUTD_SPAWN_H

/*
 * 快捷键命令的启动器。
 *
 * 单独成文件是为了能被单元测试直接调用：这里最容易出错（僵尸进程、子进程
 * 粘住合成器的终端、信号继承），而它在真实环境里要等用户真的按下那个键
 * 才跑一次，靠冒烟测试覆盖不到。
 */

#include <stdbool.h>
#include <sys/types.h>

/*
 * 用 /bin/sh -c 执行一条命令：fork → setsid → 二次 fork → exec。
 *
 * 二次 fork 让真正的命令脱离 wsd 的进程组并被 init 收养，wsd 只回收中间
 * 那个立即退出的壳进程（阻塞 waitpid），因此不会积累僵尸。
 *
 * 成功返回 true（只代表命令已启动，不代表命令自身成功）。
 */
bool wsd_spawn_command(const char *command);

#endif /* WOODLAND_SHORTCUTD_SPAWN_H */
