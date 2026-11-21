// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>

#include "output.h"
#include "output_config.h"
#include "scene.h"
#include "woodland.h"

int main(void)
{
    char tmp_dir[] = "/tmp/woodland-test-output-config-XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);
    setenv("XDG_CONFIG_HOME", tmp_dir, 1);

    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();

    assert(woodland_output_init(&server));

    /* 1. 空配置加载应返回 false */
    struct woodland_saved_outputs list = {0};
    assert(!woodland_saved_outputs_load(&list));

    /* 2. 创建一个 headless 输出 */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 验证输出已进入链表和布局 */
    struct woodland_output *w_out = out->data;
    assert(w_out != NULL);
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server.output_layout, out, &box);
    assert(box.width == 800 && box.height == 600);

    /* 3. 保存配置到 outputs.json */
    woodland_output_config_save(&server);

    /* 4. 读取验证 outputs.json */
    assert(woodland_saved_outputs_load(&list));
    assert(list.count == 1);
    const struct woodland_saved_output *saved = woodland_saved_outputs_find(&list, out->name);
    assert(saved != NULL);
    assert(saved->enabled == true);
    assert(saved->width == 800);
    assert(saved->height == 600);
    assert(saved->scale == 1.0);
    assert(saved->x == box.x && saved->y == box.y);
    woodland_saved_outputs_finish(&list);

    /* 5. 模拟修改保存配置（例如缩放 1.5，坐标 100, 200，分辨率 1024x768） */
    FILE *f = fopen("/tmp/test_outputs.json", "w");
    assert(f != NULL);
    fprintf(f, "[\n  {\n    \"name\": \"%s\",\n    \"enabled\": true,\n"
               "    \"x\": 100,\n    \"y\": 200,\n    \"width\": 1024,\n"
               "    \"height\": 768,\n    \"refresh\": 60000,\n"
               "    \"scale\": 1.500,\n    \"transform\": 0,\n    \"adaptive_sync\": false\n"
               "  },\n"
               "  {\n    \"name\": \"OFFLINE-1\",\n    \"enabled\": true,\n"
               "    \"x\": 1124,\n    \"y\": 200,\n    \"width\": 1920,\n"
               "    \"height\": 1080,\n    \"refresh\": 144000,\n"
               "    \"scale\": 2.000,\n    \"transform\": 1,\n    \"adaptive_sync\": true\n"
               "  }\n]\n", out->name);
    fclose(f);

    char dest_path[512];
    snprintf(dest_path, sizeof(dest_path), "%s/woodland/outputs.json", tmp_dir);
    assert(rename("/tmp/test_outputs.json", dest_path) == 0);

    /* 6. 测试多屏合并保存：当前只有 out 在线，保存后不应丢弃 OFFLINE-1 */
    woodland_output_config_save(&server);

    assert(woodland_saved_outputs_load(&list));
    assert(list.count == 2);
    const struct woodland_saved_output *offline = woodland_saved_outputs_find(&list, "OFFLINE-1");
    assert(offline != NULL);
    assert(offline->width == 1920 && offline->height == 1080);
    assert(offline->refresh == 144000);
    assert(offline->scale == 2.0);
    assert(offline->transform == 1);
    assert(offline->adaptive_sync == true);
    woodland_saved_outputs_finish(&list);

    /* 7. 测试恢复函数 woodland_output_config_restore */
    struct wlr_output_state restore_state;
    wlr_output_state_init(&restore_state);
    int32_t rx = 0, ry = 0;
    bool restored = woodland_output_config_restore(w_out, &restore_state, &rx, &ry);
    assert(restored == true);
    assert(rx == 0); /* 当前 out 保存的值是 0 */
    assert(ry == 0);
    wlr_output_state_finish(&restore_state);

    /* 销毁测试资源 */
    wlr_output_destroy(out);
    woodland_output_finish(&server);
    wlr_scene_node_destroy(&server.scene->tree.node);
    server.scene = NULL;
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    /* 清理临时目录 */
    unlink(dest_path);
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/woodland", tmp_dir);
    rmdir(dir_path);
    rmdir(tmp_dir);

    return 0;
}
