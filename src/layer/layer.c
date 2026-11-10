// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include "scene.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "effect.h"
#include "layer.h"
#include "output.h"
#include "session_lock.h"
#include "view.h"
#include "woodland.h"

#define WOODLAND_LAYER_SHELL_VERSION 4

/* ==================== arrange ==================== */

static void arrange_one_layer(const struct wlr_box *full_area, struct wlr_box *usable_area,
                              struct wlr_scene_tree *tree, bool exclusive)
{
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link)
    {
        struct woodland_layer_surface *surface = node->data;
        struct wlr_scene_layer_surface_v1 *scene = surface->scene_layer_surface;
        if (!scene->layer_surface->initialized) {
            continue;
        }
        if (!!scene->layer_surface->current.exclusive_zone != exclusive) {
            continue;
        }
        wlr_scene_layer_surface_v1_configure(scene, full_area, usable_area);
    }
}

void woodland_layer_arrange(struct woodland_server *server, struct woodland_output *output)
{
    (void)server;
    struct wlr_box full_area = {0};
    wlr_output_effective_resolution(output->wlr_output, &full_area.width, &full_area.height);
    struct wlr_box usable_area = full_area;

    /* 先 exclusive（overlay → background），再非 exclusive（background → overlay） */
    for (int i = 3; i >= 0; i--) {
        arrange_one_layer(&full_area, &usable_area, output->layer_tree[i], true);
    }
    for (int i = 0; i < 4; i++) {
        arrange_one_layer(&full_area, &usable_area, output->layer_tree[i], false);
    }

    output->usable_area = usable_area;
    output->usable_area_valid = true;

    /* 同步 layer tree 节点位置到输出在 layout 中的坐标 */
    for (int i = 0; i < 4; i++) {
        wlr_scene_node_set_position(&output->layer_tree[i]->node, output->scene_output->x,
                                    output->scene_output->y);
    }
}

void woodland_layer_update_top_layer_visibility(struct woodland_server *server)
{
    if (!server || !server->outputs.next) {
        return;
    }

    uint32_t top = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    struct woodland_output *output;

    /* 先启用所有有效输出的 TOP layer */
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->wlr_output || !output->wlr_output->enabled || !output->layer_tree[top]) {
            continue;
        }
        wlr_scene_node_set_enabled(&output->layer_tree[top]->node, true);
    }

    if (!server->views.next) {
        return;
    }

    /*
     * 沿 Z 序链表从顶到底遍历（server->views 表头为最顶层）：
     * 若遇到当前工作区的可见全屏窗口，且该输出在更上层未被其他窗口覆盖，
     * 则将该输出对应的 TOP layer 隐藏（对齐 labwc desktop_update_top_layer_visibility）。
     */
    size_t num_outputs = wl_list_length(&server->outputs);
    if (num_outputs == 0) {
        return;
    }

    struct woodland_output **covered = calloc(num_outputs, sizeof(*covered));
    size_t num_covered = 0;

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped || view->minimized || view->override_redirect) {
            continue;
        }
        if (server->active_workspace && view->workspace != server->active_workspace) {
            continue;
        }

        wl_list_for_each(output, &server->outputs, link) {
            if (!output->wlr_output || !output->wlr_output->enabled) {
                continue;
            }
            if (!woodland_view_on_output(view, output->wlr_output)) {
                continue;
            }

            bool already_covered = false;
            for (size_t i = 0; i < num_covered; i++) {
                if (covered[i] == output) {
                    already_covered = true;
                    break;
                }
            }

            if (view->fullscreen && !already_covered) {
                if (output->layer_tree[top]) {
                    wlr_scene_node_set_enabled(&output->layer_tree[top]->node, false);
                }
            }

            if (!already_covered && num_covered < num_outputs) {
                covered[num_covered++] = output;
            }
        }
    }
    free(covered);
}

/* ==================== 焦点（任务 3 完整实现，本任务先提供供 commit 调用） ==================== */

static bool focused_layer_has_exclusive_interactivity(struct woodland_server *server)
{
    if (!server || !server->focused_layer) {
        return false;
    }
    return server->focused_layer->current.keyboard_interactive ==
           ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

static bool has_precedence(struct woodland_server *server,
                           enum zwlr_layer_shell_v1_layer layer)
{
    if (!server || !server->focused_layer) {
        return true;
    }
    if (!focused_layer_has_exclusive_interactivity(server)) {
        return true;
    }
    if (layer >= server->focused_layer->current.layer) {
        return true;
    }
    return false;
}

static void seat_set_focus_layer(struct woodland_server *server,
                                 struct wlr_layer_surface_v1 *layer_surface)
{
    if (!layer_surface) {
        server->focused_layer = NULL;
        woodland_focus_fallback(server);
        return;
    }
    server->focused_layer = layer_surface;
    woodland_focus_yield_to_layer(server);
    woodland_seat_focus_surface(server, layer_surface->surface);
}

static void try_to_focus_next_layer_or_toplevel(struct woodland_server *server)
{
    if (!server || woodland_session_lock_is_locked(server)) {
        return;
    }

    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->wlr_output || !output->wlr_output->enabled) {
            continue;
        }
        for (int i = (int)ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
             i >= (int)ZWLR_LAYER_SHELL_V1_LAYER_TOP; i--) {
            struct wlr_scene_tree *tree = output->layer_tree[i];
            if (!tree) {
                continue;
            }
            struct wlr_scene_node *node;
            wl_list_for_each_reverse(node, &tree->children, link) {
                struct woodland_layer_surface *layer = node->data;
                if (!layer || !layer->scene_layer_surface) {
                    continue;
                }
                struct wlr_layer_surface_v1 *ls = layer->scene_layer_surface->layer_surface;
                if (!ls || !ls->surface || !ls->surface->mapped) {
                    continue;
                }
                if (ls->current.keyboard_interactive ==
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                    seat_set_focus_layer(server, ls);
                    return;
                }
            }
        }
    }

    if (server->focused_layer) {
        seat_set_focus_layer(server, NULL);
    } else {
        woodland_focus_fallback(server);
    }
}

void woodland_layer_try_focus(struct woodland_server *server,
                              struct wlr_layer_surface_v1 *layer_surface)
{
    if (!server || woodland_session_lock_is_locked(server)) {
        return;
    }

    enum zwlr_layer_surface_v1_keyboard_interactivity interactivity =
        layer_surface->current.keyboard_interactive;

    switch (interactivity) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
        if (has_precedence(server, layer_surface->current.layer)) {
            seat_set_focus_layer(server, layer_surface);
        }
        break;
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND:
        if (!focused_layer_has_exclusive_interactivity(server)) {
            seat_set_focus_layer(server, layer_surface);
        }
        break;
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE:
        if (server->focused_layer == layer_surface) {
            try_to_focus_next_layer_or_toplevel(server);
        }
        break;
    }
}

struct wlr_layer_surface_v1 *woodland_layer_surface_at(struct woodland_server *server, double lx,
                                                       double ly, struct wlr_surface **surface,
                                                       double *sx, double *sy)
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
    return wlr_layer_surface_v1_try_from_wlr_surface(scene_surface->surface);
}

/*
 * surface → layer surface 对象。背景模糊要按 surface 找"该把 blur 节点挂在
 * 哪棵树上"，顶层窗口走 view，其余能挂的只有 layer surface。
 * 对象在 handle_node_destroy 里会先清 layer_surface->data，所以这里拿到
 * 非 NULL 就说明 scene 树还活着。
 */
struct woodland_layer_surface *woodland_layer_surface_from_wlr_surface(
    struct wlr_surface *surface)
{
    struct wlr_layer_surface_v1 *layer_surface =
        wlr_layer_surface_v1_try_from_wlr_surface(surface);
    if (!layer_surface) {
        return NULL;
    }
    return layer_surface->data;
}

/* ==================== layer surface 事件 ==================== */

static void handle_map(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, map);
    layer->mapped = true;
    struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
    if (layer_surface->output) {
        struct woodland_output *output = layer_surface->output->data;
        woodland_layer_arrange(layer->server, output);
        /*
         * 独占交互，或处于 top/overlay 层的 on_demand 交互在 map 时尝试获取焦点。
         * 位于 bottom/background 层的 on_demand（如桌面或面板组件重启）禁止偷取键盘焦点。
         */
        if (layer_surface->current.keyboard_interactive ==
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
            (layer_surface->current.keyboard_interactive ==
                 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
             layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)) {
            woodland_layer_try_focus(layer->server, layer_surface);
        }
    }
    if (layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        woodland_effect_mark_blur_dirty(layer->server);
    }
    (void)data;
}

static void handle_unmap(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, unmap);
    layer->mapped = false;
    struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
    if (layer_surface->output) {
        struct woodland_output *output = layer_surface->output->data;
        woodland_layer_arrange(layer->server, output);
    }
    if (layer->server && layer->server->focused_layer == layer_surface) {
        try_to_focus_next_layer_or_toplevel(layer->server);
    }
    if (layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        woodland_effect_mark_blur_dirty(layer->server);
    }
    (void)data;
}

void woodland_layer_handle_surface_commit(struct woodland_layer_surface *layer)
{
    if (!layer || !layer->layer_surface) {
        return;
    }
    struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
    struct wlr_output *wlr_output = layer_surface->output;
    if (!wlr_output) {
        return;
    }
    struct woodland_output *output = wlr_output->data;
    uint32_t committed = layer_surface->current.committed;

    /* 处理 layer 变化：reparent 到新 layer tree */
    if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
        wlr_scene_node_reparent(&layer->scene_layer_surface->tree->node,
                                output->layer_tree[layer_surface->current.layer]);
    }
    /* 处理 keyboard interactivity 变化 */
    if (committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY) {
        if (layer_surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
            output->server->focused_layer == layer_surface) {
            /* 从 EXCLUSIVE 降级到 ON_DEMAND 时释放独占焦点 */
            try_to_focus_next_layer_or_toplevel(output->server);
        } else {
            woodland_layer_try_focus(output->server, layer_surface);
        }
    }

    if (committed || layer->mapped != layer_surface->surface->mapped) {
        layer->mapped = layer_surface->surface->mapped;
        woodland_layer_arrange(output->server, output);
    }
    if (layer_surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        woodland_effect_mark_blur_dirty(output->server);
    }
    if (layer->effect.blur) {
        woodland_effect_layer_blur(layer, &layer->effect.blur_region);
    }
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, surface_commit);
    woodland_layer_handle_surface_commit(layer);
    (void)data;
}

static void handle_node_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, node_destroy);
    struct woodland_server *server = layer->server;

    /* 若持有键盘焦点，先把指针清掉再回退给 toplevel：
     * layer 被销毁会带走键盘焦点，不回退的话输入会直接丢失 */
    if (server->focused_layer == layer->layer_surface) {
        server->focused_layer = NULL;
        try_to_focus_next_layer_or_toplevel(server);
    }
    /* wlroots 可能已释放 scene_layer_surface，置 NULL 防误用 */
    layer->scene_layer_surface = NULL;
    if (layer->layer_surface) {
        layer->layer_surface->data = NULL;
    }
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->surface_commit.link);
    wl_list_remove(&layer->output_destroy.link);
    wl_list_remove(&layer->node_destroy.link);
    wl_list_remove(&layer->new_popup.link);
    free(layer);
    (void)data;
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, output_destroy);
    layer->layer_surface->output = NULL;
    wlr_layer_surface_v1_destroy(layer->layer_surface);
    (void)data;
}

/* ==================== popup ==================== */

static void handle_popup_commit(struct wl_listener *listener, void *data)
{
    struct woodland_layer_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);

        /* 客户端请求 grab 时（如面板应用菜单）赋予键盘焦点 */
        struct woodland_server *server = popup->layer ? popup->layer->server : NULL;
        struct wlr_seat *seat = server ? server->seat : NULL;
        bool requesting_grab = !!popup->xdg_popup->seat;
        if (requesting_grab && seat && !woodland_session_lock_is_locked(server)) {
            if (seat->keyboard_state.focused_surface == popup->xdg_popup->parent) {
                popup->parent_was_focused = true;
            }
            woodland_seat_focus_surface(server, popup->xdg_popup->base->surface);
        }
    }
    (void)data;
}

static void handle_popup_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_layer_popup *popup = wl_container_of(listener, popup, destroy);
    struct woodland_server *server = popup->layer ? popup->layer->server : NULL;
    struct wlr_seat *seat = server ? server->seat : NULL;

    if (seat && seat->keyboard_state.focused_surface == popup->xdg_popup->base->surface) {
        if (popup->parent_was_focused && popup->xdg_popup->parent) {
            woodland_seat_focus_surface(server, popup->xdg_popup->parent);
        } else if (server) {
            woodland_focus_fallback(server);
        }
    }

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    (void)data;
}

static void handle_new_popup(struct wl_listener *listener, void *data)
{
    struct woodland_layer_surface *layer = wl_container_of(listener, layer, new_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    struct woodland_layer_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "failed to allocate layer popup");
        return;
    }
    popup->layer = layer;
    popup->xdg_popup = xdg_popup;
    popup->scene_tree =
        wlr_scene_xdg_surface_create(layer->scene_layer_surface->tree, xdg_popup->base);
    if (!popup->scene_tree) {
        free(popup);
        return;
    }
    /* 和 shell.c 里 toplevel/popup 一致：base->data 存这个 xdg surface 自己的
     * 场景树。嵌套 popup（父是 popup）走 shell.c 的 server_new_xdg_popup，
     * 靠读父 popup 的 base->data 找到挂载点；这里不设会留下 NULL 导致断言崩溃 */
    xdg_popup->base->data = popup->scene_tree;

    woodland_listener_connect(&popup->commit, &xdg_popup->base->surface->events.commit,
                              handle_popup_commit, "layer.commit");
    woodland_listener_connect(&popup->destroy, &xdg_popup->events.destroy, handle_popup_destroy,
                              "layer.destroy");
}

/* ==================== new_surface ==================== */

static void handle_new_layer_surface(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    /* 无输出时分配第一个输出 */
    if (!layer_surface->output) {
        struct woodland_output *wo;
        wl_list_for_each(wo, &server->outputs, link)
        {
            layer_surface->output = wo->wlr_output;
            break;
        }
        if (!layer_surface->output) {
            wlr_log(WLR_ERROR, "no output available for layer surface");
            wlr_layer_surface_v1_destroy(layer_surface);
            return;
        }
    }

    struct woodland_layer_surface *layer = calloc(1, sizeof(*layer));
    if (!layer) {
        wlr_log(WLR_ERROR, "failed to allocate layer surface");
        return;
    }
    layer->layer_surface = layer_surface;
    layer->server = server;
    layer_surface->data = layer;

    struct woodland_output *output = layer_surface->output->data;
    struct wlr_scene_tree *selected_layer = output->layer_tree[layer_surface->current.layer];

    layer->scene_layer_surface = wlr_scene_layer_surface_v1_create(selected_layer, layer_surface);
    if (!layer->scene_layer_surface) {
        wlr_log(WLR_ERROR, "failed to create scene layer surface");
        free(layer);
        return;
    }
    /* node data 存 layer surface，供 arrange_one_layer 反查 */
    layer->scene_layer_surface->tree->node.data = layer;

    woodland_listener_connect(&layer->surface_commit, &layer_surface->surface->events.commit,
                              handle_surface_commit, "layer.commit");
    woodland_listener_connect(&layer->map, &layer_surface->surface->events.map, handle_map,
                              "layer.map");
    woodland_listener_connect(&layer->unmap, &layer_surface->surface->events.unmap, handle_unmap,
                              "layer.unmap");
    woodland_listener_connect(&layer->new_popup, &layer_surface->events.new_popup, handle_new_popup,
                              "layer.new_popup");
    woodland_listener_connect(&layer->output_destroy, &layer_surface->output->events.destroy,
                              handle_output_destroy, "layer.destroy");
    woodland_listener_connect(&layer->node_destroy,
                              &layer->scene_layer_surface->tree->node.events.destroy,
                              handle_node_destroy, "layer.destroy");
}

/* ==================== init/finish ==================== */

bool woodland_layer_init(struct woodland_server *server)
{
    server->layer_shell = wlr_layer_shell_v1_create(server->display, WOODLAND_LAYER_SHELL_VERSION);
    if (!server->layer_shell) {
        wlr_log(WLR_ERROR, "failed to create layer shell");
        return false;
    }

    woodland_listener_connect(&server->new_layer_surface, &server->layer_shell->events.new_surface,
                              handle_new_layer_surface, "layer.new_surface");
    return true;
}

void woodland_layer_finish(struct woodland_server *server)
{
    if (server->layer_shell) {
        wl_list_remove(&server->new_layer_surface.link);
    }
    /* manager 随 display 销毁，不单独 destroy；此处仅置空 */
    server->layer_shell = NULL;
    server->focused_layer = NULL;
}
