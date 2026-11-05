// SPDX-License-Identifier: GPL-2.0-only
/*
 * ext-background-effect-v1：让半透明窗口声明"我身后这块要模糊"。
 *
 * 协议本身不画任何东西，它只提供一个可提交的 blur region；真正把像素糊掉
 * 的是 efecto 模块（src/effect/effect.c，走 scenefx）。所以本文件与
 * decoration.c 同一副骨架：协议只写状态，渲染端自己来读。
 *
 * 状态必须**双缓冲**：set_blur_region 写 pending，下一次 wl_surface.commit
 * 才变 current。这不用自己记 commit 序号——wlr_surface_synced 就是干这个的
 * （和 wlroots 上游实现同一条路），state 结构里的 pixman region 由它负责
 * 初始化/拷贝/释放。
 *
 * 能力位按构建决定：没编 effects（没有 scenefx）时 capabilities 是 0，
 * 客户端据此知道"这台的合成器不会模糊"，而不是设了区域一点反应都没有。
 */
#include <pixman.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "protocol_internal.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"

#include "ext-background-effect-v1-protocol.h"

/* 协议在 staging，版本写死 1：上游升版要显式跟 */
#define BG_EFFECT_VERSION 1

/* 一份可提交的 blur region；pending/current 各一份，wlr_surface_synced 负责搬 */
struct woodland_bg_state {
    pixman_region32_t blur_region;
};

struct woodland_bg_surface {
    struct woodland_proto_bg_effect *family;
    struct wl_resource *resource;
    struct wlr_surface *surface;
    struct wlr_addon addon;   /* 挂在 surface->addons 上，随 surface 销毁 */
    struct wlr_surface_synced synced;
    struct woodland_bg_state pending;
    struct woodland_bg_state current;
    bool inert; /* surface 已销毁：请求静默忽略（与上游语义一致） */
};

struct woodland_proto_bg_effect {
    struct woodland_server *server;
    struct wl_global *global;
    uint32_t capabilities; /* ext_background_effect_manager_v1_capability 位或 */
};

static const struct ext_background_effect_manager_v1_interface manager_impl;
static const struct ext_background_effect_surface_v1_interface surface_impl;
static const struct wlr_addon_interface surface_addon_impl;
static const struct wlr_surface_synced_impl surface_synced_impl;

/* ==================== 状态同步 ==================== */

static void bg_state_init(void *data)
{
    struct woodland_bg_state *state = data;
    pixman_region32_init(&state->blur_region);
}

static void bg_state_finish(void *data)
{
    struct woodland_bg_state *state = data;
    pixman_region32_fini(&state->blur_region);
}

static void bg_state_move(void *dst_data, void *src_data)
{
    struct woodland_bg_state *dst = dst_data;
    struct woodland_bg_state *src = src_data;
    pixman_region32_copy(&dst->blur_region, &src->blur_region);
}

/*
 * 提交后：区域已经变成 current，让渲染端重算这个窗口的模糊。
 *
 * 这里没有再判断"区域变没变"：比较两个 region 的成本和重算一次 blur 节点
 * 差不多，而 commit 本来就只在客户端真的提交时发生。
 */
static void bg_state_commit(struct wlr_surface_synced *synced)
{
    struct woodland_bg_surface *bg = wl_container_of(synced, bg, synced);
    struct woodland_view *view = woodland_view_from_surface(bg->surface);
    if (!view) {
        return;
    }
    /*
     * 状态已经落到 current：把"要糊哪一块"推给渲染端。协议层不碰 scenefx，
     * 只交一个 surface 局部坐标的 region，换算与裁剪由 effect 模块负责。
     */
    woodland_effect_set_blur_region(view, &bg->current.blur_region, bg->surface->current.width,
                                    bg->surface->current.height);
    woodland_trace_add("bg_effect", view->id, "blur=%s", view->effect.blur ? "on" : "off");
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
    .state_size = sizeof(struct woodland_bg_state),
    .init_state = bg_state_init,
    .finish_state = bg_state_finish,
    .move_state = bg_state_move,
    .commit = bg_state_commit,
};

/* ==================== 查找 ==================== */

static struct woodland_bg_surface *bg_from_resource(struct wl_resource *resource)
{
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static struct woodland_bg_surface *bg_from_surface(struct wlr_surface *surface)
{
    if (!surface) {
        return NULL;
    }
    struct wlr_addon *addon =
        wlr_addon_find(&surface->addons, NULL, &surface_addon_impl);
    if (!addon) {
        return NULL;
    }
    struct woodland_bg_surface *bg = wl_container_of(addon, bg, addon);
    return bg;
}

/* ==================== 生命周期 ==================== */

static void bg_destroy(struct woodland_bg_surface *bg)
{
    if (!bg) {
        return;
    }
    wlr_surface_synced_finish(&bg->synced);
    wlr_addon_finish(&bg->addon);
    wl_resource_set_user_data(bg->resource, NULL);
    free(bg);
}

/* wl_surface 先没了：对象跟着回收，客户端手里的 resource 变 inert */
static void bg_addon_destroy(struct wlr_addon *addon)
{
    struct woodland_bg_surface *bg = wl_container_of(addon, bg, addon);
    /* 区域随 surface 一起消失，窗口上的模糊也要跟着撤掉 */
    struct woodland_view *view = woodland_view_from_surface(bg->surface);
    bg->inert = true;
    if (view) {
        woodland_effect_set_blur_region(view, NULL, 0, 0);
    }
    bg_destroy(bg);
}

static const struct wlr_addon_interface surface_addon_impl = {
    .name = "ext_background_effect_surface_v1",
    .destroy = bg_addon_destroy,
};

static void bg_handle_resource_destroy(struct wl_resource *resource)
{
    struct woodland_bg_surface *bg = bg_from_resource(resource);
    if (!bg) {
        return; /* 已经随 surface 回收过了 */
    }
    struct woodland_view *view = woodland_view_from_surface(bg->surface);
    bg_destroy(bg);
    if (view) {
        /* 客户端主动撤掉对象：效果立即消失（协议：区域下次提交时移除） */
        woodland_effect_set_blur_region(view, NULL, 0, 0);
    }
}

/* ==================== surface 请求 ==================== */

static void surface_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_handle_set_blur_region(struct wl_client *client, struct wl_resource *resource,
                                           struct wl_resource *region_resource)
{
    (void)client;
    struct woodland_bg_surface *bg = bg_from_resource(resource);
    if (!bg || bg->inert) {
        return;
    }
    if (region_resource) {
        const pixman_region32_t *region = wlr_region_from_resource(region_resource);
        pixman_region32_copy(&bg->pending.blur_region, region);
    } else {
        /* NULL region = 撤掉效果（协议原文：removes the effect） */
        pixman_region32_clear(&bg->pending.blur_region);
    }
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
    .destroy = surface_handle_destroy,
    .set_blur_region = surface_handle_set_blur_region,
};

/* ==================== manager ==================== */

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_get_background_effect(struct wl_client *client,
                                                 struct wl_resource *manager_resource, uint32_t id,
                                                 struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    if (bg_from_surface(surface)) {
        wl_resource_post_error(manager_resource,
                               EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                               "surface already has a background effect object");
        return;
    }

    struct woodland_bg_surface *bg = calloc(1, sizeof(*bg));
    if (!bg) {
        wl_resource_post_no_memory(manager_resource);
        return;
    }
    bg->family = wl_resource_get_user_data(manager_resource);
    bg->surface = surface;

    if (!wlr_surface_synced_init(&bg->synced, surface, &surface_synced_impl, &bg->pending,
                                 &bg->current)) {
        free(bg);
        wl_resource_post_no_memory(manager_resource);
        return;
    }

    bg->resource = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
                                      wl_resource_get_version(manager_resource), id);
    if (!bg->resource) {
        wlr_surface_synced_finish(&bg->synced);
        free(bg);
        wl_resource_post_no_memory(manager_resource);
        return;
    }
    wl_resource_set_implementation(bg->resource, &surface_impl, bg, bg_handle_resource_destroy);
    wlr_addon_init(&bg->addon, &surface->addons, NULL, &surface_addon_impl);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_background_effect = manager_handle_get_background_effect,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_bg_effect *family = data;
    struct wl_resource *resource = wl_resource_create(
        client, &ext_background_effect_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, family, NULL);
    /* 能力位在 bind 时发一次（协议：bonud 时以及能力变化时） */
    ext_background_effect_manager_v1_send_capabilities(resource, family->capabilities);
}

/* ==================== 族初始化 ==================== */

bool woodland_proto_bg_effect_init(struct woodland_proto *proto)
{
    struct woodland_server *server = proto->server;

    struct woodland_proto_bg_effect *family = calloc(1, sizeof(*family));
    if (!family) {
        wlr_log(WLR_ERROR, "cannot allocate background-effect protocol state");
        return false;
    }
    family->server = server;
    proto->bg_effect = family;

    /*
     * 没有 scenefx 就如实报"不支持模糊"：capability=0。
     * 全局照样注册，客户端的 get_background_effect 不会报错，只是设了区域
     * 也不会有效果——协议允许这样（capability 决定客户端要不要用）。
     */
    family->capabilities = WOODLAND_HAVE_EFFECTS
                               ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR
                               : 0;
    family->global =
        wl_global_create(server->display, &ext_background_effect_manager_v1_interface,
                         BG_EFFECT_VERSION, family, manager_bind);
    if (!family->global) {
        wlr_log(WLR_ERROR, "failed to create ext-background-effect-v1 global");
        return false;
    }
    return true;
}

void woodland_proto_bg_effect_finish(struct woodland_proto *proto)
{
    struct woodland_proto_bg_effect *family = proto->bg_effect;
    if (!family) {
        return;
    }
    /* 资源随客户端/display 一起走，这里只摘全局 */
    wl_global_destroy(family->global);
    free(family);
    proto->bg_effect = NULL;
}
