# Woodland 项目骨架规划

> 日期：2026-10-22
> 状态：骨架规划（供实现前参考）
> 关联规格：`docs/superpowers/specs/2026-10-22-woodland-design.md`
> 参考项目：labwc（meson 构建 + SSD 装饰 + 堆叠窗口管理）、swayfx（scenefx wrap 集成）、mango（scenefx 布局）

## 0. 背景

woodland 是一个纯 C + wlroots 的通用轻量 Wayland 合成器，目标是兼容 treeland 生态（主要是协议）。项目体量很大，不可能一次写完，因此先规划**可编译、可运行的最小骨架**，再按里程碑逐步填充。

## 1. 目录结构（目标形态）

```
woodland/
├── meson.build                  # 顶层构建定义
├── meson_options.txt            # 编译期 feature flags
├── subprojects/                 # meson wrap 依赖（不 vendored，构建时下载）
│   ├── wlroots.wrap             #   revision = 0.20.2
│   └── scenefx.wrap             #   revision = 0.5（随 effects 启用）
├── protocols/                   # 私有协议 XML（从 treeland-protocols 提取）
│   └── *.xml
├── include/                     # 公开头（模块间接口）
│   ├── woodland.h               #   全局状态 struct woodland_server
│   ├── server.h
│   ├── seat.h
│   ├── scene.h
│   ├── view.h
│   ├── shell.h
│   ├── workspace.h
│   ├── layout.h
│   ├── config.h
│   ├── protocol.h               #   私有协议注册/实现框架
│   ├── decorations.h
│   ├── effects.h
│   ├── ime.h
│   ├── wallpaper.h
│   ├── dbus.h
│   └── util/                    #   通用工具（链表、字符串、日志）
├── src/                         # 实现（按职责分层）
│   ├── main.c
│   ├── server/
│   ├── seat/
│   ├── scene/
│   ├── shell/
│   ├── workspace/
│   ├── layout/
│   ├── protocols/
│   ├── config/
│   ├── decorations/
│   ├── effects/
│   ├── ime/
│   ├── wallpaper/
│   ├── dbus/
│   └── util/
├── tests/
│   ├── unit/                    # 单元测试（布局几何、配置解析等）
│   └── integration/             # headless backend 集成测试
└── docs/
```

### 1.1 与参考项目的对照

| 关注点 | labwc | swayfx | woodland 采用 |
| --- | --- | --- | --- |
| 构建 | meson + wrap（wlroots 0.20） | meson + scenefx wrap | meson + 双 wrap |
| 头文件 | `include/` 平铺 + 子目录 | `include/` | `include/` + `util/` |
| 源码 | `src/` 按功能平铺 | `sway/` 集中 | `src/` 按模块子目录（介于两者之间） |
| 特性开关 | `conf_data.set10()` | `wlroots_features` | `conf_data` + `meson_options` |
| scenefx | — | `subproject('scenefx')` + `fallback` | 同 swayfx |

## 2. 构建骨架（meson）

### 2.1 顶层 `meson.build` 关键点

```meson
project('woodland', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-only',
  meson_version: '>=1.3',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
    'wrap_mode=nodownload',   # 显式 meson wrap 下载，见下
  ],
)

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
], language: 'c')

# scenefx（随 effects 选项）
subproject('scenefx', required: false)
scenefx = dependency('scenefx-0.5', fallback: 'scenefx', required: get_option('effects'))

# wlroots（锁定 0.20.x）
wlroots_version = ['>=0.20.0', '<0.21.0']
subproject('wlroots', default_options: ['examples=false'], required: false, version: wlroots_version)
wlroots = dependency('wlroots-0.20', version: wlroots_version, fallback: 'wlroots')

# 其余依赖
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
libinput = dependency('libinput', required: get_option('xwayland') or get_option('...'))
...

# 特性开关 → conf_data
conf_data = configuration_data()
conf_data.set10('WOODLAND_HAVE_EFFECTS', get_option('effects').enabled() and scenefx.found())
conf_data.set10('WOODLAND_HAVE_XWAYLAND', have_xwayland)
configure_file(output: 'config.h', configuration: conf_data)

subdir('protocols')
subdir('include')
subdir('src')
subdir('tests')
```

### 2.2 `subprojects/*.wrap`

```ini
# wlroots.wrap
[wrap-git]
url = https://gitlab.freedesktop.org/wlroots/wlroots.git
revision = 0.20.2

[provide]
dependency_names = wlroots-0.20
wlroots-0.20 = wlroots
```

```ini
# scenefx.wrap
[wrap-git]
url = https://github.com/wlrfx/scenefx.git
revision = 0.5

[provide]
dependency_names = scenefx-0.5
scenefx-0.5 = scenefx
```

> 说明：`wrap_mode=nodownload` 防止 meson 隐式下载，依赖首次由 `meson wrap` 或 `meson subprojects download` 显式拉取；CI 中可改为 `wrap_mode=forcefallback` 或使用 `--wrap-mode` 参数。

### 2.3 `meson_options.txt`

```meson
option('effects', type: 'feature', value: 'disabled',
       description: 'Enable scenefx-based blur/corner/shadow effects')
option('tiling-layout', type: 'feature', value: 'disabled',
       description: 'Enable non-stacking layout modes')
option('xwayland', type: 'feature', value: 'enabled',
       description: 'Enable X11 application support')
option('test', type: 'feature', value: 'disabled',
       description: 'Build and run tests')
```

## 3. 全局状态与模块接口（最小契约）

### 3.1 全局状态

```c
// include/woodland.h
struct woodland_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;

    struct woodland_seat *seat;            // 单用户：主 seat
    struct woodland_scene *scene_manager;  // 图层树管理
    struct woodland_workspace *workspaces; // 工作区列表
    struct woodland_config *config;        // 运行时配置
    struct wl_list views;                  // 所有视图
    struct wl_list outputs;                // 所有输出
};
```

模块间通过 `struct woodland_server *server` 传递，遵循 labwc 的单例 server 模式，避免过度抽象。

### 3.2 视图封装（核心抽象）

```c
// include/view.h
struct woodland_view {
    struct wlr_scene_tree *scene_tree;   // scenefx 增强的 scene 节点
    struct wlr_scene_node *node;
    struct woodland_workspace *workspace;
    struct wlr_box geometry;
    bool mapped;
    // 状态位：maximized / minimized / fullscreen / floating ...
    struct wl_list link;
};

// shell 类型分派
struct woodland_view_impl {
    void (*destroy)(struct woodland_view *view);
    void (*map)(struct woodland_view *view);
    void (*unmap)(struct woodland_view *view);
    void (*set_activated)(struct woodland_view *view, bool activated);
    // ...
};
```

### 3.3 布局接口

```c
// include/layout.h
struct woodland_layout {
    const char *name;
    // 给定工作区与可用区域，重新计算该工作区每个视图的几何
    void (*arrange)(struct woodland_workspace *ws);
};

// 堆叠布局（默认）
extern const struct woodland_layout stacking_layout;
// 平铺布局（tiling-layout 选项启用）
#ifdef WOODLAND_HAVE_TILING
extern const struct woodland_layout tiling_layout;
#endif
```

每个工作区独立持有一个 `const struct woodland_layout *`，运行时切换；布局只算几何，由 `scene` 模块应用（与 sway 的 `arrange` 模式一致）。

## 4. 里程碑拆解

项目太大，按可独立验证的里程碑推进。每个里程碑结束产出**可编译、可运行**的合成器。

### M0：可编译骨架（本周目标）
- meson 骨架 + wrap（wlroots 0.20.2）
- `main.c` + `server`：创建 backend/renderer/allocator/scene，跑通事件循环
- headless backend 下能启动并空跑（无窗口也能运行、退出干净）
- 目录结构、`include/woodland.h`、`util/`（日志、链表）落地
- **验证**：`meson setup build && meson compile -C build` 通过；`woodland` 在 headless 下启动/退出无崩溃

### M1：输出与渲染循环
- [x] 输出热插拔（`wlr_output_layout` + new_output 去重 + init_render + add_auto + scene_output）
- [x] 渲染循环（`wlr_scene_output_layout` + frame 事件 + commit + send_frame_done）
- [x] 光标（`wlr_cursor` + `xcursor_manager`（XCURSOR_THEME/SIZE，默认 24）+ cursor-shape 协议）
- **验证**：`meson test -C build` 4/4 PASS（log/output/cursor/smoke-headless）；ASan 单测无泄漏（smoke 在 ASan 下受 wlroots/ASan 信号处理 flaky 影响，M0 已裁决不纳入 CI）

实现说明：`src/output/output.c`（输出热插拔 + frame 渲染循环）、`src/cursor/cursor.c`（光标加载与设置）、`src/server/server.c`（生命周期接线 + cursor-shape manager）。结构体扩展见 `include/woodland.h`。

### M2：基础窗口管理（xdg-shell + xwayland）
- `shell/` + `view`：xdg-toplevel/popup 视图封装、xwayland 视图
- `seat/`：libinput 输入、焦点、指针约束、拖拽
- `scene/`：图层树（背景/窗口/顶层/overlay）
- `workspace/`：10 工作区、切换
- `layout/`：堆叠布局（默认）
- **验证**：能启动并操作多个 Wayland/X11 窗口，工作区切换，窗口状态（最大化/最小化/全屏）

> **进展（2026-10-26）**：M2 部分完成。tinywl 能力补齐已合入（PR #1）；xdg-shell 完整状态处理（maximize/fullscreen/minimize/set_title/set_app_id/set_parent）+ 10 工作区（Alt+1~9/0 切换）+ ext-workspace-v1 + foreign-toplevel-v1 已在 `feature/m3-xdg-workspaces` 实现；layer-shell（zwlr_layer_shell_v1 四层 + exclusive zone + 焦点）已在 `feature/m4-layer-shell` 实现；SSD（zxdg_decoration_manager_v1 + labwc 默认样式对齐）已在 `feature/m6-ssd` 实现。
>
> **进展（2026-10-27，xwayland 基础支持）**：`feature/m8-xwayland` 补上 X11 视图，M2 收尾。做法不是给 X11 另起一套窗口管理，而是把 `struct woodland_view` 抽成**双后端**：新增 `type`（XDG / XWAYLAND）+ `xwayland_surface`，并把「surface / 内容矩形 / 请求尺寸 / 激活态 / 关闭」收进 `woodland_view_surface()`、`woodland_view_content_box()`、`woodland_view_request_size()`、`woodland_view_set_activated()`、`woodland_view_close()` 五个访问器；map/unmap/destroy/放置/最大化/全屏/最小化/焦点/贴边吸附全部两端共用，`ssd.c`、`seat.c`、`layer.c` 里写死 `xdg_toplevel` 的地方也改走访问器 —— X11 窗口因此**直接复用**同一套 labwc 风格 SSD 与吸附逻辑。
> `src/xwayland/`：`wlr_xwayland_create(..., lazy=true)`（首个 X 客户端连接才拉进程）、`wlr_xwayland_set_seat()`、`server_start` 里导出 `DISPLAY`。时序按 labwc：`map_request`（尺寸已知 → 算放置 → `wlr_xwayland_surface_configure` 回 configure）→ `associate`（建 scene surface、挂 surface map/unmap/commit）→ `map`（建 SSD、进工作区、聚焦）。`_MOTIF_WM_HINTS` 请求无边框的窗口（`decorations == NO_BORDER`）不画 SSD。`override-redirect` 窗口走 unmanaged：单独一棵 scene 树按 X 侧坐标定位，不建 view、不进窗口列表、不参与焦点（菜单/提示/拖拽影子）。标题栏关闭按钮走 `wlr_xwayland_surface_close`，最大化/全屏走 `wlr_xwayland_surface_set_maximized/fullscreen` + 我们自己 configure 尺寸。
> `-Dxwayland=disabled` 会连带关掉 wlroots 子项目的 xwayland 支持，因此 `shell.c` 里的 X11 分支用 `#if WOODLAND_HAVE_XWAYLAND` 一起编译掉。
> 验证：新增 `tests/smoke/run_xwayland_smoke.sh` + `tests/smoke/xoverride.c`（建 override-redirect 窗口的小客户端），断言 xterm 出现在 `lswt` 列表、截图顶部 26px 有 `#E1DEDB` 标题栏底色、unmanaged 窗口不产生 toplevel 但红色像素可见；`meson test` 13 项全绿。手动验过 xterm 最大化（`wmctrl -b add,maximized_vert,maximized_horz` → 内容 1280x694、标题栏让出 26px）、层叠、以及两个 weston 客户端回归。剩余：X11 的面板/dock 类窗口、拖拽（DND）、`_NET_WM_ICON` 图标、treeland-xwindow-control。
>
> **进展（2026-10-27，位置管理与焦点管理）**：把原先散在 `shell.c` 里的「放哪 / 谁有焦点」两摊逻辑抽成两个独立模块，参考 labwc 对齐语义。
> `src/shell/placement.c` + `include/placement.h`（位置管理）：输出感知（`woodland_output_for_view()` 用外框中心求最近输出，没有 layout 时退首个输出）、工作区（`woodland_output_layout_get_box` ∩ `usable_area`）、四种放置策略（`cascade` 默认，步进 32px、绕 8 圈回位；`center`；`cursor`；`autoplace` 按 labwc `placement_find_best()` 的重叠网格思路——候选 = 四角 + 层叠位 + 每个障碍窗的 4 条贴靠位，取重叠面积最小、并列时取离工作区左上最近，避免第二扇窗被甩到屏幕对角）、`woodland_view_constrain()` 越界拉回、贴边铺满（`woodland_snap_edges_for()` / `woodland_snap_tile_box()`：左/右半屏、上边最大化、四角 1/4 屏，判定顺序左右优先于上下，所以四角是半屏+半高而不是全屏）、拖动已贴边窗口时先恢复原尺寸再挂到光标下、resize 走 `woodland_view_size_limits()`（xdg 取 `current.min/max_width`，X11 取 `xcb_size_hints_t` 且 `base_size` 视作最小）。策略可用 `-p <cascade|center|cursor|autoplace>` 选。
> `src/shell/focus.c` + `include/focus.h`（焦点管理）：统一入口 `woodland_focus_view(view, raise)`——最小化先恢复、不在当前工作区先切工作区、模态对话框（`xdg_wm_dialog_v1` 与 X11 `modal`）接管、可聚焦性校验、三态同步（`wlr_xdg_toplevel_set_activated` / SSD 激活色 / foreign-toplevel activated）、raise 到 z 序头、写 `server->focused_view`、keyboard enter，全部收在一处。`woodland_focus_fallback()` 负责「焦点没了给谁」：优先还 layer surface，其次取 z 序最上视图，都没有才 `woodland_focus_none()`；unmap / destroy / 最小化 / 工作区切换 / layer 销毁全部接上。`woodland_focus_cycle()` 实现 Alt+Tab（`server->views` 即 z 序，Shift 反向）。layer 抢焦点时 `woodland_focus_yield_to_layer()` 让位，交还时 fallback。
> 另外补了 `server->focused_view` 显式记录：headless（无键盘设备）下 `wlr_seat_keyboard_notify_enter` 不写 `focused_surface`，之前靠反查 `focused_surface` 的焦点判断会恒为 NULL，导致两扇 X11 窗口标题栏同时是激活色。
> 验证：新增 `tests/unit/test_placement.c`（贴边铺满 / 贴边判定 / 约束 / 居中 / 光标 / autoplace / 多输出 / 尺寸限制 / 策略解析）与 `tests/unit/test_focus.c`（可聚焦判定 / `focus_topmost` / `focus_pick` 轮转），`meson test` 15 项全绿；`-Dxwayland=disabled` 构建通过；cascade 与 autoplace 截图对比确认摆放；`run_xwayland_smoke.sh` 加焦点回退断言（`#E1DEDB` 激活 / `#F7F6F4` 非激活）、`run_protocol_smoke.sh` 加 `xdg_wm_dialog_v1` 断言。
>
> **进展（2026-10-29，shell 按职责再分层）**：`shell.c` 在位置/焦点抽走之后仍剩 1528 行，混着窗口状态、交互拖动、SSD 按钮、光标分发四摊互不相干的东西；改任何一处都要先在同一个文件里穿过来回跳转的 static 函数。
> 按"谁拥有状态、谁只读它"切成四个文件（同一 `shell_lib`，只有 `src/shell/shell_internal.h` 里 9 个符号跨文件，其余全 static）：`shell.c`（607 行）xdg toplevel 生命周期、popup、装饰协商、双后端访问器；`toplevel_state.c`（413 行）max/fullscreen/minimized/title/app_id/parent 与 foreign-toplevel 句柄及请求；`interactive.c`（333 行）move/resize 状态机、拖动吸附累积与松手铺满；`cursor.c`（292 行）wlr_cursor 事件分发与 SSD 按钮 hover/pressed。
> 边界取"单向依赖"：`cursor → interactive → toplevel_state`，反过来不引用，否则又会绕回互引用。吸附的**判定**仍留在 `placement.c`（纯几何、可单测），`interactive.c` 只管拖动过程中累积候选边和松手后执行，两处不再各算一套。监听器注册也随职责走：`shell_interactive_attach()` / `shell_toplevel_state_attach()` 由 `shell.c` 在建视图时各调一次，注册逻辑不再散在三个文件里。
> 验证：`meson test` 18/18、`-Dxwayland=disabled` 构建通过、ASan/UBSan 18/18；纯搬移，无行为变化。

> **进展（2026-10-29，调试面：状态可查询 + 不变量自检）**：解决的是「改完怎么知道破坏了什么」。此前 PR #8 的五条评审意见里有三条不是崩溃，而是**状态之间不自洽**（unmap 后 foreign-toplevel 仍是激活态、副屏窗口按主屏工作区最大化、非激活标题栏颜色对不上），只能靠读截图数像素或读 wlroots 源码去推。
> `src/ipc/` + `include/ipc.h`：在 `$XDG_RUNTIME_DIR/woodland-<wayland socket>.sock` 上开只读调试接口，**换行分隔 JSON**，一行命令一行回应，路径导出到 `WOODLAND_IPC`。命令：`get_tree`（每个视图的 id / kind / title / app_id / workspace / output / z / mapped / focused / activated / minimized / maximized / fullscreen / override_redirect / csd / outer / content / `ssd{margins,active,present}` / limits / snapped / parent，加上输出的 box 与 workarea、当前工作区、当前焦点）、`get_focus`（含 `recorded_view` 与 `keyboard_view`，正好能看出「记的焦点」和「键盘实际在哪」是否分叉）、`self_check`、`dump_scene`（`wlr_scene` 树：节点类型 / 相对坐标 / 尺寸 / 矩形颜色 / opacity，深度上限 6、节点上限 2000）、`ping`、`help`。
> 客户端 `src/wctl/`（只依赖 libc）：`-s` / `$WOODLAND_IPC` / 扫 `$XDG_RUNTIME_DIR` 里最新的 `woodland-*.sock` 三级查找，连不上按超时重试（合成器刚起时 socket 还没 bind 是常态），`ok=false` 时退出码 1，所以 `wctl self_check || ...` 可以直接写进脚本。输出是原始单行 JSON，`wctl get_tree | jq` 与 `grep` 两种土洋用法都成立。JSON 由 `src/util/json.c` 手写拼装（不引库）：NULL → `null`、控制字符转义、非法 UTF-8 字节替换成 U+FFFD —— 标题来自客户端，一个脏字节就能让整条响应在 jq 里解析失败。
> `src/shell/selfcheck.c` + `include/selfcheck.h`：八条不变量 —— `views_list_intact`（链表无环、在链表里就意味着 mapped，手写遍历避免 `wl_list_for_each` 遇环转死）、`mapped_in_workspace`（scene_tree 确实挂在该工作区树下）、`focused_view_valid`（在链表里、可聚焦、有 surface，且与 seat 的键盘 surface 指向同一视图）、`activated_unique`（激活态至多一个且必须是焦点视图）、`ssd_active_matches`、`ft_activated_matches`、`xdg_activated_matches`（比 `scheduled` 而不是 `current`，否则客户端没 ack 就会假警报）、`ssd_geometry_matches`（SSD 缓存的内容尺寸 == 当前内容尺寸，抓「拉宽后按钮还停在旧位置」）。刻意**不**检查「窗口几何 == 期望几何」：configure 是否被 ack 是异步的，那种断言只会制造噪音。
> `--self-check` 打开后每次状态变化在 **idle** 阶段跑一遍（map 途中会连着改 activated / SSD / foreign-toplevel，中途检查必然看到半成品），失败按 `self-check <name> failed after <reason>: expected .., got ..` 记 ERROR，**不退出进程**（调试期崩掉合成器比看到报警更糟），`wctl self_check` 则返回结构化的 `{"ok":false,"failures":N,"checks":[...]}`。
> 验证：新增 `tests/unit/test_json.c`、`tests/unit/test_selfcheck.c`（空状态、单窗口、双窗口同时激活、焦点指向最小化窗口、scene_tree 挂错工作区、xdg configure 与记录不一致，逐条断言失败信息指向正确的 view#id）与 `tests/smoke/run_ipc_smoke.sh`（起真实客户端后断言 `count == 1`、`focused/activated`、内容框 = 外框 + 27px 标题栏、`workarea == 1280x720`、`self_check.ok`、`dump_scene.nodes > 0`、未知命令退出码非 0、同一连接连发两条、退出后 socket 被清理、日志里没有 self-check 失败），`meson test` **18 项全绿**；`-Dxwayland=disabled` 与 ASan/UBSan 构建通过。反向验过自检确实能抓 bug：删掉 `woodland_view_set_activated()` 里的 `view->activated = activated` 之后，`activated_unique` / `ssd_active_matches` / `ft_activated_matches` / `xdg_activated_matches` 四条同时报错，`smoke-ipc` 直接失败。


### M3：SSD 装饰 + 特效（可选）
- `decorations/`：SSD 标题栏/边框/阴影（参考 labwc `src/ssd/`）
- `effects/`：scenefx 模糊/圆角/阴影，编译期+运行期开关
- **验证**：SSD 窗口正常显示装饰；`effects=false` 时零特效开销；`effects=true` 时模糊/圆角生效

> **进展（2026-10-26）**：SSD 已完成（`feature/m6-ssd`）。`zxdg_decoration_manager_v1` 协商（按策略位决定，请求 client-side 才回 client-side，未声明时默认 server-side）、`src/ssd/ssd.c` 标题栏/边框/拖拽命中、`src/ssd/text_buffer.c`（pango+cairo 渲染标题、cairo 绘制按钮符号）。默认样式对齐 labwc `theme_builtin()`：边框 1px `#aaaaaa`、标题栏 active `#e1dedb` / inactive `#f6f5f4`、标题 `#000000` 居中（Sans 10）、按钮 26×26 黑色符号（`icon:iconify,max,close` 右段）。几何按 labwc 的 margin 模型：`woodland_ssd_get_margins()` 给出装饰外扩（普通 1/27/1/1、最大化仅标题栏 26、全屏/CSD 为 0），窗口放置与最大化/全屏都用它让位，标题栏不会被输出边缘裁掉。贴边与放置：拖动时外框吸附工作区边缘与邻窗边缘（阈值 20px，越阈值自然脱离）、transient 相对父窗口居中（越界贴回工作区）、按钮 hover/按下态（hover `#80808020` 对齐 labwc，按下 `#80808059` 近似凹陷；动作改为释放时触发）。贴边分屏（drag-to-tile）已在后续「位置管理」一版补上（左/右半屏、上边最大化、四角 1/4 屏）。剩余：阴影与圆角（属 `effects/`）、窗口图标按钮。

### M4：协议兼容（私有协议）
- `protocols/` 框架：XML 提取 + 协议实现分派
- 按规格 §7 分级推进：先上游协议（wlroots 自带），再 public 6 个，再 dde 通用子集
- **验证**：协议握手测试通过（复用 treeland 测试客户端思路）

> **进展（2026-10-27）**：截图/录屏协议已完成（`feature/m7-capture`，`src/capture/`）。`zwlr_screencopy_manager_v1`（grim 等老客户端）、`ext_image_copy_capture_manager_v1`、`ext_output_image_capture_source_manager_v1`、`ext_foreign_toplevel_image_capture_source_manager_v1` + `ext_foreign_toplevel_list_v1` 全部建好。按窗口取源由 `new_request` 惰性创建（`wlr_ext_image_capture_source_v1_create_with_scene_node(view->scene_tree)`，源随 scene 节点销毁），抓的是「窗口 + SSD 装饰」整棵子树，没有像 labwc 那样额外维护无装饰的 capture 场景树。顺带补了 `zxdg_output_manager_v1`（grim 没有它只能按物理模式猜布局，`-g` 区域截图会算成 0x0）。验证：`wayland-info` 断言全局、`lswt -j` 列表带 identifier、`grim` 整屏 1280x720 / 区域 200x150 / `grim -T <identifier>` 单窗口 252x278，均进 `meson test`（`smoke-capture`）。

> **进展（2026-10-29，上游协议接线：12 个）**：此前只有 capture 那一组协议，`viewporter` / `xdg-activation` / `presentation-time` / `idle-inhibit` 这些"现代客户端默认就会用"的接口全缺，表现是启动器拉起的窗口不抢焦点、播视频时屏保照跳、锁屏无从谈起。
> 新建 `src/protocol/`（`include/protocol.h` + 五个 .c），把"创建即接线"的上游协议从 `server.c` 里分出来——`server.c` 只该管生命周期骨架，十几个协议塞进去会把真正重要的初始化顺序淹掉。模块内部按关注点分文件（`idle.c` 空闲与抑制 / `output.c` 输出管理 / `input.c` 输入与选区 / `window.c` 窗口语义），`protocol.c` 只剩按序建立、逆序回收与三个转发入口：单文件堆到 700 行时，init 的失败回滚和 finish 的逆序拆除要横跨十几组监听器，漏一行就是悬挂指针；拆开后每族的建立与回收在同一个文件里，加协议只动一处。状态也随之下沉——各族的监听器与链表定义在 `.c` 内，`struct woodland_server` 只留一个不透明的 `struct woodland_proto *`，不再为每个协议加两行字段。
> **语义完整（客户端请求真的改变行为）**：`wp_viewporter`（scene 自带 viewport 处理）、`wp_presentation`（帧反馈由 `types/scene/surface.c` 在提交时自己上报，创建完就够）、`zwp_primary_selection_device_manager_v1`（接 seat 的 `request_set_primary_selection`）、`zwp_relative_pointer_manager_v1`（从光标 motion 事件转发，毫秒转微秒）、`zwp_pointer_gestures_v1`（按 `wlr_pointer` 设备挂 8 个监听器转发 swipe/pinch/hold）、`ext_idle_notifier_v1` + `zwp_idle_inhibit_manager_v1`（键盘与光标事件续命，抑制器数量变化时重算聚合态）、`zwp_keyboard_shortcuts_inhibit_manager_v1`（焦点 surface 持抑制器时合成器键位让路）、`zwlr_output_power_manager_v1`（DPMS 开关落到 `wlr_output_state_set_enabled`）、`zwlr_output_manager_v1`（`wlr_output_head_v1_state_apply` 落盘 + 布局位置单独写回 + 布局变化后重发配置）、`xdg_activation_v1`（按 surface 找视图再聚焦/置顶）。
> **仅记录**：`xdg_toplevel_tag_manager_v1` —— tag 存到 `view->tag`，`wctl get_tree` 能看见；窗口规则（M6）落地后才参与匹配。
> **明确不接**：`security-context-v1`（没有沙箱校验时，客户端声明"我是沙箱"是假安全，做错比不做更糟）、`tearing-control-v1`（要把 tearing 位传进 scene 的 commit 路径，属单独一版）、`content-type-v1`（scene 不消费，接了等于没接）、`session-lock` / `pointer-constraints` / `xdg-foreign` / `drm-lease`（各自需要一套独立状态机，见下一版）。
> 三处容易做错的地方：**抑制器的激活态必须跟着键盘焦点走**，所以挂在 seat 的 `keyboard_state.events.focus_change` 上，而不是在 `focus.c` 的每个出口各插一次调用（`layer.c` 也直接 `notify_enter`，漏一处就留下"旧窗口还在吞键"的死角）；**输出配置广播不能在 apply 期间发**，否则客户端分不清哪份是对自己请求的回应，用协议族内部的 `applying` 标志压住、apply 完再补一次；**idle 抑制器只有"新增"事件**，每个抑制器挂销毁监听，任一销毁就重算 `wlr_idle_notifier_v1_set_inhibited`，否则最后一个消失后 idle 一直停着。
> 验证：`tests/smoke/run_protocol_smoke.sh` 扩到 19 个全局对象断言，并加一次 primary-selection 端到端（`wl-copy --primary --paste-once` → `wl-paste --primary` 必须读到同样的内容，只有握手没有行为的协议等于没接）；`meson test` **18/18**，`-Dxwayland=disabled` 17/17，ASan/UBSan 12 个单测全清、ASan 下冒烟无报告。反向验过两次：去掉 `wlr_viewporter_create` 后「missing global」报错，不调 `wlr_seat_set_primary_selection` 后「primary selection round trip failed (got '')」。

### M5：IME + 壁纸 + 屏保
- `ime/`：input-method-v2 + text-input-v3 + 内置候选面板
- `wallpaper/`：wallpaper-manager + wallpaper-shell（合成器不渲染壁纸）
- `dbus/`：session D-Bus 屏保接口
- **验证**：中文输入法候选面板显示；壁纸客户端可渲染壁纸；D-Bus 屏保可激活

### M6：wine 协议（低优先级）+ 打磨
- wine 3 协议
- 配置 key-value 协议、多任务视图、任务切换器、贴边等窗口管理特性补全
- **验证**：规格 §16 DoD 全项通过

## 5. 首个实现计划的范围

本骨架规划对应 **M0（可编译骨架）**。M0 是后续所有里程碑的地基，建议作为第一个 writing-plans 实现计划的主题：

**M0 交付物：**
1. 完整目录结构（空模块 + 头文件桩）
2. `meson.build` / `meson_options.txt` / `subprojects/*.wrap`
3. `util/` 日志与链表
4. `main.c` + `server`：backend/renderer/allocator/scene + 事件循环，headless 可运行
5. `config.h` 特性开关生成
6. `.clang-format` + 基础 CI（`meson setup && compile`）

M0 不涉及窗口管理、协议、特效——只求"能编译、能跑、结构就位"。

## 6. 与规格的关系

骨架规划是实现前的**结构预演**，不替代 writing-plans 的详细实现计划。详细计划会按 M0 的范围展开成可勾选的任务步骤（TDD、测试、commit），并在 `docs/superpowers/plans/` 下产出。
