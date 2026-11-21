# Output 模块智能体指南

## 1. 模块职责与核心文件

`src/output` 负责显示输出（Display Outputs）的生命周期、几何布局与渲染帧管理：
- `src/output/output.c`：输出热插拔（`new_output`、`destroy`）、`wlr_output_layout` 管理、`handle_commit` 帧提交回调、输出模式（mode）与缩放（scale）应用。
- `src/output/output_config.c`：显示配置持久化（`$XDG_CONFIG_HOME/woodland/outputs.json`），记录分辨率、刷新率、缩放、旋转与布局坐标，支持多屏配置合并与热插拔/启动恢复。
- `woodland_output_refresh_all()`：缩放变化时的统一全局刷新中枢。

## 2. 关键硬约束

1. **缩放刷新唯一入口**：
   - 所有的缩放（scale）变更路径（后端 request_state、output-management apply、SIGHUP 配置重载）最终均在 `handle_commit()` 中由 `output->last_scale` 闸门拦截，并统一汇聚调用 `woodland_output_refresh_all()`。
   - **严禁省略 `last_scale` 闸门**：commit 每帧都会触发，若无闸门保护将导致每帧重建所有窗口的 SSD 缓存与图标。
2. **X11 覆盖式窗口（unmanaged）补刷**：
   - unmanaged 窗口不进入 `server->views` 链表，在 `woodland_output_refresh_all()` 中必须显式调用 `server->xwayland_refresh_hook` 完成滤波模式刷新。
3. **输出销毁信号**：
   - 输出销毁时，由于被禁用的输出早已不在 layout 中，必须通过 `server->output_removed` 信号通知 `output-management` 协议模块推送配置更新。

## 3. 相关文档

- 陷阱参考：[docs/pitfalls/wlroots-semantics.md](../../docs/pitfalls/wlroots-semantics.md)（缩放变更刷新入口收敛、输出热插拔广播）
