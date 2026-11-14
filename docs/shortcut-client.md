# 全局快捷键客户端（woodland-shortcutd）

> 日期：2026-11-14  
> 状态：已实现  
> 相关文件：`src/shortcutd/`、`include/shortcutd_config.h`、`src/protocol/shortcut.c`、`src/server/server.c`

---

## 1. 为什么需要一个客户端

`treeland_shortcut_manager_unstable_v3` 在合成器侧（`src/protocol/shortcut.c`）实现的是一个
**注册表 + 执行器**：客户端 `acquire` 拿独占，把带名字的 `bind_key` 注册进来，命中后合成器
要么执行 `action`（切工作区、贴边、最大化……），要么给 `action=notify` 的绑定回一个
`activated` 事件让客户端自己处理命令。

协议本身**不含任何默认值**。所以"有没有全局快捷键"取决于会话里有没有人注册：

| 会话 | 谁来注册 | 结果 |
| --- | --- | --- |
| DDE / deepin | `dde-daemon` | 正常 |
| LXQt / noctalia / quickshell / 裸 wlroots | 没有 | 只有合成器内置的 `Alt+Tab`、`Alt+F1..F9`、`Alt+1..0` |

`woodland-shortcutd`（下称 wsd）就是补上第二行那个空缺：它是一个普通的 Wayland 客户端，
读一份 TOML 配置，把用户要的键位注册进合成器；`command` 类绑定命中后自己 `fork/exec`。

## 2. 数据流

```
$XDG_CONFIG_HOME/woodland/shortcuts.toml
        │  (没有文件 → 内置默认键位)
        ▼
  woodland-shortcutd ──acquire + bind_key──▶ 合成器 binding 表
        ▲                                        │
        │  activated(name)                        │ 用户按键
        └────────────────────────────────────────┤
                                                 ▼
                                   action≠notify → 合成器自己执行
                                   action=notify → 回 activated
                                                 │
                                    wsd: /bin/sh -c "<command>"
```

关键点：**命令在新会话里运行**。`wsd_spawn_command()` 走 `fork → setsid → fork → exec`，
中间的壳进程被阻塞回收，真正的命令被 init 收养——按 `Super+Return` 拉起的终端不会跟着
wsd 一起退出，也不会在进程表里攒僵尸。

## 3. 什么时候启动

协议规定同一时刻**只有一个客户端**能持有 manager（第二个 `acquire` 直接 `occupied`
协议错误）。DDE 会话里那个位置属于 `dde-daemon`，woodland 抢先拉自己的客户端会让 DDE 的
全局快捷键整套失效，而且失败在对方那一侧——用户只会看到"DDE 快捷键突然不好使"。
所以启动判定必须在合成器侧做，按会话的桌面名分流：

| 条件 | 行为 |
| --- | --- |
| `--no-shortcut-client` | 不启动（测试 / 用户自己接管） |
| `$WOODLAND_SHORTCUTD` 为 `off`/`0`/`false`/`no`/`none`/空串 | 不启动 |
| 会话是 DDE（见下） | 不启动，日志说明"留给桌面" |
| 其它（`woodland:wlroots`、`KDE`、`GNOME`、`LXQt`……） | 启动 |

"是不是 DDE"看**三个**会话变量，任一命中就算：`XDG_CURRENT_DESKTOP` /
`XDG_SESSION_DESKTOP` / `DESKTOP_SESSION`。只看 `XDG_CURRENT_DESKTOP` 是不够的——
显示管理器的会话（尤其 lightdm 的 deepin 会话、老的 `.desktop` 会话）不一定把它设上，
漏判的代价正好是最坏的那种：dde-daemon 拿不到独占，整套 DDE 快捷键失效。反过来误判成
DDE 只是不启动客户端，用户可以显式开回来，代价小得多。

判定函数是 `util/env.c`：`woodland_session_is_dde()`（选变量）+
`woodland_desktop_is_dde()`（按 XDG 规范把取值当冒号分隔的 token 列表处理，大小写不敏感，
`Deepin` / `deepin-wayland` / `DDE:GNOME` 都认）。日志与 trace 会把三个变量的实际取值都
打出来，能直接看出是哪一个触发的判定。

启动点在 `woodland_server_start()` 的末尾——此时 socket、`WAYLAND_DISPLAY`、`DISPLAY`
都已就位。可执行文件的查找顺序：

1. `$WOODLAND_SHORTCUTD`（测试和开发时直接指向 build 目录里的二进制；也可写成绝对路径）
2. 与 `woodland` 同目录的 `woodland-shortcutd`（安装后同在 bindir；build 树里退一格是
   `src/shortcutd/`）
3. 编译期 `WOODLAND_BINDIR`（`config.h` 里由 meson 生成）
4. 都没有 → `execvp` 搜 `PATH`

判定过程写进决策轨迹：`wctl get_trace | jq '.entries[] | select(.what=="spawn")'`，一眼能看出
"为什么没起 / 起的是哪个文件"。

## 4. 配置

位置：`$XDG_CONFIG_HOME/woodland/shortcuts.toml`（回退 `~/.config/...`），`-c PATH` 可覆盖。

```toml
[general]
enabled = true          # false → 进程直接退出，不注册任何东西

# command 类：注册时 action=notify，命中后 wsd 执行命令
[[shortcut]]
key = "Meta+Return"
command = "foot"

# action 类：交给合成器执行内建动作，wsd 不参与
[[shortcut]]
key = "Meta+Left"
action = "tile_left"

# flags 默认 press；音量/亮度这类按住要连续变化的加 repeat
[[shortcut]]
key = "XF86AudioRaiseVolume"
command = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"
flags = "press|repeat"
```

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `key` | 是 | QKeySequence PortableText（`Ctrl+Alt+T`、`XF86AudioMute`、`Meta+Left`） |
| `command` | 与 `action` 二选一 | 命中后由 wsd 执行，走 `/bin/sh -c` |
| `action` | 与 `command` 二选一 | 合成器内建动作名，清单见 `woodland-shortcutd --help` |
| `flags` | 否 | `press` / `release` / `repeat`，用 `|` 或 `,` 组合，默认 `press` |

**校验分工**：wsd 只保证形状对（key 非空、action 已知、command 与 action 不打架）；键位是否
合法由合成器判定（`QKeySequence` 解析 + validity 规则只有一份，在 `src/protocol/shortcut.c`）。
被拒的绑定以 `bind_failure` 回来，wsd 打成一行日志：

```
woodland-shortcutd: 合成器拒绝了绑定 "Ctrl+Shift+Q"（invalid_argument）：Ctrl+Shift+Q
```

单条写错只丢那一条（带 `[[shortcut]] #N` 的警告），整份文件语法错误才是硬失败。
`--print-default-config` 的输出可以直接当配置文件用；用户复制的第一份配置就该是能跑的。

**同一个 `key` 只留第一条**，重复的条目整条跳过并警告。这不是保守：合成器里一条组合只能
有一条生效——同 `action` 的会被 `bind_key` 的 upsert 吞掉（并且保留先注册的 `name`，于是
第二条的 `command` 永远不会执行），不同 `action` 的会并存但只有表里靠前的那条会命中。
比较形式忽略大小写与空白（`meta+left` 与 `Meta+Left` 是同一个组合），因为合成器侧的
PortableText 解析对这两者都不敏感。

## 5. 内置默认键位

没有配置文件时用这一套（命令类按 `PATH` 挑第一个可用程序，一个候选都没有就跳过——
注册一条按下去没反应的键位比不注册更让人困惑）：

| 键位 | 行为 | 候选命令 |
| --- | --- | --- |
| `Meta+Return` | 终端 | `x-terminal-emulator` / `foot` / `alacritty` / `kitty` / `wezterm` / `qterminal` / `xfce4-terminal` / `konsole` / `gnome-terminal` / `xterm` |
| `Meta+E` | 文件管理器 | `dde-file-manager` / `nautilus` / `dolphin` / `pcmanfm-qt` / `thunar` / `pcmanfm` / `nemo` |
| `Meta+Q` | `close_window` | 合成器动作 |
| `Meta+Up` / `Meta+Down` | `maximize` / `cancel_maximize` | 合成器动作 |
| `Meta+Left` / `Meta+Right` | `tile_left` / `tile_right` | 合成器动作 |
| `XF86AudioRaiseVolume` / `LowerVolume` / `Mute` | 音量（前两个带 `repeat`） | `wpctl` → `pactl` |
| `XF86MonBrightnessUp` / `Down` | 亮度（带 `repeat`） | `brightnessctl` |
| `XF86AudioPlay` / `Next` / `Prev` | 播放控制 | `playerctl` |

## 6. 生命周期

1. 读配置（文件不存在 → 默认键位）。
2. `wl_display_connect` → `wl_registry` → 不出现 `treeland_shortcut_manager_v3` 就退出 4。
3. `acquire` → 逐条 `bind_key` → 一次 `roundtrip`（既把请求真正发出去，也顺便收
   `bind_failure`）。
4. 事件循环：手写 `ppoll` + `wl_display_prepare_read`，只有确认可读才 `read_events`；
   三个信号在等待之外保持阻塞，`ppoll` 的 sigmask 在等待期间原子地放开它们。
   （用 `poll` 会在"读完 pending_signal、还没进 poll"之间丢信号：handler 已经置上标志，
   `poll` 等的却是那次已经被消费掉的信号，于是再也醒不过来。丢一次 `SIGTERM` 就是一个
   永远占着独占 manager 的进程，下一轮会话注册不上任何快捷键。）
5. `activated` → 按名字查绑定 → `wsd_spawn_command()`。
6. `SIGHUP` → 重新读配置：**先读成功**再 unbind 旧的、bind 新的（读失败保留现状，用户
   改错了不该把手里的键位一起扔掉）。不放弃独占，不需要重新 `acquire`。重载成空配置时
   进程不退（合成器只在启动时拉一次，退掉就再也起不来了），但会明确警告"仍占着独占"。
7. 合成器断开 → 退出 0。socket 关闭时 libwayland 报的是 `EPIPE`/`ECONNRESET`，那是
   "会话结束"而不是错误，日志只留一行 info，退出码 0。

退出码：`0` 正常（含合成器退出）/ `1` 配置或运行期错误 / `2` 用法错误 / `3` 连不上
display / `4` 合成器没有该协议 / `5` 协议错误（最常见是 `occupied`）。

> 注意：合成器的 `SIGHUP` 只重载合成器自己的 `config.toml`。快捷键改完要
> `pkill -HUP -x woodland-shortcutd`（或重启会话）。

## 7. 测试

| 测试 | 覆盖 |
| --- | --- |
| `tests/unit/test_shortcutd_config.c` | action/flags 名表 0..45 往返、条目增删与 `key` 去重（含大小写/空白不同的同一组合）、TOML 读取与坏条目跳过、`enabled=false`、缺文件、语法错误、PATH 探测挑程序、示例配置往返 |
| `tests/unit/test_shortcutd_spawn.c` | 命令真的被执行、空命令被拒、连起 20 条不留僵尸（`waitpid` 报 `ECHILD`） |
| `tests/unit/test_env.c` | 桌面名判定（`DDE`/`Deepin`/`deepin-wayland`/`DDE:GNOME` 为真；`woodland:wlroots`/`KDE`/`ddex` 为假）、三个会话变量各自单独出现、`is_off` 词表 |
| `tests/smoke/run_shortcutd_smoke.sh` | 自动启动（非 DDE）→ `binding_count == 3` 且 key/action/flags 正确；第二个实例撞 `occupied` 非零退出；DDE 会话（含只有 `DESKTOP_SESSION=deepin`）不启动且 trace/日志给出原因；示例配置可回读；`SIGHUP` 重载后旧绑定撤掉、新绑定生效；退出释放独占；合成器退出时客户端退出 0 |

**没被自动化覆盖的**：`activated` → `wsd_spawn_command()` 这一段需要真的产生一次按键
（`tests/meson.build` 里的 wayland-protocols 没有 `virtual-keyboard-unstable-v1.xml`，
造不出合成的键事件），目前只有两端各自被测：合成器侧 `smoke-shortcut` 验 `activated`
事件与 `last_outcome`，客户端侧 `test_shortcutd_spawn` 验命令执行。中间那一步（按名字
查绑定）只有代码评审，没有测试。

冒烟测试里**默认不启动** wsd：`tests/smoke/lib.sh:smoke_init()` 会把
`WOODLAND_SHORTCUTD` 置为 `off`。它会 `acquire` 全局快捷键管理器并注册默认键位，把
`binding_count == 0`、`occupied`、`last_outcome` 这些断言全带偏。专测自动启动的
`run_shortcutd_smoke.sh` 自己覆盖这个变量。

手动验证：

```bash
woodland-shortcutd --dry-run            # 打印"打算注册什么"（连合成器都不用）
woodland-shortcutd --print-default-config
woodland-shortcutd -v -c ~/my-keys.toml # 前台跑，看每条 bind 与每次命中
wctl get_tree | jq '.shortcut_manager'  # 合成器侧：acquired / binding_count / bindings
```

## 8. 已知边界

- **裸 `Meta` 不能 `bind_key`**：协议层有意不收（理由见 `src/protocol/shortcut.c` 文件头），
  capture 语义不受影响。
- **手势绑定只登记不触发**：swipe / hold 的 action 在合成器里没有识别状态机，wsd 也不注册
  这类（配置里没有对应字段）。
- **`occupied` 时 wsd 只报错退出**，不会重试：谁先拿到独占是会话层面的约定，抢是错的。
  真要在 DDE 会话里用它，先停掉 `dde-daemon` 的快捷键服务，再手动起。
- **桌面名判定依赖会话环境**：三个会话变量里既没有 `DDE` 也没有 `deepin` 的自定义会话会被
  当成非 DDE（于是 wsd 会尝试 `acquire`，失败就退出，不影响桌面）。反过来，环境里残留一个
  `DESKTOP_SESSION=deepin` 会让判定偏保守（不启动客户端），这种情况下用
  `WOODLAND_SHORTCUTD=/path/to/woodland-shortcutd` 手动指定并自己起。注意路径形式的取值
  优先于自动查找，但仍然要过 `[general] enabled` 与桌面判定（判定在合成器侧，指定路径
  不会绕过它）。
- 没有 GUI 键位编辑器，也没有 inotify 自动重载（`SIGHUP` 手动）。
