// SPDX-License-Identifier: GPL-2.0-only
/*
 * 窗口效果：圆角、背景模糊与窗口阴影。
 *
 * 像素级渲染交给 scenefx：
 *   - 圆角：buffer / rect / shadow 的 corner radius；
 *   - 模糊：内容下层的 blur 节点，按配置可走每输出预渲染的 optimized blur；
 *   - 阴影：最底层的 wlr_scene_shadow 节点，带外框挖空。
 *
 * 本文件负责意图翻译与几何计算，纯函数与渲染节点分离，易于单元测试。
 */
#include <string.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "layer.h"
#include "output.h"
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
    return woodland_effect_server_enabled(server) && server->config->effects.blur;
}

/* ==================== 状态与换算（纯函数 / 不碰节点） ==================== */

static bool view_has_csd_margins(const struct woodland_view *view)
{
    if (!view || view->ssd) {
        return false;
    }
    struct wlr_surface *surf = woodland_view_surface(view);
    if (!surf) {
        return false;
    }
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    return content.width > 0 && content.height > 0 &&
           (content.width < surf->current.width || content.height < surf->current.height ||
            content.x > 0 || content.y > 0);
}

int woodland_effect_corner_radius(const struct woodland_view *view)
{
    if (!view || !woodland_effect_server_enabled(view->server)) {
        return 0;
    }
    int radius = 0;
    if (view->ssd_overrides.corner_radius_set) {
        radius = view->ssd_overrides.corner_radius;
    } else if (view->server && view->server->config) {
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

void woodland_effect_frame_corners(const struct woodland_view *view,
                                   struct woodland_effect_corners *out)
{
    if (!out) {
        return;
    }
    int radius = woodland_effect_corner_radius(view);
    if (!view || view->maximized || view->fullscreen) {
        radius = 0;
    }

    out->top_left = radius;
    out->top_right = radius;
    out->bottom_right = radius;
    out->bottom_left = radius;

    if (!view || view->snapped_edges == 0) {
        return;
    }

    uint32_t edges = view->snapped_edges;
    if (edges & WLR_EDGE_LEFT) {
        out->top_left = 0;
        out->bottom_left = 0;
    }
    if (edges & WLR_EDGE_RIGHT) {
        out->top_right = 0;
        out->bottom_right = 0;
    }
    if (edges & WLR_EDGE_TOP) {
        out->top_left = 0;
        out->top_right = 0;
    }
    if (edges & WLR_EDGE_BOTTOM) {
        out->bottom_left = 0;
        out->bottom_right = 0;
    }
}

void woodland_effect_corners(const struct woodland_view *view, struct woodland_effect_corners *out)
{
    if (!out) {
        return;
    }
    woodland_effect_frame_corners(view, out);
    if (woodland_ssd_titlebar_visible(view)) {
        out->top_left = 0;
        out->top_right = 0;
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

bool woodland_effect_shadow_params(const struct woodland_view *view,
                                   struct woodland_effect_shadow *out)
{
    if (!out || !view || !woodland_effect_server_enabled(view->server)) {
        return false;
    }
    if (view->maximized || view->fullscreen || view->snapped_edges != 0) {
        return false;
    }

    /*
     * 颜色编码注意（提醒后来人）：
     * 1. 协议覆盖路径（view->ssd_overrides.shadow_rgba）为 0xRRGGBBAA（Wayland wire 格式）；
     * 2. 配置文件路径（cfg->effects.shadow_color）为 0xAARRGGBB（与 ssd.*_color 同格式）。
     * 两处分别按自身定义拆包并转为非预乘 RGBA float [0..1]。
     */
    if (view->ssd_overrides.shadow_set) {
        int r = view->ssd_overrides.shadow_radius;
        uint32_t rgba = view->ssd_overrides.shadow_rgba;
        uint8_t a = (uint8_t)(rgba & 0xFF);
        if (r <= 0 || a == 0) {
            return false;
        }
        out->radius = r > WOODLAND_SSD_MAX_SIZE ? WOODLAND_SSD_MAX_SIZE : r;
        out->offset_x = view->ssd_overrides.shadow_dx;
        out->offset_y = view->ssd_overrides.shadow_dy;
        out->color[0] = ((rgba >> 24) & 0xFF) / 255.0f;
        out->color[1] = ((rgba >> 16) & 0xFF) / 255.0f;
        out->color[2] = ((rgba >> 8) & 0xFF) / 255.0f;
        out->color[3] = a / 255.0f;
        return true;
    }

    const struct woodland_config *cfg = view->server ? view->server->config : NULL;
    if (!cfg || !cfg->effects.shadow) {
        return false;
    }
    if (view_has_csd_margins(view)) {
        return false;
    }
    int r = cfg->effects.shadow_radius;
    uint32_t c = cfg->effects.shadow_color;
    uint8_t a = (uint8_t)((c >> 24) & 0xFF);
    if (r <= 0 || a == 0) {
        return false;
    }
    out->radius = r > WOODLAND_SSD_MAX_SIZE ? WOODLAND_SSD_MAX_SIZE : r;
    out->offset_x = cfg->effects.shadow_offset_x;
    out->offset_y = cfg->effects.shadow_offset_y;

    float alpha = a / 255.0f;
    if (!view->activated) {
        alpha *= 0.6f;
    }
    out->color[0] = ((c >> 16) & 0xFF) / 255.0f;
    out->color[1] = ((c >> 8) & 0xFF) / 255.0f;
    out->color[2] = (c & 0xFF) / 255.0f;
    out->color[3] = alpha;
    return true;
}

/* ==================== scenefx 落地 ==================== */
#if WOODLAND_HAVE_EFFECTS

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
        struct wlr_scene_buffer *found =
            find_surface_buffer_at(child, surface, 0, 0, out_x, out_y);
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

static struct wlr_scene_shadow *find_shadow_node(struct wlr_scene_tree *tree)
{
    if (!tree) {
        return NULL;
    }
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link)
    {
        if (node->type == WLR_SCENE_NODE_SHADOW) {
            return wlr_scene_shadow_from_node(node);
        }
    }
    return NULL;
}

static struct fx_corner_radii to_fx(const struct woodland_effect_corners *corners)
{
    return corner_radii_new(corners->top_left, corners->top_right, corners->bottom_right,
                            corners->bottom_left);
}

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
        if (view_has_csd_margins(view) && !view->ssd_overrides.corner_radius_set) {
            struct woodland_effect_corners none = {0};
            apply_corners_in_tree(view->scene_tree, surface, surface->current.width,
                                  surface->current.height, &none);
        } else {
            apply_corners_in_tree(view->scene_tree, surface, surface->current.width,
                                  surface->current.height, &corners);
        }
    }

    struct woodland_effect_corners frame_corners;
    woodland_effect_frame_corners(view, &frame_corners);
    woodland_ssd_set_corners(view, frame_corners.top_left, frame_corners.top_right,
                             frame_corners.bottom_right, frame_corners.bottom_left);
}

static bool blur_apply_internal(struct wlr_scene_tree *tree,
                                struct wlr_scene_buffer *buf, int buf_x, int buf_y,
                                int surf_w, int surf_h, const struct wlr_box *region,
                                const struct woodland_effect_corners *corners,
                                bool should_only_blur_bottom)
{
    if (!tree) {
        return false;
    }
    struct wlr_scene_blur *blur = find_blur_node(tree);
    if (!region || surf_w <= 0 || surf_h <= 0) {
        if (blur) {
            wlr_scene_node_set_enabled(&blur->node, false);
        }
        return false;
    }

    struct wlr_box full = {.x = 0, .y = 0, .width = surf_w, .height = surf_h};
    struct wlr_box clamped = *region;
    if (!wlr_box_intersection(&clamped, &clamped, &full)) {
        if (blur) {
            wlr_scene_node_set_enabled(&blur->node, false);
        }
        return false;
    }

    bool is_full = wlr_box_equal(&clamped, &full);
    int node_x = is_full ? buf_x : (buf_x + clamped.x);
    int node_y = is_full ? buf_y : (buf_y + clamped.y);
    int node_w = is_full ? surf_w : clamped.width;
    int node_h = is_full ? surf_h : clamped.height;

    if (!blur) {
        blur = wlr_scene_blur_create(tree, node_w, node_h);
        if (!blur) {
            wlr_log(WLR_ERROR, "failed to create blur node");
            return false;
        }
        wlr_scene_node_lower_to_bottom(&blur->node);
    }
    wlr_scene_node_set_position(&blur->node, node_x, node_y);
    wlr_scene_blur_set_size(blur, node_w, node_h);

    if (is_full && corners) {
        wlr_scene_blur_set_corner_radii(blur, to_fx(corners));
    } else {
        wlr_scene_blur_set_corner_radii(blur, corner_radii_none());
    }

    wlr_scene_blur_set_transparency_mask_source(blur, is_full ? buf : NULL);
    wlr_scene_blur_set_should_only_blur_bottom_layer(blur, should_only_blur_bottom);
    wlr_scene_node_set_enabled(&blur->node, true);
    return true;
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

    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface || surface->current.width <= 0 || surface->current.height <= 0) {
        woodland_effect_blur_clear(view->scene_tree);
        return;
    }

    int origin_x = 0;
    int origin_y = 0;
    struct wlr_scene_buffer *buf =
        find_surface_buffer(view->scene_tree, surface, &origin_x, &origin_y);

    struct woodland_effect_corners corners;
    woodland_effect_corners(view, &corners);

    if (view_has_csd_margins(view) && !view->ssd_overrides.corner_radius_set) {
        corners = (struct woodland_effect_corners){0};
    }

    bool opt = view->server && view->server->config &&
               view->server->config->effects.blur_optimized;

    blur_apply_internal(view->scene_tree, buf, origin_x, origin_y,
                        surface->current.width, surface->current.height,
                        &view->effect.blur_region, &corners, opt);
}

static void update_shadow(struct woodland_view *view)
{
    if (!view->scene_tree) {
        return;
    }
    struct wlr_scene_shadow *shadow = find_shadow_node(view->scene_tree);
    struct woodland_effect_shadow params;
    if (!woodland_effect_shadow_params(view, &params)) {
        if (shadow) {
            wlr_scene_node_set_enabled(&shadow->node, false);
        }
        return;
    }

    struct woodland_ssd_margins m;
    woodland_ssd_get_margins(view, &m);
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    if (content.width <= 0 || content.height <= 0) {
        if (shadow) {
            wlr_scene_node_set_enabled(&shadow->node, false);
        }
        return;
    }

    struct wlr_box frame_box = {
        .x = -m.left,
        .y = -m.top,
        .width = content.width + m.left + m.right,
        .height = content.height + m.top + m.bottom,
    };

    struct woodland_effect_corners frame_corners;
    woodland_effect_frame_corners(view, &frame_corners);
    int b = view->ssd ? (m.left > 0 ? m.left : 1) : 0;
    int outer_radius = frame_corners.top_left > 0 ? frame_corners.top_left + b : 0;
    if (frame_corners.top_right > 0 && frame_corners.top_right + b > outer_radius) {
        outer_radius = frame_corners.top_right + b;
    }
    if (frame_corners.bottom_right > 0 && frame_corners.bottom_right + b > outer_radius) {
        outer_radius = frame_corners.bottom_right + b;
    }
    if (frame_corners.bottom_left > 0 && frame_corners.bottom_left + b > outer_radius) {
        outer_radius = frame_corners.bottom_left + b;
    }

    int r = params.radius;
    int sx = frame_box.x + params.offset_x - r;
    int sy = frame_box.y + params.offset_y - r;
    int sw = frame_box.width + 2 * r;
    int sh = frame_box.height + 2 * r;

    if (!shadow) {
        shadow = wlr_scene_shadow_create(view->scene_tree, sw, sh, outer_radius, (float)r,
                                         params.color);
        if (!shadow) {
            wlr_log(WLR_ERROR, "failed to create shadow scene node");
            return;
        }
        wlr_scene_node_lower_to_bottom(&shadow->node);
    } else {
        wlr_scene_shadow_set_size(shadow, sw, sh);
        wlr_scene_shadow_set_corner_radius(shadow, outer_radius);
        wlr_scene_shadow_set_blur_sigma(shadow, (float)r);
        wlr_scene_shadow_set_color(shadow, params.color);
    }

    wlr_scene_node_set_position(&shadow->node, sx, sy);

    struct clipped_region clip = clipped_region_get_default();
    clip.area = (struct wlr_box){
        .x = frame_box.x - sx,
        .y = frame_box.y - sy,
        .width = frame_box.width,
        .height = frame_box.height,
    };
    clip.corners = corner_radii_new(
        frame_corners.top_left > 0 ? frame_corners.top_left + b : 0,
        frame_corners.top_right > 0 ? frame_corners.top_right + b : 0,
        frame_corners.bottom_right > 0 ? frame_corners.bottom_right + b : 0,
        frame_corners.bottom_left > 0 ? frame_corners.bottom_left + b : 0);
    wlr_scene_shadow_set_clipped_region(shadow, clip);

    wlr_scene_node_set_enabled(&shadow->node, true);
}

void woodland_effect_blur_clear(struct wlr_scene_tree *tree)
{
    struct wlr_scene_blur *blur = find_blur_node(tree);
    if (blur) {
        wlr_scene_node_set_enabled(&blur->node, false);
    }
}

bool woodland_effect_blur_apply(struct wlr_scene_tree *tree, int x, int y, int width,
                                int height, const struct wlr_box *region,
                                const struct woodland_effect_corners *corners)
{
    return blur_apply_internal(tree, NULL, x, y, width, height, region, corners, false);
}

void woodland_effect_layer_blur(struct woodland_layer_surface *layer, const struct wlr_box *region)
{
    if (!layer || !layer->scene_layer_surface || !layer->layer_surface) {
        return;
    }
    struct wlr_scene_tree *tree = layer->scene_layer_surface->tree;
    struct wlr_surface *surface = layer->layer_surface->surface;
    if (!region || !woodland_effect_server_blur_enabled(layer->server)) {
        layer->effect.blur = false;
        layer->effect.blur_region = (struct wlr_box){0};
        woodland_effect_blur_clear(tree);
        return;
    }

    int origin_x = 0;
    int origin_y = 0;
    struct wlr_scene_buffer *buf = find_surface_buffer(tree, surface, &origin_x, &origin_y);

    enum zwlr_layer_shell_v1_layer zlayer = layer->layer_surface->current.layer;
    bool opt = false;
    if (layer->server && layer->server->config) {
        if (zlayer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
            opt = true;
        } else if (zlayer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
            opt = layer->server->config->effects.blur_optimized;
        }
    }

    bool ok = blur_apply_internal(tree, buf, origin_x, origin_y, surface->current.width,
                                  surface->current.height, region, NULL, opt);
    layer->effect.blur = ok;
    layer->effect.blur_region = ok ? *region : (struct wlr_box){0};
}

void woodland_effect_update(struct woodland_view *view)
{
    if (!view || !view->mapped) {
        return;
    }
    update_corners(view);
    update_blur(view);
    update_shadow(view);
}

void woodland_effect_apply_scene_config(struct woodland_server *server)
{
    if (!server || !server->scene) {
        return;
    }
    const struct woodland_config *cfg = server->config;
    if (cfg) {
        wlr_scene_set_blur_data(server->scene, cfg->effects.blur_passes,
                                cfg->effects.blur_radius, (float)cfg->effects.blur_noise,
                                (float)cfg->effects.blur_brightness,
                                (float)cfg->effects.blur_contrast,
                                (float)cfg->effects.blur_saturation);
    }
    woodland_effect_outputs_update(server);
}

void woodland_effect_output_update(struct woodland_output *output)
{
    if (!output || !output->server || !output->wlr_output) {
        return;
    }
    struct woodland_server *server = output->server;
    bool enable = woodland_effect_server_blur_enabled(server);

    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    if (box.width <= 0 || box.height <= 0) {
        enable = false;
    }

    if (!enable) {
        if (output->optimized_blur) {
            wlr_scene_node_destroy(&output->optimized_blur->node);
            output->optimized_blur = NULL;
        }
        return;
    }

    if (!output->optimized_blur) {
        output->optimized_blur =
            wlr_scene_optimized_blur_create(&server->scene->tree, box.width, box.height);
        if (!output->optimized_blur) {
            wlr_log(WLR_ERROR, "failed to create optimized blur node for %s",
                    output->wlr_output->name);
            return;
        }
        if (output->layer_tree[0]) {
            wlr_scene_node_place_above(&output->optimized_blur->node,
                                       &output->layer_tree[0]->node);
        }
        wlr_scene_node_set_position(&output->optimized_blur->node, box.x, box.y);
        wlr_scene_optimized_blur_mark_dirty(output->optimized_blur);
    } else {
        if (output->optimized_blur->node.x != box.x || output->optimized_blur->node.y != box.y) {
            wlr_scene_node_set_position(&output->optimized_blur->node, box.x, box.y);
            wlr_scene_optimized_blur_mark_dirty(output->optimized_blur);
        }
        wlr_scene_optimized_blur_set_size(output->optimized_blur, box.width, box.height);
    }
    wlr_scene_node_set_enabled(&output->optimized_blur->node, true);
}

void woodland_effect_output_destroy(struct woodland_output *output)
{
    if (output && output->optimized_blur) {
        wlr_scene_node_destroy(&output->optimized_blur->node);
        output->optimized_blur = NULL;
    }
}
void woodland_effect_outputs_update(struct woodland_server *server)
{
    if (!server || !server->outputs.next) {
        return;
    }
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link)
    {
        woodland_effect_output_update(output);
    }
}

void woodland_effect_mark_blur_dirty(struct woodland_server *server)
{
    if (!server || !server->outputs.next) {
        return;
    }
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link)
    {
        if (output->optimized_blur) {
            wlr_scene_optimized_blur_mark_dirty(output->optimized_blur);
        }
    }
}

#else /* !WOODLAND_HAVE_EFFECTS */

void woodland_effect_update(struct woodland_view *view)
{
    (void)view;
}

void woodland_effect_blur_clear(struct wlr_scene_tree *tree)
{
    (void)tree;
}

bool woodland_effect_blur_apply(struct wlr_scene_tree *tree, int x, int y, int width,
                                int height, const struct wlr_box *region,
                                const struct woodland_effect_corners *corners)
{
    (void)tree;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)region;
    (void)corners;
    return false;
}

void woodland_effect_layer_blur(struct woodland_layer_surface *layer, const struct wlr_box *region)
{
    (void)layer;
    (void)region;
}

void woodland_effect_apply_scene_config(struct woodland_server *server)
{
    (void)server;
}

void woodland_effect_output_update(struct woodland_output *output)
{
    (void)output;
}

void woodland_effect_output_destroy(struct woodland_output *output)
{
    (void)output;
}

void woodland_effect_outputs_update(struct woodland_server *server)
{
    (void)server;
}

void woodland_effect_mark_blur_dirty(struct woodland_server *server)
{
    (void)server;
}

#endif
