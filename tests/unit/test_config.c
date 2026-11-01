// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "woodland_config.h"
#include "placement.h"
#include "util/json.h"
#include "woodland.h"

static void test_default_config(void) {
    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);
    assert(cfg->loaded_path == NULL);

    assert(cfg->general.placement_policy == WOODLAND_PLACE_CASCADE);
    assert(cfg->general.workspace_count == 10);
    assert(!cfg->general.focus_follows_mouse);
    assert(cfg->general.snap_distance == 20);

    assert(cfg->ssd.enabled);
    assert(cfg->ssd.titlebar_height == 26);
    assert(cfg->ssd.border_width == 1);
    assert(cfg->ssd.button_width == 26);
    assert(strcmp(cfg->ssd.font, "sans 10") == 0);
    assert(cfg->ssd.active_title_color == 0xFFE1DEDB);
    assert(cfg->ssd.inactive_title_color == 0xFFF6F5F4);

    assert(cfg->keyboard.layout == NULL);
    assert(cfg->keyboard.repeat_rate == 25);
    assert(cfg->keyboard.repeat_delay == 600);

    assert(!cfg->touchpad.tap_to_click);
    assert(!cfg->touchpad.natural_scroll);
    assert(cfg->touchpad.accel_speed == 0.0);
    assert(strcmp(cfg->touchpad.accel_profile, "adaptive") == 0);

    assert(strcmp(cfg->cursor.theme, "default") == 0);
    assert(cfg->cursor.size == 24);

    assert(!cfg->effects.enabled);
    assert(!cfg->effects.blur);
    assert(cfg->effects.corner_radius == 0);
    assert(!cfg->effects.shadow);

    assert(wl_list_empty(&cfg->output_rules));

    woodland_config_destroy(cfg);
}

static void test_parse_valid_toml(void) {
    const char *toml =
        "# Global config comment\n"
        "[general]\n"
        "placement_policy = \"center\"\n"
        "workspace_count = 8\n"
        "focus_policy = \"sloppy\"\n"
        "snap_distance = 35\n"
        "\n"
        "[decorations]\n"
        "enabled = false\n"
        "titlebar_height = 30\n"
        "border_width = 2\n"
        "button_width = 28\n"
        "font = \"monospace 11\"\n"
        "active_title_color = \"#123456\"\n"
        "inactive_title_color = \"#654321\"\n"
        "\n"
        "[input.keyboard]\n"
        "layout = \"us,fr\"\n"
        "variant = \",dvorak\"\n"
        "options = \"grp:alt_shift_toggle\"\n"
        "model = \"pc105\"\n"
        "repeat_rate = 35\n"
        "repeat_delay = 450\n"
        "\n"
        "[input.touchpad]\n"
        "tap_to_click = true\n"
        "natural_scroll = true\n"
        "accel_speed = 0.5\n"
        "accel_profile = \"flat\"\n"
        "\n"
        "[cursor]\n"
        "theme = \"Adwaita\"\n"
        "size = 32\n"
        "\n"
        "[effects]\n"
        "enabled = true\n"
        "blur = true\n"
        "corner_radius = 10\n"
        "shadow = true\n"
        "\n"
        "[[output]]\n"
        "name = \"eDP-1\"\n"
        "mode = \"1920x1080@60Hz\"\n"
        "scale = 1.25\n"
        "position = [0, 100]\n"
        "transform = \"90\"\n"
        "enabled = true\n"
        "\n"
        "[[output]]\n"
        "name = \"*\"\n"
        "scale = 2.0\n"
        "position = \"auto\"\n"
        "enabled = false\n";

    struct woodland_config *cfg = woodland_config_parse_string(toml, strlen(toml), "test_valid");
    assert(cfg != NULL);

    assert(cfg->general.placement_policy == WOODLAND_PLACE_CENTER);
    assert(cfg->general.placement_specified);
    assert(cfg->general.workspace_count == 8);
    assert(cfg->general.focus_follows_mouse);
    assert(cfg->general.snap_distance == 35);

    assert(!cfg->ssd.enabled);
    assert(cfg->ssd.titlebar_height == 30);
    assert(cfg->ssd.border_width == 2);
    assert(cfg->ssd.button_width == 28);
    assert(strcmp(cfg->ssd.font, "monospace 11") == 0);
    assert(cfg->ssd.active_title_color == 0xFF123456);
    assert(cfg->ssd.inactive_title_color == 0xFF654321);

    assert(strcmp(cfg->keyboard.layout, "us,fr") == 0);
    assert(strcmp(cfg->keyboard.variant, ",dvorak") == 0);
    assert(strcmp(cfg->keyboard.options, "grp:alt_shift_toggle") == 0);
    assert(strcmp(cfg->keyboard.model, "pc105") == 0);
    assert(cfg->keyboard.repeat_rate == 35);
    assert(cfg->keyboard.repeat_delay == 450);

    assert(cfg->touchpad.tap_to_click);
    assert(cfg->touchpad.natural_scroll);
    assert(cfg->touchpad.accel_speed == 0.5);
    assert(strcmp(cfg->touchpad.accel_profile, "flat") == 0);

    assert(strcmp(cfg->cursor.theme, "Adwaita") == 0);
    assert(cfg->cursor.size == 32);

    assert(cfg->effects.enabled);
    assert(cfg->effects.blur);
    assert(cfg->effects.corner_radius == 10);
    assert(cfg->effects.shadow);

    /* 验证输出规则 */
    assert(wl_list_length(&cfg->output_rules) == 2);
    struct woodland_config_output_rule *r1 =
        wl_container_of(cfg->output_rules.next, r1, link);
    assert(strcmp(r1->name, "eDP-1") == 0);
    assert(strcmp(r1->mode, "1920x1080@60Hz") == 0);
    assert(r1->scale == 1.25);
    assert(!r1->auto_position);
    assert(r1->x == 0 && r1->y == 100);
    assert(r1->transform == WL_OUTPUT_TRANSFORM_90);
    assert(r1->enabled);

    struct woodland_config_output_rule *r2 =
        wl_container_of(r1->link.next, r2, link);
    assert(strcmp(r2->name, "*") == 0);
    assert(r2->scale == 2.0);
    assert(r2->auto_position);
    assert(!r2->enabled);

    woodland_config_destroy(cfg);
}

static void test_malformed_fallback(void) {
    const char *bad_toml =
        "[general\n"
        "placement_policy = == 12345\n";

    /* 语法错误不得崩溃，回退至内置默认配置 */
    struct woodland_config *cfg = woodland_config_parse_string(bad_toml, strlen(bad_toml), "bad_test");
    assert(cfg != NULL);
    assert(cfg->general.placement_policy == WOODLAND_PLACE_CASCADE);
    assert(cfg->general.workspace_count == 10);
    assert(cfg->ssd.titlebar_height == 26);
    woodland_config_destroy(cfg);
}

static void test_boundary_clamping(void) {
    const char *toml =
        "[general]\n"
        "workspace_count = 9999\n"
        "snap_distance = -10\n"
        "[decorations]\n"
        "titlebar_height = -5\n"
        "border_width = 1000\n"
        "[input.touchpad]\n"
        "accel_speed = 5.0\n";

    struct woodland_config *cfg = woodland_config_parse_string(toml, strlen(toml), "clamp_test");
    assert(cfg != NULL);
    /* 超出 1..12 范围被忽略，保留默认 10 */
    assert(cfg->general.workspace_count == 10);
    /* 负数被忽略，保留默认 20 */
    assert(cfg->general.snap_distance == 20);
    /* 非法高度/边框被忽略，保留默认 26/1 */
    assert(cfg->ssd.titlebar_height == 26);
    assert(cfg->ssd.border_width == 1);
    /* 加速敏感度限制在 [-1.0, 1.0] */
    assert(cfg->touchpad.accel_speed == 1.0);

    woodland_config_destroy(cfg);
}

static void test_json_dump(void) {
    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);

    struct woodland_json json;
    woodland_json_init(&json);
    woodland_config_dump_json(cfg, &json);
    assert(woodland_json_ok(&json));

    const char *data = woodland_json_data(&json);
    assert(strstr(data, "\"general\":") != NULL);
    assert(strstr(data, "\"placement_policy\":\"cascade\"") != NULL);
    assert(strstr(data, "\"workspace_count\":10") != NULL);
    assert(strstr(data, "\"decorations\":") != NULL);
    assert(strstr(data, "\"titlebar_height\":26") != NULL);
    assert(strstr(data, "\"keyboard\":") != NULL);
    assert(strstr(data, "\"cursor\":") != NULL);

    woodland_json_finish(&json);
    woodland_config_destroy(cfg);
}

static void test_hot_reload_file(void) {
    char tmppath[] = "/tmp/woodland_test_config_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *init_content =
        "[general]\n"
        "placement_policy = \"cursor\"\n"
        "workspace_count = 5\n";
    assert(write(fd, init_content, strlen(init_content)) == (ssize_t)strlen(init_content));
    close(fd);

    /* 首次装载 */
    struct woodland_config *cfg = woodland_config_load(tmppath);
    assert(cfg != NULL);
    assert(cfg->loaded_path != NULL);
    assert(strcmp(cfg->loaded_path, tmppath) == 0);
    assert(cfg->general.placement_policy == WOODLAND_PLACE_CURSOR);
    assert(cfg->general.workspace_count == 5);

    /* 模拟服务端持有配置 */
    struct woodland_server server = {0};
    wl_list_init(&server.keyboards);
    wl_list_init(&server.views);
    woodland_config_apply(&server, cfg);
    assert(server.config == cfg);
    assert(server.placement_policy == WOODLAND_PLACE_CURSOR);

    /* 修改文件内容 */
    FILE *f = fopen(tmppath, "wb");
    assert(f != NULL);
    const char *updated_content =
        "[general]\n"
        "placement_policy = \"autoplace\"\n"
        "workspace_count = 7\n";
    assert(fwrite(updated_content, 1, strlen(updated_content), f) == strlen(updated_content));
    fclose(f);

    /* 热重载 */
    bool reloaded = woodland_config_reload(&server);
    assert(reloaded);
    assert(server.config != NULL);
    assert(server.config->general.placement_policy == WOODLAND_PLACE_AUTOPLACE);
    assert(server.config->general.workspace_count == 7);
    assert(server.placement_policy == WOODLAND_PLACE_AUTOPLACE);

    /* 写入语法错误内容：验证 Phase 1 拒绝破坏当前状态 */
    f = fopen(tmppath, "wb");
    assert(f != NULL);
    const char *bad_content = "[general\ninvalid = = =\n";
    assert(fwrite(bad_content, 1, strlen(bad_content), f) == strlen(bad_content));
    fclose(f);

    bool reload_bad = woodland_config_reload(&server);
    assert(!reload_bad);
    /* 当前配置和策略严格保持不变 */
    assert(server.config->general.placement_policy == WOODLAND_PLACE_AUTOPLACE);
    assert(server.config->general.workspace_count == 7);
    assert(server.placement_policy == WOODLAND_PLACE_AUTOPLACE);

    woodland_config_destroy(server.config);
    unlink(tmppath);
}

int main(void) {
    test_default_config();
    test_parse_valid_toml();
    test_malformed_fallback();
    test_boundary_clamping();
    test_json_dump();
    test_hot_reload_file();

    printf("test_config: all assertions passed\n");
    return 0;
}
