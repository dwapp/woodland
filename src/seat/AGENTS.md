# Seat 模块智能体指南

## 1. 模块职责与核心文件

`src/seat` 负责输入设备、Seat 交互状态与控制台会话（VT）管理：
- `src/seat/seat.c`：`wlr_seat` 初始化与管理、键盘/指针/触控设备热插拔、按键分发、XKB 键位转换与 VT 切换逻辑。

## 2. 关键硬约束

1. **VT 切换最高优先级**：
   - `Ctrl+Alt+F1..F12` 经 XKB 翻成 `XF86Switch_VT_1..12`，其判定次序排在锁屏和快捷键抑制（shortcuts-inhibit）之前，作为系统逃生通道。
2. **孤儿 Release 事件吞噬**：
   - VT 按键的 press 被合成器消费后，其对应的 release 必须丢弃并记录在 `server->vt_swallowed_keys` 位图中，防止切换回桌面时客户端收到孤立的 release。
3. **Session Inactive 守卫**：
   - 会话非激活时，渲染循环必须跳过 `handle_frame`；输入设备休眠与恢复由 wlroots session 内部自动处理，严禁手动监听 `session_active` 信号。
4. **快捷键转发链**：
   - 键盘事件优先经过合成器自身处理与全局快捷键分发（`woodland_protocol_shortcuts_key`），未消费按键才推送到键盘焦点的客户端。

## 3. 相关文档

- 陷阱参考：[docs/pitfalls/protocol-implementations.md](../../docs/pitfalls/protocol-implementations.md)（VT 切换语义与孤儿按键吞噬）
