# 私有协议与窗口子系统实现踩坑

本文档整理 Woodland 在接入 Treeland 协议族、窗口管理核心子系统（如贴边吸附、VT 切换、Xwayland 托盘摆位等）过程中的设计折衷、边界约定与踩坑历史。

---

## 1. Treeland 协议族适配规矩

### 壁纸协议（`treeland-wallpaper-*`）
- **职责边界**：Woodland 仅负责壁纸路径校验、配置持久化（`$XDG_CONFIG_HOME/woodland/wallpaper.json`）与播放控制。视频解码与图像渲染交由客户端（如 `treeland-wallpaper-factory`）完成。
- **场景挂载位置**：壁纸 scene 树（`server->wallpaper_tree`）始终作为 scene 根节点的第一个子节点挂载，严格位于 layer-shell 的 background 图层之下。
- **多源可见性裁决**：当多个客户端同时提交壁纸 surface 时，所有 surface 在场景树中处于同级重叠。必须通过 `wallpaper_apply_visibility()` 依据当前工作区与锁屏状态挑选生效的唯一 surface。
- **跨层通知**：工作区切换或输出插拔时，需触发 `server->workspace_changed` 钩子通知壁纸子系统重新裁决可见性。

### 合成器动作（`treeland-compositor-action-v1`）
- **实现范围**：工作区切换动作（`workspace_1..12`、`prev`、`next`）真实联动 `woodland_workspace_switch()`。
- **置空动作（No-op）**：15..30 号动作（包括 show-desktop、多任务视图、缩放、FPS 显示、锁屏、关机菜单、用户切换等）暂未接入具体实现——仅记录 `action` 轨迹并更新 `wctl get_tree` 的状态，不发事件也不改状态。排查时切勿将“请求未报错”误当成“动作已生效”。
- **权限边界**：bind 校验仅针对沙箱客户端进行拦截（基于 `wp_security_context_manager_v1`），同 UID 用户直接放行。Woodland 不依赖 DDE 的 privilege 外部鉴权服务。

### 全局快捷键管理器（`treeland-shortcut-manager-v3`）
- **独占性（Acquire）**：协议规定全局仅允许一个客户端持有 manager。若非 DDE 环境由 Woodland 自带的 `woodland-shortcutd` 抢占；DDE 环境下必须留给 `dde-daemon`。
- **未生效动作与手势**：
  - 动作编号 0..20、22..23、32..39 真实联动窗口操作与工作区；21、24..31、40..45 号动作当前置空。
  - **手势绑定（Gesture）只登记不触发**：Woodland 当前无触摸手势识别状态机，注册手势时记录 `gesture_unwired` 轨迹，切勿当成已支持。
- **跨层接线**：键盘事件由 seat 模块转发至 `woodland_protocol_shortcuts_key()`；指针点击与滚轮事件通过 `server->shortcut_pointer_hook` 函数指针跨层分发，避免 shell/cursor 模块反向链接 protocol 库。

### 外观设置（`treeland-appearance-v1`）
- **会话级配置不落盘**：通过协议 `set_*` 修改的外观值直接写入内存中的 `server->config`，不落盘。当收到 SIGHUP 重新加载文件配置时，配置会重置为文件内容，并在末尾调用 `woodland_protocol_refresh_appearance()` 全量重新向客户端广播。
- **深色方案与用户显式覆盖**：若配置文件中显式配置了标题栏/边框色（`ssd.colors_specified` 为真），`set_color_scheme` 只调整光标、图标与强调色，不得覆盖用户显式指定的 SSD 配色。
- **字体描述串规范**：修改字体必须调用 `woodland_font_with(desc, family, size_pt, ...)` 基于现有 Pango 描述对象微调。严禁自行拼接字符串，否则会丢失粗体（Bold 等 Pango weight）属性。
- **光标主题热替换安全性**：更换主题必须使用 `woodland_cursor_apply_theme()`（先创建新 manager，成功后再销毁旧 manager），防止因目标主题不存在而导致 `server->cursor->xcursor_manager` 变为 NULL 崩溃。
- **圆角生效条件**：`set_corner_radius` 仅在 `-Deffects=enabled` 编译且运行期 `[effects] enabled=true` 时真实生效并广播目标值；无特效后端时仅在内部记录，对外广播半径 0。
- **光标尺寸降级**：`wlr_xcursor_manager` 仅支持单值尺寸。若客户端请求 `width != height` 的非方形光标，强制按 width 裁切并向所有客户端广播 `width,width`。

### 任务栏窗口管理（`treeland-foreign-toplevel-v3`）
- **Dock 预览与 Tooltip**：`get_dock_preview_context` 相关接口虽完成合法性校验，但其 `enter`/`leave` 事件永远不发（Woodland 不做重度动效）。
- **差量刷新而非延迟批处理**：与上游使用 idle 定时器批处理不同，Woodland 采用严格的属性差量比对：仅当窗口标识、状态或输出归属发生实质性变动时才发射对应事件并收尾 `done`。
- **状态更新由信号驱动**：`set_maximized` 等客户端请求对应的 `state` 事件由合成器内部的 `woodland_view_changed` 信号异步驱动，不是在请求处理函数中同步发送。
- **信号判空包装**：调用 `view_mapped`、`view_unmapped`、`view_changed` 时必须使用 `woodland_signal_emit()` 辅助函数，防止未初始化信号指针引发段错误。

---

## 2. 窗口管理子系统踩坑

### 贴边预览（Snap Preview）
- **触发判定仅看光标位置**：触发边缘半屏或四角预览的唯一判据是光标进入工作区边缘阈值（`woodland_snap_determine_edges()`）。**严禁重新引入“窗口外框碰边”作为触发条件**，避免用户仅拖动大窗口但并无半屏意图时误触吸附。
- **多显示器跨屏接缝防抖**：两个相邻屏幕间的接缝在数学上等同于外边沿。通过 `woodland_snap_edge_is_layout_edge()` 识别接缝，并强制引入 200ms 防抖定时器；定时器触发时需二次确认指针仍处于抓取该窗口状态。
- **预览框挂载于场景树**：预览对象是普通场景矩形节点（effects 下为剪裁空心矩形，无 effects 时为四条线拼合），挂载在窗口同一父节点下并位于被拖拽窗口下方，严禁使用独立 surface 或每帧重复排布 z 序。
- **多屏不同缩放软边限制**：预览框几何为整数布局坐标，在混合缩放（scale > 1）显示器边界上可能会被拉伸产生软边。当前架构暂未提供按屏分发的设备像素几何绘制。

### VT 控制台切换（VT Switch）
- **按键优先级最高**：`Ctrl+Alt+F1..F12` 判定排在锁屏与快捷键抑制（shortcuts-inhibit）之前，作为系统级终极逃生通道。
- **孤儿 Release 事件吞噬**：VT 按键按下（press）被合成器消费后，对应的键位松开（release）事件必须被丢弃（记录在 `server->vt_swallowed_keys` 位图中），防止控制台切回后客户端收到孤立的 release。
- **会话非激活状态（Inactive）**：会话转入后台时，`handle_frame` 渲染回调必须直接跳过；输入设备暂停与恢复由 wlroots session 内部处理，不要额外监听 `session_active`。

### Xwayland 托盘图标控制（`treeland-xwindow-control-v1`）
- **Anchor 坐标反查**：Dock 传入的 anchor 是 Wayland surface。必须通过场景图遍历对应 buffer 节点反查出其在布局中的绝对坐标，严禁针对窗口类型写死分支。
- **几何状态双份同步**：X11 窗口位置既存在于场景节点中，又由 X server 内部几何维护。挪动时必须通过 `woodland_xwayland_move_by_wid()` 同时更新 scene 节点与发送 X11 `xcb_configure_window` 并调用 `xcb_flush`。
- **int16_t 坐标范围校验**：X11 的 configure 接口坐标仅支持 `int16_t`。来自 Wayland 的 `wl_fixed_t` 偏移若超出 `[-32768, 32767]` 范围，必须直接通过 `callback.done(1)` 拒绝并终止流程，禁止截断后应用，防止 X server 反弹 ConfigureNotify 破坏场景状态。
- **无 Xwayland 构建时不广播 Global**：若未开启 Xwayland 支持（`-Dxwayland=disabled`），绝对不能对外广播 `treeland_xwindow_control_v1` global。广播一个始终报错的接口会导致 Dock 误判并阻止其退回到 X11 原生托盘机制。
