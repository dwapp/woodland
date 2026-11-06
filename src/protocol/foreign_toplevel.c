// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-foreign-toplevel-manager-unstable-v3：任务栏 / Dock 观察并操作窗口。
 *
 * 语义参考 treeland 的 src/modules/foreign-toplevel/，但**只搬协议语义，不搬
 * 它的视觉部分**。treeland 的 dock preview 是一整套 QML 弹层（缩略图、底板
 * tint / border / shadow、tooltip 与预览之间的收缩淡出动画、动画在途重定向），
 * woodland 既没有那套 popover 渲染器，也不打算有。所以按能力分档：
 *
 *   完整：toplevel 发现与 closed、identifier、pid、title、app_id、state、
 *         parent、output_enter / output_leave、done；handle 上的 activate、
 *         close、set/unset_maximized、set/unset_minimized、
 *         set/unset_fullscreen 全部真的接到 shell 的窗口状态机上。
 *   部分：dock preview context 的对象生命周期与请求校验完整（invalid_surface
 *         与 context_already_exists 照样报协议错误），但 show /
 *         show_tooltip_surface 只记账不弹任何东西；因此 enter / leave 永远不
 *         会发（没有可悬停的区域，协议也只在"preview 可见"时才允许发）。
 *   只记录：set_icon_geometry。"图标在屏幕上的矩形"是给最小化动画用的提示，
 *         woodland 没有最小化动画；值存下来、wctl 里看得见，不假装生效。
 *
 * attention 状态位恒为 false：woodland 没有 urgency / 闪屏请求这一层状态机，
 * 编出"客户端在请求关注"会是凭空捏造的信号（同 decoration 对阴影的取舍）。
 *
 * 两处与 treeland 不同的实现选择，理由都写在下面对应函数上：
 *   1. done 不做 idle 延迟批处理，改成本族每次刷新都按差量发一条：刷新本来
 *      就是"一次状态变化"的粒度，差量保证了不会出现半套属性；
 *   2. tooltip surface 不抢 role。treeland 会把 surface 永久钉成
 *      treeland_dock_tooltip；既然我们不画 tooltip，抢走 role 只会让客户端的
 *      surface 永远不能再用，比不生效更糟。改成"已有别的 role 就报
 *      invalid_surface"，把错误语义留下，副作用去掉。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "config.h"

#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "focus.h"
#include "output.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "trace.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"

#include "treeland-foreign-toplevel-manager-unstable-v3-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define FTL_VERSION 1

/* state 数组最多 5 个（maximized / minimized / activated / fullscreen / attention） */
#define FTL_STATE_MAX 5

struct woodland_proto_toplevel;

/* 一个 treeland_foreign_toplevel_manager_v3 资源（一次 bind） */
struct toplevel_binding {
    struct woodland_proto_toplevel *ftl;
    struct wl_resource *resource;
    /* stop 之后不再给这个 binding 发 toplevel 事件（已发出的 handle 保留） */
    bool stopped;
    struct wl_list link; // ftl->bindings
};

/* 一个已 map 的窗口 */
struct toplevel_entry {
    struct woodland_proto_toplevel *ftl;
    /*
     * 目标视图。closed 之后 entry 立刻销毁，所以指针不需要"视图已死"的
     * 兜底监听器：视图销毁时 shell 会先发 view_unmapped（见 shell.c），
     * 我们在这个回调里把 entry 收掉。方向是 shell → 协议层，协议层不再
     * 反向挂监听器（挂上去反而会踩"回调里 view 已经 free"的坑）。
     */
    struct woodland_view *view;
    /*
     * identifier：一个窗口一个，跨 handle 共享；单调递增且**不复用**，
     * 协议要求"toplevel 消失后该值不得再被用"。uint32 回绕在单次会话里
     * 不可能发生（4G 个窗口），不额外处理。
     */
    uint32_t identifier;
    struct wl_list handles; // struct toplevel_handle.link
    struct wl_list link;    // ftl->entries

    /* set_icon_geometry：只记录（最小化动画的提示，woodland 没有动画） */
    bool icon_set;
    int icon_x, icon_y, icon_width, icon_height;
};

/* 一个 handle 上"已经发出去"的输出 */
struct sent_output {
    struct wl_list link;
    struct wlr_output *output;
};

/* 一个 treeland_foreign_toplevel_handle_v3 资源（一个 binding 一个） */
struct toplevel_handle {
    struct woodland_proto_toplevel *ftl;
    /*
     * 窗口没了之后 entry 会被销毁，这里置空；handle 资源本身活到客户端
     * 发 destroy 为止（协议要求"manager/handle 各自独立销毁"）。
     */
    struct toplevel_entry *entry;
    struct wl_resource *resource;
    /*
     * 只认 client 不认 binding：manager 对象销毁不得影响已发出的 handle
     * （协议原文），攥着 binding 指针就会在 binding 释放后悬空。同一客户端
     * bind 两次会得到两套 handle，parent 反查按 client 取第一个——那是病态
     * 用法，不值得为它加一层 binding 标识。
     */
    struct wl_client *client;
    bool closed;
    struct wl_list link; // entry->handles

    /* 已下发的快照，用来做差量（协议按 done 成批交付，重复发是噪音） */
    bool sent_identifier;
    bool sent_pid;
    char *title;
    char *app_id;
    bool state_sent;
    uint32_t state[FTL_STATE_MAX];
    size_t state_len;
    struct wl_resource *sent_parent;
    struct wl_list outputs; // struct sent_output.link
};

/* 一个 treeland_dock_preview_context_v3 资源 */
struct dock_context {
    struct woodland_proto_toplevel *ftl;
    struct wl_resource *resource;
    /* 锚点 surface：preview / tooltip 的定位基准，仅用于校验与诊断 */
    struct wlr_surface *anchor;
    bool inert;
    struct wl_list link; // ftl->contexts
    struct wl_listener anchor_destroy;

    /* 最后一次请求的样子（不渲染，但要让 wctl 和 trace 说得清收到了什么） */
    bool preview_visible;
    bool tooltip_visible;
    uint32_t preview_identifiers;
    uint32_t preview_matched;
    int32_t x, y;
    uint32_t direction;
};

/* wl_output 的 bind 监听：客户端晚于 handle 绑定 wl_output 时补发 enter */
struct output_watch {
    struct woodland_proto_toplevel *ftl;
    struct wlr_output *output;
    struct wl_list link; // ftl->output_watches
    struct wl_listener bind;
    struct wl_listener destroy;
};

struct woodland_proto_toplevel {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list bindings;       // struct toplevel_binding.link
    struct wl_list entries;        // struct toplevel_entry.link
    struct wl_list contexts;       // struct dock_context.link
    struct wl_list output_watches; // struct output_watch.link
    uint32_t next_identifier;

    /* 诊断：最近一次请求被怎么处置（wctl get_tree.toplevel_manager） */
    uint32_t request_count;
    const char *last_request;
    const char *last_outcome;
    uint32_t dock_request_count;
    const char *last_dock_request;

    /* shell 的视图通知（协议层只读，依赖方向 shell → protocol） */
    bool watching_views;
    struct wl_listener view_mapped;
    struct wl_listener view_unmapped;
    struct wl_listener view_changed;
};

/* ==================== 状态编码（纯函数，单测直接调） ==================== */

size_t woodland_protocol_toplevel_state_encode(const struct woodland_toplevel_state *state,
                                               uint32_t *out, size_t cap)
{
    if (!state || !out) {
        return 0;
    }
    size_t n = 0;
    /*
     * 顺序与 XML 里 state 枚举的取值一致（0..4），升序发送：客户端按数组
     * 里的值置位，顺序本身不影响语义，但固定下来才能被断言。
     */
    if (state->maximized && n < cap) {
        out[n++] = TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_MAXIMIZED;
    }
    if (state->minimized && n < cap) {
        out[n++] = TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_MINIMIZED;
    }
    if (state->activated && n < cap) {
        out[n++] = TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_ACTIVATED;
    }
    if (state->fullscreen && n < cap) {
        out[n++] = TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_FULLSCREEN;
    }
    if (state->attention && n < cap) {
        out[n++] = TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_STATE_ATTENTION;
    }
    return n;
}

/* ==================== 小工具 ==================== */

static struct woodland_view *handle_view(struct toplevel_handle *h)
{
    if (!h || h->closed || !h->entry) {
        return NULL;
    }
    return h->entry->view;
}

static struct toplevel_entry *entry_for_view(struct woodland_proto_toplevel *ftl,
                                             struct woodland_view *view)
{
    if (!ftl || !view) {
        return NULL;
    }
    struct toplevel_entry *e;
    wl_list_for_each(e, &ftl->entries, link)
    {
        if (e->view == view) {
            return e;
        }
    }
    return NULL;
}

static bool streq(const char *a, const char *b)
{
    if (!a || !b) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

/* 最近一次请求的处置：wctl 与 trace 共用（字符串是脚本会 grep 的稳定值） */
static void record_request(struct woodland_proto_toplevel *ftl, uint32_t view_id, const char *what,
                           const char *outcome)
{
    if (!ftl) {
        return;
    }
    ftl->request_count++;
    ftl->last_request = what;
    ftl->last_outcome = outcome;
    woodland_trace_add("ftl", view_id, "request %s -> %s", what, outcome);
}

static void record_dock_request(struct woodland_proto_toplevel *ftl, const char *what,
                                const char *detail)
{
    if (!ftl) {
        return;
    }
    ftl->dock_request_count++;
    ftl->last_dock_request = what;
    woodland_trace_add("ftl", 0, "dock %s %s", what, detail);
}

/* ==================== output_enter / output_leave ==================== */

static bool sent_output_has(struct toplevel_handle *h, struct wlr_output *output)
{
    struct sent_output *so;
    wl_list_for_each(so, &h->outputs, link)
    {
        if (so->output == output) {
            return true;
        }
    }
    return false;
}

static void sent_output_add(struct toplevel_handle *h, struct wlr_output *output)
{
    struct sent_output *so = calloc(1, sizeof(*so));
    if (!so) {
        return;
    }
    so->output = output;
    wl_list_insert(&h->outputs, &so->link);
}

static void sent_output_drop(struct toplevel_handle *h, struct wlr_output *output)
{
    struct sent_output *so, *tmp;
    wl_list_for_each_safe(so, tmp, &h->outputs, link)
    {
        if (so->output == output) {
            wl_list_remove(&so->link);
            free(so);
            return;
        }
    }
}

/*
 * 给这个 handle 的客户端发一次 enter/leave。
 *
 * 参数是 wl_output 对象，得先在这个 wl_output 的资源里找客户端自己 bind 的那
 * 一份——没找到就返回 false，调用方才好判断"这次没发出去，等它会话里绑上
 * wl_output 再补"（见 output_watch_bind）。
 */
static bool send_output_event(struct toplevel_handle *h, struct wlr_output *output, bool enter)
{
    if (!h->resource) {
        return false;
    }
    bool sent = false;
    struct wl_resource *output_resource;
    wl_resource_for_each(output_resource, &output->resources)
    {
        if (wl_resource_get_client(output_resource) != h->client) {
            continue;
        }
        if (enter) {
            treeland_foreign_toplevel_handle_v3_send_output_enter(h->resource, output_resource);
        } else {
            treeland_foreign_toplevel_handle_v3_send_output_leave(h->resource, output_resource);
        }
        sent = true;
    }
    return sent;
}

/* ==================== 属性差量与发送 ==================== */

static void handle_send_done(struct toplevel_handle *h)
{
    if (h->resource && !h->closed) {
        treeland_foreign_toplevel_handle_v3_send_done(h->resource);
    }
}

static uint32_t entry_pid(struct toplevel_entry *e)
{
    struct woodland_view *view = e->view;
    if (!view) {
        return 0;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        return (xsurface && xsurface->pid > 0) ? (uint32_t)xsurface->pid : 0;
    }
#endif
    if (!view->xdg_toplevel || !view->xdg_toplevel->base->resource) {
        return 0;
    }
    struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->base->resource);
    if (!client) {
        return 0;
    }
    pid_t pid = 0;
    uid_t uid;
    gid_t gid;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    return pid > 0 ? (uint32_t)pid : 0;
}

static void entry_collect_state(struct toplevel_entry *e, struct woodland_toplevel_state *st)
{
    struct woodland_view *view = e->view;
    *st = (struct woodland_toplevel_state){
        .maximized = view->maximized,
        .minimized = view->minimized,
        .activated = view->activated,
        .fullscreen = view->fullscreen,
        /* woodland 没有 urgency / attention 状态机，不凭空捏造 */
        .attention = false,
    };
}

/* 父窗口的 handle 必须是"同一个客户端拿到的那个"，否则是跨客户端串对象 */
static struct wl_resource *handle_parent_resource(struct toplevel_handle *h,
                                                  struct woodland_view *parent)
{
    if (!parent) {
        return NULL;
    }
    struct toplevel_entry *pe = entry_for_view(h->ftl, parent);
    if (!pe) {
        return NULL;
    }
    struct toplevel_handle *ph;
    wl_list_for_each(ph, &pe->handles, link)
    {
        if (ph->client == h->client && ph->resource && !ph->closed) {
            return ph->resource;
        }
    }
    return NULL;
}

static bool handle_refresh_outputs(struct toplevel_handle *h);
static void ftl_watch_outputs(struct woodland_proto_toplevel *ftl);

/*
 * 把 handle 的属性对齐到视图的当前状态，只发变化的那些，最后补一条 done。
 *
 * 用差量而不是"每次全发"：客户端把 done 之前的事件当一次原子快照，重复发
 * 会让它反复重建标题/状态缓存；用差量而不是"每次都发 done"：没有变化的
 * 刷新（比如纯几何变化）不该产生事件。
 */
static void handle_refresh(struct toplevel_handle *h)
{
    if (!h->resource || h->closed || !h->entry || !h->entry->view) {
        return;
    }
    struct toplevel_entry *e = h->entry;
    struct woodland_view *view = e->view;
    bool changed = false;

    if (!h->sent_identifier) {
        treeland_foreign_toplevel_handle_v3_send_identifier(h->resource, e->identifier);
        h->sent_identifier = true;
        changed = true;
    }
    if (!h->sent_pid) {
        uint32_t pid = entry_pid(e);
        /* 拿不到就先不发：协议允许稍后补，pid 0 是假信息 */
        if (pid != 0) {
            treeland_foreign_toplevel_handle_v3_send_pid(h->resource, pid);
            h->sent_pid = true;
            changed = true;
        }
    }
    if (!streq(h->title, view->title)) {
        free(h->title);
        h->title = view->title ? strdup(view->title) : NULL;
        treeland_foreign_toplevel_handle_v3_send_title(h->resource, view->title ? view->title : "");
        changed = true;
    }
    if (!streq(h->app_id, view->app_id)) {
        free(h->app_id);
        h->app_id = view->app_id ? strdup(view->app_id) : NULL;
        treeland_foreign_toplevel_handle_v3_send_app_id(h->resource,
                                                        view->app_id ? view->app_id : "");
        changed = true;
    }

    struct woodland_toplevel_state st;
    entry_collect_state(e, &st);
    uint32_t state[FTL_STATE_MAX];
    size_t n = woodland_protocol_toplevel_state_encode(&st, state, FTL_STATE_MAX);
    if (!h->state_sent || n != h->state_len ||
        (n > 0 && memcmp(state, h->state, n * sizeof(state[0])) != 0)) {
        struct wl_array array;
        wl_array_init(&array);
        uint32_t *slot = wl_array_add(&array, n * sizeof(uint32_t));
        if (slot && n > 0) {
            memcpy(slot, state, n * sizeof(uint32_t));
        }
        treeland_foreign_toplevel_handle_v3_send_state(h->resource, &array);
        wl_array_release(&array);
        memcpy(h->state, state, n * sizeof(state[0]));
        h->state_len = n;
        h->state_sent = true;
        changed = true;
    }

    /*
     * 父窗口：只在**能给出同一个客户端的 handle** 时才发。
     *
     * parent 视图存在但客户端没有对应 handle（比如父窗口在 bind 之前就
     * 关掉了）时不能贸然发 null——协议里 null 的意思是"这是根窗口"，发了
     * 客户端会把子窗口当独立窗口处理。留着上一次的值更接近事实（同 treeland）。
     */
    if (!view->parent) {
        if (h->sent_parent) {
            treeland_foreign_toplevel_handle_v3_send_parent(h->resource, NULL);
            h->sent_parent = NULL;
            changed = true;
        }
    } else {
        struct wl_resource *parent = handle_parent_resource(h, view->parent);
        if (parent && parent != h->sent_parent) {
            treeland_foreign_toplevel_handle_v3_send_parent(h->resource, parent);
            h->sent_parent = parent;
            changed = true;
        }
    }

    if (handle_refresh_outputs(h)) {
        changed = true;
    }
    if (changed) {
        handle_send_done(h);
    }
}

static void entry_refresh(struct toplevel_entry *e)
{
    struct toplevel_handle *h;
    wl_list_for_each(h, &e->handles, link)
    {
        handle_refresh(h);
    }
}

/* ==================== output watch（补发 output_enter） ==================== */

static void handle_refresh_output_of(struct toplevel_handle *h)
{
    handle_refresh(h);
}

static void output_watch_bind(struct wl_listener *listener, void *data)
{
    struct output_watch *w = wl_container_of(listener, w, bind);
    struct wlr_output_event_bind *event = data;
    struct wl_client *client = wl_resource_get_client(event->resource);

    /*
     * 客户端刚绑上这个 wl_output。之前 output_enter 发不出去（它当时还没
     * 有 wl_output 对象，见 send_output_event），现在补一次——刷新本身是
     * 差量的，已经发过的不会重发。
     */
    struct toplevel_entry *e;
    wl_list_for_each(e, &w->ftl->entries, link)
    {
        struct toplevel_handle *h;
        wl_list_for_each(h, &e->handles, link)
        {
            if (h->client == client) {
                handle_refresh_output_of(h);
            }
        }
    }
}

static void output_watch_destroy(struct wl_listener *listener, void *data)
{
    struct output_watch *w = wl_container_of(listener, w, destroy);
    (void)data;
    wl_list_remove(&w->bind.link);
    wl_list_remove(&w->destroy.link);
    wl_list_remove(&w->link);
    free(w);
}

static void ftl_watch_outputs(struct woodland_proto_toplevel *ftl)
{
    struct woodland_output *wo;
    wl_list_for_each(wo, &ftl->server->outputs, link)
    {
        if (!wo->wlr_output) {
            continue;
        }
        bool watched = false;
        struct output_watch *w;
        wl_list_for_each(w, &ftl->output_watches, link)
        {
            if (w->output == wo->wlr_output) {
                watched = true;
                break;
            }
        }
        if (watched) {
            continue;
        }
        w = calloc(1, sizeof(*w));
        if (!w) {
            return;
        }
        w->ftl = ftl;
        w->output = wo->wlr_output;
        woodland_listener_connect(&w->bind, &w->output->events.bind, output_watch_bind,
                                  "protocol.toplevel.output_bind");
        woodland_listener_connect(&w->destroy, &w->output->events.destroy, output_watch_destroy,
                                  "protocol.toplevel.output_destroy");
        wl_list_insert(&ftl->output_watches, &w->link);
    }
}

static bool handle_refresh_outputs(struct toplevel_handle *h)
{
    struct toplevel_entry *e = h->entry;
    if (!e || !e->view || !h->resource) {
        return false;
    }
    struct woodland_server *server = h->ftl->server;
    bool changed = false;

    /* 先清掉已经不存在的输出：资源随输出一起没了，leave 也没人收 */
    struct sent_output *so, *tmp;
    wl_list_for_each_safe(so, tmp, &h->outputs, link)
    {
        bool alive = false;
        struct woodland_output *o;
        wl_list_for_each(o, &server->outputs, link)
        {
            if (o->wlr_output == so->output) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            wl_list_remove(&so->link);
            free(so);
        }
    }

    ftl_watch_outputs(h->ftl);

    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        if (!wo->wlr_output) {
            continue;
        }
        bool on = woodland_view_on_output(e->view, wo->wlr_output);
        bool sent = sent_output_has(h, wo->wlr_output);
        if (on && !sent) {
            if (send_output_event(h, wo->wlr_output, true)) {
                sent_output_add(h, wo->wlr_output);
                changed = true;
            }
        } else if (!on && sent) {
            send_output_event(h, wo->wlr_output, false);
            sent_output_drop(h, wo->wlr_output);
            changed = true;
        }
    }
    return changed;
}

/* ==================== handle 生命周期 ==================== */

static void handle_resource_destroy(struct wl_resource *resource)
{
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    if (!h) {
        return;
    }
    if (h->entry) {
        wl_list_remove(&h->link);
    }
    struct sent_output *so, *tmp;
    wl_list_for_each_safe(so, tmp, &h->outputs, link)
    {
        wl_list_remove(&so->link);
        free(so);
    }
    free(h->title);
    free(h->app_id);
    free(h);
}

static const struct treeland_foreign_toplevel_handle_v3_interface handle_impl;

static struct toplevel_handle *handle_create(struct toplevel_entry *e, struct toplevel_binding *b)
{
    struct wl_client *client = wl_resource_get_client(b->resource);
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_foreign_toplevel_handle_v3_interface,
                           wl_resource_get_version(b->resource), 0);
    if (!resource) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    struct toplevel_handle *h = calloc(1, sizeof(*h));
    if (!h) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return NULL;
    }
    h->ftl = e->ftl;
    h->entry = e;
    h->resource = resource;
    h->client = client;
    wl_list_init(&h->outputs);
    wl_list_insert(&e->handles, &h->link);
    /* 先挂实现再发事件：客户端回的请求可能早于我们返回 */
    wl_resource_set_implementation(resource, &handle_impl, h, handle_resource_destroy);

    /* 顺序是协议规定的：toplevel 之后紧接着就是初始属性快照 + done */
    treeland_foreign_toplevel_manager_v3_send_toplevel(b->resource, resource);
    handle_refresh(h);
    return h;
}

/*
 * 窗口消失：给每个 handle 发 closed，然后把 entry 收掉。
 *
 * handle 资源本身留着——协议要求客户端自己 destroy；我们只是把它变成
 * inert（closed 之后除 destroy 外的请求一律忽略，见各 handler 的 handle_view
 * 判空）。
 */
static void entry_close(struct toplevel_entry *e)
{
    unsigned count = (unsigned)wl_list_length(&e->handles);
    struct toplevel_handle *h, *tmp;
    wl_list_for_each_safe(h, tmp, &e->handles, link)
    {
        if (h->resource && !h->closed) {
            treeland_foreign_toplevel_handle_v3_send_closed(h->resource);
        }
        h->closed = true;
        h->entry = NULL;
        wl_list_remove(&h->link);
        wl_list_init(&h->link);
    }
    woodland_trace_add("ftl", e->view ? e->view->id : 0, "toplevel closed id=%u handles=%u",
                       e->identifier, count);
    wl_list_remove(&e->link);
    free(e);
}

/* ==================== handle 请求 ==================== */

static void handle_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void handle_set_maximized(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_set_maximized(view, true);
    record_request(h->ftl, view->id, "set_maximized", "applied");
}

static void handle_unset_maximized(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_set_maximized(view, false);
    record_request(h->ftl, view->id, "unset_maximized", "applied");
}

static void handle_set_minimized(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_set_minimized(view, true);
    record_request(h->ftl, view->id, "set_minimized", "applied");
}

static void handle_unset_minimized(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_set_minimized(view, false);
    record_request(h->ftl, view->id, "unset_minimized", "applied");
}

static void handle_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                  struct wl_resource *output_resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    /* output 只是 hint：认不出来就退回"合成器自己挑输出" */
    struct wlr_output *output =
        output_resource ? wlr_output_from_resource(output_resource) : NULL;
    woodland_view_set_fullscreen(view, true, output);
    /* null 输出是协议允许的"合成器挑"，别和"指定了输出"一起报成 applied */
    record_request(h->ftl, view->id, "set_fullscreen",
                   output_resource ? "applied" : "applied on compositor choice");
}

static void handle_unset_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_set_fullscreen(view, false, NULL);
    record_request(h->ftl, view->id, "unset_fullscreen", "applied");
}

static void handle_activate(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *seat_resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    struct wlr_seat_client *seat_client =
        seat_resource ? wlr_seat_client_from_resource(seat_resource) : NULL;
    /*
     * 只接受本合成器 seat 的客户端对象。别的 seat（或伪造的）无法用来
     * 抢别人的键盘焦点：协议原文说合成器可依安全考虑忽略请求。
     */
    if (!seat_client || seat_client->seat != view->server->seat) {
        record_request(h->ftl, view->id, "activate", "ignored: not this seat");
        return;
    }
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_focus_view(view, true);
    record_request(h->ftl, view->id, "activate", "focused");
}

static void handle_close(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    woodland_view_close(view);
    record_request(h->ftl, view->id, "close", "sent to client");
}

static void handle_set_icon_geometry(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *surface_resource, int32_t x, int32_t y,
                                     int32_t width, int32_t height)
{
    (void)client;
    struct toplevel_handle *h = wl_resource_get_user_data(resource);
    struct woodland_view *view = handle_view(h);
    if (!view) {
        return;
    }
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_ERROR_INVALID_GEOMETRY,
                               "set_icon_geometry: negative width/height %dx%d", (int)width,
                               (int)height);
        return;
    }
    struct wlr_surface *surface =
        surface_resource ? wlr_surface_from_resource(surface_resource) : NULL;
    if (!surface) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_HANDLE_V3_ERROR_INVALID_GEOMETRY,
                               "set_icon_geometry: surface is not a wl_surface");
        return;
    }
    struct toplevel_entry *e = h->entry;
    if (width == 0 && height == 0) {
        e->icon_set = false;
        record_request(h->ftl, view->id, "set_icon_geometry", "cleared");
        return;
    }
    e->icon_set = true;
    e->icon_x = (int)x;
    e->icon_y = (int)y;
    e->icon_width = (int)width;
    e->icon_height = (int)height;
    /*
     * 协议把它定义为"最小化/缩略图动画的提示"。woodland 没有那套动画，
     * 值只进 wctl：读的人能看出客户端声明了什么，而不是以为它在生效。
     */
    record_request(h->ftl, view->id, "set_icon_geometry",
                   "recorded (no minimize animation)");
}

static const struct treeland_foreign_toplevel_handle_v3_interface handle_impl = {
    .destroy = handle_handle_destroy,
    .set_maximized = handle_set_maximized,
    .unset_maximized = handle_unset_maximized,
    .set_minimized = handle_set_minimized,
    .unset_minimized = handle_unset_minimized,
    .set_fullscreen = handle_set_fullscreen,
    .unset_fullscreen = handle_unset_fullscreen,
    .activate = handle_activate,
    .close = handle_close,
    .set_icon_geometry = handle_set_icon_geometry,
};

/* ==================== dock preview context ==================== */

static struct dock_context *context_for_anchor(struct woodland_proto_toplevel *ftl,
                                               struct wlr_surface *anchor)
{
    struct dock_context *ctx;
    wl_list_for_each(ctx, &ftl->contexts, link)
    {
        if (ctx->anchor == anchor) {
            return ctx;
        }
    }
    return NULL;
}

static void context_anchor_destroy(struct wl_listener *listener, void *data)
{
    struct dock_context *ctx = wl_container_of(listener, ctx, anchor_destroy);
    (void)data;
    wl_list_remove(&ctx->anchor_destroy.link);
    wl_list_init(&ctx->anchor_destroy.link);
    ctx->anchor = NULL;
    /* anchor 没了 = 没有可悬停的区域：协议允许不再发任何事件 */
    ctx->inert = true;
    ctx->preview_visible = false;
    ctx->tooltip_visible = false;
}

static void context_request_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void context_resource_destroy(struct wl_resource *resource)
{
    struct dock_context *ctx = wl_resource_get_user_data(resource);
    if (!ctx) {
        return;
    }
    if (ctx->anchor_destroy.link.prev) {
        wl_list_remove(&ctx->anchor_destroy.link);
    }
    wl_list_remove(&ctx->link);
    free(ctx);
}

static void context_request_show(struct wl_client *client, struct wl_resource *resource,
                                 struct wl_array *identifiers, int32_t x, int32_t y,
                                 uint32_t direction)
{
    (void)client;
    struct dock_context *ctx = wl_resource_get_user_data(resource);
    if (!ctx || ctx->inert) {
        return;
    }

    /*
     * 只做解析与记账：identifier 对不上当前窗口的按协议忽略（不是错误）。
     * 真正的缩略图弹层是 treeland 的 QML 特效，woodland 不做。
     */
    uint32_t total = 0;
    uint32_t matched = 0;
    if (identifiers && identifiers->size % sizeof(uint32_t) == 0) {
        total = (uint32_t)(identifiers->size / sizeof(uint32_t));
        const uint32_t *ids = identifiers->data;
        for (uint32_t i = 0; i < total; i++) {
            struct toplevel_entry *e;
            wl_list_for_each(e, &ctx->ftl->entries, link)
            {
                if (e->identifier == ids[i]) {
                    matched++;
                    break;
                }
            }
        }
    }

    ctx->preview_visible = true;
    ctx->tooltip_visible = false;
    ctx->preview_identifiers = total;
    ctx->preview_matched = matched;
    ctx->x = x;
    ctx->y = y;
    ctx->direction = direction;

    char detail[160];
    snprintf(detail, sizeof(detail), "show ids=%u matched=%u at %d,%d dir=%u (popover not rendered)",
             total, matched, (int)x, (int)y, direction);
    record_dock_request(ctx->ftl, "show", detail);
}

static void context_request_show_tooltip(struct wl_client *client, struct wl_resource *resource,
                                         struct wl_resource *surface_resource, int32_t x, int32_t y,
                                         uint32_t direction)
{
    struct dock_context *ctx = wl_resource_get_user_data(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    struct wlr_surface *surface =
        surface_resource ? wlr_surface_from_resource(surface_resource) : NULL;
    if (!surface ||
        (surface_resource && wl_resource_get_client(surface_resource) != client)) {
        wl_resource_post_error(resource,
                               TREELAND_DOCK_PREVIEW_CONTEXT_V3_ERROR_INVALID_SURFACE,
                               "tooltip surface must belong to the calling client");
        return;
    }
    if (surface == ctx->anchor) {
        wl_resource_post_error(resource,
                               TREELAND_DOCK_PREVIEW_CONTEXT_V3_ERROR_INVALID_SURFACE,
                               "the anchor surface cannot be its own tooltip");
        return;
    }
    /*
     * 已经有别的 role 的 surface 不能拿来当 tooltip（合成器不会替它换 role）。
     * treeland 在这里会给 surface 钉上独占 role；我们不画 tooltip，就不抢——
     * 抢了等于永久废掉客户端的这块 surface。
     */
    if (surface->role) {
        wl_resource_post_error(resource,
                               TREELAND_DOCK_PREVIEW_CONTEXT_V3_ERROR_INVALID_SURFACE,
                               "surface already has role '%s'",
                               surface->role->name ? surface->role->name : "?");
        return;
    }
    /* 没提交过内容的 surface 是客户端违约，协议规定"什么都不显示" */
    if (!surface->current.buffer) {
        record_dock_request(ctx->ftl, "show_tooltip",
                            "ignored: surface has no committed buffer");
        return;
    }

    ctx->tooltip_visible = true;
    ctx->preview_visible = false;
    ctx->x = x;
    ctx->y = y;
    ctx->direction = direction;

    char detail[128];
    snprintf(detail, sizeof(detail), "show_tooltip at %d,%d dir=%u (popover not rendered)",
             (int)x, (int)y, direction);
    record_dock_request(ctx->ftl, "show_tooltip", detail);
}

static void context_request_close(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct dock_context *ctx = wl_resource_get_user_data(resource);
    if (!ctx || ctx->inert) {
        return;
    }
    ctx->preview_visible = false;
    ctx->tooltip_visible = false;
    record_dock_request(ctx->ftl, "close", "popover hidden (nothing was drawn)");
}

static const struct treeland_dock_preview_context_v3_interface context_impl = {
    .destroy = context_request_destroy,
    .show = context_request_show,
    .show_tooltip_surface = context_request_show_tooltip,
    .close = context_request_close,
};

/* ==================== manager ==================== */

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void binding_resource_destroy(struct wl_resource *resource)
{
    struct toplevel_binding *b = wl_resource_get_user_data(resource);
    if (!b) {
        return;
    }
    wl_list_remove(&b->link);
    free(b);
}

static void manager_handle_stop(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    struct toplevel_binding *b = wl_resource_get_user_data(resource);
    if (!b || b->stopped) {
        return;
    }
    b->stopped = true;
    treeland_foreign_toplevel_manager_v3_send_finished(resource);
    woodland_trace_add("ftl", 0, "manager stop -> finished (bindings=%u)",
                       (unsigned)wl_list_length(&b->ftl->bindings));
}

static void manager_get_dock_preview_context(struct wl_client *client,
                                             struct wl_resource *resource,
                                             struct wl_resource *relative_surface,
                                             uint32_t id)
{
    struct toplevel_binding *b = wl_resource_get_user_data(resource);
    if (!b) {
        return;
    }
    if (!relative_surface) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_MANAGER_V3_ERROR_INVALID_SURFACE,
                               "relative_surface is NULL");
        return;
    }
    if (wl_resource_get_client(relative_surface) != client) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_MANAGER_V3_ERROR_INVALID_SURFACE,
                               "relative_surface is not owned by the calling client");
        return;
    }
    struct wlr_surface *anchor = wlr_surface_from_resource(relative_surface);
    if (!anchor) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_MANAGER_V3_ERROR_INVALID_SURFACE,
                               "relative_surface is not a wl_surface");
        return;
    }
    /* 一个 surface 至多一个 context：两个对象各自定位同一个弹层，谁后发谁赢 */
    if (context_for_anchor(b->ftl, anchor)) {
        wl_resource_post_error(resource,
                               TREELAND_FOREIGN_TOPLEVEL_MANAGER_V3_ERROR_CONTEXT_ALREADY_EXISTS,
                               "surface already has an active dock preview context");
        return;
    }

    struct wl_resource *ctx_resource =
        wl_resource_create(client, &treeland_dock_preview_context_v3_interface,
                           wl_resource_get_version(resource), id);
    if (!ctx_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct dock_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        wl_resource_destroy(ctx_resource);
        wl_client_post_no_memory(client);
        return;
    }
    ctx->ftl = b->ftl;
    ctx->resource = ctx_resource;
    ctx->anchor = anchor;
    wl_list_init(&ctx->anchor_destroy.link);
    /* 先挂析构再干活：后面任何一步失败都能靠 wl_resource_destroy 收尾 */
    wl_resource_set_implementation(ctx_resource, &context_impl, ctx, context_resource_destroy);
    wl_list_insert(&b->ftl->contexts, &ctx->link);
    woodland_listener_connect(&ctx->anchor_destroy, &anchor->events.destroy,
                              context_anchor_destroy, "protocol.toplevel.anchor_destroy");
}

static const struct treeland_foreign_toplevel_manager_v3_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .stop = manager_handle_stop,
    .get_dock_preview_context = manager_get_dock_preview_context,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_toplevel *ftl = data;
    if (version > FTL_VERSION) {
        version = FTL_VERSION;
    }
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_foreign_toplevel_manager_v3_interface, (int)version,
                           id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct toplevel_binding *b = calloc(1, sizeof(*b));
    if (!b) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    b->ftl = ftl;
    b->resource = resource;
    wl_resource_set_implementation(resource, &manager_impl, b, binding_resource_destroy);
    wl_list_insert(&ftl->bindings, &b->link);

    /* bind 之后先补一份已在 map 的窗口快照（协议要求 roundtrip 后即看到全部） */
    struct toplevel_entry *e;
    wl_list_for_each(e, &ftl->entries, link)
    {
        if (!handle_create(e, b)) {
            break;
        }
    }
    woodland_trace_add("ftl", 0, "manager bind v%u -> toplevels=%u", version,
                       (unsigned)wl_list_length(&ftl->entries));
}

/* ==================== 视图通知 ==================== */

static void view_mapped(struct wl_listener *listener, void *data)
{
    struct woodland_proto_toplevel *ftl = wl_container_of(listener, ftl, view_mapped);
    struct woodland_view *view = data;
    if (!view || entry_for_view(ftl, view)) {
        return;
    }
    struct toplevel_entry *e = calloc(1, sizeof(*e));
    if (!e) {
        wlr_log(WLR_ERROR, "toplevel: cannot allocate entry for view %u", view->id);
        return;
    }
    e->ftl = ftl;
    e->view = view;
    e->identifier = ftl->next_identifier++;
    wl_list_init(&e->handles);
    wl_list_insert(&ftl->entries, &e->link);

    struct toplevel_binding *b;
    wl_list_for_each(b, &ftl->bindings, link)
    {
        if (b->stopped) {
            continue;
        }
        if (!handle_create(e, b)) {
            break;
        }
    }
    woodland_trace_add("ftl", view->id, "toplevel id=%u app_id=%s", e->identifier,
                       view->app_id ? view->app_id : "-");
}

static void view_unmapped(struct wl_listener *listener, void *data)
{
    struct woodland_proto_toplevel *ftl = wl_container_of(listener, ftl, view_unmapped);
    struct toplevel_entry *e = entry_for_view(ftl, data);
    if (e) {
        entry_close(e);
    }
}

static void view_changed(struct wl_listener *listener, void *data)
{
    struct woodland_proto_toplevel *ftl = wl_container_of(listener, ftl, view_changed);
    struct toplevel_entry *e = entry_for_view(ftl, data);
    if (e) {
        entry_refresh(e);
    }
}

/* ==================== 由编排层调用的入口 ==================== */

bool woodland_proto_toplevel_init(struct woodland_proto *proto)
{
    struct woodland_server *server = proto->server;

    struct woodland_proto_toplevel *ftl = calloc(1, sizeof(*ftl));
    if (!ftl) {
        wlr_log(WLR_ERROR, "cannot allocate toplevel protocol state");
        return false;
    }
    ftl->server = server;
    /* identifier 从 1 开始：0 留给"还没分配"的语义 */
    ftl->next_identifier = 1;
    wl_list_init(&ftl->bindings);
    wl_list_init(&ftl->entries);
    wl_list_init(&ftl->contexts);
    wl_list_init(&ftl->output_watches);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->toplevel = ftl;

    ftl->global = wl_global_create(server->display,
                                   &treeland_foreign_toplevel_manager_v3_interface, FTL_VERSION,
                                   ftl, manager_bind);
    if (!ftl->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-foreign-toplevel-manager-v3 global");
        return false;
    }

    /*
     * 视图通知由 shell 发：协议层不能反向插进 map/unmap/destroy 的路径
     * （那条路径被只链 shell_lib 的单元测试覆盖，加了协议符号就链接失败）。
     *
     * 但这两条要求信号**已经 wl_signal_init 过**：单元测试会跳过 shell 直接
     * 调 woodland_protocol_init（test_syncobj / test_drm_lease 都是），那里
     * server 是 memset 出来的，三条信号全零——往零链表上挂监听器就是空指针写。
     * 判 next 与 woodland_signal_emit 用的是同一个约定：初始化过的链表
     * next/prev 指回自己，只有 memset 出来的才是 NULL。
     */
    if (server->view_mapped.listener_list.next && server->view_changed.listener_list.next &&
        server->view_unmapped.listener_list.next) {
        woodland_listener_connect(&ftl->view_mapped, &server->view_mapped, view_mapped,
                                  "protocol.toplevel.view_mapped");
        woodland_listener_connect(&ftl->view_unmapped, &server->view_unmapped, view_unmapped,
                                  "protocol.toplevel.view_unmapped");
        woodland_listener_connect(&ftl->view_changed, &server->view_changed, view_changed,
                                  "protocol.toplevel.view_changed");
        ftl->watching_views = true;
    } else {
        wlr_log(WLR_DEBUG, "toplevel: view signals not initialised, window list stays empty");
    }

    wlr_log(WLR_INFO, "treeland-foreign-toplevel-manager-v3: wired (dock preview record-only)");
    return true;
}

void woodland_proto_toplevel_finish(struct woodland_proto *proto)
{
    struct woodland_proto_toplevel *ftl = proto->toplevel;
    if (!ftl) {
        return;
    }
    if (ftl->watching_views) {
        wl_list_remove(&ftl->view_mapped.link);
        wl_list_remove(&ftl->view_unmapped.link);
        wl_list_remove(&ftl->view_changed.link);
        ftl->watching_views = false;
    }

    if (ftl->global) {
        wl_global_destroy(ftl->global);
        ftl->global = NULL;
    }
    /*
     * global 销毁不会带走已 bind 的 resource。子对象按"handle → entry →
     * context → binding"的顺序拆：handle 的析构要摘 entry 的链表，
     * entry 的析构要 free 自己，binding 最后放（它的 user_data 是本族状态）。
     */
    struct toplevel_entry *e, *etmp;
    wl_list_for_each_safe(e, etmp, &ftl->entries, link)
    {
        struct toplevel_handle *h, *htmp;
        wl_list_for_each_safe(h, htmp, &e->handles, link)
        {
            wl_resource_destroy(h->resource);
        }
        wl_list_remove(&e->link);
        free(e);
    }
    struct dock_context *ctx, *ctmp;
    wl_list_for_each_safe(ctx, ctmp, &ftl->contexts, link)
    {
        wl_resource_destroy(ctx->resource);
    }
    struct toplevel_binding *b, *btmp;
    wl_list_for_each_safe(b, btmp, &ftl->bindings, link)
    {
        wl_resource_destroy(b->resource);
    }
    struct output_watch *w, *wtmp;
    wl_list_for_each_safe(w, wtmp, &ftl->output_watches, link)
    {
        wl_list_remove(&w->bind.link);
        wl_list_remove(&w->destroy.link);
        wl_list_remove(&w->link);
        free(w);
    }
    free(ftl);
    proto->toplevel = NULL;
}

/* ==================== wctl 状态 ==================== */

void woodland_protocol_toplevel_status(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_proto_toplevel *ftl = (server && server->proto) ? server->proto->toplevel : NULL;
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "available", ftl != NULL);
    woodland_json_obj_int(&obj, "bindings", ftl ? (long long)wl_list_length(&ftl->bindings) : 0);
    woodland_json_obj_int(&obj, "toplevels", ftl ? (long long)wl_list_length(&ftl->entries) : 0);
    woodland_json_obj_int(&obj, "contexts", ftl ? (long long)wl_list_length(&ftl->contexts) : 0);
    woodland_json_obj_int(&obj, "request_count", ftl ? ftl->request_count : 0);
    woodland_json_obj_str(&obj, "last_request", ftl ? ftl->last_request : NULL);
    woodland_json_obj_str(&obj, "last_outcome", ftl ? ftl->last_outcome : NULL);
    woodland_json_obj_int(&obj, "dock_request_count", ftl ? ftl->dock_request_count : 0);
    woodland_json_obj_str(&obj, "last_dock_request", ftl ? ftl->last_dock_request : NULL);
    /*
     * 稳定值而不是人话：脚本据此判断"预览到底画没画"。现在恒为 recorded，
     * 哪天真的接了 popover 渲染器，这里改一次就能让所有断言跟着变。
     */
    woodland_json_obj_str(&obj, "dock_preview", "recorded");
    woodland_json_obj_close(&obj);
}

void woodland_protocol_toplevel_view_status(struct woodland_view *view,
                                            struct woodland_json *out)
{
    struct woodland_proto_toplevel *ftl =
        (view && view->server && view->server->proto) ? view->server->proto->toplevel : NULL;
    struct toplevel_entry *e = ftl ? entry_for_view(ftl, view) : NULL;

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "tracked", e != NULL);
    if (e) {
        woodland_json_obj_int(&obj, "identifier", e->identifier);
        woodland_json_obj_int(&obj, "handles", (long long)wl_list_length(&e->handles));
        if (e->icon_set) {
            int box[4] = {e->icon_x, e->icon_y, e->icon_width, e->icon_height};
            woodland_json_obj_key(&obj, "icon_geometry");
            woodland_json_int_array(out, box, 4);
        }
    }
    woodland_json_obj_close(&obj);
}
