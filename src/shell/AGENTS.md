# Shell 模块智能体指南

## 1. 模块职责与核心文件

`src/shell` 是窗口管理（Window Management）的核心子系统：
- `src/shell/shell.c`：xdg-shell 协议对接、`struct woodland_view` 生命周期（创建、map、unmap、destroy）、跨层信号分发（`view_mapped`、`view_unmapped`、`view_changed`）。
- `src/shell/focus.c`：键盘与指针焦点管理、激活状态维护与视图提层（raise）。
- `src/shell/toplevel_state.c`：最大化、全屏、最小化状态切换与几何状态记忆。
- `src/shell/placement.c`：新窗口初始摆位策略（级联、居中等）。
- `src/shell/interactive.c`：交互式移动与缩放状态机。
- `src/shell/snap_preview.c`：半屏与四角贴边预览矩形、防抖定时器与吸附应用。
- `src/shell/window_switcher.c`：Alt+Tab 快速切换器（候选列表、z 序遍历、面板渲染与状态机）。
- `src/shell/window_menu.c`：窗口标题栏右键上下文菜单。

## 2. 关键硬约束

1. **场景节点相对坐标**：
   - 场景节点坐标始终相对于父节点。计算窗口内容或 SSD 偏移时，只能累加子节点偏移；切勿重复加上 `view->scene_tree` 自身的 `x/y`。
2. **贴边预览判据**：
   - 吸附半屏判定**仅看光标是否撞击工作区边沿**（`woodland_snap_determine_edges()`），严禁恢复窗口外框碰边判定。跨屏接缝必须保持 200ms 防抖，且定时器到期时二次校验抓取状态。
3. **切换器候选机制**：
   - 切换器遍历所有工作区可用窗口，**最小化窗口不剔除**（以 `(已最小化)` 标记展示），确认切换时执行恢复并聚焦。
4. **跨层信号发射规范**：
   - 发射 `view_mapped`、`view_unmapped`、`view_changed` 信号时必须调用 `woodland_signal_emit()` 辅助函数，防止空指针崩溃。

## 3. 相关文档

- 架构设计：[docs/xdg-shell-workspaces.md](../../docs/xdg-shell-workspaces.md)
- 切换器设计：[docs/window-switcher.md](../../docs/window-switcher.md)
- 陷阱参考：[docs/pitfalls/protocol-implementations.md](../../docs/pitfalls/protocol-implementations.md)（贴边预览与切换器探针细节）
