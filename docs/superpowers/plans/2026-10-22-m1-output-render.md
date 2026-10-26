# Woodland M1（输出与渲染循环）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 woodland 真正把场景渲染到输出：输出热插拔、`wlr_output_layout` + `wlr_scene_output` 渲染循环、光标（`wlr_cursor` + cursor-shape 协议）。headless/DRM 下输出出现、空场景稳定渲染、光标显示。

**架构：** 延续 sway 风格单体。新增两个模块：`output.c`（输出热插拔 + 每帧渲染）、`cursor.c`（光标加载/设置/协议）。`struct woodland_server` 扩展 output_layout/scene_layout/cursor 字段，输出通过 `wlr_scene_output` 挂到 scene。渲染循环基于 output 的 `frame` 事件（wlroots 0.20 无 render_loop，这是标准做法）。

**技术栈：** C11、meson、wlroots 0.20.2（`wlr_output_layout`、`wlr_scene`、`wlr_cursor`、`wlr_xcursor_manager`、cursor-shape-v1）、wayland-server。

**规格：** `docs/superpowers/specs/2026-10-22-woodland-design.md`
**骨架规划：** `docs/superpowers/2026-10-22-woodland-skeleton.md`（本计划实现其 M1 里程碑）

---

## 全局约束

- 每个新文件顶部包含 `// SPDX-License-Identifier: GPL-2.0-only`（版权行省略，仅 SPDX 行）。
- 语言标准 `c_std=c11`；编译参数 `-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L`（cursor-shape 与 headless 均为 unstable API）。
- `warning_level=2` + `werror=true`：警告即错误。
- 依赖只经 wrap 下载（wlroots 0.20.2 已锁定），不引入新依赖。
- 资源释放顺序与 M0 一致：finish 逆序释放，每步判空置空（幂等）。
- 单实例全局 `struct woodland_server`，模块间通过指针传递。
- 光标主题：`XCURSOR_THEME` / `XCURSOR_SIZE` 环境变量，默认 size 24（labwc 做法）。
- 默认光标形状：`default`（cursor-spec 名称），对应 labwc 的 `LAB_CURSOR_DEFAULT`。

## 审查重点（Review Focus）

1. **无输出热插拔稳定性**：headless 后端无任何 output 时，事件循环必须稳定运行不崩溃；后插入 output 时能正确初始化渲染。→ 冒烟测试 + headless 加输出测试覆盖。
2. **输出拔出释放**：output 销毁事件触发时，`struct woodland_output` 必须释放（scene_output → frame 监听器 → 内存），不能 use-after-free 或泄漏。→ ASan 变体覆盖。
3. **光标在无输出下的安全**：无任何 output 时设置/移动光标不能崩溃；光标加载失败（无主题文件）不能崩溃，应回退到 wlroots 内置 fallback。→ 单测覆盖。
4. **重复 new_output 通知**：backend 可能对同一 output 发多次 new_output（labwc 注释明确提到 virtual output 场景），必须去重。→ 单测覆盖。
5. **光标形状协议正确性**：cursor-shape-v1 请求到达时，shape 名称经 `wlr_cursor_shape_v1_name()` 映射后正确设置；未知 shape 不能崩溃。→ 单测覆盖。

---

## 文件结构（M1 落地）

```
woodland/
├── include/
│   ├── woodland.h               # 修改：struct woodland_server 扩展 output_layout/scene_layout/cursor/xcursor_manager
│   ├── output.h                 # 创建：struct woodland_output + output 模块接口
│   └── cursor.h                 # 创建：cursor 模块接口
├── src/
│   ├── output/
│   │   ├── meson.build          # 创建：output_lib 静态库
│   │   └── output.c             # 创建：输出热插拔 + 渲染循环
│   ├── cursor/
│   │   ├── meson.build          # 创建：cursor_lib 静态库
│   │   └── cursor.c             # 创建：光标加载/设置 + cursor-shape 协议
│   ├── server/
│   │   └── server.c             # 修改：init/finish 接入 output/cursor 生命周期
│   └── meson.build              # 修改：subdir + link_with
└── tests/
    ├── unit/
    │   ├── test_output.c        # 创建：output 去重/创建单测
    │   └── test_cursor.c        # 创建：cursor 加载/形状单测
    └── smoke/
        └── run_headless.sh      # 修改：headless 加输出冒烟测试
```

---

### 任务 1：struct 扩展与 output/cursor 模块骨架

**文件：**
- 修改：`include/woodland.h`（扩展 struct）
- 创建：`include/output.h`、`include/cursor.h`
- 创建：`src/output/meson.build`、`src/output/output.c`、`src/cursor/meson.build`、`src/cursor/cursor.c`
- 修改：`src/meson.build`、`src/server/meson.build`

- [ ] **步骤 1：编写失败的测试**

`tests/unit/test_output.c`（验证 output 模块接口可编译、空场景下可创建）：

```c
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include "output.h"
#include "woodland.h"

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();

    assert(woodland_output_init(&server));

    /* 无 output 时 stable */
    assert(server.outputs.next == &server.outputs);
    assert(server.outputs.prev == &server.outputs);

    woodland_output_finish(&server);
    return 0;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build output`
预期：FAIL（编译错误，`woodland_output_init` 等未定义、`server.outputs` 字段不存在）。

- [ ] **步骤 3：在 `include/woodland.h` 中扩展 `struct woodland_server`**

新增字段：
```c
struct wl_list outputs;               // struct woodland_output.link 链表头
struct wlr_output_layout *output_layout;
struct wlr_scene_output_layout *scene_layout;
struct wlr_cursor *cursor;
struct wlr_xcursor_manager *xcursor_manager;
```
头文件需新增 include：`<wlr/types/wlr_output_layout.h>`、`<wlr/types/wlr_cursor.h>`、`<wlr/types/wlr_xcursor_manager.h>`。

- [ ] **步骤 4：在 `include/output.h` 定义 `struct woodland_output` 与接口**

```c
struct woodland_output {
    struct wl_list link;               // woodland_server.outputs
    struct woodland_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame;
    struct wl_listener destroy;
};

bool woodland_output_init(struct woodland_server *server);
void woodland_output_finish(struct woodland_server *server);
```

- [ ] **步骤 5：在 `include/cursor.h` 定义 cursor 接口**

```c
bool woodland_cursor_init(struct woodland_server *server);
void woodland_cursor_finish(struct woodland_server *server);
void woodland_cursor_set(struct woodland_server *server, const char *name);
```

- [ ] **步骤 6：实现 `src/output/output.c` 的最小可编译版本**

`woodland_output_init`：`wl_list_init(&server->outputs)`，创建 `wlr_output_layout_create(display)` 与 `wlr_scene_attach_output_layout(scene, output_layout)`，失败返回 false。`woodland_output_finish`：销毁 scene_layout → output_layout（判空）。**本任务不实现 new_output 渲染循环**（任务 2）。

- [ ] **步骤 7：实现 `src/cursor/cursor.c` 的最小可编译版本**

`woodland_cursor_init`：`wlr_cursor_create()` + `wlr_cursor_attach_output_layout(cursor, output_layout)`，失败返回 false。`woodland_cursor_finish`：销毁 xcursor_manager → cursor。`woodland_cursor_set` 空实现（任务 3 补）。

- [ ] **步骤 8：接线 meson**

`src/output/meson.build`：`output_lib = static_library('woodland-output', 'output.c', include_directories: woodland_inc, dependencies: [wlroots, wayland_server])`
`src/cursor/meson.build`：`cursor_lib = static_library('woodland-cursor', 'cursor.c', include_directories: woodland_inc, dependencies: [wlroots, wayland_server])`
`src/meson.build`：`subdir('output')` + `subdir('cursor')`，可执行 link_with 增加 `output_lib, cursor_lib`。

- [ ] **步骤 9：运行测试验证通过**

运行：`meson test -C build output`
预期：PASS。

- [ ] **步骤 10：Commit**

```bash
git add include/ src/
git commit -m "feat(output): add output/cursor module skeleton and struct fields"
```

---

### 任务 2：输出热插拔与渲染循环

**文件：**
- 修改：`src/output/output.c`（实现 new_output 监听 + frame 渲染循环）
- 修改：`src/server/server.c`（init 调 output_init，finish 调 output_finish；backend new_output 信号接线）
- 测试：`tests/unit/test_output.c`（扩展）、`tests/smoke/run_headless.sh`（扩展）

- [ ] **步骤 1：编写失败的测试**

`tests/unit/test_output.c` 追加（验证 headless 加输出后 scene_output 创建 + 去重）：

```c
/* 在 main 内 woodland_output_init 之后追加： */
struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
assert(out != NULL);
/* 模拟 new_output 事件 */
wlr_signal_emit_mutable(&server.backend->events.new_output, out);
/* 去重：重复 emit 不应重复创建 */
size_t n = 0;
struct woodland_output *o;
wl_list_for_each(o, &server.outputs, link) n++;
assert(n == 1);
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build output`
预期：FAIL（n == 0，因 new_output 未处理）。

- [ ] **步骤 3：在 `src/output/output.c` 实现 `new_output` 处理**

监听 `server->backend->events.new_output`（在 `woodland_output_init` 中接线，用 `struct wl_listener` 存于 server 内新增字段 `new_output`）。handler `handle_new_output` 按 labwc 顺序：
1. 遍历 `server->outputs` 去重（`wlr_output == wlr_output` 则 return）。
2. `calloc` 分配 `struct woodland_output`，`wlr_output_init_render(wlr_output, allocator, renderer)`。
3. 接线 `frame` 事件 → `handle_frame`，`destroy` 事件 → `handle_destroy`。
4. `wlr_output_layout_add_auto(output_layout, wlr_output)`；失败报错释放返回。
5. `wlr_scene_output_create(scene, wlr_output)`；失败报错释放返回。
6. `wlr_scene_output_layout_add_output(scene_layout, layout_output, scene_output)`。
7. 组装 `wlr_output_state`：`set_enabled(true)` + `wlr_output_preferred_mode`（若非 NULL 则 `set_mode`），`wlr_output_commit_state` + `wlr_output_state_finish`。
8. `wlr_output_create_global(wlr_output, display)`。

- [ ] **步骤 4：实现 `handle_frame` 渲染循环**

```c
static void handle_frame(struct wl_listener *listener, void *data) {
    struct woodland_output *output = wl_container_of(listener, output, frame);
    if (!wlr_scene_output_commit(output->scene_output, NULL)) {
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}
```

- [ ] **步骤 5：实现 `handle_destroy` 输出释放**

`wlr_scene_output_destroy(scene_output)` → `wl_list_remove(&output->destroy.link)`、`wl_list_remove(&output->frame.link)`、`wl_list_remove(&output->link)` → `free(output)`。注意：output 销毁时 wlr_output 已由 backend 负责，本模块只释放自己分配的 scene_output 与 struct。

- [ ] **步骤 6：在 `src/server/server.c` 接线生命周期**

`woodland_server_init` 在 scene 创建后调 `woodland_output_init(server)` 与 `woodland_cursor_init(server)`（均失败返回 false）；`woodland_server_finish` 在 scene 释放**前**调 `woodland_output_finish(server)` 与 `woodland_cursor_finish(server)`（因为 scene_output/cursor 引用 scene/output_layout）。

- [ ] **步骤 7：运行测试验证通过**

运行：`meson test -C build output`
预期：PASS（n == 1）。

- [ ] **步骤 8：扩展冒烟测试**

`tests/smoke/run_headless.sh` 增加 `WLR_HEADLESS_OUTPUTS=1`（若支持）或改为 headless 加输出的启动验证。核心断言不变：启动 → SIGTERM → 退出码 0。

- [ ] **步骤 9：运行冒烟测试验证通过**

运行：`sh tests/smoke/run_headless.sh ./build/src/woodland`
预期：PASS，退出码 0。

- [ ] **步骤 10：ASan 变体验证无泄漏**

运行：`meson setup build-asan -Db_sanitize=address,undefined && meson compile -C build-asan && sh tests/smoke/run_headless.sh ./build-asan/src/woodland`
预期：PASS，退出码 0，无 ASan 报错。

- [ ] **步骤 11：Commit**

```bash
git add src/output/ src/server/ tests/
git commit -m "feat(output): add output hotplug and frame render loop"
```

---

### 任务 3：光标管理 + cursor-shape 协议

**文件：**
- 修改：`src/cursor/cursor.c`（光标加载/设置 + cursor-shape 协议接线）
- 修改：`src/server/server.c`（创建 cursor-shape manager，接线 request_set_shape）
- 测试：`tests/unit/test_cursor.c`

- [ ] **步骤 1：编写失败的测试**

`tests/unit/test_cursor.c`：

```c
#include <assert.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "cursor.h"
#include "woodland.h"

int main(void) {
    struct woodland_server server = {0};
    server.display = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    server.renderer = wlr_renderer_autocreate(server.backend);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    server.scene = wlr_scene_create();
    server.output_layout = wlr_output_layout_create(server.display);

    assert(woodland_cursor_init(&server));
    assert(server.cursor != NULL);
    assert(server.xcursor_manager != NULL);

    /* 设置已知形状不崩溃 */
    woodland_cursor_set(&server, "default");
    woodland_cursor_set(&server, "pointer");
    /* 未知形状不崩溃（回退到 default） */
    woodland_cursor_set(&server, "nonexistent-shape-xyz");

    woodland_cursor_finish(&server);
    return 0;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build cursor`
预期：FAIL（`woodland_cursor_set` 空实现，或 xcursor_manager 为 NULL）。

- [ ] **步骤 3：实现 `woodland_cursor_init` 完整版**

1. `wlr_cursor_create()` + `wlr_cursor_attach_output_layout(cursor, output_layout)`。
2. 读 `XCURSOR_THEME` / `XCURSOR_SIZE`（默认 24）。
3. `wlr_xcursor_manager_create(theme, size)` + `wlr_xcursor_manager_load(manager, 1)`。
4. 失败不崩溃，返回 false。

- [ ] **步骤 4：实现 `woodland_cursor_set`**

```c
void woodland_cursor_set(struct woodland_server *server, const char *name) {
    wlr_cursor_unset_image(server->cursor);  // 强制刷新（labwc 做法）
    wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}
```
未找到的 shape 由 wlroots 回退到默认光标（不崩溃）。

- [ ] **步骤 5：实现 `woodland_cursor_finish`**

`wlr_xcursor_manager_destroy(xcursor_manager)` → `wlr_cursor_destroy(cursor)`，判空置空。

- [ ] **步骤 6：在 `src/server/server.c` 创建 cursor-shape manager 并接线**

`woodland_server_init` 中：`struct wlr_cursor_shape_manager_v1 *mgr = wlr_cursor_shape_manager_v1_create(display, 1);`（存 server 字段 `cursor_shape_manager`）。接线 `request_set_shape` → `handle_request_set_shape`：handler 中取 `event->shape`，经 `wlr_cursor_shape_v1_name(shape)` 得名称，调 `woodland_cursor_set(server, name)`。`woodland_server_finish` 中移除监听器。

- [ ] **步骤 7：运行测试验证通过**

运行：`meson test -C build cursor`
预期：PASS。

- [ ] **步骤 8：Commit**

```bash
git add src/cursor/ src/server/ tests/
git commit -m "feat(cursor): add cursor management and cursor-shape protocol"
```

---

### 任务 4：完整回归 + ASan + 文档

**文件：**
- 修改：`docs/superpowers/2026-10-22-woodland-skeleton.md`（标记 M1 完成）
- 无新代码

- [ ] **步骤 1：运行完整测试套件**

运行：`meson test -C build --print-errorlogs`
预期：`log`、`output`、`cursor`、`smoke-headless` 全部 PASS。

- [ ] **步骤 2：ASan 完整回归**

运行：`meson setup build-asan -Db_sanitize=address,undefined -Dtest=enabled && meson compile -C build-asan && meson test -C build-asan`
预期：全部 PASS，无 ASan 报错。

- [ ] **步骤 3：更新骨架规划文档标记 M1 完成**

在 `docs/superpowers/2026-10-22-woodland-skeleton.md` 的 M1 节添加 `- [x]` 完成标记与实现说明。

- [ ] **步骤 4：Commit**

```bash
git add docs/
git commit -m "docs: mark M1 complete in skeleton plan"
```

---

## 自检记录

- **规格覆盖度**：M1 对应骨架规划 §4 M1（输出热插拔、output_layout、渲染循环、光标 + cursor-shape）。M1 不触碰窗口管理（xdg-shell）、seat 输入焦点、工作区、布局——这些是 M2~M6。光标移动/按钮输入（`wlr_cursor_move` 等）属 M2 seat 范畴，M1 只做光标显示与形状协议。
- **步骤扫描**：每步有唯一命令与明确预期；接口签名已给出；算法（labwc 顺序、cursor-shape 映射）由参考实现钉死。
- **类型一致性**：`struct woodland_server` 新字段在任务 1 定义，任务 2/3 消费，命名一致；`woodland_output_init/finish`、`woodland_cursor_init/finish/set` 在任务 1 声明，任务 2/3 实现。
- **审查重点**：5 项分别由 test_output（1/2/4）、test_cursor（3/5）、ASan（2）、冒烟（1）覆盖。
- **比例**：计划短于规格，代码块仅为签名、结构体字段与关键算法（渲染循环、cursor 设置），函数体留实现者。
