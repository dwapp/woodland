# Effect 模块智能体指南

## 1. 模块职责与核心文件

`src/effect` 负责基于 scenefx 0.5 实现 Woodland 的朴素视觉特效（模糊、圆角、阴影）：
- `src/effect/effect.c`：特效生命周期、`ext-background-effect-v1` 协议支持、局部/全屏模糊节点管理、预模糊优化节点（`wlr_scene_optimized_blur`）及脏标记机制。

## 2. scenefx 核心硬约束（八条红线）

1. **头文件引用规范**：源文件必须通过 `#include "scene.h"` 引入场景头文件，**绝对禁止**直接 `#include <wlr/types/wlr_scene.h>` 或引用 scenefx 内部私有头。
2. **宏判断时序**：针对 `WOODLAND_HAVE_EFFECTS` 的 `#if` 条件必须位于 `#include "config.h"` 之后。
3. **坐标系累加**：场景节点坐标始终相对于父节点，计算绝对位置时只累加子节点偏移，不要把 `view->scene_tree` 自身的 `x/y` 算两次。
4. **渲染器配套**：启用特效构建时，`server->renderer` 必须为 `fx_renderer_create()`，scenefx 的渲染遍（render pass）强依赖此实例。
5. **圆角裁切规则**：严禁在 1px 细边框条上直接设置 `corner_radii`；必须使用外矩形结合 `clipped_region` 挖空方式绘制圆角边框。
6. **局部模糊区域限制**：scenefx 0.5 的 blur 节点不支持 `clipped_region`。局部模糊必须将 blur 节点物理尺寸与位置匹配到 region 矩形上，且**不得挂载内容 buffer 作为 transparency mask**（否则 mask 会拉伸形变）。
7. **预模糊优化（Optimized Blur）**：预模糊节点位于 `background` 与 `bottom` 层之间，采样壁纸与桌面底图；底层壁纸发生 commit 或可见性切换时，必须调用 `woodland_effect_mark_blur_dirty()` 标记失效。
8. **双库符号混合链接**：scenefx 仅替换 buffer/rect/blur/shadow/tree 基础层；子树与表面创建接口（如 `wlr_scene_surface_create`、`wlr_scene_drag_icon_create`）由 libwlroots 导出。两库在头文件中逐字段二进制兼容，混用属于既定设计。

## 3. 相关文档

- 架构设计：[docs/background-effect.md](../../docs/background-effect.md)
