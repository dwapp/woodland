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

static void test_role_validity(void)
{
    /* Version 1 supports only NORMAL (0) and OVERLAY (1) */
    assert(woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_NORMAL, 1));
    assert(woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_OVERLAY, 1));
    assert(!woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP, 1));
    assert(!woodland_dde_shell_role_is_valid(3, 1));

    /* Version 2 adds ALWAYS_ON_TOP (2) */
    assert(woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_NORMAL, 2));
    assert(woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_OVERLAY, 2));
    assert(woodland_dde_shell_role_is_valid(WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP, 2));
    assert(!woodland_dde_shell_role_is_valid(3, 2));
    assert(!woodland_dde_shell_role_is_valid(100, 2));
}

static void test_skip_flags(void)
{
    assert(WOODLAND_DDE_SHELL_SKIP_FLAG_SWITCHER == 0x1);
    assert(WOODLAND_DDE_SHELL_SKIP_FLAG_DOCK_PREVIEW == 0x2);
    assert(WOODLAND_DDE_SHELL_SKIP_FLAG_MULTITASK_VIEW == 0x4);
}

static void test_view_layers(void)
{
    assert(woodland_view_get_layer(NULL) == WOODLAND_VIEW_LAYER_NORMAL);

    struct woodland_view view = {0};
    assert(woodland_view_get_layer(&view) == WOODLAND_VIEW_LAYER_NORMAL);

    view.always_on_top = true;
    assert(woodland_view_get_layer(&view) == WOODLAND_VIEW_LAYER_ALWAYS_ON_TOP);

    view.always_on_top = false;
    view.dde_shell.active = true;
    view.dde_shell.role = WOODLAND_DDE_SHELL_ROLE_NORMAL;
    assert(woodland_view_get_layer(&view) == WOODLAND_VIEW_LAYER_NORMAL);

    view.dde_shell.role = WOODLAND_DDE_SHELL_ROLE_ALWAYS_ON_TOP;
    assert(woodland_view_get_layer(&view) == WOODLAND_VIEW_LAYER_ALWAYS_ON_TOP);

    view.dde_shell.role = WOODLAND_DDE_SHELL_ROLE_OVERLAY;
    assert(woodland_view_get_layer(&view) == WOODLAND_VIEW_LAYER_OVERLAY);
}

static void test_status_null(void)
{
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_dde_shell_status(NULL, &json);
    assert(strstr(woodland_json_data(&json), "\"available\":false") != NULL);
    woodland_json_finish(&json);

    woodland_json_init(&json);
    woodland_protocol_dde_shell_view_status(NULL, &json);
    assert(strstr(woodland_json_data(&json), "\"active\":false") != NULL);
    woodland_json_finish(&json);
}

static void test_view_status_fields(void)
{
    struct woodland_view view = {0};
    view.dde_shell.active = true;
    view.dde_shell.role = WOODLAND_DDE_SHELL_ROLE_OVERLAY;
    view.dde_shell.has_position_hint = true;
    view.dde_shell.position_x = 100;
    view.dde_shell.position_y = 200;
    view.dde_shell.has_cursor_placement_hint = false;
    view.dde_shell.skip_flags = WOODLAND_DDE_SHELL_SKIP_FLAG_SWITCHER |
                                WOODLAND_DDE_SHELL_SKIP_FLAG_MULTITASK_VIEW;
    view.dde_shell.accept_keyboard_focus = false;

    struct woodland_json json;
    woodland_json_init(&json);
    woodland_protocol_dde_shell_view_status(&view, &json);

    const char *data = woodland_json_data(&json);
    assert(strstr(data, "\"active\":true") != NULL);
    assert(strstr(data, "\"role\":\"overlay\"") != NULL);
    assert(strstr(data, "\"has_position_hint\":true") != NULL);
    assert(strstr(data, "\"position\":[100,200]") != NULL);
    assert(strstr(data, "\"has_cursor_placement_hint\":false") != NULL);
    assert(strstr(data, "\"skip_flags\":5") != NULL);
    assert(strstr(data, "\"accept_keyboard_focus\":false") != NULL);
    woodland_json_finish(&json);
}

int main(void)
{
    test_role_validity();
    test_skip_flags();
    test_view_layers();
    test_status_null();
    test_view_status_fields();

    printf("test_dde_shell: all tests passed\n");
    return 0;
}
