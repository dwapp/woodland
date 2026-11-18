# Shortcutd 模块智能体指南

## 1. 模块职责与核心文件

`src/shortcutd` 包含独立的客户端侧程序 `woodland-shortcutd`，负责在缺少桌面环境守护进程（如 DDE 会话之外的纯轻量环境）时，作为客户端注册全局快捷键：
- `src/shortcutd/main.c`：Wayland 客户端连接建立、绑定 `treeland_shortcut_manager_v3`、独占 acquire、监听按键激活事件。
- `src/shortcutd/config.c`：TOML 配置文件读取与内置默认快捷键映射。
- `src/shortcutd/spawn.c`：快捷键触发时的进程拉起逻辑。

## 2. 关键硬约束

1. **绝对纯客户端定位**：
   - 本模块是独立的二进制程序，**严禁链接合成器的任何内部代码与头文件**，仅依赖公开的 `wayland-client` 库与生成的协议客户端头。
2. **DDE 环境让渡原则**：
   - 协议只允许一个客户端持有快捷键管理器。在 DDE 会话中必须由 `dde-daemon` 抢占；`woodland-shortcutd` 仅在非 DDE 环境由合成器自动拉起。
3. **测试环境禁用隔离**：
   - 在冒烟测试环境下，必须通过 `WOODLAND_SHORTCUTD=off` 禁用该进程的自动启动，避免独占管理器抢占污染测试用例断言。

## 3. 相关文档

- 规格说明：[docs/shortcut-client.md](../../docs/shortcut-client.md)
- 陷阱参考：[docs/pitfalls/testing-and-ci.md](../../docs/pitfalls/testing-and-ci.md)（冒烟测试默认禁用 shortcutd）
