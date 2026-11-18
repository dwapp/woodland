# Server 模块智能体指南

## 1. 模块职责与核心文件

`src/server` 负责 Woodland 合成器的生命周期管理、基础渲染基础设施搭建与主事件循环：
- `src/server/server.c`：`struct woodland_server` 的创建（`woodland_server_create`）、初始化、启动（`woodland_server_start`）与销毁（`woodland_server_finish`）。
- 初始化 `wl_display`、`wlr_backend`、`wlr_renderer`、`wlr_allocator`、`wlr_scene` 与输入/输出子系统。
- 维护全局场景图根节点（`server->scene`）与分层子树（background/bottom/normal/top/overlay/wallpaper 等）。

## 2. 关键硬约束

1. **渲染器选择**：
   - 启用 effects 时（`-Deffects=enabled`），`server->renderer` 必须由 `fx_renderer_create()` 创建；scenefx 内部会将 render pass 强转为 `fx_render_pass`。
   - 未启用 effects 时，走 `wlr_renderer_autocreate(server->backend)`。
2. **初始化时序敏感**：
   - `woodland_cursor_init_events()` 必须严格在 `woodland_protocol_init()` 之前调用，确保光标焦点更新优先于 relative-pointer 事件分发。
   - `woodland_output_init()` 必须先于需要绑定输出的子系统（如 layer-shell、wallpaper）初始化。
3. **资源释放纪律**：
   - `woodland_server_finish()` 必须按照初始化的严格逆序执行，每步遵循“先判空、再销毁、最后置 NULL”的幂等原则。

## 3. 相关文档

- 架构总览：[docs/architecture.md](../../docs/architecture.md)
- 陷阱参考：[docs/pitfalls/wlroots-semantics.md](../../docs/pitfalls/wlroots-semantics.md)（监听器注册顺序、资源释放生命周期）
