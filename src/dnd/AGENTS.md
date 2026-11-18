# DND 模块智能体指南

## 1. 模块职责与核心文件

`src/dnd` 负责实现 Wayland 数据设备（`wl_data_device`）的拖拽（Drag and Drop）交互机制：
- `src/dnd/dnd.c`：`wlr_data_device_manager` 初始化、拖拽生命周期监听（start_drag、motion、drop、destroy）、拖拽图标（drag icon）场景节点构建及其实时光标位置跟随。

## 2. 关键硬约束

1. **拖拽图标场景节点**：
   - 拖拽图标节点由 `wlr_scene_drag_icon_create()` 创建，挂载在专用图层（或 overlay 层上方），确保在拖拽移动过程中图标始终可见且不被普通窗口遮挡。
2. **指针抓取与释放**：
   - 拖拽激活时指针交互进入 DND 抓取状态，所有指针移动由拖拽状态机捕获并计算目标表面；拖拽松手（drop）或取消（cancel）后必须安全恢复指针默认焦点交互。
3. **状态可观测性**：
   - 拖拽的激活状态、位置与当前目标表面必须同步反映到 `wctl get_tree` 的 `.dnd` 字段中。

## 3. 相关文档

- 架构设计：[docs/clipboard-dnd.md](../../docs/clipboard-dnd.md)
