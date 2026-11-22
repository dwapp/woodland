// SPDX-License-Identifier: GPL-2.0-only
/*
 * 只给 Xwayland 的 xdg-output global：见 include/xwayland_outputs.h。
 * 思路来自 Umbriel（src/xwayland/outputs.cpp）与 treeland（给 Xwayland 单独一个
 * xdg-output manager 并改写它看到的缩放）。
 */
#include "xwayland_outputs.h"

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "xdg-output-unstable-v1-protocol.h"

#include "woodland.h"
#include "woodland_config.h"
#include "xwayland_scale.h"

#define XDG_OUTPUT_VERSION 3

struct xwl_output_entry {
    struct wl_list link;
    struct woodland_xwl_outputs *owner;
    struct wl_resource *resource;
    struct wlr_output *output;
    struct wlr_box sent;
    struct wl_listener output_destroy;
};

struct woodland_xwl_outputs {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list entries; // struct xwl_output_entry.link
};

static void destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_impl = {
    .destroy = destroy_resource,
};

static void entry_forget(struct xwl_output_entry *entry)
{
    wl_list_remove(&entry->output_destroy.link);
    wl_list_remove(&entry->link);
    free(entry);
}

static void send_entry(struct xwl_output_entry *entry, bool initial)
{
    struct woodland_server *server = entry->owner->server;
    struct wlr_box box = woodland_xwl_map_x_box(&server->xwl_map, entry->output);
    if (wlr_box_empty(&box) || (!initial && wlr_box_equal(&box, &entry->sent))) {
        return;
    }
    entry->sent = box;
    int version = wl_resource_get_version(entry->resource);
    if (initial && version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
        zxdg_output_v1_send_name(entry->resource, entry->output->name);
        if (entry->output->description) {
            zxdg_output_v1_send_description(entry->resource, entry->output->description);
        }
    }
    zxdg_output_v1_send_logical_position(entry->resource, box.x, box.y);
    zxdg_output_v1_send_logical_size(entry->resource, box.width, box.height);
    /* 版本 3 起 xdg_output.done 由 wl_output.done 取代 */
    if (version >= 3) {
        wlr_output_schedule_done(entry->output);
    } else {
        zxdg_output_v1_send_done(entry->resource);
    }
}

static void handle_resource_destroy(struct wl_resource *resource)
{
    struct xwl_output_entry *entry = wl_resource_get_user_data(resource);
    if (entry) {
        entry_forget(entry);
    }
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct xwl_output_entry *entry = wl_container_of(listener, entry, output_destroy);
    /* 资源还归客户端所有，只是不再收事件 */
    wl_resource_set_user_data(entry->resource, NULL);
    entry_forget(entry);
}

static void manager_get_xdg_output(struct wl_client *client, struct wl_resource *manager,
                                   uint32_t id, struct wl_resource *output_resource)
{
    struct woodland_xwl_outputs *self = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(
        client, &zxdg_output_v1_interface, wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, NULL, handle_resource_destroy);
    struct wlr_output *output = wlr_output_from_resource(output_resource);
    if (!output) {
        return; /* 失效的 wl_output 对应失效的 xdg_output */
    }
    struct xwl_output_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        wl_client_post_no_memory(client);
        return;
    }
    entry->owner = self;
    entry->resource = resource;
    entry->output = output;
    entry->output_destroy.notify = handle_output_destroy;
    wl_signal_add(&output->events.destroy, &entry->output_destroy);
    wl_list_insert(&self->entries, &entry->link);
    wl_resource_set_user_data(resource, entry);
    send_entry(entry, true);
}

static const struct zxdg_output_manager_v1_interface manager_impl = {
    .destroy = destroy_resource,
    .get_xdg_output = manager_get_xdg_output,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    uint32_t v = version < XDG_OUTPUT_VERSION ? version : XDG_OUTPUT_VERSION;
    struct wl_resource *resource =
        wl_resource_create(client, &zxdg_output_manager_v1_interface, (int)v, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

struct woodland_xwl_outputs *woodland_xwl_outputs_create(struct woodland_server *server)
{
    struct woodland_xwl_outputs *self = calloc(1, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->server = server;
    wl_list_init(&self->entries);
    self->global = wl_global_create(server->display, &zxdg_output_manager_v1_interface,
                                    XDG_OUTPUT_VERSION, self, manager_bind);
    if (!self->global) {
        free(self);
        return NULL;
    }
    woodland_xwl_map_update(&server->xwl_map, server->output_layout);
    return self;
}

void woodland_xwl_outputs_destroy(struct woodland_xwl_outputs *self)
{
    if (!self) {
        return;
    }
    struct xwl_output_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &self->entries, link)
    {
        wl_resource_set_user_data(entry->resource, NULL);
        entry_forget(entry);
    }
    if (self->global) {
        wl_global_destroy(self->global);
        self->global = NULL;
    }
    free(self);
}

bool woodland_xwl_outputs_advertise(const struct woodland_server *server,
                                    const struct wl_client *client,
                                    const struct wl_global *global,
                                    const struct wl_client *xwayland)
{
    if (!server || !server->xwl_outputs) {
        return true;
    }
    bool is_xwayland = client != NULL && client == xwayland;
    if (global == server->xwl_outputs->global) {
        return is_xwayland;
    }
    if (server->xdg_output_manager && global == server->xdg_output_manager->global) {
        return !is_xwayland;
    }
    return true;
}

bool woodland_xwl_outputs_update(struct woodland_server *server)
{
    if (!server || !server->xwl_outputs) {
        return false;
    }
    bool next_enabled = !server->config || server->config->xwayland.native_resolution;
    bool toggled = (next_enabled != server->xwl_map.enabled);
    server->xwl_map.enabled = next_enabled;
    bool changed = woodland_xwl_map_update(&server->xwl_map, server->output_layout);
    struct xwl_output_entry *entry;
    wl_list_for_each(entry, &server->xwl_outputs->entries, link)
    {
        send_entry(entry, false);
    }
    return changed || toggled;
}
