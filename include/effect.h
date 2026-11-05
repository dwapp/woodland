// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_EFFECT_H
#define WOODLAND_EFFECT_H

#include <pixman.h>
#include <stdbool.h>
#include <wlr/util/box.h>

struct woodland_layer_surface;
struct woodland_view;
struct wlr_scene_tree;
struct wlr_surface;

/*
 * 四角圆角半径。顺序与 scenefx 的 fx_corner_radii 一致（顺时针），
 * 但这里只依赖 pixman/wlr_box，默认构建（没有 scenefx）也能编。
 */
struct woodland_effect_corners {
    int top_left;
    int top_right;
    int bottom_right;
    int bottom_left;
};

/*
 * 每个视图的效果状态（按值内嵌在 struct woodland_view 里）。
 *
 * 和 ssd_overrides 同一个理由：客户端在 map 之前就能设 blur region，而
 * scenefx 节点要到 map 之后才存在。协议层写这里，渲染端读这里，谁都不需要
 * 反过来问对方要数据。
 */
struct woodland_effect_state {
    bool blur;               /* 已提交的 blur region 非空 */
    struct wlr_box blur_region; /* surface 局部坐标下的包围盒（已裁到 surface 尺寸） */
};

/**
 * 当前生效的圆角半径（0 = 直角）。
 *
 * 优先级：客户端的 treeland-decoration 覆盖 > [effects] corner_radius。
 */
int woodland_effect_corner_radius(const struct woodland_view *view);

/**
 * 单个 buffer 在这一块窗口里该用哪几个角（纯函数）。
 *
 * 一个窗口不止一块 buffer：客户端用 subsurface 放视频、放输入法、放菜单时，
 * 那些 buffer 画在内容之上，会把父 surface 的圆角重新盖成直角——右下角有
 * 一块全屏大小的 subsurface，窗口右下就露一个尖角。所以每个 buffer 单独算：
 * 只有它自己贴到窗口的那条边上的角才跟着圆，中间的 buffer 一律直角。
 *
 * x/y/width/height 是这个 buffer 在窗口坐标系里的位置与尺寸，
 * window_width/window_height 是窗口（surface）尺寸。越界按贴边算。
 */
void woodland_effect_buffer_corners(const struct woodland_effect_corners *window_corners, int x,
                                    int y, int width, int height, int window_width,
                                    int window_height, struct woodland_effect_corners *out);

/**
 * 给任意 scene tree 挂/更新一块模糊（view 与 layer surface 共用，见 effect.c）。
 *
 * x/y/width/height 是 blur 节点在 tree 里的位置与尺寸，region 是 surface 局部
 * 坐标的裁剪区（NULL = 关掉模糊），corners 为 NULL 表示直角。返回是否真的
 * 画了（默认构建里恒为 false）。
 */
bool woodland_effect_blur_apply(struct wlr_scene_tree *tree, struct wlr_surface *surface, int x,
                                int y, int width, int height, const struct wlr_box *region,
                                const struct woodland_effect_corners *corners);

/** 关掉 tree 上那块模糊（节点留着，下次不用重建）。 */
void woodland_effect_blur_clear(struct wlr_scene_tree *tree);

/**
 * layer surface 的模糊（面板、Dock、启动器这些没有装饰的 surface）。
 *
 * region 是 surface 局部坐标的包围盒，NULL = 撤掉。layer surface 没有标题栏，
 * 也不圆角，所以这里只处理"糊哪一块"。
 */
void woodland_effect_layer_blur(struct woodland_layer_surface *layer, const struct wlr_box *region);

/**
 * 把半径摊到四个角上（纯函数，单测直接喂参数）。
 *
 * 有标题栏时上沿被标题栏盖住，内容只露下沿，所以上角交给 SSD、内容只圆下角；
 * 最大化/全屏铺满时一律直角——圆角会在屏幕边缘露出后面的画面。
 */
void woodland_effect_corners(const struct woodland_view *view,
                             struct woodland_effect_corners *out);

/**
 * blur region → 可画的包围盒（纯函数）。
 *
 * 协议允许多个矩形，scenefx 的裁剪区只有一个盒子，所以取包围盒；区域整体
 * 在 surface 外或为空时返回 false。out 已裁剪到 [0,0,width,height]。
 */
bool woodland_effect_blur_box(const pixman_region32_t *region, int width, int height,
                              struct wlr_box *out);

/** 这个视图当前是不是真的有模糊（没有对象/区域为空都是 false）。 */
bool woodland_effect_blur_visible(const struct woodland_view *view);

/**
 * 协议提交了新的 blur region（region 为 NULL 或空 = 撤掉模糊）。
 *
 * 只写 view->effect，顺带刷新渲染节点；宽度/高度是 surface 当前尺寸，用来
 * 把区域裁到 surface 内。由 ext-background-effect 的 commit 回调调用。
 */
void woodland_effect_set_blur_region(struct woodland_view *view, const pixman_region32_t *region,
                                     int width, int height);

/**
 * 重算一个视图的圆角与模糊。幂等，随时可调：
 * commit / SSD 重排 / map / 装饰覆盖变化之后都走这里。
 *
 * 没有编 effects 时只有状态在动，画面上什么都不会变——值照样记录在 view 上，
 * 由 get_tree 如实暴露。
 */
void woodland_effect_update(struct woodland_view *view);

#endif /* WOODLAND_EFFECT_H */
