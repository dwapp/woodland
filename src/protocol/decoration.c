// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-decoration-unstable-v1：按窗口定制服务端装饰（SSD）。
 *
 * 协议只做一件事——让客户端微调"合成器替它画的那套装饰"：圆角半径、阴影、
 * 边框、标题栏可见性。它不改变装饰模式（那是 xdg-decoration 的事），也不
 * 碰几何（那是布局的事），所以本文件与 shell.c 没有调用关系：它只往
 * view->ssd_overrides 里写值，再由 SSD 自己去读。方向是单向的
 * （protocol → ssd），shell 不需要知道这个协议存在。
 *
 * 四类请求里 woodland 真正能画出来的只有两类：
 *
 *   边框    完整支持（宽度 + 颜色，scene rect 直接改）；
 *   标题栏  完整支持（hide 后边距只剩边框，命中区域同步收掉）；
 *   圆角    只记录不渲染：wlr_scene 的矩形与 surface 都没有"圆角裁剪"，
 *           真圆角要 shader/scenefx（effects 里程碑）。协议允许合成器把
 *           越界值 clamp 到支持的上界，这里就等于上界为 0；
 *   阴影    同上（要一层模糊 buffer，属于 effects）。
 *
 * 后两项不装作"生效了"：值存下来、wctl get_tree 里看得见、trace 里写明
 * 没有渲染后端。客户端据此可以判断，而不是对着一个改了没反应的值猜。
 *
 * 生命周期（协议原文 + 本实现）：
 *   - 同一个 surface 只能有一个存活的 context，第二个是 already_used；
 *   - 请求只对"本客户端拥有的 surface"生效，否则 invalid_surface；
 *   - wl_surface 销毁后 context 变 inert，请求静默忽略；
 *   - context 销毁时把边框/圆角/阴影恢复默认，**标题栏模式保留**（同 treeland：
 *     恢复标题栏会覆盖掉 xdg-decoration 协商出的结果，客户端想回默认就自己
 *     再发一次 set_titlebar_mode(show)）。
 */
#include "util/listener.h"
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#if WOODLAND_HAVE_XWAYLAND
#include <wlr/types/wlr_xwayland_surface.h>
#endif

#include "protocol_internal.h"
#include "ssd.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"

#include "treeland-decoration-unstable-v1-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define DECORATION_VERSION 1

/* 颜色分量在协议里是 [0, 255]；越界按"忽略整条请求"处理（同 treeland） */
#define DECORATION_MAX_COLOR 255

/* titlebar_mode 只有 show(0) / hide(1)，更大的值是无效枚举 */
#define DECORATION_TITLEBAR_MAX 2

struct woodland_proto_decoration;

/* 一个 treeland_decoration_context_v1 对象 */
struct woodland_deco_context {
    struct woodland_proto_decoration *dec;
    struct wl_resource *resource;
    struct wlr_surface *surface;

    /*
     * 目标视图。用户可能在 map 之前就建 context（甚至先建 context 再建
     * toplevel），所以这里允许为 NULL，请求到来时再解析一次。
     *
     * 它**不是**指针过期就会崩的那种字段：view 销毁时，挂在 toplevel /
     * xwayland surface destroy 上的监听器会把它清空（回调里不读 view，
     * 因为 woodland 自己的监听器注册得更早、已经把 view 释放了）。
     */
    struct woodland_view *view;
    bool inert;
    bool toplevel_listener_added;

    struct wl_list link; // woodland_proto_decoration.contexts
    struct wl_listener surface_destroy;
    struct wl_listener toplevel_destroy;
};

struct woodland_proto_decoration {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list contexts; // struct woodland_deco_context.link
};

/* ==================== 小工具 ==================== */

static bool color_valid(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r <= DECORATION_MAX_COLOR && g <= DECORATION_MAX_COLOR &&
           b <= DECORATION_MAX_COLOR && a <= DECORATION_MAX_COLOR;
}

/* 协议按 r,g,b,a 四个参数传，统一打包成 0xRRGGBBAA（见 ssd.h 的约定） */
static uint32_t color_pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static struct woodland_deco_context *context_from_resource(struct wl_resource *resource)
{
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static struct woodland_deco_context *context_for_surface(struct woodland_proto_decoration *dec,
                                                         struct wlr_surface *surface)
{
    struct woodland_deco_context *ctx;
    wl_list_for_each(ctx, &dec->contexts, link)
    {
        if (ctx->surface == surface) {
            return ctx;
        }
    }
    return NULL;
}

/*
 * 视图没了：只清指针，**不要读 ctx->view**。
 *
 * 这个回调与 woodland 自己的 xdg_toplevel/xsurface destroy 监听器挂在同一个
 * 信号上，而它注册得更早（new_toplevel 时），所以走到这里时 view 已经
 * free 了。把监听器摘掉也是必须的：信号所属的对象正在销毁，留着就是悬挂
 * 链表节点（后面 destroy_resource 再 wl_list_remove 会踩到已释放的内存）。
 */
static void context_handle_toplevel_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_deco_context *ctx = wl_container_of(listener, ctx, toplevel_destroy);
    (void)data;
    wl_list_remove(&ctx->toplevel_destroy.link);
    wl_list_init(&ctx->toplevel_destroy.link);
    ctx->toplevel_listener_added = false;
    ctx->view = NULL;
    ctx->inert = true;
}

/* wl_surface 销毁：协议规定的 inert 时机，和视图生命周期无关 */
static void context_handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_deco_context *ctx = wl_container_of(listener, ctx, surface_destroy);
    (void)data;
    wl_list_remove(&ctx->surface_destroy.link);
    wl_list_init(&ctx->surface_destroy.link);
    ctx->surface = NULL;
    ctx->view = NULL;
    ctx->inert = true;
}

/* 把"视图死了"这个信号接上：xdg 与 x11 各有一条销毁路径 */
static void context_watch_toplevel(struct woodland_deco_context *ctx)
{
    struct woodland_view *view = ctx->view;
    if (!view || ctx->toplevel_listener_added) {
        return;
    }
    struct wl_signal *signal = NULL;
    if (view->type == WOODLAND_VIEW_XDG && view->xdg_toplevel) {
        signal = &view->xdg_toplevel->events.destroy;
    }
#if WOODLAND_HAVE_XWAYLAND
    else if (view->type == WOODLAND_VIEW_XWAYLAND && view->xwayland_surface) {
        signal = &view->xwayland_surface->events.destroy;
    }
#endif
    if (!signal) {
        return;
    }
    woodland_listener_connect(&ctx->toplevel_destroy, signal, context_handle_toplevel_destroy,
                              "decoration.toplevel_destroy");
    ctx->toplevel_listener_added = true;
}

/*
 * 解析目标视图（惰性、可重复）。
 *
 * 惰性是因为 context 允许早于 map / 早于 toplevel 创建；可重复是因为第一次
 * 解析失败不代表以后也失败。解析成功后立刻接管 view 上的覆盖值并盯住它的
 * 销毁，这样后续请求不用每次都查。
 */
static struct woodland_view *context_resolve_view(struct woodland_deco_context *ctx)
{
    if (ctx->view) {
        return ctx->view;
    }
    if (ctx->inert || !ctx->surface) {
        return NULL;
    }
    struct woodland_view *view = woodland_view_from_surface(ctx->surface);
    if (!view) {
        return NULL;
    }
    ctx->view = view;
    view->ssd_overrides.managed = true;
    context_watch_toplevel(ctx);
    return view;
}

/* 覆盖值变了：让 SSD 重排一次（没 map 时是空操作，值已经记在 view 上） */
static void context_apply(struct woodland_deco_context *ctx)
{
    struct woodland_view *view = context_resolve_view(ctx);
    if (!view) {
        return;
    }
    woodland_ssd_deco_changed(view);
}

/* ==================== context 请求 ==================== */

static void context_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void context_set_corner_radius(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t radius)
{
    (void)client;
    struct woodland_deco_context *ctx = context_from_resource(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    struct woodland_view *view = context_resolve_view(ctx);
    if (!view) {
        return;
    }
    if (radius > WOODLAND_SSD_MAX_SIZE) {
        /* 协议允许 clamp；这里选择"忽略这一条"，与 treeland 一致，也好过
         * 把 4G 像素的圆角悄悄当成 1000 */
        wlr_log(WLR_INFO, "decoration: corner radius %u exceeds %d, ignored", radius,
                WOODLAND_SSD_MAX_SIZE);
        return;
    }
    view->ssd_overrides.corner_radius_set = true;
    view->ssd_overrides.corner_radius = (int)radius;
    /*
     * 只有边框/标题栏真的会改变画面：圆角要 shader 裁剪整块 surface，
     * wlr_scene 做不到（见文件头）。轨迹里写清楚"记了但没画"，免得
     * 排查的人以为是自己没生效。
     */
    woodland_trace_add("deco", view->id, "corner_radius=%u recorded (no render backend)", radius);
    context_apply(ctx);
}

static void context_set_shadow(struct wl_client *client, struct wl_resource *resource,
                               uint32_t radius, int32_t offset_x, int32_t offset_y, uint32_t r,
                               uint32_t g, uint32_t b, uint32_t a)
{
    (void)client;
    struct woodland_deco_context *ctx = context_from_resource(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    struct woodland_view *view = context_resolve_view(ctx);
    if (!view) {
        return;
    }
    if (radius > WOODLAND_SSD_MAX_SIZE) {
        wlr_log(WLR_INFO, "decoration: shadow radius %u exceeds %d, ignored", radius,
                WOODLAND_SSD_MAX_SIZE);
        return;
    }
    if (!color_valid(r, g, b, a)) {
        wlr_log(WLR_INFO, "decoration: shadow color out of [0,255]: %u %u %u %u, ignored", r, g, b,
                a);
        return;
    }
    view->ssd_overrides.shadow_set = true;
    view->ssd_overrides.shadow_radius = (int)radius;
    view->ssd_overrides.shadow_dx = offset_x;
    view->ssd_overrides.shadow_dy = offset_y;
    view->ssd_overrides.shadow_rgba = color_pack(r, g, b, a);
    woodland_trace_add("deco", view->id, "shadow radius=%u offset=%d,%d recorded (no render backend)",
                       radius, (int)offset_x, (int)offset_y);
    context_apply(ctx);
}

static void context_set_border(struct wl_client *client, struct wl_resource *resource,
                               uint32_t width, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    (void)client;
    struct woodland_deco_context *ctx = context_from_resource(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    struct woodland_view *view = context_resolve_view(ctx);
    if (!view) {
        return;
    }
    if (width > WOODLAND_SSD_MAX_SIZE) {
        wlr_log(WLR_INFO, "decoration: border width %u exceeds %d, ignored", width,
                WOODLAND_SSD_MAX_SIZE);
        return;
    }
    if (!color_valid(r, g, b, a)) {
        wlr_log(WLR_INFO, "decoration: border color out of [0,255]: %u %u %u %u, ignored", r, g, b,
                a);
        return;
    }
    view->ssd_overrides.border_set = true;
    view->ssd_overrides.border_width = (int)width;
    view->ssd_overrides.border_rgba = color_pack(r, g, b, a);
    /* 边框改变的是边距（0 宽 = 没有边框），几何会跟着变，所以这里要重排 */
    woodland_trace_add("deco", view->id, "border width=%u rgba=%08x", width,
                       (unsigned)view->ssd_overrides.border_rgba);
    context_apply(ctx);
}

static void context_set_titlebar_mode(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t mode)
{
    (void)client;
    struct woodland_deco_context *ctx = context_from_resource(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    struct woodland_view *view = context_resolve_view(ctx);
    if (!view) {
        return;
    }
    if (mode >= DECORATION_TITLEBAR_MAX) {
        /* 协议原文：枚举以外的值静默忽略，当前可见性保持不变 */
        wlr_log(WLR_INFO, "decoration: titlebar mode %u is not a valid enum, ignored", mode);
        return;
    }
    view->ssd_overrides.titlebar_set = true;
    view->ssd_overrides.titlebar_hidden = (mode == TREELAND_DECORATION_CONTEXT_V1_TITLEBAR_MODE_HIDE);
    /* 隐藏标题栏会改边距（top 只剩边框），命中区域也随之收掉 */
    woodland_trace_add("deco", view->id, "titlebar=%s",
                       view->ssd_overrides.titlebar_hidden ? "hide" : "show");
    context_apply(ctx);
}

static const struct treeland_decoration_context_v1_interface context_impl = {
    .destroy = context_handle_destroy,
    .set_corner_radius = context_set_corner_radius,
    .set_shadow = context_set_shadow,
    .set_border = context_set_border,
    .set_titlebar_mode = context_set_titlebar_mode,
};

/*
 * context 资源销毁。
 *
 * 恢复默认只做边框/圆角/阴影（treeland 同取舍）：标题栏模式留着，因为它和
 * xdg-decoration 的协商结果共用一处可见性，盲目恢复会把客户端刚谈下来的
 * CSD 变成"又长出标题栏"。
 */
static void context_resource_destroy(struct wl_resource *resource)
{
    struct woodland_deco_context *ctx = context_from_resource(resource);
    if (!ctx) {
        return;
    }
    if (ctx->view) {
        struct woodland_view *view = ctx->view;
        view->ssd_overrides.managed = false;
        view->ssd_overrides.border_set = false;
        view->ssd_overrides.corner_radius_set = false;
        view->ssd_overrides.shadow_set = false;
        woodland_trace_add("deco", view->id, "context destroyed, border/radius/shadow reverted");
        woodland_ssd_deco_changed(view);
    }
    if (ctx->surface_destroy.link.prev) {
        wl_list_remove(&ctx->surface_destroy.link);
    }
    if (ctx->toplevel_listener_added) {
        wl_list_remove(&ctx->toplevel_destroy.link);
        ctx->toplevel_listener_added = false;
    }
    wl_list_remove(&ctx->link);
    free(ctx);
}

/* ==================== manager ==================== */

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_get_decoration_context(struct wl_client *client, struct wl_resource *resource,
                                           uint32_t id, struct wl_resource *surface_resource)
{
    struct woodland_proto_decoration *dec = wl_resource_get_user_data(resource);

    if (!surface_resource) {
        wl_resource_post_error(resource, TREELAND_DECORATION_MANAGER_V1_ERROR_INVALID_SURFACE,
                               "surface is NULL");
        return;
    }
    /* 不能替别人家的 surface 设装饰：跨客户端改窗口外观是明确的越权 */
    if (wl_resource_get_client(surface_resource) != client) {
        wl_resource_post_error(resource, TREELAND_DECORATION_MANAGER_V1_ERROR_INVALID_SURFACE,
                               "surface is not owned by the requesting client");
        return;
    }
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    if (!surface) {
        wl_resource_post_error(resource, TREELAND_DECORATION_MANAGER_V1_ERROR_INVALID_SURFACE,
                               "invalid wl_surface");
        return;
    }
    /* 一个 surface 至多一个 context：两个对象各自改同一个窗口，谁后发谁赢，
     * 客户端自己都说不清当前值是什么 */
    if (context_for_surface(dec, surface)) {
        wl_resource_post_error(resource, TREELAND_DECORATION_MANAGER_V1_ERROR_ALREADY_USED,
                               "a decoration context already exists for this surface");
        return;
    }

    struct wl_resource *ctx_resource =
        wl_resource_create(client, &treeland_decoration_context_v1_interface,
                           wl_resource_get_version(resource), id);
    if (!ctx_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct woodland_deco_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        wl_resource_destroy(ctx_resource);
        wl_client_post_no_memory(client);
        return;
    }
    ctx->dec = dec;
    ctx->resource = ctx_resource;
    ctx->surface = surface;
    wl_list_init(&ctx->surface_destroy.link);
    wl_list_init(&ctx->toplevel_destroy.link);
    /* 先挂析构再干活：后面任何一步失败都靠 wl_resource_destroy 统一收尾 */
    wl_resource_set_implementation(ctx_resource, &context_impl, ctx, context_resource_destroy);
    wl_list_insert(&dec->contexts, &ctx->link);

    woodland_listener_connect(&ctx->surface_destroy, &surface->events.destroy,
                              context_handle_surface_destroy, "decoration.surface_destroy");
    /* 立即解析一次：绝大多数客户端在 map 前后就会发请求，早接上早生效 */
    context_resolve_view(ctx);
}

static const struct treeland_decoration_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_decoration_context = manager_get_decoration_context,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_decoration *dec = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_decoration_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, dec, NULL);
}

/* ==================== 由编排层调用的入口 ==================== */

bool woodland_proto_decoration_init(struct woodland_proto *proto)
{
    struct woodland_server *server = proto->server;

    struct woodland_proto_decoration *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        wlr_log(WLR_ERROR, "cannot allocate decoration protocol state");
        return false;
    }
    dec->server = server;
    wl_list_init(&dec->contexts);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->decoration = dec;

    dec->global = wl_global_create(server->display, &treeland_decoration_manager_v1_interface,
                                   DECORATION_VERSION, dec, manager_bind);
    if (!dec->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-decoration global");
        return false;
    }
    return true;
}

void woodland_proto_decoration_finish(struct woodland_proto *proto)
{
    struct woodland_proto_decoration *dec = proto->decoration;
    if (!dec) {
        return;
    }
    if (dec->global) {
        wl_global_destroy(dec->global);
        dec->global = NULL;
    }
    /*
     * 客户端资源正常都已随 display 销毁；这里兜底释放包装结构。先把
     * user_data 置空，万一还有活着的资源，它的析构函数不会再二次 free。
     */
    struct woodland_deco_context *ctx, *tmp;
    wl_list_for_each_safe(ctx, tmp, &dec->contexts, link)
    {
        if (ctx->surface_destroy.link.prev) {
            wl_list_remove(&ctx->surface_destroy.link);
        }
        if (ctx->toplevel_listener_added) {
            wl_list_remove(&ctx->toplevel_destroy.link);
        }
        wl_list_remove(&ctx->link);
        if (ctx->resource) {
            wl_resource_set_user_data(ctx->resource, NULL);
        }
        free(ctx);
    }
    free(dec);
    proto->decoration = NULL;
}
