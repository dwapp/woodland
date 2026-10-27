// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

#include "config.h"
#if WOODLAND_HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "capture.h"
#include "layer.h"
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "workspace.h"
#include "woodland.h"

/* ==================== 双后端访问器 ==================== */

struct wlr_surface *woodland_view_surface(const struct woodland_view *view) {
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

void woodland_view_content_box(const struct woodland_view *view,
                               struct wlr_box *box) {
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
    *box = view->xdg_toplevel->base->geometry;
}

void woodland_view_request_size(struct woodland_view *view, int width,
                                int height) {
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
        wlr_xwayland_surface_configure(xsurface,
            (int16_t)view->scene_tree->node.x,
            (int16_t)view->scene_tree->node.y,
            (uint16_t)width, (uint16_t)height);
        return;
    }
#endif
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
}

void woodland_view_set_activated(struct woodland_view *view, bool activated) {
    if (!view) {
        return;
    }
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_activate(view->xwayland_surface, activated);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
}

void woodland_view_close(struct woodland_view *view) {
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

struct woodland_view *woodland_view_from_surface(struct wlr_surface *surface) {
    if (!surface) {
        return NULL;
    }
    struct wlr_xdg_toplevel *toplevel =
        wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (toplevel) {
        struct wlr_scene_tree *tree = toplevel->base->data;
        return tree ? tree->node.data : NULL;
    }
#if WOODLAND_HAVE_XWAYLAND
    struct wlr_xwayland_surface *xsurface =
        wlr_xwayland_surface_try_from_wlr_surface(surface);
    return xsurface ? xsurface->data : NULL;
#else
    return NULL;
#endif
}

/* ==================== 几何纯函数 ==================== */

struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server) {
    struct wlr_box box = {0};

    /* 取输出 layout 中的第一个输出作为最大化目标（单输出场景与多输出首个一致） */
    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        /* usable_area 是输出局部坐标，须加 layout 偏移转成布局坐标 */
        wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
        if (wo->usable_area_valid) {
            box.x += wo->usable_area.x;
            box.y += wo->usable_area.y;
            box.width = wo->usable_area.width;
            box.height = wo->usable_area.height;
        }
        return box;
    }
    return box;
}

struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output) {
    (void)server;
    struct wlr_box box = {0};
    if (!output) {
        return box;
    }
    /* 全屏取输出完整 box，布局坐标 */
    wlr_output_layout_get_box(server->output_layout, output, &box);
    return box;
}

/*
 * 层叠放置：外框（内容 + SSD 边距）落进 area。
 *
 * 内容原点 = 外框原点 + 边距；外框宽度含左右边距、高度含标题栏，
 * 这样标题栏/边框不会被输出边缘裁掉（旧实现直接把内容放 (0,0)，
 * y=-26 的标题栏就跑到屏幕外了）。
 */
void woodland_view_placement(const struct wlr_box *area,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             int cascade, struct wlr_box *out) {
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;

    int x = area->x + cascade;
    int y = area->y + cascade;
    int max_x = area->x + area->width - outer_w;
    int max_y = area->y + area->height - outer_h;
    if (x > max_x) {
        x = max_x;
    }
    if (y > max_y) {
        y = max_y;
    }
    if (x < area->x) {
        x = area->x;
    }
    if (y < area->y) {
        y = area->y;
    }

    out->x = x + margins->left;
    out->y = y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

/*
 * 单轴贴边吸附：在候选线里挑位移最小的一个。
 *
 * 候选来源：工作区两端（起点贴 lo、末边贴 hi）+ 每条外部边（贴上去 / 贴回来）。
 * 位移超过 threshold 的一概不吸附——这样"吸附"只在贴近时生效，
 * 光标继续拉动超过阈值就自然脱离（labwc edge resistance 的简化版）。
 */
int woodland_snap_axis(int desired, int size, int lo, int hi,
                       const int *edges, int n_edges, int threshold) {
    /* 外框比工作区还大：没有可贴的意义，原样返回 */
    if (size > hi - lo) {
        return desired;
    }

    int best = desired;
    int best_delta = threshold + 1;

    /* 先试工作区两端，再试其它窗口的边（同距离时更偏向前者） */
    for (int i = 0; i < n_edges + 2; i++) {
        if (i > 1) {
            int edge = edges[i - 2];
            int d0 = edge - desired;
            int d1 = d0 - size;   /* 我们的末边贴到该边 */
            if (d0 < 0) {
                d0 = -d0;
            }
            if (d1 < 0) {
                d1 = -d1;
            }
            if (d0 <= threshold && d0 < best_delta) {
                best_delta = d0;
                best = edge;
            }
            if (d1 <= threshold && d1 < best_delta) {
                best_delta = d1;
                best = edge - size;
            }
            continue;
        }

        int cand = i == 0 ? lo : hi - size;
        int delta = cand - desired;
        if (delta < 0) {
            delta = -delta;
        }
        if (delta <= threshold && delta < best_delta) {
            best_delta = delta;
            best = cand;
        }
    }

    return best;
}

/* transient 相对父窗口居中（参考 labwc box_center：居中后退回 bound 内侧） */
void woodland_view_center_on(const struct wlr_box *parent_box,
                             int content_w, int content_h,
                             const struct woodland_ssd_margins *margins,
                             const struct wlr_box *area, struct wlr_box *out) {
    int outer_w = content_w + margins->left + margins->right;
    int outer_h = content_h + margins->top + margins->bottom;

    int x = parent_box->x + (parent_box->width - outer_w) / 2;
    int y = parent_box->y + (parent_box->height - outer_h) / 2;

    if (x < area->x) {
        x = area->x;
    } else if (x + outer_w > area->x + area->width) {
        x = area->x + area->width - outer_w;
    }
    if (y < area->y) {
        y = area->y;
    } else if (y + outer_h > area->y + area->height) {
        y = area->y + area->height - outer_h;
    }
    /* 子窗口比工作区还大：贴左上，不产生负向越界 */
    if (x < area->x) {
        x = area->x;
    }
    if (y < area->y) {
        y = area->y;
    }

    out->x = x + margins->left;
    out->y = y + margins->top;
    out->width = content_w;
    out->height = content_h;
}

/* ==================== view 聚焦与命中 ==================== */

void woodland_view_focus(struct woodland_view *view) {
    if (view == NULL) {
        return;
    }
    struct woodland_server *server = view->server;
    /* 只聚焦当前工作区、已 mapped 且未最小化的视图；
     * 覆盖重定向的 X11 窗口（菜单/提示）按 EWMH 不参与焦点管理 */
    if (!view->mapped || view->minimized || view->override_redirect ||
        view->workspace != server->active_workspace) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface) {
        return;
    }
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct woodland_view *prev_view = NULL;
    if (prev_surface == surface) {
        return;
    }
    /* view 得焦点即 layer surface 失焦点 */
    server->focused_layer = NULL;
    if (prev_surface) {
        prev_view = woodland_view_from_surface(prev_surface);
        if (prev_view) {
            woodland_view_set_activated(prev_view, false);
            if (prev_view->toplevel_handle) {
                wlr_foreign_toplevel_handle_v1_set_activated(
                    prev_view->toplevel_handle, false);
            }
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    woodland_view_set_activated(view, true);
    /* SSD 激活态同步：获焦视图激活色，失焦视图非激活色 */
    woodland_ssd_set_active(view, true);
    if (prev_view) {
        woodland_ssd_set_active(prev_view, false);
    }
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_activated(view->toplevel_handle, true);
    }
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

struct woodland_view *woodland_view_at(struct woodland_server *server,
                                       double lx, double ly,
                                       struct wlr_surface **surface,
                                       double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree->node.data;
}

/* ==================== 窗口放置 ==================== */

/* 首次 map：transient 相对父窗口居中，其余按 SSD 边距层叠放置 */
void woodland_view_place_initial(struct woodland_view *view) {
    struct woodland_server *server = view->server;
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);

    struct wlr_box area = woodland_view_maximized_geometry(server);
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    int content_w = content.width;
    int content_h = content.height;

    struct wlr_box geometry;
    if (view->parent && view->parent->mapped) {
        /* 对话框/工具窗：以父窗口内容框为中心（越界时贴回工作区） */
        struct wlr_box parent_content;
        woodland_view_content_box(view->parent, &parent_content);
        struct wlr_box parent_box = {
            .x = view->parent->scene_tree->node.x,
            .y = view->parent->scene_tree->node.y,
            .width = parent_content.width,
            .height = parent_content.height,
        };
        woodland_view_center_on(&parent_box, content_w, content_h, &margins,
                                &area, &geometry);
    } else {
        /* 层叠步进：第 n 个窗口偏移 n%8 格，避免新窗口完全盖住上一个。
         * 统计时排除自己：X11 的首次放置发生在 map_request（尚未进链表），
         * 而 xdg 发生在 map（已在链表里），两者的"序号"语义要一致。 */
        int count = 0;
        struct woodland_view *it;
        wl_list_for_each(it, &server->views, link) {
            if (it != view) {
                count++;
            }
        }
        int cascade = (count % 8) * 32;
        woodland_view_placement(&area, content_w, content_h, &margins,
                                cascade, &geometry);
    }
    wlr_scene_node_set_position(&view->scene_tree->node, geometry.x, geometry.y);
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
view_decoration_mode(const struct woodland_view *view) {
    return view->deco_csd
        ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

void woodland_view_map(struct woodland_view *view) {
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
    wlr_scene_node_reparent(&view->scene_tree->node,
                            view->workspace->tree);
    wl_list_insert(&server->views, &view->link);
    /* 首次 map 时创建 SSD（未 map 前不显示空装饰，同 labwc handle_map） */
    if (!view->ssd && !view->deco_csd) {
        woodland_ssd_create(view);
    }
    /* 只在首次 map 放置；remap 保留用户摆好的位置 */
    if (!view->placed) {
        view->placed = true;
        woodland_view_place_initial(view);
    }
    woodland_view_focus(view);
}

void woodland_view_unmap(struct woodland_view *view) {
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
    /* 只在确实在链表里时摘除：X11 窗口可能没 map 过就 unmap */
    if (view->mapped) {
        wl_list_remove(&view->link);
        view->mapped = false;
    }
    /* unmap 时销毁 SSD（避免残留空装饰）；remap 时按 deco_csd 重建 */
    if (view->ssd) {
        woodland_ssd_destroy(view);
    }
}

void woodland_view_destroy(struct woodland_view *view) {
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
    free(view->title);
    free(view->app_id);
    free(view);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, map);
    woodland_view_map(view);
    (void)data;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, unmap);
    woodland_view_unmap(view);
    (void)data;
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, commit);
    struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
    if (xdg->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
        /* initial commit：此时 surface 已 initialized，可以 set_mode 完成 zxdg 协商 */
        if (view->deco) {
            wlr_xdg_toplevel_decoration_v1_set_mode(view->deco,
                view_decoration_mode(view));
        }
    }
    /* 内容尺寸/状态变化后同步 SSD 几何（标题栏/边框/按钮） */
    woodland_ssd_update(view);
    (void)data;
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
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

/* ==================== 交互式 move/resize ==================== */

void woodland_view_begin_interactive(struct woodland_view *view,
                                     enum woodland_cursor_mode mode,
                                     uint32_t edges) {
    struct woodland_server *server = view->server;
    server->grabbed_view = view;
    server->cursor_mode = mode;

    if (mode == WOODLAND_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - view->scene_tree->node.x;
        server->grab_y = server->cursor->y - view->scene_tree->node.y;
    } else {
        struct wlr_box geo_box;
        woodland_view_content_box(view, &geo_box);
        double border_x = (view->scene_tree->node.x + geo_box.x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (view->scene_tree->node.y + geo_box.y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += view->scene_tree->node.x;
        server->grab_geobox.y += view->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, request_move);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_MOVE, 0);
    (void)data;
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct woodland_view *view = wl_container_of(listener, view, request_resize);
    woodland_view_begin_interactive(view, WOODLAND_CURSOR_RESIZE, event->edges);
}

/* ==================== 窗口状态处理 ==================== */

static void view_save_geometry(struct woodland_view *view) {
    struct wlr_box content;
    woodland_view_content_box(view, &content);
    view->saved_geometry.x = view->scene_tree->node.x;
    view->saved_geometry.y = view->scene_tree->node.y;
    view->saved_geometry.width = content.width;
    view->saved_geometry.height = content.height;
}

static void view_apply_geometry(struct woodland_view *view,
                                const struct wlr_box *box) {
    /* 先落位置再改尺寸：X11 的 configure 会把当前位置一并发给客户端 */
    wlr_scene_node_set_position(&view->scene_tree->node, box->x, box->y);
    woodland_view_request_size(view, box->width, box->height);
}

/*
 * 按"外框"应用几何：内容 = 外框减去 SSD 边距。
 *
 * 最大化/全屏给的是整块区域，装饰（标题栏）占外框顶部，内容必须让位，
 * 否则标题栏会被推到屏幕/输出边界外面。
 */
static void view_apply_outer_geometry(struct woodland_view *view,
                                      struct wlr_box *box) {
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);
    int content_w = box->width - margins.left - margins.right;
    int content_h = box->height - margins.top - margins.bottom;
    if (content_w < 1) {
        content_w = 1;
    }
    if (content_h < 1) {
        content_h = 1;
    }
    wlr_scene_node_set_position(&view->scene_tree->node,
        box->x + margins.left, box->y + margins.top);
    woodland_view_request_size(view, content_w, content_h);
}

/* ---- 状态标志的协议侧同步（xdg 走 configure，X11 走 _NET_WM_STATE） ---- */

static void view_apply_maximized_state(struct woodland_view *view,
                                       bool maximized) {
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_maximized(view->xwayland_surface,
                                               maximized, maximized);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
}

static void view_apply_fullscreen_state(struct woodland_view *view,
                                        bool fullscreen) {
#if WOODLAND_HAVE_XWAYLAND
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        if (view->xwayland_surface) {
            wlr_xwayland_surface_set_fullscreen(view->xwayland_surface,
                                                fullscreen);
        }
        return;
    }
#endif
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
}

/* X11 的 configure 已由 woodland_view_request_size 直接发出，无需再排程 */
static void view_schedule_configure(struct woodland_view *view) {
    if (view->type == WOODLAND_VIEW_XWAYLAND) {
        return;
    }
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

void woodland_view_set_maximized(struct woodland_view *view, bool maximized) {
    if (maximized == view->maximized) {
        return;
    }
    struct woodland_server *server = view->server;
    if (maximized) {
        /* 先退出 fullscreen（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->fullscreen) {
            view->fullscreen = false;
            view_apply_geometry(view, &view->saved_geometry);
            view_apply_fullscreen_state(view, false);
        }
        view_save_geometry(view);
        view->maximized = true;
        struct wlr_box box = woodland_view_maximized_geometry(server);
        /* 最大化保留标题栏，内容让出 titlebar 高度 */
        view_apply_outer_geometry(view, &box);
        view_apply_maximized_state(view, true);
    } else {
        view->maximized = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        view_apply_maximized_state(view, false);
    }
    view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
    }
}

void woodland_view_set_fullscreen(struct woodland_view *view, bool fullscreen,
                                  struct wlr_output *output) {
    if (fullscreen == view->fullscreen) {
        return;
    }
    struct woodland_server *server = view->server;
    if (fullscreen) {
        /* 先退出 maximize（恢复其前的几何并落标志），再保存非 max/fullscreen 几何 */
        if (view->maximized) {
            view->maximized = false;
            view_apply_geometry(view, &view->saved_geometry);
            view_apply_maximized_state(view, false);
        }
        view_save_geometry(view);
        view->fullscreen = true;
        if (!output) {
            /* 无指定输出时取第一个输出 */
            struct woodland_output *wo;
            wl_list_for_each(wo, &server->outputs, link) {
                output = wo->wlr_output;
                break;
            }
        }
        struct wlr_box box = woodland_view_fullscreen_geometry(server, output);
        view_apply_outer_geometry(view, &box);
        view_apply_fullscreen_state(view, true);
    } else {
        view->fullscreen = false;
        struct wlr_box box = view->saved_geometry;
        view_apply_geometry(view, &box);
        view_apply_fullscreen_state(view, false);
    }
    view_schedule_configure(view);
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(
            view->toplevel_handle, view->fullscreen);
        wlr_foreign_toplevel_handle_v1_set_maximized(
            view->toplevel_handle, view->maximized);
    }
}

void woodland_view_set_minimized(struct woodland_view *view, bool minimized) {
    if (minimized == view->minimized) {
        return;
    }
    view->minimized = minimized;
    wlr_scene_node_set_enabled(&view->scene_tree->node, !minimized);
    if (minimized) {
        /* 最小化后若它是焦点视图则清键盘焦点并同步 deactivated */
        struct wlr_surface *focused =
            view->server->seat->keyboard_state.focused_surface;
        if (focused != NULL && focused == woodland_view_surface(view)) {
            wlr_seat_keyboard_notify_clear_focus(view->server->seat);
            woodland_view_set_activated(view, false);
            if (view->toplevel_handle) {
                wlr_foreign_toplevel_handle_v1_set_activated(
                    view->toplevel_handle, false);
            }
        }
    }
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_set_minimized(
            view->toplevel_handle, minimized);
    }
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_maximize);
    woodland_view_set_maximized(view, view->xdg_toplevel->requested.maximized);
    (void)data;
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_fullscreen);
    woodland_view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen,
                                 view->xdg_toplevel->requested.fullscreen_output);
    (void)data;
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, request_minimize);
    woodland_view_set_minimized(view, view->xdg_toplevel->requested.minimized);
    (void)data;
}

/* 客户端/合成器侧销毁句柄时清指针，避免视图销毁时二次释放 */
static void ext_toplevel_handle_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view =
        wl_container_of(listener, view, ext_handle_destroy);
    wl_list_remove(&view->ext_handle_destroy.link);
    view->ext_toplevel_handle = NULL;
    (void)data;
}

/* ext-foreign-toplevel-list 的句柄状态：只有 title/app_id 两项 */
static void ext_toplevel_sync_state(struct woodland_view *view) {
    if (!view->ext_toplevel_handle) {
        return;
    }
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = view->title,
        .app_id = view->app_id,
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(
        view->ext_toplevel_handle, &state);
}

void woodland_view_set_title(struct woodland_view *view, const char *title) {
    char *copy = title ? strdup(title) : NULL;
    if (title && !copy) {
        return;
    }
    free(view->title);
    view->title = copy;
    /* wlroots 的 set_title 不接受 NULL（内部 strdup），空标题就不下发 */
    if (view->toplevel_handle && view->title) {
        wlr_foreign_toplevel_handle_v1_set_title(view->toplevel_handle,
                                                 view->title);
    }
    ext_toplevel_sync_state(view);
    /* 标题变化时刷新 SSD 标题文本 */
    if (view->ssd) {
        woodland_ssd_update(view);
    }
}

void woodland_view_set_app_id(struct woodland_view *view, const char *app_id) {
    char *copy = app_id ? strdup(app_id) : NULL;
    if (app_id && !copy) {
        return;
    }
    free(view->app_id);
    view->app_id = copy;
    if (view->toplevel_handle && view->app_id) {
        wlr_foreign_toplevel_handle_v1_set_app_id(view->toplevel_handle,
                                                  view->app_id);
    }
    ext_toplevel_sync_state(view);
}

void woodland_view_set_parent(struct woodland_view *view,
                              struct woodland_view *parent) {
    view->parent = parent;
    /* 同步 foreign-toplevel parent */
    if (view->toplevel_handle) {
        struct wlr_foreign_toplevel_handle_v1 *parent_handle =
            (parent && parent->toplevel_handle)
                ? parent->toplevel_handle : NULL;
        wlr_foreign_toplevel_handle_v1_set_parent(
            view->toplevel_handle, parent_handle);
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_title);
    woodland_view_set_title(view, view->xdg_toplevel->title);
    (void)data;
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_app_id);
    woodland_view_set_app_id(view, view->xdg_toplevel->app_id);
    (void)data;
}

static void xdg_toplevel_set_parent(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, set_parent);
    struct wlr_xdg_toplevel *parent = view->xdg_toplevel->parent;
    struct woodland_view *parent_view = NULL;
    if (parent && parent->base->data) {
        struct wlr_scene_tree *parent_tree = parent->base->data;
        parent_view = parent_tree->node.data;
    }
    woodland_view_set_parent(view, parent_view);
    (void)data;
}

/* ==================== foreign-toplevel 请求 ==================== */

static void ft_request_activate(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_activate);
    /* 若最小化则先恢复 */
    if (view->minimized) {
        woodland_view_set_minimized(view, false);
    }
    woodland_view_focus(view);
    (void)data;
}

static void ft_request_maximize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    woodland_view_set_maximized(view, event->maximized);
}

static void ft_request_fullscreen(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    woodland_view_set_fullscreen(view, event->fullscreen, event->output);
}

static void ft_request_minimize(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    woodland_view_set_minimized(view, event->minimized);
}

static void ft_request_close(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, ft_request_close);
    woodland_view_close(view);
    (void)data;
}

/* ==================== 交互式 motion 处理 ==================== */

void woodland_cursor_reset_mode(struct woodland_server *server) {
    server->cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
}

/* ==================== SSD 按钮 hover/pressed ==================== */

/*
 * 更新悬停的按钮。
 *
 * 指针每帧只会悬停在一个 SSD 部件上，所以这里维护"唯一的 hover 目标"：
 * 换了视图先清旧的，避免两个窗口同时亮着。
 */
static void ssd_update_hover(struct woodland_server *server,
                             struct woodland_view *view,
                             enum woodland_ssd_part part) {
    if (server->hover_view == view && server->hover_part == part) {
        return;
    }
    if (server->hover_view && server->hover_view != view) {
        woodland_ssd_set_hover(server->hover_view, WOODLAND_SSD_NONE);
    }
    server->hover_view = view;
    server->hover_part = part;
    if (view) {
        woodland_ssd_set_hover(view, part);
    }
}

/* 清掉按下态（不执行动作） */
static void ssd_clear_pressed(struct woodland_server *server) {
    if (server->pressed_view) {
        woodland_ssd_set_pressed(server->pressed_view, WOODLAND_SSD_NONE);
    }
    server->pressed_view = NULL;
    server->pressed_part = WOODLAND_SSD_NONE;
}

/* 吸附候选边上限：每个视图贡献 2 条边（左右或上下） */
#define WOODLAND_SNAP_MAX_EDGES 64

/*
 * 收集吸附候选线（纯数据，交给 woodland_snap_axis 决策）。
 *
 * x_edges=true 收集竖直边（外框左右边），此时只考虑纵向范围与
 * [span_start, span_start+span_len) 有交叠的视图——否则隔着半屏的窗口
 * 也会把我们的窗口吸过去。
 */
static int collect_snap_edges(struct woodland_server *server,
                              struct woodland_view *exclude, bool x_edges,
                              int span_start, int span_len,
                              int *edges, int max_edges) {
    int n = 0;
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link) {
        if (it == exclude || !it->mapped || it->minimized || it->fullscreen
            || it->workspace != exclude->workspace) {
            continue;
        }
        struct woodland_ssd_margins m;
        woodland_ssd_get_margins(it, &m);
        struct wlr_box content;
        woodland_view_content_box(it, &content);
        struct wlr_box box = {
            .x = it->scene_tree->node.x - m.left,
            .y = it->scene_tree->node.y - m.top,
            .width = content.width + m.left + m.right,
            .height = content.height + m.top + m.bottom,
        };
        int other_start = x_edges ? box.y : box.x;
        int other_len = x_edges ? box.height : box.width;
        if (other_start + other_len <= span_start
            || span_start + span_len <= other_start) {
            continue;
        }
        if (n + 2 > max_edges) {
            break;
        }
        edges[n++] = x_edges ? box.x : box.y;
        edges[n++] = x_edges ? box.x + box.width : box.y + box.height;
    }
    return n;
}

/*
 * 拖动移动：外框跟随光标，靠近工作区边缘或其它窗口边缘时吸附。
 *
 * 先按 x 吸附（用当前的纵向跨度判断邻窗是否并排），再按吸附后的 x
 * 算 y，避免两次吸附互相打脸。
 */
static void process_cursor_move(struct woodland_server *server) {
    struct woodland_view *view = server->grabbed_view;
    struct woodland_ssd_margins margins;
    woodland_ssd_get_margins(view, &margins);
    struct wlr_box geo;
    woodland_view_content_box(view, &geo);

    /* grab_x/y 是光标相对内容原点的偏移，换算成外框原点 */
    int x = (int)(server->cursor->x - server->grab_x) - margins.left;
    int y = (int)(server->cursor->y - server->grab_y) - margins.top;
    int outer_w = geo.width + margins.left + margins.right;
    int outer_h = geo.height + margins.top + margins.bottom;

    if (!view->maximized && !view->fullscreen) {
        struct wlr_box area = woodland_view_maximized_geometry(server);
        int edges[WOODLAND_SNAP_MAX_EDGES];

        int n = collect_snap_edges(server, view, true, y, outer_h,
                                   edges, WOODLAND_SNAP_MAX_EDGES);
        x = woodland_snap_axis(x, outer_w, area.x, area.x + area.width,
                               edges, n, WOODLAND_SNAP_DISTANCE);

        n = collect_snap_edges(server, view, false, x, outer_w,
                               edges, WOODLAND_SNAP_MAX_EDGES);
        y = woodland_snap_axis(y, outer_h, area.y, area.y + area.height,
                               edges, n, WOODLAND_SNAP_DISTANCE);
    }

    wlr_scene_node_set_position(&view->scene_tree->node,
        x + margins.left, y + margins.top);
}

static void process_cursor_resize(struct woodland_server *server) {
    struct woodland_view *view = server->grabbed_view;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box geo_box;
    woodland_view_content_box(view, &geo_box);
    wlr_scene_node_set_position(&view->scene_tree->node,
        new_left - geo_box.x, new_top - geo_box.y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    woodland_view_request_size(view, new_width, new_height);
}

void woodland_cursor_process_motion(struct woodland_server *server, uint32_t time) {
    if (server->cursor_mode == WOODLAND_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == WOODLAND_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct woodland_view *view = woodland_view_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    /* 未命中 surface 时，检查是否悬停在某视图的 SSD 区域（标题栏/边框） */
    enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
    uint32_t edges = 0;
    struct woodland_view *ssd_view = NULL;
    if (!view) {
        struct woodland_view *v;
        wl_list_for_each(v, &server->views, link) {
            enum woodland_ssd_part part = woodland_ssd_at(v,
                server->cursor->x, server->cursor->y, &edges);
            if (part != WOODLAND_SSD_NONE) {
                ssd_part = part;
                ssd_view = v;
                break;
            }
        }
    }

    if (ssd_view) {
        /* 悬停 SSD：按区域切换光标，不给 surface 指针焦点 */
        const char *cursor_name = "default";
        if (ssd_part == WOODLAND_SSD_TITLEBAR) {
            cursor_name = "default";
        } else if (ssd_part >= WOODLAND_SSD_BORDER_LEFT
                && ssd_part <= WOODLAND_SSD_BORDER_BOTTOM) {
            cursor_name = "se-resize";
        }
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
            cursor_name);
        wlr_seat_pointer_clear_focus(seat);
        /* 只有按钮才亮；标题栏/边框传 NONE（顺带清掉上一个按钮的高亮） */
        bool on_button = ssd_part == WOODLAND_SSD_BUTTON_CLOSE
            || ssd_part == WOODLAND_SSD_BUTTON_MAXIMIZE
            || ssd_part == WOODLAND_SSD_BUTTON_MINIMIZE;
        ssd_update_hover(server, on_button ? ssd_view : NULL,
                         on_button ? ssd_part : WOODLAND_SSD_NONE);
        return;
    }

    /* 离开 SSD 区域（含移到客户端内容上）：清掉按钮高亮 */
    ssd_update_hover(server, NULL, WOODLAND_SSD_NONE);

    if (!view) {
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

/* ==================== new_toplevel ==================== */

/* foreign-toplevel 与 ext-foreign-toplevel-list 句柄：两个后端共用 */
void woodland_view_init_handles(struct woodland_view *view) {
    struct woodland_server *server = view->server;

    if (server->foreign_toplevel_manager) {
        view->toplevel_handle = wlr_foreign_toplevel_handle_v1_create(
            server->foreign_toplevel_manager);
        if (view->toplevel_handle) {
            view->toplevel_handle->data = view;
            view->ft_request_activate.notify = ft_request_activate;
            wl_signal_add(&view->toplevel_handle->events.request_activate,
                          &view->ft_request_activate);
            view->ft_request_maximize.notify = ft_request_maximize;
            wl_signal_add(&view->toplevel_handle->events.request_maximize,
                          &view->ft_request_maximize);
            view->ft_request_fullscreen.notify = ft_request_fullscreen;
            wl_signal_add(&view->toplevel_handle->events.request_fullscreen,
                          &view->ft_request_fullscreen);
            view->ft_request_minimize.notify = ft_request_minimize;
            wl_signal_add(&view->toplevel_handle->events.request_minimize,
                          &view->ft_request_minimize);
            view->ft_request_close.notify = ft_request_close;
            wl_signal_add(&view->toplevel_handle->events.request_close,
                          &view->ft_request_close);
        }
    }

    /* ext-foreign-toplevel-list-v1 句柄：截图/任务栏类客户端（lswt、grim -T）用。
     * handle->data 指回 view，capture 模块据它建「按窗口取源」。 */
    if (server->ext_toplevel_list) {
        struct wlr_ext_foreign_toplevel_handle_v1_state state = {
            .title = view->title,
            .app_id = view->app_id,
        };
        view->ext_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
            server->ext_toplevel_list, &state);
        if (view->ext_toplevel_handle) {
            view->ext_toplevel_handle->data = view;
            view->ext_handle_destroy.notify = ext_toplevel_handle_destroy;
            wl_signal_add(&view->ext_toplevel_handle->events.destroy,
                          &view->ext_handle_destroy);
        }
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct woodland_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "failed to allocate woodland_view");
        return;
    }
    view->server = server;
    view->type = WOODLAND_VIEW_XDG;
    view->xdg_toplevel = xdg_toplevel;
    view->scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
    view->scene_tree->node.data = view;
    xdg_toplevel->base->data = view->scene_tree;

    view->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);

    view->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);

    view->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
    view->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
    view->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
    view->request_minimize.notify = xdg_toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);
    view->set_title.notify = xdg_toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
    view->set_app_id.notify = xdg_toplevel_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);
    view->set_parent.notify = xdg_toplevel_set_parent;
    wl_signal_add(&xdg_toplevel->events.set_parent, &view->set_parent);

    woodland_view_init_handles(view);
}

/* ==================== popup ==================== */

struct woodland_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct woodland_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
    (void)data;
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct woodland_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    (void)data;
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
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

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (parent == NULL) {
        free(popup);
        return;
    }
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ==================== zxdg_decoration ==================== */

/* 应用客户端请求的装饰模式（不直接 set_mode，避免未 initialized 时 assert） */
static void deco_apply_mode(struct woodland_view *view,
                            enum wlr_xdg_toplevel_decoration_v1_mode requested) {
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
static void deco_request_mode(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, deco_request_mode);
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;

    deco_apply_mode(view, deco->requested_mode);

    /* 仅在 surface 已 initialized 时才能 schedule configure；回给客户端的模式
     * 必须是"我们实际会采用的模式"，否则客户端会重复画装饰 */
    if (deco->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco,
            view_decoration_mode(view));
    }
}

static void deco_destroy(struct wl_listener *listener, void *data) {
    struct woodland_view *view = wl_container_of(listener, view, deco_destroy);
    if (view->deco_registered) {
        wl_list_remove(&view->deco_destroy.link);
        wl_list_remove(&view->deco_request_mode.link);
        view->deco_registered = false;
    }
    view->deco = NULL;
    (void)data;
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_toplevel_decoration);
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
    view->deco_request_mode.notify = deco_request_mode;
    wl_signal_add(&deco->events.request_mode, &view->deco_request_mode);
    view->deco_destroy.notify = deco_destroy;
    wl_signal_add(&deco->events.destroy, &view->deco_destroy);
    view->deco_registered = true;

    /* 立即按客户端当前请求模式调整（未 initialized 时 set_mode 延迟到 initial commit） */
    deco_apply_mode(view, deco->requested_mode);
    if (xdg_surface->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(deco,
            view_decoration_mode(view));
    }
}

/* ==================== init/finish ==================== */

bool woodland_shell_init(struct woodland_server *server) {
    wl_list_init(&server->views);

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        return false;
    }

    server->foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(server->display);

    /* ext-foreign-toplevel-list-v1：只读的窗口列表，配合
     * ext-foreign-toplevel-image-capture-source-v1 做按窗口截图 */
    server->ext_toplevel_list =
        wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);

    server->xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server->display);

    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
    if (server->xdg_decoration_manager) {
        server->new_toplevel_decoration.notify = server_new_toplevel_decoration;
        wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration,
                      &server->new_toplevel_decoration);
    }

    return true;
}

void woodland_shell_finish(struct woodland_server *server) {
    if (server->xdg_shell) {
        wl_list_remove(&server->new_xdg_toplevel.link);
        wl_list_remove(&server->new_xdg_popup.link);
    }
    if (server->xdg_decoration_manager) {
        wl_list_remove(&server->new_toplevel_decoration.link);
    }
}

/* ==================== 光标事件处理 ==================== */

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    woodland_cursor_process_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
        event->x, event->y);
    woodland_cursor_process_motion(server, event->time_msec);
}

/*
 * 释放 SSD 按钮：只有指针仍停在按下的那个按钮上才执行动作。
 *
 * 动作放在释放而不是按下，是为了让"按下态"有意义（按住时能看到凹陷），
 * 同时支持按住后拖离按钮取消（与常见工具包的行为一致）。
 */
static void ssd_button_release(struct woodland_server *server) {
    struct woodland_view *view = server->pressed_view;
    enum woodland_ssd_part part = server->pressed_part;
    ssd_clear_pressed(server);

    if (!view || !view->mapped) {
        return;
    }
    uint32_t edges = 0;
    if (woodland_ssd_at(view, server->cursor->x, server->cursor->y, &edges)
            != part) {
        return;
    }

    switch (part) {
    case WOODLAND_SSD_BUTTON_CLOSE:
        woodland_view_close(view);
        break;
    case WOODLAND_SSD_BUTTON_MAXIMIZE:
        woodland_view_set_maximized(view, !view->maximized);
        break;
    case WOODLAND_SSD_BUTTON_MINIMIZE:
        woodland_view_set_minimized(view, true);
        break;
    default:
        break;
    }
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        woodland_cursor_reset_mode(server);
        ssd_button_release(server);
    } else {
        /* 新的按下：先清掉上一次遗留的按下态 */
        ssd_clear_pressed(server);
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct woodland_view *view = woodland_view_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        if (view) {
            woodland_view_focus(view);
        } else {
            /* 未命中 surface：先查 SSD 区域（按钮/标题栏/边框） */
            uint32_t edges = 0;
            struct woodland_view *ssd_view = NULL;
            enum woodland_ssd_part ssd_part = WOODLAND_SSD_NONE;
            struct woodland_view *v;
            wl_list_for_each(v, &server->views, link) {
                enum woodland_ssd_part part = woodland_ssd_at(v,
                    server->cursor->x, server->cursor->y, &edges);
                if (part != WOODLAND_SSD_NONE) {
                    ssd_part = part;
                    ssd_view = v;
                    break;
                }
            }
            if (ssd_view) {
                woodland_view_focus(ssd_view);
                switch (ssd_part) {
                case WOODLAND_SSD_BUTTON_CLOSE:
                case WOODLAND_SSD_BUTTON_MAXIMIZE:
                case WOODLAND_SSD_BUTTON_MINIMIZE:
                    /* 记下按下目标，释放时再执行（见 ssd_button_release） */
                    server->pressed_view = ssd_view;
                    server->pressed_part = ssd_part;
                    woodland_ssd_set_pressed(ssd_view, ssd_part);
                    break;
                case WOODLAND_SSD_TITLEBAR:
                    woodland_view_begin_interactive(ssd_view, WOODLAND_CURSOR_MOVE, 0);
                    break;
                case WOODLAND_SSD_BORDER_LEFT:
                case WOODLAND_SSD_BORDER_RIGHT:
                case WOODLAND_SSD_BORDER_TOP:
                case WOODLAND_SSD_BORDER_BOTTOM:
                    woodland_view_begin_interactive(ssd_view, WOODLAND_CURSOR_RESIZE, edges);
                    break;
                default:
                    break;
                }
            } else {
                /* 点中 layer surface 时按 interactivity 给焦点 */
                struct wlr_surface *lsurf = NULL;
                double lsx, lsy;
                struct wlr_layer_surface_v1 *layer = woodland_layer_surface_at(
                    server, server->cursor->x, server->cursor->y,
                    &lsurf, &lsx, &lsy);
                if (layer) {
                    woodland_layer_try_focus(server, layer);
                }
            }
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
    (void)data;
}

void woodland_cursor_init_events(struct woodland_server *server) {
    server->cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
        &server->cursor_motion_absolute);
    server->cursor_button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
}

void woodland_cursor_finish_events(struct woodland_server *server) {
    if (server->cursor) {
        wl_list_remove(&server->cursor_motion.link);
        wl_list_remove(&server->cursor_motion_absolute.link);
        wl_list_remove(&server->cursor_button.link);
        wl_list_remove(&server->cursor_axis.link);
        wl_list_remove(&server->cursor_frame.link);
    }
}
