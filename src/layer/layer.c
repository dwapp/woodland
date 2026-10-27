// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "output.h"
#include "view.h"
#include "woodland.h"

#define WOODLAND_LAYER_SHELL_VERSION 4

/* ==================== arrange ==================== */

static void arrange_one_layer(const struct wlr_box *full_area,
                              struct wlr_box *usable_area,
                              struct wlr_scene_tree *tree, bool exclusive) {
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
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

void woodland_layer_arrange(struct woodland_server *server,
                            struct woodland_output *output) {
    (void)server;
    struct wlr_box full_area = {0};
    wlr_output_effective_resolution(output->wlr_output,
        &full_area.width, &full_area.height);
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
        wlr_scene_node_set_position(&output->layer_tree[i]->node,
            output->scene_output->x, output->scene_output->y);
    }
}

/* ==================== 焦点（任务 3 完整实现，本任务先提供供 commit 调用） ==================== */

void woodland_layer_try_focus(struct woodland_server *server,
                              struct wlr_layer_surface_v1 *layer_surface) {
    enum zwlr_layer_surface_v1_keyboard_interactivity interactivity =
        layer_surface->current.keyboard_interactive;
    struct wlr_seat *seat = server->seat;

    switch (interactivity) {
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND:
        if (interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
            && server->focused_layer != NULL) {
            /* on_demand 不抢占已有 exclusive 层的焦点 */
            if (server->focused_layer->current.keyboard_interactive
                == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                return;
            }
        }
        server->focused_layer = layer_surface;
        /* 失活当前 toplevel（xdg 或 xwayland 通吃）：
         * activated / SSD 激活色 / foreign-toplevel 三处一起改 */
        woodland_focus_yield_to_layer(server);
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
        if (keyboard != NULL) {
            wlr_seat_keyboard_notify_enter(seat, layer_surface->surface,
                keyboard->keycodes, keyboard->num_keycodes,
                &keyboard->modifiers);
        }
        break;
    case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE:
        if (server->focused_layer == layer_surface) {
            server->focused_layer = NULL;
            /* 焦点还给当前工作区顶层视图（统一的回退逻辑） */
            woodland_focus_fallback(server);
        }
        break;
    }
}

struct wlr_layer_surface_v1 *woodland_layer_surface_at(
        struct woodland_server *server, double lx, double ly,
        struct wlr_surface **surface, double *sx, double *sy) {
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
    return wlr_layer_surface_v1_try_from_wlr_surface(scene_surface->surface);
}

/* ==================== layer surface 事件 ==================== */

static void handle_map(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, map);
    layer->mapped = true;
    struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
    if (layer_surface->output) {
        struct woodland_output *output = layer_surface->output->data;
        woodland_layer_arrange(layer->server, output);
        /* exclusive 交互时给键盘焦点 */
        if (layer_surface->current.keyboard_interactive
            == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
            woodland_layer_try_focus(layer->server, layer_surface);
        }
    }
    (void)data;
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, unmap);
    layer->mapped = false;
    struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
    if (layer_surface->output) {
        struct woodland_output *output = layer_surface->output->data;
        woodland_layer_arrange(layer->server, output);
    }
    (void)data;
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, surface_commit);
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
        woodland_layer_try_focus(output->server, layer_surface);
    }

    if (committed || layer->mapped != layer_surface->surface->mapped) {
        layer->mapped = layer_surface->surface->mapped;
        woodland_layer_arrange(output->server, output);
    }
    (void)data;
}

static void handle_node_destroy(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, node_destroy);
    struct woodland_server *server = layer->server;

    /* 若持有键盘焦点，先把指针清掉再回退给 toplevel：
     * layer 被销毁会带走键盘焦点，不回退的话输入会直接丢失 */
    if (server->focused_layer == layer->layer_surface) {
        server->focused_layer = NULL;
        woodland_focus_fallback(server);
    }
    /* wlroots 可能已释放 scene_layer_surface，置 NULL 防误用 */
    layer->scene_layer_surface = NULL;

    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->surface_commit.link);
    wl_list_remove(&layer->output_destroy.link);
    wl_list_remove(&layer->node_destroy.link);
    wl_list_remove(&layer->new_popup.link);
    free(layer);
    (void)data;
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, output_destroy);
    layer->layer_surface->output = NULL;
    wlr_layer_surface_v1_destroy(layer->layer_surface);
    (void)data;
}

/* ==================== popup ==================== */

static void handle_popup_commit(struct wl_listener *listener, void *data) {
    struct woodland_layer_popup *popup =
        wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
    (void)data;
}

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
    struct woodland_layer_popup *popup =
        wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    (void)data;
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
    struct woodland_layer_surface *layer =
        wl_container_of(listener, layer, new_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    struct woodland_layer_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wlr_log(WLR_ERROR, "failed to allocate layer popup");
        return;
    }
    popup->xdg_popup = xdg_popup;
    popup->scene_tree = wlr_scene_xdg_surface_create(
        layer->scene_layer_surface->tree, xdg_popup->base);
    if (!popup->scene_tree) {
        free(popup);
        return;
    }

    popup->commit.notify = handle_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = handle_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ==================== new_surface ==================== */

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
    struct woodland_server *server =
        wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    /* 无输出时分配第一个输出 */
    if (!layer_surface->output) {
        struct woodland_output *wo;
        wl_list_for_each(wo, &server->outputs, link) {
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

    struct woodland_output *output = layer_surface->output->data;
    struct wlr_scene_tree *selected_layer =
        output->layer_tree[layer_surface->current.layer];

    layer->scene_layer_surface = wlr_scene_layer_surface_v1_create(
        selected_layer, layer_surface);
    if (!layer->scene_layer_surface) {
        wlr_log(WLR_ERROR, "failed to create scene layer surface");
        free(layer);
        return;
    }
    /* node data 存 layer surface，供 arrange_one_layer 反查 */
    layer->scene_layer_surface->tree->node.data = layer;

    layer->surface_commit.notify = handle_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &layer->surface_commit);
    layer->map.notify = handle_map;
    wl_signal_add(&layer_surface->surface->events.map, &layer->map);
    layer->unmap.notify = handle_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
    layer->new_popup.notify = handle_new_popup;
    wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);
    layer->output_destroy.notify = handle_output_destroy;
    wl_signal_add(&layer_surface->output->events.destroy, &layer->output_destroy);
    layer->node_destroy.notify = handle_node_destroy;
    wl_signal_add(&layer->scene_layer_surface->tree->node.events.destroy,
                  &layer->node_destroy);
}

/* ==================== init/finish ==================== */

bool woodland_layer_init(struct woodland_server *server) {
    server->layer_shell = wlr_layer_shell_v1_create(
        server->display, WOODLAND_LAYER_SHELL_VERSION);
    if (!server->layer_shell) {
        wlr_log(WLR_ERROR, "failed to create layer shell");
        return false;
    }

    server->new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface,
                  &server->new_layer_surface);
    return true;
}

void woodland_layer_finish(struct woodland_server *server) {
    if (server->layer_shell) {
        wl_list_remove(&server->new_layer_surface.link);
    }
    /* manager 随 display 销毁，不单独 destroy；此处仅置空 */
    server->layer_shell = NULL;
    server->focused_layer = NULL;
}
