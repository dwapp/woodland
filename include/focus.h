// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_FOCUS_H
#define WOODLAND_FOCUS_H

#include <stdbool.h>

struct woodland_server;
struct woodland_view;
struct wlr_surface;

/**
 * 视图是否"可聚焦"：已映射、未最小化、不是 override-redirect、
 * 且属于当前工作区。
 *
 * 焦点回退、循环切换、命中测试都走这一个判断，避免各处条件漂移
 * （之前 layer.c 和 shell.c 各写了一份几乎一样的过滤）。
 */
bool woodland_view_focusable(const struct woodland_view *view);

/** 当前持有键盘焦点的视图；焦点在 layer surface 或没有焦点时返回 NULL。 */
struct woodland_view *woodland_focus_current(struct woodland_server *server);

/**
 * 聚焦视图（统一入口，替代旧的 woodland_view_focus）。
 *
 * 依次处理：最小化则先恢复（重 map 会再次进来）、不在当前工作区则切过去、
 * 有模态子对话框则改为聚焦对话框、必要时置顶（z 序 + views 链表头）、
 * 同步 activated / SSD 激活色 / foreign-toplevel 状态、下发键盘 enter。
 *
 * raise=false 用于"只给焦点不改层叠"（如从 layer 退回、焦点回退）。
 */
void woodland_focus_view(struct woodland_view *view, bool raise);

/**
 * 焦点回退：挑当前工作区 z 序最上的可聚焦视图；没有就清焦点。
 *
 * 窗口关闭/最小化/unmap、工作区切换、layer 放弃键盘时都调这个，
 * 避免"焦点悬空导致键盘输入丢失"。
 */
void woodland_focus_fallback(struct woodland_server *server);

/** 清掉 toplevel 焦点（不挑新目标），layer 焦点同时放弃。 */
void woodland_focus_none(struct woodland_server *server);

/**
 * 焦点回退目标：当前工作区 z 序最上的可聚焦视图（无则 NULL）。
 *
 * 抽出来是为了可测：woodland_focus_fallback() 会真的下发键盘 enter，
 * 单测里没有 surface 就观测不到，选择逻辑单独成一个纯查询。
 */
struct woodland_view *woodland_focus_topmost(struct woodland_server *server);

/**
 * 循环切换的目标：沿 z 序取 from 的下一个（forward）或上一个视图。
 *
 * from 不在候选集里时取第一个（forward）或最后一个（backward）。
 * 没有其它可聚焦视图时返回 NULL。
 */
struct woodland_view *woodland_focus_pick(struct woodland_server *server,
                                          struct woodland_view *from,
                                          bool forward);

/**
 * 焦点循环（Alt+Tab / Alt+Shift+Tab）。
 *
 * forward=true 沿 z 序往下（下一个更靠后的窗口），false 往上。
 * 只有当前工作区的可见窗口参与；不足两个时保持不动。
 */
void woodland_focus_cycle(struct woodland_server *server, bool forward);

/**
 * toplevel 把键盘让给 layer surface 时调用：清 activated、SSD 激活色、
 * foreign-toplevel 激活态。
 */
void woodland_focus_yield_to_layer(struct woodland_server *server);

/** 沿 parent 关系找该视图所属的模态对话框（含自己）；没有则返回 view。 */
struct woodland_view *woodland_focus_modal_dialog(struct woodland_view *view);

#endif
