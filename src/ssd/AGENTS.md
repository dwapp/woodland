# SSD 模块智能体指南

## 1. 模块职责与核心文件

`src/ssd` 负责为不支持或未开启客户端装饰（CSD）的窗口提供服务端窗口装饰（Server-Side Decoration）：
- `src/ssd/ssd.c`：标题栏与边框场景节点搭建、按钮（最小化、最大化、关闭）布局与交互事件分发、调色板（palette）计算、scenefx 圆角剪裁应用。
- `src/ssd/text_buffer.c`：基于 Cairo 与 Pango 的窗口标题文本光栅化绘制。
- `src/ssd/icon.c`：XDG 图标查找、解析与缓存管理。

## 2. 关键硬约束

1. **圆角边框绘制机制（scenefx）**：
   - **严禁在 1px 细条上设圆角**：scenefx 按照矩形自身宽高裁剪半径，细条边框会导致圆角半径被截断失效。
   - 正确做法：使用覆盖整个外框的大矩形，配合 `clipped_region` 挖空内部内容区域，设置外半径 $R+b$、内半径 $R$（详见 `ssd_apply_frame_corners()`）。
2. **调色板唯一权威源**：
   - 窗口生效配色遵循四层覆盖优先级（客户端覆盖 -> 配色方案 -> 配置文件 -> 内置默认），由 `woodland_ssd_get_palette()` 统一计算。
   - 状态必须同步至 `wctl get_tree` 的 `.ssd.palette`。
   - 若用户在配置文件中显式指定过颜色（`ssd.colors_specified == true`），深色方案切换严禁覆盖用户的标题栏/边框色。
3. **字体微调规范**：
   - 修改字体必须通过 `woodland_font_with()` 操作 PangoFontDescription，严禁使用字符串拼接破坏 Bold 字重。

## 3. 相关文档

- 架构设计：[docs/treeland-decoration.md](../../docs/treeland-decoration.md)
- 陷阱参考：[docs/pitfalls/testing-and-ci.md](../../docs/pitfalls/testing-and-ci.md)（像素断言取色规范）
