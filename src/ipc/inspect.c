// SPDX-License-Identifier: GPL-2.0-only
#include "config.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scene.h"
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

#include "ipc_internal.h"
#include "multitask_view.h"
#include "output.h"
#include "ssd.h"
#include "util/json.h"
#include "view.h"
#include "window_menu.h"
#include "woodland.h"

#define INSPECT_MAX_PIXELS (4096ULL * 4096ULL)
#define INSPECT_TOP_COLORS 8
#define INSPECT_DEFAULT_TOLERANCE 2

struct sampled_region {
    struct woodland_output *wo;
    struct wlr_box obox;
    struct wlr_box logical_box;
    struct wlr_box buffer_box;
    int trans_w;
    int trans_h;
    uint8_t *pixels;
};

static void sampled_region_finish(struct sampled_region *sr)
{
    free(sr->pixels);
    sr->pixels = NULL;
}

static bool parse_int_strict(const char *s, int *out)
{
    if (!s || !s[0]) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool parse_box_csv(const char *s, struct wlr_box *box)
{
    if (!s || !s[0]) {
        return false;
    }
    int vals[4] = {0};
    const char *p = s;
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (errno != 0 || end == p || v < INT32_MIN || v > INT32_MAX) {
            return false;
        }
        vals[i] = (int)v;
        if (i < 3) {
            if (*end != ',') {
                return false;
            }
            p = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    box->x = vals[0];
    box->y = vals[1];
    box->width = vals[2];
    box->height = vals[3];
    return true;
}

static struct woodland_output *find_output_wrapper(struct woodland_server *server,
                                                   struct wlr_output *wlr_output)
{
    if (!server || !wlr_output || !server->outputs.next) {
        return NULL;
    }
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        if (wo->wlr_output == wlr_output) {
            return wo;
        }
    }
    return NULL;
}

static struct woodland_output *first_enabled_output(struct woodland_server *server)
{
    if (!server || !server->outputs.next) {
        return NULL;
    }
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        if (wo->wlr_output && wo->wlr_output->enabled && wo->scene_output &&
            wo->wlr_output->renderer) {
            return wo;
        }
    }
    return NULL;
}

static struct woodland_output *find_output_by_name(struct woodland_server *server,
                                                   const char *name)
{
    if (!server || !server->outputs.next || !name || !name[0]) {
        return NULL;
    }
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        if (wo->wlr_output && wo->wlr_output->name &&
            strcmp(wo->wlr_output->name, name) == 0 &&
            wo->wlr_output->enabled && wo->scene_output &&
            wo->wlr_output->renderer) {
            return wo;
        }
    }
    return NULL;
}

static void logical_point_to_buffer_box(const struct wlr_box *logical_pt,
                                        const struct wlr_box *obox,
                                        struct wlr_output *wlr_output,
                                        int trans_w, int trans_h,
                                        struct wlr_box *out_box)
{
    double scale = (wlr_output->scale > 0.0f) ? (double)wlr_output->scale : 1.0;
    int bx0 = (int)floor((logical_pt->x - obox->x) * scale);
    int by0 = (int)floor((logical_pt->y - obox->y) * scale);
    int bx1 = (int)ceil((logical_pt->x + logical_pt->width - obox->x) * scale);
    int by1 = (int)ceil((logical_pt->y + logical_pt->height - obox->y) * scale);
    if (bx1 <= bx0) {
        bx1 = bx0 + 1;
    }
    if (by1 <= by0) {
        by1 = by0 + 1;
    }
    struct wlr_box box = {
        .x = bx0,
        .y = by0,
        .width = bx1 - bx0,
        .height = by1 - by0,
    };
    wlr_box_transform(&box, &box, wlr_output_transform_invert(wlr_output->transform),
                      trans_w, trans_h);
    *out_box = box;
}

static bool sample_output_region(struct woodland_server *server,
                                 const char *output_name,
                                 const struct wlr_box *req_box,
                                 bool full_output,
                                 struct sampled_region *sr,
                                 char *err, size_t err_cap)
{
    memset(sr, 0, sizeof(*sr));
    if (!server || !server->scene || !server->renderer || !server->output_layout) {
        snprintf(err, err_cap, "renderer or output layout unavailable");
        return false;
    }

    if (!full_output &&
        ((int64_t)req_box->x + (int64_t)req_box->width > INT32_MAX ||
         (int64_t)req_box->y + (int64_t)req_box->height > INT32_MAX ||
         (int64_t)req_box->x + (int64_t)req_box->width < INT32_MIN ||
         (int64_t)req_box->y + (int64_t)req_box->height < INT32_MIN)) {
        snprintf(err, err_cap, "region coordinates overflow");
        return false;
    }

    struct woodland_output *wo = NULL;
    struct wlr_output *wlr_output = NULL;
    bool explicit_output = output_name && output_name[0] != '\0';
    if (explicit_output) {
        wo = find_output_by_name(server, output_name);
        wlr_output = wo ? wo->wlr_output : NULL;
        if (!wo || !wlr_output) {
            snprintf(err, err_cap, "output not found or disabled");
            return false;
        }
    } else if (full_output) {
        wo = first_enabled_output(server);
        wlr_output = wo ? wo->wlr_output : NULL;
    } else {
        wlr_output = wlr_output_layout_output_at(server->output_layout,
                                                 (double)req_box->x, (double)req_box->y);
        wo = find_output_wrapper(server, wlr_output);
        if ((!wo || !wlr_output || !wo->scene_output || !wlr_output->enabled ||
             !wlr_output->renderer) &&
            server->outputs.next) {
            struct woodland_output *cand;
            wl_list_for_each(cand, &server->outputs, link) {
                if (!cand->wlr_output || !cand->wlr_output->enabled ||
                    !cand->scene_output || !cand->wlr_output->renderer) {
                    continue;
                }
                struct wlr_box cand_box = {0};
                wlr_output_layout_get_box(server->output_layout, cand->wlr_output, &cand_box);
                struct wlr_box tmp = {0};
                if (wlr_box_intersection(&tmp, req_box, &cand_box) &&
                    tmp.width > 0 && tmp.height > 0) {
                    wo = cand;
                    wlr_output = cand->wlr_output;
                    break;
                }
            }
        }
    }

    if (!wo || !wlr_output || !wo->scene_output || !wlr_output->enabled ||
        !wlr_output->renderer) {
        snprintf(err, err_cap, full_output ? "no enabled output available"
                                           : "region outside output");
        return false;
    }

    struct wlr_box obox = {0};
    wlr_output_layout_get_box(server->output_layout, wlr_output, &obox);
    if (obox.width <= 0 || obox.height <= 0) {
        snprintf(err, err_cap, "output layout box is empty");
        return false;
    }

    struct wlr_box clip = obox;
    if (!full_output) {
        if (!wlr_box_intersection(&clip, req_box, &obox) ||
            clip.width <= 0 || clip.height <= 0) {
            snprintf(err, err_cap, "region outside output");
            return false;
        }
    }

    int trans_w = 0, trans_h = 0;
    wlr_output_transformed_resolution(wlr_output, &trans_w, &trans_h);
    if (trans_w <= 0 || trans_h <= 0) {
        snprintf(err, err_cap, "output resolution is empty");
        return false;
    }

    struct wlr_box src_box = {0};
    logical_point_to_buffer_box(&clip, &obox, wlr_output, trans_w, trans_h, &src_box);

    /*
     * 锁 attach_render 再构帧：避免单层全屏触发 direct scanout，保证必定合成到
     * swapchain 缓冲；同时不调用 wlr_output_commit_state，避免影响真实输出提交
     * 时序或在 DRM 后端触发 EBUSY。
     */
    wlr_output_lock_attach_render(wlr_output, true);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    bool built = wlr_scene_output_build_state(wo->scene_output, &state, NULL);
    wlr_output_lock_attach_render(wlr_output, false);

    if (!built || !(state.committed & WLR_OUTPUT_STATE_BUFFER) || !state.buffer) {
        wlr_output_state_finish(&state);
        snprintf(err, err_cap, "failed to render scene output state");
        return false;
    }

    struct wlr_box buf_bounds = {
        .x = 0,
        .y = 0,
        .width = state.buffer->width,
        .height = state.buffer->height,
    };
    if (!wlr_box_intersection(&src_box, &src_box, &buf_bounds) ||
        src_box.width <= 0 || src_box.height <= 0) {
        wlr_output_state_finish(&state);
        snprintf(err, err_cap, "region outside output buffer");
        return false;
    }

    uint64_t npixels = (uint64_t)src_box.width * (uint64_t)src_box.height;
    if (npixels > INSPECT_MAX_PIXELS) {
        wlr_output_state_finish(&state);
        snprintf(err, err_cap, "region exceeds pixel limit");
        return false;
    }

    uint8_t *pixels = malloc((size_t)npixels * 4);
    if (!pixels) {
        wlr_output_state_finish(&state);
        snprintf(err, err_cap, "out of memory");
        return false;
    }

    struct wlr_texture *tex = wlr_texture_from_buffer(wlr_output->renderer, state.buffer);
    if (!tex) {
        free(pixels);
        wlr_output_state_finish(&state);
        snprintf(err, err_cap, "failed to create texture from output buffer");
        return false;
    }

    bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
        .data = pixels,
        .format = DRM_FORMAT_ABGR8888,
        .stride = (uint32_t)src_box.width * 4,
        .src_box = src_box,
    });
    wlr_texture_destroy(tex);
    wlr_output_state_finish(&state);

    if (!ok) {
        free(pixels);
        snprintf(err, err_cap, "failed to read pixels from renderer");
        return false;
    }

    sr->wo = wo;
    sr->obox = obox;
    sr->logical_box = clip;
    sr->buffer_box = src_box;
    sr->trans_w = trans_w;
    sr->trans_h = trans_h;
    sr->pixels = pixels;
    return true;
}

bool woodland_ipc_sample_pixel(struct woodland_server *server, int lx, int ly,
                               uint8_t rgba[4], char *err, size_t err_cap)
{
    struct wlr_box req = {.x = lx, .y = ly, .width = 1, .height = 1};
    struct sampled_region sr;
    if (!sample_output_region(server, NULL, &req, false, &sr, err, err_cap)) {
        return false;
    }
    memcpy(rgba, sr.pixels, 4);
    sampled_region_finish(&sr);
    return true;
}

static bool node_in_tree(const struct wlr_scene_node *node,
                         const struct wlr_scene_tree *ancestor)
{
    if (!node || !ancestor) {
        return false;
    }
    const struct wlr_scene_tree *cur = node->parent;
    for (int depth = 0; cur && depth < 32; depth++) {
        if (cur == ancestor) {
            return true;
        }
        cur = cur->node.parent;
    }
    return false;
}

static unsigned float_to_byte(float v)
{
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    return (unsigned)(v * 255.0f + 0.5f);
}

static bool rect_is_opaque(const struct wlr_scene_rect *rect)
{
    return rect->color[3] >= 0.999f;
}

static uint32_t rect_color_rgb(const struct wlr_scene_rect *rect)
{
    return (float_to_byte(rect->color[0]) << 16) |
           (float_to_byte(rect->color[1]) << 8) |
           float_to_byte(rect->color[2]);
}

static bool try_set_rect_expected(struct wlr_scene_node *hit, bool *has_expected,
                                  uint32_t *expected_rgb)
{
    if (hit->type != WLR_SCENE_NODE_RECT) {
        return false;
    }
    const struct wlr_scene_rect *rect = wlr_scene_rect_from_node(hit);
    if (!rect_is_opaque(rect)) {
        return false;
    }
    *has_expected = true;
    *expected_rgb = rect_color_rgb(rect);
    return true;
}

int woodland_ipc_rgb_delta(const uint8_t rgba[4], uint32_t expected_rgb)
{
    int er = (int)((expected_rgb >> 16) & 0xFF);
    int eg = (int)((expected_rgb >> 8) & 0xFF);
    int eb = (int)(expected_rgb & 0xFF);
    int dr = abs((int)rgba[0] - er);
    int dg = abs((int)rgba[1] - eg);
    int db = abs((int)rgba[2] - eb);
    int m = dr > dg ? dr : dg;
    return m > db ? m : db;
}

static const char *scene_node_type_str(const struct wlr_scene_node *node)
{
    if (!node) {
        return NULL;
    }
    switch (node->type) {
    case WLR_SCENE_NODE_TREE:
        return "tree";
    case WLR_SCENE_NODE_RECT:
        return "rect";
    case WLR_SCENE_NODE_BUFFER:
        return "buffer";
#if WOODLAND_HAVE_EFFECTS
    case WLR_SCENE_NODE_BLUR:
        return "blur";
    case WLR_SCENE_NODE_OPTIMIZED_BLUR:
        return "optimized_blur";
    case WLR_SCENE_NODE_SHADOW:
        return "shadow";
#endif
    default:
        return "unknown";
    }
}

void woodland_ipc_write_pixel_source(struct woodland_server *server, int lx, int ly,
                                     const uint8_t rgba[4], struct woodland_json *out,
                                     bool *has_expected, uint32_t *expected_rgb,
                                     const char **expected_src, bool *is_ssd_titlebar,
                                     const struct woodland_view **out_view)
{
    double sx = 0.0, sy = 0.0;
    struct wlr_scene_node *hit = NULL;
    if (server && server->scene) {
        hit = wlr_scene_node_at(&server->scene->tree.node, (double)lx, (double)ly, &sx, &sy);
    }

    const char *kind = "overlay";
    const char *node_type = scene_node_type_str(hit);
    const char *layer_ns = NULL;
    const struct woodland_view *hit_view = NULL;
    bool local_expected = false;
    uint32_t local_rgb = 0;
    const char *local_src = "unknown";
    bool local_titlebar = false;

    if (!hit) {
        kind = "background";
        local_expected = true;
        local_rgb = 0x000000;
        local_src = "compositor.background";
    } else {
        if (server->views.next) {
            struct woodland_view *view;
            wl_list_for_each(view, &server->views, link) {
                if (view->scene_tree && node_in_tree(hit, view->scene_tree)) {
                    hit_view = view;
                    break;
                }
            }
        }

        if (hit_view) {
            bool is_client_surf = false;
            if (hit->type == WLR_SCENE_NODE_BUFFER) {
                struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(hit);
                if (wlr_scene_surface_try_from_buffer(sb) != NULL) {
                    is_client_surf = true;
                }
            }
            if (is_client_surf) {
                kind = "view";
                local_src = "client.surface";
            } else {
                uint32_t edges = 0;
                enum woodland_ssd_part part =
                    woodland_ssd_at((struct woodland_view *)hit_view,
                                    (double)lx, (double)ly, &edges);
                struct woodland_ssd_palette pal = {0};
                woodland_ssd_get_palette(hit_view, &pal);

                switch (part) {
                case WOODLAND_SSD_TITLEBAR:
                    kind = "ssd.titlebar";
                    local_titlebar = true;
                    if (hit->type == WLR_SCENE_NODE_RECT) {
                        local_expected = true;
                        local_rgb = hit_view->activated ? pal.titlebar_active
                                                        : pal.titlebar_inactive;
                        local_src = hit_view->activated ? "ssd.palette.titlebar_active"
                                                        : "ssd.palette.titlebar_inactive";
                    } else {
                        local_src = "ssd.title_text";
                    }
                    break;
                case WOODLAND_SSD_BORDER_LEFT:
                case WOODLAND_SSD_BORDER_RIGHT:
                case WOODLAND_SSD_BORDER_TOP:
                case WOODLAND_SSD_BORDER_BOTTOM:
                    kind = "ssd.border";
                    if (hit->type == WLR_SCENE_NODE_RECT) {
                        local_expected = true;
                        local_rgb = hit_view->activated ? pal.border_active
                                                        : pal.border_inactive;
                        local_src = hit_view->activated ? "ssd.palette.border_active"
                                                        : "ssd.palette.border_inactive";
                    } else {
                        local_src = "ssd.border";
                    }
                    break;
                case WOODLAND_SSD_BUTTON_CLOSE:
                    kind = "ssd.button.close";
                    local_src = "ssd.button";
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    break;
                case WOODLAND_SSD_BUTTON_MAXIMIZE:
                    kind = "ssd.button.maximize";
                    local_src = "ssd.button";
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    break;
                case WOODLAND_SSD_BUTTON_MINIMIZE:
                    kind = "ssd.button.minimize";
                    local_src = "ssd.button";
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    break;
                default:
                    kind = "ssd.decoration";
                    if (hit->type == WLR_SCENE_NODE_RECT) {
                        try_set_rect_expected(hit, &local_expected, &local_rgb);
                        local_src = "scene.rect";
                    } else {
                        local_src = "scene.buffer";
                    }
                    break;
                }
            }
        } else if (server->xwayland_unmanaged_tree &&
                   node_in_tree(hit, server->xwayland_unmanaged_tree)) {
            kind = "xwayland.unmanaged";
            local_src = "xwayland.unmanaged";
        } else {
            bool in_layer = false;
            if (server->outputs.next) {
                struct woodland_output *wo;
                wl_list_for_each(wo, &server->outputs, link) {
                    for (int i = 0; i < 4; i++) {
                        if (wo->layer_tree[i] && node_in_tree(hit, wo->layer_tree[i])) {
                            in_layer = true;
                            break;
                        }
                    }
                    if (in_layer) {
                        break;
                    }
                }
            }
            if (in_layer) {
                kind = "layer";
                local_src = "layer.surface";
                if (hit->type == WLR_SCENE_NODE_BUFFER) {
                    struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(hit);
                    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
                    if (ss && ss->surface) {
                        struct wlr_layer_surface_v1 *ls =
                            wlr_layer_surface_v1_try_from_wlr_surface(ss->surface);
                        if (ls) {
                            layer_ns = ls->namespace;
                        }
                    }
                }
            } else if (server->wallpaper_tree && node_in_tree(hit, server->wallpaper_tree)) {
                kind = "wallpaper";
                local_src = "wallpaper";
            } else if (server->snap_preview.tree && node_in_tree(hit, server->snap_preview.tree)) {
                kind = "snap_preview";
                if (hit->type == WLR_SCENE_NODE_RECT) {
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    local_src = "snap_preview.rect";
                } else {
                    local_src = "snap_preview";
                }
            } else if (server->dnd.icons && node_in_tree(hit, server->dnd.icons)) {
                kind = "dnd_icon";
                local_src = "dnd.icon";
            } else if (server->multitask_view && server->multitask_view->tree &&
                       node_in_tree(hit, server->multitask_view->tree)) {
                kind = "multitask_view";
                if (hit->type == WLR_SCENE_NODE_RECT) {
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    local_src = "multitask_view.rect";
                } else {
                    local_src = "multitask_view";
                }
            } else if (server->window_menu && server->window_menu->tree &&
                       node_in_tree(hit, server->window_menu->tree)) {
                kind = "window_menu";
                if (hit->type == WLR_SCENE_NODE_RECT) {
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    local_src = "window_menu.rect";
                } else {
                    local_src = "window_menu";
                }
            } else {
                kind = "overlay";
                if (hit->type == WLR_SCENE_NODE_RECT) {
                    try_set_rect_expected(hit, &local_expected, &local_rgb);
                    local_src = "scene.rect";
                } else {
                    local_src = "scene.buffer";
                }
            }
        }
    }

    struct woodland_json_obj src;
    woodland_json_obj_open(&src, out);
    woodland_json_obj_str(&src, "kind", kind);
    woodland_json_obj_str(&src, "node_type", node_type);
    if (hit_view) {
        woodland_json_obj_int(&src, "view", hit_view->id);
        woodland_json_obj_str(&src, "title", hit_view->title);
        woodland_json_obj_str(&src, "app_id", hit_view->app_id);
        woodland_json_obj_bool(&src, "activated", hit_view->activated);
    }
    if (layer_ns) {
        woodland_json_obj_str(&src, "namespace", layer_ns);
    }
    if (local_expected) {
        char ehex[8];
        snprintf(ehex, sizeof(ehex), "#%06x", local_rgb & 0xFFFFFFu);
        int delta = woodland_ipc_rgb_delta(rgba, local_rgb);
        woodland_json_obj_str(&src, "expected_hex", ehex);
        woodland_json_obj_str(&src, "expected_source", local_src);
        woodland_json_obj_bool(&src, "matches_expected", delta <= INSPECT_DEFAULT_TOLERANCE);
        woodland_json_obj_int(&src, "channel_delta", delta);
    } else {
        woodland_json_obj_key(&src, "expected_hex");
        woodland_json_raw(out, "null");
        woodland_json_obj_str(&src, "expected_source", local_src);
    }
    woodland_json_obj_close(&src);

    if (has_expected) {
        *has_expected = local_expected;
    }
    if (expected_rgb) {
        *expected_rgb = local_rgb;
    }
    if (expected_src) {
        *expected_src = local_src;
    }
    if (is_ssd_titlebar) {
        *is_ssd_titlebar = local_titlebar;
    }
    if (out_view) {
        *out_view = hit_view;
    }
}

static void respond_inspect_error(struct woodland_json *out, const char *cmd, const char *msg)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", false);
    woodland_json_obj_str(&obj, "cmd", cmd);
    woodland_json_obj_str(&obj, "error", msg);
    woodland_json_obj_close(&obj);
}

bool woodland_ipc_cmd_pixel(struct woodland_server *server, const char *args,
                            struct woodland_json *out)
{
    char tok1[32], tok2[32], extra[8];
    const char *p = woodland_ipc_next_token(args, tok1, sizeof(tok1));
    p = p ? woodland_ipc_next_token(p, tok2, sizeof(tok2)) : NULL;
    int lx = 0, ly = 0;
    if (!p || woodland_ipc_next_token(p, extra, sizeof(extra)) ||
        !parse_int_strict(tok1, &lx) || !parse_int_strict(tok2, &ly)) {
        respond_inspect_error(out, "pixel", "usage: pixel <x> <y>");
        return false;
    }

    struct wlr_box req = {.x = lx, .y = ly, .width = 1, .height = 1};
    struct sampled_region sr;
    char err[128];
    if (!sample_output_region(server, NULL, &req, false, &sr, err, sizeof(err))) {
        respond_inspect_error(out, "pixel", err);
        return false;
    }

    uint8_t rgba[4];
    memcpy(rgba, sr.pixels, 4);
    int rgba_vals[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    int bcoords[2] = {sr.buffer_box.x, sr.buffer_box.y};
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", rgba[0], rgba[1], rgba[2]);

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", true);
    woodland_json_obj_str(&obj, "cmd", "pixel");
    woodland_json_obj_int(&obj, "x", lx);
    woodland_json_obj_int(&obj, "y", ly);
    woodland_json_obj_str(&obj, "output", sr.wo->wlr_output->name);
    woodland_json_obj_key(&obj, "buffer_coords");
    woodland_json_int_array(out, bcoords, 2);
    woodland_json_obj_key(&obj, "rgba");
    woodland_json_int_array(out, rgba_vals, 4);
    woodland_json_obj_str(&obj, "hex", hex);
    woodland_json_obj_key(&obj, "source");
    woodland_ipc_write_pixel_source(server, lx, ly, rgba, out, NULL, NULL, NULL, NULL, NULL);
    woodland_json_obj_close(&obj);

    sampled_region_finish(&sr);
    return true;
}

struct color_bucket {
    uint32_t rgba;
    uint32_t count;
};

struct color_hist {
    struct color_bucket *buckets;
    size_t cap;
    size_t unique;
};

static uint32_t hash_rgba(uint32_t rgba)
{
    uint32_t h = rgba * 2654435761u;
    h ^= h >> 16;
    return h;
}

static bool hist_init(struct color_hist *hist, size_t initial_cap)
{
    hist->buckets = calloc(initial_cap, sizeof(*hist->buckets));
    if (!hist->buckets) {
        hist->cap = 0;
        hist->unique = 0;
        return false;
    }
    hist->cap = initial_cap;
    hist->unique = 0;
    return true;
}

static void hist_finish(struct color_hist *hist)
{
    free(hist->buckets);
    hist->buckets = NULL;
    hist->cap = 0;
    hist->unique = 0;
}

static void hist_rehash(struct color_hist *hist, size_t new_cap)
{
    struct color_bucket *nb = calloc(new_cap, sizeof(*nb));
    if (!nb) {
        return;
    }
    for (size_t i = 0; i < hist->cap; i++) {
        if (hist->buckets[i].count == 0) {
            continue;
        }
        size_t idx = hash_rgba(hist->buckets[i].rgba) & (new_cap - 1);
        while (nb[idx].count > 0) {
            idx = (idx + 1) & (new_cap - 1);
        }
        nb[idx] = hist->buckets[i];
    }
    free(hist->buckets);
    hist->buckets = nb;
    hist->cap = new_cap;
}

static void hist_add(struct color_hist *hist, uint32_t rgba)
{
    if (!hist->buckets || hist->cap == 0) {
        return;
    }
    if (hist->unique * 2 >= hist->cap && hist->cap < (1u << 22)) {
        hist_rehash(hist, hist->cap * 2);
    }
    size_t idx = hash_rgba(rgba) & (hist->cap - 1);
    for (size_t probe = 0; probe < hist->cap; probe++) {
        struct color_bucket *b = &hist->buckets[idx];
        if (b->count == 0) {
            if (hist->unique + 1 >= hist->cap) {
                return;
            }
            b->rgba = rgba;
            b->count = 1;
            hist->unique++;
            return;
        }
        if (b->rgba == rgba) {
            b->count++;
            return;
        }
        idx = (idx + 1) & (hist->cap - 1);
    }
}

static int cmp_bucket_desc(const void *a, const void *b)
{
    const struct color_bucket *ba = a;
    const struct color_bucket *bb = b;
    if (ba->count < bb->count) {
        return 1;
    }
    if (ba->count > bb->count) {
        return -1;
    }
    if (ba->rgba < bb->rgba) {
        return -1;
    }
    return ba->rgba > bb->rgba ? 1 : 0;
}

static bool write_ppm_rgb(const char *path, int width, int height, const uint8_t *rgba)
{
    if (!path || !path[0] || width <= 0 || height <= 0 || !rgba) {
        return false;
    }
    uint8_t *row = malloc((size_t)width * 3);
    if (!row) {
        return false;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(row);
        return false;
    }
    bool ok = fprintf(fp, "P6\n%d %d\n255\n", width, height) > 0;
    for (int y = 0; ok && y < height; y++) {
        const uint8_t *src = rgba + ((size_t)y * (size_t)width) * 4;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(row, 1, (size_t)width * 3, fp) != (size_t)width * 3) {
            ok = false;
        }
    }
    if (fclose(fp) != 0) {
        ok = false;
    }
    free(row);
    if (!ok) {
        unlink(path);
    }
    return ok;
}

bool woodland_ipc_cmd_screenshot(struct woodland_server *server, const char *args,
                                 struct woodland_json *out)
{
    bool has_region = false;
    struct wlr_box req = {0};
    bool has_expect = false;
    uint32_t expect_arg_rgb = 0;
    bool has_output = false;
    char output_name[64] = {0};
    bool has_dump_ppm = false;
    char dump_ppm[256] = {0};
    int tolerance = INSPECT_DEFAULT_TOLERANCE;
    int pos_vals[4] = {0};
    int pos_count = 0;

    char tok[320];
    const char *p = args;
    while ((p = woodland_ipc_next_token(p, tok, sizeof(tok))) != NULL) {
        if (strcmp(tok, "--region") == 0) {
            char val[128];
            p = woodland_ipc_next_token(p, val, sizeof(val));
            if (!p || !parse_box_csv(val, &req) || has_region || pos_count > 0) {
                respond_inspect_error(
                    out, "screenshot",
                    "usage: screenshot [--output NAME] [--region x,y,w,h] [--expect #rrggbb] [--tolerance N] [--dump-ppm PATH]");
                return false;
            }
            has_region = true;
        } else if (strncmp(tok, "--region=", 9) == 0 || strncmp(tok, "region=", 7) == 0) {
            const char *val = (tok[0] == '-') ? tok + 9 : tok + 7;
            if (!parse_box_csv(val, &req) || has_region || pos_count > 0) {
                respond_inspect_error(out, "screenshot", "invalid region format (want x,y,w,h)");
                return false;
            }
            has_region = true;
        } else if (strcmp(tok, "--output") == 0) {
            char val[64];
            p = woodland_ipc_next_token(p, val, sizeof(val));
            if (!p || !val[0]) {
                respond_inspect_error(out, "screenshot", "invalid --output name");
                return false;
            }
            snprintf(output_name, sizeof(output_name), "%s", val);
            has_output = true;
        } else if (strncmp(tok, "--output=", 9) == 0 || strncmp(tok, "output=", 7) == 0) {
            const char *val = (tok[0] == '-') ? tok + 9 : tok + 7;
            if (!val[0]) {
                respond_inspect_error(out, "screenshot", "invalid output name");
                return false;
            }
            snprintf(output_name, sizeof(output_name), "%s", val);
            has_output = true;
        } else if (strcmp(tok, "--dump-ppm") == 0) {
            char val[256];
            p = woodland_ipc_next_token(p, val, sizeof(val));
            if (!p || !val[0]) {
                respond_inspect_error(out, "screenshot", "invalid --dump-ppm path");
                return false;
            }
            snprintf(dump_ppm, sizeof(dump_ppm), "%s", val);
            has_dump_ppm = true;
        } else if (strncmp(tok, "--dump-ppm=", 11) == 0 || strncmp(tok, "dump_ppm=", 9) == 0) {
            const char *val = (tok[0] == '-') ? tok + 11 : tok + 9;
            if (!val[0]) {
                respond_inspect_error(out, "screenshot", "invalid dump-ppm path");
                return false;
            }
            snprintf(dump_ppm, sizeof(dump_ppm), "%s", val);
            has_dump_ppm = true;
        } else if (strcmp(tok, "--expect") == 0) {
            char val[64];
            p = woodland_ipc_next_token(p, val, sizeof(val));
            if (!p || !woodland_ipc_parse_hex_rgb(val, &expect_arg_rgb)) {
                respond_inspect_error(out, "screenshot", "invalid --expect color (want #rrggbb)");
                return false;
            }
            has_expect = true;
        } else if (strncmp(tok, "--expect=", 9) == 0 || strncmp(tok, "expect=", 7) == 0) {
            const char *val = (tok[0] == '-') ? tok + 9 : tok + 7;
            if (!woodland_ipc_parse_hex_rgb(val, &expect_arg_rgb)) {
                respond_inspect_error(out, "screenshot", "invalid expect color (want #rrggbb)");
                return false;
            }
            has_expect = true;
        } else if (strcmp(tok, "--tolerance") == 0) {
            char val[32];
            p = woodland_ipc_next_token(p, val, sizeof(val));
            if (!p || !parse_int_strict(val, &tolerance) || tolerance < 0 || tolerance > 255) {
                respond_inspect_error(out, "screenshot", "tolerance must be 0..255");
                return false;
            }
        } else if (strncmp(tok, "--tolerance=", 12) == 0 || strncmp(tok, "tolerance=", 10) == 0) {
            const char *val = (tok[0] == '-') ? tok + 12 : tok + 10;
            if (!parse_int_strict(val, &tolerance) || tolerance < 0 || tolerance > 255) {
                respond_inspect_error(out, "screenshot", "tolerance must be 0..255");
                return false;
            }
        } else if (tok[0] == '-') {
            int val = 0;
            if (parse_int_strict(tok, &val) && !has_region && pos_count < 4) {
                pos_vals[pos_count++] = val;
            } else {
                respond_inspect_error(out, "screenshot", "unknown option");
                return false;
            }
        } else {
            int val = 0;
            if (parse_int_strict(tok, &val) && !has_region && pos_count < 4) {
                pos_vals[pos_count++] = val;
            } else {
                respond_inspect_error(out, "screenshot", "unexpected argument");
                return false;
            }
        }
    }

    if (pos_count > 0) {
        if (pos_count != 4) {
            respond_inspect_error(out, "screenshot",
                                  "positional region requires 4 integers: x y w h");
            return false;
        }
        req = (struct wlr_box){
            .x = pos_vals[0],
            .y = pos_vals[1],
            .width = pos_vals[2],
            .height = pos_vals[3],
        };
        has_region = true;
    }

    if (has_region && (req.width <= 0 || req.height <= 0)) {
        respond_inspect_error(out, "screenshot", "region width and height must be positive");
        return false;
    }

    if (has_dump_ppm && !server->debug_ctl) {
        respond_inspect_error(out, "screenshot", "--dump-ppm requires --debug-ctl");
        return false;
    }

    struct sampled_region sr;
    char err[128];
    if (!sample_output_region(server, has_output ? output_name : NULL,
                              &req, !has_region, &sr, err, sizeof(err))) {
        respond_inspect_error(out, "screenshot", err);
        return false;
    }

    if (has_dump_ppm &&
        !write_ppm_rgb(dump_ppm, sr.buffer_box.width, sr.buffer_box.height, sr.pixels)) {
        sampled_region_finish(&sr);
        respond_inspect_error(out, "screenshot", "failed to write ppm file");
        return false;
    }

    uint64_t total_pixels = (uint64_t)sr.buffer_box.width * (uint64_t)sr.buffer_box.height;
    struct color_hist hist = {0};
    if (!hist_init(&hist, total_pixels < 4096 ? 1024 : 65536)) {
        sampled_region_finish(&sr);
        respond_inspect_error(out, "screenshot", "out of memory");
        return false;
    }

    for (uint64_t i = 0; i < total_pixels; i++) {
        const uint8_t *px = sr.pixels + i * 4;
        uint32_t packed = ((uint32_t)px[0] << 24) | ((uint32_t)px[1] << 16) |
                          ((uint32_t)px[2] << 8) | (uint32_t)px[3];
        hist_add(&hist, packed);
    }

    struct color_bucket top[INSPECT_TOP_COLORS] = {0};
    size_t top_len = 0;
    for (size_t i = 0; i < hist.cap; i++) {
        if (hist.buckets[i].count == 0) {
            continue;
        }
        if (top_len < INSPECT_TOP_COLORS) {
            top[top_len++] = hist.buckets[i];
            if (top_len == INSPECT_TOP_COLORS) {
                qsort(top, top_len, sizeof(top[0]), cmp_bucket_desc);
            }
        } else if (cmp_bucket_desc(&hist.buckets[i], &top[INSPECT_TOP_COLORS - 1]) < 0) {
            top[INSPECT_TOP_COLORS - 1] = hist.buckets[i];
            qsort(top, INSPECT_TOP_COLORS, sizeof(top[0]), cmp_bucket_desc);
        }
    }
    if (top_len > 1 && top_len < INSPECT_TOP_COLORS) {
        qsort(top, top_len, sizeof(top[0]), cmp_bucket_desc);
    }
    size_t unique_colors = hist.unique;
    hist_finish(&hist);

    int cx = sr.logical_box.x + sr.logical_box.width / 2;
    int cy = sr.logical_box.y + sr.logical_box.height / 2;
    struct wlr_box c_pt = {.x = cx, .y = cy, .width = 1, .height = 1};
    struct wlr_box c_buf = {0};
    logical_point_to_buffer_box(&c_pt, &sr.obox, sr.wo->wlr_output,
                                sr.trans_w, sr.trans_h, &c_buf);
    int px_x = c_buf.x - sr.buffer_box.x;
    int px_y = c_buf.y - sr.buffer_box.y;
    if (px_x < 0) {
        px_x = 0;
    } else if (px_x >= sr.buffer_box.width) {
        px_x = sr.buffer_box.width - 1;
    }
    if (px_y < 0) {
        px_y = 0;
    } else if (px_y >= sr.buffer_box.height) {
        px_y = sr.buffer_box.height - 1;
    }
    uint8_t center_rgba[4];
    memcpy(center_rgba,
           sr.pixels + ((size_t)px_y * (size_t)sr.buffer_box.width + (size_t)px_x) * 4, 4);

    int region_arr[4] = {
        sr.logical_box.x,
        sr.logical_box.y,
        sr.logical_box.width,
        sr.logical_box.height,
    };
    int bbox_arr[4] = {
        sr.buffer_box.x,
        sr.buffer_box.y,
        sr.buffer_box.width,
        sr.buffer_box.height,
    };

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", true);
    woodland_json_obj_str(&obj, "cmd", "screenshot");
    woodland_json_obj_str(&obj, "output", sr.wo->wlr_output->name);
    if (has_dump_ppm) {
        woodland_json_obj_str(&obj, "dumped_ppm", dump_ppm);
    }
    woodland_json_obj_key(&obj, "region");
    woodland_json_int_array(out, region_arr, 4);
    woodland_json_obj_key(&obj, "buffer_box");
    woodland_json_int_array(out, bbox_arr, 4);
    woodland_json_obj_int(&obj, "total_pixels", (long long)total_pixels);
    woodland_json_obj_int(&obj, "unique_colors", (long long)unique_colors);

    woodland_json_obj_key(&obj, "dominant");
    struct woodland_json_arr dom_arr;
    woodland_json_arr_open(&dom_arr, out);
    for (size_t i = 0; i < top_len; i++) {
        woodland_json_arr_sep(&dom_arr);
        uint32_t c = top[i].rgba;
        int crgba[4] = {
            (int)((c >> 24) & 0xFF),
            (int)((c >> 16) & 0xFF),
            (int)((c >> 8) & 0xFF),
            (int)(c & 0xFF),
        };
        char chex[8];
        snprintf(chex, sizeof(chex), "#%02x%02x%02x", crgba[0], crgba[1], crgba[2]);
        struct woodland_json_obj item;
        woodland_json_obj_open(&item, out);
        woodland_json_obj_str(&item, "hex", chex);
        woodland_json_obj_key(&item, "rgba");
        woodland_json_int_array(out, crgba, 4);
        woodland_json_obj_int(&item, "count", (long long)top[i].count);
        woodland_json_obj_close(&item);
    }
    woodland_json_arr_close(&dom_arr);

    bool center_has_expected = false;
    uint32_t center_expected_rgb = 0;
    const char *center_expected_src = NULL;
    bool center_is_titlebar = false;
    const struct woodland_view *center_view = NULL;

    char center_hex[8];
    snprintf(center_hex, sizeof(center_hex), "#%02x%02x%02x",
             center_rgba[0], center_rgba[1], center_rgba[2]);
    int center_rgba_vals[4] = {
        center_rgba[0],
        center_rgba[1],
        center_rgba[2],
        center_rgba[3],
    };

    woodland_json_obj_key(&obj, "center");
    struct woodland_json_obj cobj;
    woodland_json_obj_open(&cobj, out);
    woodland_json_obj_int(&cobj, "x", cx);
    woodland_json_obj_int(&cobj, "y", cy);
    woodland_json_obj_str(&cobj, "hex", center_hex);
    woodland_json_obj_key(&cobj, "rgba");
    woodland_json_int_array(out, center_rgba_vals, 4);
    woodland_json_obj_key(&cobj, "source");
    woodland_ipc_write_pixel_source(server, cx, cy, center_rgba, out,
                                    &center_has_expected, &center_expected_rgb,
                                    &center_expected_src, &center_is_titlebar,
                                    &center_view);
    woodland_json_obj_close(&cobj);

    bool have_match_target = false;
    uint32_t target_rgb = 0;
    const char *target_src = NULL;

    if (has_expect) {
        have_match_target = true;
        target_rgb = expect_arg_rgb;
        target_src = "argument";
    } else if (center_has_expected) {
        have_match_target = true;
        target_rgb = center_expected_rgb;
        target_src = center_expected_src;
    } else if (center_is_titlebar && center_view) {
        struct woodland_ssd_palette pal = {0};
        woodland_ssd_get_palette(center_view, &pal);
        have_match_target = true;
        target_rgb = center_view->activated ? pal.titlebar_active : pal.titlebar_inactive;
        target_src = center_view->activated ? "ssd.palette.titlebar_active"
                                            : "ssd.palette.titlebar_inactive";
    }

    if (have_match_target) {
        uint64_t match_count = 0;
        for (uint64_t i = 0; i < total_pixels; i++) {
            const uint8_t *px = sr.pixels + i * 4;
            if (woodland_ipc_rgb_delta(px, target_rgb) <= tolerance) {
                match_count++;
            }
        }
        int ratio_milli = total_pixels > 0 ? (int)((match_count * 1000ULL) / total_pixels) : 0;
        char mhex[8];
        snprintf(mhex, sizeof(mhex), "#%06x", target_rgb & 0xFFFFFFu);

        woodland_json_obj_key(&obj, "match");
        struct woodland_json_obj mobj;
        woodland_json_obj_open(&mobj, out);
        woodland_json_obj_str(&mobj, "expected_hex", mhex);
        woodland_json_obj_str(&mobj, "expected_source", target_src);
        woodland_json_obj_int(&mobj, "tolerance", tolerance);
        woodland_json_obj_int(&mobj, "count", (long long)match_count);
        woodland_json_obj_int(&mobj, "ratio_milli", ratio_milli);
        woodland_json_obj_close(&mobj);
    }

    woodland_json_obj_close(&obj);
    sampled_region_finish(&sr);
    return true;
}
