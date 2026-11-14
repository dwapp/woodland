// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SSD_H
#define WOODLAND_SSD_H

#include <stdbool.h>
#include <stdint.h>

struct woodland_view;
struct woodland_server;
struct wlr_scene_tree;

/* SSD 几何常量（对齐 kylin-wayland-compositor 默认主题的密度：32 标题栏 + 32 按钮，
 * 与右键窗口菜单的视觉比例保持一致）。
 *
 * 按钮高度故意等于标题栏高度：按钮图标是**方形 buffer**（见 text_buffer.c 的
 * woodland_button_icon_buffer），dest_size 一旦不是方形就会把 □ / × 压扁。
 * 想给按钮留上下留白，得先让图标支持非方形，别只改这个宏。 */
#define WOODLAND_SSD_TITLEBAR_H 32
#define WOODLAND_SSD_BORDER_W 1
#define WOODLAND_SSD_BUTTON_W 32
#define WOODLAND_SSD_BUTTON_H 32
#define WOODLAND_SSD_BUTTON_GAP 0
#define WOODLAND_SSD_RESIZE_GRAB_W 8
#define WOODLAND_SSD_CORNER_W 16

/*
 * treeland-decoration 客户端能改的半径/宽度上界。
 *
 * 协议对越界值只要求"clamp 到支持的上界或忽略"，没有定死数字。取 1000 是
 * 照抄 treeland 的选择：它比任何真实窗口都大，又小到不会让几何计算溢出。
 */
#define WOODLAND_SSD_MAX_SIZE 1000

/** SSD 可命中区域类型（用于光标交互）。 */
enum woodland_ssd_part {
    WOODLAND_SSD_NONE = 0,
    WOODLAND_SSD_TITLEBAR,    /* 拖拽移动 */
    WOODLAND_SSD_BORDER_LEFT, /* 缩放左边缘 */
    WOODLAND_SSD_BORDER_RIGHT,
    WOODLAND_SSD_BORDER_TOP,
    WOODLAND_SSD_BORDER_BOTTOM,
    WOODLAND_SSD_BUTTON_CLOSE,
    WOODLAND_SSD_BUTTON_MAXIMIZE,
    WOODLAND_SSD_BUTTON_MINIMIZE,
};

struct woodland_ssd;

/*
 * treeland-decoration-unstable-v1 的每窗口覆盖值。
 *
 * 为什么存在 view 上而不是 SSD 里：客户端在 map 之前就会创建装饰上下文并
 * 下请求（xdg-decoration 协商也发生在 initial commit），而 SSD 对象要到 map
 * 才创建；边距计算（woodland_ssd_get_margins）更要在这之前就能用。放在 view
 * 上，SSD 创建时自然读到，未被覆盖的窗口只是一片零。
 *
 * 每个 *_set 表示"客户端接管了这个属性"；没 set 过的继续用配置默认（协议里
 * 的 opt-in 语义）。*_rgba 一律 0xRRGGBBAA —— 注意与配置里的 0xAARRGGBB
 * 不同：协议按 r,g,b,a 四个独立参数传，挨着协议写少一次脑内转置。
 */
struct woodland_ssd_overrides {
    bool managed; /* 有存活的装饰上下文（wctl get_tree 里可见） */
    bool titlebar_set;
    bool titlebar_hidden; /* titlebar_mode == hide */
    bool border_set;
    int border_width;     /* 0 = 不画边框 */
    uint32_t border_rgba; /* 0xRRGGBBAA */
    bool corner_radius_set;
    int corner_radius;
    bool shadow_set;
    int shadow_radius;
    int shadow_dx;
    int shadow_dy;
    uint32_t shadow_rgba; /* 0xRRGGBBAA */
};

/* SSD 占用的外边距（内容矩形之外的部分），语义同 labwc ssd_thickness() */
struct woodland_ssd_margins {
    int left;
    int top;
    int right;
    int bottom;
};

/** 创建 SSD 并挂到 view->scene_tree（lower_to_bottom）。 */
void woodland_ssd_create(struct woodland_view *view);

/**
 * 当前状态下 SSD 占用的外边距。
 *
 * 窗口布局（放置/最大化）必须把它算进去：内容原点 = 外框原点 + 边距。
 * 不依赖 SSD 是否已创建（map 之前就要能算），只看 deco_csd / maximized / fullscreen。
 */
void woodland_ssd_get_margins(struct woodland_view *view, struct woodland_ssd_margins *margins);

/** 销毁 SSD（清空 view->ssd）。 */
void woodland_ssd_destroy(struct woodland_view *view);

/** 重建/更新 SSD 几何（内容尺寸或状态变化后调用）。 */
void woodland_ssd_update(struct woodland_view *view);

/**
 * 装饰覆盖值变了一次，重排并刷新颜色。
 *
 * 与 woodland_ssd_update() 的差别只在"清状态"：覆盖值会改变标题栏是否存在、
 * 边框有多宽，旧的高亮/按下态可能指向已经消失的按钮，所以这里一并清掉。
 * 没有 SSD（未 map）时直接返回——覆盖值已经存在 view 上，map 时会读到。
 */
void woodland_ssd_deco_changed(struct woodland_view *view);

/** 切换激活态（颜色两态）。 */
void woodland_ssd_set_active(struct woodland_view *view, bool active);

/**
 * 设置按钮悬停态（part 为 WOODLAND_SSD_BUTTON_* 或 WOODLAND_SSD_NONE）。
 *
 * 非按钮区域（标题栏/边框）一律传 NONE：悬停高亮只作用于按钮。
 */
void woodland_ssd_set_hover(struct woodland_view *view, enum woodland_ssd_part part);

/**
 * 设置按钮按下态（part 为 WOODLAND_SSD_BUTTON_* 或 WOODLAND_SSD_NONE）。
 *
 * 按下态比悬停态更深一层；释放后由调用方清回 NONE。
 */
void woodland_ssd_set_pressed(struct woodland_view *view, enum woodland_ssd_part part);

/** 命中测试：返回 (lx,ly)（布局坐标）命中的 SSD 区域，并输出 resize edges。 */
enum woodland_ssd_part woodland_ssd_at(struct woodland_view *view, double lx, double ly,
                                       uint32_t *resize_edges);

/**
 * SSD 是否处于激活（标题栏激活色）状态；没有 SSD 时返回 false。
 *
 * 单独暴露是因为激活态分散在三处（客户端 activated、SSD 颜色、
 * foreign-toplevel 位），不变量检查要逐处核对，不能靠看像素。
 */
bool woodland_ssd_is_active(const struct woodland_view *view);

/**
 * SSD 内部缓存的内容尺寸；没有 SSD 时返回 false。
 *
 * 与当前内容尺寸不一致说明 ssd_update() 少调了一次（窗口拉宽后按钮
 * 还停在旧位置就是这么来的）。
 */
bool woodland_ssd_content_size(const struct woodland_view *view, int *width, int *height);

/** 返回 SSD 内部缓存的渲染缩放比例；没有 SSD 时返回 1.0f。 */
float woodland_ssd_get_scale(const struct woodland_view *view);

/** 释放 SSD 全局缓存与资源。 */
void woodland_ssd_finish(void);

/**
 * 用户级外观变了：重排这个窗口并丢掉图标缓存（下次 update 重新取）。
 *
 * 与 woodland_ssd_deco_changed 的差别只在图标与字体：那两样有缓存键/缓存串，
 * 不清就等于"改了没反应"。treeland-appearance-manager 的 set_font /
 * set_icon_theme / set_titlebar_height / set_color_scheme 都走这里。
 */
void woodland_ssd_refresh(struct woodland_view *view);

/** 对所有视图跑一遍 woodland_ssd_refresh（含未 map 的，它们只是空操作）。 */
void woodland_ssd_refresh_all(struct woodland_server *server);

/**
 * 标题栏当前是否可见（隐藏或者配置高度为 0 都算不可见）。
 *
 * 圆角要按它分上下沿：标题栏在时内容只露下沿，上沿归标题栏自己圆。
 */
bool woodland_ssd_titlebar_visible(const struct woodland_view *view);

/**
 * 设置 SSD 外框的内圆角半径（[effects] enabled 关掉时调用方已经折成 0）。
 *
 * 传进来的是**内轮廓**半径：内容 buffer 与标题栏按它裁角，描边外轮廓自动
 * 加一个边框厚度。实现是单个 frame rect + clipped_region 挖空内框——四条
 * 1px 细条拼方框时，半径会被细条尺寸 clamp 掉，角上只能留下直角。
 */
void woodland_ssd_set_corner_radius(struct woodland_view *view, int radius);
void woodland_ssd_set_corners(struct woodland_view *view, int top_left, int top_right,
                              int bottom_right, int bottom_left);

#endif /* WOODLAND_SSD_H */
