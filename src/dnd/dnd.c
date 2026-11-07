// SPDX-License-Identifier: GPL-2.0-only
/*
 * 拖拽（DnD）：wl_data_device.start_drag 的合成器侧。
 *
 * 为什么需要这个模块（而不是"wlroots 应该已经做了"）：wlroots 只把协议
 * 状态机做完了，**起不起拖拽、拖到谁身上、图标在哪**三个策略问题一概不管。
 * 缺了前两个，客户端发出 start_drag 之后没有任何回应——拖文件到终端、
 * 浏览器拖图上传、拖拽选中文本全部静默失败。
 *
 * 三个容易踩的既有实现坑（labwc NEWS / treeland 血统都踩过）：
 *
 *   1. 拖拽期间不能用裸 wlr_seat_pointer_clear_focus()：它绕过 pointer grab，
 *      wlr_drag 收不到 leave/enter，表现是"在标题栏或桌面上松手，文件却掉进
 *      了上一个窗口"。光标层一律用 notify_ 版本。
 *   2. 图标树必须挂在 scene 根并在开始时置顶：layer-shell 的 panel/dock 也是
 *      scene 根的子节点，不置顶就被面板盖住（labwc #1936）。
 *   3. 图标子树自己搭，不用 wlr_scene_drag_icon_create()：scenefx 0.5 声明了
 *      这个符号但没实现，链接会落到 libwlroots 的实现上，而那是按自己的
 *      wlr_scene_node / wlr_scene_tree 布局访问 scenefx 建的树——今天字段恰好
 *      一致，改天就是随机内存破坏。见
 *      docs/superpowers/specs/2026-11-08-clipboard-dnd-design.md §9.1。
 */
#include "config.h"
#include "util/listener.h"
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "scene.h"

#include "dnd.h"
#include "trace.h"
#include "woodland.h"

/* drag->events.destroy 的处理器（定义在生命周期一节） */
static void dnd_handle_destroy(struct wl_listener *listener, void *data);

/* ==================== 图标 ==================== */

/*
 * 图标子树的销毁。
 *
 * 三条路径会走到这里（drag 销毁 / 客户端把 icon surface 销毁 / 模块收尾），
 * 所以必须幂等：armed 关掉之后 rest 全部跳过。
 */
static void dnd_icon_release(struct woodland_dnd *dnd)
{
    struct woodland_dnd_icon *icon = &dnd->icon;
    if (!icon->armed) {
        return;
    }
    icon->armed = false;
    wl_list_remove(&icon->commit.link);
    wl_list_remove(&icon->destroy.link);
    if (icon->tree) {
        /* 连带销毁 subsurface 子树（wlr_scene_subsurface_tree_create 建的） */
        wlr_scene_node_destroy(&icon->tree->node);
        icon->tree = NULL;
    }
    icon->icon = NULL;
}

/*
 * hotspot 增量：客户端每次 commit 可以把图标相对指针的偏移改一点
 * （surface->current.dx/dy），照抄 wlroots types/scene/drag_icon.c 的语义。
 * 注意不是 sx/sy：那两个是 surface 内的坐标，跟位置无关。
 */
static void dnd_icon_handle_commit(struct wl_listener *listener, void *data)
{
    struct woodland_dnd_icon *icon = wl_container_of(listener, icon, commit);
    if (!icon->tree || !icon->icon) {
        return;
    }
    struct wlr_surface *surface = icon->icon->surface;
    if (!surface) {
        return;
    }
    wlr_scene_node_set_position(&icon->tree->node, icon->tree->node.x + surface->current.dx,
                                icon->tree->node.y + surface->current.dy);
    (void)data;
}

static void dnd_icon_handle_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_dnd_icon *icon = wl_container_of(listener, icon, destroy);
    /* 收尾只看 dnd，不需要 icon 本身：release 会把它一并清掉 */
    struct woodland_dnd *dnd = wl_container_of(icon, dnd, icon);
    dnd_icon_release(dnd);
    (void)data;
}

static void dnd_icon_create(struct woodland_dnd *dnd, struct wlr_drag_icon *icon)
{
    /* 客户端可以不设图标：这是合法状态，只是什么都看不见 */
    if (!icon || !dnd->icons) {
        return;
    }
    /*
     * subsurface 树自带 map/unmap → enabled 的处理（跟着 surface->mapped 走），
     * 所以这里不挂 map/unmap 监听器：客户端可能先给个空图标再贴 buffer，
     * 节点可见性交给 scenefx 比我们自己跟更准。
     */
    struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(dnd->icons, icon->surface);
    if (!tree) {
        wlr_log(WLR_ERROR, "failed to create drag icon node");
        return;
    }
    dnd->icon.tree = tree;
    dnd->icon.icon = icon;
    dnd->icon.armed = true;
    woodland_listener_connect(&dnd->icon.commit, &icon->surface->events.commit,
                              dnd_icon_handle_commit, "dnd.icon_commit");
    woodland_listener_connect(&dnd->icon.destroy, &icon->events.destroy,
                              dnd_icon_handle_destroy, "dnd.icon_destroy");
}

/* ==================== 投放目标 ==================== */

bool woodland_dnd_node_is_icon(const struct woodland_dnd *dnd, struct wlr_scene_node *node)
{
    if (!dnd->icons) {
        return false;
    }
    /* scenefx 没有 wlr_scene_node_set_input_bypassed，只能靠祖先检查 */
    for (struct wlr_scene_node *n = node; n != NULL; n = n->parent ? &n->parent->node : NULL) {
        if (n == &dnd->icons->node) {
            return true;
        }
    }
    return false;
}

/*
 * 光标下的 surface。
 *
 * 不复用 woodland_view_at()：那条路只认 toplevel 视图，而拖到 panel
 * （layer-shell）、Dock、X11 override-redirect 窗口上都该有 enter。这里
 * 直接问场景，surface 是谁就交给 wlroots——它自己会按 seat client 查表。
 */
static struct wlr_surface *dnd_surface_at(struct woodland_server *server, double lx, double ly,
                                          double *sx, double *sy)
{
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    if (woodland_dnd_node_is_icon(&server->dnd, node)) {
        return NULL;
    }
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    return scene_surface ? scene_surface->surface : NULL;
}

static void dnd_position_icon(struct woodland_server *server)
{
    struct woodland_dnd *dnd = &server->dnd;
    if (!dnd->icons || !server->cursor) {
        return;
    }
    /* 布局坐标：scene 根的子节点和视图用的是同一套坐标，跨输出天然正确 */
    wlr_scene_node_set_position(&dnd->icons->node, (int)server->cursor->x,
                                (int)server->cursor->y);
}

static void dnd_update_target(struct woodland_server *server, uint32_t time)
{
    struct woodland_dnd *dnd = &server->dnd;
    if (!server->seat || !server->cursor) {
        return;
    }
    double sx = 0, sy = 0;
    struct wlr_surface *surface =
        dnd_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);

    /*
     * 只在目标变化时 enter：drag_set_focus 现在有早退，重复 enter 是无谓开销，
     * 而且一旦 wlroots 改了早退条件就会变成"每帧重建一次 offer"。
     */
    if (surface != dnd->last_surface) {
        dnd->last_surface = surface;
        if (surface) {
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        } else {
            /* 必须是 notify_ 版本：裸 clear_focus 绕过 grab，drag 收不到 leave */
            wlr_seat_pointer_notify_clear_focus(server->seat);
        }
    }
    if (surface) {
        wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
    }
}

/* ==================== 生命周期 ==================== */

/*
 * 拖拽前后的不变量自检。
 *
 * selfcheck.c 在 shell 库里，而 dnd 是 shell 的下游（光标层要调 dnd），
 * 直接调会成环——走 server 上的钩子，NULL 表示 shell 没建起来（单测）。
 */
static void dnd_schedule_selfcheck(struct woodland_server *server, const char *reason)
{
    if (server->selfcheck_schedule_hook) {
        server->selfcheck_schedule_hook(server, reason);
    }
}

static const char *dnd_grab_name(enum wlr_drag_grab_type type)
{
    switch (type) {
    case WLR_DRAG_GRAB_KEYBOARD:
        return "keyboard";
    case WLR_DRAG_GRAB_KEYBOARD_POINTER:
        return "pointer";
    case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
        return "touch";
    }
    return "unknown";
}

bool woodland_dnd_active(const struct woodland_server *server)
{
    return server != NULL && server->dnd.drag != NULL;
}

void woodland_dnd_motion(struct woodland_server *server, uint32_t time)
{
    if (!woodland_dnd_active(server) || !server->cursor) {
        return;
    }
    dnd_position_icon(server);
    dnd_update_target(server, time);
}

void woodland_dnd_handle_start(struct woodland_server *server, struct wlr_drag *drag)
{
    struct woodland_dnd *dnd = &server->dnd;
    if (!drag || dnd->drag == drag) {
        return;
    }
    if (dnd->drag) {
        /* 上一个还没收干净：宁可不要这次图标，也不要在同一棵树上叠两层 */
        wlr_log(WLR_ERROR, "start_drag while another drag is still active");
        return;
    }
    dnd->drag = drag;
    woodland_listener_connect(&dnd->events.destroy, &drag->events.destroy,
                              dnd_handle_destroy, "dnd.destroy");

    if (dnd->icons) {
        wlr_scene_node_set_enabled(&dnd->icons->node, true);
        /*
         * 置顶到底：panel/dock/锁屏遮罩都是 scene 根的子节点，图标不置顶
         * 就会被最上面那层盖住（labwc 0.7.3 修的就是这个）。
         * 位置在起手就先给一次，别等第一帧 motion——否则图标会从左上角
         * 闪过来（labwc 就是这么闪的）。
         */
        wlr_scene_node_raise_to_top(&dnd->icons->node);
        dnd_position_icon(server);
    }
    dnd_icon_create(dnd, drag->icon);

    woodland_trace_add("dnd", 0, "start icon=%d grab=%s", drag->icon != NULL,
                       dnd_grab_name(drag->grab_type));
    /* 起手就算一次目标：客户端完全可以在第一次 motion 之前松手 */
    dnd_update_target(server, 0);
    dnd_schedule_selfcheck(server, "dnd start");
}

void woodland_dnd_handle_destroy(struct woodland_server *server, struct wlr_drag *drag)
{
    struct woodland_dnd *dnd = &server->dnd;
    if (!drag || dnd->drag != drag) {
        return;
    }
    /* 先摘监听器：wlr_drag 在 emit 完 destroy 之后会断言监听器链表为空 */
    if (dnd->events.destroy.link.prev) {
        wl_list_remove(&dnd->events.destroy.link);
    }
    dnd->drag = NULL;
    dnd->last_surface = NULL;
    dnd_icon_release(dnd);
    if (dnd->icons) {
        wlr_scene_node_set_enabled(&dnd->icons->node, false);
    }
    /*
     * 不 refocus：woodland 是点击聚焦语义，拖拽期间焦点没变，收手后留在
     * 源窗口是对的（openbox / mutter 同款）。需要 followMouse 行为时在这里
     * 按光标下的 surface 补一次 focus 即可。
     */
    woodland_trace_add("dnd", 0, "end dropped=%d", drag->dropped ? 1 : 0);
    dnd_schedule_selfcheck(server, "dnd end");
}

void woodland_dnd_cancel(struct woodland_server *server)
{
    struct woodland_dnd *dnd = &server->dnd;
    if (!server->seat || !dnd->drag) {
        return;
    }
    woodland_trace_add("dnd", 0, "cancel");

    /*
     * 两条 grab 的 cancel 都指向 drag_destroy，所以先按 grab 类型选一条；
     * 拖拽已经销毁就别再碰第二条（那时 grab 已经回到 default，是空操作，
     * 但读 dnd->drag->grab_type 会踩已释放的对象）。
     */
    switch (dnd->drag->grab_type) {
    case WLR_DRAG_GRAB_KEYBOARD_POINTER:
        wlr_seat_pointer_end_grab(server->seat);
        break;
    case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
        wlr_seat_touch_end_grab(server->seat);
        break;
    case WLR_DRAG_GRAB_KEYBOARD:
        break;
    }
    if (dnd->drag) {
        /* 纯键盘 grab 的拖拽（少见）：只能从键盘侧取消 */
        wlr_seat_keyboard_end_grab(server->seat);
    }
}

/* ==================== seat 事件 ==================== */

static void dnd_handle_request_start(struct wl_listener *listener, void *data)
{
    struct woodland_dnd *dnd = wl_container_of(listener, dnd, events.request);
    struct woodland_server *server = dnd->server;
    struct wlr_seat_request_start_drag_event *event = data;
    struct wlr_seat *seat = server->seat;

    if (seat->drag != NULL) {
        /* wlroots 自己也会挡，这里只是不让源码存在"两个 drag"的可能 */
        woodland_trace_add("dnd", 0, "reject: another drag is active");
        if (event->drag->source) {
            wlr_data_source_destroy(event->drag->source);
        }
        return;
    }

    if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
        /* 只起 pointer drag：woodland 还没接 touch 设备（server_new_input 只有
         * keyboard / pointer 两个分支），接上 WLR_INPUT_DEVICE_TOUCH 之后再补
         * wlr_seat_validate_touch_grab_serial 这一档，别先留个永不执行的死分支。 */
        wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
        return;
    }

    /* 校验没过：拖拽源必须销毁，否则客户端会一直挂在那里等 start_drag */
    woodland_trace_add("dnd", 0, "reject: serial %u is not a pointer grab", event->serial);
    if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
}

static void dnd_handle_start(struct wl_listener *listener, void *data)
{
    struct woodland_dnd *dnd = wl_container_of(listener, dnd, events.start);
    woodland_dnd_handle_start(dnd->server, data);
}

static void dnd_handle_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_dnd *dnd = wl_container_of(listener, dnd, events.destroy);
    woodland_dnd_handle_destroy(dnd->server, data);
}

/* ==================== init / finish ==================== */

bool woodland_dnd_init(struct woodland_server *server)
{
    struct woodland_dnd *dnd = &server->dnd;
    /* 生产路径上 server 是 calloc 出来的，但单测会在栈上伪造它；顺手归一。 */
    memset(dnd, 0, sizeof(*dnd));
    dnd->server = server;

    dnd->icons = wlr_scene_tree_create(&server->scene->tree);
    if (!dnd->icons) {
        wlr_log(WLR_ERROR, "failed to create drag icon tree");
        return false;
    }
    /* 空闲时 disable：不参与渲染，也不参与 hit-test（隐藏节点会被跳过） */
    wlr_scene_node_set_enabled(&dnd->icons->node, false);

    if (!server->seat) {
        /* 调用点保证 seat 已经建好；走到这里说明 init 顺序被改坏了 */
        wlr_log(WLR_ERROR, "dnd init before seat");
        return false;
    }
    woodland_listener_connect(&dnd->events.request, &server->seat->events.request_start_drag,
                              dnd_handle_request_start, "dnd.request_start");
    woodland_listener_connect(&dnd->events.start, &server->seat->events.start_drag,
                              dnd_handle_start, "dnd.start");
    return true;
}

void woodland_dnd_finish(struct woodland_server *server)
{
    struct woodland_dnd *dnd = &server->dnd;
    /* 拖拽中退出：先取消，让 wlr_drag 走自己的销毁路径（否则它内部还挂着
     * 我们的监听器，display destroy 时会踩到已释放的回调） */
    if (dnd->drag) {
        woodland_dnd_cancel(server);
    }
    if (dnd->events.request.link.prev) {
        wl_list_remove(&dnd->events.request.link);
    }
    if (dnd->events.start.link.prev) {
        wl_list_remove(&dnd->events.start.link);
    }
    dnd_icon_release(dnd);
    if (dnd->icons) {
        wlr_scene_node_destroy(&dnd->icons->node);
        dnd->icons = NULL;
    }
    dnd->server = NULL;
}
