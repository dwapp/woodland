// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PLACEMENT_H
#define WOODLAND_PLACEMENT_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "ssd.h"

struct woodland_server;
struct woodland_view;
struct woodland_output;
struct wlr_output;
struct wlr_output_layout;

/* 贴边吸附阈值（px）：外框边缘距目标线不超过该值即吸附（labwc
 * screen_edge_strength 默认 20）。 */
#define WOODLAND_SNAP_DISTANCE 20
/* 层叠步进（labwc placement cascadeOffset 默认 40/30，这里沿用更紧的 32） */
#define WOODLAND_CASCADE_STEP 32
/* 层叠满一圈后回到起点，避免窗口被挤到屏幕角落 */
#define WOODLAND_CASCADE_WRAP 8

/**
 * 新窗口放置策略（对齐 labwc <placement><policy>）：
 *
 *   cascade   层叠递进（labwc 默认），逐个偏移避免完全遮挡
 *   center    工作区居中
 *   cursor    左上角跟在光标处（越界则拉回工作区）
 *   autoplace 扫描候选位置，选与其它窗口重叠面积最小的一处
 */
enum woodland_placement_policy {
    WOODLAND_PLACE_CASCADE = 0,
    WOODLAND_PLACE_CENTER,
    WOODLAND_PLACE_CURSOR,
    WOODLAND_PLACE_AUTOPLACE,
};

const char *woodland_placement_policy_name(enum woodland_placement_policy policy);
bool woodland_placement_policy_parse(const char *name,
                                     enum woodland_placement_policy *out);

/* ==================== 输出与工作区 ==================== */

/**
 * 点 (lx,ly) 所在的输出；不在任何输出上时返回 NULL。
 */
struct woodland_output *woodland_output_at(struct woodland_server *server,
                                           double lx, double ly);

/** 输出链表的第一个（单输出场景与"主输出"一致）。 */
struct woodland_output *woodland_output_first(struct woodland_server *server);

/**
 * 离 (lx,ly) 最近的输出（点不在任何输出上时用：输出之间的空隙、
 * 窗口被拖到输出之外）。没有输出时返回 NULL。
 */
struct woodland_output *woodland_output_nearest(struct woodland_server *server,
                                                double lx, double ly);

/**
 * 视图所在的输出。
 *
 * 先按外框中心命中；窗口被拖到输出之间的空隙时退化为"最近的输出"，
 * 都不行再退回首输出——保证永远有可用的工作区，不会出现零尺寸。
 */
struct woodland_output *woodland_output_for_view(struct woodland_view *view);

/** 输出的可用区域（布局坐标）：layout box 叠加 layer-shell 让出的 exclusive zone。 */
void woodland_output_workarea(struct woodland_output *output, struct wlr_box *out);

/** 布局坐标处的输出可用区域；没有输出时填零矩形。 */
void woodland_workarea_at(struct woodland_server *server, double lx, double ly,
                          struct wlr_box *out);

/** 最大化几何（布局坐标，可指定输出；output 为 NULL 时取首输出）。 */
struct wlr_box woodland_view_maximized_geometry_on(struct woodland_server *server,
                                              struct woodland_output *output);

/** 最大化几何（首输出，兼容旧接口与单测）。 */
struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server);

/** 全屏几何（指定输出的完整 box，布局坐标；不吃 usable_area）。 */
struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output);

/* ==================== 外框与约束 ==================== */

/** 视图外框（内容 + SSD 边距），布局坐标。 */
void woodland_view_outer_box(const struct woodland_view *view, struct wlr_box *out);

/**
 * 把外框整体约束进 area（纯函数）。
 *
 * 窗口比 area 还大时贴左上——与 labwc 一致：宁可右侧/底部被裁，
 * 也保证标题栏和左上角可见可抓。area 为空时原样返回。
 */
void woodland_box_constrain(struct wlr_box *box, const struct wlr_box *area);

/**
 * 把视图拉回所在输出的工作区（拖动/输出变更/布局切换后调用）。
 *
 * 只摆位置不改尺寸：尺寸由客户端决定，硬改会让窗口在拖动中不断
 * 触发 configure 抖动。
 */
void woodland_view_constrain(struct woodland_view *view);

/* ==================== 放置几何（纯函数） ==================== */

/**
 * 计算新窗口的内容原点（布局坐标）。
 *
 * area 为可用工作区，content_w/h 为客户端内容尺寸，margins 为 SSD 外扩，
 * cascade 为层叠偏移像素。外框（内容 + margins）整体落在 area 内，
 * 这样标题栏不会被输出边缘裁掉。
 */
void woodland_view_placement(const struct wlr_box *area,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             int cascade, struct wlr_box *out);

/** 工作区内居中（外框居中）。 */
void woodland_placement_center(const struct wlr_box *area,
                               int content_w, int content_h,
                               const struct woodland_ssd_margins *margins,
                               struct wlr_box *out);

/** 左上角跟随光标（外框左上角放在光标处，再整体约束进 area）。 */
void woodland_placement_cursor(const struct wlr_box *area,
                               double cursor_x, double cursor_y,
                               int content_w, int content_h,
                               const struct woodland_ssd_margins *margins,
                               struct wlr_box *out);

/**
 * 自动放置：在候选位置里选与 obstacles 重叠面积最小的一处（纯函数）。
 *
 * 候选集 = 工作区四角 + 层叠位 + 每个障碍窗口外框的四条外沿贴靠位。
 * 这是 labwc placement_find_best（重叠网格卷积）的轻量版：候选从 O(网格)
 * 降到 O(窗口数)，在常见的 1~4 个窗口场景下结论基本一致。
 *
 * obstacles/n 为其它窗口的外框（布局坐标），可为 NULL/0。
 */
void woodland_placement_autoplace(const struct wlr_box *area,
                                  int content_w, int content_h,
                                  const struct woodland_ssd_margins *margins,
                                  const struct wlr_box *obstacles, int n_obstacles,
                                  int cascade, struct wlr_box *out);

/* ==================== 贴边铺满 ==================== */

/**
 * 拖动中光标所处边缘触发的贴边模式（bitmask，WLR_EDGE_* 语义，纯函数）。
 *
 * 参照 gxde-wlcom 语义：
 *   光标靠近上沿（阈值内）       → 最大化 (WLR_EDGE_TOP)
 *   光标靠近左/右沿上 25% 区域   → 上角 1/4 屏 (LEFT|TOP 或 RIGHT|TOP)
 *   光标靠近左/右沿下 25% 区域   → 下角 1/4 屏 (LEFT|BOTTOM 或 RIGHT|BOTTOM)
 *   光标靠近左/右沿中间 50% 区域 → 半屏 (LEFT 或 RIGHT)
 */
uint32_t woodland_snap_cursor_edges(const struct wlr_box *area,
                                    double cursor_x, double cursor_y,
                                    int threshold);

/**
 * 该输出边是否落在外**输出布局**的外边界上（纯函数）。
 *
 * 输出之间的接缝在几何上也是一条边，但它外面还有别的显示器：贴到那儿
 * 只是"路过"，预览要防抖；布局外沿才是真的贴边，可以立刻显示。
 * layout 或 output 为空（拿不到布局/输出）时返回 true：按外边界处理。
 */
bool woodland_snap_edge_is_layout_edge(struct wlr_output_layout *layout,
                                       struct wlr_output *output, uint32_t edge);

/**
 * 贴边铺满的目标外框（布局坐标，写入 out_outer）。
 *
 *   LEFT / RIGHT        半屏（纵向占满工作区）
 *   TOP                 最大化（labwc snapToEdge 上边语义）
 *   LEFT|TOP 等两两组合  四分之一屏
 *
 * 返回 true 表示该边组合支持铺满。
 */
bool woodland_snap_tile_box(const struct wlr_box *area, uint32_t edges,
                            struct wlr_box *out_outer);

/**
 * transient 相对父窗口居中（纯函数，语义同 labwc view_compute_centered_position）。
 *
 * 外框（内容 + margins）以 parent_box 为中心；越出 area 时贴回 area 内侧，
 * 中心点也落不进 area 时贴左上。out 为内容原点。
 */
void woodland_view_center_on(const struct wlr_box *parent_box,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             const struct wlr_box *area, struct wlr_box *out);

/** 单轴贴边吸附（纯函数），见 view.h 历史语义。 */
int woodland_snap_axis(int desired, int size, int lo, int hi,
                       const int *edges, int n_edges, int threshold);

/* ==================== 首次放置与尺寸限制 ==================== */

/** 判断视图是否处于浮动状态（非最大化、非全屏、非贴边平铺）。 */
bool woodland_view_is_floating(const struct woodland_view *view);

/** 首次放置：按策略摆位置（transient 恒为相对父窗口居中）。 */
void woodland_view_place_initial(struct woodland_view *view);

/**
 * 按"外框"应用几何：内容 = 外框减去 SSD 边距，内容尺寸下发给客户端。
 *
 * 最大化/全屏/贴边给的都是外框，装饰占用外框顶部，内容必须让位，
 * 否则标题栏会被推到输出边界外面。
 */
void woodland_view_apply_outer_geometry(struct woodland_view *view,
                                        const struct wlr_box *box);

/**
 * 把视图贴到指定的工作区边（左/右半屏、上边最大化、四角 1/4）。
 *
 * 会记录贴边前的外框，供再次拖动时恢复（见 view_unsnap_for_drag）。
 * 返回 false 表示该边组合不支持/窗口当前状态不允许。
 */
bool woodland_view_snap_to(struct woodland_view *view, uint32_t edges);
/**
 * 把视图贴到指定输出的指定边（左/右半屏、上边最大化、四角 1/4）。
 * output 为 NULL 时自动取视图当前所在输出。
 */
bool woodland_view_snap_to_output(struct woodland_view *view, uint32_t edges,
                                  struct woodland_output *output);

/** 贴边铺满预览状态序列化（供 wctl get_tree）。 */
struct woodland_json;
void woodland_snap_preview_status(struct woodland_server *server, struct woodland_json *out);
void woodland_snap_preview_init(struct woodland_server *server);
void woodland_snap_preview_finish(struct woodland_server *server);
void woodland_snap_preview_show(struct woodland_server *server, struct woodland_view *view,
                                struct woodland_output *output, uint32_t edges);
void woodland_snap_preview_hide(struct woodland_server *server);
void woodland_snap_preview_destroy(struct woodland_server *server);
void woodland_snap_preview_update(struct woodland_server *server, struct woodland_view *view);
void shell_view_apply_snap(struct woodland_view *view);

/**
 * "松手会铺满哪条边"的判定（纯函数，入参是几何量而非 server）。
 *
 * **只看光标**：光标落进工作区边的阈值内才算，跟窗口外框贴不贴边无关——
 * 窗口被拖到贴边、光标还在标题栏中间时不算，那只是挪窗口。
 * 全屏、尺寸被客户端钉死这两种情况返回 0。
 */
uint32_t woodland_snap_determine_edges(struct woodland_view *view,
                                       const struct wlr_box *area,
                                       double cursor_x, double cursor_y,
                                       int threshold);


/** 告诉客户端"当前被 tiled"（xdg 专用，X11 无对应语义）。 */
void woodland_view_set_tiled(struct woodland_view *view, uint32_t edges);

/**
 * 视图的尺寸上下限（客户端 hint）。
 *
 * xdg 取 current min/max size，X11 取 WM_NORMAL_HINTS（base size 视作最小）。
 * 0 表示该方向无限制；max 不会小于 min。
 */
void woodland_view_size_limits(const struct woodland_view *view,
                               int *min_w, int *min_h,
                               int *max_w, int *max_h);

#endif
