// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "treeland-wine-window-management-unstable-v1-protocol.h"
#include "treeland-wine-window-state-unstable-v1-protocol.h"

static void test_wm_enums(void)
{
    /* Z-order operations matching Windows SetWindowPos */
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_TOP == 0);
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_BOTTOM == 1);
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_TOPMOST == 2);
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_NOTOPMOST == 3);
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_Z_ORDER_OP_HWND_INSERT_AFTER == 4);

    /* Manager errors */
    assert(TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_TOPLEVEL_ALREADY_CONTROLLED == 0);
    assert(TREELAND_WINE_WINDOW_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL == 1);

    /* Control errors */
    assert(TREELAND_WINE_WINDOW_CONTROL_V1_ERROR_INVALID_SIBLING == 0);
}

static void test_state_enums(void)
{
    /* State bitfield flags */
    assert(TREELAND_WINE_WINDOW_STATE_V1_STATE_MINIMIZED == 1);
    assert(TREELAND_WINE_WINDOW_STATE_V1_STATE_ATTENTION == 2);

    /* Activate reasons */
    assert(TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_USER_REQUEST == 0);
    assert(TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_PROGRAMMATIC == 1);
    assert(TREELAND_WINE_WINDOW_STATE_V1_ACTIVATE_REASON_RESTORE == 2);

    /* Manager errors */
    assert(TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_TOPLEVEL_ALREADY_BOUND == 0);
    assert(TREELAND_WINE_WINDOW_STATE_MANAGER_V1_ERROR_DEFUNCT_TOPLEVEL == 1);
}

struct dummy_listener {
    struct wl_listener listener;
    int count;
};

static void dummy_view_changed_listener(struct wl_listener *listener, void *data)
{
    struct dummy_listener *d = wl_container_of(listener, d, listener);
    d->count++;
    (void)data;
}

static void test_view_attention(void)
{
    struct woodland_server server = {0};
    wl_signal_init(&server.view_changed);

    struct dummy_listener dl = {
        .count = 0,
    };
    dl.listener.notify = dummy_view_changed_listener;
    wl_signal_add(&server.view_changed, &dl.listener);

    struct woodland_view view = {
        .server = &server,
        .attention = false,
    };

    woodland_view_set_attention(&view, true);
    assert(view.attention == true);
    assert(dl.count == 1);

    /* No-op if unchanged */
    woodland_view_set_attention(&view, true);
    assert(dl.count == 1);

    woodland_view_set_attention(&view, false);
    assert(view.attention == false);
    assert(dl.count == 2);

    /* Activation clears attention automatically */
    woodland_view_set_attention(&view, true);
    assert(view.attention == true);
    woodland_view_set_activated(&view, true);
    assert(view.activated == true);
    assert(view.attention == false);

    wl_list_remove(&dl.listener.link);
}

static void test_status_and_query_null(void)
{
    struct woodland_json json;

    /* Wine Window Management status & query */
    woodland_json_init(&json);
    woodland_protocol_wine_wm_status(NULL, &json);
    assert(strstr(woodland_json_data(&json), "\"available\":false") != NULL);
    woodland_json_finish(&json);

    struct woodland_wine_wm_query_state wm_query;
    assert(!woodland_protocol_wine_wm_query_state(NULL, &wm_query));
    assert(!wm_query.available);

    /* Wine Window State status & query */
    woodland_json_init(&json);
    woodland_protocol_wine_state_status(NULL, &json);
    assert(strstr(woodland_json_data(&json), "\"available\":false") != NULL);
    woodland_json_finish(&json);

    struct woodland_wine_state_query_state ws_query;
    assert(!woodland_protocol_wine_state_query_state(NULL, &ws_query));
    assert(!ws_query.available);
}

int main(void)
{
    test_wm_enums();
    test_state_enums();
    test_view_attention();
    test_status_and_query_null();

    printf("test_wine_protocols: all tests passed\n");
    return 0;
}
