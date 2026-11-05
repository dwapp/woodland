# treeland-decoration-unstable-v1 支持设计规格

> 日期：2026-11-05
> 状态：已实现（协议语义完整；圆角真的画出来——scenefx 常开，能不能画由
> `[effects] enabled` 决定，阴影仍只记录，见 §4）
> 参考实现：treeland `src/modules/decoration/decorationmanagerinterfacev1.cpp`
> （Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only）
> 协议定义：`treeland-protocols/public/treeland-decoration-unstable-v1.xml`（MIT）

## 1. 背景与目标

treeland 生态里，客户端（尤其是 dde-shell 系的标题栏实现）会通过
`treeland_decoration_manager_v1` 拿一个 per-surface 的
`treeland_decoration_context_v1`，按窗口微调**合成器画的**服务端装饰：圆角、
阴影、边框、标题栏可见性。典型用法是"半 CSD 半 SSD"——保留合成器的边框/
圆角/阴影，把标题栏藏掉，自己在窗口内容里画一条。

woodland 的目标是协议兼容 treeland 生态，所以这个协议必须接。本项目已有的
SSD（`src/ssd/`）正好覆盖了其中两项：边框（scene rect 宽度 + 颜色）与标题栏
（`include/ssd.h` 的边距/命中/排版都从 `ssd_titlebar_height()` 派生）。

## 2. 非目标（显式砍掉）

- **不改装饰模式**。CSD/SSD 的协商归 xdg-decoration（`src/shell/shell.c` 的
  `deco_apply_mode`），本协议只调"已决定的 SSD 长什么样"。
- **不改几何**。摘要说得很清楚："takes effect immediately; there is no
  pending/commit model"。所以覆盖值不进 configure、不触发重新放置：内容原点
  不动，边距变了只是外框跟着变（下一次 `woodland_view_outer_box()` 自然读到）。
- **不新增配置项**。四个属性只来自协议，配置里的 `[decorations]` 仍是无覆盖
  时的默认值。
- **圆角与阴影不渲染**（见 §4）。
- 不实现"每个窗口多套装饰"或动画过渡。

## 3. 数据模型

### 3.1 覆盖值放 view 上，不放 SSD 里

```c
/* include/ssd.h */
struct woodland_ssd_overrides {
    bool managed;              /* 有存活的 context（wctl 可见） */
    bool titlebar_set; bool titlebar_hidden;
    bool border_set;   int border_width; uint32_t border_rgba; /* 0xRRGGBBAA */
    bool corner_radius_set; int corner_radius;
    bool shadow_set;   int shadow_radius, shadow_dx, shadow_dy; uint32_t shadow_rgba;
};
```

`struct woodland_view` 里按值内嵌一份（`view->ssd_overrides`）。理由：

1. 客户端在 **map 之前**（甚至 xdg-decoration 协商之前）就会建 context 并发
   请求，而 SSD 对象要到 `woodland_view_map()` 才创建；放 view 上，SSD 创建
   时自然读到，都不用"补一次应用"。
2. `woodland_ssd_get_margins()` 在 map 之前就要能算（放置、X11 configure 都
   依赖它），它同样只能读 view。

`ssd.c` 里三个派生函数各读一处，其余全部自动跟随：

| 属性 | 读它的地方 | 效果 |
|---|---|---|
| `titlebar_hidden` | `ssd_titlebar_height()` | 边距 top 归零、标题栏/按钮/图标/文字全隐、命中区域消失 |
| `border_width` | `ssd_border_width()` | 边距、四边矩形、抓取宽度一起变 |
| `border_rgba` | `ssd_get_border_color()` | 激活/非激活同色（协议只给一个颜色） |
| `corner_radius` / `shadow_*` | 无（只经 wctl 暴露） | 不改画面 |

### 3.2 协议对象

```
woodland_proto_decoration
  ├ global: treeland_decoration_manager_v1
  └ contexts: [woodland_deco_context ...]
        ├ resource, surface
        ├ view（可为 NULL，惰性解析）
        ├ inert
        ├ surface_destroy 监听（wl_surface 消失 → inert）
        └ toplevel_destroy 监听（view 被 free 之前/之后 → 只清指针）
```

### 3.3 模块依赖

`protocol → ssd`（调 `woodland_ssd_deco_changed()`），**shell 不需要知道这个
协议存在**。方向是单向的：协议写 `view->ssd_overrides`，SSD 自己读。

## 4. 渲染能力与取舍

协议允许合成器"按自己的策略处理，并把越界值 clamp 到支持的上界"。woodland
的渲染能力是编译期固定的（scenefx 常开），画不画由 `[effects] enabled` 决定：

| `[effects] enabled` | 场景库 | 圆角 | 阴影 |
|---|---|---|---|
| `true`（默认） | scenefx 0.5（自带 fx_renderer） | 真画 | 只记录 |
| `false` | 同一套 scenefx，只是不建节点 | 只记录 | 只记录 |

- **圆角**由 `src/effect/effect.c` 落到 scenefx：
  客户端内容用 `wlr_scene_buffer_set_corner_radii()`，SSD 的标题栏与边框用
  `wlr_scene_rect_set_corner_radii()`。四角怎么分配见
  `woodland_effect_corners()`：有标题栏 → 内容上角归 0（被标题栏盖住），
  标题栏/上边框自己圆；最大化/全屏 → 一律直角。
  半径只来自 `[effects] corner_radius` 或本协议的 per-view 覆盖，二者都 clamp
  到 `WOODLAND_SSD_MAX_SIZE`；总开关关掉时按 0 算。
- **阴影**仍不画：它不只影响像素，还得算进摆放几何（否则相邻窗口互相压边），
  会牵动 placement/snap/maximize 全链路。

"没画"这件事必须能看出来，所以开关两侧都不说谎：

- `wctl get_tree` 的 `deco.rendered` 只列真正改变画面的属性。特效开着时是
  `["border","titlebar","corner_radius"]`，关掉后没有 `corner_radius`；
- `--trace` 的 `deco` 条目在总开关关掉时写 `recorded (effects disabled)`，
  开着不写这句（否则脚本会以为没生效）；
- 协议里的 `set_*` 完整校验（上界 1000、颜色 [0,255]），与开关无关。

## 5. 生命周期与错误

| 情形 | 行为 |
|---|---|
| `surface` 不属于请求方 | `invalid_surface`（`wl_display.error`） |
| 同一 surface 第二个 context | `already_used` |
| context 早于 toplevel / map 创建 | view 惰性解析：请求到来时再试一次 |
| `wl_surface` 销毁 | context 变 inert，请求静默忽略 |
| 视图销毁（toplevel/xsurface destroy） | 只清 `ctx->view`，此后请求是空操作 |
| context `destroy` | 边框/圆角/阴影恢复默认，**标题栏模式保留** |

最后一条同 treeland 的取舍：标题栏可见性叠着 xdg-decoration 的协商结果，
context 销毁时盲目恢复会把客户端刚谈下来的 CSD 变成"又长出标题栏"。客户端想
回默认就再发一次 `set_titlebar_mode(show)`。

**关于视图指针的悬挂**：`context_handle_toplevel_destroy()` 与 woodland 自己的
销毁监听器挂在同一个 `wl_signal` 上，而后者注册得更早（`new_toplevel` 时），
所以走到本回调时 view 已经 free 了——回调里**不读 view**，只清指针并摘监听。
摘监听是必须的：信号所属对象正在销毁，留着就是悬挂链表节点。

## 6. 测试

- `tests/unit/test_ssd.c`：覆盖值与几何/命中的关系（隐藏标题栏后 top 只剩
  边框、边框宽度进边距、0 宽 + 隐藏 = 无装饰、最大化下两者叠加、恢复默认）。
- `tests/smoke/run_deco_smoke.sh` + `tests/smoke/deco_probe.c`：真客户端 + 真
  合成器，用 `wctl get_tree` 断言 `deco` 与 `ssd.margins`；覆盖 6 个场景
  （无 context / border / 隐藏标题栏 / 半 CSD / 零边框 / 圆角阴影）以及
  `already_used` 负例；最后断言 `self_check` 与 `deco` 轨迹。

冒烟里刻意 **不数像素**：隐藏标题栏的效果就是边距变化，`jq '.views[0].ssd.margins'`
比截图可靠，也不会因为字体渲染差异假失败。

`invalid_surface` 那条错误没有进冒烟：它要求"把别人家的 surface 传进来"，而
Wayland 的资源 id 是连接私有的，单个客户端构造不出这种请求（只有伪造/损坏的
客户端能触发），所以只做了代码路径，没有回归用例。

## 7. 验收标准（DoD）

1. `meson test -C build` 全绿，含新增 `ssd` 单测断言与 `smoke-decoration`。
2. `wctl get_tree` 里能看到 `deco{managed,titlebar,border_width,border_rgba,
   corner_radius,shadow_*,rendered[]}`。
3. `set_border(5,...)` 后 `ssd.margins` 从 `[1,27,1,1]` 变 `[5,31,5,5]`。
4. `set_titlebar_mode(hide)` 后 `ssd.margins == [1,1,1,1]`，且内容原点上方
   不再可拖动（单测断言命中为空）。
5. 同一 surface 第二次 `get_decoration_context` 返回 `already_used`（error 0）。
6. `--self-check` 全程无失败项。
