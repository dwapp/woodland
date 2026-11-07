// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xwayland 基础支持。
 *
 * 分两条路径：
 *   - managed（普通顶层窗口）：建 woodland_view，走与 xdg 完全相同的
 *     SSD 装饰 / 放置 / 聚焦 / 最大化 / 贴边吸附逻辑；
 *   - unmanaged（override-redirect：菜单、提示、拖拽影子）：单独一棵
 *     scene 树，按 X 侧坐标定位，无装饰、不参与焦点与窗口列表。
 *
 * 时序（与 labwc 一致）：new_surface → map_request（拿得到尺寸，可放置并
 * configure）→ associate（wlr_surface 可用，挂 surface 事件）→ map（真正上屏）。
 */
#include "util/listener.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "cursor.h"
#include "output.h"
#include "placement.h"
#include "woodland_config.h"

#include "scene.h"
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "effect.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "xwayland.h"
#include "focus.h"

/* ==================== unmanaged（override-redirect） ==================== */

struct woodland_unmanaged {
    struct wl_list link;               // server->xwayland_unmanaged
    struct woodland_server *server;
    struct wlr_xwayland_surface *xsurface;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_surface *scene_surface;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener set_override_redirect;
};

static void unmanaged_set_geometry(struct woodland_unmanaged *unmanaged);
static void unmanaged_map(struct wl_listener *listener, void *data);
static void unmanaged_unmap(struct wl_listener *listener, void *data);
static void unmanaged_update_filter_mode(struct woodland_unmanaged *unmanaged);
static void unmanaged_set_override_redirect(struct wl_listener *listener, void *data);
static void xwl_new_view(struct woodland_server *server, struct wlr_xwayland_surface *xsurface);
static void xwl_new_unmanaged(struct woodland_server *server, struct wlr_xwayland_surface *xsurface);
static void xwl_destroy(struct wl_listener *listener, void *data);

static void unmanaged_set_geometry(struct woodland_unmanaged *unmanaged) {
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;
    wlr_scene_node_set_position(&unmanaged->scene_tree->node,
                                xsurface->x, xsurface->y);
    unmanaged_update_filter_mode(unmanaged);
}
static enum wlr_scale_filter_mode xwl_get_scale_filter_mode(struct woodland_server *server,
                                                            float output_scale)
{
    enum woodland_scale_filter sf = WOODLAND_SCALE_FILTER_SMART;
    if (server && server->config) {
        sf = server->config->xwayland.scale_filter;
    }

    switch (sf) {
    case WOODLAND_SCALE_FILTER_NEAREST:
        return (output_scale > 1.0f) ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR;
    case WOODLAND_SCALE_FILTER_BILINEAR:
        return WLR_SCALE_FILTER_BILINEAR;
    case WOODLAND_SCALE_FILTER_SMART:
    default:
        if (output_scale > 1.0f && floorf(output_scale) == output_scale) {
            return WLR_SCALE_FILTER_NEAREST;
        }
        return WLR_SCALE_FILTER_BILINEAR;
    }
}

static void unmanaged_update_filter_mode(struct woodland_unmanaged *unmanaged) {
    if (!unmanaged || !unmanaged->scene_surface || !unmanaged->server) {
        return;
    }
    struct woodland_output *wo = woodland_output_at(unmanaged->server,
        unmanaged->scene_tree->node.x, unmanaged->scene_tree->node.y);
    float scale = (wo && wo->wlr_output && wo->wlr_output->scale > 0.0f) ? wo->wlr_output->scale : 1.0f;
    enum wlr_scale_filter_mode mode = xwl_get_scale_filter_mode(unmanaged->server, scale);
    if (unmanaged->scene_surface->buffer) {
        wlr_scene_buffer_set_filter_mode(unmanaged->scene_surface->buffer, mode);
    }
}

/* 挂上 wlr_surface：建 scene surface + 接 map/unmap */
static void unmanaged_attach_surface(struct woodland_unmanaged *unmanaged,
                                     struct wlr_surface *surface) {
    if (!surface || unmanaged->scene_surface) {
        return;
    }
    unmanaged->scene_surface = wlr_scene_surface_create(unmanaged->scene_tree,
                                                        surface);
    woodland_listener_connect(&unmanaged->map, &surface->events.map,
                          unmanaged_map, "xwayland.map");
    woodland_listener_connect(&unmanaged->unmap, &surface->events.unmap,
                          unmanaged_unmap, "xwayland.unmap");
    /* set_geometry 里已经会刷一次滤波（buffer 这时通常还没有，是空操作），
     * 真正的第一次生效在 map 之后 */
    if (surface->mapped) {
        unmanaged_map(&unmanaged->map, NULL);
    }
}

static void unmanaged_associate(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, associate);
    unmanaged_attach_surface(unmanaged, unmanaged->xsurface->surface);
    (void)data;
}

static void unmanaged_dissociate(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, dissociate);
    /* surface 正在销毁：先摘监听，scene surface 随 surface 自动释放 */
    if (unmanaged->scene_surface) {
        wl_list_remove(&unmanaged->map.link);
        wl_list_remove(&unmanaged->unmap.link);
        unmanaged->scene_surface = NULL;
    }
    (void)data;
}

static void unmanaged_map(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, map);
    unmanaged_set_geometry(unmanaged);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, true);
    if (unmanaged->xsurface &&
        wlr_xwayland_surface_override_redirect_wants_focus(unmanaged->xsurface)) {
        woodland_seat_focus_surface(unmanaged->server, unmanaged->xsurface->surface);
    }
    (void)data;
}

static void unmanaged_unmap(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, unmap);
    wlr_scene_node_set_enabled(&unmanaged->scene_tree->node, false);
    struct woodland_server *server = unmanaged->server;
    if (server && server->seat && unmanaged->xsurface &&
        server->seat->keyboard_state.focused_surface == unmanaged->xsurface->surface) {
        struct woodland_unmanaged *u;
        bool focused_next = false;
        wl_list_for_each_reverse(u, &server->xwayland_unmanaged, link) {
            if (u != unmanaged && u->xsurface && u->xsurface->surface &&
                u->xsurface->surface->mapped &&
                wlr_xwayland_surface_override_redirect_wants_focus(u->xsurface)) {
                woodland_seat_focus_surface(server, u->xsurface->surface);
                focused_next = true;
                break;
            }
        }
        if (!focused_next) {
            if (server->focused_view && server->focused_view->mapped) {
                woodland_focus_view(server->focused_view, false);
            } else {
                woodland_focus_fallback(server);
            }
        }
    }
    (void)data;
}

/* override-redirect 窗口自己决定几何，跟随 X 侧的 ConfigureNotify 即可 */
static void unmanaged_set_geometry_event(struct wl_listener *listener,
                                         void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, set_geometry);
    unmanaged_set_geometry(unmanaged);
    (void)data;
}

static void unmanaged_destroy(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, destroy);
    wl_list_remove(&unmanaged->associate.link);
    wl_list_remove(&unmanaged->dissociate.link);
    wl_list_remove(&unmanaged->destroy.link);
    wl_list_remove(&unmanaged->set_geometry.link);
    if (unmanaged->scene_surface) {
        wl_list_remove(&unmanaged->map.link);
        wl_list_remove(&unmanaged->unmap.link);
    }
    wl_list_remove(&unmanaged->link);
    wl_list_remove(&unmanaged->set_override_redirect.link);
    wlr_scene_node_destroy(&unmanaged->scene_tree->node);
    free(unmanaged);
    (void)data;
}
static void unmanaged_set_override_redirect(struct wl_listener *listener, void *data) {
    struct woodland_unmanaged *unmanaged =
        wl_container_of(listener, unmanaged, set_override_redirect);
    struct wlr_xwayland_surface *xsurface = unmanaged->xsurface;
    (void)data;
    if (!xsurface || xsurface->override_redirect) {
        return;
    }
    struct woodland_server *server = unmanaged->server;
    bool mapped = xsurface->surface && xsurface->surface->mapped;
    if (mapped) {
        unmanaged_unmap(&unmanaged->unmap, NULL);
    }
    unmanaged_destroy(&unmanaged->destroy, NULL);
    xwl_new_view(server, xsurface);
}


static void xwl_new_unmanaged(struct woodland_server *server,
                              struct wlr_xwayland_surface *xsurface) {
    struct woodland_unmanaged *unmanaged = calloc(1, sizeof(*unmanaged));
    if (!unmanaged) {
        wlr_log(WLR_ERROR, "failed to allocate unmanaged X11 window");
        return;
    }
    unmanaged->server = server;
    unmanaged->xsurface = xsurface;
    unmanaged->scene_tree =
        wlr_scene_tree_create(server->xwayland_unmanaged_tree);

    woodland_listener_connect(&unmanaged->associate, &xsurface->events.associate,
                          unmanaged_associate, "xwayland.associate");
    woodland_listener_connect(&unmanaged->dissociate, &xsurface->events.dissociate,
                          unmanaged_dissociate, "xwayland.dissociate");
    woodland_listener_connect(&unmanaged->destroy, &xsurface->events.destroy,
                          unmanaged_destroy, "xwayland.destroy");
    woodland_listener_connect(&unmanaged->set_geometry, &xsurface->events.set_geometry,
                          unmanaged_set_geometry_event, "xwayland.set_geometry");
    woodland_listener_connect(&unmanaged->set_override_redirect, &xsurface->events.set_override_redirect,
                          unmanaged_set_override_redirect, "xwayland.set_override_redirect");

    wl_list_insert(&server->xwayland_unmanaged, &unmanaged->link);

    /* 多数情况下 surface 在 associate 里才出现；已存在时直接挂 */
    unmanaged_attach_surface(unmanaged, xsurface->surface);
}

/* ==================== managed 顶层窗口 ==================== */

/*
 * 放置并回 configure。
 *
 * X11 客户端必须收到 configure 才会上屏，所以"首次放置"和"ack 尺寸"
 * 合并成一次：先按 SSD 边距算出内容原点，再用同一个尺寸 configure。
 */
static void xwl_place_and_configure(struct woodland_view *view) {
    if (!view->placed) {
        woodland_view_place_initial(view);
        view->placed = true;
    }
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    woodland_view_request_size(view, content.width, content.height);
}

static void xwl_map_request(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map_request);
    xwl_place_and_configure(view);
    (void)data;
}

static void xwl_map(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map);
    woodland_view_map(view);
    (void)data;
}

static void xwl_unmap(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    woodland_view_unmap(view);
    (void)data;
}

static void xwl_commit(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, commit);
    woodland_ssd_update(view);
    if (!view->ssd) {
        woodland_effect_update(view);
    }
    (void)data;
}

static void xwl_associate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, associate);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    if (!xsurface->surface) {
        return;
    }

    view->scene_surface = wlr_scene_surface_create(view->scene_tree,
                                                   xsurface->surface);
    woodland_xwayland_view_update_scale_filter(view);

    woodland_listener_connect(&view->map, &xsurface->surface->events.map,
                          xwl_map, "xwayland.map");
    woodland_listener_connect(&view->unmap, &xsurface->surface->events.unmap,
                          xwl_unmap, "xwayland.unmap");
    woodland_listener_connect(&view->commit, &xsurface->surface->events.commit,
                          xwl_commit, "xwayland.commit");

    /*
     * EWMH: 窗口映射前可能已经设置了 _NET_WM_STATE（fullscreen/maximized）。
     * 在 associate 阶段定下初始全屏/最大化状态（对齐 labwc）。
     */
    if (xsurface->fullscreen) {
        woodland_view_set_fullscreen(view, true, NULL);
    } else if (xsurface->maximized_horz || xsurface->maximized_vert) {
        woodland_view_set_maximized(view, true);
    }

    /* 属性在 map_request 之后才读完，这里定下装饰策略：
     * _MOTIF_WM_HINTS 请求无边框的窗口不画 SSD（同 labwc want_deco） */
    bool borderless =
        (xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL) ||
        !woodland_view_wants_foreign_toplevel(view);
    if (borderless != view->deco_csd) {
        view->deco_csd = borderless;
        /* SSD 边距变了（27px 标题栏 vs 0）：按新边距重算内容原点并重新 configure */
        view->placed = false;
        xwl_place_and_configure(view);
    }

    /* map_request 里还不知道父窗口，这里补上（供后续 remap 居中用） */
    if (!view->parent && xsurface->parent && xsurface->parent->data) {
        woodland_view_set_parent(view, xsurface->parent->data);
    }
    (void)data;
}

static void xwl_dissociate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, dissociate);
    /* surface 正在销毁：先摘监听（节点还活着，摘完才失效） */
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    view->scene_surface = NULL;
    (void)data;
}

static void xwl_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->associate.link);
    wl_list_remove(&view->dissociate.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->map_request.link);
    wl_list_remove(&view->request_configure.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->request_activate.link);
    wl_list_remove(&view->request_close.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_class.link);
    wl_list_remove(&view->set_parent.link);
    wl_list_remove(&view->set_geometry.link);
    wl_list_remove(&view->set_override_redirect.link);
    wl_list_remove(&view->set_window_type.link);
    wl_list_remove(&view->request_skip_taskbar.link);
    wl_list_remove(&view->set_decorations.link);
    if (view->xwayland_surface) {
        view->xwayland_surface->data = NULL;
    }
    woodland_view_destroy(view);
    (void)data;
}

static void xwl_request_configure(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    /* 最大化/全屏下由 WM 决定几何，忽略客户端的尺寸请求 */
    if (view->maximized || view->fullscreen) {
        return;
    }
    woodland_view_request_size(view, event->width, event->height);
}

static void xwl_request_move(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xwl_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

static void xwl_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_maximize);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    woodland_view_set_maximized(view,
        xsurface->maximized_horz || xsurface->maximized_vert);
    (void)data;
}

static void xwl_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_fullscreen);
    woodland_view_set_fullscreen(view, view->xwayland_surface->fullscreen, NULL);
    (void)data;
}

static void xwl_request_minimize(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_minimize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_minimize);
    woodland_view_set_minimized(view, event->minimize);
}

static void xwl_request_activate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view,
                                                 request_activate);
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_focus_view(view, true);
    (void)data;
}

static void xwl_request_close(struct wl_listener *listener, void *data) {
    /* 客户端自己的 WM_DELETE_WINDOW 请求：由 wlroots 转发，无需处理 */
    (void)listener;
    (void)data;
}

static void xwl_set_title(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    woodland_view_set_title(view, view->xwayland_surface->title);
    (void)data;
}

static void xwl_set_class(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_class);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    /* X11 的 app_id 近似取 WM_CLASS 的 instance（对齐 labwc 的 app_id 语义） */
    const char *app_id = xsurface->instance ? xsurface->instance
                                            : xsurface->class;
    woodland_view_set_app_id(view, app_id);
    (void)data;
}

static void xwl_set_parent(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xwayland_surface *parent = view->xwayland_surface->parent;
    woodland_view_set_parent(view, parent ? parent->data : NULL);
    (void)data;
}

static void xwl_set_geometry(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_geometry);
    /* 位置由 WM 决定；尺寸变化后同步 SSD（客户端自己 resize 的情况） */
    if (view->mapped) {
        woodland_ssd_update(view);
    }
    (void)data;
}
static void xwl_set_override_redirect(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_override_redirect);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface || !xsurface->override_redirect) {
        return;
    }
    struct woodland_server *server = view->server;
    bool mapped = xsurface->surface && xsurface->surface->mapped;
    if (mapped) {
        woodland_view_unmap(view);
    }
    xwl_destroy(&view->destroy, xsurface);
    xwl_new_unmanaged(server, xsurface);
}

static void xwl_set_window_type(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_window_type);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface) {
        return;
    }
    bool borderless = (xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL) ||
                      !woodland_view_wants_foreign_toplevel(view);
    if (borderless != view->deco_csd) {
        view->deco_csd = borderless;
        if (view->mapped) {
            if (borderless && view->ssd) {
                woodland_ssd_destroy(view);
                woodland_effect_update(view);
            } else if (!borderless && !view->ssd) {
                woodland_ssd_create(view);
            }
        }
        view->placed = false;
        xwl_place_and_configure(view);
    }
    woodland_view_update_taskbar_presence(view);
}

static void xwl_set_decorations(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_decorations);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface) {
        return;
    }
    bool borderless = (xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL) ||
                      !woodland_view_wants_foreign_toplevel(view);
    if (borderless != view->deco_csd) {
        view->deco_csd = borderless;
        if (view->mapped) {
            if (borderless && view->ssd) {
                woodland_ssd_destroy(view);
                woodland_effect_update(view);
            } else if (!borderless && !view->ssd) {
                woodland_ssd_create(view);
            }
        }
        view->placed = false;
        xwl_place_and_configure(view);
    }
}

static void xwl_request_skip_taskbar(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_skip_taskbar);
    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    (void)data;
    if (!xsurface) {
        return;
    }
    wlr_xwayland_surface_set_skip_taskbar(xsurface, xsurface->skip_taskbar);
    woodland_view_update_taskbar_presence(view);
}

static void xwl_new_view(struct woodland_server *server,
                         struct wlr_xwayland_surface *xsurface) {
    struct woodland_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_view");
        return;
    }
    view->server = server;
    view->id = ++server->next_view_id;
    view->type = WOODLAND_VIEW_XWAYLAND;
    view->xwayland_surface = xsurface;
    view->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    if (!view->scene_tree) {
        wlr_log(WLR_ERROR, "failed to create scene tree for X11 view");
        free(view);
        return;
    }
    view->scene_tree->node.data = view;
    xsurface->data = view;

    woodland_listener_connect(&view->associate, &xsurface->events.associate,
                          xwl_associate, "xwayland.associate");
    woodland_listener_connect(&view->dissociate, &xsurface->events.dissociate,
                          xwl_dissociate, "xwayland.dissociate");
    woodland_listener_connect(&view->destroy, &xsurface->events.destroy,
                          xwl_destroy, "xwayland.destroy");

    woodland_listener_connect(&view->map_request, &xsurface->events.map_request,
                          xwl_map_request, "xwayland.map_request");
    woodland_listener_connect(&view->request_configure, &xsurface->events.request_configure,
                          xwl_request_configure, "xwayland.request_configure");
    woodland_listener_connect(&view->request_move, &xsurface->events.request_move,
                          xwl_request_move, "xwayland.request_move");
    woodland_listener_connect(&view->request_resize, &xsurface->events.request_resize,
                          xwl_request_resize, "xwayland.request_resize");
    woodland_listener_connect(&view->request_maximize, &xsurface->events.request_maximize,
                          xwl_request_maximize, "xwayland.request_maximize");
    woodland_listener_connect(&view->request_fullscreen, &xsurface->events.request_fullscreen,
                          xwl_request_fullscreen, "xwayland.request_fullscreen");
    woodland_listener_connect(&view->request_minimize, &xsurface->events.request_minimize,
                          xwl_request_minimize, "xwayland.request_minimize");
    woodland_listener_connect(&view->request_activate, &xsurface->events.request_activate,
                          xwl_request_activate, "xwayland.request_activate");
    woodland_listener_connect(&view->request_close, &xsurface->events.request_close,
                          xwl_request_close, "xwayland.request_close");
    woodland_listener_connect(&view->set_title, &xsurface->events.set_title,
                          xwl_set_title, "xwayland.set_title");
    woodland_listener_connect(&view->set_class, &xsurface->events.set_class,
                          xwl_set_class, "xwayland.set_class");
    woodland_listener_connect(&view->set_parent, &xsurface->events.set_parent,
                          xwl_set_parent, "xwayland.set_parent");
    woodland_listener_connect(&view->set_geometry, &xsurface->events.set_geometry,
                          xwl_set_geometry, "xwayland.set_geometry");
    woodland_listener_connect(&view->set_override_redirect, &xsurface->events.set_override_redirect,
                          xwl_set_override_redirect, "xwayland.set_override_redirect");
    woodland_listener_connect(&view->set_window_type, &xsurface->events.set_window_type,
                          xwl_set_window_type, "xwayland.set_window_type");
    woodland_listener_connect(&view->request_skip_taskbar, &xsurface->events.request_skip_taskbar,
                          xwl_request_skip_taskbar, "xwayland.request_skip_taskbar");
    woodland_listener_connect(&view->set_decorations, &xsurface->events.set_decorations,
                          xwl_set_decorations, "xwayland.set_decorations");

    woodland_view_set_title(view, xsurface->title);
    woodland_view_set_app_id(view, xsurface->instance
                                      ? xsurface->instance : xsurface->class);

    if (xsurface->surface) {
        xwl_associate(&view->associate, NULL);
        if (xsurface->surface->mapped) {
            xwl_map(&view->map, NULL);
        }
    }
}


static void xwl_new_surface(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    if (xsurface->override_redirect) {
        xwl_new_unmanaged(server, xsurface);
        return;
    }

    xwl_new_view(server, xsurface);
}
void woodland_xwayland_view_update_scale_filter(struct woodland_view *view) {
    if (!view || view->type != WOODLAND_VIEW_XWAYLAND || !view->scene_surface) {
        return;
    }
    struct woodland_output *wo = woodland_output_for_view(view);
    float scale = (wo && wo->wlr_output && wo->wlr_output->scale > 0.0f) ? wo->wlr_output->scale : 1.0f;
    enum wlr_scale_filter_mode mode = xwl_get_scale_filter_mode(view->server, scale);
    if (view->scene_surface->buffer && view->scene_surface->buffer->filter_mode != mode) {
        wlr_scene_buffer_set_filter_mode(view->scene_surface->buffer, mode);
    }
}

void woodland_xwayland_refresh_unmanaged_filters(struct woodland_server *server) {
    if (!server || !server->xwayland_unmanaged.next) {
        return;
    }
    struct woodland_unmanaged *u;
    wl_list_for_each(u, &server->xwayland_unmanaged, link) {
        unmanaged_update_filter_mode(u);
    }
}

void woodland_xwayland_update_cursor(struct woodland_server *server) {
    if (!server || !server->xwayland || !server->xcursor_manager) {
        return;
    }
    struct wlr_xcursor *xcursor =
        wlr_xcursor_manager_get_xcursor(server->xcursor_manager, "default", 1);
    if (!xcursor) {
        xcursor = wlr_xcursor_manager_get_xcursor(server->xcursor_manager, "left_ptr", 1);
    }
    if (xcursor && xcursor->images && xcursor->images[0]) {
        struct wlr_xcursor_image *image = xcursor->images[0];
        struct wlr_buffer *cursor_buffer = wlr_xcursor_image_get_buffer(image);
        if (cursor_buffer) {
            wlr_xwayland_set_cursor(server->xwayland, cursor_buffer,
                                    image->hotspot_x, image->hotspot_y);
        }
    }
}

static void spawn_script_async(const char *path) {
    wlr_log(WLR_INFO, "running xinitrc script: %s", path);
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "failed to fork for xinitrc");
        return;
    }
    if (pid == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        signal(SIGPIPE, SIG_DFL);
        setsid();

        pid_t grand_pid = fork();
        if (grand_pid < 0) {
            _exit(1);
        }
        if (grand_pid == 0) {
            execl("/bin/sh", "sh", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

static void run_xinitrc(void) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[1024];

    /* 1. $XDG_CONFIG_HOME/woodland/xinitrc or ~/.config/woodland/xinitrc */
    if (config_home && config_home[0]) {
        snprintf(path, sizeof(path), "%s/woodland/xinitrc", config_home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    } else if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/woodland/xinitrc", home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    }

    /* 2. $XDG_CONFIG_DIRS/woodland/xinitrc or /etc/xdg/woodland/xinitrc */
    const char *config_dirs = getenv("XDG_CONFIG_DIRS");
    if (!config_dirs || !config_dirs[0]) {
        config_dirs = "/etc/xdg";
    }
    char dirs_copy[1024];
    strncpy(dirs_copy, config_dirs, sizeof(dirs_copy) - 1);
    dirs_copy[sizeof(dirs_copy) - 1] = '\0';
    char *saveptr = NULL;
    char *dir = strtok_r(dirs_copy, ":", &saveptr);
    while (dir) {
        snprintf(path, sizeof(path), "%s/woodland/xinitrc", dir);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    /* 3. Fallback: labwc xinitrc */
    if (config_home && config_home[0]) {
        snprintf(path, sizeof(path), "%s/labwc/xinitrc", config_home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    } else if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/labwc/xinitrc", home);
        if (access(path, R_OK) == 0) {
            spawn_script_async(path);
            return;
        }
    }
}

/* ==================== 模块入口 ==================== */
static void xwl_ready(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, xwayland_ready);
    wlr_log(WLR_INFO, "xwayland ready on DISPLAY=%s",
            server->xwayland ? server->xwayland->display_name : "(null)");
    run_xinitrc();
    woodland_xwayland_update_cursor(server);
    (void)data;
}

bool woodland_xwayland_init(struct woodland_server *server) {
    wl_list_init(&server->xwayland_unmanaged);

    server->xwayland = wlr_xwayland_create(server->display, server->compositor,
                                           true);
    if (!server->xwayland) {
        wlr_log(WLR_ERROR, "failed to create xwayland");
        return false;
    }
    if (server->seat) {
        wlr_xwayland_set_seat(server->xwayland, server->seat);
    }

    server->xwayland_unmanaged_tree =
        wlr_scene_tree_create(&server->scene->tree);

    woodland_listener_connect(&server->new_xwayland_surface, &server->xwayland->events.new_surface,
                          xwl_new_surface, "xwayland.new_surface");

    woodland_listener_connect(&server->xwayland_ready, &server->xwayland->events.ready,
                          xwl_ready, "xwayland.ready");
    server->xwayland_cursor_hook = woodland_xwayland_update_cursor;
    server->xwayland_scale_filter_hook = woodland_xwayland_view_update_scale_filter;
    server->xwayland_refresh_hook = woodland_xwayland_refresh_unmanaged_filters;

    wlr_log(WLR_INFO, "xwayland initialised, DISPLAY=%s",
            server->xwayland->display_name);
    return true;
}

void woodland_xwayland_finish(struct woodland_server *server) {
    if (!server->xwayland) {
        return;
    }
    wl_list_remove(&server->new_xwayland_surface.link);
    wl_list_remove(&server->xwayland_ready.link);
    server->xwayland_cursor_hook = NULL;
    server->xwayland_scale_filter_hook = NULL;
    server->xwayland_refresh_hook = NULL;
    wlr_xwayland_destroy(server->xwayland);
    server->xwayland = NULL;
}
