# Layer 模块智能体指南

## 1. 模块职责与核心文件

`src/layer` 负责对接 `wlr-layer-shell-v1` 协议，为桌面环境壳体（DDE Dock、Noctalia、Quickshell、LXQt 等）提供分层表面支持：
- `src/layer/layer.c`：`wlr_layer_shell_v1` 初始化、四个分层（`background`、`bottom`、`top`、`overlay`）的表面生命周期管理、独占区（exclusive zone）边距计算与键盘交互模式处理。

## 2. 关键硬约束

1. **场景层级划分**：
   - 四大图层在全局场景树中严格隔离：`background` < 普通应用窗口（`bottom` / `normal`） < `top` < `overlay`。
   - 壁纸根节点（`server->wallpaper_tree`）始终位于 `background` 之下。
2. **排版与工作区排他**：
   - 壳体表面的 `exclusive_zone` 变化时，必须重新计算对应输出的可用几何区（usable area），并通知 shell 模块重新排版最大化窗口及居中策略。
3. **时序契约**：
   - 严格遵循 layer-shell 规范：在表面首次提交（first commit）之前，不能向客户端发送配置或假设其有效几何。

## 3. 相关文档

- 架构设计：[docs/layer-shell.md](../../docs/layer-shell.md)
