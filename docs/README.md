# Woodland 设计与规格文档

本文档目录收录 Woodland 合成器各核心模块与协议的设计规格说明，为 AI 辅助开发和代码维护提供领域上下文与设计约束。

| 文档 | 领域与内容 |
| --- | --- |
| [architecture.md](architecture.md) | 全局架构规格：三大核心目标（DDE运行/朴素特效、Noctalia/Quickshell/LXQt、deepin/arch）、单体结构、技术选型与协议分级 |
| [clipboard-dnd.md](clipboard-dnd.md) | 剪贴板与拖拽（selection 与 wl_data_device drag-and-drop）机制：时序图、数据流、图标树位置与指针捕获细节 |
| [layer-shell.md](layer-shell.md) | layer-shell 子系统设计：四层图层树（background/bottom/top/overlay）、exclusive zone 排版计算、键盘交互与弹层 |
| [xdg-shell-workspaces.md](xdg-shell-workspaces.md) | 窗口与工作区管理：xdg-shell 状态机、12 工作区 scene 节点 reparent、多输出几何与窗口约束 |
| [background-effect.md](background-effect.md) | 扩展背景特效（ext-background-effect-v1）：scenefx 模糊/圆角、局部区域与全屏模式、预模糊缓存优化 |
| [treeland-decoration.md](treeland-decoration.md) | 窗口装饰与 SSD：zxdg 装饰模式协商、Cairo/Pango 标题栏与按钮渲染、scenefx 圆角裁切 |
| [wallpaper-protocols.md](wallpaper-protocols.md) | 壁纸协议族：treeland-wallpaper-manager 与 wallpaper-shell、配置持久化（wallpaper.json）、多源可见性裁决 |
| [drm-lease.md](drm-lease.md) | DRM 租借协议（wp_drm_lease_device_v1）：VR/XR 头显独占访问、non-desktop 设备识别与隔离策略 |
| [shortcut-client.md](shortcut-client.md) | 全局快捷键客户端（woodland-shortcutd）：非 DDE 会话的自动启动判定、TOML 配置、内置默认键位、命令启动与生命周期 |
