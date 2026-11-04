// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <stdlib.h>

#include <wlr/backend/drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "output.h"
#include "protocol_internal.h"
#include "protocol.h"
#include "woodland.h"

struct woodland_proto_drm_lease {
    struct woodland_proto *owner;
    struct woodland_server *server;

    struct wlr_drm_lease_v1_manager *manager;
    struct wl_listener request;
    struct wl_listener new_output;
};

static bool is_output_offered(struct wlr_drm_lease_v1_manager *manager,
                              struct wlr_output *output)
{
    struct wlr_drm_lease_device_v1 *device;
    wl_list_for_each(device, &manager->devices, link) {
        struct wlr_drm_lease_connector_v1 *conn;
        wl_list_for_each(conn, &device->connectors, link) {
            if (conn->output == output) {
                return true;
            }
        }
    }
    return false;
}

static void handle_lease_request(struct wl_listener *listener, void *data)
{
    struct woodland_proto_drm_lease *lease =
        wl_container_of(listener, lease, request);
    struct wlr_drm_lease_request_v1 *req = data;

    struct wlr_drm_lease_v1 *granted_lease = wlr_drm_lease_request_v1_grant(req);
    if (!granted_lease) {
        wlr_log(WLR_ERROR, "failed to grant drm lease request");
        wlr_drm_lease_request_v1_reject(req);
        return;
    }
    wlr_log(WLR_INFO, "granted drm lease request %p", (void *)req);
    (void)lease;
}

static void handle_new_output(struct wl_listener *listener, void *data)
{
    struct woodland_proto_drm_lease *lease =
        wl_container_of(listener, lease, new_output);
    struct wlr_output *wlr_output = data;

    if (!lease->manager || !wlr_output) {
        return;
    }

    if (wlr_output_is_drm(wlr_output) && !is_output_offered(lease->manager, wlr_output)) {
        wlr_log(WLR_DEBUG, "offering DRM output %s for lease", wlr_output->name);
        wlr_drm_lease_v1_manager_offer_output(lease->manager, wlr_output);
    }
}

bool woodland_proto_drm_lease_init(struct woodland_proto *proto)
{
    struct woodland_server *server = proto->server;
    struct woodland_proto_drm_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) {
        wlr_log(WLR_ERROR, "failed to allocate drm lease protocol state");
        return false;
    }
    lease->owner = proto;
    lease->server = server;
    proto->drm_lease = lease;

    lease->manager = wlr_drm_lease_v1_manager_create(server->display, server->backend);
    if (!lease->manager) {
        wlr_log(WLR_DEBUG, "DRM backend not found, wp_drm_lease_device_v1 not available");
        return true;
    }

    woodland_listener_connect(&lease->request, &lease->manager->events.request,
                              handle_lease_request, "protocol.drm_lease_request");

    woodland_listener_connect(&lease->new_output, &server->backend->events.new_output,
                              handle_new_output, "protocol.drm_lease_new_output");

    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output && wlr_output_is_drm(output->wlr_output) &&
            !is_output_offered(lease->manager, output->wlr_output)) {
            wlr_drm_lease_v1_manager_offer_output(lease->manager, output->wlr_output);
        }
    }

    return true;
}

void woodland_proto_drm_lease_finish(struct woodland_proto *proto)
{
    if (!proto || !proto->drm_lease) {
        return;
    }
    struct woodland_proto_drm_lease *lease = proto->drm_lease;
    if (lease->new_output.link.prev) {
        wl_list_remove(&lease->new_output.link);
    }
    if (lease->request.link.prev) {
        wl_list_remove(&lease->request.link);
    }
    free(lease);
    proto->drm_lease = NULL;
}

void woodland_proto_drm_lease_offer_output(struct woodland_proto *proto,
                                           struct wlr_output *output)
{
    if (!proto || !proto->drm_lease || !proto->drm_lease->manager || !output) {
        return;
    }
    if (wlr_output_is_drm(output) && !is_output_offered(proto->drm_lease->manager, output)) {
        wlr_drm_lease_v1_manager_offer_output(proto->drm_lease->manager, output);
    }
}

void woodland_proto_drm_lease_withdraw_output(struct woodland_proto *proto,
                                              struct wlr_output *output)
{
    if (!proto || !proto->drm_lease || !proto->drm_lease->manager || !output) {
        return;
    }
    if (wlr_output_is_drm(output) && is_output_offered(proto->drm_lease->manager, output)) {
        wlr_drm_lease_v1_manager_withdraw_output(proto->drm_lease->manager, output);
    }
}
