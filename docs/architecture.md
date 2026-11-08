# Woodland 合成器设计规格

> 日期：2026-10-22
> 状态：已获用户批准（设计方向 + 中文文档）
> 调研对象：`/home/ut004971@uos/work/treeland`（含 waylib、treeland-protocols）

## 1. 背景与动机

treeland 是 deepin 桌面的 Wayland 合成器，架构为三层：vendored wlroots 0.20.2（C）→ waylib（Qt/C++ 封装库）→ treeland（Qt6/QML 桌面策略层）。它深度绑定 DDE 生态：DDM 登录管理器、DConfig 配置中心、DTK 组件、D-Bus(logind)、Qt Remote Objects 调试通道，以及 30+ 私有 Wayland 协议。

Woodland 是一个基于 C + wlroots 的通用轻量 Wayland 合成器，核心目标聚焦于三大方向：
1. **兼容 treeland 生态，可以运行 DDE 桌面环境**：常规单用户支持（运行在用户 session 内），提供朴素的特效（基于 scenefx 适度提供模糊、圆角、阴影等，克制简洁、性能优先，不复刻 treeland 臃肿逻辑与重度绑定）；
2. **支持运行 noctalia / quickshell / lxqt**：作为通用轻量合成器，全面支持现代 layer-shell 壳体组件（Quickshell、Noctalia Shell）与轻量桌面环境（LXQt Wayland 会话）；
3. **发行版优先支持 deepin / arch**：优先保障 deepin 与 Arch Linux 环境下的构建兼容、依赖适配、打包与开箱运行体验。

## 2. 目标与非目标

### 2.1 定位

**通用轻量 Wayland 合成器**，常规单用户支持，运行在用户 session 内：
- **兼容 treeland 生态与 DDE 运行**：能够完整运行 DDE（Deepin Desktop Environment）桌面环境及其外壳组件（dde-shell、dock、壁纸客户端、控制中心等），实现 treeland 私有协议的高效兼容与语义闭环，内部架构独立简约，不复刻 treeland 的冗余逻辑。
- **支持现代与轻量桌面生态**：支持运行 Noctalia、Quickshell 定制壳体以及 LXQt 桌面环境，标准 Wayland 协议语义严谨完整。
- **常规单用户与朴素特效**：定位为标准单用户 session 合成器，提供实用、克制的“朴素特效”，追求极致性能与代码整洁。
- **优先面向目标发行版**：深度适配 deepin 与 Arch Linux 两个主要发行版生态。

### 2.2 目标

1. **兼容 treeland 生态与 DDE 运行**：
   - 纯 C + wlroots 实现，无 Qt/QML/DTK 强依赖。
   - 兼容 treeland 私有协议（见第 7 节分级），满足 DDE 桌面环境各组件（dde-shell/dock、壁纸服务、控制中心、全局快捷键等）正常运行与交互。
   - 常规单用户设计：通过常规 display manager 或 TTY session 启动，无需承担多用户登录管理器（DDM）中转复杂性。
   - 朴素的特效：基于 scenefx 提供模糊、圆角、阴影等轻量视觉呈现，渲染管线以 `wlr_scene` 直出为基底，拒绝过度消耗资源的重度 3D/复杂转场。
2. **支持运行 noctalia / quickshell / lxqt**：
   - 严谨实现 upstream wlroots 协议矩阵：完备支持 `zwlr_layer_shell_v1`（四图层、exclusive zone 避让、键盘交互三态）、`wlr_foreign_toplevel_management_v1`、`xdg-shell`、`ext-session-lock-v1` 等。
   - 确保 Quickshell 框架及其衍生桌面外壳（如 Noctalia Shell）的面板、小组件、弹层可稳定挂载与交互。
   - 确保 LXQt Wayland 会话（lxqt-panel, pcmanfm-qt, lxqt-runner 等）的图层停靠、窗口菜单及托盘协同无障碍运行。
3. **发行版优先支持 deepin / arch**：
   - **deepin**：作为 DDE/treeland 原生阵地，保障依赖链可构建，适配 deepin 运行环境、系统库版本与桌面组件联动。
   - **Arch Linux**：利用 Arch 最新的编译器、上游 Wayland/wlroots 依赖栈及 AUR 生态，持续验证最新工具链兼容性与 CI 自动化测试（CI 基线为 `archlinux:base-devel`）。
4. **性能与工程质量优先**：
   - 单体结构（sway/labwc 风格），避免层层抽象。
   - 特效编译期与运行期双可控（`-Deffects=enabled/disabled`，`[effects] enabled`）。
### 2.3 非目标（显式砍掉）

- **DDM 登录管理器集成**：不内置登录/greeter，不支持 `treeland-ddm-v1` 与 Qt Remote Objects 登录通道。
- **DConfig 配置中心**：不依赖 DTK 的 DConfig；配置走配置文件 + key-value 协议（见第 8 节）。
- **Sandbox appId 解析**（`treeland-app-id-resolver`）：不实现。
- **Qt Remote Objects 调试工具**（`treeland-debug`）：不实现；调试能力依赖 wlroots 日志 + 上游协议（如 screencopy）。
- **DTK/DDM 依赖**：不引入。D-Bus 仅用于 logind（session 获取）与 session 级屏保控制（见第 11 节）。

## 3. 术语与参考基线

- **上游协议**：wlroots 自带的协议（xdg-shell、layer-shell、output-management、screencopy、pointer-constraints、foreign-toplevel、idle、fractional-scale、viewporter、session-lock 等）。
- **私有协议**：`treeland-protocols` 仓库定义的协议，分 `public/`、`dde/`、`internal/`、`wine/`、`deprecated/` 五类。
- **线缆级兼容**：客户端能绑定全局对象、创建对象、完成握手与版本协商不报协议错误；但语义可能降级或为空实现。
- **语义完整**：协议请求产生真实、正确的行为。

## 4. 技术选型（已决策）

| 维度 | 决定 |
| --- | --- |
| 语言 | C（C11 起步，允许 C23 匿名参数等由编译器决定） |
| wlroots | meson wrap 下载，**锁定最新 tag 0.20.2**（不跟随 master） |
| 渲染 | wlr_scene + scenefx + wlr_renderer（gles2 / vulkan / pixman 后端） |
| 构建 | meson + meson wrap（scenefx、wlroots 均经 wrap 下载） |
| 许可证 | GPL-2.0-only |
| 配置 | 配置文件 + key-value Wayland 协议 |
| 会话/登录/锁屏 | 无内置登录；session 走 seatd/logind；锁屏走 ext-session-lock-v1（外部屏保） |
| XWayland | 完整支持（wlr_xwayland） |
| 特效 | scenefx 实现模糊/圆角/阴影等（scenefx 0.5，经 wrap 下载）；编译期 feature flag + 运行期配置，**编译期默认开启**、运行期由 `[effects] enabled` 控制（默认 true） |
| 布局 | 单体内置（sway 风格）；**默认堆叠（stacking）布局**，支持运行时切换模式，不同工作区可为不同模式 |
| 目标发行版 | 优先支持 deepin 与 Arch Linux（CI 基线为 archlinux:base-devel） |

## 5. 架构组织

**单体结构（sway 风格）**。单个 C 可执行 `woodland`，`src/` 按职责分层；布局为内置模块，无插件加载机制。

```
woodland/
├── meson.build
├── meson_options.txt          # 编译期 feature flags
├── protocols/                 # 私有协议 XML（从 treeland-protocols 提取）
├── src/
│   ├── main.c
│   ├── server/                # 后端创建、事件循环、渲染循环
│   ├── seat/                  # 输入设备、光标、焦点、指针约束
│   ├── scene/                 # wlr_scene 图层树（背景/底层/窗口/顶层/overlay/锁定）
│   ├── shell/                 # xdg-shell / xwayland 视图封装
│   ├── workspace/             # 工作区模型与切换（12 个）
│   ├── layout/                # 布局实现（堆叠默认 + 多种布局模式）
│   ├── protocols/             # 私有协议实现（分级，见第 7 节）
│   ├── config/                # 配置解析 + key-value 协议服务端
│   ├── decorations/           # SSD 标题栏/边框/阴影（可选）
│   ├── effects/               # scenefx 模糊/圆角/阴影（默认开，运行期可关）
│   ├── ime/                   # 输入法协议 + 候选面板
│   ├── wallpaper/             # 壁纸协议协调(manager + shell)，不渲染
│   └── dbus/                  # session D-Bus 服务（屏保控制等）
├── docs/
└── tests/
```

### 5.1 核心模块职责

| 模块 | 职责 |
| --- | --- |
| `server` | 创建 wlr_backend、wlr_renderer、wlr_allocator、wlr_scene；Wayland 事件循环；渲染循环；输出热插拔。 |
| `seat` | libinput 设备接入；wlr_seat；光标管理；键盘/指针/触摸事件分发；pointer-constraints、relative-pointer、拖拽。 |
| `scene` | wlr_scene 的图层树：背景层、底层(面板)、窗口层、顶层(弹窗/菜单)、overlay 层、session-lock 层；Z 序与显示管理。 |
| `shell` | xdg-toplevel/popup 视图、xwayland 视图封装；窗口状态(最大化/最小化/全屏/浮动/贴边)、激活与焦点策略。 |
| `workspace` | 工作区模型；12 工作区切换；窗口跨工作区移动；"显示在所有工作区"。 |
| `config` | 配置文件解析(默认值)+ key-value 协议服务端(运行时热更新)。 |
| `decorations` | SSD：标题栏、边框、圆角、阴影；与 xdg-decoration 联动，支持"半 CSD 半 SSD"。 |
| `effects` | 基于 scenefx 的模糊、圆角、阴影等特效；编译期+运行期双开关，默认开（可 `-Deffects=disabled` 退回直出）。 |
| `ime` | input-method-v2、text-input-v3；内置候选面板。 |
| `wallpaper` | 壁纸协议协调者：实现 `treeland-wallpaper-manager-unstable-v1`（存储/校验/通知壁纸源）与 `treeland-wallpaper-shell-unstable-v1`（壁纸 surface 角色 + 播放控制）；不渲染壁纸内容。 |
| `dbus` | session D-Bus 服务：暴露屏保控制接口（激活/恢复/抑制），供外部屏保程序对接（单用户无需私有协议中转）。 |
| `layout` | 内置布局实现：堆叠（stacking，默认）与平铺等布局模式；每个工作区独立选择模式；计算窗口几何供 `scene` 应用。 |

### 5.2 布局模块

布局逻辑内置于核心，与 sway 的 arrange 模式一致：布局函数接收当前工作区的视图列表与可用区域，计算每个视图的几何并应用到 scene 节点。**默认布局为堆叠（stacking）布局**（传统浮动窗口管理，窗口可重叠）。布局模式可运行时切换，**每个工作区独立持有自己的布局模式**——不同工作区可以是不同模式（例如工作区 1 堆叠、工作区 2 平铺）。

```c
// src/layout/layout.h
struct woodland_layout {
    const char *name;
    void (*arrange)(struct woodland_workspace *ws,
                    struct wlr_box *area);
};
```

布局模式通过配置项 `general.default_layout` 指定默认值，运行时经 key-value 协议切换；每个工作区独立存储其当前模式。布局不直接操作渲染树，只计算几何，由 `scene` 模块应用。平铺等非堆叠布局可选编译（`-Dtiling-layout=true`）。



## 6. 渲染与特效边界

- 默认渲染路径：`wlr_scene` 节点直出，无额外合成开销。
- 特效（阴影、圆角、模糊、过渡动画、多任务视图缩放动画）均为**可选项**：
  - 编译期：`meson_options.txt` 中 `-Deffects=enabled/disabled`（默认 enabled）。
  - 运行期：配置文件 `[effects] enabled`（默认 true）总开关，`blur`（默认 true）模糊开关，
    `corner_radius` 是圆角半径。关掉后协议状态照记、只是不建节点。
- 需要特效时，使用 **scenefx**（wlroots 生态的 scene 扩展库）实现模糊、圆角、阴影等：scenefx 在 wlr_scene 之上提供 scene 节点装饰能力（圆角 clip、阴影、blur 等），避免自研渲染管线。
- 多任务视图（overview）、任务切换器、显示桌面属于窗口管理特性（第 9 节），与特效独立，但其**动画**受特效开关控制。

## 7. 协议兼容清单（两级分级）

> 基线版本：treeland-protocols 0.8.0。所有 XML 提取至 `protocols/`。

### 7.1 语义完整级

**上游 wlroots 协议**（全量）：

- xdg-shell、xdg-decoration、layer-shell、output-management-v1、output-power-management、screencopy-v1、ext-image-capture-source-v1、ext-image-copy-capture-v1、pointer-constraints-v1、relative-pointer-v1、pointer-gestures-v1、foreign-toplevel-management-v1、ext-foreign-toplevel-list-v1、idle-notify-v1、idle-inhibit-v1、fractional-scale-v1、viewporter、single-pixel-buffer、subcompositor、data-control-v1、primary-selection、cursor-shape-v1、xdg-output、xdg-activation-v1、keyboard-shortcuts-inhibit-v1、session-lock-v1、xdg-dialog-v1、xdg-toplevel-tag-v1、alpha-modifier-v1、text-input-v3、input-method-v2、security-context-v1、color-management-v1、tearing-control-v1、drm-lease-v1、presentation-time、xdg-foreign-v1/v2。

**treeland public 协议**（6 个全支持）：

| 协议 | 说明 |
| --- | --- |
| `treeland-window-transition-unstable-v1` | 窗口打开/关闭转场；无特效时退化为即时显示 |
| `treeland-snap-target-unstable-v1` | 吸附区域提示 |
| `treeland-appearance-unstable-v1` | 只读外观查询（光标/字体/图标主题等） |
| `treeland-decoration-unstable-v1` | 逐窗口 SSD 定制（圆角/阴影/边框/标题栏） |
| `treeland-output-uuid-unstable-v1` | 输出稳定 uuid 身份 |
| `treeland-output-uuid-head-unstable-v1` | 禁用输出的 uuid 身份 |

**treeland dde 协议（通用子集，语义完整）**：

| 协议 | 说明 |
| --- | --- |
| `treeland-foreign-toplevel-manager-unstable-v3` | 顶层窗口观察 + dock 预览（通用合成器刚需）。**上游已用 v3 取代 v2**，woodland 直接实现 v3；dock preview context 只做对象/错误码校验与记账，不渲染缩略图与 tooltip 弹层 |
| `treeland-shortcut-manager-unstable-v3` | 全局快捷键 + 手势（通用合成器刚需） |
| `treeland-compositor-action-unstable-v1` | 合成器动作（工作区切换/显示桌面/锁屏/缩放等） |
| `treeland-appearance-manager-unstable-v1` | 特权外观配置（光标/字体/强调色等） |
| `treeland-layer-shell-extension-unstable-v1` | layer-shell 交互式缩放（dock/侧边栏） |
| `treeland-keyboard-state-notify-unstable-v1` | 键盘修饰键状态通知 |
| `treeland-active-notify-unstable-v1` | seat 活跃通知 |
| `treeland-region-watch-unstable-v1` | 输出边缘区域重叠监控 |
| `treeland-output-manager-unstable-v2` | 主屏指定 + 色温/亮度控制（uuid 寻址） |
| `treeland-output-group-manager-unstable-v1` | 输出镜像分组（uuid 寻址） |
| `treeland-xwindow-control-unstable-v1` | XWayland 窗口定位 |
| `treeland-wallpaper-manager-unstable-v1` | 壁纸源管理（对接控制中心）；仅存储/校验/通知，不渲染 |

**treeland internal 协议（语义完整）**：

| 协议 | 说明 |
| --- | --- |
| `treeland-wallpaper-shell-unstable-v1` | 壁纸 surface 角色 + 源变化通知 + 播放控制；渲染由壁纸客户端完成 |

**treeland wine 协议（语义完整，低优先级）**：

> 以下协议服务于 wine 兼容层，实现优先级低，可后置于核心功能完成后再接入。

| 协议 | 说明 |
| --- | --- |
| `treeland-remote-subsurface-unstable-v1` | 基于令牌的跨进程子 surface 关系（替代 wine 的 wl_subcompositor） |
| `treeland-wine-window-management-unstable-v1` | wine 窗口定位（SetWindowPos）与 Z 序控制（HWND_TOPMOST 等） |
| `treeland-wine-window-state-unstable-v1` | wine 窗口状态：最小化/还原、带焦点抢占防护的激活、注意提示（FlashWindowEx） |

### 7.2 线缆兼容级（可绑定、握手不报错，语义降级/空实现）

| 协议 | 降级策略 |
| --- | --- |
| `treeland-dde-shell-unstable-v2` | surface 角色可绑定并渲染为普通/overlay 层；`set_skip_flags`、`set_position_hint` 等请求接受但不保证完整语义；无 DDE 特权客户端校验（或弱校验） |
| `treeland-input-manager-unstable-v1` | 绑定成功，逐设备输入配置映射到内置 seat/input 模块；xkb 设置支持 |

### 7.3 明确不支持

| 协议/能力 | 原因 |
| --- | --- |
| `treeland-ddm-v1`（deprecated） | 依赖 DDM，已砍 |
| `treeland-app-id-resolver-unstable-v2` | sandbox 应用 ID 解析，依赖 sandbox 基础设施，不实现 |
| `treeland-prelaunch-splash-unstable-v2` | prelaunch 特效（应用预启动闪屏），不实现此特效 |
| `treeland-screensaver-unstable-v2` | treeland 是多用户合成器，无法建立 session 级 D-Bus 故用该协议中转；woodland 单用户，直接提供 session D-Bus 屏保接口，不实现该协议 |
| Qt Remote Objects 调试通道（`treeland-debug`） | 依赖 Qt，已砍 |
| `deprecated/` 其余协议 | 仅保持 XML 存在供客户端读取，不注册全局对象 |

## 8. 配置机制

配置文件 + key-value 协议双通道：

1. **配置文件**（默认值来源）：`~/.config/woodland/config.toml`（或 `$XDG_CONFIG_HOME/woodland/config.toml`）。TOML 格式，启动时读取，作为所有选项的默认值。
2. **key-value Wayland 协议**（运行时热更新）：实现一个 `woodland-config` 私有协议（或采用/仿照 wlr 生态常见的 key-value 模式），供控制客户端运行时读取/修改配置并即时生效。优先级：运行时协议值 > 配置文件值 > 内置默认值。

配置项类别（初始最小集）：

- `output`：模式、缩放、刷新率、主屏、镜像组。
- `input`：键盘布局、指针加速、触摸板设置。
- `general`：工作区数量、默认布局模式（堆叠等）、焦点策略。
- `effects`：总开关 + 各特效子开关（默认开）。
- `decorations`：SSD 开关、标题栏高度、圆角半径。
- `wallpaper` / `screensaver`：壁纸默认源路径、屏保超时。

## 9. 窗口管理特性

对齐 treeland 的窗口管理行为（线缆级可观察行为一致，实现细节不同）：

- 堆叠（stacking）布局为默认，支持运行时切换布局模式，不同工作区可为不同模式（平铺等可选编译）。
- 12 个工作区，支持切换、窗口跨工作区移动、"显示在所有工作区"。
- 窗口状态：最大化、最小化、全屏、浮动、贴边（snap：半屏/四分之一/边缘）。
- 任务切换器（alt-tab）与任务窗口预览。
- 多任务视图（overview）：工作区选择列表 + 窗口选择网格。
- 显示桌面（show-desktop）。
- 窗口菜单（最大化/最小化/关闭/移动/贴边等）。
- 焦点策略：点击聚焦、跟随鼠标可选、焦点抢占防护（支持 wine 的 focus 抢占保护语义，见第 7.1 节 wine 协议）。

## 10. IME 与输入法

完整 IME 支持（对齐 treeland 的 `imcandidatepanelmanager` 但纯 C 实现）：

- 实现 `input-method-v2`、`text-input-v3` 两个协议（不实现 input-method-v3、text-input-v1/v2、virtual-keyboard-v1）。
- 候选面板：在 wlr_scene overlay 层自绘（矩形 + 文本 + 高亮 + 分页），支持跟随光标/文本位置定位。
- 输入法输入事件经 seat 焦点正确路由至 IME 与客户端。

## 11. 壁纸与屏保

**壁纸**：合成器不自己渲染壁纸，仅实现壁纸相关协议作为协调者；壁纸的实际渲染由独立的壁纸客户端完成。

两个协议的分工：

1. `treeland-wallpaper-manager-unstable-v1`（对接控制中心/系统设置）：控制中心通过 `get_treeland_wallpaper` 创建壁纸对象，用 `set_image_source` / `set_video_source` 设置壁纸源。合成器负责存储壁纸源（按 output + role + workspace 维度）、文件访问校验，并发送 `failed` / `changed` 事件。合成器不加载、不解码、不渲染壁纸内容。
2. `treeland-wallpaper-shell-unstable-v1`（对接壁纸客户端）：壁纸客户端通过 `get_treeland_wallpaper_surface` 将一个 wl_surface 声明为壁纸角色。合成器将该 surface 置于背景层（wallpaper z 序），通过 `treeland_wallpaper_notifier_v1` 的 `add`/`remove` 广播壁纸源变化，并通过 `position`/`play`/`pause`/`set_playback_rate`/`slow_down` 控制播放。壁纸内容的渲染（图片/视频）由壁纸客户端完成。

壁纸客户端可作为独立轻量进程（参考 treeland 的 wallpaper-factory，但非必需）；woodland 合成器二进制本身不包含壁纸渲染代码。

**屏保**：不实现 `treeland-screensaver-unstable-v2` 协议（该协议是 treeland 多用户架构下无法建立 session 级 D-Bus 的中转方案）。woodland 是单用户合成器，运行在用户 session 内，直接提供 session D-Bus 屏保接口：外部屏保程序通过 D-Bus 与合成器协商屏保激活/恢复/抑制（idle 超时触发），屏保内容由外部程序渲染，合成器只管理 idle 计时与屏保 layer 的 Z 序。

## 12. 会话管理、登录与锁屏

- **无内置登录/greeter**：合成器作为普通 Wayland 合成器，由外部 display manager 或 TTY + `wayland-session` 启动。
- session 获取：优先 logind，回退 seatd（wlr_backend 自动处理）。
- **锁屏**：实现 `ext-session-lock-v1`（上游 session-lock 协议），将锁屏 UI 交给外部屏保/锁屏程序（如 swaylock 类）。合成器仅保证锁定态下窗口不可见/不可交互，并正确管理锁定 layer 的 Z 序。

## 13. 构建与目录约定

- 构建：meson + meson wrap。**wlroots 与 scenefx 均通过 meson wrap 下载**（不 vendored），锁定版本：wlroots 0.20.2、scenefx 0.5。
- `subprojects/wlroots.wrap`（revision = 0.20.2）与 `subprojects/scenefx.wrap`（revision = 0.5），参考 swayfx 的 `subproject('scenefx', required: false)` + `dependency('scenefx-0.5', fallback: 'scenefx')` 写法。
- 依赖（运行时最小集）：libinput、xkbcommon、pixman、libdrm、libseat/libsystemd(logind)、libdisplay-info、wayland、wlroots(经 wrap)、scenefx(随 effects，经 wrap；effects 默认启用)。
- 编译期 feature flags（`meson_options.txt`）：`effects`（默认 true，启用时引入 scenefx，关闭时退回 wlroots 直出）、`tiling-layout`（默认 false）、`xwayland`（默认 true）。

## 14. 测试策略

- 单元测试：配置解析、布局几何计算、uuid 管理、协议 XML 解析。
- 集成测试：headless backend 下启动合成器，连接测试客户端（复用 treeland 的 `tests/protocols` 测试客户端思路，重新实现）。
- 协议测试：为每个语义完整级私有协议编写最小客户端测试握手 + 行为。
- 冒烟测试：XWayland 客户端启动、IME 候选面板出现、锁屏协议握手、session D-Bus 屏保接口调用。

## 15. 风险与开放问题

1. **wlroots API 破坏**：wlroots 每次 minor 升级会破坏 API。缓解：锁定 tag 0.20.2，升级时集中适配；跟随上游 tag 而非 master。
2. **treeland 私有协议是实验性**（treeland-protocols 自述）：协议可能不向后兼容地变更。缓解：vendored XML 快照 + 版本记录，定期同步上游。
3. **线缆级兼容的边界**：DDE 专属协议若客户端强制要求完整语义（如 dock 依赖 dde-shell overlay 的精确行为），可能仍有兼容缺口。缓解：在实现中优先保证 `dde-shell-v2` 的 surface 角色与 skip 位域行为正确，其余降级。
4. **特效与性能目标的内在张力**：即使特效可关，实现特效的代码路径也会增加核心复杂度。缓解：特效代码全部隔离在 `src/effect/`，`-Deffects=disabled` 时退回 wlroots 直出、不进入特效路径。
## 16. 验收标准（DoD）

1. `meson setup build && meson compile -C build` 无警告通过（`-Wall -Wextra -Werror` 可选开启），meson wrap 依赖可自动下载。
2. 在 headless backend 与真实 DRM 后端均能启动，XWayland 客户端可运行。
3. 全部语义完整级协议通过最小客户端握手测试。
4. 线缆兼容级协议客户端绑定不报协议错误。
5. 中文输入法（fcitx5 或等价实现）候选面板正常显示。
6. 配置文件与 key-value 协议运行时改配置即时生效。
7. 特效运行期关闭时，`[effects] enabled=false` 下渲染路径无特效开销（可测量帧时间无显著差异）。
8. 许可证检查通过（GPL-2.0-only，无不兼容代码混入）。
9. DDE 核心外壳组件（dde-shell/dock、壁纸、控制中心交互）协议握手与基础功能可用。
10. 基于 layer-shell 的现代组件与桌面（Noctalia、Quickshell、LXQt 组件）可正确停靠、获得输入与交互。
11. deepin 与 Arch Linux 构建与运行顺畅，CI（Arch Linux）自动化测试通过。
