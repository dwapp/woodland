// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-xwindow-control-unstable-v1：把 XWayland 窗口摆到某个 surface 旁边。
 *
 * 真实用途是 DDE Dock 的 XEmbed 托盘图标：dock 是 Wayland 客户端，没法像 X11
 * 那样把 XEmbed 窗口 reparent 进自己，只能请合成器按 X11 window id 把那个
 * 窗口挪到 dock 上对应的位置（见 dde-shell 的 WaylandDockHelper）。
 *
 * 协议本身是无状态的：一个 global、destroy / set_xwindow_position_relative
 * 两个请求、没有 event，每条请求的成败通过**这一次请求新建的 wl_callback**
 * 回 done(0) / done(1) 并立即销毁。所谓"state"全是给 wctl 看的诊断计数。
 *
 * 三个容易做错的地方：
 *
 *  1. anchor 的"左上角"是**布局坐标**，不是 surface 局部坐标。客户端给的
 *     wl_surface 可能是 xdg toplevel / layer surface / popup / X11 窗口，
 *     逐个后端反查会把协议族变成一个大 switch。这里统一走
 *     wlr_scene_node_for_each_buffer()：它按渲染顺序给出每个 buffer 节点在
 *     布局里的 (x,y)，再用 wlr_scene_surface_try_from_buffer() 认领哪个是
 *     anchor。裸 wl_surface（没建 scene 节点 / 没 mapped）自然找不到，按
 *     协议回失败——和 treeland 的 「getSurface(anchor) 为 NULL」同义。
 *
 *  2. X11 窗口的位置有两份真相：wlr_scene 节点和 X server 那边的窗口几何。
 *     只挪 scene 节点，下一次 X11 的 set_geometry 就会把它弹回去。真正的
 *     摆位（含 override-redirect 窗口）放在 xwayland.c，由
 *     server->xwindow_move_hook 反向提供：它同时 set_position 和
 *     wlr_xwayland_surface_configure()。protocol_lib 不反向链接
 *     xwayland_lib（只链 protocol_lib 的单测会缺符号），钩子为 NULL 就表示
 *     本构建没有 Xwayland，如实回 callback.done(1) 而不是假装生效。
 *
 *  3. wl_global_destroy() 不会带走已 bind 的 wl_resource。resource 的
 *     user_data 指向本族状态，finish 里必须先把 binding 全部销毁再 free，
 *     否则活着的 resource 再收到请求就是 UAF（同 compositor_action.c）。
 *
 *  4. X11 侧的 wlr_xwayland_surface_configure() 收的是 int16_t x/y，而协议
 *     的 wl_fixed_t 能给出几万的合法偏移。截断后回 done(0) 等于把窗口配到
 *     另一个坐标，还会被随后的 ConfigureNotify 反向污染 scene 节点。所以
 *     目标先过 woodland_xwindow_target_in_x11_range()，越界一律回 done(1)
 *     且**不调用摆位钩子**，两份真相都不动。没有 XWayland 的构建连 global
 *     都不广播：DDE Dock 用 isActive() 判通道可用性，广播一个永远失败的
 *     global 会让它一直发请求、不再回退到 X11 原生 reparent。
 *
 * 权限：协议要求"应当拒绝非特权客户端的 bind"。woodland 没有 DDE 的
 * privilege 服务，只做两层力所能及的校验——对端 uid 与合成器相同、且该连接
 * 没有 security context（沙箱客户端的 global 过滤在 sec_ctx_global_filter
 * 里已经挡了一道，这里是纵深防御）。这不是 DDE 的按 app_id 授权，也不等价
 * 于上游要求 target 与 anchor 同 session 的隔离：任何同 uid 的非沙箱进程
 * 都能按 X11 window id 挪动别人的窗口。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h> /* wl_callback_interface / wl_callback_send_done */
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "config.h" /* WOODLAND_HAVE_XWAYLAND：宏判断必须在 include 之后 */
#include "protocol.h"
#include "protocol_internal.h"
#include "scene.h"
#include "trace.h"
#include "util/json.h"
#include "woodland.h"

#include "treeland-xwindow-control-unstable-v1-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define XWINDOW_VERSION 1

static const char *const outcome_names[] = {
    [WOODLAND_XWINDOW_NONE] = "none",
    [WOODLAND_XWINDOW_MOVED] = "moved",
    [WOODLAND_XWINDOW_NO_XWAYLAND] = "no_xwayland",
    [WOODLAND_XWINDOW_INVALID_ANCHOR] = "invalid_anchor",
    [WOODLAND_XWINDOW_NO_WINDOW] = "no_window",
    [WOODLAND_XWINDOW_OUT_OF_RANGE] = "out_of_range",
    [WOODLAND_XWINDOW_CALLBACK_FAILED] = "callback_failed",
};

/* 一个 bind 出来的对象；链表只是为了 finish 时能全部回收 */
struct xwindow_binding {
    struct woodland_proto_xwindow_control *family;
    struct wl_resource *resource;
    struct wl_list link;
};

/* for_each_buffer 的出参：找到 anchor 的第一份布局坐标 */
struct anchor_lookup {
    struct wlr_surface *surface;
    bool found;
    int x, y;
};

struct woodland_proto_xwindow_control {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list bindings; // struct xwindow_binding.link

    /* 只读诊断状态：协议本身无状态，这些只是"最近一次请求怎么处置的" */
    uint32_t request_count;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t last_wid;
    int last_dx, last_dy;              /* fixed 取整后的像素偏移 */
    int32_t last_dx_fixed, last_dy_fixed; /* 客户端原始 24.8 值，/256 即像素 */
    enum woodland_xwindow_outcome last_outcome;
    bool have_anchor;
    int last_anchor_x, last_anchor_y;
    bool have_target;
    int last_target_x, last_target_y;
    uint32_t last_view_id;             /* 0 = unmanaged 或没找到 */
    bool last_unmanaged;
};

/* ==================== 纯逻辑（单元测试直接调） ==================== */

const char *woodland_xwindow_outcome_name(enum woodland_xwindow_outcome outcome)
{
    switch (outcome) {
    case WOODLAND_XWINDOW_NONE:
    case WOODLAND_XWINDOW_MOVED:
    case WOODLAND_XWINDOW_NO_XWAYLAND:
    case WOODLAND_XWINDOW_INVALID_ANCHOR:
    case WOODLAND_XWINDOW_NO_WINDOW:
    case WOODLAND_XWINDOW_OUT_OF_RANGE:
    case WOODLAND_XWINDOW_CALLBACK_FAILED:
        return outcome_names[outcome];
    default:
        return "unknown";
    }
}

int woodland_xwindow_control_round_fixed(int32_t value)
{
    /*
     * wl_fixed_t 是 24.8 定点（typedef int32_t）。转成 double 再"远离 0"
     * 四舍五入，不用 lround()：那会为了一个纯函数把 libm 拖进每个只链
     * protocol_lib 的单测。24.8 的值域是 ±8388607.996，结果永远装得进 int。
     */
    double v = wl_fixed_to_double((wl_fixed_t)value);
    return (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

bool woodland_xwindow_target_in_x11_range(int x, int y)
{
    /*
     * wlr_xwayland_surface_configure() 的 x/y 是 int16_t，且 X11 的
     * ConfigureWindow 本身也只收 int16 坐标。别在这里 clamp 后继续摆位：
     * "成功"但落点不是客户端请求的位置，比直接回失败更难排查。
     */
    return x >= INT16_MIN && x <= INT16_MAX && y >= INT16_MIN && y <= INT16_MAX;
}

void woodland_xwindow_control_target(int anchor_x, int anchor_y,
                                     int32_t dx, int32_t dy,
                                     int *out_x, int *out_y)
{
    int64_t x = (int64_t)anchor_x + woodland_xwindow_control_round_fixed(dx);
    int64_t y = (int64_t)anchor_y + woodland_xwindow_control_round_fixed(dy);
    if (out_x) {
        *out_x = (int)(x > INT32_MAX ? INT32_MAX : (x < INT32_MIN ? INT32_MIN : x));
    }
    if (out_y) {
        *out_y = (int)(y > INT32_MAX ? INT32_MAX : (y < INT32_MIN ? INT32_MIN : y));
    }
}

void woodland_protocol_xwindow_control_status(struct woodland_server *server,
                                              struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);

    struct woodland_proto_xwindow_control *family =
        (server && server->proto) ? server->proto->xwindow_control : NULL;
    if (!family) {
        woodland_json_obj_bool(&obj, "available", false);
        woodland_json_obj_close(&obj);
        return;
    }

    /*
     * noxwl 构建里族状态仍保留（便于诊断），但 global 根本没广播；available
     * 必须按 global 判，否则客户端会以为摆位通道可用而不再回退。
     */
    woodland_json_obj_bool(&obj, "available", family->global != NULL);
    woodland_json_obj_int(&obj, "bindings", (long long)wl_list_length(&family->bindings));
    woodland_json_obj_bool(&obj, "xwayland", server->xwindow_move_hook != NULL);
    woodland_json_obj_int(&obj, "request_count", family->request_count);
    woodland_json_obj_int(&obj, "success_count", family->success_count);
    woodland_json_obj_int(&obj, "failure_count", family->failure_count);
    woodland_json_obj_int(&obj, "last_wid", family->last_wid);
    woodland_json_obj_int(&obj, "last_dx", family->last_dx);
    woodland_json_obj_int(&obj, "last_dy", family->last_dy);
    /* 原始 24.8 值：排查 Dock 小数偏移时不会把"取整"误判成"算错" */
    woodland_json_obj_int(&obj, "last_dx_fixed", family->last_dx_fixed);
    woodland_json_obj_int(&obj, "last_dy_fixed", family->last_dy_fixed);
    woodland_json_obj_str(&obj, "last_outcome",
                          woodland_xwindow_outcome_name(family->last_outcome));

    woodland_json_obj_key(&obj, "last_anchor");
    if (family->have_anchor) {
        int pair[2] = {family->last_anchor_x, family->last_anchor_y};
        woodland_json_int_array(obj.json, pair, 2);
    } else {
        woodland_json_raw(obj.json, "null");
    }
    woodland_json_obj_key(&obj, "last_target");
    if (family->have_target) {
        int pair[2] = {family->last_target_x, family->last_target_y};
        woodland_json_int_array(obj.json, pair, 2);
    } else {
        woodland_json_raw(obj.json, "null");
    }
    woodland_json_obj_str(&obj, "last_target_kind",
                          !family->have_target ? NULL
                          : family->last_unmanaged ? "unmanaged" : "managed");
    woodland_json_obj_int(&obj, "last_view_id", family->last_view_id);

    woodland_json_obj_close(&obj);
}

/*
 * 以下整段是"有客户端能 bind 到这个 global"才需要的路径。编译期关闭
 * XWayland 时 global 根本不创建，这些 static 函数在 werror=true 下会全部
 * 触发 -Wunused-function；与其逐个 (void) 引用，不如整段裁掉——没有
 * XWayland 时本协议族只剩上面的纯逻辑和一个 available:false 的诊断壳。
 */
#if WOODLAND_HAVE_XWAYLAND

/* ==================== anchor 定位 ==================== */

static void anchor_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    struct anchor_lookup *lookup = data;
    if (lookup->found) {
        return;
    }
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (!scene_surface || scene_surface->surface != lookup->surface) {
        return;
    }
    lookup->found = true;
    lookup->x = sx;
    lookup->y = sy;
}

/*
 * anchor 必须是**本 client 自己的** wl_surface，且在场景里有一份 buffer 节点
 * （否则没有布局坐标）。libwayland 的对象 ID 按 client 隔离，跨 client 传
 * surface 实际很难成立，但上游 xwindowcontrolinterfacev1.cpp:52 也显式校验，
 * 这里对齐它；另外补一次 wl_surface_interface 类型检查，避免客户端传错对象
 * 时 wlr_surface_from_resource() 内部 assert 把合成器带崩。
 */
static bool anchor_fail(const char **reason, const char *why)
{
    if (reason) {
        *reason = why;
    }
    return false;
}

static bool anchor_position(struct woodland_server *server, struct wl_client *client,
                            struct wl_resource *anchor_resource, int *x, int *y,
                            const char **reason)
{
    if (!anchor_resource || !server || !server->scene) {
        return anchor_fail(reason, "no anchor surface or scene");
    }
    if (client && wl_resource_get_client(anchor_resource) != client) {
        return anchor_fail(reason, "anchor belongs to a different client");
    }
    /*
     * wl_resource_instance_of() 的第三参数是要**相等**的 implementation，
     * 传 NULL 会把所有合法 wl_surface 都判成不匹配；这里只需要比接口类型。
     */
    if (wl_resource_get_interface(anchor_resource) != &wl_surface_interface) {
        return anchor_fail(reason, "anchor is not a wl_surface");
    }
    struct wlr_surface *surface = wlr_surface_from_resource(anchor_resource);
    if (!surface) {
        return anchor_fail(reason, "anchor wl_surface has no wlr_surface");
    }
    struct anchor_lookup lookup = {.surface = surface, .found = false};
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, anchor_iterator, &lookup);
    if (!lookup.found) {
        return anchor_fail(reason, "anchor has no scene buffer (unmapped or unknown surface)");
    }
    *x = lookup.x;
    *y = lookup.y;
    return true;
}

/* ==================== bind / 请求 ==================== */

static bool client_allowed(struct woodland_server *server, struct wl_client *client,
                           uid_t *out_uid)
{
    pid_t pid = 0;
    uid_t uid = (uid_t)-1;
    gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    (void)pid;
    (void)gid;
    if (out_uid) {
        *out_uid = uid;
    }
    if (uid != (uid_t)-1 && uid != getuid()) {
        return false;
    }
    /*
     * 沙箱连接的 global 过滤器已经把这个特权协议藏起来了；再判一次是防止
     * 客户端在 commit 之前就 bind（或绕过过滤器的路径）拿到对象。
     */
    return woodland_protocol_client_security_context(server, client) == NULL;
}

static void binding_resource_destroy(struct wl_resource *resource)
{
    struct xwindow_binding *binding = wl_resource_get_user_data(resource);
    if (!binding) {
        return;
    }
    wl_list_remove(&binding->link);
    free(binding);
}

static void xwindow_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    /* 协议原文：destroy 不改变任何窗口位置；已应用的摆位保持原样 */
    wl_resource_destroy(resource);
}

static void xwindow_handle_set_position_relative(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 uint32_t callback_id, uint32_t wid,
                                                 struct wl_resource *anchor_resource,
                                                 wl_fixed_t dx, wl_fixed_t dy)
{
    struct xwindow_binding *binding = wl_resource_get_user_data(resource);
    struct woodland_proto_xwindow_control *family = binding ? binding->family : NULL;
    struct woodland_server *server = family ? family->server : NULL;

    /*
     * 先建 callback：协议要求**每条**请求都回一次 done，参数不合法也要回。
     * 只有连 callback 都分配不出来时才 post_no_memory（没有对象可发 done）。
     */
    struct wl_resource *callback =
        wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (!callback) {
        wl_client_post_no_memory(client);
        if (family) {
            family->request_count++;
            family->failure_count++;
            family->last_wid = wid;
            family->last_outcome = WOODLAND_XWINDOW_CALLBACK_FAILED;
            wlr_log(WLR_ERROR, "xwindow-control: cannot allocate wl_callback for wid=%u", wid);
        }
        return;
    }

    enum woodland_xwindow_outcome outcome = WOODLAND_XWINDOW_NO_WINDOW;
    int anchor_x = 0, anchor_y = 0, target_x = 0, target_y = 0;
    uint32_t view_id = 0;
    bool unmanaged = false;
    const char *reason = "unknown";
    char detail[160] = "";

    if (family) {
        family->request_count++;
        family->last_wid = wid;
        family->last_dx = woodland_xwindow_control_round_fixed(dx);
        family->last_dy = woodland_xwindow_control_round_fixed(dy);
        family->last_dx_fixed = (int32_t)dx;
        family->last_dy_fixed = (int32_t)dy;
        family->have_anchor = false;
        family->have_target = false;
        family->last_view_id = 0;
        family->last_unmanaged = false;
    }

    if (!server || !server->xwindow_move_hook) {
        outcome = WOODLAND_XWINDOW_NO_XWAYLAND;
        snprintf(detail, sizeof(detail), "xwayland layer unavailable");
    } else if (!anchor_position(server, client, anchor_resource,
                                &anchor_x, &anchor_y, &reason)) {
        outcome = WOODLAND_XWINDOW_INVALID_ANCHOR;
        snprintf(detail, sizeof(detail), "%s", reason);
    } else {
        if (family) {
            family->have_anchor = true;
            family->last_anchor_x = anchor_x;
            family->last_anchor_y = anchor_y;
        }
        woodland_xwindow_control_target(anchor_x, anchor_y, dx, dy, &target_x, &target_y);
        if (!woodland_xwindow_target_in_x11_range(target_x, target_y)) {
            /* 不调钩子：scene 节点与 X11 几何两份真相都保持原样 */
            outcome = WOODLAND_XWINDOW_OUT_OF_RANGE;
            snprintf(detail, sizeof(detail),
                     "anchor=(%d,%d) + (%d,%d) -> (%d,%d) exceeds X11 int16 range",
                     anchor_x, anchor_y,
                     woodland_xwindow_control_round_fixed(dx),
                     woodland_xwindow_control_round_fixed(dy), target_x, target_y);
        } else {
            bool moved = server->xwindow_move_hook(server, wid, target_x, target_y,
                                                   &view_id, &unmanaged);
            if (moved) {
                outcome = WOODLAND_XWINDOW_MOVED;
                snprintf(detail, sizeof(detail), "anchor=(%d,%d) + (%d,%d) -> (%d,%d) %s",
                         anchor_x, anchor_y,
                         woodland_xwindow_control_round_fixed(dx),
                         woodland_xwindow_control_round_fixed(dy), target_x, target_y,
                         unmanaged ? "unmanaged" : "managed");
            } else {
                outcome = WOODLAND_XWINDOW_NO_WINDOW;
                snprintf(detail, sizeof(detail), "no mapped X11 window with this wid");
            }
        }
    }

    if (family) {
        family->last_outcome = outcome;
        if (outcome == WOODLAND_XWINDOW_MOVED) {
            family->success_count++;
            family->have_target = true;
            family->last_target_x = target_x;
            family->last_target_y = target_y;
            family->last_view_id = view_id;
            family->last_unmanaged = unmanaged;
        } else {
            family->failure_count++;
        }
    }

    uint32_t result = outcome == WOODLAND_XWINDOW_MOVED ? 0 : 1;
    /* detail 只写决策依据；状态数值由 get_tree 查 */
    woodland_trace_add("xwindow", view_id, "wid=%u offset=(%d,%d) -> %s: %s", wid,
                       woodland_xwindow_control_round_fixed(dx),
                       woodland_xwindow_control_round_fixed(dy),
                       woodland_xwindow_outcome_name(outcome), detail);
    wlr_log(WLR_INFO, "xwindow-control: wid=%u -> %s: %s", wid,
            woodland_xwindow_outcome_name(outcome), detail);

    wl_callback_send_done(callback, result);
    wl_resource_destroy(callback);
}

static const struct treeland_xwindow_control_v1_interface xwindow_impl = {
    .destroy = xwindow_handle_destroy,
    .set_xwindow_position_relative = xwindow_handle_set_position_relative,
};

static void xwindow_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_xwindow_control *family = data;
    uid_t uid = (uid_t)-1;
    if (!client_allowed(family->server, client, &uid)) {
        wlr_log(WLR_ERROR,
                "xwindow-control: denied bind from uid %u (compositor uid %u), sandboxed=%d",
                (unsigned)uid, (unsigned)getuid(),
                woodland_protocol_client_security_context(family->server, client) != NULL);
        /*
         * 协议没有自己的 error enum：用 implementation 错误明确拒绝 bind。
         * 注意这会断掉整条连接（不只是这次 bind），误 bind 的普通应用会直接
         * 掉线。security-context 的 global 过滤已经挡了第一道，这里的失败面
         * 很小；要更温和可以改成建完 resource 立刻销毁，但那样诊断更弱。
         */
        wl_client_post_implementation_error(client,
            "treeland_xwindow_control_v1: bind rejected (different user or sandboxed)");
        return;
    }

    if (version > XWINDOW_VERSION) {
        version = XWINDOW_VERSION;
    }
    struct wl_resource *resource = wl_resource_create(
        client, &treeland_xwindow_control_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct xwindow_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    binding->family = family;
    binding->resource = resource;
    /* 先挂析构再入链：客户端 destroy 与 finish 回收走同一条路 */
    wl_resource_set_implementation(resource, &xwindow_impl, binding,
                                    binding_resource_destroy);
    wl_list_insert(&family->bindings, &binding->link);
}

#endif /* WOODLAND_HAVE_XWAYLAND */

/* ==================== 由编排层调用的入口 ==================== */

bool woodland_proto_xwindow_control_init(struct woodland_proto *proto)
{
    struct woodland_proto_xwindow_control *family = calloc(1, sizeof(*family));
    if (!family) {
        wlr_log(WLR_ERROR, "cannot allocate xwindow-control protocol state");
        return false;
    }
    family->server = proto->server;
    family->last_outcome = WOODLAND_XWINDOW_NONE;
    wl_list_init(&family->bindings);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->xwindow_control = family;

#if !WOODLAND_HAVE_XWAYLAND
    /*
     * 与上游 waylandserver.cpp 的 #ifndef DISABLE_XWAYLAND 一致：没有 XWayland
     * 就不广播这个 global。DDE Dock 用 isActive() 判通道是否可用，广播一个
     * 永远回 done(1) 的 global 会让它一直发请求、不再回退到 X11 原生 reparent。
     * 族状态保留下来给 wctl 看（available:false），但不建 global、不接 bind。
     */
    wlr_log(WLR_INFO,
            "treeland-xwindow-control-v1: global not advertised (XWayland disabled)");
    return true;
#else
    family->global = wl_global_create(proto->server->display,
                                       &treeland_xwindow_control_v1_interface,
                                       XWINDOW_VERSION, family, xwindow_bind);
    if (!family->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-xwindow-control global");
        return false;
    }
    /*
     * 别在这里打印 xwindow_move_hook 有没有接上：server_init 的顺序是
     * protocol_init 在前、xwayland_init 在后，此刻钩子必然是 NULL，日志会
     * 误导成"本构建没有 XWayland"。真正决定成败的是请求到达时的值，wctl
     * get_tree 的 xwindow_control.xwayland 报的就是它。
     */
    wlr_log(WLR_INFO, "treeland-xwindow-control-v1: global advertised");
    return true;
#endif
}

void woodland_proto_xwindow_control_finish(struct woodland_proto *proto)
{
    struct woodland_proto_xwindow_control *family = proto->xwindow_control;
    if (!family) {
        return;
    }
    if (family->global) {
        wl_global_destroy(family->global);
        family->global = NULL;
    }
    /*
     * global 销毁不会带走已 bind 的 resource。先全部销毁（destructor 摘链并
     * 释放 binding），再 free 本族状态，否则活着的 resource 上还留着指向
     * family 的 user_data，请求一到就是 UAF。
     */
    struct xwindow_binding *binding, *tmp;
    wl_list_for_each_safe(binding, tmp, &family->bindings, link)
    {
        wl_resource_destroy(binding->resource);
    }
    free(family);
    proto->xwindow_control = NULL;
}
