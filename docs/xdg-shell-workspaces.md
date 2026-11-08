# xdg-shell 完整支持与工作区设计规格

> 日期：2026-10-26
> 状态：已获用户批准（方案 A + 六节设计）
> 参考实现：labwc（GPL-2.0-only，与 woodland 同许可证）

## 1. 背景与目标

woodland 当前（M2 后）的 `src/shell/shell.c` 处于 tinywl 级：xdg-toplevel 视图只做 map/unmap/commit + 交互式 move/resize，`request_maximize`/`request_fullscreen` 只是空 `schedule_configure` 应答，不改几何；无工作区概念。

本规格将：

1. **补齐完整 xdg-shell 支持**：maximize / fullscreen / minimize / set_title / set_app_id / set_parent 产生真实行为。
2. **引入工作区系统**：固定 10 个工作区，键盘切换，视图归属与显隐。
3. **实现两个上游协议**供外部客户端集成：
   - `zwlr_foreign_toplevel_manager_v1`：任务栏/面板枚举窗口、激活（恢复最小化窗口）、最大化/全屏/最小化/关闭。
   - `ext-workspace-v1`：外部工作区切换器枚举与激活工作区。

参照 labwc，但**大幅简化**：无 action 系统、无 rc.xml 配置、无 SSD、无 OSD、无 snap，纯 C 单体结构。

## 2. 非目标（显式砍掉）

- 服务器端装饰（SSD）、圆角、阴影、窗口贴边（snap）。
- 窗口菜单（show_window_menu）。
- action 系统、rc.xml 配置解析、key-value 协议运行时配置。
- 任务切换器（alt-tab）、多任务视图（overview）、显示桌面（show-desktop）。
- xwayland 视图（留到 xwayland 里程碑）。
- ext-workspace-v1 的 create/deactivate/assign/remove 请求（10 工作区固定，仅支持 activate）。
- foreign-toplevel 的 `set_rectangle`（任务栏缩略图矩形）语义处理。

## 3. 数据模型

### 3.1 新增 `include/workspace.h` + `src/workspace/workspace.c`

```c
struct woodland_workspace {
    struct wl_list link;                 // server.workspaces
    struct woodland_server *server;
    char *name;                          // "1".."10"
    struct wlr_scene_tree *tree;         // 挂 server->scene->tree 下的子树
    struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

bool woodland_workspace_init(struct woodland_server *server);
void woodland_workspace_finish(struct woodland_server *server);
void woodland_workspace_switch(struct woodland_server *server,
                               struct woodland_workspace *target);
struct woodland_workspace *woodland_workspace_by_name(
    struct woodland_server *server, const char *name);
```

### 3.2 `struct woodland_server` 新增字段（`include/woodland.h`）

```c
struct wl_list workspaces;               // woodland_workspace.link
struct woodland_workspace *active_workspace;
struct wlr_ext_workspace_manager_v1 *ext_workspace_manager;
struct wlr_ext_workspace_group_handle_v1 *ext_workspace_group;
struct wl_listener ext_workspace_commit;
struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
```

### 3.3 `struct woodland_view` 新增字段（`include/view.h`）

```c
struct woodland_workspace *workspace;    // 归属工作区
char *title;
char *app_id;
bool mapped;
bool minimized;
bool maximized;
bool fullscreen;
struct wlr_box saved_geometry;           // 退出 max/fullscreen 时恢复
struct wlr_foreign_toplevel_handle_v1 *toplevel_handle;
struct wl_listener set_title;
struct wl_listener set_app_id;
struct wl_listener set_parent;
struct wl_listener request_minimize;
```

### 3.4 模块依赖

- `workspace.c` → `woodland.h` + `view.h`（切换时聚焦视图）。
- `shell.c` → `workspace.h`（map 时挂到 active_workspace->tree）。
- `seat.c` → `workspace.h`（Alt+数字键切换）。
- 链接已用 `--start-group`，静态库循环引用无碍。

## 4. xdg-shell 行为

### 4.1 几何纯函数（可单测）

```c
// view.h
struct wlr_box woodland_view_maximized_geometry(struct woodland_server *server);
struct wlr_box woodland_view_fullscreen_geometry(struct woodland_server *server,
                                                 struct wlr_output *output);
```

- maximize 几何 = 活动工作区视图的 `output_layout` box（当前实现的简化：取输出 layout box；多输出时用视图所在输出）。
- fullscreen 几何 = 指定输出的完整 box（不裁剪 usable 区域）。

### 4.2 maximize / fullscreen

`request_maximize` / `request_fullscreen` 处理逻辑（两者对称）：

1. 首次请求（true）：
   - 记 `view->saved_geometry`（当前 geometry）。
   - 置 `view->maximized = true`（或 `fullscreen`）。
   - `wlr_xdg_toplevel_set_maximized(t, true)` / `set_fullscreen(t, true)`。
   - `wlr_xdg_toplevel_set_size(t, w, h)` + 移动 scene 节点到 (x, y)。
   - `wlr_xdg_surface_schedule_configure`。
2. 再次请求（false）：恢复 `saved_geometry` + 对应 `set_*(false)`。

### 4.3 minimize

- `request_minimize(true)` → `view->minimized = true`；`wlr_scene_node_set_enabled(&view->scene_tree->node, false)`；若焦点视图则清键盘焦点；`set_minimized(true)` 同步到 foreign-toplevel。
- **恢复入口**：foreign-toplevel 的 `request_activate`（任务栏点击）→ `woodland_view_focus(view)`，focus 开头检测 `minimized` 则先恢复（`set_enabled(true)` + `minimized=false` + `schedule_configure` + `set_minimized(false)` 同步）。

### 4.4 set_title / set_app_id

- 保存到 `view->title` / `view->app_id`（wlr 分配的字符串，直接引用，wlr 负责释放）。
- 同步 `wlr_foreign_toplevel_handle_v1_set_title/set_app_id`。

### 4.5 set_parent

- 记录父关系；新窗口 map 时若有 parent，inherit 父的 workspace。

## 5. 工作区

### 5.1 初始化

`woodland_workspace_init`：创建 10 个工作区（name "1"~"10"），每个挂一棵 `wlr_scene_tree` 到 `server->scene->tree`；只有工作区 1 `set_enabled(true)`；`active_workspace = ws1`。

### 5.2 视图归属

`xdg_toplevel_map` 时：`view->workspace = server->active_workspace`；`wlr_scene_node_reparent(&view->scene_tree->node, &ws->tree->node)`。

### 5.3 切换

`woodland_workspace_switch(server, target)`：

1. 旧 ws `set_enabled(false)`，新 ws `set_enabled(true)`。
2. 更新 `active_workspace` + ext_workspace `set_active` 标志（旧的 false、新的 true）。
3. 焦点移到新 ws 顶层视图；无视图则清键盘焦点。

### 5.4 键位（seat.c，Alt 修饰）

- `Alt+1` ~ `Alt+9` → 工作区 1~9；`Alt+0` → 工作区 10。
- 保留 `Alt+Esc`（退出）、`Alt+F1`（切窗，改为只切**当前工作区**内的窗口）。

## 6. 协议

### 6.1 ext-workspace-v1

对齐 labwc 最小实现：

1. `wlr_ext_workspace_manager_v1_create(display, 1)`。
2. `wlr_ext_workspace_group_handle_v1_create(manager, 0)`。
3. 每个工作区 `wlr_ext_workspace_handle_v1_create(manager, NULL, ACTIVATE)` + `set_group` + `set_name("1".."10")` + `set_active(当前)`。
4. 监听 `manager->events.commit`，遍历 requests；`ACTIVATE` → `woodland_workspace_switch`；其余请求类型忽略。

### 6.2 foreign-toplevel-v1

1. `shell_init` 时 `wlr_foreign_toplevel_manager_v1_create(display)`。
2. `new_toplevel` 时 `wlr_foreign_toplevel_handle_v1_create`，存 view，接线 5 个请求监听器：
   - `request_activate` → `woodland_view_focus(view)`
   - `request_maximize` → 复用 xdg maximize 处理
   - `request_fullscreen` → 复用 xdg fullscreen 处理
   - `request_minimize` → 真正隐藏
   - `request_close` → `wlr_xdg_toplevel_send_close`
3. 状态同步：title/app_id/activated/maximized/minimized/fullscreen 变更时同步 set_*。
4. 销毁：`view_destroy` 先 `wlr_foreign_toplevel_handle_v1_destroy`。

## 7. 测试策略

1. `tests/unit/test_workspace.c`（新增）：headless 起 server → `workspace_init` → 断言 10 个工作区、active 标志、ext_workspace 非空；`workspace_switch` 到 ws3 → 断言 tree enabled 翻转、active_workspace 更新；逆序释放无泄漏。
2. `tests/unit/test_view.c`（新增）：headless + 1 个 headless output → 断言 `maximized_geometry` / `fullscreen_geometry` 纯函数返回正确 box（800x600 输出）。
3. `tests/smoke/run_protocol_smoke.sh`：追加 grep `ext_workspace_manager_v1` 与 `zwlr_foreign_toplevel_manager_v1` 两个全局。
4. 回归：现有 6 个测试全绿 + `meson compile` 无警告（werror）。

## 8. 验收标准（DoD）

1. `meson compile -C build` 无警告通过（werror）。
2. `meson test -C build --print-errorlogs` 全部通过（含新增 test_workspace、test_view）。
3. 工作区切换键位（Alt+1~9/0）生效；视图随工作区显隐。
4. maximize / fullscreen 产生真实几何变化并可恢复。
5. minimize 隐藏窗口；foreign-toplevel `request_activate` 可恢复。
6. wayland-info 可见 `ext_workspace_manager_v1` 与 `zwlr_foreign_toplevel_manager_v1` 全局。
