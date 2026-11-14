// SPDX-License-Identifier: GPL-2.0-only
/*
 * 快捷键命令启动器的回归。见 src/shortcutd/spawn.c。
 *
 * 这段代码在真实环境里只有用户真的按下那个键才跑，冒烟测试够不着；而它
 * 出错的方式都很隐蔽：命令没起来、每次按键攒一个僵尸、启动的程序跟着
 * wsd 一起死。所以这里直接调它。
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "shortcutd_spawn.h"

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void test_spawn_runs_command(void)
{
    char path[] = "/tmp/wsd-spawn-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "touch %s", path);
    assert(wsd_spawn_command(cmd));

    /* 命令在孙进程里跑，启动是异步的：轮询等它落地 */
    bool created = false;
    for (int i = 0; i < 300; i++) {
        if (access(path, F_OK) == 0) {
            created = true;
            break;
        }
        sleep_ms(10);
    }
    assert(created);
    unlink(path);
}

static void test_spawn_rejects_empty(void)
{
    assert(!wsd_spawn_command(NULL));
    assert(!wsd_spawn_command(""));
}

static void test_spawn_leaves_no_zombies(void)
{
    /*
     * 连起 20 条。spawn 的中间层必须在这之后全部被回收——只要漏一个，
     * waitpid(-1) 就会返回那个 pid 而不是 ECHILD。攒僵尸正是"每天按几百次
     * Super+Return 之后进程表里一堆 defunct"的来源。
     */
    for (int i = 0; i < 20; i++) {
        assert(wsd_spawn_command("true"));
    }
    int status = 0;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    assert(pid == -1);
    assert(errno == ECHILD);
}

int main(void)
{
    test_spawn_runs_command();
    test_spawn_rejects_empty();
    test_spawn_leaves_no_zombies();
    printf("shortcutd_spawn unit passed\n");
    return 0;
}
