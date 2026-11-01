// SPDX-License-Identifier: GPL-2.0-only
/* 基于 labwc 与 Sway 的输入法中继器实现 */
#include "ime.h"

#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "seat.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"

#define SAME_CLIENT(wlr_obj1, wlr_obj2)                                                            \
    (wl_resource_get_client((wlr_obj1)->resource) == wl_resource_get_client((wlr_obj2)->resource))

struct woodland_text_input {
    struct woodland_ime_relay *relay;
    struct wlr_text_input_v3 *input;
    struct wl_list link;

    struct wl_listener enable;
    struct wl_listener disable;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct woodland_ime_popup {
    struct woodland_ime_relay *relay;
    struct wlr_input_popup_surface_v2 *popup_surface;
    struct wlr_scene_tree *tree;
    struct wl_list link;

    struct wl_listener commit;
    struct wl_listener destroy;
};

/* ==================== 键盘与防死循环逻辑 ==================== */

bool woodland_ime_is_emulated_keyboard(struct woodland_server *server,
                                       struct wlr_keyboard *keyboard)
{
    if (!server || !server->ime || !server->ime->input_method || !keyboard) {
        return false;
    }
    struct wlr_virtual_keyboard_v1 *vk = wlr_input_device_get_virtual_keyboard(&keyboard->base);
    return vk && SAME_CLIENT(vk, server->ime->input_method);
}

static struct wlr_input_method_keyboard_grab_v2 *get_keyboard_grab(struct woodland_server *server,
                                                                   struct wlr_keyboard *keyboard)
{
    if (!server || !server->ime || !server->ime->input_method) {
        return NULL;
    }
    struct wlr_input_method_v2 *im = server->ime->input_method;
    if (!im->keyboard_grab) {
        return NULL;
    }
    /* 防死循环：输入法自身通过虚拟键盘注入的按键，绝对不能再次转发给输入法 grab */
    if (woodland_ime_is_emulated_keyboard(server, keyboard)) {
        return NULL;
    }
    return im->keyboard_grab;
}

bool woodland_ime_keyboard_grab_key(struct woodland_server *server, struct wlr_keyboard *keyboard,
                                    struct wlr_keyboard_key_event *event)
{
    struct wlr_input_method_keyboard_grab_v2 *grab = get_keyboard_grab(server, keyboard);
    if (!grab) {
        return false;
    }
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec, event->keycode,
                                               event->state);
    return true;
}

bool woodland_ime_keyboard_grab_modifiers(struct woodland_server *server,
                                          struct wlr_keyboard *keyboard)
{
    struct wlr_input_method_keyboard_grab_v2 *grab = get_keyboard_grab(server, keyboard);
    if (!grab) {
        return false;
    }
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &keyboard->modifiers);
    return true;
}

/* ==================== 活跃输入状态推导 ==================== */

static struct woodland_text_input *get_active_text_input(struct woodland_ime_relay *relay)
{
    if (!relay->input_method) {
        return NULL;
    }
    struct woodland_text_input *ti;
    wl_list_for_each(ti, &relay->text_inputs, link)
    {
        if (ti->input->focused_surface && ti->input->current_enabled) {
            return ti;
        }
    }
    return NULL;
}

static void update_active_text_input(struct woodland_ime_relay *relay)
{
    struct woodland_text_input *active = get_active_text_input(relay);
    if (relay->input_method && relay->active_text_input != active) {
        if (active) {
            wlr_input_method_v2_send_activate(relay->input_method);
        } else {
            wlr_input_method_v2_send_deactivate(relay->input_method);
        }
        wlr_input_method_v2_send_done(relay->input_method);
    }
    relay->active_text_input = active;
}

static void update_text_inputs_focused_surface(struct woodland_ime_relay *relay)
{
    struct woodland_text_input *ti;
    wl_list_for_each(ti, &relay->text_inputs, link)
    {
        struct wlr_text_input_v3 *input = ti->input;
        struct wlr_surface *new_focused = NULL;
        if (relay->input_method && relay->focused_surface &&
            SAME_CLIENT(input, relay->focused_surface)) {
            new_focused = relay->focused_surface;
        }
        if (input->focused_surface == new_focused) {
            continue;
        }
        if (input->focused_surface) {
            wlr_text_input_v3_send_leave(input);
        }
        if (new_focused) {
            wlr_text_input_v3_send_enter(input, new_focused);
        }
    }
}

/* ==================== 候选框定位 ==================== */

static void update_popup_position(struct woodland_ime_popup *popup)
{
    struct woodland_ime_relay *relay = popup->relay;
    struct woodland_text_input *text_input = relay->active_text_input;
    if (!text_input || !relay->focused_surface || !popup->popup_surface->surface->mapped) {
        return;
    }

    struct wlr_box cursor_rect = {0};
    if (text_input->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) {
        cursor_rect = text_input->input->current.cursor_rectangle;
    }

    /* 转换为全局输出布局坐标 */
    struct woodland_view *view = NULL;
    struct woodland_view *v;
    wl_list_for_each(v, &relay->server->views, link)
    {
        if (woodland_view_surface(v) == relay->focused_surface) {
            view = v;
            break;
        }
    }
    if (view) {
        struct wlr_box geo;
        woodland_view_content_box(view, &geo);
        int lx = 0, ly = 0;
        wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
        cursor_rect.x += lx + geo.x;
        cursor_rect.y += ly + geo.y;
    } else {
        struct wlr_layer_surface_v1 *layer_surface =
            wlr_layer_surface_v1_try_from_wlr_surface(relay->focused_surface);
        if (layer_surface && layer_surface->data) {
            struct woodland_layer_surface *layer = layer_surface->data;
            if (layer->scene_layer_surface && layer->scene_layer_surface->tree) {
                int lx = 0, ly = 0;
                wlr_scene_node_coords(&layer->scene_layer_surface->tree->node, &lx, &ly);
                cursor_rect.x += lx;
                cursor_rect.y += ly;
            }
        }
    }

    /* 根据屏幕边界 unconstrain */
    int pw = popup->popup_surface->surface->current.width;
    int ph = popup->popup_surface->surface->current.height;
    int x = cursor_rect.x;
    int y = cursor_rect.y + cursor_rect.height;

    struct wlr_output *wlr_out =
        wlr_output_layout_output_at(relay->server->output_layout, cursor_rect.x, cursor_rect.y);
    if (wlr_out) {
        struct wlr_box out_box;
        wlr_output_layout_get_box(relay->server->output_layout, wlr_out, &out_box);
        if (y + ph > out_box.y + out_box.height) {
            y = cursor_rect.y - ph; // 翻转至光标上方
        }
        if (x + pw > out_box.x + out_box.width) {
            x = out_box.x + out_box.width - pw; // 左移防越界
        }
        if (x < out_box.x)
            x = out_box.x;
        if (y < out_box.y)
            y = out_box.y;
    }

    wlr_scene_node_set_position(&popup->tree->node, x, y);
    wlr_scene_node_raise_to_top(&relay->popup_tree->node);

    struct wlr_box rel_rect = {
        .x = cursor_rect.x - x,
        .y = cursor_rect.y - y,
        .width = cursor_rect.width,
        .height = cursor_rect.height,
    };
    wlr_input_popup_surface_v2_send_text_input_rectangle(popup->popup_surface, &rel_rect);
}

static void update_popups_position(struct woodland_ime_relay *relay)
{
    struct woodland_ime_popup *popup;
    wl_list_for_each(popup, &relay->popups, link)
    {
        update_popup_position(popup);
    }
}

/* ==================== 状态双向转发 ==================== */

static void send_state_to_input_method(struct woodland_ime_relay *relay)
{
    if (!relay->active_text_input || !relay->input_method) {
        return;
    }
    struct wlr_input_method_v2 *im = relay->input_method;
    struct wlr_text_input_v3 *ti = relay->active_text_input->input;

    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
        wlr_input_method_v2_send_surrounding_text(im, ti->current.surrounding.text,
                                                  ti->current.surrounding.cursor,
                                                  ti->current.surrounding.anchor);
    }
    wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
        wlr_input_method_v2_send_content_type(im, ti->current.content_type.hint,
                                              ti->current.content_type.purpose);
    }
    wlr_input_method_v2_send_done(im);
}

static void handle_input_method_commit(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, input_method_commit);
    struct wlr_input_method_v2 *im = relay->input_method;
    struct woodland_text_input *ti = relay->active_text_input;
    (void)data;
    if (!ti) {
        return;
    }
    if (im->current.preedit.text) {
        wlr_text_input_v3_send_preedit_string(ti->input, im->current.preedit.text,
                                              im->current.preedit.cursor_begin,
                                              im->current.preedit.cursor_end);
    }
    if (im->current.commit_text) {
        wlr_text_input_v3_send_commit_string(ti->input, im->current.commit_text);
    }
    if (im->current.delete.before_length || im->current.delete.after_length) {
        wlr_text_input_v3_send_delete_surrounding_text(ti->input, im->current.delete.before_length,
                                                       im->current.delete.after_length);
    }
    wlr_text_input_v3_send_done(ti->input);
}

/* ==================== 弹窗与生命周期 ==================== */

static void handle_popup_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_ime_popup *popup = wl_container_of(listener, popup, destroy);
    (void)data;
    wlr_scene_node_destroy(&popup->tree->node);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void handle_popup_commit(struct wl_listener *listener, void *data)
{
    struct woodland_ime_popup *popup = wl_container_of(listener, popup, commit);
    (void)data;
    update_popup_position(popup);
}

static void handle_input_method_new_popup_surface(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay =
        wl_container_of(listener, relay, input_method_new_popup_surface);
    struct wlr_input_popup_surface_v2 *popup_surface = data;

    struct woodland_ime_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        return;
    }
    popup->popup_surface = popup_surface;
    popup->relay = relay;

    popup->tree = wlr_scene_subsurface_tree_create(relay->popup_tree, popup_surface->surface);
    if (!popup->tree) {
        free(popup);
        return;
    }

    woodland_listener_connect(&popup->destroy, &popup_surface->events.destroy, handle_popup_destroy,
                              "ime.popup.destroy");
    woodland_listener_connect(&popup->commit, &popup_surface->surface->events.commit,
                              handle_popup_commit, "ime.popup.commit");

    wl_list_insert(&relay->popups, &popup->link);
    update_popup_position(popup);
}

static void handle_keyboard_grab_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, keyboard_grab_destroy);
    (void)data;
    wl_list_remove(&relay->keyboard_grab_destroy.link);
}

static void handle_input_method_grab_keyboard(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, input_method_grab_keyboard);
    struct wlr_input_method_keyboard_grab_v2 *grab = data;

    struct wlr_keyboard *active_kb = wlr_seat_get_keyboard(relay->server->seat);
    if (active_kb && !woodland_ime_is_emulated_keyboard(relay->server, active_kb)) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(grab, active_kb);
    }
    woodland_listener_connect(&relay->keyboard_grab_destroy, &grab->events.destroy,
                              handle_keyboard_grab_destroy, "ime.grab_destroy");
}

static void handle_input_method_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, input_method_destroy);
    (void)data;
    wl_list_remove(&relay->input_method_commit.link);
    wl_list_remove(&relay->input_method_grab_keyboard.link);
    wl_list_remove(&relay->input_method_new_popup_surface.link);
    wl_list_remove(&relay->input_method_destroy.link);
    relay->input_method = NULL;

    update_text_inputs_focused_surface(relay);
    update_active_text_input(relay);
}

static void handle_new_input_method(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, new_input_method);
    struct wlr_input_method_v2 *im = data;
    if (relay->server->seat != im->seat) {
        return;
    }

    if (relay->input_method) {
        wlr_log(WLR_INFO, "ime: second input-method connected, rejecting");
        wlr_input_method_v2_send_unavailable(im);
        return;
    }

    relay->input_method = im;

    woodland_listener_connect(&relay->input_method_commit, &im->events.commit,
                              handle_input_method_commit, "ime.im_commit");
    woodland_listener_connect(&relay->input_method_grab_keyboard, &im->events.grab_keyboard,
                              handle_input_method_grab_keyboard, "ime.grab_keyboard");
    woodland_listener_connect(&relay->input_method_destroy, &im->events.destroy,
                              handle_input_method_destroy, "ime.im_destroy");
    woodland_listener_connect(&relay->input_method_new_popup_surface, &im->events.new_popup_surface,
                              handle_input_method_new_popup_surface, "ime.new_popup");

    update_text_inputs_focused_surface(relay);
    update_active_text_input(relay);
}

/* ==================== text-input-v3 监听 ==================== */

static void handle_text_input_enable(struct wl_listener *listener, void *data)
{
    struct woodland_text_input *ti = wl_container_of(listener, ti, enable);
    (void)data;
    update_active_text_input(ti->relay);
    if (ti->relay->active_text_input == ti) {
        update_popups_position(ti->relay);
        send_state_to_input_method(ti->relay);
    }
}

static void handle_text_input_disable(struct wl_listener *listener, void *data)
{
    struct woodland_text_input *ti = wl_container_of(listener, ti, disable);
    (void)data;
    update_active_text_input(ti->relay);
}

static void handle_text_input_commit(struct wl_listener *listener, void *data)
{
    struct woodland_text_input *ti = wl_container_of(listener, ti, commit);
    (void)data;
    if (ti->relay->active_text_input == ti) {
        update_popups_position(ti->relay);
        send_state_to_input_method(ti->relay);
    }
}

static void handle_text_input_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_text_input *ti = wl_container_of(listener, ti, destroy);
    (void)data;
    wl_list_remove(&ti->enable.link);
    wl_list_remove(&ti->disable.link);
    wl_list_remove(&ti->commit.link);
    wl_list_remove(&ti->destroy.link);
    wl_list_remove(&ti->link);
    update_active_text_input(ti->relay);
    free(ti);
}

static void handle_new_text_input(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, new_text_input);
    struct wlr_text_input_v3 *wlr_ti = data;
    if (relay->server->seat != wlr_ti->seat) {
        return;
    }

    struct woodland_text_input *ti = calloc(1, sizeof(*ti));
    if (!ti) {
        return;
    }
    ti->input = wlr_ti;
    ti->relay = relay;
    wl_list_insert(&relay->text_inputs, &ti->link);

    woodland_listener_connect(&ti->enable, &wlr_ti->events.enable, handle_text_input_enable,
                              "ime.ti.enable");
    woodland_listener_connect(&ti->disable, &wlr_ti->events.disable, handle_text_input_disable,
                              "ime.ti.disable");
    woodland_listener_connect(&ti->commit, &wlr_ti->events.commit, handle_text_input_commit,
                              "ime.ti.commit");
    woodland_listener_connect(&ti->destroy, &wlr_ti->events.destroy, handle_text_input_destroy,
                              "ime.ti.destroy");

    update_text_inputs_focused_surface(relay);
}

/* ==================== 焦点与虚拟键盘 ==================== */

static void handle_focused_surface_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, focused_surface_destroy);
    (void)data;
    woodland_ime_set_focus(relay->server, NULL);
}

static void handle_seat_focus_change(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, seat_focus_change);
    struct wlr_seat_keyboard_focus_change_event *event = data;
    woodland_ime_set_focus(relay->server, event->new_surface);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
    struct woodland_ime_relay *relay = wl_container_of(listener, relay, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vk = data;
    /* 虚拟键盘以标准键盘接入 seat 键盘链表 */
    woodland_seat_new_keyboard(relay->server, &vk->keyboard.base);
}

void woodland_ime_set_focus(struct woodland_server *server, struct wlr_surface *surface)
{
    if (!server || !server->ime) {
        return;
    }
    struct woodland_ime_relay *relay = server->ime;
    if (relay->focused_surface == surface) {
        return;
    }
    if (relay->focused_surface) {
        wl_list_remove(&relay->focused_surface_destroy.link);
    }
    relay->focused_surface = surface;
    if (surface) {
        woodland_listener_connect(&relay->focused_surface_destroy, &surface->events.destroy,
                                  handle_focused_surface_destroy, "ime.focused_destroy");
    }

    update_text_inputs_focused_surface(relay);
    update_active_text_input(relay);
}

/* ==================== 模块初始化与销毁 ==================== */

bool woodland_ime_init(struct woodland_server *server)
{
    if (!server || !server->display || !server->scene) {
        return false;
    }

    struct woodland_ime_relay *relay = calloc(1, sizeof(*relay));
    if (!relay) {
        return false;
    }
    relay->server = server;
    wl_list_init(&relay->text_inputs);
    wl_list_init(&relay->popups);

    /* 候选框挂载在场景树顶层 */
    relay->popup_tree = wlr_scene_tree_create(&server->scene->tree);
    if (!relay->popup_tree) {
        free(relay);
        return false;
    }
    wlr_scene_node_raise_to_top(&relay->popup_tree->node);

    /*
     * 注册三个核心协议 global:
     * 安全特权说明：当前 Woodland 运行在用户会话内，未启用 security-context-v1 协议。
     * zwp_input_method_manager_v2 与 zwp_virtual_keyboard_manager_v1 对当前会话内客户端开放，
     * 便于 Fcitx5 等用户输入法服务自由接入。特权控制将在后续沙箱安全机制统一落地。
     */
    relay->input_method_manager = wlr_input_method_manager_v2_create(server->display);
    relay->text_input_manager = wlr_text_input_manager_v3_create(server->display);
    relay->virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server->display);
    if (!relay->input_method_manager || !relay->text_input_manager ||
        !relay->virtual_keyboard_manager) {
        wlr_log(WLR_ERROR, "failed to create input method managers");
        wlr_scene_node_destroy(&relay->popup_tree->node);
        free(relay);
        return false;
    }

    woodland_listener_connect(&relay->new_input_method,
                              &relay->input_method_manager->events.new_input_method,
                              handle_new_input_method, "ime.new_input_method");
    woodland_listener_connect(&relay->new_text_input,
                              &relay->text_input_manager->events.new_text_input,
                              handle_new_text_input, "ime.new_text_input");
    woodland_listener_connect(&relay->new_virtual_keyboard,
                              &relay->virtual_keyboard_manager->events.new_virtual_keyboard,
                              handle_new_virtual_keyboard, "ime.new_virtual_keyboard");

    /* 监听 seat 键盘焦点变更信号 */
    if (server->seat) {
        woodland_listener_connect(&relay->seat_focus_change,
                                  &server->seat->keyboard_state.events.focus_change,
                                  handle_seat_focus_change, "ime.seat_focus_change");
    }

    server->ime = relay;
    return true;
}

void woodland_ime_finish(struct woodland_server *server)
{
    if (!server || !server->ime) {
        return;
    }
    struct woodland_ime_relay *relay = server->ime;
    server->ime = NULL;

    if (relay->focused_surface) {
        wl_list_remove(&relay->focused_surface_destroy.link);
    }
    if (server->seat) {
        wl_list_remove(&relay->seat_focus_change.link);
    }
    wl_list_remove(&relay->new_input_method.link);
    wl_list_remove(&relay->new_text_input.link);
    wl_list_remove(&relay->new_virtual_keyboard.link);

    if (relay->input_method) {
        wl_list_remove(&relay->input_method_commit.link);
        wl_list_remove(&relay->input_method_grab_keyboard.link);
        wl_list_remove(&relay->input_method_new_popup_surface.link);
        wl_list_remove(&relay->input_method_destroy.link);
    }

    struct woodland_ime_popup *popup, *p_tmp;
    wl_list_for_each_safe(popup, p_tmp, &relay->popups, link)
    {
        handle_popup_destroy(&popup->destroy, NULL);
    }

    struct woodland_text_input *ti, *ti_tmp;
    wl_list_for_each_safe(ti, ti_tmp, &relay->text_inputs, link)
    {
        handle_text_input_destroy(&ti->destroy, NULL);
    }

    wlr_scene_node_destroy(&relay->popup_tree->node);
    free(relay);
}
