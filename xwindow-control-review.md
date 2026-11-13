# `treeland_xwindow_control_v1`：对照上游 treeland 的实现评审

评审对象：分支 `feature/treeland-xwindow-control`（worktree `.worktrees/xwindow`，
`dc8445b` + `d9cbbad`）
对照基线：`/home/ut004971@uos/work/treeland` master `76832ce9f`，协议 XML 取
`/home/ut004971@uos/work/treeland-protocols/dde/treeland-xwindow-control-unstable-v1.xml`，
客户端行为参考 `/home/ut004971@uos/work/dde-shell/panels/dock/waylanddockhelper.cpp`。

---

## 1. 结论

协议语义对齐度是高的：请求/回调语义、`destroy` 不改摆位、失败回 `done(1)`、
X11 位置与场景节点两份真相同步，都落到了正确的位置。工程性上 woodland 明显
强于上游——有 trace / `wctl` 诊断、binding 生命周期回收、以及**真正跑通了
成功路径**的冒烟（上游 README 第 34–40 行明确写着成功路径因 CI 无 GPU 未覆盖）。

但有 **2 项必须修**：一是 `int16_t` 强转溢出会让「成功」请求实际把 X11 窗口
配到错误坐标；二是无 XWayland 构建仍广播 global，会让真实 Dock 认为协议可用
却永远失败、不再回退到 X11 原生路径。另有 3 项建议和 3 项取舍说明。

---

## 2. 逐项对比

| 维度 | 上游 treeland | woodland 当前实现 | 评价 |
| --- | --- | --- | --- |
| global 创建 | `waylandserver.cpp:1241-1243`，`#ifndef DISABLE_XWAYLAND` 才 `attach<XWindowControlInterfaceV1>()`；noxwl 下 global 不存在 | 恒创建 global，无 hook 时请求回 `no_xwayland` | **差异，见 P1-2** |
| bind 权限 | 协议模块里无任何校验（`xwindowcontrolinterfacev1.cpp` 通篇没有 uid / security context 判断） | 同 uid + 无 `security_context`，并登记为 `privileged_protocols` | woodland 更严，符合 XML 第 20–21 / 32–33 行「should reject non-privileged」 |
| anchor 归属 | `xwindowcontrolinterfacev1.cpp:52` 校验 `wl_resource_get_client(anchor) == resource->client()` | 未校验 | 纵深防御，见 P2-3 |
| anchor 定位 | `m_rootSurfaceContainer->getSurface(anchor)` → `SurfaceWrapper::position()`（`waylandserver.cpp:1013,1036`） | `wlr_scene_node_for_each_buffer()` + `wlr_scene_surface_try_from_buffer()` 取布局坐标（`xwindow_control.c:197-235`） | 思路等价、避免按 surface 类型写 switch；细节见 P3-6 |
| target 查找 | 遍历 `rootSurfaceContainer()->surfaces()`，要求 `Type::XWayland` **且 `wrapper->session() == ach->session()`**（`waylandserver.cpp:1020-1030`） | 遍历 `server->views`（managed）+ `server->xwayland_unmanaged`（override-redirect），只比 `window_id`，要求 mapped | 覆盖 Dock XEmbed 的 unmanaged 场景；缺 session 等价隔离，见 P2-4 |
| 坐标精度 | `wl_fixed_to_double(dx)` → `QRectF::translate()` 浮点（`waylandserver.cpp:1037`） | `woodland_xwindow_control_round_fixed()` 先取整（`xwindow_control.c:122-145`） | ≤0.5px 偏差，见 P2-5 |
| X11 几何同步 | `setXwaylandPositionFromSurface(false)` → `setPosition()`（内部发 configure）→ 恢复 `true`（`waylandserver.cpp:1039-1045`） | 同时 `wlr_scene_node_set_position()` + `wlr_xwayland_surface_configure()` + `xcb_flush`（`xwayland.c:1016-1022,1039-1042`） | 目的相同；woodland 的 WM 位置由 `view->placed` / `request_configure` 管，不需要上游那个「隐式跟随」开关。**但 configure 的窄类型没处理，见 P1-1** |
| callback | 先算 ok，再 `wl_resource_create`；OOM 时 `post_no_memory` 且不发 done（`:54-61`） | 先建 callback 再算；OOM 同样 `post_no_memory`，并额外记 `callback_failed`（`xwindow_control.c:292-303`） | 语义一致，woodland 多一层诊断；创建顺序无功能差别 |
| `destroy` | `wl_resource_destroy(resource->handle)`（`:39-42`） | 同（`xwindow_control.c:271-276`） | 一致，都不回退已应用摆位 |
| 生命周期 | QtWaylandServer 基类管 global remove | `finish()` 先 `wl_global_destroy`，再逐个 `wl_resource_destroy(binding->resource)` 后 `free`（`xwindow_control.c:453-475`） | 上游靠框架，woodland 手工处理 UAF，正确且必要 |
| 可观测性 | 无 | `xwindow` trace + `wctl get_tree.xwindow_control`（计数、last_*、anchor/target/kind/view_id） | woodland 增强 |
| 测试 | `tests/protocols/.../` 只覆盖 bind / 裸 anchor 失败 / destroy；README 明说成功路径未覆盖 | 单测 + 探针 + smoke：bind/destroy、两种失败、**真 X11 unmanaged + managed 成功摆位**，并用 `dump_scene` / `views[].content` 独立核对坐标 | woodland 明显更完整 |

---

## 3. 必须修

### P1-1 `(int16_t)x/y` 溢出：回 `done(0)` 却把窗口配到错误坐标

`wlr_xwayland_surface_configure()` 的签名是 `int16_t x, int16_t y`
（`subprojects/wlroots/include/wlr/xwayland/xwayland.h:301-302`），而
`woodland_xwindow_control_target()` 把目标坐标饱和到 `INT32_*`
（`xwindow_control.c:133-145`）。`xwayland.c:1017` 与 `:1040` 直接
`(int16_t)x` 强转：

```
anchor=(0,0), dx=wl_fixed_from_int(40000)
  → target_x = 40000
  → wlr_scene_node_set_position(node, 40000, ...)      // 场景里在 +40000
  → wlr_xwayland_surface_configure(xs, -25536, ...)   // X11 侧变成 -25536
  → 回 wl_callback.done(0)                            // 报告"成功"
```

`dx` 是协议允许范围内的合法值（`wl_fixed_t` 值域 ±8388607.996），客户端
无需任何越权就能触发。后果不只是坐标错：X server 回 `ConfigureNotify` 后，
`unmanaged_request_configure()`（`xwayland.c:226-237`）会用 `event->x/y`
把 scene 节点拉回 -25536，两份真相同时错；`xwl_request_configure()`
（`:510-515`）对 managed 视图同样会把节点设回错误值。多屏大布局或 Dock
传入较大偏移时也可能自然命中。

建议：
- 在调用 configure 前把 `x/y` clamp 到 `INT16_MIN..INT16_MAX`；超出时
  回 `done(1)` 并记新的 `out_of_range` 结果（或复用 `no_window` 但 detail
  写清楚），**不要**只 clamp configure 而不动 scene 节点；
- 或反过来：把 clamp 后的值同时用于 `wlr_scene_node_set_position()`，保证
  两份真相一致，并在 detail 里记「clamped」；
- 单测补一条 `dx = wl_fixed_from_int(40000)`，冒烟补一条大偏移断言
  `last_outcome != "moved"` 且 scene 节点未被挪走。

顺带：`xwayland.c:1018-1019` 用 `content.width/height` 当 configure 的宽高。
多数情况下等于 `xsurface->width/height`，但带 SSD / transform 时不一定；
协议只要求改位置，传 `xsurface->width/height` 更贴合「不产生 resize 副作用」。

### P1-2 无 XWayland 构建仍广播 global，Dock 不会回退

上游 `waylandserver.cpp:1241-1243` 用 `#ifndef DISABLE_XWAYLAND` 包住
`attach<XWindowControlInterfaceV1>()`，测试 README 第 5 行也写明
「关闭 Xwayland 时，生产模块与对应 global 一并移除」。woodland 在
`-Dxwayland=disabled` 下仍创建 global，请求统一回 `no_xwayland`。

这不是纯粹的语义洁癖。真实 Dock 的判定是
`waylanddockhelper.cpp:176`：`if (!m_xwindowControl || !m_xwindowControl->isActive() …) return false`，
`isActive()` 取决于 global 是否存在。global 存在 → Dock 认为 Wayland
摆位通道可用 → 每次 `moveXEmbedWindow()` 都拿到 `done(1)`，
`xembedWindowMoveResult(wid, false)`（`:194-199`），**不会**再走 X11 原生
reparent 路径；上游 noxwl 下 `isActive()==false`，Dock 直接判不可用并回退。
结果是 noxwl 构建里托盘图标可能永远摆不对位。

建议：noxwl 构建不创建该 global（与上游一致），`get_tree.xwindow_control.available`
自然变 false；保留 `smoke-xwindow-control` 的 noxwl 分支用于验证「global 不存在
时客户端行为正确」，而不是验证「global 存在但请求失败」。若坚持广播以便诊断，
至少要在 `AGENTS.md` / `include/protocol.h` 档位表里写明这会破坏 Dock 回退。

---

## 4. 建议

### P2-3 补 anchor 归属校验（成本低，与上游对齐）

`xwindowcontrolinterfacev1.cpp:52` 显式要求 `wl_resource_get_client(anchor)
== resource->client()`。woodland 的 `anchor_position()`
（`xwindow_control.c:217-235`）没有这一步。

需要公允说明：libwayland 的对象 ID 是 per-client 的，客户端实际上很难把
别家连接的 `wl_surface` 当参数传进来（派发时查的是本 client 的 object
map，类型不符直接 protocol error）。所以这**不是**可利用的越权漏洞，而是
与上游对齐的纵深防御；在 `anchor_position()` 开头加一次
`wl_resource_get_client(anchor_resource) == wl_resource_get_client(resource)`
即可，失败时归入 `invalid_anchor` 并写进 detail。

### P2-4 缺少与上游 `session()` 等价的隔离

`waylandserver.cpp:1022` 要求 target 与 anchor 同一个 session。woodland 是
单 session 设计，没有这个字段，当前任何「同 uid、无 security context」的
进程都能 bind 后按 X11 window id 挪动**任意** X11 窗口（协议 XML 只说
"should reject bind attempts from non-privileged clients"，而 woodland 没有
DDE 的 app_id / 签名授权服务）。

这是当前权限模型的真实边界，不是实现 bug，但值得显式处理：
- 在 `include/protocol.h` 档位说明里写清「同 uid ≠ 特权」，避免被误读为
  已等价实现 DDE 授权；
- 可选加固：配置一个允许使用该协议的 client 白名单（按 pid/可执行路径），
  或要求 target X11 窗口与 anchor 属于同一 X11 客户端（用 `xcb` 查 pid /
  `WM_CLIENT_MACHINE`），把影响面收敛到 Dock 自己创建的托盘窗口。

### P2-5 `wl_fixed_t` 提前取整

Dock 传的是 `wl_fixed_from_double(dx)`（`waylanddockhelper.cpp:250`），
QML 布局坐标常带小数；上游用 `wl_fixed_to_double()` 进 `QRectF` 浮点运算
（`waylandserver.cpp:1037`）。woodland 在
`woodland_xwindow_control_round_fixed()` 里先「远离 0」取整，最终最多
0.5px 偏差，且 `wctl` 的 `last_dx/last_dy` 也丢了小数部分。

X11 位置最终总要落到整数，所以这不是错误，但建议：要么把 fixed 值保留到
最后一步再取整，要么在 `wctl` 里多给一对原始 fixed 值（或 `double`），
排查 Dock 位置偏移时不会把「取整」误判成「算错」。

---

## 5. 取舍与观察（可不改，但应记录）

- **P3-6 anchor 坐标来源**：`wlr_scene_node_for_each_buffer()` 给的是
  **buffer 节点**的布局坐标，且 `lookup->found` 后取第一个匹配。上游取的是
  `SurfaceWrapper::position()`（窗口左上角）。对 Dock 用的 layer surface
  通常一致；但若 anchor 是带 SSD 的 toplevel、带 subsurface 或 viewport
  crop 的 surface，可能差一个标题栏高度或取到子节点。建议冒烟补一个
  「anchor 是带装饰的 xdg toplevel」用例来钉住语义。
- **P3-7 bind 拒绝方式偏激进**：`xwindow_bind()`（`xwindow_control.c:387-395`）
  用 `wl_client_post_implementation_error()`，会直接断掉整条连接。上游根本
  不拒绝，协议也只说 "should reject"。对误 bind 的普通应用来说，静默创建后
  立即销毁 resource（或按惯例发 protocol error）比杀连接更温和。woodland
  已有 security-context global 过滤挡第一道，这里的失败面很小，属可接受的
  取舍，但建议在注释里写明「拒绝 = 断连」。
- **P3-8 上游还有一条旧入口**：`treeland_dde_shell_manager_v1` 也实现了
  同名请求（`src/modules/dde-shell/ddeshellmanagerinterfacev1.cpp:67-91`），
  且没有 anchor client 校验。当前 Dock 用的是 `xwindow_control`
  （`waylanddockhelper.h:87`、`waylanddockhelper.cpp:233-250`），所以本次
  不实现它不影响 Dock；但若其它 DDE 组件仍走旧路径，需要另开一个协议族接线。
- **X11 feedback 覆盖**：上游 `waylandserver.cpp:1039-1045` 特意临时关闭
  `setXwaylandPositionFromSurface` 以防火 feedback 覆盖。woodland 的
  `xwl_set_geometry()`（`xwayland.c:597-604`）只更新 SSD、不回写位置，
  `xwl_request_configure()` 才按客户端请求同步，架构上不存在那个开关，
  因此不需要照抄三步——**但 P1-1 的溢出恰好会把这条路径变成覆盖源**，
  两个问题要一起看。
- **callback 创建顺序**：上游先算结果再建 callback，woodland 先建再算，
  两者都满足「每条请求回一次 done」，无功能差异。

---

## 6. 建议补的回归用例

1. 大偏移 / 大 anchor 坐标（`dx = 40000`）→ `done(1)`、scene 节点不动、
   `dump_scene` 里 X11 窗口位置不变（P1-1）。
2. noxwl 构建里客户端 `wl_registry` 收不到该 global（P1-2）。
3. anchor 传其他 client 的 surface / 非 `wl_surface` 对象 → `invalid_anchor`
   且客户端不被误杀（P2-3 / P3-7）。
4. `dx/dy` 带小数（如 `wl_fixed_from_double(12.25)`）→ 核对 `wctl` 记录与
   实际落点，钉住取整策略（P2-5）。
5. anchor 为带 SSD 的 xdg toplevel → 与 `SurfaceWrapper::position()` 语义
   对照（P3-6）。

---

## 7. 相对上游做得更好的地方（保持，不要「对齐」掉）

- 成功路径真跑通：unmanaged override-redirect 与 managed X11 toplevel 两条
  分支各有一条 smoke，并用 `dump_scene` 与 `views[].content` 独立核对坐标；
  上游 README 第 34–40 行承认成功路径在 CI 里无法验证。
- 失败可解释：`xwindow` trace 标签 + `wctl get_tree.xwindow_control`
  把 `no_xwayland` / `invalid_anchor` / `no_window` 分开记账，不会把「没报错」
  当成「已生效」。
- 生命周期：`wl_global_destroy()` 不回收已 bind resource 这个坑被显式处理，
  `finish()` 逆序清理；上游靠 Qt 框架，woodland 手工做对了。
- `protocol_lib` 不反向链接 `xwayland_lib`：用 `server->xwindow_move_hook`
  跨层，纯逻辑单测能独立链接。
- 三套构建矩阵（build / build-noxwl / build-asan）全绿、0 跳过。

---

## 8. 优化落地（2026-11-13）

§3 / §4 是评审当时的快照；以下问题已按优先级处理，改动在分支
`feature/treeland-xwindow-control`（worktree `.worktrees/xwindow`）。

| 项 | 处置 |
| --- | --- |
| P1-1 int16 溢出 | **已修**：新增 `out_of_range` 结果与 `woodland_xwindow_target_in_x11_range()`，目标越界回 `done(1)` 且**不调用摆位钩子**（两份真相都不动）；`xwayland.c` 再加一层范围防御，configure 宽高改用 `xsurface->width/height` |
| P1-2 noxwl 广播 global | **已修**：`#if WOODLAND_HAVE_XWAYLAND` 条件化 global 与整条 bind/请求路径，`wctl` 报 `available:false`；`test_security_context.c` 的 registry 断言同步条件化 |
| P2-3 anchor 归属 | **已修**：`anchor_position()` 校验同 client 与 `wl_surface_interface`，失败原因写进 trace 与日志 |
| P2-4 权限边界 | **文档化**：`include/protocol.h`、协议族文件头、`AGENTS.md` 写明"同 uid ≠ DDE 特权"；未引入白名单机制（属新特性，另行评估） |
| P2-5 fixed 精度 | **已修**：`wctl` 增 `last_dx_fixed/last_dy_fixed` 原始 24.8 值；探针改用 `wl_fixed_from_double`（与 Dock 一致），smoke 用 12.25 / -3.75 钉住取整策略 |
| P3-6 / P3-7 / P3-8 | **保留取舍**：P3-7 在 `xwindow_bind()` 注释里写明"拒绝 = 断连"；带 SSD 的 anchor 用例与旧 dde-shell 入口未纳入本次改动 |

新增回归：单测覆盖 INT16 端点与 `dx=40000`；smoke 覆盖 noxwl global 缺席、
小数偏移、越界拒绝后 `views[].content` 与 `dump_scene` 均无位移、trace 含
`out_of_range`。验证：build 49/49、build-noxwl 48/48、build-asan（关 ASLR）
49/49，全部 0 失败 0 跳过。

一个实现细节值得留档：`wl_resource_instance_of()` 的第三参数是**要相等**的
implementation，传 `NULL` 会把所有合法 `wl_surface` 判成不匹配（noxwl 之外
第一次编译就踩到，冒烟表现为 mapped anchor 被误判 `invalid_anchor`）。只校验
接口类型要用 `wl_resource_get_interface(res) == &wl_surface_interface`。
