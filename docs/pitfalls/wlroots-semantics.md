# wlroots 语义与时序陷阱

本文档总结在 Woodland 开发与扩展协议过程中，因 wlroots 内部实现细节、信号分发机制以及 Wayland 核心时序约束所引发的典型陷阱。

---

## 1. 信号分发与对象生命周期

### `wlr_idle_inhibitor_v1` 先发射 destroy 信号，后从链表中移除
- **现象**：当最后一个空闲抑制器（idle inhibitor）关闭时，合成器的空闲状态机未能恢复，显示器超时后永久不灭屏。
- **原因**：在 wlroots 的 `types/wlr_idle_inhibit_v1.c` 中，`idle_inhibitor_v1_destroy()` 的执行次序为：
  ```c
  wl_signal_emit_mutable(&inhibitor->events.destroy, inhibitor);
  wl_list_remove(&inhibitor->link);
  ```
  在 destroy 信号回调被触发的瞬间，该 inhibitor 尚未从 `manager->inhibitors` 链表中解绑。若直接通过 `wl_list_empty(&manager->inhibitors)` 判断是否已无抑制器，条件判定将永远为 false。
- **解决规则**：聚合状态时应维护合成器自有的 inhibitor 链表，或使用 `wl_list_length(&manager->inhibitors) > 1` 判定（参考 `src/protocol/idle.c`）。回归测试见 `meson test -C build smoke-idle-inhibit`。

### `wl_signal` 监听器按注册顺序同步调用，顺序即行为
- **现象**：跨 surface 移动光标时，相对光标位移（relative pointer）的第一帧增量丢失，或被误发给刚刚失去焦点的旧 surface。
- **原因**：在 `server->cursor->events.motion` 信号上，协议层（发射 relative-pointer）与 shell（更新指针焦点）均注册了回调。若协议层的回调先于 shell 回调注册，分发相对位移时使用的依然是旧的焦点目标。
- **解决规则**：
  1. `woodland_cursor_init_events()` 必须严格在 `woodland_protocol_init()` 之前完成注册（见 `src/server/server.c`）。
  2. 新输出的热插拔补广播监听器必须注册在 `handle_new_output` 之后。
  3. **禁止直接手写 `wl_signal_add`**：新监听器一律通过 `woodland_listener_connect()`（`util/listener.h`）进行连接，确保监听器名称与注册序号进入调试账本，可通过 `wctl get_listeners` 随时审查。

### 协议扩展对象严禁在 destroy 回调中反查 `struct woodland_view *`
- **现象**：客户端关闭窗口时合成器发生偶发性 Use-After-Free 或段错误。
- **原因**：客户端在销毁 toplevel 表面时，挂在 `xdg_toplevel->events.destroy` 或 `xwayland_surface->events.destroy` 上的监听器执行顺序取决于注册时机。Shell 层在窗口创建阶段（`new_toplevel`）最先注册监听器，并在其 destroy 回调中执行了 `view_destroy(view)` 释放了视图结构体。协议层后续注册的 destroy 监听器被唤醒时，`woodland_view` 内存已被释放。
- **解决规则**：协议层上下文（如 decoration、foreign-toplevel 等）中的 destroy 回调只能清理自身状态、解除监听器（`wl_list_remove`）并将指针置 NULL，**绝对不许解引用 view**（参考 `src/protocol/decoration.c:context_handle_toplevel_destroy()`）。

---

## 2. 输出与显示配置管理（wlr-output-management）

### `output-management` 的 test 请求严禁产生任何副作用
- **现象**：使用 `wlr-randr --dryrun` 测试显示器位置调整后，虽然未执行应用，但屏幕实际显示位置发生了错乱。
- **原因**：协议规范要求 `test` 请求仅校验新配置硬件与后端是否支持，不得修改当前运行状态。若在 test 阶段直接调用了 `wlr_output_layout_add/remove`，等于提前修改了合成器的逻辑布局。
- **解决规则**：test 仅调用 `wlr_backend_test()` 校验；真正的布局变更必须在 `commit` 成功后才可执行。多输出配置必须使用 `wlr_backend_test()` + `wlr_backend_commit()` 进行整体验证与提交，严禁逐个调用 `wlr_output_commit_state()`（后者在中途输出失败时无法原子回滚）。

### apply 的 `succeeded` 事件必须先于新一轮 `configuration` + `done`
- **现象**：执行显示配置切换后，客户端（如 kanshi、wdisplays）陷入循环震荡，误认为合成器又产生了一轮外部配置变更。
- **原因**：协议规范明确指出：“若配置变更，变更事件必须紧跟在 `succeeded` 之后”。在 `apply_layout()` 内部修改 `wlr_output_layout` 会触发布局变更信号，而该信号上挂有配置广播逻辑。若在写布局前未抑制广播，客户端会先收到新 configuration 事件，后收到本次请求的 succeeded 事件。
- **解决规则**：`server->proto->output->applying` 标志位的抑制范围必须持续覆盖至 `wlr_output_configuration_v1_send_succeeded()` 发送完毕，之后再统一触发补发广播。
- **拔除输出的处理**：输出被拔除时，已通过 `--off` 禁用的输出并不在 layout 树中，销毁它不会触发 layout change 信号。必须通过 `server->output_removed` 专用信号向协议层显式推送新一轮配置广播。

---

## 3. 全局缩放（Scale）刷新入口收敛

### 缩放变更的影响范围与统一刷新入口
- **影响范围**：缩放变化不仅影响输出渲染，还深度联动四个关键子系统：
  1. 光标主题不同 scale 的加载（`wlr_xcursor_manager_load`）。
  2. scenefx 的圆角与模糊缓存重置。
  3. 服务端装饰（SSD）按钮与标题缓存（cairo device scale 改变，图标缓存失效）。
  4. X11 窗口输出缩放滤波（根据当前所在输出的 scale 选择 nearest 或 bilinear）。
- **触发路径的离散性**：修改 scale 存在三条**完全不相交**的代码路径：
  - 后端直接发起的 `request_state`（例如从其他 TTY 切换回本合成器）。
  - `wlr-output-management` 协议发起的 apply（走 `wlr_backend_commit`，不触发 `request_state`）。
  - SIGHUP 配置重载修改了配置文件的 `[[output]] scale`。
- **解决方案与闸门机制**：
  上述三条路径在物理渲染上最终均汇聚于 `wlr_output.events.commit`。因此，在 `src/output/output.c` 中通过 `handle_commit()` 监听器比对 `output->last_scale`，仅在 scale 确实变更时统一调用 `woodland_output_refresh_all()`。
  - **关键约束**：commit 事件每帧都会触发，`output->last_scale` 的变更检测闸门**绝对不能省略**，否则会导致每一帧都重建全屏所有窗口的 SSD 缓存。
  - **X11 覆盖式窗口（unmanaged）**：不受 `server->views` 链表管理，在 `woodland_output_refresh_all()` 中必须显式调用 `server->xwayland_refresh_hook` 触发联动重刷。
