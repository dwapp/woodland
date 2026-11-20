# IPC 模块智能体指南

## 1. 模块职责与核心文件

`src/ipc` 负责为 `wctl` 调试工具提供 Unix Domain Socket 通信与 JSON 协议分发：
- `src/ipc/ipc.c`：Socket 创建（`$XDG_RUNTIME_DIR/woodland-<socket>.sock`）、客户端连接管理与文本行解析。
- `src/ipc/commands.c`：只读查询命令（`get_tree`、`get_focus`、`dump_scene`、`get_trace`、`self_check`、`pixel`、`screenshot` 等）及权威命令文档表 `ipc_cmd_docs`。
- `src/ipc/inspect.c`：渲染像素与区域统计断言（`pixel`、`screenshot` 采样与场景归因）。
- `src/ipc/wait.c`：结构化事件/状态/像素等待引擎（`wait` 命令与同步/异步求值）。
- `src/ipc/ctl.c`：受控写操作实现（`focus_view`、`move`、`resize`、`maximize`、`fullscreen`、`pointer_move`、`inject_key` 等）。

## 2. 关键硬约束

1. **默认只读安全策略**：
   - IPC 调试接口默认仅允许只读查询；写控制命令必须显式在合成器启动时传入 `--debug-ctl` 才开放，未开启时必须安全拒绝（返回 `ok: false`）且不改动任何系统状态。
2. **命令文档唯一事实源**：
   - 新增或调整命令时，必须同步更新 `src/ipc/commands.c` 顶部的 `ipc_cmd_docs` 文档结构体表。`wctl help` 的输出完全由此表生成。
3. **协议格式规范**：
   - 交互严格遵循“一行命令请求、一行完整 JSON 响应”（换行符结尾）。单条命令限制在 255 字节以内。
4. **统一走状态机入口**：
   - 写命令严禁直接修改视图内部字段，必须调用对应的公共 API（如 `woodland_view_set_maximized`），并在 `--trace` 中记录标签为 `action`、detail 以 `debug-ctl` 开头的决策轨迹。

## 3. 相关文档

- 陷阱与工具指南：[docs/pitfalls/debugging.md](../../docs/pitfalls/debugging.md)
