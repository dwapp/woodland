// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "placement.h"
#include "util/json.h"
#include "woodland.h"
#include "woodland_config.h"

static void test_default_config(void)
{
    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);
    assert(cfg->loaded_path == NULL);

    assert(cfg->general.placement_policy == WOODLAND_PLACE_CASCADE);
    assert(cfg->general.workspace_count == 10);
    assert(!cfg->general.focus_follows_mouse);
    assert(cfg->general.snap_distance == 20);
    assert(!cfg->general.allow_tearing);
    assert(cfg->general.snap_preview);
    /* 预览样式：天蓝、边框 75% alpha、填充 25%、边框 2px */
    assert(cfg->general.snap_preview_color == 0xBF0080FF);
    assert(!cfg->general.snap_preview_color_specified);
    assert(cfg->general.snap_preview_opacity == 0.25);
    assert(cfg->general.snap_preview_border_width == 2);

    assert(cfg->ssd.enabled);
    assert(cfg->ssd.titlebar_height == 32);
    assert(cfg->ssd.border_width == 1);
    assert(cfg->ssd.button_width == 32);
    assert(cfg->ssd.resize_grab_width == 8);
    assert(strcmp(cfg->ssd.font, "sans 10") == 0);
    assert(cfg->ssd.active_title_color == 0xFFF4F4F4);
    assert(cfg->ssd.inactive_title_color == 0xFFF9F9F9);

    assert(cfg->keyboard.layout == NULL);
    assert(cfg->keyboard.repeat_rate == 25);
    assert(cfg->keyboard.repeat_delay == 600);

    assert(!cfg->touchpad.tap_to_click);
    assert(!cfg->touchpad.natural_scroll);
    assert(cfg->touchpad.accel_speed == 0.0);
    assert(strcmp(cfg->touchpad.accel_profile, "adaptive") == 0);

    assert(strcmp(cfg->cursor.theme, "default") == 0);
    assert(cfg->cursor.size == 24);

    /* 特效是运行期开关：默认开着，用户想关才关 */
    assert(cfg->effects.enabled);
    assert(cfg->effects.blur);
    assert(!cfg->effects.blur_optimized);
    assert(cfg->effects.blur_passes == 3);
    assert(cfg->effects.blur_radius == 5);
    assert(cfg->effects.corner_radius == 4);
    assert(!cfg->effects.shadow);
    assert(cfg->effects.shadow_radius == 20);
    assert(cfg->effects.shadow_offset_y == 4);
    assert(wl_list_empty(&cfg->output_rules));
    assert(cfg->xwayland.scale_filter == WOODLAND_SCALE_FILTER_SMART);
    assert(cfg->xwayland.native_resolution);
    assert(cfg->xwayland.xsettings);

    woodland_config_destroy(cfg);
}

static void test_parse_valid_toml(void)
{
    const char *toml = "# Global config comment\n"
                       "[general]\n"
                       "placement_policy = \"center\"\n"
                       "workspace_count = 8\n"
                       "focus_policy = \"sloppy\"\n"
                       "snap_distance = 35\n"
                       "allow_tearing = true\n"
                       "snap_preview = false\n"
                       "snap_preview_color = \"#12345678\"\n"
                       "snap_preview_opacity = 0.5\n"
                       "snap_preview_border_width = 4\n"
                       "\n"
                       "[decorations]\n"
                       "enabled = false\n"
                       "border_width = 2\n"
                       "button_width = 28\n"
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
                       "# theme / size 已迁移到 appearance.json，这里不再解析\n"
                       "\n"
                       "[effects]\n"
                       "enabled = true\n"
                       "blur = true\n"
                       "shadow = true\n"
                       "blur_optimized = true\n"
                       "blur_passes = 4\n"
                       "blur_radius = 8\n"
                       "shadow_radius = 25\n"
                       "shadow_offset_y = 6\n"
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
                       "enabled = false\n"
                       "\n"
                       "[xwayland]\n"
                       "scale_filter = \"nearest\"\n";

    struct woodland_config *cfg = woodland_config_parse_string(toml, strlen(toml), "test_valid");
    assert(cfg != NULL);

    assert(cfg->general.placement_policy == WOODLAND_PLACE_CENTER);
    assert(cfg->general.placement_specified);
    assert(cfg->general.workspace_count == 8);
    assert(cfg->general.focus_follows_mouse);
    assert(cfg->general.snap_distance == 35);
    assert(cfg->general.allow_tearing);
    assert(!cfg->general.snap_preview);
    /* #RRGGBBAA：文件里 alpha 在最后，落进结构体时按 0xAARRGGBB 存放
     * （与 decorations 的 *_color 同一口径） */
    assert(cfg->general.snap_preview_color == 0x78123456);
    assert(cfg->general.snap_preview_color_specified);
    assert(cfg->general.snap_preview_opacity == 0.5);
    assert(cfg->general.snap_preview_border_width == 4);

    assert(!cfg->ssd.enabled);
    assert(cfg->ssd.titlebar_height == 32); /* 已迁移到 appearance.json */
    assert(cfg->ssd.border_width == 2);
    assert(cfg->ssd.button_width == 28);
    assert(strcmp(cfg->ssd.font, "sans 10") == 0); /* 已迁移到 appearance.json */
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

    assert(strcmp(cfg->cursor.theme, "default") == 0); /* 已迁移到 appearance.json */
    assert(cfg->cursor.size == 24);                      /* 已迁移到 appearance.json */

    assert(cfg->effects.enabled);
    assert(cfg->effects.blur);
    assert(cfg->effects.corner_radius == 4); /* 已迁移到 appearance.json */
    assert(cfg->effects.shadow);
    assert(cfg->effects.blur_optimized);
    assert(cfg->effects.blur_passes == 4);
    assert(cfg->effects.blur_radius == 8);
    assert(cfg->effects.shadow_radius == 25);
    assert(cfg->effects.shadow_offset_y == 6);

    assert(cfg->xwayland.scale_filter == WOODLAND_SCALE_FILTER_NEAREST);
    /* 验证输出规则 */
    assert(wl_list_length(&cfg->output_rules) == 2);
    struct woodland_config_output_rule *r1 = wl_container_of(cfg->output_rules.next, r1, link);
    assert(strcmp(r1->name, "eDP-1") == 0);
    assert(strcmp(r1->mode, "1920x1080@60Hz") == 0);
    assert(r1->scale == 1.25);
    assert(!r1->auto_position);
    assert(r1->x == 0 && r1->y == 100);
    assert(r1->transform == WL_OUTPUT_TRANSFORM_90);
    assert(r1->enabled);

    struct woodland_config_output_rule *r2 = wl_container_of(r1->link.next, r2, link);
    assert(strcmp(r2->name, "*") == 0);
    assert(r2->scale == 2.0);
    assert(r2->auto_position);
    assert(!r2->enabled);

    woodland_config_destroy(cfg);
}
static void test_xwayland_config(void)
{
    const char *toml_smart = "[xwayland]\nscale_filter = \"smart\"\n";
    struct woodland_config *c1 = woodland_config_parse_string(toml_smart, strlen(toml_smart), "t1");
    assert(c1 && c1->xwayland.scale_filter == WOODLAND_SCALE_FILTER_SMART);
    woodland_config_destroy(c1);

    const char *toml_linear = "[xwayland]\nscale_filter = \"linear\"\n";
    struct woodland_config *c2 = woodland_config_parse_string(toml_linear, strlen(toml_linear), "t2");
    assert(c2 && c2->xwayland.scale_filter == WOODLAND_SCALE_FILTER_BILINEAR);
    woodland_config_destroy(c2);

    const char *toml_bilinear = "[xwayland]\nscale_filter = \"bilinear\"\n";
    struct woodland_config *c3 = woodland_config_parse_string(toml_bilinear, strlen(toml_bilinear), "t3");
    assert(c3 && c3->xwayland.scale_filter == WOODLAND_SCALE_FILTER_BILINEAR);
    woodland_config_destroy(c3);

    const char *toml_nearest = "[xwayland]\nscale_filter = \"nearest\"\n"
                               "native_resolution = false\nxsettings = false\n";
    struct woodland_config *c4 = woodland_config_parse_string(toml_nearest, strlen(toml_nearest), "t4");
    assert(c4 && c4->xwayland.scale_filter == WOODLAND_SCALE_FILTER_NEAREST);
    assert(!c4->xwayland.native_resolution);
    assert(!c4->xwayland.xsettings);

    struct woodland_json j;
    woodland_json_init(&j);
    woodland_config_dump_json(c4, &j);
    assert(woodland_json_ok(&j));
    const char *js = woodland_json_data(&j);
    assert(strstr(js, "\"native_resolution\":false") != NULL);
    assert(strstr(js, "\"xsettings\":false") != NULL);
    woodland_json_finish(&j);
    woodland_config_destroy(c4);

    const char *toml_native = "[xwayland]\nnative_resolution = true\nxsettings = true\n";
    struct woodland_config *c5 = woodland_config_parse_string(toml_native, strlen(toml_native), "t5");
    assert(c5 && c5->xwayland.native_resolution);
    assert(c5->xwayland.xsettings);
    woodland_config_destroy(c5);
}


static void test_malformed_fallback(void)
{
    const char *bad_toml = "[general\n"
                           "placement_policy = == 12345\n";

    /* 语法错误不得崩溃，回退至内置默认配置 */
    struct woodland_config *cfg =
        woodland_config_parse_string(bad_toml, strlen(bad_toml), "bad_test");
    assert(cfg != NULL);
    assert(cfg->general.placement_policy == WOODLAND_PLACE_CASCADE);
    assert(cfg->general.workspace_count == 10);
    assert(cfg->ssd.titlebar_height == 32);
    woodland_config_destroy(cfg);
}

static void test_boundary_clamping(void)
{
    const char *toml = "[general]\n"
                       "workspace_count = 9999\n"
                       "snap_distance = -10\n"
                       "snap_preview_color = \"nonsense\"\n"
                       "snap_preview_opacity = 5.0\n"
                       "snap_preview_border_width = 999\n"
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
    /* 颜色写错 = 没写（不能既改色又装作"用户指定过"）；超范围的不透明度
     * 与边框宽度一律忽略，保留默认 0.25 / 2 */
    assert(cfg->general.snap_preview_color == 0xBF0080FF);
    assert(!cfg->general.snap_preview_color_specified);
    assert(cfg->general.snap_preview_opacity == 0.25);
    assert(cfg->general.snap_preview_border_width == 2);
    /* titlebar_height 已迁移到 appearance.json，这里即使写了 -5 也直接不解析；
     * 非法 border_width 被忽略，保留默认 1 */
    assert(cfg->ssd.titlebar_height == 32);
    assert(cfg->ssd.border_width == 1);
    /* 加速敏感度限制在 [-1.0, 1.0] */
    assert(cfg->touchpad.accel_speed == 1.0);

    woodland_config_destroy(cfg);
}
static void test_invalid_syntax_checks(void)
{
    /* 未知转义字符测试 */
    const char *bad_escape = "[general]\nplacement_policy = \"\\q\"\n";
    struct woodland_config *cfg1 =
        woodland_config_parse_string(bad_escape, strlen(bad_escape), "bad_escape");
    assert(cfg1 != NULL);
    assert(!cfg1->general.placement_specified);
    woodland_config_destroy(cfg1);

    /* Section 头部后跟随非法字符 */
    const char *bad_header1 = "[general] unexpected\nworkspace_count = 5\n";
    struct woodland_config *cfg2 =
        woodland_config_parse_string(bad_header1, strlen(bad_header1), "bad_header1");
    assert(cfg2 != NULL);
    assert(cfg2->general.workspace_count == 10);
    woodland_config_destroy(cfg2);

    /* 数组表头部后跟随非法字符 */
    const char *bad_header2 = "[[output]] trailing\nname = \"eDP-1\"\n";
    struct woodland_config *cfg3 =
        woodland_config_parse_string(bad_header2, strlen(bad_header2), "bad_header2");
    assert(cfg3 != NULL);
    assert(wl_list_empty(&cfg3->output_rules));
    woodland_config_destroy(cfg3);

    /* 非 NUL 结尾的有界缓冲区解析 */
    char bounded_buf[128];
    const char *valid_snippet = "[general]\nplacement_policy = \"center\"\n";
    size_t snippet_len = strlen(valid_snippet);
    memcpy(bounded_buf, valid_snippet, snippet_len);
    memset(bounded_buf + snippet_len, 'X', sizeof(bounded_buf) - snippet_len);
    struct woodland_config *cfg4 =
        woodland_config_parse_string(bounded_buf, snippet_len, "bounded");
    assert(cfg4 != NULL);
    assert(cfg4->general.placement_policy == WOODLAND_PLACE_CENTER);
    woodland_config_destroy(cfg4);
}

static void test_json_dump(void)
{
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
    assert(strstr(data, "\"titlebar_height\":32") != NULL);
    /* 预览样式也要能被 wctl get_config 看到（颜色 0xAARRGGBB、不透明度毫值） */
    assert(strstr(data, "\"snap_preview_color\":3204481279") != NULL);
    assert(strstr(data, "\"snap_preview_opacity_milli\":250") != NULL);
    assert(strstr(data, "\"keyboard\":") != NULL);
    assert(strstr(data, "\"cursor\":") != NULL);
    assert(strstr(data, "\"active_border_color\":") != NULL);
    assert(strstr(data, "\"inactive_border_color\":") != NULL);
    assert(strstr(data, "\"native_resolution\":true") != NULL);
    assert(strstr(data, "\"xsettings\":true") != NULL);
    woodland_json_finish(&json);
    woodland_config_destroy(cfg);
}

static void test_hot_reload_file(void)
{
    char tmppath[] = "/tmp/woodland_test_config_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *init_content = "[general]\n"
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
    const char *updated_content = "[general]\n"
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

    /* 移除 placement_policy 恢复默认级联策略 */
    f = fopen(tmppath, "wb");
    assert(f != NULL);
    const char *no_placement = "[general]\nworkspace_count = 3\n";
    assert(fwrite(no_placement, 1, strlen(no_placement), f) == strlen(no_placement));
    fclose(f);

    bool reload_restore = woodland_config_reload(&server);
    assert(reload_restore);
    assert(server.config->general.placement_policy == WOODLAND_PLACE_CASCADE);
    assert(server.placement_policy == WOODLAND_PLACE_CASCADE);

    woodland_config_destroy(server.config);
    unlink(tmppath);
}

static void test_appearance_persist_roundtrip(void)
{
    /* 用临时 XDG_CONFIG_HOME 隔离，避免污染真实配置目录 */
    char tmpdir[] = "/tmp/woodland_appearance_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);
    char config_home[512];
    snprintf(config_home, sizeof(config_home), "%s/config", dir);

    const char *old_home = getenv("XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", config_home, 1);

    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);

    /* 模拟 treeland-appearance 的 set_*：改 11 项里的代表字段 */
    free(cfg->cursor.theme);
    cfg->cursor.theme = strdup("Adwaita");
    cfg->cursor.size = 32;
    free(cfg->ssd.font);
    cfg->ssd.font = strdup("Noto Sans Bold 12");
    cfg->ssd.titlebar_height = 30;
    cfg->effects.corner_radius = 12;
    cfg->appearance.accent_set = true;
    cfg->appearance.accent_rgb = 0xFF0000;
    cfg->appearance.color_scheme_set = true;
    cfg->appearance.dark = true;
    free(cfg->appearance.icon_theme);
    cfg->appearance.icon_theme = strdup("Papirus");
    cfg->appearance.monospace_font = strdup("DejaVu Sans Mono");
    cfg->appearance.opacity_set = true;
    cfg->appearance.window_opacity = 0.5;

    assert(woodland_config_appearance_save(cfg));

    /* 用一个全新的默认 config 读回，验证恢复 */
    struct woodland_config *restored = woodland_config_create_default();
    assert(restored != NULL);
    woodland_config_appearance_overlay(restored);

    assert(strcmp(restored->cursor.theme, "Adwaita") == 0);
    assert(restored->cursor.size == 32);
    assert(strcmp(restored->ssd.font, "Noto Sans Bold 12") == 0);
    assert(restored->ssd.titlebar_height == 30);
    assert(restored->effects.corner_radius == 12);
    assert(restored->appearance.accent_set);
    assert(restored->appearance.accent_rgb == 0xFF0000);
    assert(restored->appearance.color_scheme_set);
    assert(restored->appearance.dark);
    assert(strcmp(restored->appearance.icon_theme, "Papirus") == 0);
    assert(strcmp(restored->appearance.monospace_font, "DejaVu Sans Mono") == 0);
    assert(restored->appearance.opacity_set);
    assert(restored->appearance.window_opacity == 0.5);

    woodland_config_destroy(restored);
    woodland_config_destroy(cfg);

    /* 清理 */
    unlink(config_home); /* 不存在的文件忽略 */
    char wp[700];
    snprintf(wp, sizeof(wp), "%s/woodland/appearance.json", config_home);
    unlink(wp);
    snprintf(wp, sizeof(wp), "%s/woodland", config_home);
    rmdir(wp);
    rmdir(config_home);
    rmdir(dir);

    if (old_home) {
        setenv("XDG_CONFIG_HOME", old_home, 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
}

/* appearance.json 的绝对路径写入位置：$XDG_CONFIG_HOME/woodland/appearance.json */
static void appearance_test_config_home(char *buf, size_t size, const char *tmpdir)
{
    snprintf(buf, size, "%s/config", tmpdir);
}

static void appearance_test_write_raw(const char *config_home, const char *content)
{
    char dir[700];
    snprintf(dir, sizeof(dir), "%s/woodland", config_home);
    /* mkdir -p（与 appearance_persist.c 一致） */
    char p[700];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            mkdir(p, 0755);
            *c = '/';
        }
    }
    mkdir(p, 0755);
    char path[760];
    snprintf(path, sizeof(path), "%s/appearance.json", dir);
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void appearance_test_cleanup(const char *tmpdir, const char *old_home)
{
    char config_home[512];
    appearance_test_config_home(config_home, sizeof(config_home), tmpdir);
    char wp[700];
    snprintf(wp, sizeof(wp), "%s/woodland/appearance.json", config_home);
    unlink(wp);
    snprintf(wp, sizeof(wp), "%s/woodland", config_home);
    rmdir(wp);
    rmdir(config_home);
    rmdir(tmpdir);
    if (old_home) {
        setenv("XDG_CONFIG_HOME", old_home, 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
}

/* 未设过任何值的默认态 round-trip：标志位应保持 false，不被数值键误翻 */
static void test_appearance_persist_unset_roundtrip(void)
{
    char tmpdir[] = "/tmp/woodland_appearance_unset_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);
    char config_home[512];
    appearance_test_config_home(config_home, sizeof(config_home), dir);
    const char *old_home = getenv("XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", config_home, 1);

    /* 默认配置：什么都没设 */
    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);
    assert(!cfg->appearance.accent_set);
    assert(!cfg->appearance.opacity_set);
    assert(!cfg->appearance.color_scheme_set);
    assert(cfg->appearance.icon_theme == NULL);
    assert(cfg->appearance.monospace_font == NULL);
    assert(woodland_config_appearance_save(cfg));

    struct woodland_config *restored = woodland_config_create_default();
    assert(restored != NULL);
    woodland_config_appearance_overlay(restored);

    /* 关键：数值键 0 / 0.000 不应翻转标志位 */
    assert(!restored->appearance.accent_set);
    assert(restored->appearance.accent_rgb == 0);
    assert(!restored->appearance.opacity_set);
    assert(restored->appearance.window_opacity == 0.0);
    assert(!restored->appearance.color_scheme_set);
    assert(!restored->appearance.dark);
    assert(restored->appearance.icon_theme == NULL);
    assert(restored->appearance.monospace_font == NULL);

    woodland_config_destroy(restored);
    woodland_config_destroy(cfg);
    appearance_test_cleanup(dir, old_home);
}

/* 空字符串 / null 回落、越界 clamp、malformed JSON 回退默认 */
static void test_appearance_persist_edge_cases(void)
{
    char tmpdir[] = "/tmp/woodland_appearance_edge_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);
    char config_home[512];
    appearance_test_config_home(config_home, sizeof(config_home), dir);
    const char *old_home = getenv("XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", config_home, 1);

    /* 1. 空字符串回落 + 越界 clamp + null 回落 */
    appearance_test_write_raw(config_home,
                              "{\"cursor_theme\":\"\",\"cursor_size\":999,"
                              "\"titlebar_height\":999,\"corner_radius\":9999,"
                              "\"icon_theme\":\"\",\"monospace_font\":null,"
                              "\"window_opacity\":2.5,\"opacity_set\":true,"
                              "\"accent_set\":true,\"accent_rgb\":16777215}");
    struct woodland_config *cfg = woodland_config_create_default();
    assert(cfg != NULL);
    woodland_config_appearance_overlay(cfg);

    /* 空串 cursor_theme 不应用（保持默认，协议层永不会写出空串） */
    assert(strcmp(cfg->cursor.theme, "default") == 0);
    assert(cfg->cursor.size == 256);                 /* clamp 到上限 */
    assert(cfg->ssd.titlebar_height == 200);          /* clamp 到上限 */
    assert(cfg->effects.corner_radius == WOODLAND_SSD_MAX_SIZE); /* clamp */
    assert(cfg->appearance.icon_theme == NULL);       /* 空串回落 NULL */
    assert(cfg->appearance.monospace_font == NULL);   /* null 回落 NULL */
    assert(cfg->appearance.opacity_set);
    assert(cfg->appearance.window_opacity == 1.0);    /* 2.5 clamp 到 1.0 */
    assert(cfg->appearance.accent_set);
    assert(cfg->appearance.accent_rgb == 0xFFFFFF);   /* 16777215 = 0xFFFFFF */
    woodland_config_destroy(cfg);

    /* 2. malformed JSON：回退当前值（全新默认） */
    appearance_test_write_raw(config_home, "{ this is not valid json");
    cfg = woodland_config_create_default();
    assert(cfg != NULL);
    woodland_config_appearance_overlay(cfg);
    /* 保持默认 */
    assert(cfg->cursor.size == 24);
    assert(cfg->ssd.titlebar_height == 32);
    assert(cfg->effects.corner_radius == 4);
    assert(!cfg->appearance.accent_set);
    assert(!cfg->appearance.opacity_set);
    woodland_config_destroy(cfg);

    appearance_test_cleanup(dir, old_home);
}

int main(void)
{
    test_default_config();
    test_parse_valid_toml();
    test_malformed_fallback();
    test_boundary_clamping();
    test_invalid_syntax_checks();
    test_json_dump();
    test_hot_reload_file();
    test_xwayland_config();
    test_appearance_persist_roundtrip();
    test_appearance_persist_unset_roundtrip();
    test_appearance_persist_edge_cases();

    printf("test_config: all assertions passed\n");
    return 0;
}
