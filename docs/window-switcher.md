# Alt+Tab 窗口切换器

> 日期：2026-11-15
> 状态：已实现
> 相关文件：`src/shell/window_switcher.c`、`include/window_switcher.h`、`src/seat/seat.c`、`src/shell/cursor.c`、`src/shell/focus.c`

---

## 1. 为什么不是 `focus_cycle`

`focus.h` 里已经有 `woodland_focus_cycle(server, forward)`：按 z 序取下一个窗口直接聚焦，无状态、无 UI。它的问题是**没有"我在选谁"这一段**——

- 按一下切一个，切过了只能反着再切回来，看不到窗口列表；
- 两个窗口时它在"当前窗口"和"下一个"之间来回，用户根本不知道自己按到了第几个；
- 键位是 `Alt+Tab`，但客户端的键盘焦点在按键之间被反复改写，`activated` 抖动一串。

所以本模块是"按住 Alt 期间的面板 + 候选快照"，`focus_cycle` 保留给 `Alt+F1` 这种"按一下就走"的键位（`seat.c:handle_keybinding`）。

## 2. 状态机

三个动作，全部在 `window_switcher.c`：

| 动作 | 触发 | 做什么 |
| --- | --- | --- |
| `begin` | `Alt+Tab` 的 press（`seat.c` → `handle_keybinding`） | 拍候选快照、预选下一个、显示面板 |
| `step` | 面板开着时再按 `Tab` / `Shift+Tab` / 方向键 | 只移动高亮，不动焦点、不动 z 序 |
| `finish(apply)` | 松开 `Alt` / `Enter`（apply=true）、`Escape` / 鼠标（apply=false） | 收起面板；apply 时聚焦选中项（`raise=true`），最小化的目标先恢复 |

```
Alt↓  Tab↓ ──────▶ 面板出现，高亮"下一个窗口"
       Tab↓ ──────▶ 高亮再走一格（列表不变形）
       Tab↑ ...
Alt↑  ───────────▶ 面板消失，选中的窗口被 raise + 聚焦
Esc   ───────────▶ 面板消失，焦点不动
```

`begin` 在 `tab` 的 press 里被调用，`step` 永远不会经由键位表——面板一开，键盘就整体交给了切换器（见 §4），第二次 `Tab` 直接进 `handle_key`。

## 3. 候选集与顺序

- **候选集 = `woodland_view_restorable()`**：当前工作区、已映射、非 override-redirect、不是 `NEVER` 焦点模型的 X11 窗口，**最小化的窗口也在内**。
  它就是 `woodland_view_focusable()` 放过"最小化"那一条，判定只写在 `focus.c` 一份（`view_focusable_ex(view, ignore_minimized)`）——两处各抄一遍条件迟早会漂。
  - 为什么放宽：Alt+Tab 是"把窗口叫回来"最自然的手势。任务栏不一定有（quickshell / LXQt 之外的会话），而面板摘掉最小化的行等于"最小化的窗口再也切不回来"。
  - 代价在提交路径：最小化的窗口自己拿不到焦点，所以 `woodland_focus_view()` 改成"先恢复、再继续聚焦"（见 §6），切换器这边只记一条 `restore` 轨迹。
- **顺序 = z 序**（`server->views`，表头最上）。所以"松开 Alt 落到谁"和"直接点任务栏激活谁"是同一个答案。
- **快照**：`begin` 时把指针拷进 `sw->views[]`（定长 32），最小化状态另拷一份到 `sw->minimized[]`——后者用来判断"面板要不要重画"，现算 `view->minimized` 就分不出"真变了"和"`view_changed` 又发了一次"。之后 `step` 只在这个数组里走。每步重算不行——每次聚焦都会把目标窗口 raise 到表头（`focus.c:view_raise`），边遍历边改序会让列表在脚底下变形，连按 `Tab` 会在两个窗口之间弹。
- **方向**：`forward` 沿链表往**表尾**走（表头是最上面那个），和 `focus.c:woodland_focus_pick()` 逐行同构——
  两边都"先拍快照再按下标走"，所以 Alt+Tab 一次聚焦谁、面板预选谁，是同一个答案。
  冒烟用**三个**窗口钉这条：两个窗口时正着切和反着切是同一件事，方向写反了测试照样绿。
- **初始高亮** `woodland_switcher_initial_index()`：焦点在列表里就沿方向走一格（`Alt+Tab` → 上一个用过的窗口；`Alt+Shift+Tab` → 列表最后一个），不在就取首/尾。纯函数，单测直接钉。
- **只剩最小化窗口时面板照开**：那正是唯一能把它们叫回来的入口。`begin` 只在候选数为 0（全部 unmap / 都在别的工作区）时才拒绝。

## 4. 键盘归属

插入点在 `seat.c:keyboard_handle_key`，位置很关键——排在**锁屏之后、窗口菜单 / 交互式移动 / 全局快捷键 / 内置键位之前**：

```c
uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
if (woodland_window_switcher_is_active(server)) {
    for (int i = 0; i < nsyms; i++)
        if (woodland_window_switcher_handle_key(server, syms[i], event->state, modifiers))
            return;
    return;   /* 没识别的键也吞掉 */
}
```

三条理由：

1. **`Tab` 不能漏给客户端**：漏了就是往编辑器里插一个制表符；
2. **`release` 也要吞**：press 已经被合成器吃掉，孤立的 release 会让客户端的按键状态错位（和 `seat.c` 里 VT 切换键的"孤儿 release"同一套理由）；
3. **排在全局快捷键之前**：`treeland-shortcut-manager-v3` 的绑定和内置键位在选窗口期间都不该生效。

**松开 Alt 的判据是 keysym（`Alt_L` / `Alt_R` 的 release），不是 modifiers。** `wlr_keyboard_notify_key` 是先发 `events.key`、再 `xkb_state_update_key`（`wlr_keyboard.c:110`），所以按键回调里看到的 modifiers 是**这一下之前的**状态——指望它判断"Alt 松了没有"等于依赖实现顺序。keysym 就是这一下按的是什么，不会骗人。

`keyboard-shortcuts-inhibit` 生效时整条链都不进（`inhibited` 判定在 `handle_keybinding` 之前），所以全屏游戏里 `Alt+Tab` 不会被抢。

## 5. 渲染

一块 cairo 画好的 `wlr_scene_buffer`，挂在自己的 `wlr_scene_tree`（scene 根的子节点，打开时 `raise_to_top`，所以盖在 layer 的 overlay 层之上）。

- **尺寸**：行高 30、内边距 6，宽度按最长一行的文本量（pango，夹在 260..560 之间），高度 = 内边距 × 2 + 可见行数 × 30。量的是**加过后缀的完整文本**，否则最小化那行会被 ellipsize 掉尾巴。
- **可见行窗口**：候选 ≤ 9 行全画；多了只画 9 行，`scroll` 跟着高亮走（`switcher_clamp_scroll`）。
- **最小化的行**：标题后加 `  (已最小化)` / `  (minimized)`，未高亮时文字 alpha 压到 0.55。只改 alpha、不另配一套颜色——配色有客户端覆盖 / 配色方案 / 配置文件三层来源，多一套就多一处要同步的常量。后缀拼进同一条 pango 文本，所以 `switcher_row_text()` 先按"给后缀留位置"截标题，不能直接 `snprintf("%s%s")`（长标题会把后缀截掉，而那条信息最不能丢）。
- **配色**：读 `[appearance]`（treeland-appearance 写入的 `dark` / `accent_rgb`），没有就用内置浅色盘 + 蓝色高亮。和窗口菜单同一套取值逻辑，不是第二套主题。
- **圆角 / 阴影**：圆角取 `[effects] corner_radius`（`effects.enabled=false` 时为 0），阴影只在 effects 构建且 `[effects] shadow=true` 时建 `wlr_scene_shadow`。非 effects 构建里这两条路径都是空操作。
- **缩放**：`cairo_surface_set_device_scale()` 按输出 scale 画，`dest_size` 用逻辑尺寸——和 `window_menu.c` 一样的做法。

标题优先 `view->title`，空则 `app_id`，都空则 `未命名窗口` / `Untitled window`（跟 `window_menu.c` 一样按 locale 选）。

## 6. 生命周期与安全

- **候选中途变化**：两个信号一起盯，摘除逻辑共用 `switcher_drop()`（`memmove` 保持 z 序、钳高亮、`relayout` 重画，摘空了自动收起）。**最小化不走摘除**，只是改标记（理由见 §3）。
  - `server->view_unmapped`（`switcher.unmapped`）：关闭 / 销毁。信号在 `free(view)` 之前发
    （`shell.c:woodland_view_destroy`），所以摘的时候指针仍然有效。**不摘就是悬空指针**——
    面板每帧都会读 `view->title`。
  - `server->view_changed`（`switcher.changed`）：**最小化不发 unmap**。`woodland_view_set_minimized()`
    只是关掉 scene 节点再发 `view_changed`。回调里的判定顺序是**先"还能不能切"、再"最小化状态变没变"**：
    1. 该视图不在快照里 → 直接返回（这个信号是全场广播，发得极勤）；
    2. `!woodland_view_restorable()` → `switcher_drop()`（挪去别的工作区、客户端自己 unmap）；
    3. 最小化状态翻转 → 行留着，更新 `sw->minimized[idx]` 再 `relayout`（标签变了、宽度可能跟着变），
       记一条 `min n=` / `unmin n=`；
    4. 其余（标题 / 激活态） → 返回。
    **2 必须在 3 前面**：两件事可能落在同一个信号里（最小化 + 被工作区逻辑带走），反过来写就会
    走进 3 然后 return，面板里留下一行**已经切不过去**的候选，要等下一个信号才摘掉。
- **提交到最小化的窗口**：`switcher_finish()` 只调 `woodland_focus_view(target, true)`，恢复由它自己做——
  这条路径原来有个坑：`focus_view` 恢复完就 `return`，注释说"重 map 会再进来"，但 xdg 视图最小化
  **不 unmap**，于是窗口恢复了却拿不到焦点。现在改成恢复后继续往下走，顺带修好了
  `xdg-activation`（点通知栏激活一个最小化窗口）同样的毛病。切换器这边额外记一条 `restore` trace，
  否则"切到一个最小化窗口"和普通切换在轨迹里看不出区别。
- **鼠标按下**：收起面板，但那一下照常下发给客户端（`cursor.c:server_cursor_button`）。吞掉 press 会留下没有配对的 release。
- **右键菜单开着时按 Alt+Tab**：`begin` 第一件事就是 `woodland_window_menu_hide()`。不收的话两块面板并存，
  面板盖住菜单，提交后焦点换了、菜单还挂在**原来那个窗口**上——接下来一个 Enter 就作用到旧窗口
  （典型的"切换完莫名其妙关掉一个窗口"）。Alt+Tab 是"选窗口"的模态手势，右键菜单在这个上下文里没有意义。
- **拖动 / 缩放或多任务视图进行中**：`begin` 直接拒绝。拖动/缩放时键盘归 `interactive` 那一层
  （方向键微调、Escape 取消，见 `seat.c` 的 `cursor_mode` 分支），多任务视图激活时键盘归总览界面（含 `Tab` 切卡）；
  反过来，切换器开着时经协议触发 `woodland_multitask_view_open()` 会先调用 `woodland_window_switcher_cancel()` 收起面板，避免两层模态叠放。
- **拖拽图标层序**：面板打开时 `raise_to_top`，若当前正在拖拽（`server->dnd.drag && server->dnd.icons`），随即把 `dnd->icons` 重新置顶，保持 `self_check` 的 `dnd_icon` 不变量。
- **切换工作区**：面板开着时若外部经 `ext-workspace` / `treeland-compositor-action` 切走工作区，`workspace_changed`（`switcher.workspace` 监听器）一到即 `finish(false)` 收起，避免快照残留旧工作区窗口。
- **锁屏**：两头都堵。`begin` 直接拒绝；面板开着时锁屏，`lock_changed` 一到就 `finish(false)` 收起
  （`switcher.lock` 监听器）。只靠锁屏树 `raise_to_top` 盖住面板不够——状态留着的话，解锁后第一次
  `Tab` 会落进一个陈旧的高亮项，松手就换焦点，用户看到的是"解锁后窗口突然跳了一下"。
- **输出拔插**：**不缓存 `struct woodland_output *`**。`output.c` 是先摘链、发 `output_removed`、
  再 `free(output)`，而面板的生命周期跨事件；把指针存下来就得处理"切换途中那个输出没了"。
  改成 `switcher_relayout()` 里现挑（跟焦点窗口走，没有就第一个），指针的有效期缩到一次函数调用。
- **`shell_finish`**：`window_switcher_finish()` 先 `finish(false)` 收起，再摘监听器、销毁 scene 树。
  顺序要紧：`shell_finish` 排在 `session_lock_finish` 之前（`server.c`），所以摘 `lock_changed`
  时那个信号仍然有效。`lock_changed.link` 在 init 里无条件 `wl_list_init`，收尾按 `link.prev` 判断，
  单测那种没有 session_lock 的裸 server 也不会踩空。
- **面板 buffer 在 finish 时放掉**：`wlr_scene_buffer_set_buffer(panel, NULL)`。一个 560×300 的
  ARGB 面 600KB 出头，面板关着不会被渲染，攥着只是白占。
- 没有候选（一个可聚焦窗口都没有）时不弹空面板，也不吞 `Alt+Tab`。

## 7. 调试与验证

```bash
wctl get_tree | jq .window_switcher
# {"active":true,"count":3,"index":1,"selected":42}
wctl get_trace | jq '[.entries[] | select(.what=="switcher") | .detail]'
# ["begin n=3 dir=fwd sel=1","commit"]
```

`get_tree` 的每个视图现在多了 `restorable`，紧挨着 `focusable`：

```bash
wctl get_tree | jq -c '.views[] | {id, title, minimized, focusable, restorable}'
# {"id":3,"title":"xterm","minimized":true,"focusable":false,"restorable":true}
```

`minimized:true` + `focusable:false` + `restorable:true` 就是"它在 Alt+Tab 列表里、但点不过去也拿不到键盘"的官方解释。排查"为什么面板里有这一行 / 为什么自检红了"时先看这三个字段，不用去推条件。

`self_check` 里多了一条 `switcher_snapshot`（`selfcheck.c:check_switcher`）：

- 未激活时要求快照为空（`count == 0` 且 `selected == -1`）——收尾没清干净会在这里显形；
- 激活时要求每一项**都还在 `server->views` 里、且仍然 `woodland_view_restorable()`**（即"可聚焦，或者最小化了但恢复之后能聚焦"）。这条抓的是
  `view_unmapped` 监听的漏挂/漏摘：快照里留着已卸载的窗口，面板画的是陈旧行；留着已 free 的
  窗口，渲染时读 `view->title` 就是 UAF。两者从面板外观上都看不出来（最多少一行），只有拿链表
  对照才说得清。先查链表成员（纯指针比较）再查 restorable（会解引用），顺序反过来就可能崩在
  已释放对象上。
- **最小化的候选必须是绿的**。这条不变量放宽过一次：早先写的是"必须 focusable"，最小化进列表
  一上线就会被自己的自检判红。判据换成 restorable，单测里两个方向都钉（最小化→仍绿；只改
  `mapped` 不发信号→红）。

注意通过的检查项只写 `name`、不写 `expected`/`actual`（自检的约定是"绿了就别吵"），所以
脚本里只能断言"这条检查在跑 + 整体 ok"，两个分支的语义由 `tests/unit/test_selfcheck.c` 钉住
（它故意只改 `mapped`、不发信号，断言 active 分支会红）。

trace 标签 `switcher` 的 detail 有七种：`begin n=<候选数> dir=fwd|back sel=<下标>`、`min n=<候选数> sel=<下标>`、`unmin n=<候选数> sel=<下标>`、`drop n=<剩余> sel=<下标>`、`commit`、`restore`、`cancel`。`min`/`unmin` 只记最小化状态真的翻转那一次（`view_changed` 发得很勤，不是每次都写）；`restore` 只在提交目标当时是最小化窗口时多记一条，紧跟在 `commit` 后面。

验证分两层：

| 用例 | 覆盖 |
| --- | --- |
| `tests/unit/test_focus.c`（`meson test -C build focus`） | `woodland_view_restorable()` 与 `focusable()` 的**唯一分歧**就在最小化那一条：`restorable` 在最小化 / 普通窗口上为真，在别的工作区 / override-redirect / 未映射上为假 |
| `tests/unit/test_window_menu.c`（`meson test -C build window_menu`） | 交叉用例：菜单开着时 `begin` 会把它收掉；菜单没开时 `hide` 是空操作 |
| `tests/unit/test_window_switcher.c`（`meson test -C build window_switcher`） | 下标轮转、预选、高亮移动、候选被 unmap 时摘除、**最小化时行留着且标记翻转**（只发 `view_changed`）、**提交到最小化目标会恢复**、**全部候选都最小化时仍能开面板**、**最小化 + 挪工作区落在同一个信号里时摘除**（钉判定顺序）、单候选时不动、锁屏即收且解锁不回弹 |
| `tests/unit/test_selfcheck.c`（`meson test -C build selfcheck`） | `switcher_snapshot` 的两面：面板开着全绿、**最小化的候选仍然全绿**；漏发 `view_unmapped` 时红在"no longer a valid target" |
| `tests/smoke/run_switcher_smoke.sh`（`meson test -C build smoke-switcher`） | **三窗口**、按视图 id 断言四轮：① `Alt↓ Tab↓ Tab↑` 落到 z 序第二个、再一格到第三个，松手提交给第三个；② raise 改写序后再切落到第二个（证明顺序不是巧合）；③ 面板开着时 `xdg_toplevel_set_minimized()` 掉**高亮**那个——行还在（计数 3）、`get_tree` 报 `minimized:true`、松手焦点落到它且 `minimized:false`；④ 开面板**前**最小化一个非焦点窗口（收集路径），按两下 Tab 走到它、松手恢复并聚焦。每个关键点抓一次 `self_check`，轨迹断言 4×`begin n=3` + 4×commit + 2×`restore` + 1×`min n=3` + **0 次 drop** |

探针带 `--hold-ms N`，面板会停住 N 毫秒，方便截图或人工看一眼；两个停点分别是第一轮刚打开（看列表本身）和第三轮最小化之后（看"已最小化"那行）：

```bash
XDG_RUNTIME_DIR=$(mktemp -d) WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  WOODLAND_SHORTCUTD=off ./build/src/woodland &
WOODLAND_IPC=$XDG_RUNTIME_DIR/woodland-wayland-0.sock ./build/tests/switcher_probe --hold-ms 5000 &
WAYLAND_DISPLAY=wayland-0 grim /tmp/switcher.png
```

> 虚拟键盘必须**自己发 `modifiers` 事件**：wlr 的 virtual-keyboard 后端把 `key` 事件的
> `update_state` 置成 `false`，xkb 状态只由 `modifiers` 请求驱动。少发这一条，合成器那边
> 修饰位恒为 0，`Alt+Tab` 根本进不到键位表（探针第一版就卡在这里）。
>
> **`unmin` 这条分支没有端到端覆盖**：Wayland stable `xdg-shell` 有意只提供
> `set_minimized`、不提供 `unset_minimized`（恢复最小化由合成器或任务栏经 `foreign-toplevel` /
> `xdg-activation` 触发），客户端没法通过 `xdg-shell` 自己撤销最小化。
> 所以 `unmin` 靠单测（直接改字段 + 补发 `view_changed`）；端到端只钉"提交时由合成器恢复"。

## 8. 已知取舍

- **不做缩略图/图标**：只画标题。目标是"看得清在切谁"，不是复刻 treeland 的 switch-window。
- **不做实时预览**：切换期间不把高亮窗口临时提到最上面（labwc 的 `preview`）。少一次 z 序抖动，也少一套"临时 reparent 再还原"的还原逻辑。
- **最小化窗口进列表，但只是"进列表"**：面板上标 `(已最小化)` 并压暗，提交等于"恢复 + 聚焦"。
  没做的是"最小化一个窗口后自动把它从列表里隐形"那种语义（Windows 的 Alt+Tab 也保留它们），
  也没有"恢复后落到原来的位置"——`focus_view(raise=true)` 会把它抬到最上面，和点任务栏一致。
- **候选上限 32**、可见 9 行。上限之外（真的开了 33 个窗口）的那些窗口这一轮切不到，下一轮从新的 z 序重新拍快照。
- **单候选时 `Alt+Tab` 仍会弹一下**，面板里只有它自己，松手后焦点不变。和 labwc 一致：不特判，省一条分支。
