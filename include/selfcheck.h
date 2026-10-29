// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SELFCHECK_H
#define WOODLAND_SELFCHECK_H

#include <stdbool.h>

struct woodland_server;
struct woodland_json;

/* 单次检查最多报几条（多出来的只计数） */
#define WOODLAND_SELFCHECK_MAX 16
/* 检查失败时记录的说明长度 */
#define WOODLAND_SELFCHECK_TEXT 96

/**
 * 一条不变量检查的结果。
 *
 * expected/actual 只在失败时有意义，但始终填：日志和 JSON 共用同一份
 * 结果，省掉"失败时再拼一遍字符串"的重复代码。
 */
struct woodland_check {
    const char *name;
    bool ok;
    char expected[WOODLAND_SELFCHECK_TEXT];
    char actual[WOODLAND_SELFCHECK_TEXT];
};

/**
 * 收集当前状态的不变量检查结果，返回条数（<= max）。
 *
 * 检查项都是"内部状态自洽"类断言，不依赖客户端的下一帧：
 *
 *   views_list_intact        views 链表无环、每条都在链表里
 *   mapped_in_workspace      已映射视图有工作区，scene_tree 挂在该工作区树下
 *   focused_view_valid       focused_view 在链表里、可聚焦、有 surface
 *   activated_unique         激活视图至多一个，且就是 focused_view
 *   ssd_active_matches       SSD 标题栏激活色与 activated 一致
 *   ft_activated_matches     foreign-toplevel 激活位与 activated 一致
 *   xdg_activated_matches    xdg configure 的 activated 与 activated 一致
 *   ssd_geometry_matches     SSD 缓存的内容尺寸与当前内容尺寸一致
 *
 * 刻意不检查"窗口几何 == 期望几何"：configure 是否被客户端 ack 是异步的，
 * 刚发出去时几何必然还是旧的，那种断言只会制造假警报。
 */
int woodland_selfcheck_collect(struct woodland_server *server,
                               struct woodland_check *out, int max);

/**
 * 跑一遍检查，把结果写成 JSON 到 out：
 *   {"ok":true,"failures":0,"checks":[{"name":..,"ok":true}, ...]}
 * 失败项额外带 expected/actual。返回失败条数。
 */
int woodland_selfcheck_run(struct woodland_server *server,
                           struct woodland_json *out);

/**
 * 状态变化后调度一次检查（未开启 self_check 时直接返回）。
 *
 * 用 idle 回调而不是立即执行：一次事件（比如 map）里会连着改
 * activated / SSD / foreign-toplevel，中途检查必然看到半成品。
 * idle 在本轮事件派发结束后才跑，正好是"改完了"的时刻。
 */
void woodland_selfcheck_schedule(struct woodland_server *server,
                                 const char *reason);

/** 开启/关闭检查（server->self_check 由 main 设置，这里只建 idle 源）。 */
void woodland_selfcheck_init(struct woodland_server *server);
void woodland_selfcheck_finish(struct woodland_server *server);

#endif
