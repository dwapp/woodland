# Woodland 智能体指南

## 全局认识

- `woodland` 是**纯 C + wlroots** 的通用轻量 Wayland 合成器，目标是**兼容 treeland 生态（主要是协议）**，但**不复刻 treeland 的全部逻辑**。单体结构（sway/labwc 风格），单用户、运行在用户 session 内。
- 无 Qt/QML 依赖。渲染默认走 `wlr_scene` 直出；特效经 scenefx 实现且**默认关闭**。
- wlroots 经 meson wrap 下载，**锁定 tag 0.20.2**（不跟随 master）；scenefx 锁定 0.5（随 `effects` 选项启用）。两者都**不 vendored**。
- 许可证：MIT。

## 代码在哪里

- `include/`：公开头（模块间接口），`include/woodland.h` 是全局状态 `struct woodland_server`。
- `src/`：按模块子目录分层（`server/`、`util/`，后续 milestone 增加 `seat/`、`scene/`、`shell/` 等）。
- `tests/unit/`：单元测试；`tests/smoke/`：冒烟测试脚本。
- `docs/superpowers/specs/`：设计规格；`docs/superpowers/plans/`：实现计划；`docs/superpowers/`：里程碑骨架规划。

模块职责划分以规格文档 §5 为准：`server`（backend/renderer/allocator/scene + 事件循环 + 渲染循环 + 输出热插拔）、`seat`（输入设备/光标/焦点）、`scene`（图层树）、`layout`（布局几何计算）等。

## 构建与测试

```bash
meson setup build -Dtest=enabled     # 首次配置（会自动下载 wrap 依赖）
meson compile -C build               # 编译
meson test -C build --print-errorlogs  # 跑全部测试
```

- 编译选项：`c_std=c11`、`warning_level=2` + `werror=true`（**警告即错误**）、`-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L`。
- 特性开关在 `meson_options.txt`：`effects`（默认 false）、`tiling-layout`（默认 false）、`xwayland`（默认 true）、`test`（默认 false，跑测试要开）。
- 特性开关经 `configure_file` 生成 `build/config.h`，用 `WOODLAND_HAVE_*` 宏。
- 冒烟测试用 headless 后端：`tests/smoke/run_headless.sh` 内部以 `WLR_BACKENDS=headless WLR_RENDERER=pixman` 后台启动 + `kill -TERM` + 等退出码 0（M1 起追加 `WLR_HEADLESS_OUTPUTS=1`）。ASan 验证用 `meson setup build-asan -Db_sanitize=address,undefined -Dtest=enabled`。

## ⚠️ 调试陷阱（必读）

**绝不要用 gdb/lldb 的 `run` 或 `r` 来调试 woodland。**

woodland 是 Wayland 合成器：主循环会**正常阻塞在 `ep_poll`** 等待事件（客户端连接、输入、frame 等）。程序不崩溃、不命中断点，`gdb run` 就**永远不返回**，`bt`/`bt full` 永远执行不到。这不是死锁，是合成器本该如此。

**正确的验证/调试方式：**

1. **功能验证靠测试**：`meson test -C build --print-errorlogs`（冒烟测试脚本自己会后台启动 + 发 SIGTERM + 收退出码，不需要你手动跑二进制）。
2. **要看运行日志**：`WLR_BACKENDS=headless WLR_RENDERER=pixman ./build/src/woodland 2>&1` 直接看 wlr_log 输出（前台阻塞是正常的，看完 `Ctrl-C` 或另发 SIGTERM 即可）。
3. **要排查崩溃**：用 ASan（`build-asan`）+ 崩溃时 ASan 会自动打印栈，或者给 wlr_log 加日志/读代码推理。**不要 attach gdb 到阻塞的事件循环上等它"出问题"**。
4. **确需 gdb 时**：只对**已崩溃的 core dump** 做 `gdb ./woodland core`（事后分析，不是 `run`），或对纯函数（不跑事件循环）的单元测试用 gdb。

## 项目规则

- 只用 C（C11 起步）。不引入 C++/Qt/QML。
- 每个新源文件顶部 `// SPDX-License-Identifier: MIT`（版权行省略，仅 SPDX 行）。非源码文件（meson、配置）不需要。
- 资源释放纪律：`finish` 逆序释放，每步判空 + 置空（幂等），不能 double-free、不能 use-after-free。
- 单实例全局 `struct woodland_server`，模块间通过指针传递，不做依赖注入框架。
- 保持 diff 最小：只碰与任务相关的文件和符号，不做任务外的大范围重构/重命名/格式化。
- 依赖只经 meson wrap（wlroots 0.20.2、scenefx 0.5），**不新增依赖**、不 vendored。
- 参照实现以 labwc 的 `src/output.c` / `src/input/cursor.c`、wlroots 的 `examples/scene-graph.c` 为权威（本机已下载：`/home/ut004971@uos/work/labwc`、`/home/ut004971@uos/work/wlroots`）。

## 里程碑

进度见 `docs/superpowers/2026-10-22-woodland-skeleton.md`：

- **M0（已完成，已合入 main）**：可编译骨架（meson + wrap、util/log、server 生命周期、headless 冒烟）。
- **M1（进行中，feature/m1-output-render 分支）**：输出与渲染循环（输出热插拔、`wlr_output_layout`、`wlr_scene_output` 渲染、光标 + cursor-shape）。
- **M2~M6**：窗口管理、SSD/特效、私有协议、IME/壁纸/屏保、wine 协议。
