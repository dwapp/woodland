# layer-shell 支持实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 woodland 支持 `zwlr_layer_shell_v1` 协议——面板/壁纸/启动器等客户端按 background/bottom/top/overlay 四层停靠，exclusive zone 缩小 maximize 可用区域，keyboard interactivity 参与焦点流转。

**架构：** 新增 `src/layer/` 模块（静态库 `woodland-layer`）。每个输出持有 4 棵 layer tree（挂根 scene、与工作区树平级），z-order `background→bottom→工作区→top→overlay`。arrange 复用 wlroots `wlr_scene_layer_surface_v1_configure` 计算锚点/边距/exclusive zone，结果写回 `output->usable_area` 供 maximize 使用。

**技术栈：** C11 + wlroots 0.20.2（`wlr_layer_shell_v1`、`wlr_scene_layer_surface_v1`）。

**规格：** `docs/superpowers/specs/2026-10-26-layer-shell-design.md`

## 全局约束

- 纯 C（C11），`-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L`，`warning_level=2` + `werror=true`（警告即错误）。
- 新源文件顶部 `// SPDX-License-Identifier: GPL-2.0-only`（版权行省略）。
- 资源逆序释放、每步判空置空（幂等），不 double-free、不 use-after-free。
- 单实例 `struct woodland_server`，模块间指针传递。
- 保持最小 diff：只碰与任务相关的文件。
- layer-shell 版本用 **4**（与 labwc 一致；wlroots 0.20.2 最高支持 5）。
- 不新增依赖、不 vendored。
- 参照 labwc `src/layers.c`、`src/output.c`（本机 `/home/ut004971@uos/work/labwc`）。

## 审查重点（Review Focus）

1. **layer surface 销毁时 listener 未摘除 → UAF**：node_destroy 主清理点必须一次性摘除全部 6 个 listener + free，且 `scene_layer_surface` 已可能被 wlroots 释放须置 NULL。正常人期待：客户端断开/关闭面板不崩溃。
2. **工作区切换清掉 layer surface 焦点**：`woodland_workspace_switch` 的 clear_focus 若不清除 focus 而只对 xdg view 生效，layer surface（面板/启动器）应保持焦点。正常人期待：面板跨工作区常驻且输入不被打断。
3. **exclusive zone 扣减后 usable_area 变负**：多个面板重叠占位时 usable 宽/高可为负，须 clamp（wlroots `layer_surface_exclusive_zone` 已 clamp，arrange 依赖它）。正常人期待：极端占位不产生负几何。
4. **layer surface 无输出时的分配与输出销毁**：new_surface 时 output 为 NULL 须分配 fallback；输出销毁时该 layer surface 一并销毁。正常人期待：拔显示器不残留悬挂 surface。
5. **maximize 用 usable_area、fullscreen 仍用完整 box**：maximize 须避开面板，fullscreen 须盖过面板。正常人期待：全屏视频盖住面板，最大化窗口贴着面板边缘。

---

### 任务 1：layer 模块骨架 + 每输出 4 棵 layer tree + z-order

**文件：**
- 创建：`include/layer.h`
- 创建：`src/layer/layer.c`
- 创建：`src/layer/meson.build`
- 修改：`include/output.h`（加 `layer_tree[4]` + `usable_area`）
- 修改：`include/woodland.h`（加 `layer_shell`、`new_layer_surface`、`focused_layer`）
- 修改：`src/output/output.c`（new_output 创建 4 棵树 + z-order；destroy 销毁）
- 修改：`src/server/server.c`（接线 layer_init/layer_finish）
- 修改：`src/meson.build`（链接 layer_lib）
- 修改：`tests/smoke/run_protocol_smoke.sh`（加 `zwlr_layer_shell_v1` 断言）
- 修改：`tests/unit/meson.build`（加 test_layer）
- 创建：`tests/unit/test_layer.c`

- [ ] **步骤 1：编写失败的测试**

`tests/unit/test_layer.c`（headless 起 server + 1 个 headless output，仿 `test_view.c` 结构）：

```c
// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "layer.h"
#include "output.h"
#include "woodland.h"

int main(void) {
    struct woodland_server server = {0};
    /* …display/event_loop/backend/renderer/allocator/scene 同 test_view.c… */
    assert(woodland_output_init(&server));
    assert(woodland_layer_init(&server));

    /* layer_shell 已创建 */
    assert(server.layer_shell != NULL);

    /* 添加 800x600 headless output，触发 new_output */
    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    /* 找到该输出，断言 4 棵 layer_tree 非空 */
    struct woodland_output *wo = wl_container_of(server.outputs.next, wo, link);
    for (int i = 0; i < 4; i++) {
        assert(wo->layer_tree[i] != NULL);
    }

    /* z-order：background(layer_tree[0]) 是场景最底，overlay(layer_tree[3]) 最顶 */
    struct wl_list *children = &server.scene->tree.children;
    assert(wl_container_of(children->next, struct wlr_scene_node, link)
           == &wo->layer_tree[0]->node);   /* 最底 = background */
    assert(wl_container_of(children->prev, struct wlr_scene_node, link)
           == &wo->layer_tree[3]->node);   /* 最顶 = overlay */

    woodland_layer_finish(&server);
    woodland_output_finish(&server);
    /* …逆序释放 wlr 资源同 test_view.c… */
    return 0;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson setup build -Dtest=enabled && meson compile -C build && meson test -C build layer`
预期：编译失败（`layer.h` 不存在 / `woodland_layer_init` 未定义）。

- [ ] **步骤 3：实现 `include/layer.h` + `src/layer/layer.c` 骨架**

`include/layer.h`（本任务先只声明 init/finish，后续任务补 surface 相关）：

```c
// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_LAYER_H
#define WOODLAND_LAYER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct woodland_server;
struct woodland_output;

bool woodland_layer_init(struct woodland_server *server);
void woodland_layer_finish(struct woodland_server *server);

#endif
```

`src/layer/layer.c`：

```c
// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>
#include "layer.h"
#include "woodland.h"

#define WOODLAND_LAYER_SHELL_VERSION 4

bool woodland_layer_init(struct woodland_server *server) {
    server->layer_shell =
        wlr_layer_shell_v1_create(server->display, WOODLAND_LAYER_SHELL_VERSION);
    if (!server->layer_shell) {
        wlr_log(WLR_ERROR, "failed to create layer shell");
        return false;
    }
    return true;
}

void woodland_layer_finish(struct woodland_server *server) {
    /* manager 随 display 销毁，不单独 destroy；此处仅置空 */
    server->layer_shell = NULL;
    server->focused_layer = NULL;
}
```

- [ ] **步骤 4：修改 `include/output.h` + `include/woodland.h`**

`output.h` 的 `struct woodland_output` 加：

```c
struct wlr_scene_tree *layer_tree[4];   // 下标 = zwlr_layer_shell_v1_layer 枚举
struct wlr_box usable_area;             // 扣除 exclusive zone 后的可用区域
```

`woodland.h` 的 `struct woodland_server` 加（含 `#include <wlr/types/wlr_layer_shell_v1.h>`）：

```c
struct wlr_layer_shell_v1 *layer_shell;
struct wl_listener new_layer_surface;
struct wlr_layer_surface_v1 *focused_layer;   // 当前持有键盘焦点的 layer surface
```

- [ ] **步骤 5：修改 `src/output/output.c` 创建/销毁 layer tree**

`handle_new_output` 里在 `wlr_scene_output_layout_add_output` 之后创建 4 棵树，z-order 顺序是**关键细节**（labwc `output.c:718-748` 同款）：

```c
for (int i = 0; i < 4; i++) {
    output->layer_tree[i] = wlr_scene_tree_create(&server->scene->tree);
}
/* z-order 从下到上：background → bottom → [工作区] → top → overlay。
 * lower_to_bottom 先 [1] 后 [0]，保证 background 在 bottom 之下；
 * raise_to_top 先 [2] 后 [3]，保证 overlay 在 top 之上。 */
wlr_scene_node_lower_to_bottom(&output->layer_tree[1]->node);
wlr_scene_node_lower_to_bottom(&output->layer_tree[0]->node);
wlr_scene_node_raise_to_top(&output->layer_tree[2]->node);
wlr_scene_node_raise_to_top(&output->layer_tree[3]->node);
```

`handle_destroy` 里在 free(output) 前销毁 4 棵 tree（`wlr_scene_node_destroy`）。注意 `handle_destroy` 现由 `wlr_scene_output_destroy` 开头，须在释放 `scene_output` 相关后、`free(output)` 前销毁 layer tree。

- [ ] **步骤 6：修改 `src/server/server.c` 接线 + `src/meson.build` 链接**

`woodland_server_init` 在 `woodland_workspace_init` 之后调用 `woodland_layer_init`；`woodland_server_finish` 在 `woodland_workspace_finish` 之前调用 `woodland_layer_finish`。

`src/meson.build`：加 `subdir('layer')`，`woodland_bin` 的 `link_with` 加 `layer_lib`。`src/layer/meson.build`：

```meson
layer_lib = static_library('woodland-layer', 'layer.c',
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server])
```

- [ ] **步骤 7：修改 `tests/unit/meson.build` 加 test_layer**

```meson
test_layer = executable('test_layer', 'test_layer.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, layer_lib])
test('layer', test_layer)
```

- [ ] **步骤 8：修改 `tests/smoke/run_protocol_smoke.sh` 加断言**

在现有 4 个 grep 后追加：

```sh
echo "$output" | grep -q 'zwlr_layer_shell_v1' || {
    echo "zwlr_layer_shell_v1 global missing from output:" >&2
    echo "$output" >&2
    exit 1
}
```

并把末尾 echo 加上 ` + zwlr_layer_shell_v1`。

- [ ] **步骤 9：运行测试验证通过**

运行：`meson compile -C build && meson test -C build --print-errorlogs`
预期：全部通过（9/9，含新增 test_layer + smoke-protocol 新断言）。

- [ ] **步骤 10：Commit**

```bash
git add include/layer.h src/layer/ include/output.h include/woodland.h \
  src/output/output.c src/server/server.c src/meson.build \
  tests/unit/test_layer.c tests/unit/meson.build tests/smoke/run_protocol_smoke.sh
git commit -m "feat(layer): add layer-shell skeleton with per-output layer trees"
```

---

### 任务 2：layer surface 生命周期 + arrange + popup

**文件：**
- 修改：`include/layer.h`（补 surface/popup 结构 + arrange 声明）
- 修改：`src/layer/layer.c`（new_surface → 6 listener + arrange + popup）

- [ ] **步骤 1：扩展 `include/layer.h`**

```c
struct wlr_layer_surface_v1;
struct wlr_xdg_popup;

struct woodland_layer_surface {
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer_surface;
    bool mapped;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener surface_commit;
    struct wl_listener output_destroy;
    struct wl_listener node_destroy;
    struct wl_listener new_popup;
};

struct woodland_layer_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener commit;
    struct wl_listener destroy;
};

void woodland_layer_arrange(struct woodland_server *server,
                            struct woodland_output *output);
```

- [ ] **步骤 2：实现 `new_surface` 接线 + 6 listener（`src/layer/layer.c`）**

`woodland_layer_init` 里接线：

```c
server->new_layer_surface.notify = handle_new_layer_surface;
wl_signal_add(&server->layer_shell->events.new_surface,
              &server->new_layer_surface);
```

`handle_new_layer_surface`：
1. 若 `layer_surface->output == NULL` → 取 `server->outputs` 首个输出的 `wlr_output` 赋值。
2. 分配 `struct woodland_layer_surface`（calloc），记 `layer_surface`。
3. 取输出对应 layer_tree：`wo->layer_tree[layer_surface->current.layer]`。
4. `wlr_scene_layer_surface_v1_create(该 tree, layer_surface)` 存入 `scene_layer_surface`。
5. 接线 map/unmap/surface_commit/output_destroy/node_destroy/new_popup 六个 listener。

- [ ] **步骤 3：实现 arrange（`woodland_layer_arrange`）**

复刻 labwc `layers_arrange` 核心（去掉 override）：

```c
void woodland_layer_arrange(struct woodland_server *server,
                            struct woodland_output *output) {
    struct wlr_box full_area = {0};
    wlr_output_effective_resolution(output->wlr_output,
        &full_area.width, &full_area.height);
    struct wlr_box usable_area = full_area;

    /* 先 exclusive（overlay → background），再非 exclusive（background → overlay） */
    for (int i = 3; i >= 0; i--)
        arrange_one_layer(&full_area, &usable_area, output->layer_tree[i], true);
    for (int i = 0; i < 4; i++)
        arrange_one_layer(&full_area, &usable_area, output->layer_tree[i], false);

    output->usable_area = usable_area;

    /* 同步 layer tree 节点位置到输出在 layout 中的坐标 */
    for (int i = 0; i < 4; i++)
        wlr_scene_node_set_position(&output->layer_tree[i]->node,
            output->scene_output->x, output->scene_output->y);
}
```

`arrange_one_layer(full_area, usable_area, tree, exclusive)` 遍历 `tree->children`，跳过未 initialized 的 surface，按 `!!current.exclusive_zone != exclusive` 过滤，调 `wlr_scene_layer_surface_v1_configure(scene, full_area, usable_area)`。

- [ ] **步骤 4：实现 map/unmap/commit 回调**

- `handle_map`：`layer->mapped = true`；`woodland_layer_arrange(server, output)`。
- `handle_unmap`：`layer->mapped = false`；`woodland_layer_arrange`。
- `handle_surface_commit`：若 `current.committed & LAYER` → `wlr_scene_node_reparent` 到新 layer tree；若 `current.committed & KEYBOARD_INTERACTIVITY` → 调 `woodland_layer_try_focus`（任务 3 实现，本任务先留声明 + 空调用或直接调声明）；若 committed 非 0 或 mapped 变化 → arrange。

- [ ] **步骤 5：实现 node_destroy 主清理 + output_destroy**

`handle_node_destroy`（主清理点，防 UAF）：
1. 若 `server->focused_layer == layer->layer_surface` → 置 NULL。
2. `layer->scene_layer_surface = NULL`（wlroots 可能已释放）。
3. 摘除全部 6 个 listener（map/unmap/surface_commit/output_destroy/node_destroy/new_popup）。
4. `free(layer)`。

`handle_output_destroy`：`wlr_layer_surface_v1_destroy(layer->layer_surface)`（会触发 node_destroy 链）。

- [ ] **步骤 6：实现 popup**

`handle_new_popup`（`layer_surface->events.new_popup`）：分配 `struct woodland_layer_popup`，`wlr_scene_xdg_surface_create(scene_layer_surface->tree, popup->base)` 存入 scene_tree；接线 commit/destroy。`handle_popup_commit`：initial_commit 则 `wlr_xdg_surface_schedule_configure`。`handle_popup_destroy`：摘 2 个 listener + free。

- [ ] **步骤 7：运行测试验证通过**

运行：`meson compile -C build && meson test -C build --print-errorlogs`
预期：全部通过（9/9；本任务无新单测，验证不破坏现有 + smoke 仍绿）。

- [ ] **步骤 8：Commit**

```bash
git add include/layer.h src/layer/layer.c
git commit -m "feat(layer): add layer surface lifecycle, arrange and popups"
```

---

### 任务 3：焦点流转 + usable_area 集成

**文件：**
- 修改：`include/layer.h`（加 `layer_try_focus`、`layer_surface_at` 声明）
- 修改：`src/layer/layer.c`（实现三态焦点 + 命中测试）
- 修改：`src/workspace/workspace.c`（切换工作区区分焦点）
- 修改：`src/shell/shell.c`（`woodland_view_maximized_geometry` 改用 usable_area；指针点击命中 layer surface）
- 修改：`tests/unit/test_view.c`（usable_area 集成断言）

- [ ] **步骤 1：编写失败的测试（usable_area 集成）**

在 `tests/unit/test_view.c` 现有 maximize 断言处，追加：

```c
/* exclusive zone 占用后 maximize 应避开：手动设 usable_area */
struct woodland_output *wo =
    wl_container_of(server.outputs.next, wo, link);
wo->usable_area = (struct wlr_box){ .x = 10, .y = 10, .width = 780, .height = 580 };
struct wlr_box max_geo2 = woodland_view_maximized_geometry(&server);
assert(max_geo2.x == 10 && max_geo2.y == 10);
assert(max_geo2.width == 780 && max_geo2.height == 580);
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson compile -C build && meson test -C build view`
预期：FAIL（maximized_geometry 仍返回 800x600 完整 box）。

- [ ] **步骤 3：实现 `woodland_view_maximized_geometry` 改用 usable_area**

`src/shell/shell.c` 该函数：取第一个输出后，若 `wo->usable_area.width/height != 0` 则返回 `wo->usable_area`，否则 fallback 到 `wlr_output_layout_get_box`。fullscreen 保持完整 box 不变。

- [ ] **步骤 4：实现 `woodland_layer_try_focus`（三态）**

`include/layer.h` 加声明：

```c
void woodland_layer_try_focus(struct woodland_server *server,
                              struct wlr_layer_surface_v1 *layer_surface);
struct wlr_layer_surface_v1 *woodland_layer_surface_at(
    struct woodland_server *server, double lx, double ly,
    struct wlr_surface **surface, double *sx, double *sy);
```

`src/layer/layer.c` 实现：按 `current.keyboard_interactive` 分派——exclusive 直接给键盘焦点（`wlr_seat_keyboard_notify_enter` + 失活当前 xdg view）；on_demand 若当前无 exclusive 层持焦点则给；none 若焦点正是自己则转移给顶层 view。得焦点时置 `server->focused_layer`；失焦点时置 NULL 并把焦点还给当前工作区顶层 view（复用 `woodland_view_focus` 对视图列表头的视图）。

- [ ] **步骤 5：实现 `woodland_layer_surface_at` 命中测试**

仿 `woodland_view_at`：`wlr_scene_node_at` 命中后，若 node 是 layer surface 的 surface（`wlr_layer_surface_v1_try_from_wlr_surface`），返回该 layer surface + 局部坐标。用于指针点击区分「点中 layer surface」还是「点中 xdg view」。

- [ ] **步骤 6：指针点击命中 layer surface 时给焦点**

`src/shell/shell.c` 的 `server_cursor_button`：点击时若 `woodland_view_at` 返回 NULL，再试 `woodland_layer_surface_at`；命中则 `woodland_layer_try_focus(server, layer_surface)`。

- [ ] **步骤 7：修改 `woodland_workspace_switch` 区分焦点**

`src/workspace/workspace.c`：clear_focus 前判断——若 `server->focused_layer != NULL`（焦点在 layer surface，跨工作区常驻），**不**清键盘焦点；否则（焦点在 xdg view，属于旧工作区）才 `wlr_seat_keyboard_notify_clear_focus`。

- [ ] **步骤 8：运行测试验证通过**

运行：`meson compile -C build && meson test -C build --print-errorlogs`
预期：全部通过（9/9）。

- [ ] **步骤 9：Commit**

```bash
git add include/layer.h src/layer/layer.c src/workspace/workspace.c \
  src/shell/shell.c tests/unit/test_view.c
git commit -m "feat(layer): add keyboard focus handling and usable_area integration"
```

---

### 任务 4：回归 + 文档

**文件：**
- 修改：`docs/superpowers/2026-10-22-woodland-skeleton.md`（M2 进展补 layer-shell）

- [ ] **步骤 1：全量回归**

运行：`meson compile -C build && meson test -C build --print-errorlogs`
预期：全部通过（9/9）。

- [ ] **步骤 2：更新里程碑文档**

在 `docs/superpowers/2026-10-22-woodland-skeleton.md` 的 M2 进展注释补一句：layer-shell（zwlr_layer_shell_v1 四层 + exclusive zone + 焦点）已在 `feature/m4-layer-shell` 实现。

- [ ] **步骤 3：Commit**

```bash
git add docs/superpowers/2026-10-22-woodland-skeleton.md
git commit -m "docs: mark layer-shell complete in milestone skeleton"
```
