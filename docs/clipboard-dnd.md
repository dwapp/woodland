# 剪贴板与拖拽（selection / DnD）调研与实现设计

> 日期：2026-11-08
> 状态：**已实现**（拖拽模块 + 光标层守卫 + 可观测性 + 测试；落地记录见 §11）
> 对照实现：labwc（`~/labwc`，GPL-2.0-only）、gxde-wlcom（treeland 血统，`~/gxde-wlcom`）、wlroots 0.20.2（`subprojects/wlroots`）、scenefx 0.5（`subprojects/scenefx`）

## 1. 结论摘要

**剪贴板（selection）woodland 已经齐了。** `wl_data_device_manager`、`zwlr_data_control_manager_v1`、`ext_data_control_manager_v1`、`primary_selection_v1` 四个全局都已创建，`request_set_selection` / `request_set_primary_selection` 都已转发，serial 校验由 wlroots 自己做，X11 的 CLIPBOARD/PRIMARY 由 xwm 自动桥接（因为已经 `wlr_xwayland_set_seat`）。和 labwc 比只差两件"体验层"的东西，都不是协议缺口：

- 没有剪贴板持久化（labwc 也没有，靠 `wl-clip-persist` / `cliphist`；它们靠我们已经提供的 data-control 工作）；
- 没有 `org.kde.KWin.Clipboard` 这组 D-Bus 方法（treeland 系的 dde-clipboard 可能要看，见 §4.5，待验证）。

**拖拽（DnD）woodland 是零。** 全仓没有任何 `request_start_drag` / `start_drag` 监听，后果是**任何 Wayland 内部拖放（文件管理器拖文件到终端、GTK/Qt 拖放、浏览器拖图上传、拖拽选中文本）都静默失败**——客户端发出 start_drag，合成器不 start，拖拽源被挂在那里直到超时或下一次点击。这是唯一的功能性缺口。

工作量估算：新增约 250 行（一个 `src/dnd/` 模块 + 一个头），加 3 处既有光标路径的守卫改造。三个必须先知道的坑：

1. **拖拽期间不能用裸 `wlr_seat_pointer_clear_focus()`**（woodland 现在在 SSD 分支里用了，`src/shell/cursor.c:179`、`:199`）。它绕过 pointer grab，不通知 `wlr_drag`，结果是"光标明明在标题栏/桌面上松手，文件却掉进了上一个窗口"。
2. **图标树必须挂在 scene 根、并在 drag 开始时置顶**，否则会被 layer-shell 的 panel 盖住（labwc 0.7.3 修的就是这个，NEWS #1936）。
3. **`wlr_scene_drag_icon_create` 可以用**（这条初稿判断反了，见 §9.1）。scenefx 0.5 的头里声明了它、`.so` 里没有实现，链接落到 `libwlroots` 那份——而 `wlr_scene_subsurface_tree_create` / `wlr_scene_surface_create` 等十个符号全是同样情况，woodland 每个窗口的表面子树走的本来就是 libwlroots 这份。mango 也是这么用的。

## 2. 协议层的责任划分

Wayland 的 selection 是**源端持有**模型：数据不经过合成器，合成器只做"谁有选区、谁在拖、拖到谁身上"的路由。wlroots 把协议状态机全做完了，合成器要做的是策略。

| 环节 | wlroots 已做 | 合成器必须做 |
| --- | --- | --- |
| `set_selection` 请求 | 校验 serial 是否发给过该客户端、是否被更新的 serial 覆盖（`types/data_device/wlr_data_device.c:146-162`），然后 emit `request_set_selection` | 决定接受与否，调 `wlr_seat_set_selection()`（woodland 已做，`src/seat/seat.c:318`） |
| 选区生命周期 | 旧源销毁、给当前键盘焦点客户端重发 selection、emit `set_selection`（`:184-214`） | 什么都不用做；**但要知道源进程退出 = 选区变空** |
| `start_drag` 请求 | 只 emit `request_start_drag`，**不自己 start** | **校验 serial**，选 pointer/touch，调 `wlr_seat_start_pointer_drag()` / `_touch_drag()`；失败就 `wlr_data_source_destroy()` |
| 拖拽中的投放目标 | 由 `notify_enter` 驱动：`drag_handle_pointer_enter` → `drag_set_focus` → 给目标客户端发 `wl_data_device.enter` + offer（`wlr_drag.c:182-186`） | 每帧算 hit-test，调 `wlr_seat_pointer_notify_enter()` / `_notify_motion()` |
| 投放（drop） | pointer grab 的 button handler 在 grab_button 释放时完成 drop + 销毁 drag（`wlr_drag.c:233`） | **释放事件必须照常 `notify_button` 下发**；没有 surface 时也不能吞 |
| 拖拽图标 | 什么都不做。`wlr_scene_drag_icon_create` 只把 icon surface 挂成 subsurface 树，并跟随 `surface->current.dx/dy`（`types/scene/drag_icon.c:22-27`） | 建节点、**自己摆位置**（wlroots 建在 0,0）、每帧跟光标、结束时收掉 |
| 键盘焦点 | drag 期间起 keyboard grab，焦点锁在源窗口 | drop 之后要不要 refocus 由策略决定 |

推论：**合成器不做 hit-test，就没有拖拽投放；合成器不动图标，图标就钉在左上角。** 这两件事 wlroots 一概不管。

## 3. labwc 的做法（`src/dnd.c` 全文 120 行）

### 3.1 剪贴板

`src/server.c:713-714` 很早就建 `wlr_data_device_manager_create()`，紧跟着（`:721-731`）是 primary selection，注释里写明理由：

> Empirically, primary selection doesn't work with Gtk apps unless the device manager is one of the earliest globals to be advertised. All credit to Wayfire.

代价是 data device manager 被钉在全局列表靠前的位置，后面加全局得回头看中键粘贴有没有坏。`rc.primary_selection` 可配，默认 true（`src/config/rcxml.c:1590`）。data-control 两个都建（`:771-772`，zwlr + ext），并把它们列进 `parse_privileged_interface()` 的"特权协议"表，供 security-context 场景按客户端过滤。

### 3.2 拖拽：三个监听器 + 一棵专用树

初始化（`src/dnd.c:91-115`）：建一棵 `seat->drag.icons` 树挂在 **scene 根**，默认 `disabled`；挂 `request_start_drag` / `start_drag` 两个监听器。`destroy` 监听器不在这里挂，而是在 start 时挂、destroy 时摘——因为 `wlr_drag` 对象是每次拖拽新建的。

- **request**：`wlr_seat_validate_pointer_grab_serial()` 过就 `wlr_seat_start_pointer_drag()`，不过就 `wlr_data_source_destroy(event->drag->source)` 并打 ERROR。**只有 pointer 一条路**，没有 touch 分支。
- **start**：`assert(!seat->drag.active)`，清掉 `pressed` 上下文（避免拖拽结束后残留的按下态被当成点击），然后 `wlr_scene_drag_icon_create(seat->drag.icons, drag->icon)` + `raise_to_top` + `enable`。注意 **icon 可以为 NULL**（客户端可以不设图标），此时只置 active 不建节点。
- **destroy**：关树、摘监听器；`focus/followMouse` 打开时按光标下的 surface **重新 refocus**（先 focus NULL 再 focus 目标，强制走一次 enter）。默认不 refocus——和 openbox/mutter 一致：拖拽结束后焦点留在源窗口。

移动在光标层（`src/input/cursor.c:648-650`）：motion 里 `if (seat->drag.active) dnd_icons_move(seat, cursor->x, cursor->y)`，用的是 layout 坐标，所以跨输出天然正确。

### 3.3 cursor.c 里的三处守卫

| 位置 | 作用 |
| --- | --- |
| `:558-560` | "按住按钮移到别的 surface 上"的抑制逻辑，`!seat->drag.active` 时才生效（拖拽不是隐式抓取） |
| `:596-605` | 光标在服务端区域（桌面/SSD）时**不要**自己设光标——拖拽中覆盖光标会把客户端/图标的光标顶掉 |
| `:648-650` | 移动图标 |

### 3.4 labwc NEWS 里的六个历史坑（都是"看代码看不出来"的那种）

1. **#1936 图标被 layer-shell 盖住** → 开始时 `raise_to_top` 到 scene 根，不能只 enable。
2. **#1673 桌面上没有 surface 时松手，拖拽不结束** → 释放事件必须无条件 `notify_button`，不能因为"没有焦点 surface"就跳过。
3. **拖拽结束的 release 被误发给 layer client** → 顺序：先把 release 发给 grab（结束拖拽），再做窗口拖动收尾/SSD 状态清理。
4. **Xwayland 关闭时正在拖拽会崩**（wlroots < 0.18.2）→ labwc 用 `xwaylandPersistence=yes` 规避；我们锁的是 0.20.2，不受影响，但说明"拖拽与 X 生命周期耦合"这件事真实存在。
5. **拖拽/切换器/菜单期间要清指针与键盘焦点**（0.9 的 #2455）→ 否则 hover 状态会在浮层下继续变化。
6. **`Drag` 鼠标绑定在没有真实按下时也会跑**（#2196）→ 与拖拽无关但同源：区分"按下"与"grab 中"。

### 3.5 labwc 的边界

- **不桥接 X11 ↔ Wayland 的拖拽**。X11 内部拖拽由 X 客户端 + xwm 自己完成（图标是 override-redirect 窗口），X11 拖进 Wayland 客户端不支持。
- **不做剪贴板持久化**，靠外部 manager。
- 没有 touch drag、没有 `xdg-toplevel-drag-v1`。
- 直出渲染，不需要考虑图标与圆角/模糊的交互。

## 4. gxde-wlcom（treeland 血统）的做法

treeland 系的实现思路和 labwc 差别很大，值得看的是它多出来的三块：**输入类型回退**、**自建图标节点**、**内建持久化**。

### 4.1 每个 seat 一个 selection，三级 serial 校验回退

`src/input/selection.c:236-263` 的 `request_start_drag`：

```
pointer grab serial 通过  → wlr_seat_start_pointer_drag
touch grab serial 通过    → wlr_seat_start_touch_drag(point)
tablet 有隐式抓取         → 强设 pointer_state.grab_button = BTN_LEFT，打标记后按 pointer 起
都不通过                  → wlr_data_source_destroy(source)
```

平板那一档是纯务实做法：手写笔拖拽在 wlroots 里没有对应的 grab 类型，只能伪装成左键拖拽。

### 4.2 图标自己搭，不用 `wlr_scene_drag_icon_create`

`selection.c:196-232` 建 `tree` + `subsurface_tree`，然后自己监听 icon surface 的 `map` / `unmap` / `commit` / `destroy`：

- `map`/`unmap` → 切节点 enabled（**不是**靠 drag 生命周期）；
- `commit` → `node->x += surface->current.dx`（跟随 hotspot 增量，和 wlroots 内部实现同款）；
- 起点就用 `cursor->lx/ly` 定位，**不等第一帧 motion**。

这条路绕开了自己搭的那份逻辑，代价是 60 行重复代码（而且它依赖的 `wlr_scene_subsurface_tree_create` 同样来自 libwlroots——见 §9.1，绕不掉的其实没绕掉）。treeland 血统选择自建大概是历史原因，不是必要。

### 4.3 拖拽状态是光标层的一等公民

对外两个函数（`selection.c:403-420`）：`selection_handle_cursor_move(seat, lx, ly)` 和 `selection_is_dragging(seat)`。

调用点分布很能说明问题：

- `selection_handle_cursor_move()` 在 **4 处**被调：cursor motion、tablet motion、touch motion、portal（远程桌面注入）——即"所有能移动光标的地方"都要更新图标；
- `selection_is_dragging()` 在 **6 处**做守卫：抑制 hover 切换（`cursor.c:96`）、抑制点击后的假 motion（`:267`）、抑制坐标重算（`:290`）、抑制按钮释放路径（`:451`、`:470`）、touch 的 down/up（`touch.c:689`）。

这印证了 §6.3 的判断：**拖拽不是一个独立模块的事，它会穿透光标层的每一条分支**。

### 4.4 内建剪贴板持久化（404 + 191 行）

`selection_persist.c` 干的事：监听 `set_selection` → 把自己的 `wlr_data_source` 装成新选区 → 逐个 mime 从**旧源**的 `send()` 拿 fd 读进内存（pipe + `wl_event_source`，单条上限 64 MiB，超了放弃该 mime）→ 客户端来 paste 时再把缓存写回 fd（`selection_data.c` 的 writer 处理 `EAGAIN`，挂 `EPOLLOUT` 分片写）。

收益：关掉源应用后剪贴板还在（Wayland 用户最常见的抱怨）。代价：合成器要持有用户剪贴板内容（含密码）、要处理大图内存、实现量 600 行且都是易错的 fd 生命周期。

### 4.5 `org.kde.KWin.Clipboard` D-Bus 服务

`selection.c:86-121` 注册了 `/Clipboard` 上的 `getClipboardSelectionPid` / `getPrimarySelectionPid`（方法 + 属性 + 变更信号），语义是"告诉我现在选区的持有者 pid"。代码注释说这是给 `dde-clipboard-daemon` 这类 KWayland 客户端用的——它们靠这个 pid 判断"选区是不是我自己的，要不要接管"。

**这条对 woodland 需要验证**：如果 DDE 的剪贴板守护进程硬依赖该接口，那么在没有它的情况下，treeland 生态里的剪贴板历史会退化成"每次复制都被自己覆盖"或干脆不工作。woodland 目前没有 D-Bus 依赖，加一个 sd-bus 服务会引入新的构建依赖（可选项见 §10）。

### 4.6 顺带：`xdg-toplevel-drag-v1`

`toplevel_drag.c`（272 行）实现的是 wayland-protocols staging 里的 `xdg-toplevel-drag-v1`——浏览器把标签页拖出成新窗口、GIMP 拖图层。做法是把被拖的 toplevel 挂到拖拽源上、并让它的输入旁路（`ky_scene_node_set_input_bypassed`）。wlroots 0.20.2 没有这个协议的封装（`include/wlr/types` 里只有 `xdg_toplevel_icon_v1` / `xdg_toplevel_tag_v1`），要做得自己用 `wayland-scanner` 生成再手写状态机，属于另一个量级的活，本轮不做。

## 5. 三方对比

| 维度 | labwc | gxde-wlcom（treeland 系） | woodland 现状 |
| --- | --- | --- | --- |
| data device / primary selection | 有；primary 可配、默认开 | 有（全局注册顺序有额外约束，见下） | 有（`server.c:185`、`protocol/input.c:311`） |
| data-control | zwlr + ext 都建 | zwlr（+ KDE D-Bus） | zwlr + ext 都建（`server.c:197-198`） |
| serial 校验 | 交给 wlroots | 交给 wlroots + 自算选区 pid | 交给 wlroots |
| 剪贴板持久化 | 无（外部 manager） | 内建（64 MiB 上限） | 无 |
| X11 剪贴板 | wlroots xwm 自动 | 同 | 同（已 `set_seat`，`xwayland.c:753`） |
| X11 ↔ Wayland 拖拽 | 不桥接 | 不桥接 | 不桥接（保持） |
| DnD 图标 | `wlr_scene_drag_icon_create` + 专用树 + 置顶 | 自建 subsurface 树 + 四监听 | **无** |
| touch drag | 无 | 有（+ tablet 伪装） | 无（且没有 touch 设备支持） |
| `xdg-toplevel-drag-v1` | 无 | 有 | 无 |
| 拖拽中改光标 | 不改（`cursor.c:599`） | 不改（`selection_is_dragging`） | **会改**（SSD/default 分支） |
| drop 后 refocus | 仅 followMouse | 由普通 motion 决定 | 焦点不变（符合点击聚焦语义） |
| 拖拽期间 hover/SSD | 部分抑制 | 全面抑制 | 全跑 |

**一个值得记下的顺序约束**：labwc 说"data device manager 要尽早发"（GTK 中键粘贴），gxde 说"selection 相关全局必须晚于 `wl_seat`"（KWayland 客户端一看到 manager 就建 data device，seat 还没发会崩）。两条不冲突，但把 `data_control` 挪到 `wl_seat` 前面就会踩第二条。woodland 现状（data device 在 `server.c:185`、seat 在 `server.c:251`、primary selection 在 `protocol/input.c:311`）两条都满足，**改动全局注册顺序时要注意别破坏这个排布**。

## 6. woodland 现状审计

### 6.1 已有的

| 能力 | 位置 |
| --- | --- |
| `wl_data_device_manager` | `src/server/server.c:185`（紧跟 compositor，最早） |
| `zwlr_data_control` + `ext_data_control` | `src/server/server.c:197-198` |
| `primary_selection_v1` | `src/protocol/input.c:311` |
| `request_set_selection` 转发 | `src/seat/seat.c:318-323` |
| `request_set_primary_selection` 转发 | `src/protocol/input.c:276-279` |
| xwayland ↔ X11 剪贴板 | `src/xwayland/xwayland.c:753`（`wlr_xwayland_set_seat`） |

结论：**剪贴板侧没有功能缺口**，`wl-copy` / `wl-paste` / `cliphist` / `wl-clip-persist` 现在就能用（`--trace` 之外无副作用）。

### 6.2 缺口

- **A1**：没有 `request_start_drag` 监听 → 所有 Wayland 内拖放在协议层就断了。
- **A2**：没有图标节点/树 → 即使补了 A1 也看不见拖的是什么。
- **A3**：拖拽期间光标层全跑 → 改光标形状、刷 SSD hover、走贴边吸附判断。
- **A4**：`src/shell/cursor.c:179`、`:199` 用裸 `wlr_seat_pointer_clear_focus()` → 补了 A1 之后会变成"投放目标错窗口"的 bug。
- **A5**：没有 touch（`WLR_INPUT_DEVICE_TOUCH` 全仓无处理，capabilities 只发 pointer/keyboard）→ touch drag 本轮不做。
- **A6**：没有 trace 标签、没有 self_check 不变量、`wctl` 看不到拖拽状态。

### 6.3 会被拖拽放大的既有隐患

1. `woodland_cursor_process_motion()` 的 SSD 分支（`:153-188`）在算完 hover 后 **`return`**，且用的是裸 `clear_focus`。拖拽期间走到这里，图标不跟、目标丢、光标被改成 default。
2. `server_cursor_button()`（`:246`）按下分支会 `woodland_focus_view(view, true)` 并刷新 SSD pressed 态。拖拽期间不会有新的按下，但**释放**分支会跑 `shell_view_apply_snap()` / `ssd_button_release()`——前者有 `cursor_mode == MOVE` 兜住，后者依赖 `pressed_view` 是否残留。start 时清一次 pressed 态最省事（labwc 的 `cursor_context_save(&seat->pressed, NULL)` 就是干这个）。
3. 图标树不能挂 `workspace->tree`：切工作区会把正在拖的图标一起带走。必须挂 scene 根。
4. self_check 有一条"壁纸树是 scene 根的第一个子节点"（`src/shell/selfcheck.c:467`）。图标树是**追加**在末尾并 `raise_to_top`（仍然在末尾），不影响该不变量；但如果将来有人 `wlr_scene_node_lower_to_bottom` 图标树，会同时踩两条。

## 7. 实现设计

### 7.1 模块

```
include/dnd.h
src/dnd/dnd.c        ~260 行
src/dnd/meson.build  静态库，链 scene_deps + trace_lib（与 src/seat/ 同款）
```

不新建 `src/selection/`：剪贴板侧的转发已经分散在 `seat.c` 和 `protocol/input.c`，搬动收益小于风险。`dnd.c` 只负责拖拽。

`woodland_seat_init()` 里 `woodland_dnd_init(server)`，`woodland_seat_finish()` 里 `woodland_dnd_finish(server)`——保证监听器成对摘除（项目的 listener audit 惯例）。

### 7.2 数据结构

```c
struct woodland_dnd_icon {
    struct wlr_scene_tree *tree;     /* 本次拖拽的子树，挂在 icons 下；NULL = 没图标 */
    struct wl_listener tree_destroy; /* 子树被销毁时清 tree，防悬垂 */
};

struct woodland_dnd {
    struct woodland_server *server;
    struct wlr_scene_tree *icons;    /* scene 根的子节点，init 时建、默认 disabled */
    struct wlr_drag *drag;           /* NULL = 没有拖拽 */
    struct woodland_dnd_icon icon;
    struct { struct wl_listener request, start, destroy; } events;
};
```

`server->dnd` 内联进 `struct woodland_server`（和 `hover_view` / `pressed_view` 同层），不额外分配。

投放目标不另存指针：直接用 wlroots 自己维护的 `drag->focus`（它挂了 `focus_destroy` 监听器，目标窗口崩了会自动置 NULL）。早期版本存过 `last_surface`，那是 Use-After-Free 的入口——见 §11.6。

### 7.3 事件流

```
客户端 A: pointer button press → (serial s) → wl_data_device.start_drag(source, icon, s)
        ↓ wlroots 校验 s 是发给过 A 的
   seat.events.request_start_drag  →  wlr_seat_validate_pointer_grab_serial(origin, s)
        ↓ 通过                                    ↓ 不通过
   wlr_seat_start_pointer_drag               wlr_data_source_destroy(source)
        ↓ （内部先 clear_focus 再起 pointer grab）
   seat.events.start_drag  →  建子树 + 定位 + 置顶 + enable + trace "dnd start"
        ↓
   每帧 motion: 挪图标 → hit-test → notify_enter(变) / notify_motion → return（不走 SSD）
        ↓
   客户端 A 松手 → notify_button → grab 收尾：drop → wl_data_device.drop 给目标 → drag 销毁
        ↓
   seat.events.destroy(drag)  → 关树 + 摘监听 + trace "dnd end"
```

### 7.4 图标：交给 `wlr_scene_drag_icon_create`

理由见 §9.1（初稿曾主张自搭，实现时推翻）：

```c
/* icons 是 scene 根下的容器，跟光标走；helper 建的子树只承担 hotspot 增量 */
dnd->icon.tree = wlr_scene_drag_icon_create(dnd->icons, drag->icon);
/* 子树被销毁（drag 收尾 / 客户端撤 icon surface）时清指针，避免悬垂 */
woodland_listener_connect(&dnd->icon.tree_destroy,
                          &dnd->icon.tree->node.events.destroy,
                          dnd_icon_handle_tree_destroy, "dnd.icon_tree_destroy");
```

要点：

- helper 内部已经做了 `wlr_scene_tree_create` + `wlr_scene_subsurface_tree_create` + 跟 `surface->current.dx/dy` 的 commit 监听器 + 自动销毁，不用再挂 `map`/`unmap`/`commit`；
- 容器 `icons` 仍然自己管：它跟光标走（`set_position(icons, cursor->x, cursor->y)`），**起点在 start 处理器里就给一次**，别等第一帧 motion——否则图标闪一下左上角（labwc 就闪，因为它只在 motion 里挪）；
- 容器空闲时 `enabled=false`：不渲染、不参与 hit-test，self_check 的 `dnd_icon` 也据此判断；
- `drag->icon == NULL` 是合法状态，此时只置 active，不建节点。

### 7.5 motion 分发改造（`src/shell/cursor.c`）

在 `woodland_cursor_process_motion()` 里、session lock 分支之后、`cursor_mode` 分支之前插入：

```c
if (woodland_dnd_active(server)) {
    woodland_dnd_motion(server, time);   /* 挪图标 + hit-test + notify_* */
    return;                              /* 不走 SSD hover / 贴边 / 约束 */
}
```

`woodland_dnd_motion()` 内部：

```c
wlr_scene_node_set_position(&dnd->icons->node, cursor->x, cursor->y);

struct wlr_surface *surface = NULL; double sx = 0, sy = 0;
dnd_surface_at(server, cursor->x, cursor->y, &surface, &sx, &sy);

if (surface != dnd->drag->focus) {
    if (surface) wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    else         wlr_seat_pointer_notify_clear_focus(seat);   /* 必须带 notify */
}
if (surface) wlr_seat_pointer_notify_motion(seat, time, sx, sy);
```

`dnd_surface_at()` 在 hit-test 前临时将 `dnd->icons` 隐藏并在返回前恢复（同 labwc 机制），确保光标无论处在图标何处都能穿透命中下方的真正投放窗口；若命中结果仍落在图标子树里则当作没有目标。

只在 surface 变化时 `notify_enter`：以 `drag->focus` 为权威判定，重复 enter 虽然被 `drag_set_focus` 的早退挡住，但每帧一次进入 `notify_enter` 是无谓的（而且一旦 wlroots 改了早退条件就会变成每帧重建 offer）。

### 7.6 button 分发改造

```c
/* 在 server_cursor_button 最前面，session lock 之后 */
if (woodland_dnd_active(server)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;   /* 不做 focus / SSD pressed / 贴边 */
}
```

**顺序**：先 `notify_button`（grab 在这里完成 drop 并销毁 drag），再做任何收尾。反过来会出现 labwc #1657 那种"release 发给 layer client"的现象。

### 7.7 生命周期

- **start**：清 `pressed_view` / `pressed_part` 与 hover 态（等价 labwc 的 `cursor_context_save(NULL)`）。
- **destroy**：关 icons 树、摘 drag 的 destroy 监听器、trace。**不 refocus**——woodland 是点击聚焦语义，拖拽期间焦点没变，留在源窗口是对的。
- **锁屏**：`woodland_session_lock_is_locked()` 时若正在拖拽，直接 `wlr_seat_pointer_end_grab(seat)`（触发 grab 的 cancel → drag 销毁 → 我们的 destroy 处理器收尾）。放在锁屏请求入口，而不是光标路径里。
- **工作区/视图销毁**：不需要处理，drag 由 wlroots 持有，图标树在 scene 根。

### 7.8 可观测性

按项目的调试习惯，三件一起做，别只做功能：

- **trace**：`include/trace.h` 里加标签 `dnd`（先改清单再发第一条）。事件用 detail 写**决策依据**：`start icon=1 grab=pointer`、`end dropped=1`、`cancel lock`。
- **self_check**：拖动期间不跑（状态每帧都在变，容易误报），改成在 **start/destroy 之后**各跑一次，断言：
  - `drag == NULL` ⇒ icons 树 disabled；
  - `drag != NULL` ⇒ icons 树 enabled、是 scene 根的最后一个子节点、坐标等于光标坐标（±1 容差）；
  - `drag != NULL && drag->icon != NULL` ⇒ 存在子树且 enabled。
- **wctl**：`get_tree` 顶层加 `"dnd": {"active":bool, "dropped":bool, "icon":{w,h,visible}, "target":"<app_id/title>"}`。拖拽"掉错窗口"这类问题，一行 JSON 就能定位，不用录屏数像素。

## 8. 测试策略

### 8.1 headless 能测的

- 全局存在性：`wl_data_device_manager`、两个 data-control、`primary_selection`（扩展 `tests/smoke/` 里现成的探针，`appearance_probe.c` 的两次 roundtrip 写法）。
- 启动即无拖拽时：icons 树存在且 disabled、`wctl get_tree` 有 `dnd.active=false`。
- `--self-check` 全绿（确保新子节点没破坏"壁纸是第一个子节点"）。
- 单位测试 `tests/unit/test_dnd.c`：把 start/destroy/lock 的处理抽成纯函数（入参是 `struct wlr_drag *` 的假对象 + 计数桩），覆盖"无图标"、"图标先 map 后 unmap"、"destroy 未 start" 三条路径。

### 8.2 为什么 DnD 无法 headless 驱动

`wl_data_device.start_drag` 的 serial 必须来自一次**真实的 pointer button press**（`wlr_seat_validate_pointer_grab_serial` 查的是 `seat->pointer_state.grab_serial`）。headless 后端没有指针设备，合成器不会产生 button press，客户端也就拿不到可用 serial。想自动化，要么在合成器里加 `wlr_virtual_pointer_v1`（现在没建，且为了测试加一个全局不划算），要么造一个假 `wlr_drag` 直接 emit `start_drag`（能覆盖图标与渲染路径，覆盖不到协议校验）。

建议：**接受"真机手测 + 单测状态机"**，别为它引入虚拟指针。

### 8.3 真机手测清单

1. Thunar/nautilus 拖文件到 foot/gnome-terminal → 终端拿到路径（不是"掉进别的窗口"）。
2. 拖到桌面空白 → 取消，源应用收到 `dnd_finish(action=none)`，两分钟内不崩。
3. 拖到 panel（layer-shell）上方 → 图标**盖在** panel 之上；松手不触发 panel 的点击。
4. 拖过 SSD 标题栏 → 图标继续跟手；松手不落到任何窗口。
5. 双屏之间拖 → 图标连续跨屏，无跳变、无残影。
6. 拖拽中按 Super+1 切工作区 → 图标消失不残留；回到原工作区状态一致。
7. 拖拽中触发锁屏 → 拖拽被取消，解锁后无残留图标、点击正常。
8. X11 应用（如 xterm 下拉文件）→ 图标是 override-redirect 窗口，走 unmanaged 路径，不受本模块影响。

## 9. 风险与取舍

### 9.1 `wlr_scene_drag_icon_create` 到底能不能用（初稿结论已推翻）

初稿的判断是"不能用：scenefx 缺这个符号，链接落到 libwlroots 会跨库访问结构体"。**这个判断是错的**，实现阶段复核后推翻。

事实是：`wlr_scene_*` 这一族符号**本来就分属两个库**，而且是有意的切分。scenefx 只替换 buffer / rect / blur / shadow / node / tree / output 那一层，**没有**替换"把 surface 包成节点"那一层：

```
$ nm -D --defined-only libscenefx-0.5.so | grep '^wlr_scene' | wc -l      # 82
# comm -13 scenefx wlroots —— 只有 libwlroots 有的：
wlr_scene_subsurface_tree_create      wlr_scene_surface_create
wlr_scene_xdg_surface_create          wlr_scene_surface_send_frame_done
wlr_scene_layer_surface_v1_create     wlr_scene_surface_try_from_buffer
wlr_scene_layer_surface_v1_configure  wlr_scene_drag_icon_create
wlr_scene_attach_output_layout        wlr_scene_output_layout_add_output
wlr_scene_subsurface_tree_set_clip
```

也就是说 `wlr_scene_subsurface_tree_create` 和 `wlr_scene_drag_icon_create` 属于**同一类**——woodland 每个窗口的表面子树原本走的就是 libwlroots 这份，渲染一直是好的。所以"绕开 drag_icon_create"自相矛盾：要绕就得连 `wlr_scene_subsurface_tree_create` 一起绕，那整个合成器就不用画窗口了。

而且 `wlr_scene_drag_icon_create` 的实现（`subprojects/wlroots/types/scene/drag_icon.c`，约 60 行）就是 `wlr_scene_tree_create` + `wlr_scene_subsurface_tree_create` + 一条跟 `surface->current.dx/dy` 的 commit 监听器 + 两条销毁监听器——**手搭等于把它抄一遍**。它只调用公开 API，两头的 `struct wlr_scene_node` / `wlr_scene_tree` 也逐字段相同（type/parent/link/enabled/x/y/events/data/addons/visible）。

旁证：mango（mangowm，`dependency('scenefx-0.5')`）在 `src/input/pointer.c:handle_start_drag` 里就是 `wlr_scene_drag_icon_create(server.drag_icon, drag->icon)`，生产在用。

**决定**：用上游的 `wlr_scene_drag_icon_create()`。初稿手搭的那 60 行已删除（见 §11.5）。`AGENTS.md` 的 scenefx 硬约束第 8 条相应改写为"符号分属两库是有意的，别当缺符号绕"。

### 9.2 图标会不会被 hit-test 命中

`wlr_scene_node_at()` 命中 buffer 节点后会问 `wlr_surface_point_accepts_input()`，也就是看 surface 的输入区域。拖拽图标的 surface 输入区域在不同工具包里不一定为空（有的把 hotspot 那一点设成可命中）。一旦命中，`notify_enter` 会把**图标自己**当成投放目标，drop 就永远送不到真正的窗口。

**决定**：双层防御。① hit-test 期间临时 `set_enabled(icons, false)` 再恢复——命中结果直接"穿透"到图标下方的窗口，语义正确（labwc `desktop.c:332` 就是这个做法，注释写着 *Prevent drag icons to be on top of the hitbox detection*）；② 命中结果若仍落在图标子树里，祖先检查当作没有目标（兜底）。只做②是不够的：`wlr_scene_node_at()` 返回的是**最上面**那个节点，图标挡住的窗口根本没机会成为目标，拖到图标覆盖区就等于没有投放目标。

### 9.3 剪贴板持久化要不要内置

- labwc 不做：合成器不碰用户数据，600 行 fd 状态机省掉；
- gxde 做：用户体验好（关掉应用还能粘贴），代价是合成器持有明文剪贴板。

**决定**：本轮不做。我们已经有 data-control，`wl-clip-persist` / `cliphist` 拿来即用，功能上不缺；持久化属于"桌面环境该不该替用户存密码"的产品决策，先留白。若将来要做，照 gxde 的三段式（读缓存 → 装自己的 source → 写回 fd）实现，但**上限要可配**，默认别给 64 MiB。

### 9.4 touch

woodland 现在连 touch 设备都不接（`server_new_input` 只有 keyboard/pointer 分支，capabilities 只发 P/K）。加 touch drag 之前得先有 touch 支持，两件事一起做才有意义，本轮跳过。写代码时把 `wlr_seat_validate_touch_grab_serial` 的分支留成 `/* TODO: touch 设备支持后再接 */`，别留一个永不执行的死分支。

### 9.5 X11 ↔ Wayland 拖拽

wlroots 不桥接（xwm 只处理 X11 内部 Xdnd），labwc 也不桥接。保持不桥接：woodland 的定位是"能跑 treeland 生态客户端"，跨 X/Wayland 拖放属于桌面环境级特性，成本远高于收益。

## 10. 待验证清单（进实现前先回答）

1. `dde-clipboard-daemon` / klipper 类客户端是否**硬依赖** `org.kde.KWin.Clipboard`（`getClipboardSelectionPid`）。硬依赖 → 需要 sd-bus 服务（约 80 行 + 一个构建依赖），否则可以只记 TODO。
2. 常用工具包（GTK3/4、Qt5/6、Firefox、Chromium）的拖拽图标 surface 输入区域到底是不是空。不管结论如何，§9.2 的"隐藏 + 祖先检查"都保留——前者管语义（穿透），后者管兜底。
3. 分片缩放（`wlr_fractional_scale`）下图标 buffer scale 是否随输出变化：icon surface 的 `preferred_buffer_scale` 由 scenefx 的 surface 树自动处理，但**跨不同 scale 的输出之间拖动**要实测是否出现图标忽大忽小。
4. 拖拽期间 `[effects] blur` / 圆角是否会错误地作用到图标子树（图标不是 `woodland_view`，不该被 `woodland_effect_update()` 碰到；确认 effect 模块的遍历入口只走 views 链表）。

## 11. 落地记录（2026-11-08 实现）

分支 `feature/clipboard-dnd`（worktree `/home/rewine/woodland-dnd`，基于 `origin/main`）。

### 11.1 文件

| 文件 | 内容 |
| --- | --- |
| `include/dnd.h` | `struct woodland_dnd` / `struct woodland_dnd_icon` 与模块接口 |
| `src/dnd/dnd.c` | 拖拽全流程：request_start_drag 校验、图标子树、hit-test、enter/motion、cancel、收尾 |
| `src/dnd/meson.build` | `dnd_lib`，只链 `util_lib` + `trace_lib` |
| `src/seat/seat.c` | `woodland_dnd_init/finish` 挂在 seat 的 init/finish 里（监听器成对） |
| `src/shell/cursor.c` | motion 早退走 dnd；button 先 notify 再收尾；两处裸 `clear_focus` 换成 `notify_` |
| `src/session_lock/session_lock.c` | 收到锁屏请求先 `woodland_dnd_cancel()` |
| `src/ipc/commands.c` | `get_tree.dnd` + `wctl help` 字段文档 |
| `src/shell/selfcheck.c` | 新增不变量 `dnd_icon`（空闲关、拖拽开且置顶、有图标必有节点） |
| `tests/unit/test_dnd.c` | 状态机单测；`test_selfcheck.c` 增加 dnd 的两条反例 |
| `tests/smoke/dnd_probe.c` + `run_dnd_smoke.sh` | start_drag 拒绝路径的端到端断言 |

### 11.2 与设计的差异

1. **自检调度走钩子**。`selfcheck.c` 在 `shell_lib` 里，而 dnd 是 shell 的**下游**（光标层调 dnd），直接调 `woodland_selfcheck_schedule()` 会让两个静态库互相依赖。改成 `server->selfcheck_schedule_hook`，由 `woodland_shell_init/finish` 挂上、摘掉——和既有的 `pointer_constraint_is_focused_hook` 同一套写法。
2. **hit-test 不用 `woodland_view_at()`**。那条路只认 toplevel 视图，而拖到 panel（layer-shell）、Dock、X11 override-redirect 窗口上都该有 `enter`。直接问 `wlr_scene_node_at()`，拿到什么 surface 就交给 wlroots 自己按 seat client 查表。图标子树的双层防御（隐藏 + 祖先检查，§9.2）照做。
3. **按钮分支做的是"先下发再收尾"**，而不是整段跳过：`notify_button` 必须在任何收尾之前（grab 在里面完成 drop 并销毁 drag），之后只清 SSD 的 pressed/hover 态就返回。
4. 图标树两层：`icons` 容器跟光标（布局坐标），`wlr_scene_drag_icon_create` 建的子树只承担 `surface->current.dx/dy` 的 hotspot 增量——和 wlroots 自己的实现同构（因为就是它）。

### 11.3 测试结论

- `meson test`：effects 构建 40 OK / 0 fail / 3 SKIP；`-Deffects=disabled` 构建 41 OK / 0 fail / 3 SKIP（SKIP 是缺 `grim`/`rndr`/`xcb` 之类的外部工具，与本次改动无关）。
- **拒绝路径可以 headless 驱动**（设计里没预料到）：`start_drag` 只有 `origin` surface 非空这个限制（协议签名 `?oo?ou`），serial 可以故意给错。客户端发一次 → 合成器校验失败 → 销毁源 → 客户端收到 `wl_data_source.cancelled`。`smoke-dnd` 断言这条 + 轨迹里有 `dnd`/`reject` + 事后 `get_tree.dnd` 干净。
- 真拖拽（按下 → 拖 → 投放、图标跟手、跨输出）仍然只能真机手测，清单见 §8.3。

### 11.4 仍未做

- touch drag：woodland 还没接 `WLR_INPUT_DEVICE_TOUCH`，`request_start_drag` 里留了注释而不是死分支。
- `xdg-toplevel-drag-v1`（§4.6）、剪贴板持久化（§9.3）、`org.kde.KWin.Clipboard`（§4.5）——与设计一致，本轮不做。

### 11.5 交付后修正：图标改用 `wlr_scene_drag_icon_create`

提交 `f4d9970` 里的图标是自己搭的（60 行：建 tree、建 subsurface 树、commit 监听器跟 dx/dy、两条销毁监听器）。交付后被问到"mango 为什么能用 `wlr_scene_drag_icon_create`"，复核发现**初稿 §9.1 的结论是错的**：

- `nm -D libscenefx-0.5.so` 里确实没有这个符号，但 `wlr_scene_subsurface_tree_create`、`wlr_scene_surface_create`、`wlr_scene_xdg_surface_create`、`wlr_scene_layer_surface_v1_create` 等十个符号**全都没有**——它们本来就在 libwlroots 里，woodland 每个窗口的表面子树走的就是那一份。把"符号在另一个库里"当成风险，等于说整个合成器不该工作。
- 自搭的代码里第一个调用就是 `wlr_scene_subsurface_tree_create`，即"Risk"根本没被绕开。
- mango（`dependency('scenefx-0.5')`）在 `handle_start_drag` 里直接用 `wlr_scene_drag_icon_create`，生产验证。

修正：`dnd_icon_create()` 换成 helper，`struct woodland_dnd_icon` 从 5 个字段缩到 2 个（`tree` + `tree_destroy` 监听器），`append_dnd()` 的图标尺寸改读 `dnd->drag->icon->surface`。净删 60 行，行为不变（helper 就是把这几个调用串起来）。`AGENTS.md` 第 8 条同步改写。

### 11.6 评审优化：焦点权威源、图标穿透与无源拒绝

根据代码评审结果完成三处优化：

1. **移除 `dnd->last_surface`**：改为直接以 wlroots 自身维护的 `dnd->drag->focus` 作为事实源。原 `last_surface` 指针在目标窗口被销毁时无法同步感知，在 `wctl get_tree`（`append_dnd`）中会引发 Use-After-Free 崩溃。
2. **图标 hit-test 穿透**：`dnd_surface_at()` 在调用 `wlr_scene_node_at()` 时临时禁用 `dnd->icons`，规避图标自身遮挡底层投放窗口。
3. **`source == NULL` 时的 drag 对象释放**：在校验失败拒绝请求时，若 `event->drag->source == NULL`，通过 `pointer_grab.interface->cancel` 彻底销毁未启动的 `wlr_drag` 对象，杜绝内存泄漏。

**对这三条的独立核实**（不是照单接收）：

- `struct wlr_drag` 的 `focus` 是公开字段，wlroots 自己挂了 `focus_destroy` 监听器在目标 surface 销毁时置 NULL——所以它比自定义缓存更安全，这条成立。副作用是语义更好：`drag_set_focus()` 的早退条件就是 `surface == drag->focus`，用它判断"目标变没变"天然一致。
- `pointer_grab.data` / `pointer_grab.interface` 在 `wlr_drag_create()`（`wlr_drag.c:481`）里就设好了，不是 start 之后才设；`drag_destroy()` 对 `started == false` 的 drag 不碰 `seat->drag`，也不 `end_grab`——所以对未启动的 drag 调 cancel 是安全的，不会误伤别人的 grab。`cancelling` 标志还保证幂等。
- 泄漏是**真的**，不是理论上的：把 `dnd_reject_drag()` 的 `else if` 分支临时去掉，ASan 构建下 `smoke-dnd` 立刻红：
  ```
  Direct leak of 312 byte(s) in 1 object(s) allocated from:
      #1 ... wlr_drag_create ../subprojects/wlroots/types/data_device/wlr_drag.c:446
  ```
  恢复后转绿。所以 `tests/smoke/dnd_probe.c` 里新加的 "`start_drag` with `source == NULL`" 用例是有效回归，不是摆设。
- 图标穿透这条与 labwc 逐行对过：`labwc/src/desktop.c:332` `dnd_icons_show(&server.seat, false)` → `wlr_scene_node_at()` → `:340` 恢复；`labwc/src/dnd.c:104` 的 `dnd_icons_show()` 就是一个 `set_enabled` 包装。我们的实现把这个开关收进 `dnd_surface_at()` 内部，调用方不用知道。

顺带修正：会话里早先写的 `dnd->last_surface` 字段、`dnd_icon_handle_commit()`、`icon.armed` 全部删除；`include/dnd.h` 的 `struct woodland_dnd_icon` 现在只有 `tree` + `tree_destroy` 两个字段。
