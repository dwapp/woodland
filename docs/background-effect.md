# ext-background-effect-v1 与圆角渲染设计规格

> 日期：2026-11-05
> 状态：已实现（协议 + 渲染；编译期默认开启 scenefx，模糊由 `[effects]` 运行期开关控制）
> 协议定义：wayland-protocols ≥ 1.41 `staging/ext-background-effect/ext-background-effect-v1.xml`
> 参考实现：`/home/ut004971@uos/work/doors`（wlroots 0.21 内建同名协议 + 自研 shader）、
> treeland vendored 的 wlroots MR5304；`/home/ut004971@uos/work/mango`（scenefx 用法对照，
> 见 §8）

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

### 3.2 提交时按目标分发

`bg_state_commit` 把 current region 推给渲染端，目标有两类：

| surface 角色 | 处理 |
|---|---|
| xdg toplevel / xwayland 窗口 | `woodland_effect_set_blur_region(view, ...)`，状态记在 view 上 |
| layer surface（面板、Dock、启动器） | `woodland_effect_layer_blur(layer, box)`，blur 节点挂在它自己的场景树上 |
| 其他（cursor、输入法） | 记一条 `ignored` 轨迹，不假装生效 |

区域统一先过 `woodland_effect_blur_box()` 裁到 surface 尺寸，越界部分丢掉。
每类都记一条 `bg_effect` 轨迹（`blur=on|off` / `layer blur=on|off`），脚本靠它
断言"提交真的被处理了"。

layer surface 这条路径不是可选项：面板是模糊最常见的用户，而它们没有 view
对象——只认 view 的话，客户端设了区域什么都不发生，还查不出原因。

### 3.3 capability 按运行期配置如实报

`capabilities` 在 bind 时**现算**（全局在配置加载之前就建好了，不能读初始化
时的快照）：`[effects] enabled` 且 `blur` 时发 `BLUR`，否则发 `0`（"我不会模
糊"），而不是"接了请求但什么都不发生"——客户端据此决定要不要保留半透明外观。
`reload_config` 改了开关会再广播一次（协议允许重复发送，以最后一条为准）。
这一点被 `smoke-bg-effect` 的 CAPS 阶段钉住（开 → 1，关 → 0）。

## 4. 渲染（`src/effect/effect.c`）

### 4.1 节点位置：贴 surface，且用 tree 的坐标系

blur 节点是 `view->scene_tree` 的子节点，`wlr_scene_node_set_position()` 收的是
**相对父节点**的坐标；而 `view->scene_tree` 的原点已经是内容原点（SSD 的标题栏
和边框画在负坐标上）。两件事叠起来有个坑：

- 位置要取内容 buffer 在 tree 内的偏移（不是 `xdg_toplevel->base->geometry` 的
  `x/y`，CSD 客户端会把阴影算进 geometry，那样会整体偏移）；
- 这个偏移必须**从 tree 的子节点开始算**，不能把 `view->scene_tree->node.x/y`
  自己加进去——那是 tree 相对场景根的位置，加上去等于把标题栏高度算两遍，
  模糊会整体下沉一个标题栏。这个 bug 真的发生过，靠 `dump_scene` 里
  `blur pos` 与 `buffer pos` 对不上才逮到（差 32px = 标题栏高度）。

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

但"一个窗口"往往不止一块 buffer：客户端用 subsurface 放视频、放输入法候选框、
放自带子窗口时，那些 buffer 画在父 surface **之上**，会把父 buffer 的圆角重新
盖成直角——右下角一块全屏大小的 subsurface，窗口右下就露一个尖角。所以圆角是
**逐 buffer** 下发的（`woodland_effect_buffer_corners()`）：只有自己贴到窗口
那条边的角才跟着圆，中间的 buffer 一律直角。

### 4.4 只在客户端画过的地方糊

`set_blur_region` 是客户端声明的区域，而客户端声明得比画得大很常见：CSD 窗口的
surface 里有一圈"阴影预留"的全透明边，把整个 surface 声明成模糊区域时，那一圈
会在窗口外面糊出一圈毛边。所以 blur 节点带一个透明度遮罩
（`wlr_scene_blur_set_transparency_mask_source()`，指到内容 buffer），scenefx 用
它打一张 stencil，完全透明的像素不画模糊。

判据是二值的（`discard_transparent` 写 stencil），不是按 alpha 加权，所以半透明
窗口不会被"压暗"——它只是把"没画东西的地方"排除掉。实测：窗口边缘留 40px
全透明，模糊前后这两张图在那一圈的 RMSE 为 **0**，而中心区域差 0.043。

### 4.5 渲染器必须是 fx（且不能回退 pixman）

scenefx 的 `wlr_scene_*` 会顶替 wlroots 的实现（同名符号，链接顺序
`scene_deps = [scenefx, wlroots]`），而它的 `wlr_scene_output_commit()` 直接把
render pass 强转成自己的 `fx_render_pass`。所以 `server->renderer` 必须是
`fx_renderer_create()` 的产物；塞一个 pixman/gles2 的 wlroots renderer 进去会在
`fx_get_render_pass` 里断言死。

fx renderer 只走 EGL/GLES2，没有 DRM 渲染节点（容器、纯 headless）时第一把必然
失败。这时 `server.c` 会设 `WLR_RENDERER_FORCE_SOFTWARE=1` 重试一次，落到
llvmpipe 的 surfaceless EGL——**启用 effects 时唯一的降级方向**，回退到 pixman
会直接崩。没有 GLES2 可用的环境（如 CI 无 GPU）则用 `-Deffects=disabled` 整体
退回 wlroots 直出，特效代码不参与链接。

## 5. 一套构建 + 运行期开关

| 能力 | `[effects] enabled=false` | `true`（默认） |
|---|---|---|
| `ext_background_effect_manager_v1` 全局 | 有 | 有 |
| capability | 0 | blur（`blur=false` 时也是 0） |
| `view->effect.blur` / region | 记录 | 记录 + 画 |
| 圆角 | 记录（半径按 0 算） | 记录 + 画 |
| `dump_scene` 里的 blur 节点 | 无 | 有 |
| `wctl get_tree` 顶层 `effects` / `effects_blur` | false / false | true / 按配置 |

配置只切"画不画"，不切"记不记"：单测、IPC、trace 在开关两侧行为一致，脚本的
分支点集中在"渲染结果"上（`get_tree` 的 `effects`、`dump_scene` 的 blur 节点）。

## 6. 测试

- `tests/unit/test_effect.c`：半径优先级（覆盖 > `[effects] corner_radius`）、
  clamp、四角分配、region 裁剪（空/越界/全外/负坐标）、提交状态、逐 buffer
  圆角的边判定（全窗口 / 中间 / 左上 / 右下 / 整条边 / 越界 / 上角本来就是 0），
  以及运行期开关的优先级（`enabled=false` 压过客户端覆盖与配置半径、
  `blur=false` 只停模糊不停圆角、空 server/配置按关处理）。
- `tests/unit/test_session_lock.c`：必须用 `fx_renderer_create()`，否则 scenefx
  断言；没有 DRM 节点时先按 llvmpipe 重试，仍不行才 skip（77），不伪装成通过。
- `smoke-bg-effect`：跑两遍。默认配置下断言 capability=1、整窗/局部/越界/
  无对象/销毁五个场景的协议状态，外加 **scene 节点断言**（必须有一个铺满内容、
  四角跟内容一致的 blur 节点）；`--layer` 场景断言 layer surface 的模糊会建出
  铺满它、四角为 0 的节点，surface 销毁后节点跟着消失。第二遍用
  `[effects] enabled=false` 重启，断言 `get_tree` 的 `effects`/`effects_blur`
  为 false、capability=0、一个 blur 节点都没有，**但客户端设的区域照样记录**。
- `smoke-xwayland`：标题栏底色断言改成 `±1%` 容差——wlroots renderer 与
  fx_renderer 在 sRGB 舍入上差一个最低位（247 vs 246），这不是画错了。

## 7. 已知限制

- blur 的观感取决于 GPU/驱动，冒烟只断言"节点在、区域对"，不断言像素。
  人工验证用「底层彩条窗口 + 半透明窗口 + `blur`」两张对照截图看边缘能量
  （嵌套跑，别占 DRM）。
- `set_blur_region` 只取区域的**包围盒**（`pixman_region32_extents`），不做
  逐矩形的多块裁剪。多矩形区域的中间空洞会被一起糊——协议允许，先记着。
- `[effects] shadow` 仍是预留字段；`enabled` / `blur` 已经接线（默认 true）。
- 运行期开关改 `enabled`/`blur` 后，已有窗口要等下一次 commit 才重画；
  `reload_config` 会立刻遍历窗口重算并重播 capability。

## 8. 与 mango 的对照（代码级）

mango 也用 scenefx，但它的模糊是**配置驱动**（`blur`/`blur_layer`/规则里的
`no_blur`），woodland 是**协议驱动**（客户端 `set_blur_region`）。两者取舍不同，
下面是逐条对照——抄了什么、为什么不抄剩下的：

| mango 的做法 | woodland | 理由 |
|---|---|---|
| 客户端创建时就建 blur 节点，之后只改 size/pos/enabled | 懒建，建好就留着，只切 `enabled` | 结果一样；woodland 里多数窗口根本不会请求模糊，没必要每个 view 都挂一个节点 |
| 逐个 buffer 下发圆角（`wlr_scene_node_for_each_buffer`） | 照着做，但按"贴边"判断 | mango 给每个 buffer 同一份四角，中间的小 subsurface 会被圆成一个胶囊；woodland 只圆贴窗口边的角 |
| `wlr_scene_blur_set_transparency_mask_source()` 给 layer 用 | 所有模糊都用 | 见 §4.4，这是"声明得比画得大"时唯一不露毛边的做法 |
| layer surface 也模糊（非 BACKGROUND/BOTTOM 层） | 照做，但只在客户端显式请求时 | 面板是模糊的主要用户；区别是 mango 无条件糊，woodland 要协议 |
| `should_only_blur_bottom_layer`（"xray"，只糊壁纸） | **不做** | 这个开关在 scenefx 里走的是 optimized blur 的离屏缓冲，那个缓冲只由 `wlr_scene_optimized_blur` 节点（整屏一块）填充。woodland 没有这种节点，直接打开开关等于去采样一块没初始化的 buffer。要做就得先引入 mango 那套"整屏背景模糊层"，属于另一个特性 |
| `blur_params_*`（遍数/半径/噪声/亮度/对比度/饱和度）可配、改配置时 `reset_blur_params()` 重建 | 仍写死（3 遍、半径 5） | 见 §2 非目标；参数化要先想清楚配置面（`[effects]` 已有 4 个字段），不是这一轮的题目 |
| 窗口级 config 开关 `no_blur` + 规则 | 不做 | woodland 没有窗口规则系统，协议本身就是"逐窗口开关" |
| 阴影（`wlr_scene_shadow_create`，带圆角） | 不做 | 见 treeland-decoration 规格 §4：阴影要算进摆放几何 |

结论：值得抄且已经抄的三条是**逐 buffer 圆角**、**透明度遮罩**、**layer surface
支持**；`should_only_blur_bottom_layer` 和模糊参数化看着诱人，但都依赖 woodland
还没有的东西，硬抄会得到一个"配置生效了、画面更糟"的结果。

## 9. 与 swayfx / mango 的对照（圆角边框）

SSD 边框原先是四条 1px 矩形首尾相接的方框，圆角只给上下两条设了
`corner_radii_top/bottom`、左右竖条干脆保持直角。细条宽/高只有 1px，scenefx
的 `fx_render_pass_add_rounded_rect()` 会把半径按 rect 尺寸 clamp 掉：实际画面
上标题栏圆角外没有描边、下边框在角上仍是直的，右下角还戳出一小截竖线。
"窗口圆了、边框还是方的"就是这么来的。

swayfx `sway/desktop/transaction.c` 把上下边框 rect 加高到
`border_thickness + corner_radius`，外圆角设 `corner_radius + border_width`，
再用 `clipped_region` 挖掉内框（内圆角 `corner_radius`）；左右竖边框两端各
缩进一个半径，只画直边段。mango `src/draw/text-node.c:rect_apply_border()`
更直接：一个 rect 设外圆角、挖空内框，得到一圈等宽描边。

woodland 采用 mango 的整框挖空（单一 `frame` rect，见
`src/ssd/ssd.c:ssd_apply_frame_corners()`）+ swayfx 的半径换算（外 `R+b` /
内 `R`，弧线线宽才均匀）。**没有抄** swayfx 的竖边缩进——整框挖空后角部已由
描边连续覆盖，再缩进反而留缺口；也没有抄它 `-1px` 的位移 hack
（swayfx issue #386），那是把上下边框拆成两个 rect 时对齐产生的误差。

验收：`smoke-decoration` 断言 `dump_scene` 里只有一个 `corners == [R+b]*4`
的 rect、其 `clip.corners == [R]*4`、且不再有 1px 细条；嵌套截图四角放大可见
描边沿弧线连续、无直角尖角。
