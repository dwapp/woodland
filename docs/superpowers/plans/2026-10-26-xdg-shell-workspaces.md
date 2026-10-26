# xdg-shell 完整支持与工作区 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 补齐完整 xdg-shell 支持（maximize/fullscreen/minimize/set_title/set_app_id/set_parent）+ 固定 10 个工作区 + ext-workspace-v1 与 foreign-toplevel-v1 两个协议。

**架构：** 新增 `src/workspace/` 模块（工作区模型 + 切换 + ext-workspace-v1）；扩展 `src/shell/shell.c`（真实几何/状态 + foreign-toplevel-v1）；`src/seat/seat.c` 加 Alt+数字切换键位。链接已用 `--start-group`，静态库循环引用无碍。

**技术栈：** 纯 C11 + wlroots 0.20.2（`wlr_xdg_shell`、`wlr_scene`、`wlr_ext_workspace_v1`、`wlr_foreign_toplevel_management_v1`）。

**规格：** `docs/superpowers/specs/2026-10-26-xdg-shell-workspaces-design.md`

## 全局约束

- `c_std=c11`、`warning_level=2` + `werror=true`（警告即错误）。
- 每个新源文件顶部 `// SPDX-License-Identifier: GPL-2.0-only`（版权行省略）。
- 资源释放纪律：finish 逆序、判空置空、不 double-free / use-after-free。
- 单实例 `struct woodland_server`，模块间通过 `server` 指针传递。
- 保持最小 diff；不新增依赖（只用 wlroots 自带协议）。
- 固定 10 个工作区（name "1"~"10"）。

## 审查重点（Review Focus）

1. **视图销毁时 listener 未摘除**：`woodland_view` 销毁必须摘除全部 listener（map/unmap/commit/destroy/request_*/set_title/set_app_id/set_parent/request_minimize）+ foreign-toplevel handle 先 destroy，否则 wlr 回调触发 UAF。
2. **工作区切换后旧工作区视图仍可聚焦**：`woodland_view_focus` 必须只聚焦当前工作区的 mapped 视图；切换工作区时若焦点视图不在新工作区须清焦点。
3. **minimize 后 Alt+F1 切窗仍会聚焦隐藏窗口**：切窗遍历须跳过 `minimized` 视图；restore 仅经 foreign-toplevel `request_activate`。
4. **maximize/fullscreen 恢复几何丢失**：`saved_geometry` 必须在首次 maximize/fullscreen 前保存；恢复后置空状态标志，重复切换不覆盖原始几何。
5. **ext-workspace commit 遍历 requests 时 workspace 已销毁**：`ACTIVATE` 请求的 `req->activate.workspace` 可能为 NULL（客户端已销毁 handle），须判空再 `->data` 取 workspace。

---

### 任务 1：工作区模块骨架 + 几何纯函数

**文件：**
- 创建：`include/workspace.h`
- 创建：`src/workspace/workspace.c`
- 创建：`src/workspace/meson.build`
- 修改：`src/meson.build`（`subdir('workspace')` + `workspace_lib` 加入 `link_with`）
- 修改：`include/woodland.h`（server 新增 workspaces 字段）
- 修改：`include/view.h`（声明两个几何纯函数）
- 修改：`src/shell/shell.c`（实现两个几何纯函数，暂不接工作区）
- 测试：`tests/unit/test_workspace.c`（新建）
- 修改：`tests/unit/meson.build`（登记 test_workspace）

**接口：**
```c
// workspace.h
struct woodland_workspace {
    struct wl_list link;                 // server.workspaces
    struct woodland_server *server;
    char *name;                          // "1".."10"
    struct wlr_scene_tree *tree;
    struct wlr_ext_workspace_handle_v1 *ext_workspace; // 任务 2 填充
};

bool woodland_workspace_init(struct woodland_server *server);
void woodland_workspace_finish(struct woodland_server *server);
struct woodland_workspace *woodland_workspace_by_name(
    struct woodland_server *server, const char *name);

// view.h
struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server);
struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output);
```

`server` 新增字段（`woodland.h`）：
```c
struct wl_list workspaces;               // woodland_workspace.link
struct woodland_workspace *active_workspace;
```

- [ ] **步骤 1：写失败的测试** `tests/unit/test_workspace.c`

headless 起 server（display/backend/renderer/allocator/scene），调 `woodland_workspace_init(&server)`，断言：
- `server.active_workspace` 非空且 `strcmp(name, "1") == 0`
- 遍历 `server.workspaces` 计数 == 10
- 工作区 1 的 `tree->node.enabled == true`，其余为 false
- `woodland_workspace_by_name(server, "10")` 返回 name "10" 的工作区；`by_name(server, "11")` 返回 NULL

（本任务暂不测几何纯函数，留到任务 2 引入 output 后。）

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build test_workspace --print-errorlogs`
预期：FAIL（编译或链接错误：`woodland_workspace_init` 未定义 / `server.workspaces` 字段不存在）

- [ ] **步骤 3：实现 `include/workspace.h` + `src/workspace/workspace.c`**

`workspace_init`：`wl_list_init(&server->workspaces)`；循环 10 次，每次 `calloc` 一个 `woodland_workspace`，`name = strdup("1".."10")`（用 `snprintf` 生成），`tree = wlr_scene_tree_create(&server->scene->tree)`，`wl_list_insert`；仅第 1 个 `wlr_scene_node_set_enabled(&tree->node, true)`；`server->active_workspace = 第 1 个`。
`workspace_by_name`：遍历链表 `strcmp`。
`workspace_finish`：`wl_list_for_each_safe` 摘除，`wlr_scene_node_destroy(&tree->node)` + `free(name)` + `free(ws)`；`active_workspace = NULL`。

`woodland.h` 加字段；`src/meson.build` 加 `subdir('workspace')` + `workspace_lib`（依赖 wlroots + wayland_server）；`src/shell/shell.c` 实现两个几何纯函数（maximized 用 `wlr_output_layout_get_box` 取活动输出 box；fullscreen 用传入 output 的 `wlr_output->width/height`，返回全 0 box 若 layout 无输出）。几何函数本任务先写但无测试（任务 2 补测）。

- [ ] **步骤 4：运行测试验证通过**

运行：`meson test -C build test_workspace --print-errorlogs`
预期：PASS

- [ ] **步骤 5：Commit**

```bash
git add include/workspace.h src/workspace/ src/meson.build include/woodland.h include/view.h src/shell/shell.c tests/unit/test_workspace.c tests/unit/meson.build
git commit -m "feat(workspace): add workspace module skeleton and geometry helpers"
```

---

### 任务 2：工作区切换 + 视图归属 + Alt 键位

**文件：**
- 修改：`include/workspace.h`（`woodland_workspace_switch` 声明）
- 修改：`src/workspace/workspace.c`（实现 switch）
- 修改：`include/view.h`（`struct woodland_view` 加 `workspace`、`mapped` 字段）
- 修改：`src/shell/shell.c`（map 时挂到 active_workspace->tree；`woodland_view_focus` 只聚焦当前工作区 mapped 视图）
- 修改：`src/seat/seat.c`（Alt+数字切换键位）
- 测试：`tests/unit/test_workspace.c`（追加 switch 断言）
- 修改：`tests/unit/test_view.c`（新建，几何纯函数测试）

**接口：**
```c
// workspace.h
void woodland_workspace_switch(struct woodland_server *server,
                               struct woodland_workspace *target);
```

- [ ] **步骤 1：写失败的测试**

`test_workspace.c` 追加：`woodland_workspace_switch(server, ws3)`（ws3 = by_name("3")）后断言 `ws3->tree->node.enabled == true`、`ws1->tree->node.enabled == false`、`server.active_workspace == ws3`。

`test_view.c`（新建）：headless + `wlr_headless_add_output(backend, 800, 600)` + emit new_output；断言 `woodland_view_maximized_geometry` 返回 `(0,0,800,600)`；`woodland_view_fullscreen_geometry(server, out)` 返回 `(0,0,800,600)`。

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build test_workspace test_view --print-errorlogs`
预期：FAIL（`woodland_workspace_switch` 未定义 / test_view 未登记）

- [ ] **步骤 3：实现 switch + 归属 + 键位**

`workspace_switch`：旧 ws `set_enabled(false)` → 新 ws `set_enabled(true)` → `server->active_workspace = target`。ext_workspace 同步留任务 3。

`shell.c` `xdg_toplevel_map`：`view->mapped = true`；`view->workspace = server->active_workspace`；`wlr_scene_node_reparent(&view->scene_tree->node, &ws->tree->node)`；再 `woodland_view_focus(view)`。
`xdg_toplevel_unmap`：`view->mapped = false`。
`woodland_view_focus`：开头加守卫 `if (view->workspace != server->active_workspace || !view->mapped || view->minimized) return;`（minimized 字段任务 4 加，本任务先只写 workspace/mapped 守卫）。

`seat.c` `handle_keybinding` 加分支：`XKB_KEY_1`~`XKB_KEY_9` → `woodland_workspace_by_name(server, "1".."9")` 后 switch；`XKB_KEY_0` → "10"。`XKB_KEY_F1` 切窗遍历改为跳过 `!view->mapped || view->workspace != server->active_workspace` 的视图。

- [ ] **步骤 4：运行测试验证通过**

运行：`meson test -C build test_workspace test_view --print-errorlogs`
预期：PASS

- [ ] **步骤 5：Commit**

```bash
git add include/workspace.h src/workspace/workspace.c include/view.h src/shell/shell.c src/seat/seat.c tests/unit/test_workspace.c tests/unit/test_view.c tests/unit/meson.build
git commit -m "feat(workspace): add workspace switching, view assignment and keybindings"
```

---

### 任务 3：ext-workspace-v1 协议

**文件：**
- 修改：`include/woodland.h`（server 加 ext_workspace_manager/group/listener 字段）
- 修改：`include/workspace.h`（声明不变，`woodland_workspace` 的 `ext_workspace` 字段已就位）
- 修改：`src/workspace/workspace.c`（init 创建 manager/group + handle；commit 监听；switch 同步 set_active）
- 修改：`src/server/server.c`（调用处不新增，workspace_init 已含协议创建）
- 测试：`tests/smoke/run_protocol_smoke.sh`（追加 grep 两个全局）

**接口：** 复用任务 1 的 `woodland_workspace_init`（内部扩展），无新公共接口。

- [ ] **步骤 1：写失败的测试**

`run_protocol_smoke.sh` 在末尾追加**一行**（仅 ext_workspace 断言，zwlr 断言留到任务 4）：
```sh
echo "$output" | grep -q 'ext_workspace_manager_v1' || { echo "ext_workspace_manager_v1 global missing" >&2; exit 1; }
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build smoke-protocol --print-errorlogs`
预期：FAIL（`ext_workspace_manager_v1 global missing`；当前代码无此协议）

- [ ] **步骤 3：实现 ext-workspace-v1**

`workspace_init` 追加：
- `server->ext_workspace_manager = wlr_ext_workspace_manager_v1_create(server->display, 1)`
- `server->ext_workspace_group = wlr_ext_workspace_group_handle_v1_create(manager, 0)`
- 每个工作区：`wlr_ext_workspace_handle_v1_create(manager, NULL, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE)` → `set_group` → `set_name(name)` → 仅活动 ws `set_active(true)` → `ext_workspace->data = ws`
- `server->ext_workspace_commit.notify = handle_ext_workspace_commit` + `wl_signal_add(&manager->events.commit, ...)`；handler 遍历 `event->requests`，`ACTIVATE` 且 `req->activate.workspace` 非空 → `workspace_switch(server, req->activate.workspace->data)`。

`workspace_switch` 追加：旧 `active_workspace->ext_workspace` `set_active(false)`，新 `set_active(true)`。

`workspace_finish` 追加：摘 `ext_workspace_commit` listener；`wlr_ext_workspace_group_handle_v1_destroy`；各 workspace `wlr_ext_workspace_handle_v1_destroy`；manager 随 display 销毁（不单独 destroy）。

- [ ] **步骤 4：运行测试验证通过**

运行：`meson test -C build smoke-protocol --print-errorlogs`
预期：PASS（`ext_workspace_manager_v1 global` 出现）

- [ ] **步骤 5：Commit**

```bash
git add include/woodland.h src/workspace/workspace.c tests/smoke/run_protocol_smoke.sh
git commit -m "feat(workspace): implement ext-workspace-v1 protocol"
```

---

### 任务 4：xdg-shell 真实几何/状态 + foreign-toplevel-v1

**文件：**
- 修改：`include/woodland.h`（server 加 `foreign_toplevel_manager`）
- 修改：`include/view.h`（view 加 title/app_id/minimized/maximized/fullscreen/saved_geometry/toplevel_handle + 5 个 listener）
- 修改：`src/shell/shell.c`（maximize/fullscreen/minimize/set_title/set_app_id/set_parent + foreign-toplevel 接线）
- 测试：`tests/smoke/run_protocol_smoke.sh`（追加 zwlr 断言）

**接口：** view 字段扩展；shell.c 内部处理函数复用。

- [ ] **步骤 1：写失败的测试**

`run_protocol_smoke.sh` 在 ext_workspace 断言后追加**一行**：
```sh
echo "$output" | grep -q 'zwlr_foreign_toplevel_manager_v1' || { echo "zwlr_foreign_toplevel_manager_v1 global missing" >&2; exit 1; }
```

- [ ] **步骤 2：运行测试验证失败**

运行：`meson test -C build smoke-protocol --print-errorlogs`
预期：FAIL（`zwlr_foreign_toplevel_manager_v1 global missing`；当前代码无此协议）

- [ ] **步骤 3：实现**

`shell_init` 加 `server->foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server->display)`。

`server_new_xdg_toplevel` 加：
- `view->toplevel_handle = wlr_foreign_toplevel_handle_v1_create(manager)`，`toplevel_handle->data = view`。
- 接线 foreign-toplevel 5 个 listener（request_activate/maximize/fullscreen/minimize/close），handler 转发到 xdg 处理函数。

maximize/fullscreen 处理（真实几何）：见规格 §4.2；minimize 见 §4.3；set_title/set_app_id 见 §4.4；set_parent 见 §4.5。

状态同步函数：每次 maximize/fullscreen/minimize/activate 变更后调用对应 `wlr_foreign_toplevel_handle_v1_set_*`；`set_title/set_app_id` handler 里同步 `set_title/set_app_id`。

`view_destroy` 追加：先摘 foreign-toplevel listener + `wlr_foreign_toplevel_handle_v1_destroy`，再摘 xdg listener + free。

- [ ] **步骤 4：运行测试验证通过**

运行：`meson test -C build --print-errorlogs`
预期：全部 PASS（含 smoke-protocol 出现 zwlr_foreign_toplevel_manager_v1——本任务把任务 3 移出的 zwlr 断言加回）

- [ ] **步骤 5：Commit**

```bash
git add include/woodland.h include/view.h src/shell/shell.c tests/smoke/run_protocol_smoke.sh
git commit -m "feat(shell): implement xdg-shell state handling and foreign-toplevel-v1"
```

---

### 任务 5：回归测试 + 文档更新

**文件：**
- 修改：`docs/superpowers/2026-10-22-woodland-skeleton.md`（标记本里程碑完成）
- 修改：`AGENTS.md`（若模块职责有变化则更新 §代码在哪里）

- [ ] **步骤 1：全量回归**

运行：`meson compile -C build && meson test -C build --print-errorlogs`
预期：编译无警告；全部测试 PASS（test_workspace、test_view、test_log、test_output、test_cursor、test_seat、smoke-protocol、smoke-headless）

- [ ] **步骤 2：ASan 冒烟（可选，若用户要求）**

运行：`meson setup build-asan -Db_sanitize=address,undefined -Dtest=enabled && meson test -C build-asan --print-errorlogs`
预期：单测退出码 0 无报错（DEADLYSIGNAL 刷屏为已知环境问题，M0 已裁决）

- [ ] **步骤 3：更新文档并 Commit**

```bash
git add docs/superpowers/2026-10-22-woodland-skeleton.md AGENTS.md
git commit -m "docs: mark xdg-shell and workspaces milestone complete"
```
