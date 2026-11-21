# Protocol 模块智能体指南

## 1. 模块职责与核心文件

`src/protocol` 负责实现通用 Wayland 扩展协议与 Treeland 私有协议族的服务端接线：
- `src/protocol/protocol.c`：全局协议总线初始化（`woodland_protocol_init`）与逆序销毁。
- Treeland 协议实现：
  - `appearance.c`：系统外观、字体、光标与调色板广播。
  - `compositor_action.c`：工作区与合成器动作。
  - `shortcut.c`：全局快捷键独占注册与捕获。
  - `wallpaper.c`、`wallpaper_config.c`：壁纸协议与持久化。
  - `foreign_toplevel.c`：任务栏窗口列表与状态同步。
  - `xwindow_control.c`：Xwayland 托盘图标摆位。
  - `remote_subsurface.c`：跨进程/同进程基于 token 的子表面关联与 Z 序控制。
- 通用扩展协议实现：
  - `decoration.c`：CSD/SSD 协商（zxdg 与 treeland 装饰）。
  - `idle.c`：空闲抑制器管理。
  - `output.c`：显示配置动态调节（wlr-output-management）。
  - `drm_lease.c`：VR/XR 设备 DRM 租借。
  - `security_context.c`：沙箱权限与特权协议隔离。

## 2. 关键硬约束

1. **对象生命周期解耦**：
   - 协议监听器不要长期持有并假设 `struct woodland_view *` 有效。在 view 销毁信号触发时，仅清空自身指针并注销监听器，**绝对禁止解引用已销毁的 view**。
2. **协议 XML 只读纪律**：
   - `protocols/treeland/*.xml` 为外部协议副本，一律不得在本项目中修改内容，所有改动均在 `src/protocol/*.c` 实现层完成。
3. **特权协议登记**：
   - 新增向客户端暴露的特权 global（如控制窗口位置、全局按键捕获）必须在 `security_context.c` 的 `privileged_protocols` 列表中登记，阻断 Flatpak 等沙箱应用的非法访问。
4. **输出管理协议时序**：
   - `test` 请求必须零副作用；`apply` 流程中必须优先发送 `succeeded` 事件，其后再补发 `configuration` 与 `done`。

## 3. 相关文档

- 规格说明：[docs/wallpaper-protocols.md](../../docs/wallpaper-protocols.md)、[docs/shortcut-client.md](../../docs/shortcut-client.md)、[docs/drm-lease.md](../../docs/drm-lease.md)
- 陷阱参考：[docs/pitfalls/wlroots-semantics.md](../../docs/pitfalls/wlroots-semantics.md)、[docs/pitfalls/protocol-implementations.md](../../docs/pitfalls/protocol-implementations.md)
