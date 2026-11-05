// SPDX-License-Identifier: GPL-2.0-only
/*
 * 窗口效果：圆角与背景模糊。
 *
 * 这两件事在 wlr_scene 里都做不了（它只会画直角、不采样背景），所以真正的
 * 像素工作交给 scenefx：圆角是 buffer/rect 节点的 corner radius，模糊是一个
 * 画在内容之前的 blur 节点。本文件只负责"什么时候用多大半径、糊哪一块"，
 * 也就是把协议/配置里的意图翻译成 scenefx 的节点参数。
 *
 * 默认构建（-Deffects=disabled）里 WOODLAND_HAVE_EFFECTS=0，渲染部分整个编掉，
 * 只剩半径/区域的换算与状态记录。这样单测、wctl 和 trace 在两种构建下行为
 * 一致，差别只在"画不画"。
 *
 * 圆角叠加规则（见 woodland_effect_corners）：
 *   有标题栏 → 上沿被标题栏盖住，内容只圆下角，上角由 SSD 的矩形自己圆；
 *   无标题栏 / CSD → 四角都圆；
 *   最大化 / 全屏 → 直角（否则屏幕边缘会透出后面的画面）。
 */
#include <string.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "scene.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

/* ==================== 状态与换算（默认构建也编） ==================== */

int woodland_effect_corner_radius(const struct woodland_view *view)
{
    if (!view) {
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

#if WOODLAND_HAVE_EFFECTS

/* 内容 surface 在 scene 树里的那个 buffer 节点（xdg 与 x11 布局不同，按 surface 认） */
static struct wlr_scene_buffer *find_surface_buffer(struct wlr_scene_node *node,
                                                    struct wlr_surface *surface, int depth)
{
    if (depth > 3) {
        return NULL; /* xdg 树最深两层，再深就不是内容节点了 */
    }
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (scene_surface && scene_surface->surface == surface) {
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
        struct wlr_scene_buffer *found = find_surface_buffer(child, surface, depth + 1);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static struct wlr_scene_blur *find_blur_node(struct woodland_view *view)
{
    if (!view->scene_tree) {
        return NULL;
    }
    struct wlr_scene_node *node;
    wl_list_for_each(node, &view->scene_tree->children, link)
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

static void update_corners(struct woodland_view *view)
{
    struct woodland_effect_corners corners;
    woodland_effect_corners(view, &corners);

    struct wlr_surface *surface = woodland_view_surface(view);
    if (surface && view->scene_tree) {
        struct wlr_scene_buffer *buffer =
            find_surface_buffer(&view->scene_tree->node, surface, 0);
        if (buffer) {
            wlr_scene_buffer_set_corner_radii(buffer, to_fx(&corners));
        }
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
    struct wlr_scene_blur *blur = find_blur_node(view);
    if (!view->effect.blur || !view->scene_tree) {
        /* 没有区域：节点留着（下次再模糊不用重建），只是不画 */
        if (blur) {
            wlr_scene_node_set_enabled(&blur->node, false);
        }
        return;
    }

    struct wlr_box content;
    woodland_view_content_box(view, &content);
    if (content.width <= 0 || content.height <= 0) {
        return;
    }

    /*
     * 节点要贴在 surface 上，而不是 scene_tree 的原点：XDG 的 geometry 原点
     * 与 surface 原点可以不同（CSD 客户端会把阴影算进 geometry），拿 geometry
     * 的 x/y 当位置就会整体偏。直接问真正的 buffer 节点在哪最准。
     */
    int origin_x = 0;
    int origin_y = 0;
    struct wlr_surface *surface = woodland_view_surface(view);
    if (surface) {
        struct wlr_scene_buffer *buffer = find_surface_buffer(&view->scene_tree->node, surface, 0);
        if (buffer) {
            origin_x = buffer->node.x;
            origin_y = buffer->node.y;
        }
    }

    if (!blur) {
        blur = wlr_scene_blur_create(view->scene_tree, content.width, content.height);
        if (!blur) {
            wlr_log(WLR_ERROR, "failed to create blur node");
            return;
        }
        /* 必须画在内容之前：blur 采样的是"已经画上去的东西" */
        wlr_scene_node_lower_to_bottom(&blur->node);
    }
    wlr_scene_node_set_position(&blur->node, origin_x, origin_y);
    wlr_scene_blur_set_size(blur, content.width, content.height);

    struct woodland_effect_corners corners;
    woodland_effect_corners(view, &corners);
    wlr_scene_blur_set_corner_radii(blur, to_fx(&corners));

    /*
     * 协议区域是 surface 局部坐标，blur 节点也贴在 surface 原点上，两者同一个
     * 坐标系，所以直接照搬——别再减 content 的原点，那会把区域整体推偏。
     * 盖满整个内容时给空裁剪区（scenefx 的语义：空 = 不裁剪）。
     */
    struct clipped_region clip = clipped_region_get_default();
    struct wlr_box full = {.x = 0, .y = 0, .width = content.width, .height = content.height};
    if (!wlr_box_equal(&view->effect.blur_region, &full)) {
        clip.area = view->effect.blur_region;
    }
    wlr_scene_blur_set_clipped_region(blur, clip);
    wlr_scene_node_set_enabled(&blur->node, true);
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

#else /* !WOODLAND_HAVE_EFFECTS */

void woodland_effect_update(struct woodland_view *view)
{
    (void)view;
}

#endif
