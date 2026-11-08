# layer-shell 支持设计规格

> 日期：2026-10-26
> 状态：已获用户批准（标准深度 + usable_area 集成 + 新 `src/layer/` 模块）
> 参考实现：labwc（`src/layers.c`，GPL-2.0-only，与 woodland 同许可证）

## 1. 背景与目标

woodland 当前（M3 后）的 scene 树结构为：`根 → 10 个工作区树 → xdg 视图`。所有客户端都走 xdg-toplevel，无法承载面板（waybar）、壁纸（swaybg）、通知、启动器（wofi）等**与输出绑定、跨工作区常驻、分层停靠**的客户端。

本规格引入 `zwlr_layer_shell_v1` 协议支持，使：

1. 客户端可按 **background / bottom / top / overlay** 四层停靠在输出边缘。
2. exclusive zone（面板占位）能**缩小窗口 maximize 的可用区域**。
3. keyboard interactivity 三态（exclusive / on_demand / none）参与键盘焦点流转。

参照 labwc `src/layers.c`，但**大幅简化**：无 fractional-scale 通知、无 bottom 层 popup 提到 top 层的重定位、无 usable_area_override 配置、无 cycle OSD / session-lock 树。

## 2. 非目标（显式砍掉）

- layer-shell popup 的 bottom→top 层重定位（labwc `move_popup_to_top_layer`；当前 popup 直接挂在其 layer surface 的 tree 下，满足大部分需求；后续随 LXQt 支持深入可按需补强重定位）。
- fractional-scale 通知（`wlr_fractional_scale_v1_notify_scale`）。
- `usable_area_override` 配置（rc.xml 定制边距）。
- 显示桌面（show-desktop）、cycle OSD 树、session-lock 树。
- layer surface 的 xwayland / SSD / 特效。
- 多输出下 layer surface 分配输出时的"离光标最近的输出"启发式——简化取第一个输出。

## 3. 数据模型

### 3.1 新增 `include/layer.h` + `src/layer/layer.c`

```c
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

bool woodland_layer_init(struct woodland_server *server);
void woodland_layer_finish(struct woodland_server *server);

/** 重排某输出的所有 layer surface，更新 usable_area。 */
void woodland_layer_arrange(struct woodland_server *server,
                            struct woodland_output *output);

/** layer surface 焦点候选：按 keyboard interactivity 决定是否给键盘焦点。 */
void woodland_layer_try_focus(struct woodland_server *server,
                              struct wlr_layer_surface_v1 *layer_surface);
```

### 3.2 `struct woodland_server` 新增字段（`include/woodland.h`）

```c
struct wlr_layer_shell_v1 *layer_shell;
struct wl_listener new_layer_surface;
struct wlr_layer_surface_v1 *focused_layer;   // 当前持有键盘焦点的 layer surface
```

### 3.3 `struct woodland_output` 新增字段（`include/output.h`）

```c
struct wlr_scene_tree *layer_tree[4];   // 下标 = zwlr_layer_shell_v1_layer 枚举
struct wlr_box usable_area;             // 扣除 exclusive zone 后的可用区域
```

### 3.4 模块依赖

- `layer.c` → `woodland.h` + `output.h` + `seat.h`（焦点流转需 view 焦点接口）。
- `shell.c` → `layer.h`（maximize 几何读 usable_area）。
- `workspace.c` → `layer.h`（切换工作区时焦点判断是否 layer surface）。
- 链接已用 `--start-group`，静态库循环引用无碍。

## 4. scene 图层结构与 z-order

layer surface 绑定在具体输出、跨工作区常驻，因此 layer tree 挂在**根 `server->scene->tree` 下，与 10 个工作区树平级**（不挂工作区树内）。

每个输出在 `new_output` 时创建 4 个 layer tree（下标用协议枚举：0=background、1=bottom、2=top、3=overlay）。z-order 从下到上：

```
background → bottom → [工作区树 1..10] → top → overlay
```

实现方式：

- background/bottom 创建后 `wlr_scene_node_lower_to_bottom`（排到工作区树下方）。
- top/overlay 创建后 `wlr_scene_node_raise_to_top`（排到工作区树上方）。
- layer tree 节点位置设为输出在 layout 中的坐标（`wlr_output_layout_get_box`），layer surface 内部用**输出局部坐标** arrange，渲染时自动落到正确全局位置。

输出销毁时 `wlr_scene_node_destroy` 各自 layer tree。

## 5. layer surface 生命周期

`handle_new_layer_surface`（`layer_shell->events.new_surface`）：

1. 若 `layer_surface->output == NULL` → 分配 `server->outputs` 首个输出。
2. 创建 `wlr_scene_layer_surface_v1_create(输出对应 layer_tree, layer_surface)`。
3. 接线 6 个 listener：

| listener | 事件 | 行为 |
|---|---|---|
| map | `surface->events.map` | arrange 该输出；exclusive 交互时给键盘焦点 |
| unmap | `surface->events.unmap` | arrange 更新；焦点若是自己则转移 |
| surface_commit | `surface->events.commit` | 状态字段变化 → 重 arrange；layer 变化 → reparent；interactivity 变化 → `layer_try_focus` |
| output_destroy | `output->events.destroy` | 销毁该 layer surface（输出没了） |
| node_destroy | `scene tree node->events.destroy` | **主清理点**：摘除全部 listener + free（防 UAF，同 labwc） |
| new_popup | `layer_surface->events.new_popup` | 创建 popup scene tree 挂到 layer surface 的 tree 下 |

**销毁顺序保证**：wlroots 保证 layer surface 销毁前先发 unmap（若 mapped）；node_destroy 是 scene 侧清理点，**在此一次性摘除全部 6 个 listener + free `woodland_layer_surface`**。其中 `scene_layer_surface` 可能已被 wlroots 释放，须置 NULL 防误用（同 labwc `handle_node_destroy` 注释）。

**popup**：`new_popup` 时 `wlr_scene_xdg_surface_create(layer_surface 的 tree, popup->base)`，commit 时 `initial_commit` 则 `schedule_configure`；destroy 摘 listener + free。

## 6. arrange 与 usable_area 集成

`woodland_layer_arrange(server, output)` 复刻 labwc `layers_arrange` 核心：

1. `full_area` = `wlr_output_effective_resolution`（0,0,w,h）；`usable_area` = full_area。
2. **先 exclusive**（overlay → background 降序）、**再非 exclusive**（background → overlay 升序），逐一 `wlr_scene_layer_surface_v1_configure(scene, &full_area, &usable_area)`。wlroots 内部完成锚点/边距/尺寸计算，并扣减 positive exclusive zone。
3. 结果写回 `output->usable_area`。
4. 将各 layer tree 节点位置更新为输出在 layout 中的坐标。

触发时机：layer surface map / unmap / commit（状态变化）/ 输出几何变化时。

**maximize 集成**：`woodland_view_maximized_geometry` 改为返回**活动输出（当前简化：第一个输出）的 `usable_area`**，而非输出完整 box。fullscreen 仍用完整 box（全屏应盖过面板）。

## 7. keyboard interactivity 与焦点流转

`server->focused_layer` 记录当前持有键盘焦点的 layer surface。三态语义：

- **EXCLUSIVE**：map 或 commit 时直接拿键盘焦点。
- **ON_DEMAND**：默认不抢焦点；用户**点击**该 surface 时给焦点。
- **NONE**：不参与键盘焦点。

`woodland_layer_try_focus(server, layer_surface)`：

- exclusive → 给键盘焦点（`wlr_seat_keyboard_notify_enter`），当前 xdg view 失活（`set_activated(false)` + foreign 同步）。
- on_demand → 若当前无 exclusive 层持焦点，给焦点。
- none → 若焦点正是自己，转移给顶层 view。

焦点流转：

- layer surface 得焦点：`focused_layer = layer_surface`；当前 xdg view 失活。
- layer surface 失焦点 / unmap / destroy：`focused_layer = NULL`；焦点还给当前工作区顶层 view。
- 指针点击命中 layer surface（`woodland_view_at` 返回 NULL 但命中 layer surface）→ 走 `layer_try_focus`。

**与工作区切换的交互**：`woodland_workspace_switch` 的 clear_focus 逻辑须区分——仅当焦点在 xdg view（属于旧工作区）时清；焦点在 layer surface（跨工作区常驻）时保留。layer surface 不随工作区显隐。

## 8. 测试策略

1. `tests/smoke/run_protocol_smoke.sh`：追加 grep `zwlr_layer_shell_v1` 全局断言（RED→GREEN）。
2. `tests/unit/test_layer.c`（新增）：headless 起 server + 1 个 headless output → `layer_init` → 断言 `layer_shell` 非空、4 个 layer_tree 已创建、z-order 正确（background 节点在工作区树节点之前、overlay 之后）；`layer_finish` 逆序释放无泄漏。layer surface 的 arrange/焦点需要真实客户端交互，留集成测试。
3. 回归：现有 8 个测试全绿（新增后预期 9/9）+ `meson compile` 无警告（werror）。

## 9. 验收标准（DoD）

1. `meson compile -C build` 无警告通过（werror）。
2. `meson test -C build --print-errorlogs` 全部通过（含新增 test_layer）。
3. `wayland-info` 可见 `zwlr_layer_shell_v1` 全局。
4. layer tree 正确分层：background/bottom 在工作区下方，top/overlay 在上方，且跨工作区常驻。
5. exclusive zone 生效：maximize 几何避开有 exclusive zone 的面板。
6. keyboard interactivity：exclusive 层 map 时拿焦点；焦点在 layer surface 时工作区切换不清焦点。
