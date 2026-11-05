// SPDX-License-Identifier: GPL-2.0-only
/*
 * shell 主文件：xdg toplevel 的生命周期、popup、装饰协商，以及双后端
 * （xdg / X11）访问器。
 *
 * 这里只回答"视图什么时候出现、什么时候消失、内容怎么跟客户端对齐"。
 * 窗口状态（最大化/全屏/标题）、交互拖动、光标事件都不在这里：
 *
 *   toplevel_state.c  状态标志与 foreign-toplevel 同步
 *   interactive.c     move/resize 与贴边铺满
 *   cursor.c          光标事件分发与 SSD 按钮状态
 *   focus.c           焦点计算与切换
 *   placement.c       放置与最大化/全屏几何
 *   selfcheck.c       不变量断言
 *
 * 分工的边界是"谁拥有状态、谁只读它"，不是按行数切。
 */
#include "util/listener.h"
#include <stdlib.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_pointer.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "capture.h"
#include "effect.h"
#include "layer.h"
#include "output.h"
#include "selfcheck.h"
#include "session_lock.h"
#include "shell_internal.h"
#include "ssd.h"
#include "trace.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

/* ==================== 双后端访问器 ==================== */

struct wlr_surface *woodland_view_surface(const struct woodland_view *view)
{
    if (!view) {
        return NULL;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        return xsurface ? xsurface->surface : NULL;
    }
#endif
    return view->xdg_toplevel ? view->xdg_toplevel->base->surface : NULL;
}

void woodland_view_content_box(const struct woodland_view *view, struct wlr_box *box)
{
    *box = (struct wlr_box){0};
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        /* X11 窗口的 surface 就是窗口本身，几何原点恒为 0,0 */
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (xsurface) {
            box->width = xsurface->width;
            box->height = xsurface->height;
        }
        return;
    }
#endif
    /* xdg_toplevel 可能为空：X11 分支走到这里说明 type 标错了，
     * 而单测里也常拿 {0} 的假 view 调这个接口 */
    if (!view->xdg_toplevel) {
        return;
    }
    *box = view->xdg_toplevel->base->geometry;
}

void woodland_view_request_size(struct woodland_view *view, int width, int height)
{
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
        if (!xsurface) {
            return;
        }
        /* X11 没有"尺寸由客户端决定"的语义，必须给出具体值。
         * 位置用布局坐标直接当 root 坐标发出去：单输出（布局原点 0,0）下等价，
         * 多输出时 X11 客户端看到的是同一坐标系，暂不做输出内部偏移换算。 */
        if (width < 1) {
            width = 1;
        }
        if (height < 1) {
            height = 1;
        }
        wlr_xwayland_surface_configure(xsurface, (int16_t)view->scene_tree->node.x,
                                       (int16_t)view->scene_tree->node.y, (uint16_t)width,
                                       (uint16_t)height);
        return;
    }
#endif
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
}

void woodland_view_set_activated(struct woodland_view *view, bool activated)
{
    if (!view) {
        return;
    }
    /* 记下我们要求的状态：X11 后端没有可回读的 activated 字段，
     * 而三态同步与不变量检查都需要这个真值 */
    view->activated = activated;
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_activate(view->xwayland_surface, activated);
        }
        return;
    }
#endif
    if (view->xdg_toplevel) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
    }
}

void woodland_view_close(struct woodland_view *view)
{
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_close(view->xwayland_surface);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

struct woodland_view *woodland_view_from_surface(struct wlr_surface *surface)
{
    if (!surface) {
        return NULL;
    }
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root);
    while (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg_surface->popup) {
        if (!xdg_surface->popup->parent) {
            break;
        }
        root = wlr_surface_get_root_surface(xdg_surface->popup->parent);
        xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root);
    }
    if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->toplevel) {
        struct wlr_scene_tree *tree = xdg_surface->data;
        return tree ? tree->node.data : NULL;
    }
#if WOODLAND_HAVE_XWAYLAND
    struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
    return xsurface ? xsurface->data : NULL;
#else
    return NULL;
#endif
}

/* ==================== view 聚焦与命中 ==================== */

struct woodland_view *woodland_view_at(struct woodland_server *server, double lx, double ly,
                                       struct wlr_surface **surface, double *sx, double *sy)
{
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;
    return woodland_view_from_surface(scene_surface->surface);
}

/* ==================== toplevel 视图事件 ==================== */

/*
 * 服务端最终采用的装饰模式。
 *
 * 只看策略位 deco_csd，**不能**看 view->ssd 是否存在：SSD 对象在 map 时才创建，
 * 而 zxdg 协商发生在 initial commit（早于 map），那时 ssd 必为 NULL，
 * 拿它判断会给"请求 server-side"的客户端回一个 client-side configure，
 * 客户端于是自己再画一条标题栏 —— 叠加我们的 SSD 就是双标题栏（foot 即此例）。
 */
static enum wlr_xdg_toplevel_decoration_v1_mode
view_decoration_mode(const struct woodland_view *view)
{
    return view->deco_csd ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                          : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

void woodland_view_map(struct woodland_view *view)
{
    if (view->mapped) {
        return;
    }
    struct woodland_server *server = view->server;
    view->mapped = true;
    /* 有已映射父视图则 inherit 其工作区，否则进当前活动工作区 */
    if (view->parent && view->parent->mapped) {
        view->workspace = view->parent->workspace;
    } else {
        view->workspace = server->active_workspace;
    }
    wlr_scene_node_reparent(&view->scene_tree->node, view->workspace->tree);
    wl_list_insert(&server->views, &view->link);
    woodland_trace_add("map", view->id, "ws=%s parent=%u",
                       view->workspace ? view->workspace->name : "-",
                       view->parent ? view->parent->id : 0);
    /* 首次 map 时创建 SSD（未 map 前不显示空装饰，同 labwc handle_map） */
    if (!view->ssd && !view->deco_csd) {
        woodland_ssd_create(view);
    }
    /* 只在首次 map 放置；remap 保留用户摆好的位置 */
    if (!view->placed) {
        view->placed = true;
        woodland_view_place_initial(view);
    }
    woodland_focus_view(view, true);
    woodland_view_update_outputs(view);
    /* CSD 视图没有 SSD，map 之后补一次圆角/模糊（SSD 视图由 ssd_create 覆盖） */
    woodland_effect_update(view);
    woodland_selfcheck_schedule(server, "map");
}

void woodland_view_unmap(struct woodland_view *view)
{
    /* 记在函数开头：后面的 focus_none / fallback 也会各写一条，
     * 顺序就是真实的因果顺序（先 unmap，再处理焦点） */
    woodland_trace_add("unmap", view->id, "mapped=%d", view->mapped ? 1 : 0);
    if (view == view->server->grabbed_view) {
        woodland_cursor_reset_mode(view->server);
    }
    /* SSD 即将销毁：清掉指向它的指针状态，避免悬空 */
    if (view->server->hover_view == view) {
        view->server->hover_view = NULL;
        view->server->hover_part = WOODLAND_SSD_NONE;
    }
    if (view->server->pressed_view == view) {
        view->server->pressed_view = NULL;
        view->server->pressed_part = WOODLAND_SSD_NONE;
    }
    woodland_session_lock_handle_view_destroy(view->server, view);
    /* 焦点：先按正常卸载路径复位（改 activated / SSD 激活色 / foreign-toplevel，
     * 并清掉键盘焦点），再摘链表。只把 focused_view 置空会留下"句柄还是
     * 激活态"的残影——没有键盘设备时 focus_current() 反查不到旧视图，
     * 回退时就没人负责把它变暗。摘链表放在后面，回退会自然跳过它。 */
    bool was_focused = (view->server->focused_view == view);
    if (was_focused) {
        woodland_focus_none(view->server);
        /* 没有 seat 时 focus_none 会提前返回，指针得自己清 */
        view->server->focused_view = NULL;
    }
    /* 只在确实在链表里时摘除：X11 窗口可能没 map 过就 unmap */
    if (view->mapped) {
        wl_list_remove(&view->link);
        view->mapped = false;
        woodland_view_update_outputs(view);
    }
    /* unmap 时销毁 SSD（避免残留空装饰）；remap 时按 deco_csd 重建 */
    if (view->ssd) {
        woodland_ssd_destroy(view);
    }
    /* 焦点回退：此时视图已不在链表里，回退不会再选中它 */
    if (was_focused) {
        woodland_focus_fallback(view->server);
    }
    woodland_selfcheck_schedule(view->server, "unmap");
}

void woodland_view_destroy(struct woodland_view *view)
{
    woodland_trace_add("destroy", view->id, "type=%s mapped=%d",
                       view->type == WOODLAND_VIEW_XWAYLAND ? "x11" : "xdg", view->mapped ? 1 : 0);
    /* 销毁中的视图不能作为回退目标，也不能被 focus_current 反查到：
     * 先清焦点（只改状态，不挑新目标），等视图摘出链表后再回退 */
    woodland_session_lock_handle_view_destroy(view->server, view);
    bool was_focused = (view->server->focused_view == view);
    if (was_focused) {
        woodland_focus_none(view->server);
        /* 没有 seat 时 focus_none 会提前返回，指针得自己清 */
        view->server->focused_view = NULL;
    }
    if (view->mapped) {
        wl_list_remove(&view->link);
        view->mapped = false;
    }
    /* foreign-toplevel handle 与 listener */
    if (view->toplevel_handle) {
        wl_list_remove(&view->ft_request_activate.link);
        wl_list_remove(&view->ft_request_maximize.link);
        wl_list_remove(&view->ft_request_fullscreen.link);
        wl_list_remove(&view->ft_request_minimize.link);
        wl_list_remove(&view->ft_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(view->toplevel_handle);
        view->toplevel_handle = NULL;
    }
    /* ext-foreign-toplevel-list 句柄 */
    if (view->ext_toplevel_handle) {
        wl_list_remove(&view->ext_handle_destroy.link);
        wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_toplevel_handle);
        view->ext_toplevel_handle = NULL;
    }
    /* 按窗口捕获源：源随 scene 节点释放，这里只摘监听 */
    woodland_capture_view_finish(view);
    /* SSD 在 scene 树销毁时级联释放，这里只清指针 */
    if (view->ssd) {
        woodland_ssd_destroy(view);
    }
    if (view->server->hover_view == view) {
        view->server->hover_view = NULL;
        view->server->hover_part = WOODLAND_SSD_NONE;
    }
    if (view->server->pressed_view == view) {
        view->server->pressed_view = NULL;
        view->server->pressed_part = WOODLAND_SSD_NONE;
    }
    if (was_focused) {
        woodland_focus_fallback(view->server);
    }
    woodland_selfcheck_schedule(view->server, "destroy");
    free(view->title);
    free(view->app_id);
    free(view->tag);
    free(view);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, map);
    woodland_view_map(view);
    (void)data;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    woodland_view_unmap(view);
    (void)data;
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, commit);
    struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
    if (xdg->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        /* initial commit：此时 surface 已 initialized，可以 set_mode 完成 zxdg 协商 */
        if (view->deco) {
            wlr_xdg_toplevel_decoration_v1_set_mode(view->deco, view_decoration_mode(view));
        }
    }
    /* 内容尺寸/状态变化后同步 SSD 几何（标题栏/边框/按钮） */
    woodland_ssd_update(view);
    /* CSD 视图没有 SSD，圆角/模糊只能在这里跟一次（SSD 视图是重复调用，幂等） */
    woodland_effect_update(view);
    (void)data;
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);
    wl_list_remove(&view->set_parent.link);
    /* zxdg_decoration 监听器（协商过才注册） */
    if (view->deco_registered) {
        wl_list_remove(&view->deco_destroy.link);
        wl_list_remove(&view->deco_request_mode.link);
        view->deco_registered = false;
    }
    woodland_view_destroy(view);
    (void)data;
}

/* ==================== new_toplevel ==================== */

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct woodland_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_view");
        return;
    }
    view->server = server;
    view->id = ++server->next_view_id;
    view->type = WOODLAND_VIEW_XDG;
    view->xdg_toplevel = xdg_toplevel;
    view->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
    view->scene_tree->node.data = view;
    xdg_toplevel->base->data = view->scene_tree;

    /* 生命周期：出现/消失由本文件管 */
    woodland_listener_connect(&view->map, &xdg_toplevel->base->surface->events.map,
                              xdg_toplevel_map, "shell.map");
    woodland_listener_connect(&view->unmap, &xdg_toplevel->base->surface->events.unmap,
                              xdg_toplevel_unmap, "shell.unmap");
    woodland_listener_connect(&view->commit, &xdg_toplevel->base->surface->events.commit,
                              xdg_toplevel_commit, "shell.commit");
    woodland_listener_connect(&view->destroy, &xdg_toplevel->events.destroy, xdg_toplevel_destroy,
                              "shell.destroy");

    /* 状态与交互：各自的文件负责注册自己那几个监听器 */
    shell_interactive_attach(view);
    shell_toplevel_state_attach(view);

    woodland_view_init_handles(view);
}

/* ==================== popup ==================== */

struct woodland_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void xdg_popup_commit(struct wl_listener *listener, void *data)
{
    struct woodland_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
    (void)data;
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    (void)data;
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wlr_xdg_popup *xdg_popup = data;

    /* layer-shell 的 popup 分两步：先经 xdg_wm_base.get_popup 创建
     * （此时 parent 尚未关联，为 NULL），再经 zwlr_layer_surface_v1.get_popup
     * 关联到 layer surface。父为 NULL 的 popup 交给 layer.c 的
     * layer_surface->events.new_popup 处理，这里跳过。 */
    if (xdg_popup->parent == NULL) {
        return;
    }

    struct woodland_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_popup");
        return;
    }
    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (parent == NULL) {
        free(popup);
        return;
    }
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    woodland_listener_connect(&popup->commit, &xdg_popup->base->surface->events.commit,
                              xdg_popup_commit, "shell.commit");
    woodland_listener_connect(&popup->destroy, &xdg_popup->events.destroy, xdg_popup_destroy,
                              "shell.destroy");
}

/* ==================== zxdg_decoration ==================== */

/* 应用客户端请求的装饰模式（不直接 set_mode，避免未 initialized 时 assert） */
static void deco_apply_mode(struct woodland_view *view,
                            enum wlr_xdg_toplevel_decoration_v1_mode requested)
{
    switch (requested) {
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        view->deco_csd = false;
        if (view->mapped && !view->ssd) {
            woodland_ssd_create(view);
        }
        break;
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        view->deco_csd = true;
        if (view->ssd) {
            woodland_ssd_destroy(view);
        }
        break;
    case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE:
    default:
        /* 未指定：默认 SSD（见设计决策） */
        view->deco_csd = false;
        if (view->mapped && !view->ssd) {
            woodland_ssd_create(view);
        }
        break;
    }
}

/* 客户端 set_mode/unset_mode 时触发：更新状态，set_mode 延迟到 initial commit */
static void deco_request_mode(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, deco_request_mode);
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;

    deco_apply_mode(view, deco->requested_mode);

    /* 仅在 surface 已 initialized 时才能 schedule configure；回给客户端的模式
     * 必须是"我们实际会采用的模式"，否则客户端会重复画装饰 */
    if (deco->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco, view_decoration_mode(view));
    }
}

static void deco_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_view *view = wl_container_of(listener, view, deco_destroy);
    if (view->deco_registered) {
        wl_list_remove(&view->deco_destroy.link);
        wl_list_remove(&view->deco_request_mode.link);
        view->deco_registered = false;
    }
    view->deco = NULL;
    (void)data;
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, new_toplevel_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct wlr_xdg_surface *xdg_surface = deco->toplevel->base;
    if (!xdg_surface || !xdg_surface->data) {
        return;
    }
    struct wlr_scene_tree *tree = xdg_surface->data;
    struct woodland_view *view = tree->node.data;
    if (!view) {
        return;
    }

    /* 注册 mode 请求与销毁监听 */
    view->deco = deco;
    woodland_listener_connect(&view->deco_request_mode, &deco->events.request_mode,
                              deco_request_mode, "shell.request_mode");
    woodland_listener_connect(&view->deco_destroy, &deco->events.destroy, deco_destroy,
                              "shell.destroy");
    view->deco_registered = true;

    /* 立即按客户端当前请求模式调整（未 initialized 时 set_mode 延迟到 initial commit） */
    deco_apply_mode(view, deco->requested_mode);
    if (xdg_surface->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco, view_decoration_mode(view));
    }
}

/* ==================== init/finish ==================== */

bool woodland_shell_init(struct woodland_server *server)
{
    wl_list_init(&server->views);

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        return false;
    }

    server->foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server->display);

    /* ext-foreign-toplevel-list-v1：只读的窗口列表，配合
     * ext-foreign-toplevel-image-capture-source-v1 做按窗口截图 */
    server->ext_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);

    /* xdg-dialog-v1：模态对话框标志。GTK4 / Qt 的模态对话框只通过这个
     * 扩展声明，没有它就只能按 parent 关系猜，焦点分配会不准 */
    server->xdg_dialog_manager = wlr_xdg_wm_dialog_v1_create(server->display, 1);

    woodland_listener_connect(&server->new_xdg_toplevel, &server->xdg_shell->events.new_toplevel,
                              server_new_xdg_toplevel, "shell.new_toplevel");
    woodland_listener_connect(&server->new_xdg_popup, &server->xdg_shell->events.new_popup,
                              server_new_xdg_popup, "shell.new_popup");
    if (server->xdg_decoration_manager) {
        woodland_listener_connect(&server->new_toplevel_decoration,
                                  &server->xdg_decoration_manager->events.new_toplevel_decoration,
                                  server_new_toplevel_decoration, "shell.new_toplevel_decoration");
    }

    return true;
}

void woodland_shell_finish(struct woodland_server *server)
{
    if (server->xdg_shell) {
        wl_list_remove(&server->new_xdg_toplevel.link);
        wl_list_remove(&server->new_xdg_popup.link);
    }
    if (server->xdg_decoration_manager) {
        wl_list_remove(&server->new_toplevel_decoration.link);
    }
}
