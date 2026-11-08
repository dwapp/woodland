<!--
Woodland 设计规格：treeland 壁纸协议实现
协议来源：/home/ut004971@uos/work/treeland-protocols
参考实现：/home/ut004971@uos/work/treeland  （src/modules/wallpaper, src/wallpaper, wallpaper-factory）
状态：已实现（feature/wallpaper，2026-11-05；实现细节与偏差见 §15）
最后更新：2026-11-05
-->

# 壁纸协议实现设计规格（treeland-wallpaper-*）

## 1. 目标与范围

实现 treeland 生态里的两套壁纸协议，使 woodland 能与 **`treeland-wallpaper-factory`**（外部进程）配合显示桌面 / 锁屏壁纸，并能被 dde 设置面板通过 manager 协议配置壁纸。

**两套协议，同一功能的两半：**

| 协议 | 方向 | 作用 |
|---|---|---|
| `treeland_wallpaper_shell_unstable_v1` | 合成器 ⇐ 壁纸客户端 | 客户端把 `wl_surface` 标记为"壁纸角色"，合成器把它塞到场景最底层，并用 pause/play/slow_down 等事件控制播放 |
| `treeland_wallpaper_manager_unstable_v1` | 合成器 ⇐ 设置面板(dde) | 面板把"某 output 用哪张图/视频"告诉合成器，合成器做文件校验、`(output, role, workspace)` 存储、回 `changed` / `failed` |

**本规格的 7 项决策已与用户确认（见 §11）。核心结论：**

- 复用 `treeland-wallpaper-factory`（外部二进制），woodland **不自己启动客户端、不内置解码器**。
- 壁纸 surface 渲染在 **layer-shell `background` 层之下**。
- 协议级完整实现 + 渲染委派给客户端，零图片/视频解码依赖（贴合 woodland "不新增依赖" 红线）。
- 配置持久化用**纯配置文件**（不用 dconfig），数据模型对齐 treeland。

**明确不做（v1）：**

- woodland 自身不解码任何图片/视频；解码（含 **视频经 mpv**）全在 factory 里。
- 不内置壁纸客户端 / launcher；factory 由桌面会话负责拉起并连接 woodland 的 wayland socket。
- 锁屏壁纸 role 只**存储 + 转发**给 factory，woodland 自己的 session-lock 屏不渲染它（见 §10）。
- 多 output 不同壁纸：v1 仅保证"单张全局壁纸 surface 铺满所有输出"；per-output 不同壁纸需 factory 创建多个 surface 或协议扩展，推迟。

---

## 2. 架构总览

```
        dde 设置面板                 treeland-wallpaper-factory (外部进程)
        (wallpaper_manager)          (绑定 wallpaper_shell + wallpaper_notifier)
              │                                │
   set_image_source / set_video_source   get_treeland_wallpaper_surface
              │                                │
   ┌──────────┴────────────────────────────────┴──────────┐
   │                   WOODLAND 合成器                      │
   │                                                        │
   │  manager 族 ── 校验 + 存配置 + 回 changed/failed        │
   │  notifier 族 ── 维护"可用源列表"，广播 add/remove       │
   │  shell 族 ── 壁纸角色赋值 + 场景最底层 + 播放控制事件     │
   │                                                        │
   │  $XDG_CONFIG_HOME/woodland/wallpaper.json  (持久化)     │
   └────────────────────────────────────────────────────────┘
```

**关键事实（来自 treeland 现状核对）：**

- factory 二进制名 `treeland-wallpaper-factory`（`/usr/bin/treeland-wallpaper-factory`）。它绑定 `treeland_wallpaper_notifier_v1` + `treeland_wallpaper_shell_v1`，图片用 Qt、视频用 **mpv** 解码（见 `wallpaper-factory/mpvvideoitem.cpp`、`qwaylandwallpapershellintegration.cpp`）。
- treeland 自身用 `WallpaperLauncher`（QProcess）拉起 factory 并传 `WAYLAND_DISPLAY` + `QT_QPA_PLATFORM=wayland`。**woodland 不复制 launcher**——假设 factory 已由会话预先启动（见 §11.1）。
- treeland 的 `WallpaperManager` 把配置以 **JSON 数组字符串**存进 DConfig（`m_config->setWallpaperConfig(json)`）。woodland 用等价 JSON 写进**普通文件**（见 §6）。

---

## 3. 协议参考（精确，来自 XML）

### 3.1 `treeland_wallpaper_shell_unstable_v1`

- **`treeland_wallpaper_notifier_v1`**（全局，纯事件）
  - enum `wallpaper_source_type`：`image=0`、`video=1`
  - request：`destroy`
  - event `add(source_type, file_source)`
  - event `remove(file_source)`
- **`treeland_wallpaper_shell_v1`**（全局，**单例**：协议声明至多一个 client 可 bind，但 treeland **不强制**）
  - request：`destroy`
  - request `get_treeland_wallpaper_surface(id → treeland_wallpaper_surface_v1, surface: wl_surface, file_source: string)`
    - 给 `surface` 赋壁纸角色；`surface` 不得已有角色。
- **`treeland_wallpaper_surface_v1`**
  - enum `error`：`invalid_source=1`、`permission_denied=2`
  - request：`destroy`、`source_failed(error)`、`ready()`
  - event：`position(fixed)`、`pause()`、`set_playback_rate(fixed rate)`、`play()`、`slow_down(uint duration)`

### 3.2 `treeland_wallpaper_manager_unstable_v1`

- **`treeland_wallpaper_manager_v1`**（全局）
  - request：`destroy`、`get_treeland_wallpaper(id → treeland_wallpaper_v1, output: wl_output, surface: wl_surface | null)`
- **`treeland_wallpaper_v1`**
  - enum `error`：`already_used=0`、`invalid_source=1`、`permission_denied=2`
  - enum `wallpaper_source_type`：`image=0`、`video=1`
  - enum `wallpaper_role`：`desktop=0x1`、`lockscreen=0x2`
  - request：`destroy`、`set_image_source(file_source, role)`、`set_video_source(file_source, role)`
  - event：`failed(file_source, error)`、`changed(role, source_type, file_source)`

**注意**：manager 协议文档文字写"compositor is responsible for decoding/presenting video"，但 treeland 实际**不解码**——它只存路径，解码在 factory。`set_image_source` / `set_video_source` 的"类型"由**用哪个请求**决定（image 请求→`Image`，video 请求→`Video`），无需内容嗅探。

---

## 4. 数据流

### 4.1 配置路径（dde → manager → 存储 → notifier → factory）

1. dde 调 `get_treeland_wallpaper(output, surface|null)` 拿到 `treeland_wallpaper_v1`。
2. dde 调 `set_image_source(path, role)` / `set_video_source(path, role)`。
3. woodland：
   - 校验文件（存在 + 可读 + 路径白名单，§5）。失败 → `failed(path, invalid_source | permission_denied)`。
   - 成功 → 写进 `(output, role, 当前 workspace)` 配置（§6）→ 回 `changed(role, type, path)` 给该 client。
   - 若 `path` 是全局新源 → `notifier.add(type, path)` 广播给所有 notifier 客户端；若已存在 → `failed(path, already_used)`（**非致命**，与 treeland `onImageChanged` 一致）。
4. factory 收到 `notifier.add` 后，用 `get_treeland_wallpaper_surface` 创建壁纸 surface 并渲染 `path`。

### 4.2 渲染路径（factory → shell → 场景）

1. factory bind `treeland_wallpaper_shell_v1`，调 `get_treeland_wallpaper_surface(surface, file_source)`。
2. woodland 给 `surface` 赋壁纸角色，把其 scene 节点 reparent 到 `server->wallpaper_tree`（最底层，§7）。
3. factory 提交 buffer 后调 `ready()`；woodland 由"隐藏/fallback"切到显示。
4. factory 解码失败 → `source_failed(error)`；woodland 记日志 + 可回退纯色。

### 4.3 锁屏壁纸（与 session-lock 解耦，见 §10）

`lockscreen` role 走与 desktop 完全相同的 manager 路径，但 woodland 的锁屏屏不渲染它——只存储并可在 `changed` 里回给 factory；factory 自行决定是否在锁屏时绘制。

---

## 5. 校验规则（manager 的 set_*_source）

- **存在性**：`access(path, R_OK) == 0`，否则 `failed(path, permission_denied)`。
- **白名单前缀**（可配置，默认）：
  - `/usr/share/wallpapers/`
  - `/var/cache/wallpapers/`
  - `$XDG_DATA_HOME/wallpapers/`（默认 `~/.local/share/wallpapers/`）
  - 越界路径 → `failed(path, invalid_source)`。
- 不校验文件内容/格式（解码在 factory）；纯路径级校验，与 treeland 一致。
- `surface` 参数（manager 的 `get_treeland_wallpaper` 的引用 surface）只用于"按 surface 反查 wallpaper 对象"（treeland `getReferenceWallpaperInterfaceFromSurface`），v1 可仅存储、不强制用到。

---

## 6. 配置与持久化（纯文件，对齐 treeland 数据模型）

**文件**：`$XDG_CONFIG_HOME/woodland/wallpaper.json`（默认 `~/.config/woodland/wallpaper.json`）。
**格式**：JSON 数组，每个元素一个 output（与 treeland `WallpaperOutputConfig` / `WallpaperWorkspaceConfig` 同构；键名沿用 treeland 原样，含其原始 typo 键 `desktopWallpapertype` / `lockScreenWallpapertype`，以保证将来可共享配置）：

```json
[
  {
    "outputName": "HEADLESS-1",
    "lockScreenWallpaper": "/usr/share/wallpapers/deepin/deepin-default.jpg",
    "lockScreenWallpapertype": 0,
    "workspaces": [
      { "workspaceIndex": 0,
        "desktopWallpaper": "/usr/share/wallpapers/deepin/deepin-default.jpg",
        "desktopWallpapertype": 0 },
      { "workspaceIndex": 1,
        "desktopWallpaper": "/usr/share/wallpapers/deepin/deepin-default.jpg",
        "desktopWallpapertype": 0 }
    ]
  }
]
```

**内存模型**（C 结构，对应上面 JSON）：

```c
struct wp_workspace_cfg {
    int index;
    char *desktop;          // 路径
    int desktop_type;       // 0=image,1=video
};
struct wp_output_cfg {
    char *output_name;
    char *lockscreen;       // 路径
    int lockscreen_type;
    struct wp_workspace_cfg *workspaces;
    size_t n_workspaces;
};
struct wp_config {
    struct wp_output_cfg *outputs;
    size_t n_outputs;
};
```

**读写时机**（对齐 treeland `wallpapermanager.cpp`）：

- 启动 / 首次配置：`loadWallpaperConfig()`。
  - 文件非空 → 解析 JSON 数组。
  - 文件空 / 不存在 → `defaultWallpaperConfig()`：对每个 output、每个 workspace、lockscreen 填默认壁纸（§6.1），并立即写回文件。
- 任意变更后：`saveWallpaperConfig()` 写回文件（treeland 每次 `setOutputWallpaper` 后都写）。
- 新增 output：`ensureWallpaperConfigForOutput()` —— 参照已有 output 的配置克隆到新 output（含 workspace 列表），并给 notifier 补 `add`。
- 移除 output：`removeOutputWallpaper()` —— 若该源不再被任何 output 引用，给 notifier 发 `remove`。
- workspace 增减：`syncAddWorkspace()` —— 参照首个 workspace 的配置补齐缺失 workspace。

**§6.1 默认壁纸**：`/usr/share/wallpapers/deepin/deepin-default.jpg`（与 treeland `DEFAULT_WALLPAPER` 一致）。woodland 侧允许经自身配置（若有 `default_background` 键）覆盖。缺失该文件时回退为**纯色**（合成器背景色），不崩溃。

---

## 7. 场景与 z 序

- 在 server 初始化时新建全局 `server->wallpaper_tree = wlr_scene_tree_create(server->scene->tree)`。
- 立即 `wlr_scene_node_lower_to_bottom(&server->wallpaper_tree->node)`：把它压到 `server->scene->tree` 子节点的最底。
  - 依据：layer-shell 的各层 `output->layer_tree[i]`（background=0…overlay=3）是 `output->scene_output` 的子节点，而 `output->scene_output` 是 `server->scene->tree` 的子节点（`layer.c:59,294`）。因此 `wallpaper_tree` 作为 `server->scene->tree` 的"最底子节点"会被画在所有输出内容（含 layer-shell `background`）**之下**，满足"壁纸在 layer-shell 下面"。
- 每个壁纸 surface：`wlr_scene_surface_create(server->wallpaper_tree, wlr_surface)` 或 reparent 其节点进 `wallpaper_tree`。
- **同时允许 layer-shell 壁纸**：客户端也可以用 `zwlr_layer_shell_v1`（`layer=background`）自己画壁纸——woodland 已有 layer-shell，这条路径天然可用；壁纸协议 surface 则落在它之下。
- **必须挑一张显示（实现时补的一条）**：`treeland-wallpaper-factory` 是**每个源开一个窗口**（notifier 的 `add` 一条一个），而协议的 `get_treeland_wallpaper_surface` 不带 output 参数，所以这些 surface 全落在 `wallpaper_tree` 的同一个位置、彼此完全重叠。不按当前状态挑，就永远是"最后创建的那张盖住所有"、切工作区也没有任何变化。规则见 §9。
- 显示/隐藏：`ready()` 前节点 `set_enabled(false)`，`ready()` 后由 §9 的可见性函数决定（不是无条件 `true`）。

---

## 8. 角色与生命周期（shell 族）

- `get_treeland_wallpaper_surface(surface, file_source)`：
  - 检查 `surface` 是否已有角色（`wlr_surface_has_role` / 子表面树），已有则 `wl_resource_post_error`（沿用 treeland `get_treeland_wallpaper_surface` 的 NULL 检查 + 角色约束）。
  - `wlr_surface_set_role()` + `wlr_surface_set_role_object()` 赋"treeland_wallpaper"角色。
  - 建 `struct woodland_wallpaper_surface`：包 `wlr_surface *`、引用 `file_source`、ready 标志。
  - 在 `wlr_surface_role.destroy` / surface destroy 信号里回收（参考 `layer.c` 的 `handle_new_layer_surface` / 节点销毁回收）。
  - 维护 `produced_wallpapers` 列表（源字符串），用于 `handleWallpaperSurfaceAdded` 时清理 notifier 里已无 surface 对应的源（对齐 treeland `WallpaperManager::handleWallpaperSurfaceAdded`）。
- `ready()`：标记 ready（显示）。
- `source_failed(error)`：记日志；可触发 fallback 纯色。`error` 取值即 §3.1 的 `invalid_source` / `permission_denied`。
- 多 surface 并发：v1 不强制单例（与 treeland 一致）。

---

## 9. 播放控制（合成器 → 客户端事件）

核心是一条函数：`wallpaper_apply_visibility()`。它算出"此刻该显示哪个源"，然后

1. 匹配该源的 surface → 节点 `set_enabled(true)`；其余 → `set_enabled(false)`；
2. 状态真的变了才发事件（每张 surface 记 `play_state`，三态：没说 / paused / playing），
   免得切换路径上重复发事件把客户端的调速逻辑打断。

| 触发时机 | 结论 |
|---|---|
| 当前源可见 | `play()` |
| 源不可见（别的 workspace 的、锁屏期间的桌面壁纸…） | `pause()`，**不再继续解码** |
| `ready()` | 由上面两条给出**首帧的显式告知**（客户端不用猜） |
| `slow_down()` | **v1 不发**，见下 |

**"哪个源该显示"**：锁屏期间取锁屏源，否则取**当前 workspace** 的桌面源；输出取配置里
第一块**已连接**的输出（配置文件里可能留着上次拔掉的屏）。一个源都匹配不上时（客户端
还没为它建 surface）**全部显示**——宁可多盖一张，也不要黑屏。

**`slow_down` 为什么不发**：treeland 只在 `show-desktop` 请求时发它
（`SourceOutput.qml` 的 `onShowDesktopRequested`，3000ms），那是 QML shell 的功能，
woodland 没有"显示桌面"这个概念。规格最初写的是"全屏窗口遮挡 → `slow_down(0)`"，属于
自创语义，按"以 treeland 现状为准"退掉：真要做，触发点应该在"遮挡检测"这个尚不存在的
能力上，而不是在壁纸族里硬凑。协议留着的 `slow_down` / `set_playback_rate` / `position`
三个事件在客户端侧（factory 的 QtWayland 集成）都已实现，将来补触发点不用改协议。

**workspace 联动**：切换工作区的唯一收口是 `woodland_workspace_switch_focus()`，它在状态
落定后 `wl_signal_emit_mutable(&server->workspace_changed, target)`；壁纸族挂监听重跑
可见性。用信号不用直接调用，理由同 `server->output_removed`：workspace 层不能反向依赖
协议层（否则每个链接 workspace 库的单元测试都要连带链接协议族）。

---

## 10. 与 session-lock 的关系

- woodland **已实现** session-lock（`src/session_lock/session_lock.c`，基于 `wlr_session_lock_v1`）。
- **锁屏壁纸与 session-lock 实现解耦**：woodland 的锁屏屏只渲染 lock surface（`session_lock.c`），**不渲染壁纸**。
- 因此 `lockscreen` role 的处理 = 与 desktop 完全相同的 manager 存储 + `changed`/`failed` 回报 + notifier 广播；只是 woodland 自己不把它画出来。factory 若想在锁屏绘制，自行根据收到的 `changed(lockscreen, ...)` 创建锁屏壁纸 surface。
- 播放控制的 `pause` 在锁屏激活时照常下发（§9），影响的是 factory 的桌面壁纸动画，与锁屏渲染互不干扰。

---

## 11. 已确认的设计决策（用户 2026-11-05 拍板）

| # | 议题 | 决策 |
|---|---|---|
| 1 | 谁来渲染 | **复用 `treeland-wallpaper-factory`**，woodland 不内置解码器、不自己启动客户端（无 launcher）。壁纸 surface 渲染在 layer-shell `background` 层之下。 |
| 2 | 现状核对 | 以 treeland 当前代码为权威参考（已读 `wallpapermanager.cpp` / `wallpapershellinterfacev1.cpp` / `wallpapermanagerinterfacev1.cpp` / `wallpapernotifierinterfacev1.cpp` / `wallpaperlauncher.cpp` / `wallpaper-factory/*`）。 |
| 3 | session-lock | woodland 已有 session-lock，但**与壁纸无关**：lockscreen role 只存储+转发，woodland 锁屏屏不渲染它。 |
| 4 | 默认壁纸/fallback | `/usr/share/wallpapers/deepin/deepin-default.jpg`（对齐 treeland `DEFAULT_WALLPAPER`）；缺失回退纯色。 |
| 5 | 视频解码范围 | `set_video_source` 只校验+登记+广播（type=video），**不解码**；解码全在 factory(mpv)。 |
| 6 | 单例强制 | 与 treeland 一致：**不强制** shell/man 多 bind。 |
| 7 | 持久化 | 与 treeland 数据模型一致，但**用纯配置文件** `$XDG_CONFIG_HOME/woodland/wallpaper.json`，不用 dconfig。 |

---

## 12. 实施顺序（建议）

1. ~~构建 codegen~~ ✅ `src/protocol/meson.build` 用 `wayland_scanner` 生成两协议的
   `server-header` + `private-code`；XML 用仓库内副本 `protocols/treeland/`（CI 没装
   `treeland-protocols` 包）。
2. ~~manager 最小闭环~~ ✅ 三个全局 + `get_treeland_wallpaper` + `set_*_source` 校验 +
   `changed` / `failed` + `(output, role, workspace)` 内存存储。
3. ~~notifier 源列表~~ ✅ `add` / `remove` 广播；新 bind 重放；**列表完全由配置推导**
   （不额外维护 `produced_wallpapers`，两处状态迟早不一致）。
4. ~~shell 角色 + 场景最底层~~ ✅ 角色赋值、`server->wallpaper_tree`、`lower_to_bottom`、
   `ready` / `source_failed`、表面回收（不放 `role.destroy`：它只拿得到 `wlr_surface`，
   拿不到包装结构；统一放资源析构）。
5. ~~配置文件读写~~ ✅ §6 的模型 + 手写 JSON 读写 + 默认/新增 output/workspace 同步；
   写入用临时文件 + `rename`。
6. ~~播放控制 + workspace 联动~~ ✅ §9 的可见性函数；workspace 侧发信号。
   **与原计划不同的一点**：不再"切工作区时给 manager 客户端重发 `changed(desktop)`"——
   treeland 也只在创建 wallpaper 对象时发一次（`onWallpaperAdded`），重发会让设置面板
   以为"值被别人改了"；真正需要的是重挑可见 surface（§9）。
7. ~~全程 headless 冒烟~~ ✅ §13。

---

## 13. 测试（headless 冒烟 + 单元）

一个测试、三段跑，全在 `smoke-wallpaper`（`tests/smoke/run_wallpaper_smoke.sh`）：

1. **主流程**（`wallpaper_probe`）：bind notifier 收 `add` 重放；`get_treeland_wallpaper`
   收 lockscreen + desktop 两条 `changed`；`set_image_source` 合法 → notifier 多一条；
   不存在/白名单外 → `failed(invalid_source)`；重复 → `failed(already_used)`；
   `set_video_source` → 类型由请求决定；shell 赋角色 + commit + `ready` → `play`。
   脚本侧再查：`wallpaper.json` 落盘且带输出名与源、日志里有 `wallpaper surface for`。
2. **配置回读**：重启合成器，`--expect-desktop <path>` 断言读回来的桌面壁纸和上次一致
   （写/读不对称是最常见的坑）。
3. **工作区联动**：协议本身没法"只给某个工作区设壁纸"（`set_*_source` 写全部 workspace，
   对齐 treeland 的 `setOutputWallpaper`——那里 `workspaceIndex` 参数是 `[[maybe_unused]]`），
   所以脚本直接改配置文件的 `workspaceIndex 1` 那一项（`sed` 定位，顺带把写入器的键顺序
   钉住），再起一份干净的合成器。探针造两个 surface（各对应一张图）+ 用 **ext-workspace**
   的 `activate` 切到工作区 2（headless 没有键盘设备，这是唯一能程序化切换的路子），断言
   可见的那张跟着换边：`played=1/1 paused=1/1`。

**合成器侧不变量**：`wctl self_check` 的 `wallpaper_below_layers`——壁纸树必须是 scene 根
的**第一个**子节点。这条掉了只表现为"壁纸被 layer-shell 的 background 盖住"，截图里几乎
看不出来（§7 的图层树也是 `lower_to_bottom` 建的，输出热插拔会把壁纸顶上去）。冒烟里
用 `jq` 断言它 `ok==true`；`tests/unit/test_selfcheck.c` 还单独覆盖了"没压底 → 失败"这一支。

---

## 14. 风险与开放问题

- **外部依赖**：woodland 运行依赖 `treeland-wallpaper-factory` 存在并被会话拉起；缺失时只有纯色背景，无壁纸（不崩溃）。是否在 woodland 文档里标注此依赖？
- **多 output 不同壁纸**：v1 单全局 surface，无法 per-output 不同；需 factory 多 surface 或协议扩展，推迟。
- **workspace 依赖**：per-workspace desktop 壁纸需要 woodland 当前 workspace 追踪（`woodland_workspace_switch` + 一个"当前 workspace"指针与变更信号）。若 woodland 当前未暴露"当前 workspace 变更"信号，需先补一个（工作量小）。
- **path 白名单**：若 dde 传 `/home/...` 自定义壁纸被拒，需把用户壁纸目录纳入白名单（§5 已留 `$XDG_DATA_HOME/wallpapers/`）。

---

## 15. 实现记录（落地时发现 / 决定的事）

文件：`protocols/treeland/*.xml`（协议副本）、`src/protocol/wallpaper.c`、
`src/protocol/wallpaper_config.c`、`src/protocol/wallpaper_internal.h`、
`tests/smoke/wallpaper_probe.c`、`tests/smoke/run_wallpaper_smoke.sh`。

- **`already_used` 必须在写入配置之前判定**。写完再判，每一次设置都会误报
  （这个源显然"正在被用"）。treeland 同语义：它是"这张图已经在缓存里"的提示，非致命。
- **新的跨层信号 `server->workspace_changed`**（`include/woodland.h`）。理由见 §9。
- **`server->wallpaper_tree` 挂在 server 上**，而不是让 self_check 反查协议层：
  `libwoodland-shell` 不能反过来依赖 `woodland-protocol`，否则每个链接 shell 的单元测试
  都要连带链接协议族（第 3 节的分层就是反着来的）。
- **`woodland_protocol_init()` 的前置条件补齐了 workspace**：壁纸族要挂
  `workspace_changed`，而单元测试的 fixture 是"手工搭一个 server"——少 init 一个模块
  就会挂到未初始化的 `wl_signal` 上，表现是 `wl_list_insert` 段错误而不是启动失败。
  现在缺 `active_workspace` 直接返回 false；`test_gamma_control` / `test_drm_lease` /
  `test_pointer_constraints` 三个 fixture 相应补了 `woodland_workspace_init/finish`。
- **`server.c` 里的重复 `wlr_renderer_init_wl_shm()` 已删除**：`wlr_renderer_init_wl_display()`
  内部已经调过，再调一次会出现两个 `wl_shm` 全局（`wayland-info` 能看到 name 1 和 5），
  客户端按名字各绑一个；冒烟探针里表现为"绑第二个时把第一个代理泄漏掉"，ASan 报 96 字节。
- **探针自己的泄漏也要治**：`ext-workspace` 的 group 句柄是事件里 `new_id` 出来的代理，
  客户端必须自己销毁。不销毁时 LeakSanitizer 同样报 96 字节（`wl_display_read_events`
  里给新代理分配的闭包），看起来像是合成器在漏。
- **`play_state` 用三态而不是 bool**：`ready()` 时必须给客户端**明确**的首帧告知（play 或
  pause 之一），否则"这张当前不该播"只能靠客户端猜；用 bool 且初值 false 会把首个 pause
  当成"已经说过"而吞掉。
- **`realpath` 需要 `_XOPEN_SOURCE`**（glibc）：`_POSIX_C_SOURCE` 只够声明其余 POSIX 函数，
  少了它是 `-Werror=implicit-function-declaration`。
