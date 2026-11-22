# Woodland 智能体指南

## 全局认识

- `woodland` 是**纯 C + wlroots** 的通用轻量 Wayland 合成器，单体结构（sway/labwc 风格），常规单用户、运行在用户 session 内，无 Qt/QML 依赖。
- **三大核心目标**：
  1. **兼容 treeland 生态，运行 DDE 桌面环境**：朴素特效（基于 scenefx 适度提供模糊/圆角/阴影，性能优先，克制简洁）。
  2. **支持运行 noctalia / quickshell / lxqt**：完整支持现代 layer-shell 壳体组件与轻量桌面会话。
  3. **发行版优先支持 deepin / arch**：优先保障 deepin 与 Arch Linux 环境下的构建、打包与运行体验。
- 依赖锁定：wlroots 锁定 tag 0.20.2（经 meson wrap 下载），scenefx 锁定 0.5（随 `-Deffects=enabled` 默认启用）。两者均**不 vendored**。许可证：GPL-2.0-only。

---

## 核心命令

### 构建与测试

```bash
meson setup build -Dtest=enabled        # 首次配置（自动下载 wrap 依赖）
meson compile -C build                  # 编译（-Werror 警告即错误）
meson test -C build --print-errorlogs   # 运行全部测试
meson test -C build --suite quick       # 子集分层：quick | unit | smoke | xwayland | needs-gpu
./tools/verify.sh                       # 一键双构建（effects + no-effects）验证并高亮 SKIP 警告
./tools/verify.sh --quick               # 仅快速子集（compile + --suite quick）
./tools/verify.sh --json                # 输出机器可读 JSON 摘要（支持 --fail-on-skip）
./tools/check-deps.sh                   # 检查构建/冒烟依赖（支持 --json / --strict）
```

- **特性开关**（`meson_options.txt`）：`effects`（默认 true）、`xwayland`（默认 true）、`tiling-layout`（默认 false）、`test`（默认 false）。由 `build/config.h` 导出 `WOODLAND_HAVE_*` 宏。
- **ASan 消毒器构建**：
  ```bash
  meson setup build-asan -Db_sanitize=address,undefined -Dtest=enabled
  ASAN_OPTIONS=detect_leaks=0 meson test -C build-asan
  # 若机器遇高熵 ASLR 冲突，用 setarch 关掉 ASLR 运行：
  setarch $(uname -m) -R ./build-asan/src/woodland
  ```

### 调试接口（wctl）

合成器在 `$XDG_RUNTIME_DIR/woodland-<socket>.sock` 上开只读调试 socket，`WOODLAND_IPC` 导出路径：

```bash
wctl get_tree   | jq '.views[] | {id,title,focused,activated,outer}'
wctl get_tree   | jq '.views[] | {id, palette: .ssd.palette}'  # 实际生效的 SSD 标题栏/边框色
wctl get_focus  | jq .         # 当前键盘与指针焦点视图
wctl dump_scene | jq '.nodes'  # 完整场景图，排查图层遮挡或节点可见性
wctl self_check | jq .         # 不变量断言状态检查
wctl get_trace  | jq .         # 导出决策轨迹环形缓冲
wctl pixel <x> <y>             # 采样像素并返回颜色与场景归因/期望来源
wctl screenshot [--output NAME] [--region x,y,w,h] [--expect #rrggbb] [--dump-ppm PATH]  # 区域主色统计、期望匹配与可选 PPM 落盘（--dump-ppm 需 --debug-ctl）
wctl wait <cond> [key=val...]  # 等待事件/状态/像素就绪（替代脚本 sleep）
```

- **开启写命令**：启动加 `--debug-ctl`（会自动接入 headless 虚拟键盘；支持 `wctl focus_view <id>`、`wctl move <id> <x> <y>`、`wctl resize <id> <w> <h>`、`wctl close <id>`、`wctl key <code> tap`、`wctl inject_key <code> tap`、`wctl screenshot --dump-ppm` 等，完整清单见 `wctl help`）。
- **决策追踪**：启动加 `--trace`，记入 4096 条状态转移环形缓冲，用 `what` 权威标签（`include/trace.h`）与 `detail` 决策依据排查因果。
- **状态自检**：启动加 `--self-check`，在 idle 阶段自动断言视图链表、焦点、SSD 几何与壁纸根节点不变量。

---

## 核心硬约束

1. **纯 C11 与语言限制**：严禁引入 C++、Qt 或 QML。单实例全局 `struct woodland_server` 经指针传递，不做依赖注入。
2. **源码规范**：
   - 每个新源文件顶部必须带单行 `// SPDX-License-Identifier: GPL-2.0-only`。
   - 保持 diff 最小，禁止无关联代码格式化。
   - **禁止直接 push 到 `main`**：必须通过分支和 PR 合入，保持线性提交历史。
3. **资源释放纪律**：各子模块 `finish` 必须与初始化严格逆序释放，遵循“先判空、再销毁、置 NULL”的幂等原则。
4. **调试红线**：**绝不要用 gdb/lldb 的 `run` 或 `r` 调试合成器**（主循环正常阻塞于 `ep_poll`，永不返回）。验证靠自动化测试、运行时状态查 `wctl`、前台日志看 `wlr_log`。
5. **scenefx 核心硬约束**（特效构建）：
   - 源文件只 `#include "scene.h"`，严禁直接包含 `<wlr/types/wlr_scene.h>`。宏判断必须在 `#include "config.h"` 之后。
   - 启用 effects 时 `server->renderer` 必须由 `fx_renderer_create()` 创建。
   - 场景节点坐标均相对于父节点，计算绝对几何时严禁重复累加父树自身的 `x/y`。
   - 严禁在 1px 细条上设圆角，圆角边框统一使用覆盖外框的矩形配合 `clipped_region` 挖空绘制。
   - scenefx 仅替换底层渲染节点，表面子树创建接口（如 `wlr_scene_surface_create`）链接自 libwlroots，双库混用为预期设计。

---

## 模块分层指引（src/*/AGENTS.md）

为节省上下文，深入具体模块开发时请查阅对应子目录下的局部指南：

| 模块目录 | 领域与职责 | 模块指南入口 |
| --- | --- | --- |
| `src/server/` | 合成器主干：Backend/Renderer/Scene 初始化、主事件循环与生命周期 | [src/server/AGENTS.md](src/server/AGENTS.md) |
| `src/output/` | 显示输出管理：输出热插拔、`wlr_output_layout`、全局缩放统一刷新收敛 | [src/output/AGENTS.md](src/output/AGENTS.md) |
| `src/seat/` | 输入设备交互：`wlr_seat`、键盘/指针按键分发、VT 控制台切换 | [src/seat/AGENTS.md](src/seat/AGENTS.md) |
| `src/shell/` | 窗口管理：xdg-shell 状态机、焦点、排版、贴边吸附预览、Alt+Tab 切换器、窗口菜单 | [src/shell/AGENTS.md](src/shell/AGENTS.md) |
| `src/layer/` | 桌面外壳组件：`wlr-layer-shell-v1` 四层图层树、exclusive zone 边缘排他 | [src/layer/AGENTS.md](src/layer/AGENTS.md) |
| `src/ssd/` | 服务端装饰：Cairo/Pango 标题栏与控制按钮绘制、scenefx 圆角边框、调色板 | [src/ssd/AGENTS.md](src/ssd/AGENTS.md) |
| `src/effect/` | 视觉特效：scenefx 模糊、预模糊优化缓存（optimized blur）、阴影 | [src/effect/AGENTS.md](src/effect/AGENTS.md) |
| `src/protocol/` | 协议适配：Treeland 协议族（appearance, shortcut, wallpaper 等）与通用扩展 | [src/protocol/AGENTS.md](src/protocol/AGENTS.md) |
| `src/ipc/` | 调试控制：wctl 调试套接字、JSON 协议分发、`--debug-ctl` 写操作 | [src/ipc/AGENTS.md](src/ipc/AGENTS.md) |
| `src/xwayland/` | X11 兼容：Xwayland 实例生命周期、unmanaged 悬浮窗、托盘摆位钩子 | [src/xwayland/AGENTS.md](src/xwayland/AGENTS.md) |
| `src/dnd/` | 拖拽交互：`wl_data_device` 拖拽状态机与拖拽图标场景节点跟随 | [src/dnd/AGENTS.md](src/dnd/AGENTS.md) |
| `src/shortcutd/` | 快捷键客户端：独立的 `woodland-shortcutd` 守护进程与按键拉起 | [src/shortcutd/AGENTS.md](src/shortcutd/AGENTS.md) |

---

## 参考代码与行为准则（Reference Code Guidelines）

为避免盲目猜测或重复造轮子，工作区根目录下设立 `reference/` 目录（已加入 `.gitignore`，不污染版本库与构建），作为智能体开发与排查的权威参考源：

- **主动检索原则**：
  - 在实现新协议（如 treeland 系列）、复杂窗口状态机、SSD 绘制、场景图组织或视觉特效（模糊/圆角/阴影）时，**必须优先主动检索 `reference/` 下的成熟开源实现**，对照其边界处理与状态流转，严禁脱离参考实现凭空臆造。
- **环境通用与多用户原则（严禁假定用户名为 rewine）**：
  - **严禁硬编码任何绝对家目录路径**（如 `/home/rewine/`）。统一使用环境变量 `$HOME` 或 `~` 进行相对解析与定位。
- **Git Worktree 场景定位与复用**：
  - 当在 `git worktree` 下工作时，原仓库通常位于 `~/` 或 `~/work/`，亦可通过 `git rev-parse --path-format=absolute --git-common-dir` 定位主仓根目录。
  - 若主仓已存在 `reference/` 目录，当前 worktree 应优先复用或软链接主仓的 `reference` 目录（`ln -s "$(git rev-parse --path-format=absolute --git-common-dir)/../reference" reference`），避免重复克隆。
- **本地参考代码查找与链接（Discovery Order）**：
  - 寻找本地已有第三方仓库（如 `treeland`、`labwc` 等）时，按如下顺序定位并软链至 `reference/<name>`：
    1. 当前工作区根目录的 `reference/<name>`
    2. 主仓库根目录下的 `reference/<name>`
    3. `~/<name>` 或 `~/work/<name>`（用户本地已有开发目录）
  - 常用参考库定位示例：
    - `treeland`（`~/treeland` 或 `~/work/treeland`）：参考 Treeland 生态私有协议（appearance, wallpaper, shortcut, window-management 等）与 DDE 交互规范。
    - `labwc`（`~/labwc` 或 `~/work/labwc`）：参考纯 C + wlroots 合成器单体架构、SSD 边框与标题栏绘制、Scene 场景节点组织、配置管理。
    - `umbriel`：参考基于 scenefx/umbrielfx 的模糊、Popup 继承模糊、圆角阴影裁剪与特效优化细节。
- **按需下载新参考**：
  - 若本地未安装且任务需要参考其他开源项目（如 sway, wayfire, hyprland, umbriel 等），在 `reference/` 下按需执行浅克隆：`git clone --depth=1 <repo_url> reference/<name>`。
- **代码检索须知**：
  - 因 `reference/` 位于 `.gitignore` 中，使用 `rg` 搜索参考代码时**必须带 `--no-ignore` 或显式指定路径**，例如：`rg --no-ignore <symbol> reference/`。

---

## 踩坑与陷阱专栏（Pitfalls）

开发与排查疑难问题时，按主题参考详细踩坑记录：

- **[构建、CI 与测试踩坑](docs/pitfalls/testing-and-ci.md)**：CI 镜像环境要求、子项目协议头包装、CI 下 effects 限制原因、冒烟测试现场转储规范、探针双 roundtrip 与 prepare_read 规则、像素断言取色规范。
- **[调试方法与工具踩坑](docs/pitfalls/debugging.md)**：严禁 gdb run 原理、ASan 与内核高熵 ASLR 冲突规避、Mesa 退出缓存导致 LSan rc=134 误报、`--trace` 环形缓冲标签与 detail 准则、`--self-check` 不变量断言。
- **[wlroots 语义与时序陷阱](docs/pitfalls/wlroots-semantics.md)**：`idle_inhibitor` 销毁顺序与判空陷阱、`wl_signal` 注册顺序决定行为、`output-management` test 零副作用与 succeeded 广播时序、销毁信号中禁止解引用 view、缩放变更统一刷新收敛。
- **[私有协议与窗口系统踩坑](docs/pitfalls/protocol-implementations.md)**：壁纸多源可见性与持久化、compositor-action 与 shortcut 独占限制、appearance 会话级不落盘/字体字重/光标安全替换、foreign-toplevel 差量刷新、VT 切换逃生通道、贴边预览纯光标判定、Xwayland 托盘摆位几何与 int16 边界。

---

## 设计规格索引（docs/）

各子系统详细架构与协议设计规范参见 [docs/README.md](docs/README.md)。
