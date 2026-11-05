// SPDX-License-Identifier: GPL-2.0-only
/*
 * 窗口效果：圆角与背景模糊。
 *
 * 这两件事在 wlr_scene 里都做不了（它只会画直角、不采样背景），所以真正的
 * 像素工作交给 scenefx：圆角是 buffer/rect 节点的 corner radius，模糊是一个
 * 画在内容之前的 blur 节点。本文件只负责"什么时候用多大半径、糊哪一块"，
 * 也就是把协议/配置里的意图翻译成 scenefx 的节点参数。
 *
 * 渲染能力是固定的（scenefx 常开），画不画由运行期配置决定：[effects] enabled
 * 是总开关，blur 单独管模糊，corner_radius 是默认半径。关掉的时候状态照样记录
 * （get_tree 如实暴露客户端要了什么），只是不建节点——"记不记"和"画不画"仍然
 * 分开，排查时不用猜是协议没到还是没画。
 *
 * 圆角叠加规则（见 woodland_effect_corners）：
 *   有标题栏 → 上沿被标题栏盖住，内容只圆下角，上角由 SSD 的矩形自己圆；
 *   无标题栏 / CSD → 四角都圆；
 *   最大化 / 全屏 → 直角（否则屏幕边缘会透出后面的画面）。
 *
 * 模糊单独由客户端经 ext-background-effect 声明（view 与 layer surface 都有，
 * 见 woodland_effect_blur_apply）；"用哪个 surface 做遮罩"这类判断也在这里，
 * 协议层只管交一个 surface 局部坐标的 region。
 */
#include <string.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "layer.h"
#include "scene.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

/* ==================== 运行期开关 ==================== */

bool woodland_effect_server_enabled(const struct woodland_server *server)
{
    return server && server->config && server->config->effects.enabled;
}

bool woodland_effect_server_blur_enabled(const struct woodland_server *server)
{
    /* 模糊挂在总开关下面：enabled=false 时 blur=true 也不该生效 */
    return woodland_effect_server_enabled(server) && server->config->effects.blur;
}

/* ==================== 状态与换算（纯函数 / 不碰节点） ==================== */

int woodland_effect_corner_radius(const struct woodland_view *view)
{
    if (!view || !woodland_effect_server_enabled(view->server)) {
        return 0;
    }
    int radius = 0;
    if (view->ssd_overrides.corner_radius_set) {
        /* 客户端接管：协议允许合成器 clamp，这里按 SSD 的几何上界收一下 */
        radius = view->ssd_overrides.corner_radius;
    } else if (view->server && view->server->config) {
        /* 半径只有这一个来源；[decorations] 里不再有同名开关，免得两处打架 */
        radius = view->server->config->effects.corner_radius;
    }
    if (radius < 0) {
        radius = 0;
    }
    if (radius > WOODLAND_SSD_MAX_SIZE) {
        radius = WOODLAND_SSD_MAX_SIZE;
    }
    return radius;
}

void woodland_effect_corners(const struct woodland_view *view, struct woodland_effect_corners *out)
{
    if (!out) {
        return;
    }
    int radius = woodland_effect_corner_radius(view);
    /* 铺满屏幕时不圆角：圆角会在屏幕边缘露出后面的内容 */
    if (!view || view->maximized || view->fullscreen) {
        radius = 0;
    }

    if (woodland_ssd_titlebar_visible(view)) {
        /* 标题栏盖住内容上沿：上角归 SSD，内容只露下角 */
        out->top_left = 0;
        out->top_right = 0;
        out->bottom_right = radius;
        out->bottom_left = radius;
    } else {
        out->top_left = radius;
        out->top_right = radius;
        out->bottom_right = radius;
        out->bottom_left = radius;
    }
}

void woodland_effect_buffer_corners(const struct woodland_effect_corners *window_corners, int x,
                                    int y, int width, int height, int window_width,
                                    int window_height, struct woodland_effect_corners *out)
{
    if (!out) {
        return;
    }
    struct woodland_effect_corners none = {0};
    if (!window_corners || width <= 0 || height <= 0) {
        *out = none;
        return;
    }
    /* 贴边算"<=/>="，subsurface 可以比父 surface 稍微越界 */
    bool at_left = x <= 0;
    bool at_top = y <= 0;
    bool at_right = x + width >= window_width;
    bool at_bottom = y + height >= window_height;

    out->top_left = (at_left && at_top) ? window_corners->top_left : 0;
    out->top_right = (at_right && at_top) ? window_corners->top_right : 0;
    out->bottom_right = (at_right && at_bottom) ? window_corners->bottom_right : 0;
    out->bottom_left = (at_left && at_bottom) ? window_corners->bottom_left : 0;
}

bool woodland_effect_blur_box(const pixman_region32_t *region, int width, int height,
                              struct wlr_box *out)
{
    if (!region || !out || width <= 0 || height <= 0) {
        return false;
    }
    if (pixman_region32_empty((pixman_region32_t *)region)) {
        return false;
    }
    pixman_box32_t *extents = pixman_region32_extents((pixman_region32_t *)region);
    struct wlr_box box = {
        .x = extents->x1,
        .y = extents->y1,
        .width = extents->x2 - extents->x1,
        .height = extents->y2 - extents->y1,
    };
    struct wlr_box surface_box = {.x = 0, .y = 0, .width = width, .height = height};
    if (!wlr_box_intersection(&box, &box, &surface_box)) {
        return false;
    }
    *out = box;
    return true;
}

bool woodland_effect_blur_visible(const struct woodland_view *view)
{
    return view && view->effect.blur;
}

void woodland_effect_set_blur_region(struct woodland_view *view, const pixman_region32_t *region,
                                     int width, int height)
{
    if (!view) {
        return;
    }
    struct wlr_box box = {0};
    if (region && woodland_effect_blur_box(region, width, height, &box)) {
        view->effect.blur = true;
        view->effect.blur_region = box;
    } else {
        view->effect.blur = false;
        view->effect.blur_region = (struct wlr_box){0};
    }
    woodland_effect_update(view);
}


/* ==================== scenefx 落地 ==================== */

/*
 * 这个 surface 是不是 root 自己，或者它的（可多层嵌套的）subsurface。
 * 圆角要按 buffer 算，就得先确认哪些 buffer 属于这个窗口——同一棵树里
 * 还可能有 popup 之类的别人的东西。
 */
static bool surface_belongs_to(struct wlr_surface *surface, struct wlr_surface *root)
{
    for (int depth = 0; surface && depth < 4; depth++) {
        if (surface == root) {
            return true;
        }
        struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
        if (!sub) {
            return false;
        }
        surface = sub->parent;
    }
    return false;
}

/*
 * 内容 surface 在 scene 树里的那个 buffer 节点（xdg 与 x11 布局不同，按 surface 认）。
 * 顺带把它的绝对坐标带出去：blur 节点要贴在 buffer 上，而 buffer->node.x 只是
 * 相对父节点的偏移，CSD 客户端（geometry 与 surface 原点不同）照着摆会偏。
 */
static struct wlr_scene_buffer *find_surface_buffer_at(struct wlr_scene_node *node,
                                                       struct wlr_surface *surface, int base_x,
                                                       int base_y, int *out_x, int *out_y)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (scene_surface && scene_surface->surface == surface) {
            if (out_x) {
                *out_x = base_x + node->x;
            }
            if (out_y) {
                *out_y = base_y + node->y;
            }
            return buffer;
        }
        return NULL;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return NULL;
    }
    struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        struct wlr_scene_buffer *found = find_surface_buffer_at(
            child, surface, base_x + node->x, base_y + node->y, out_x, out_y);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/*
 * 在 tree 里找内容 buffer，坐标相对于 tree 自身（不是相对场景根）。
 *
 * 从"tree 的子节点"开始走而不是从 tree 本身：tree 自己的 x/y 就是这个坐标系
 * 的原点，把它加进去会让 blur 节点整体偏移一个标题栏（曾经真的这么偏过）。
 */
static struct wlr_scene_buffer *find_surface_buffer(struct wlr_scene_tree *tree,
                                                    struct wlr_surface *surface, int *out_x,
                                                    int *out_y)
{
    if (!tree) {
        return NULL;
    }
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        struct wlr_scene_buffer *found = find_surface_buffer_at(child, surface, 0, 0, out_x, out_y);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static struct wlr_scene_blur *find_blur_node(struct wlr_scene_tree *tree)
{
    if (!tree) {
        return NULL;
    }
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link)
    {
        if (node->type == WLR_SCENE_NODE_BLUR) {
            return wlr_scene_blur_from_node(node);
        }
    }
    return NULL;
}

static struct fx_corner_radii to_fx(const struct woodland_effect_corners *corners)
{
    return corner_radii_new(corners->top_left, corners->top_right, corners->bottom_right,
                            corners->bottom_left);
}

/*
 * 逐 buffer 下发圆角。父 surface 的 buffer 通常占满窗口，四个角都对得上；
 * subsurface（视频、输入法、自带子窗口的客户端）画在它上面，如果原样留直角，
 * 窗口边缘就会露出一块尖角——所以每个 buffer 只圆自己贴边的那几个角。
 */
static void apply_corners_at(struct wlr_scene_node *node, struct wlr_surface *root, int base_x,
                             int base_y, int width, int height,
                             const struct woodland_effect_corners *corners)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (!scene_surface || !surface_belongs_to(scene_surface->surface, root)) {
            return;
        }
        struct woodland_effect_corners per_buffer;
        woodland_effect_buffer_corners(corners, base_x + node->x, base_y + node->y,
                                       scene_surface->surface->current.width,
                                       scene_surface->surface->current.height, width, height,
                                       &per_buffer);
        wlr_scene_buffer_set_corner_radii(buffer, to_fx(&per_buffer));
        return;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return;
    }
    struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        apply_corners_at(child, root, base_x + node->x, base_y + node->y, width, height, corners);
    }
}

/* 同样以 tree 自身的原点为基准：半径判定要用"相对窗口"的坐标 */
static void apply_corners_in_tree(struct wlr_scene_tree *tree, struct wlr_surface *root, int width,
                                  int height, const struct woodland_effect_corners *corners)
{
    if (!tree || !root) {
        return;
    }
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        apply_corners_at(child, root, 0, 0, width, height, corners);
    }
}

static void update_corners(struct woodland_view *view)
{
    if (!view->scene_tree) {
        return;
    }
    struct woodland_effect_corners corners;
    woodland_effect_corners(view, &corners);

    struct wlr_surface *surface = woodland_view_surface(view);
    if (surface) {
        apply_corners_in_tree(view->scene_tree, surface, surface->current.width,
                              surface->current.height, &corners);
    }

    /*
     * SSD 那几块矩形用的是"外框半径"：和内容的不一样——有标题栏时内容的上角
     * 是 0（被盖住），标题栏的上角却是圆的。铺满屏幕时外框也要直角。
     */
    int frame_radius = woodland_effect_corner_radius(view);
    if (view->maximized || view->fullscreen) {
        frame_radius = 0;
    }
    woodland_ssd_set_corner_radius(view, frame_radius);
}

static void update_blur(struct woodland_view *view)
{
    if (!view->scene_tree) {
        return;
    }
    if (!view->effect.blur || !woodland_effect_server_blur_enabled(view->server)) {
        woodland_effect_blur_clear(view->scene_tree);
        return;
    }

    struct wlr_box content;
    woodland_view_content_box(view, &content);
    if (content.width <= 0 || content.height <= 0) {
        return;
    }

    struct wlr_surface *surface = woodland_view_surface(view);
    int origin_x = 0;
    int origin_y = 0;
    if (surface) {
        /* 只为了把 node 的绝对坐标带出来；找不到 buffer 就退回 (0,0) */
        find_surface_buffer(view->scene_tree, surface, &origin_x, &origin_y);
    }

    struct woodland_effect_corners corners;
    woodland_effect_corners(view, &corners);

    woodland_effect_blur_apply(view->scene_tree, surface, origin_x, origin_y, content.width,
                               content.height, &view->effect.blur_region, &corners);
}

void woodland_effect_blur_clear(struct wlr_scene_tree *tree)
{
    struct wlr_scene_blur *blur = find_blur_node(tree);
    if (blur) {
        wlr_scene_node_set_enabled(&blur->node, false);
    }
}

bool woodland_effect_blur_apply(struct wlr_scene_tree *tree, struct wlr_surface *surface, int x,
                                int y, int width, int height, const struct wlr_box *region,
                                const struct woodland_effect_corners *corners)
{
    if (!tree) {
        return false;
    }
    struct wlr_scene_blur *blur = find_blur_node(tree);
    if (!region || width <= 0 || height <= 0) {
        if (blur) {
            wlr_scene_node_set_enabled(&blur->node, false);
        }
        return false;
    }

    if (!blur) {
        blur = wlr_scene_blur_create(tree, width, height);
        if (!blur) {
            wlr_log(WLR_ERROR, "failed to create blur node");
            return false;
        }
        /* 必须画在内容之前：blur 采样的是"已经画上去的东西" */
        wlr_scene_node_lower_to_bottom(&blur->node);
    }
    wlr_scene_node_set_position(&blur->node, x, y);
    wlr_scene_blur_set_size(blur, width, height);

    if (corners) {
        wlr_scene_blur_set_corner_radii(blur, to_fx(corners));
    } else {
        wlr_scene_blur_set_corner_radii(blur, corner_radii_none());
    }

    /*
     * region 是 surface 局部坐标，blur 节点也贴在 surface 原点上，两者同一个
     * 坐标系，所以直接照搬——别再减 content 的原点，那会把区域整体推偏。
     * 盖满整个内容时给空裁剪区（scenefx 的语义：空 = 不裁剪）。
     */
    struct clipped_region clip = clipped_region_get_default();
    struct wlr_box full = {.x = 0, .y = 0, .width = width, .height = height};
    if (!wlr_box_equal(region, &full)) {
        clip.area = *region;
    }
    wlr_scene_blur_set_clipped_region(blur, clip);

    /*
     * 只在客户端真的画了东西的地方糊：遮罩用内容 buffer 的 alpha，scenefx 据此
     * 打一张 stencil（完全透明的像素不画模糊）。
     *
     * 区域是客户端自己声明的，但声明得比画得大很常见——CSD 窗口的可视范围里
     * 有一圈是"阴影预留"的全透明边，整个 surface 声明成 blur 区域时，那一圈
     * 会在窗口外面糊出一圈毛边。没有遮罩就只能靠客户端自己算准。
     */
    struct wlr_scene_buffer *mask = NULL;
    if (surface) {
        mask = find_surface_buffer(tree, surface, NULL, NULL);
    }
    wlr_scene_blur_set_transparency_mask_source(blur, mask);

    wlr_scene_node_set_enabled(&blur->node, true);
    return true;
}

void woodland_effect_layer_blur(struct woodland_layer_surface *layer, const struct wlr_box *region)
{
    if (!layer || !layer->scene_layer_surface || !layer->layer_surface) {
        return;
    }
    struct wlr_scene_tree *tree = layer->scene_layer_surface->tree;
    struct wlr_surface *surface = layer->layer_surface->surface;
    if (!region || !woodland_effect_server_blur_enabled(layer->server)) {
        woodland_effect_blur_clear(tree);
        return;
    }
    /* layer surface 没有装饰：从 (0,0) 起铺满自己，四角直角 */
    woodland_effect_blur_apply(tree, surface, 0, 0, surface->current.width,
                               surface->current.height, region, NULL);
}

void woodland_effect_update(struct woodland_view *view)
{
    if (!view || !view->mapped) {
        /* map 之前没有内容可画；map 之后 ssd_update/commit 会补一次 */
        return;
    }
    update_corners(view);
    update_blur(view);
}
