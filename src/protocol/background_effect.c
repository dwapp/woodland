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
 * 能力位跟着**运行期配置**（[effects] enabled/blur）走，不跟编译期：scenefx 一定
 * 在，但关掉模糊时如实报 capability=0，客户端据此知道"这台的合成器不会模糊"，
 * 而不是设了区域一点反应都没有。bind 时现算，配置热重载后再广播一次。
 */
#include <pixman.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#include "effect.h"
#include "layer.h"
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
    /*
     * 已 bind 的 manager 资源。配置热重载可能把模糊关掉，能力位得再发一次
     * （协议允许重复发送，客户端以最后一条为准），所以要留住资源引用。
     * 不能用 wl_resource->link：那个字段属于 client 的资源链表。
     */
    struct wl_list managers;
};

struct bg_manager {
    struct wl_list link;
    struct wl_resource *resource;
    struct woodland_proto_bg_effect *family;
};

static uint32_t bg_capabilities(struct woodland_proto_bg_effect *family)
{
#if WOODLAND_HAVE_EFFECTS
    return woodland_effect_server_blur_enabled(family->server)
               ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR
               : 0;
#else
    (void)family;
    return 0;
#endif
}

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
 * 把 current 区域推给渲染端。
 *
 * 目标不一定是顶层窗口：面板、Dock、启动器这些 layer surface 同样会用这个
 * 协议（"把我身后那块糊掉"），它们没有 view 对象，得走 effect 模块的另一条
 * 入口。两条都不匹配（cursor、输入法这类没有可挂 blur 的树）时写一条 trace
 * 说明被忽略了，而不是让客户端以为设了就有用。
 */
static void bg_apply(struct woodland_bg_surface *bg)
{
    struct woodland_view *view = woodland_view_from_surface(bg->surface);
    if (view) {
        /*
         * 状态已经落到 current：把"要糊哪一块"推给渲染端。协议层不碰 scenefx，
         * 只交一个 surface 局部坐标的 region，换算与裁剪由 effect 模块负责。
         */
        woodland_effect_set_blur_region(view, &bg->current.blur_region,
                                        bg->surface->current.width,
                                        bg->surface->current.height);
        woodland_trace_add("bg_effect", view->id, "blur=%s", view->effect.blur ? "on" : "off");
        return;
    }

    struct woodland_layer_surface *layer =
        woodland_layer_surface_from_wlr_surface(bg->surface);
    if (layer) {
        struct wlr_box box = {0};
        bool on = woodland_effect_blur_box(&bg->current.blur_region, bg->surface->current.width,
                                           bg->surface->current.height, &box);
        woodland_effect_layer_blur(layer, on ? &box : NULL);
        woodland_trace_add("bg_effect", 0, "layer blur=%s", on ? "on" : "off");
        return;
    }

    woodland_trace_add("bg_effect", 0, "ignored (surface has no view or layer)");
}

/* 撤掉效果：view 清状态，layer surface 直接关掉 blur 节点 */
static void bg_clear(struct woodland_bg_surface *bg)
{
    struct woodland_view *view = woodland_view_from_surface(bg->surface);
    if (view) {
        woodland_effect_set_blur_region(view, NULL, 0, 0);
        return;
    }
    struct woodland_layer_surface *layer =
        woodland_layer_surface_from_wlr_surface(bg->surface);
    if (layer) {
        woodland_effect_layer_blur(layer, NULL);
    }
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
    bg_apply(bg);
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
    bg->inert = true;
    bg_clear(bg);
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
    bg_clear(bg);
    bg_destroy(bg);
    /* 客户端主动撤掉对象：效果立即消失（协议：区域下次提交时移除） */
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
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                               "surface has been destroyed");
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
    struct bg_manager *manager = wl_resource_get_user_data(manager_resource);
    bg->family = manager ? manager->family : NULL;
    if (!bg->family) {
        free(bg);
        wl_resource_post_no_memory(manager_resource);
        return;
    }
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

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct bg_manager *manager = wl_resource_get_user_data(resource);
    if (!manager) {
        return;
    }
    wl_list_remove(&manager->link);
    free(manager);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_bg_effect *family = data;
    struct wl_resource *resource = wl_resource_create(
        client, &ext_background_effect_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct bg_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    manager->resource = resource;
    manager->family = family;
    wl_list_insert(&family->managers, &manager->link);
    wl_resource_set_implementation(resource, &manager_impl, manager,
                                   manager_resource_destroy);
    /*
     * 能力位在 bind 时现算一次（协议：bind 时以及能力变化时发）。这里不能读
     * 初始化时的快照——全局在配置加载之前就建好了，那时 server->config 还是 NULL。
     */
    ext_background_effect_manager_v1_send_capabilities(resource, bg_capabilities(family));
}

void woodland_proto_bg_effect_refresh(struct woodland_proto *proto)
{
    struct woodland_proto_bg_effect *family = proto ? proto->bg_effect : NULL;
    if (!family) {
        return;
    }
    uint32_t caps = bg_capabilities(family);
    struct bg_manager *manager;
    wl_list_for_each(manager, &family->managers, link)
    {
        ext_background_effect_manager_v1_send_capabilities(manager->resource, caps);
    }
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
    wl_list_init(&family->managers);
    proto->bg_effect = family;

    /*
     * 全局无条件注册，能力位按当次的 [effects] 现算（见 manager_bind）：
     * 关掉模糊时客户端拿到 0，不会去设区域，但也不会因为协议缺失而失败。
     */
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
    if (family->global) {
        wl_global_destroy(family->global);
        family->global = NULL;
    }
    struct bg_manager *manager, *tmp;
    wl_list_for_each_safe(manager, tmp, &family->managers, link)
    {
        wl_list_remove(&manager->link);
        if (manager->resource) {
            wl_resource_set_user_data(manager->resource, NULL);
        }
        free(manager);
    }
    free(family);
    proto->bg_effect = NULL;
}
