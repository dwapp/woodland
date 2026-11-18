# Xwayland 模块智能体指南

## 1. 模块职责与核心文件

`src/xwayland` 负责 X11 应用程序兼容支持与 Xwayland 进程生命周期：
- `src/xwayland/xwayland.c`：`wlr_xwayland` 实例管理、X11 toplevel 视图生命周期封装、override-redirect（unmanaged）悬浮窗口追踪、托盘窗口物理摆位（`woodland_xwayland_move_by_wid`）。

## 2. 关键硬约束

1. **几何状态双份同步**：
   - 挪动 X11 窗口（如 DDE Dock 托盘 XEmbed 图标）时，必须同时更新 Wayland 场景图节点位置，并向 X server 发送 `xcb_configure_window` 且显式调用 `xcb_flush`。仅挪场景节点会被 X11 的下一次 configure 弹回。
2. **坐标溢出保护**：
   - X11 configure 协议接口坐标为 `int16_t`。输入超出 `[-32768, 32767]` 时必须拒绝并报错，严禁静默截断后写入。
3. **跨层解耦钩子**：
   - `protocol_lib` 不反向链接 `xwayland_lib`。协议层通过 `server->xwindow_move_hook` 与 `server->xwayland_refresh_hook` 回调与 Xwayland 交互；钩子为 NULL 时协议层安全返回失败，绝不能引发符号缺失。
4. **特性开关契约**：
   - 编译期禁用 Xwayland 时，相关特权 global（`treeland_xwindow_control_v1`）必须停止广播，防止客户端误判并不退回到 X11 原生模式。

## 3. 相关文档

- 陷阱参考：[docs/pitfalls/protocol-implementations.md](../../docs/pitfalls/protocol-implementations.md)（托盘图标摆位双份真相与 int16 边界）、[docs/pitfalls/testing-and-ci.md](../../docs/pitfalls/testing-and-ci.md)（宿主 X 授权冲突）
