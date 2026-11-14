// SPDX-License-Identifier: GPL-2.0-only
/*
 * 快捷键命令启动器。见 include/shortcutd_spawn.h。
 *
 * 和 src/xwayland/xwayland.c:spawn_script_async() 同一套进程模型：中间层
 * 立即退出、孙进程 setsid 后 exec，父进程 waitpid 回收中间层。区别是这里
 * 不阻塞很久——中间层只做一次 fork，所以 waitpid 几乎是立刻返回。
 *
 * 为什么要这么绕：
 *   - 直接单 fork + exec 的话，wsd 得一直替命令收尸，要么装 SIGCHLD handler
 *     把别的子进程的退出也吞掉，要么攒僵尸；
 *   - 孙进程 setsid 之后脱离 wsd 的进程组和会话，wsd 退出/被杀不会带走
 *     用户刚打开的程序（这正是"按键启动的终端不该跟着守护进程一起死"）。
 */
#include "shortcutd_spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

bool wsd_spawn_command(const char *command)
{
    if (!command || !*command) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        /*
         * 子进程只做最小的事，失败不回头（父进程已不会再管它）：
         * setsid 之后孙进程才有自己的会话，Ctrl+C 之类的终端信号不会
         * 顺着 wsd 的进程组漏给用户程序。
         */
        setsid();

        pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild == 0) {
            /*
             * wsd 是 Wayland 客户端，继承来的 fd 里 wayland socket 带
             * CLOEXEC；stdin 指向启动它的终端，用户程序不该跟着抢。
             */
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > STDERR_FILENO) {
                    close(devnull);
                }
            }
            /*
             * 合成器把 SIGPIPE 设成了 SIG_IGN，被忽略的信号会跨 exec 继承；
             * 用户程序要的是默认行为（否则写断开管道时拿不到 EPIPE/SIGPIPE）。
             */
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }

    /* 中间层马上退，阻塞回收它：既不留僵尸，也不用装全局 SIGCHLD handler */
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        /* 被信号打断就继续等 */
    }
    return true;
}
