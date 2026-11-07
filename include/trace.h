// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TRACE_H
#define WOODLAND_TRACE_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_json;

/*
 * 决策轨迹（--trace）：定长环形缓冲，记"什么事件、动了哪个对象、依据是什么"。
 *
 * 为什么需要它：这是事件驱动的合成器，调试时最常问的三个问题是
 * "这个 handler 跑了吗""它走了哪个分支""上一步是谁改的"。没有轨迹时
 * 唯一的回答方式是加 printf、重编、重跑；有轨迹时一条 `wctl get_trace`
 * 就够——这正是本项目"AI 优先"要省掉的那一步。
 *
 * 三条半硬约束，加日志前先读：
 *   1. 关闭时不做任何分配与格式化，只剩一次函数调用 + 一次 bool 判断；
 *   2. 打开后也不再分配：缓冲在 init 时一次 calloc，写满就覆盖最旧的，
 *      seq 单调递增，客户端据此能算出被覆盖了多少条（dropped）；
 *   3. 任何模块都可以写，trace 只依赖 util/json。**不要**为了记一条轨迹
 *      去反向依赖业务模块，否则它会变成依赖环的入口；
 *   4. 不加锁：所有写入点和 dump 都必须在主事件循环里调用。现在成立
 *      （埋点全在 wlroots handler，get_trace 走同一循环），别让异步
 *      路径伸手进来。
 *
 * 为什么状态是模块内的 static 而不是 struct woodland_server 的字段：
 * 它是诊断汇（sink），不是业务状态，和 util/log.c 的 verbosity 同类。
 * 放进去的话每个调用点都要把 server 指针层层传下来，收益为零。
 */

/** 打开/关闭轨迹。enabled=false 时不分配任何内存，add() 直接返回。 */
void woodland_trace_init(bool enabled);
void woodland_trace_finish(void);

/** 当前是否在记录。调用方在拼参数较贵时可以先用它短路。 */
bool woodland_trace_enabled(void);

/**
 * 记录一条。obj 是被改动的对象 id（没有就传 0，例如输出事件）。
 *
 * what 用短标签且**尽量稳定**（脚本会 grep 它，别写成一句人话）。
 * 这是全仓库唯一的权威清单，加新标签先改这里、再发第一条轨迹；
 * AGENTS.md 里的清单只是转述：
 *   map / unmap / destroy / focus / focus_none / layer_focus / place /
 *   workspace / output_add / output_remove / cfg_apply / deco / bg_effect /
 *   action / appearance / shortcut / ftl / tearing / vt / snap
 *
 * detail 只写**决策依据**（哪个分支、算了什么、为什么），不要重复
 * get_tree 已经能查到的状态——轨迹的价值在"为什么"，不在"是什么"。
 * 没有细节就传 NULL（不要传 ""：-Wformat-zero-length 会把它当错误拦下）。
 */
void woodland_trace_add(const char *what, uint32_t obj, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** 缓冲里当前还有几条（绕圈后恒等于容量）。 */
uint32_t woodland_trace_count(void);

/** 累计写入过多少条；比 count 大就说明有记录被覆盖了。 */
uint32_t woodland_trace_total(void);

/** 把整个环按 seq 升序导出成一份完整的 JSON 响应。 */
void woodland_trace_dump(struct woodland_json *out);

#endif
