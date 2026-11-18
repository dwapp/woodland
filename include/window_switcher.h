// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WINDOW_SWITCHER_H
#define WOODLAND_WINDOW_SWITCHER_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

/*
 * Alt+Tab 窗口切换器（按住 Alt 期间显示的窗口列表）。
 *
 * 与 focus.h 的 woodland_focus_cycle() 的分工：
 *   - focus_cycle 是"按一下切一个"，无状态、无 UI，适合 F1 这类键位；
 *   - 本模块是"按住 Alt 连按 Tab"的交互：打开时把候选列表**拍成快照**，
 *     之后 Tab/Shift+Tab 只在快照里移动高亮，松开 Alt 才真正改焦点。
 *
 * 拍快照而不是每步重算 z 序，是因为每次聚焦都会把目标窗口提到表头
 * （focus.c 的 view_raise）：边遍历边改序会让列表在脚底下变形，
 * 连按 Tab 会在两个窗口之间来回弹。
 *
 * 候选集用 woodland_view_restorable()（= focusable 里放过"最小化"那一条）：
 * 当前工作区、已映射、非 override-redirect，**最小化的窗口也在内**——Alt+Tab
 * 是"把窗口叫回来"最自然的手势，提交时先恢复再聚焦。列表顺序就是 z 序
 * （最上在前），所以"松开 Alt"落到谁身上，和 Alt+Tab 一次聚焦到谁完全一致。
 */

struct woodland_server;
struct woodland_view;
struct woodland_json;

/* 候选个数上限（面板最多画 WOODLAND_SWITCHER_MAX_ROWS 行，其余靠滚动） */
#define WOODLAND_SWITCHER_MAX_ITEMS 32
#define WOODLAND_SWITCHER_MAX_ROWS 9

/** 不透明状态；字段只对 window_switcher.c 可见。 */
struct woodland_window_switcher;

/**
 * 建立场景节点与监听器（由 woodland_shell_init 调用）。
 * 失败不致命：server->window_switcher 为 NULL 时下面所有接口都是空操作。
 */
bool woodland_window_switcher_init(struct woodland_server *server);

/** 释放（由 woodland_shell_finish 调用）；激活中也只是收起面板。 */
void woodland_window_switcher_finish(struct woodland_server *server);

/** 面板是否正开着（按住 Alt 期间为 true）。 */
bool woodland_window_switcher_is_active(const struct woodland_server *server);

/**
 * 打开切换器并预选下一个窗口（Alt+Tab 的 press 走这里）。
 *
 * forward=false 是 Alt+Shift+Tab 的语义：从 z 序尾部往前选。
 * 已经在切换中时退化为 step()——按住 Alt 连按 Tab 的第二次及以后由
 * seat 的按键钩子直接调 step，走不到这里，这个分支只是防御。
 * 没有候选（一个可聚焦窗口都没有）时返回 false，不弹空面板。
 */
bool woodland_window_switcher_begin(struct woodland_server *server, bool forward);

/** 在候选列表里前/后移动一格并重绘。没有激活或只有一个候选时是空操作。 */
bool woodland_window_switcher_step(struct woodland_server *server, bool forward);

/**
 * 键盘事件入口（press 与 release 都要过这里）。
 *
 * 激活期间键盘归切换器：Tab / ISO_Left_Tab / 方向键移动，Enter / 空格确认，
 * Escape 取消，松开 Alt 提交。返回 true 表示按键已被消费——调用方在激活
 * 期间应当无条件吞掉按键，不能让它漏进焦点窗口（否则按下 Tab 会在编辑器
 * 里插一个制表符）。
 */
bool woodland_window_switcher_handle_key(struct woodland_server *server, xkb_keysym_t sym,
                                         uint32_t state, uint32_t modifiers);

/** 收起面板且不改焦点（Escape、候选全部消失时用）。 */
void woodland_window_switcher_cancel(struct woodland_server *server);

/**
 * 自检用：把候选快照拷出来（只复制指针，不转移所有权）。
 *
 * out 至少 max 个槽位；返回实际拷出的个数，*selected 收到当前高亮下标
 * （未激活时为 -1，此时返回 0）。给 self_check 验证"快照里每一项都还在
 * server->views 里、且仍然是个合法目标"用——漏挂 view_unmapped 监听会在这里显形。
 */
int woodland_window_switcher_snapshot(const struct woodland_server *server,
                                      struct woodland_view *out[], int max, int *selected);

/** 导出状态（wctl get_tree 的 window_switcher 对象）。 */
void woodland_window_switcher_status(struct woodland_server *server, struct woodland_json *out);

/* ==================== 纯逻辑（单测直接调，不建 display） ==================== */

/** 循环取下一个下标：from 越界时按方向取首/尾，count<=0 返回 -1。 */
int woodland_switcher_next_index(int count, int from, bool forward);

/**
 * 打开时的预选下标：origin 在列表里就沿方向走一格，否则取首/尾。
 * 这就是"Alt+Tab 先切到上一个窗口、Alt+Shift+Tab 切到最后一个"的算法。
 */
int woodland_switcher_initial_index(struct woodland_view *const *views, int count,
                                    const struct woodland_view *origin, bool forward);

#endif /* WOODLAND_WINDOW_SWITCHER_H */
