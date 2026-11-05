# Woodland 智能体指南

## 全局认识

- `woodland` 是**纯 C + wlroots** 的通用轻量 Wayland 合成器，目标是**兼容 treeland 生态（主要是协议）**，但**不复刻 treeland 的全部逻辑**。单体结构（sway/labwc 风格），单用户、运行在用户 session 内。
- 无 Qt/QML 依赖。渲染默认走 `wlr_scene` 直出；特效经 scenefx 实现且**默认关闭**。
- wlroots 经 meson wrap 下载，**锁定 tag 0.20.2**（不跟随 master）；scenefx 锁 0.5，**是硬依赖**（scene 实现固定走它）。两者都**不 vendored**。
- 许可证：GPL-2.0-only。

## 代码在哪里

- `include/`：公开头（模块间接口），`include/woodland.h` 是全局状态 `struct woodland_server`。
- `src/`：按模块子目录分层（`server/`、`util/`，后续 milestone 增加 `seat/`、`scene/`、`shell/` 等）。
- `tests/unit/`：单元测试；`tests/smoke/`：冒烟测试脚本。
- `protocols/treeland/`：从 treeland 生态抄来的协议 XML **只读副本**（MIT），
  由 `src/protocol/meson.build` 用 `wayland-scanner` 生成服务端头/接口表。
  不放代码、不改内容；同步办法见该目录的 `README.md`。CI 里没有
  `treeland-protocols` 包，所以**不要**改成从 `/usr/share` 取。
- `docs/superpowers/specs/`：设计规格；`docs/superpowers/plans/`：实现计划；`docs/superpowers/`：里程碑骨架规划。

模块职责划分以规格文档 §5 为准：`server`（backend/renderer/allocator/scene + 事件循环 + 渲染循环 + 输出热插拔）、`seat`（输入设备/光标/焦点）、`scene`（图层树）、`layout`（布局几何计算）等。

## 构建与测试

```bash
meson setup build -Dtest=enabled     # 首次配置（会自动下载 wrap 依赖）
meson compile -C build               # 编译
meson test -C build --print-errorlogs  # 跑全部测试
```

- 编译选项：`c_std=c11`、`warning_level=2` + `werror=true`（**警告即错误**）、`-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L`。
- 特性开关在 `meson_options.txt`：`tiling-layout`（默认 false）、`xwayland`（默认 true）、`test`（默认 false，跑测试要开）。**没有 `effects` 选项**——scenefx 常开，见下一节。
- 特性开关经 `configure_file` 生成 `build/config.h`，用 `WOODLAND_HAVE_*` 宏。
- 冒烟测试用 headless 后端：`tests/smoke/run_headless.sh` 内部以 `WLR_BACKENDS=headless` 后台启动 + `kill -TERM` + 等退出码 0（M1 起追加 `WLR_HEADLESS_OUTPUTS=1`）。ASan 验证用 `meson setup build-asan -Db_sanitize=address,undefined -Dtest=enabled`。`WLR_RENDERER=pixman` 已经没用，不要以为写它会切渲染器。
- **冒烟脚本一律先 `. tests/smoke/lib.sh` 再 `smoke_init <名字>`。** 失败时它把现场落到 `build/meson-logs/smoke/<名字>/`：`compositor.log`、`meta.txt`（参数 / socket / 退出码），有 `WCTL` 和 `SOCK` 时还会抓 `get_tree` / `get_focus` / `self_check` / `get_trace` / `dump_scene` 的 JSON 和一张 `screenshot.png`。脚本自己的收尾写成 `smoke_cleanup()`（dump 之后才调用），断言失败用 `smoke_fail "..."`。**不要再用 `cat 日志; exit 1`**——那正是"只有一行结论、没有现场"的老写法。
- **CI 跑在 `archlinux:base-devel`，不要换回 `debian:stable`。** wlroots 0.20 要 wayland >= 1.24、libdrm >= 2.4.129、xkbcommon >= 1.8、pixman >= 0.43，bookworm 一个都不满足，meson 会退到 wrap 里逐个编 fallback，链条断在第一个 wayland 子项目（`Dependency "expat" not found`）。缺 expat 只是表象，补它只会把失败推到下一个依赖。
- **顶层 `meson.build` 里给 `wlroots` 包的那层 `declare_dependency` 别删。** wlroots 作为子项目构建时，`wayland-scanner` 生成的协议头落在 `build/subprojects/wlroots/protocol/`，而它自己只导出 `include/`，于是 `wlr/types/wlr_layer_shell_v1.h` 里 `#include "wlr-layer-shell-unstable-v1-protocol.h"` 直接编译失败。系统装过 wlroots 的机器上 `/usr/include` 有这个头，问题会被掩盖——**本地能编不代表能编**。

### scenefx 必读（常开，不是可选特性）

场景库固定是 scenefx 0.5，它**自带一套同名 `wlr_scene_*`** 符号（链接顺序 `scene_deps = [scenefx, wlroots]`），带来 blur / 圆角 / 阴影节点。想"关掉特效"是**运行期**的事（`[effects] enabled=false`），不存在只编一套的构建。四条硬约束：

1. **`.c` 里只 `#include "scene.h"`，不要直接 include `wlr/types/wlr_scene.h` 或 `scenefx/...`。** `include/scene.h` 是唯一入口，混用会让同一个结构体在两个 TU 里有不同布局。
2. **不要再加 `#if WOODLAND_HAVE_EFFECTS` 这类编译期分支。** 宏已经不存在；新增"要不要画"的判断一律走 `woodland_effect_server_enabled()` / `woodland_effect_server_blur_enabled()`。曾经因为宏判断写在 `#include "config.h"` 之前、`#if` 恒假，整段代码被静默编掉（X11 的 context destroy 监听器没了，留下悬空 view）。
3. **场景节点坐标是"相对父节点"的，算位置时只累计子节点的偏移。** `view->scene_tree` 的原点就是内容原点（SSD 的标题栏/边框在负坐标上），把 tree 自己的 `x/y` 也加进去等于把标题栏高度算两遍——blur 节点会整体下沉一个标题栏（27px），肉眼像"模糊糊到了标题栏以外的位置"。`dump_scene` 里 `blur pos` 与内容 `buffer pos` 对不上就是这个。
4. **`server->renderer` 必须是 `fx_renderer_create()`。** scenefx 的 `wlr_scene_output_commit()` 把 render pass 强转成自己的 `fx_render_pass`，塞 wlroots 的 pixman/gles2 renderer 进去会在 `fx_get_render_pass` 里断言死（`test_session_lock` 就是这么挂的）。它只走 EGL/GLES2：没有 DRM 渲染节点时（容器 / 纯 headless）`server.c` 会自动 `WLR_RENDERER_FORCE_SOFTWARE=1` 重试一次，落到 llvmpipe 的 surfaceless EGL。**回退不到 pixman**——scene 只认 fx render pass。

配置只切"画不画"，不切"记不记"：`wctl get_tree` 的 `bg_effect`、`deco.rendered`、`--trace` 在开关两侧都要能查。`[effects]` 关掉后 `get_tree` 的 `effects` / `effects_blur` 为 false、`deco.rendered` 里没有 `corner_radius`、`dump_scene` 里没有 blur 节点、ext-background-effect 的 capability 报 0（已连接的客户端会在 `reload_config` 后收到重播）。想确认"到底画没画"，查这几处，不要靠截图猜。

## 调试接口（wctl）

合成器在 `$XDG_RUNTIME_DIR/woodland-<wayland socket>.sock` 上开一个**只读**调试 socket，路径同时导出到 `WOODLAND_IPC`。协议是「一行一条命令、一行一条 JSON 回应」，所以 `wctl` 和 `nc -U` 都能用：

```bash
wctl get_tree   | jq '.views[] | {id,title,focused,activated,outer,snapped}'
wctl get_tree   | jq '.views[] | {id, margins: .ssd.margins, deco}'
wctl get_focus  | jq .        # 焦点在谁身上（recorded_view vs keyboard_view）
wctl get_trace  | jq '.entries[] | "\(.seq) \(.what) \(.obj) \(.detail)"'
wctl self_check | jq .        # 不变量断言；失败项带 expected / actual
wctl dump_scene | jq '.nodes' # wlr_scene 树，排查「画了但看不见」
wctl --help
```

**每个命令的完整字段清单（名字+类型+说明）在 `wctl help` 的响应里**，加新命令时同步更新 `src/ipc/commands.c` 顶部的 `ipc_cmd_docs` 表——它是唯一事实源，help 输出由它生成。

### 决策轨迹：`--trace` + `get_trace`

`get_tree` 回答"现在是什么样"，**回答不了"为什么变成这样"**。加 `--trace` 启动后，合成器把最近 4096 次状态变更记进环缓冲（满了覆盖最旧的），`wctl get_trace` 按 `seq` 升序导出：

```
{"ok":true,"cmd":"get_trace","enabled":true,"capacity":4096,"count":4,"total":4,"dropped":0,
 "entries":[{"seq":0,"ms":13,"obj":0,"what":"output_add","detail":"HEADLESS-1 1280x720 heads=1"},
            {"seq":1,"ms":16,"obj":1,"what":"map","detail":"ws=1 parent=0"},
            {"seq":2,"ms":31,"obj":1,"what":"place","detail":"policy=cascade 250x250 at 1,27"},
            {"seq":3,"ms":31,"obj":1,"what":"focus","detail":"prev=0 raise=1"}]}
```

- `what` 是**稳定标签**（脚本会 grep），权威清单在 `include/trace.h`：`map` / `unmap` / `destroy` / `focus` / `focus_none` / `layer_focus` / `place` / `workspace` / `output_add` / `output_remove` / `cfg_apply` / `deco` / `bg_effect`。加新标签先改 trace.h 的清单，再发第一条轨迹。
- `detail` 只写**决策依据**（走了哪个分支、策略名、抑制窗口），不要抄 `get_tree` 已有的数值。
- 关闭时零分配零格式化；打开后也不分配（一次 calloc）。**它只依赖 `util/json`，不许反向依赖任何业务模块**——否则会成为依赖环的入口。
- 环的语义：`seq` 全局单调，`total` 是累计条数，`dropped = total - count` 就是被覆盖掉的条数。第一个 entry 的 `seq` 一定等于 `dropped`。
- 回归：`tests/unit/test_trace.c`（覆盖顺序、截断、开关），`smoke-ipc` 断言 `map → place → focus` 的因果顺序。

`wctl` 找 socket 的顺序：`-s <path>` → `$WOODLAND_IPC` → 扫 `$XDG_RUNTIME_DIR` 里最新的 `woodland-*.sock`。`ok=false` 时退出码 1。

**查状态优先用 `wctl get_tree` / `get_focus`，不要去数截图里的像素。** "非激活标题栏到底是 246 还是 247" 这类问题，`wctl get_tree | jq '[.views[] | select(.activated)] | length'` 一句话就说清了。

`--self-check` 打开后，每次状态变化（map / unmap / destroy / 焦点切换）都会在 **idle** 阶段跑一遍不变量断言（视图链表完整、焦点可聚焦且与键盘 surface 一致、激活态唯一、SSD 颜色 / foreign-toplevel / xdg configure 与激活态一致、SSD 几何与内容尺寸一致），失败记 `self-check <name> failed after <reason>: expected .., got ..` 的 ERROR 日志。它不退出进程，但会让 `tests/smoke/run_ipc_smoke.sh` 失败——改完窗口状态相关的代码后跑一次 `--self-check` 是成本最低的回归。

## ⚠️ 调试陷阱（必读）

**绝不要用 gdb/lldb 的 `run` 或 `r` 来调试 woodland。**

woodland 是 Wayland 合成器：主循环会**正常阻塞在 `ep_poll`** 等待事件（客户端连接、输入、frame 等）。程序不崩溃、不命中断点，`gdb run` 就**永远不返回**，`bt`/`bt full` 永远执行不到。这不是死锁，是合成器本该如此。

**正确的验证/调试方式：**

1. **功能验证靠测试**：`meson test -C build --print-errorlogs`（冒烟测试脚本自己会后台启动 + 发 SIGTERM + 收退出码，不需要你手动跑二进制）。
2. **查运行时状态用 `wctl`**：`wctl get_tree` / `get_focus` / `self_check`（见上一节），比截图和推理都可靠。
3. **要看运行日志**：`WLR_BACKENDS=headless ./build/src/woodland 2>&1` 直接看 wlr_log 输出（前台阻塞是正常的，看完 `Ctrl-C` 或另发 SIGTERM 即可）。没有 DRM 节点时它会自动落到 llvmpipe，日志里第一条 `no DRM render node, retrying with llvmpipe` 属于正常。
4. **要排查崩溃**：用 ASan（`build-asan`）+ 崩溃时 ASan 会自动打印栈，或者给 wlr_log 加日志/读代码推理。**不要 attach gdb 到阻塞的事件循环上等它"出问题"**。
5. **确需 gdb 时**：只对**已崩溃的 core dump** 做 `gdb ./woodland core`（事后分析，不是 `run`），或对纯函数（不跑事件循环）的单元测试用 gdb。

**ASan 二进制在这台机器上要先关掉 ASLR。** 直接跑 `build-asan/` 下的程序有时会刷屏输出
`AddressSanitizer:DEADLYSIGNAL` 然后卡住不返回（`--help` 都跑不完），看起来像崩溃循环，
实际是 ASan 与该内核的高熵 ASLR 不兼容，和代码无关。用 `setarch $(uname -m) -R ./xxx` 关掉
ASLR 就正常了。**别把这种 DEADLYSIGNAL 当成自己的 bug 去修**，先关 ASLR 复现一次再说。

## ⚠️ wlroots 语义陷阱（接协议时最容易踩）

**1. `wlr_idle_inhibitor_v1` 销毁时先 emit destroy、后摘链表。**
`types/wlr_idle_inhibit_v1.c` 的 `idle_inhibitor_v1_destroy()` 顺序是
`wl_signal_emit_mutable(destroy)` → `wl_list_remove(&inhibitor->link)`。所以 destroy
回调里读 `manager->inhibitors` 仍能看到正在销毁的那一个：
按它判空会让**最后一个抑制器关掉后 idle 永久停摆**（屏幕再也不熄）。
聚合要数自己的链表，或按 `wl_list_length(...) > 1` 判断（见 `src/protocol/idle.c`）。
回归测试：`meson test -C build smoke-idle-inhibit`（旧实现会 FAIL）。

**2. `wl_signal` 的监听器按注册顺序调用，顺序就是行为。**
`server->cursor->events.motion` 上，协议层（发 relative-pointer）和 shell（更新 pointer
focus）各挂一个回调。协议层先注册就会用**旧焦点**发相对位移：跨 surface 的第一帧
delta 被丢弃或发给刚离开的客户端。所以 `woodland_cursor_init_events()` 必须早于
`woodland_protocol_init()`（见 `src/server/server.c`）。同理，新输出的补广播监听器
必须注册在 `handle_new_output` 之后。注册顺序现在可以从运行时查证：
`wctl get_listeners` 按 seq 列出全部监听器（seq 即调用顺序），smoke-ipc 断言了
`cursor.motion` 早于 `input.motion`。**新注册一律用 `woodland_listener_connect()`
（`util/listener.h`），不要再手写 `notify` 赋值 + `wl_signal_add`**——名字和注册
序号进账本，漏注册/顺序反了一眼看出。

**3. `output-management` 的 test 请求不许有副作用。**
协议要求 test 只验证、不应用；布局变更（`wlr_output_layout_add/remove`）必须在
commit 成功之后做。多输出要用 `wlr_backend_test()` + `wlr_backend_commit()` 整组过，
不要逐个 `wlr_output_commit_state()` —— 后者中途失败没法撤销。
回归测试：`meson test -C build smoke-output-mgmt`（`wlr-randr --dryrun` 挪位置后位置必须不变）。

**4. apply 的 `succeeded` 必须先于新一轮 `configuration` + `done`。**
协议在 `succeeded` 的描述里明说"若配置有变，变更事件跟在 succeeded 之后"。而
`apply_layout()` 里的 `wlr_output_layout_add/remove` 会触发 layout change，广播就挂在
那个信号上——所以 `server->proto->output->applying` 的抑制范围要一直盖到
`wlr_output_configuration_v1_send_succeeded()` 之后，再统一补一次广播。只盖住
test/commit、写布局时已放开，客户端就会先收到 configuration（wdisplays/kanshi 会把它
当成另一轮配置变更）。同理：**输出拔除**也要补一份 configuration，被 `--off` 禁用的
输出早已不在 layout 里，销毁它不会触发 layout change，只剩 `server->output_removed`
这一条路（`src/output/output.c` 发，`src/protocol/output.c` 收）。
回归测试：`meson test -C build smoke-output-mgmt-order`（旧顺序会 FAIL）。

**5. 协议对象别死攥 `struct woodland_view *`。**
客户端可以在 toplevel 之后随时销毁它，而我们挂在 `xdg_toplevel->events.destroy` /
`xwayland_surface->events.destroy` 上的监听器**一定排在 shell 那个之后**（shell 在
`new_toplevel` 时就注册了，它在这个信号里把 view free 掉）。所以"视图没了"的回调
里**不许读 view**，只能清指针 + `wl_list_remove` 自己的监听器（信号所属对象正在
销毁，不摘就是悬挂链表节点）。参见 `src/protocol/decoration.c` 的
`context_handle_toplevel_destroy()`。

**6. 写探针时 `wl_display_dispatch_pending()` 返回的是"分发了几个事件"。**
把它当 0/1 判会立刻误判成"连接出错"——症状是探针在收到第一个 configure 后
就退出、打印一个假的协议错误。要判错只看 `< 0`；真要报协议错误码用
`wl_display_get_protocol_error()` 的返回值（`wl_display_get_error()` 给的是
libwayland 的 errno，协议错误恒为 EPROTO=71）。见 `tests/smoke/deco_probe.c`。

## 项目规则

- 只用 C（C11 起步）。不引入 C++/Qt/QML。
- 每个新源文件顶部 `// SPDX-License-Identifier: GPL-2.0-only`（版权行省略，仅 SPDX 行）。非源码文件（meson、配置）不需要。
- 资源释放纪律：`finish` 逆序释放，每步判空 + 置空（幂等），不能 double-free、不能 use-after-free。
- 单实例全局 `struct woodland_server`，模块间通过指针传递，不做依赖注入框架。
- 保持 diff 最小：只碰与任务相关的文件和符号，不做任务外的大范围重构/重命名/格式化。
- **不要直接推 `main`**：任何改动（包括只修 CI、只改文档）都开分支、走 PR 合入。`main` 只接受 fast-forward 之外的合并，提交历史要保持线性、一个主题一个 commit。
- 依赖只经 meson wrap（wlroots 0.20.2、scenefx 0.5），**不新增依赖**、不 vendored。
- 参照实现以 labwc 的 `src/output.c` / `src/input/cursor.c`、wlroots 的 `examples/scene-graph.c` 为权威（本机已下载：`/home/ut004971@uos/work/labwc`、`/home/ut004971@uos/work/wlroots`）。

## 里程碑

进度见 `docs/superpowers/2026-10-22-woodland-skeleton.md`：

- **M0（已完成，已合入 main）**：可编译骨架（meson + wrap、util/log、server 生命周期、headless 冒烟）。
- **M1（进行中，feature/m1-output-render 分支）**：输出与渲染循环（输出热插拔、`wlr_output_layout`、`wlr_scene_output` 渲染、光标 + cursor-shape）。
- **M2~M6**：窗口管理、SSD/特效、私有协议、IME/壁纸/屏保、wine 协议。
