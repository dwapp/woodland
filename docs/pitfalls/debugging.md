# 调试方法与工具踩坑

本文档汇总 Woodland 合成器的调试禁忌、正确排查手段、内存消毒器（ASan/LSan）环境兼容性，以及内置调试工具（wctl、trace、self-check）的使用规范。

---

## 1. 调试核心禁忌与排查方式

### 严禁使用 gdb/lldb 的 `run` 或 `r` 调试合成器
- **现象**：在 gdb 下执行 `run`，合成器输出若干行初始化日志后彻底“卡住”，不响应任何输入，`Ctrl-C` 捕获后堆栈停留在 `ep_poll` 或 `wl_event_loop_dispatch`。
- **原因**：Woodland 是标准的 Wayland 合成器，其主循环在空闲时会正常阻塞在 `epoll_wait` 监听客户端连接、输入设备和渲染帧事件。程序没有崩溃、也没有命中断点时，`run` 永远不会返回。
- **正确的排查方式**：
  1. **自动化测试优先**：使用 `meson test -C build --print-errorlogs`。冒烟测试脚本会自动在后台拉起合成器、发送操作、发送 SIGTERM 并回收退出状态。
  2. **状态排查使用 wctl**：使用 `wctl get_tree`、`wctl get_focus`、`wctl dump_scene` 等命令检查运行时对象，避免依赖肉眼观察或截图像素。
  3. **运行日志排查**：前台启动命令 `WLR_BACKENDS=headless ./build/src/woodland 2>&1`，直接阅读 wlr_log 输出。观察结束后发送 `Ctrl-C` 或 SIGTERM 即可。
  4. **崩溃排查**：优先使用 ASan 构建（`build-asan`），崩溃时会自动输出结构化符号调用栈；若必须使用 gdb，仅用于崩溃后生成的 core dump 进行事后分析（`gdb ./woodland core`），或对无事件循环的纯逻辑单测使用断点。

---

## 2. 消毒器（ASan / LSan）环境陷阱

### ASan 二进制与内核高熵 ASLR 冲突
- **现象**：直接运行 `build-asan/` 下编译出的二进制程序时，终端持续刷屏输出 `AddressSanitizer:DEADLYSIGNAL` 并卡死，甚至连 `--help` 都无法执行完成。
- **原因**：部分 Linux 内核开启了高熵地址空间布局随机化（ASLR），与 AddressSanitizer 的阴影内存（shadow memory）映射区域产生冲突。该问题与项目代码无关。
- **解决方法**：使用 `setarch` 关闭 ASLR 运行程序：
  ```bash
  setarch $(uname -m) -R ./build-asan/src/woodland
  ```

### 跑 `build-asan` 必须禁用内存泄漏检测（`detect_leaks=0`）
- **现象**：在 ASan 构建下执行 `meson test` 时，约一半的测试用例因信号 6（SIGABRT）挂掉，进程退出码为 134。
- **原因**：Mesa 驱动退出时故意不释放 DRI/EGL 的进程级单例缓存（栈顶可见 `driCreateNewScreen3`、`driCreateContextAttribs`、`drmGetDeviceNameFromFd2`、`FcFontRenderPrepare` 等）。Meson 为测试默认注入了 `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1`，LeakSanitizer 在程序正常退出阶段检测到驱动残留并触发 `abort()`。
- **解决方法**：显式关闭泄漏检测，保留地址越界与未定义行为检测：
  ```bash
  ASAN_OPTIONS=detect_leaks=0 meson test -C build-asan
  ```
- **注意**：在消毒器构建下，不要将 rc=134 轻易当作合成器自身崩溃，先排查是否为 LSan 驱动残留触发。

---

## 3. 调试工具与追踪机制规范

### 决策轨迹：`--trace` 与 `get_trace`
`get_tree` 仅反映当前合成器的静态状态快照，无法解释“状态为何变成这样”。Woodland 提供了环形缓冲区追踪机制：
- **容量与开销**：固定 4096 条环形缓冲。未启用时零内存分配、零字符串格式化开销；启用后仅在启动时一次性 calloc 环形队列。
- **标签清单**：`what` 字段必须使用 `include/trace.h` 中定义的权威稳定标签（供测试脚本 grep 过滤）。新增标签必须先在 `include/trace.h` 登记，再在代码中发射。
- **详情规范**：`detail` 字段仅记录**决策依据**（如选择的分支、策略名称、抑制窗口的条件），禁止机械复制已在 `get_tree` 中存在的数值。
- **依赖约束**：`trace` 模块仅能依赖 `util/json`，**严禁反向依赖任何业务模块**，防止引入循环依赖。
- **环形缓冲区语义**：
  - `seq`：全局单调递增序号。
  - `total`：累计记录条数。
  - `count`：当前留存条数（上限 4096）。
  - `dropped`：被覆盖覆盖的旧记录数（`dropped = total - count`）。当前首条 entry 的 `seq` 严格等于 `dropped`。

### `--self-check` 不变量断言
以 `--self-check` 参数启动合成器后，每次发生视图映射（map）、解除映射（unmap）、销毁（destroy）或焦点切换时，系统会在事件循环的 **idle** 阶段自动执行全量不变量校验：
- 视图链表结构双向完整性。
- 当前焦点视图必须处于可聚焦状态，且与键盘目标 wl_surface 一致。
- 全局激活态视图必须唯一。
- SSD 渲染颜色、foreign-toplevel 状态位、xdg-toplevel configure 事件必须与激活状态严格保持一致。
- SSD 装饰几何必须与内容尺寸对齐。
- 壁纸根节点（`wallpaper_tree`）必须为 scene 根节点的第一个子节点。

若校验失败，合成器会在日志中输出 `self-check <name> failed after <reason>: expected .., got ..` 级别为 ERROR 的报告。在修改窗口状态机或图层管理逻辑后，运行一次 `--self-check` 是回归验证成本最低的方式。
