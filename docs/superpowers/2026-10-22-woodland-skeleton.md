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
  license: 'MIT',
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
- 输出热插拔、`wlr_output_layout`、渲染循环（`wlr_scene_output_layout`）
- 光标（`wlr_cursor` + `cursor-shape`）
- **验证**：DRM/headless 下输出出现，光标显示，空 scene 稳定渲染

### M2：基础窗口管理（xdg-shell + xwayland）
- `shell/` + `view`：xdg-toplevel/popup 视图封装、xwayland 视图
- `seat/`：libinput 输入、焦点、指针约束、拖拽
- `scene/`：图层树（背景/窗口/顶层/overlay）
- `workspace/`：12 工作区、切换
- `layout/`：堆叠布局（默认）
- **验证**：能启动并操作多个 Wayland/X11 窗口，工作区切换，窗口状态（最大化/最小化/全屏）

### M3：SSD 装饰 + 特效（可选）
- `decorations/`：SSD 标题栏/边框/阴影（参考 labwc `src/ssd/`）
- `effects/`：scenefx 模糊/圆角/阴影，编译期+运行期开关
- **验证**：SSD 窗口正常显示装饰；`effects=false` 时零特效开销；`effects=true` 时模糊/圆角生效

### M4：协议兼容（私有协议）
- `protocols/` 框架：XML 提取 + 协议实现分派
- 按规格 §7 分级推进：先上游协议（wlroots 自带），再 public 6 个，再 dde 通用子集
- **验证**：协议握手测试通过（复用 treeland 测试客户端思路）

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
