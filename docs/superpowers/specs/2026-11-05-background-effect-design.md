# ext-background-effect-v1 与圆角渲染设计规格

> 日期：2026-11-05
> 状态：已实现（协议 + 渲染；scenefx 路径需要 `-Deffects=enabled`）
> 协议定义：wayland-protocols ≥ 1.41 `staging/ext-background-effect/ext-background-effect-v1.xml`
> 参考实现：`/home/ut004971@uos/work/doors`（wlroots 0.21 内建同名协议 + 自研 shader）、
> treeland vendored 的 wlroots MR5304

## 1. 背景与目标

半透明窗口（终端、侧边栏、通知中心）想要"身后这块糊掉"，而不是靠客户端自己
画一张假装模糊的图。`ext-background-effect-v1` 就是这件事的标准协议：
客户端说"我这块 surface 的这块区域要模糊"，合成器在画这个 surface 之前先把它
身后已经上屏的内容采样糊一遍。

woodland 的目标：

- 协议族自己实现（woodland 用 wlroots 0.20，还没有内建这个协议）；
- 真的画出来——走 scenefx 0.5 的 blur 节点；
- 顺带把上一轮"只记录不渲染"的圆角补齐（同一个 renderer 切换，一起做完才
  谈得上"看起来是同一个东西"）。

doors 的实现不能照搬：它跑在 wlroots 0.21，协议与 blur 渲染都是上游自带的；
woodland 的 0.20 两样都没有。所以拆成两半：**协议按上游语义自己写**，
**像素交给 scenefx**。

## 2. 非目标（显式砍掉）

- **不做"自动模糊"**：不给所有半透明窗口自动加模糊，也不看 alpha 猜意图。
  只有客户端显式 `get_background_effect` + `set_blur_region` 才生效。
- **不做阴影**（理由见 treeland-decoration 规格 §4：要动摆放几何）。
- **不把 blur 参数开放成配置项**：遍数/半径写死在 `wlr_scene_set_blur_data()`
  （3 遍 downsample、半径 5），先用起来再说。
- **不做多输出/多 scene buffer 的模糊一致性调优**，交给 scenefx。

## 3. 协议实现（`src/protocol/background_effect.c`）

### 3.1 状态必须双缓冲

`set_blur_region` 是 pending 请求，下一次 `wl_surface.commit` 才生效。不自己
记 commit 序号：用 `wlr_surface_synced`（wlroots 0.20 的 surface 同步助手），
pending/current 两份 `struct woodland_bg_state { pixman_region32_t blur_region; }`
由它负责 init/move/finish。这和上游 MR5304 同一条路。

surface 销毁时按 `wlr_addon` 摘挂（`struct woodland_bg_surface` 挂在
`surface->addons` 上），保证"对象比 surface 活得久"的客户端不会踩到野指针。

### 3.2 提交时才写 view 状态

`bg_state_commit` 里做两件事：

1. 把 current region 交给 `woodland_effect_set_blur_region(view, region, w, h)`；
   区域在这里就裁到 surface 尺寸（`woodland_effect_blur_box()`），越界部分丢掉；
2. 记一条 `bg_effect` 轨迹（`blur=on|off`），脚本靠它断言"提交真的被处理了"。

`view` 还没解析出来（surface 先于 toplevel）时静默跳过——下一次 commit 会再来。

### 3.3 capability 按构建如实报

`capabilities` 在 bind 时发一次。没有 scenefx 的构建发 `0`（"我不会模糊"），
而不是"接了请求但什么都不发生"：客户端据此决定要不要保留半透明外观。
这一点被 `smoke-bg-effect` 的 CAPS 阶段钉住（effects → 1，非 effects → 0）。

## 4. 渲染（`src/effect/effect.c`，仅 effects 构建）

### 4.1 节点位置：贴 surface，不贴 scene_tree 原点

blur 节点是 `view->scene_tree` 的子节点，但 `scene_tree` 的原点是**内容原点**
（SSD 的标题栏/边框在负坐标上）。所以节点位置要取内容 buffer 节点的 `x/y`，
不能拿 `xdg_toplevel->base->geometry` 的 `x/y` 当位置——CSD 客户端会把阴影算进
geometry，那样会整体偏移。

### 4.2 裁剪区域用 surface 局部坐标

`clipped_region.area` 是**节点局部坐标**。节点已经贴在 surface 原点上，所以
协议给的 region 直接照搬，不要减任何原点。盖满整个内容时给
`clipped_region_get_default()`（空 = 不裁剪），避免多一次无谓的裁剪计算。

### 4.3 圆角的四角分配

`woodland_effect_corners()` 把"一个半径"摊到四个角：

| 情形 | 上角 | 下角 |
|---|---|---|
| 有标题栏（SSD，且未被协议隐藏） | 0（被标题栏盖住） | 半径 |
| 无标题栏 / CSD | 半径 | 半径 |
| 最大化 / 全屏 | 0 | 0 |

blur 节点也吃同一份四角：模糊区域自己也要圆，否则圆角窗口的模糊会漏出直角。

### 4.4 渲染器必须换

scenefx 的 `wlr_scene_*` 会顶替 wlroots 的实现（同名符号，链接顺序
`scene_deps = [scenefx, wlroots]`），而它的 `wlr_scene_output_commit()` 直接把
render pass 强转成自己的 `fx_render_pass`。所以 effects 构建里
`server->renderer` 必须是 `fx_renderer_create()` 的产物；塞一个 pixman/gles2
的 wlroots renderer 进去会在 `fx_get_render_pass` 里断言死。

## 5. 两条构建互不污染

| 能力 | 默认构建 | effects 构建 |
|---|---|---|
| `ext_background_effect_manager_v1` 全局 | 有 | 有 |
| capability | 0 | blur |
| `view->effect.blur` / region | 记录 | 记录 + 画 |
| 圆角 | 记录 | 记录 + 画 |
| `dump_scene` 里的 blur 节点 | 无 | 有 |
| `wctl get_tree` 顶层 `effects` | false | true |

`#if WOODLAND_HAVE_EFFECTS` 只切"画不画"，不切"记不记"：单测、IPC、trace 在
两种构建下行为一致，脚本不需要到处分支（分支点集中在"渲染结果"上）。

## 6. 测试

- `tests/unit/test_effect.c`：半径优先级（覆盖 > `[effects] corner_radius`）、
  clamp、四角分配、region 裁剪（空/越界/全外/负坐标）、提交状态。
- `tests/unit/test_session_lock.c`：effects 构建里必须用 `fx_renderer_create()`，
  否则 scenefx 断言；没有 DRM 节点的机器上 skip（77），不伪装成通过。
- `smoke-bg-effect`：capability、整窗/局部/越界/无对象/销毁五个场景的协议状态，
  外加 **scene 节点断言**（effects 构建必须有铺满内容的 blur 节点，默认构建
  必须没有）。
- `smoke-xwayland`：标题栏底色断言改成 `±1%` 容差——wlroots renderer 与
  fx_renderer 在 sRGB 舍入上差一个最低位（247 vs 246），这不是画错了。

## 7. 已知限制

- blur 的观感取决于 GPU/驱动，冒烟只断言"节点在、区域对"，不断言像素。
  人工验证用「底层彩条窗口 + 半透明窗口 + `blur`」两张对照截图看边缘能量。
- `set_blur_region` 只取区域的**包围盒**（`pixman_region32_extents`），不做
  逐矩形的多块裁剪。多矩形区域的中间空洞会被一起糊——协议允许，先记着。
- `[effects] enabled/blur/shadow` 仍是预留字段，别当成生效开关。
