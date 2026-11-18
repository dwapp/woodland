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
| [window-switcher.md](window-switcher.md) | Alt+Tab 窗口切换器：候选快照与 z 序、begin/step/finish 状态机、切换期间的键盘归属、Cairo 面板渲染与验证方式 |

## 踩坑与陷阱专栏（Pitfalls）

收录合成器开发、协议接入、构建测试与调试过程中的踩坑总结与边界规矩：

| 文档 | 领域与内容 |
| --- | --- |
| [pitfalls/testing-and-ci.md](pitfalls/testing-and-ci.md) | 构建、CI 容器与测试陷阱（Arch 镜像依赖、子项目协议头包装、CI effects 限制、探针两次 roundtrip/prepare_read、测试现场收集） |
| [pitfalls/debugging.md](pitfalls/debugging.md) | 调试方法与工具陷阱（严禁 gdb run、ASan 与高熵 ASLR、LSan/Mesa 缓存退出码 134、wctl 调试、--trace 环形缓冲、--self-check 不变量） |
| [pitfalls/wlroots-semantics.md](pitfalls/wlroots-semantics.md) | wlroots 语义与事件时序陷阱（idle inhibitor 销毁顺序、监听器注册顺序、output-management 测试无副作用与 succeeded 广播时序、scale 统一刷新） |
| [pitfalls/protocol-implementations.md](pitfalls/protocol-implementations.md) | 私有协议与窗口系统踩坑（壁纸可见性、compositor-action 与 shortcut 独占、外观不落盘/字体/光标、foreign-toplevel 差量刷新、VT 切换、贴边预览、X11 托盘摆位） |
