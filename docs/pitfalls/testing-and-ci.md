# 构建、CI 与测试踩坑

本文档整理 Woodland 合成器在构建系统、持续集成（CI）容器环境、冒烟测试脚本及 Wayland 探针编写中的典型陷阱与既有经验。

---

## 1. CI 与构建系统陷阱

### CI 必须跑在 `archlinux:base-devel`，不要换回 `debian:stable`
- **现象**：在 Debian stable (bookworm) 镜像中构建时，Meson 会退到 subprojects wrap 中逐个编译 fallback，最终断在 `Dependency "expat" not found` 或其他底层库上。
- **原因**：wlroots 0.20 对底层依赖版本要求极高（Wayland >= 1.24、libdrm >= 2.4.129、xkbcommon >= 1.8、pixman >= 0.43），Debian bookworm 的系统包版本均不满足，补装 expat 只会将失败推迟到下一个不满足的依赖。
- **结论**：CI 环境必须保持 Arch Linux 镜像，不要尝试回退到 Debian stable。

### 顶层 `meson.build` 里给 `wlroots` 的 `declare_dependency` 包装不能删
- **现象**：删除顶层对 wlroots 的 `declare_dependency` 包装后，在未全局安装 wlroots 的干净机器上，`#include <wlr/types/wlr_layer_shell_v1.h>` 会报找不到 `wlr-layer-shell-unstable-v1-protocol.h`。
- **原因**：wlroots 作为 Meson 子项目构建时，`wayland-scanner` 生成的协议私有头输出在 `build/subprojects/wlroots/protocol/`，而 wlroots 原生 wrap 仅导出了 `include/`。若宿主机的 `/usr/include` 碰巧装过 wlroots，该问题会被掩盖（**本地能编不等于干净环境能编**）。
- **结论**：顶层中为 wlroots 追加包含目录的 `declare_dependency` 必须保留。

### CI 仅跑 `-Deffects=disabled` 是结构性限制
- **现象**：CI runner 上启用 effects 构建会直接死在 `failed to create allocator`。
- **原因**：scenefx 0.5 的 `fx_renderer` 仅声明了 `WLR_BUFFER_CAP_DMABUF` 缓冲能力。`wlr_allocator_autocreate` 分配器在无 `SHM|DATA_PTR` 支持时，仅能尝试 udmabuf 与 gbm。CI 容器环境通常没有 `/dev/dri` 渲染节点，也无 `/dev/udmabuf` 设备。即使设置 `WLR_RENDERER_FORCE_SOFTWARE=1` 启动了 llvmpipe，allocator 仍无法创建。
- **结论**：CI 仅在 `-Deffects=disabled`（pixman 直出）上运行全量冒烟测试，`-Deffects=enabled` 仅运行编译与 `--suite unit`。所有涉及 effects 的冒烟测试分支（圆角、阴影、模糊等）仍必须在本地具备 GPU/DRM 的环境中验证；场景级核心逻辑由不受此限制的单元测试（`test_effect.c`、`test_ssd.c` 等）兜底。

---

## 2. 冒烟测试运行与脚本编写陷阱

### 冒烟测试默认禁用 `woodland-shortcutd`
- **现象**：测试脚本中断言快捷键绑定数 `binding_count == 0` 或 `occupied`、`last_outcome` 时偶然出现非预期失败。
- **原因**：合成器启动时若自动拉起 `woodland-shortcutd`，该守护进程会通过 `treeland_shortcut_manager_v3` 独占 acquire 管理器并注册默认键位，破坏测试的不变量环境。
- **规范**：`tests/smoke/lib.sh` 中的 `smoke_init()` 统一将 `WOODLAND_SHORTCUTD=off` 写入环境。要测试 shortcutd 自身的拉起机制，请参考 `tests/smoke/run_shortcutd_smoke.sh`。

### 冒烟脚本规范：必须转储现场，严禁 `cat 日志; exit 1`
- **规范**：每个冒烟脚本必须遵循标准生命周期：
  ```bash
  . tests/smoke/lib.sh
  smoke_init <测试名>
  # ... 执行逻辑 ...
  smoke_cleanup() {
      # 自身的资源释放
  }
  # 断言失败时：
  smoke_fail "错误描述"
  ```
- **现场收集**：`smoke_fail` 与 `smoke_init` 会将测试现场完整保存到 `build/meson-logs/smoke/<测试名>/`（包含 `compositor.log`、`meta.txt`、`get_tree.json`、`get_focus.json`、`self_check.json`、`dump_scene.json` 及 `screenshot.png`）。

### 像素颜色断言的期望色必须从 `wctl get_tree` 的 `ssd.palette` 获取
- **现象**：主题配色调整后，像素测试失败并提示 `no SSD titlebar pixels`，误以为标题栏渲染丢失。
- **原因**：SSD 的标题栏和边框颜色有四层来源（客户端覆盖、配色方案、配置文件、内置默认），由 `woodland_ssd_get_palette()` 统一计算并反映在 `wctl get_tree` 的 `.ssd.palette` 中。
- **规范**：脚本中不要硬编码十六进制颜色值，应始终通过 jq 提取生效调色板颜色作为断言期望。

### `smoke-xwayland` / `smoke-capture` 依赖缺失时会整条 SKIP
- **现象**：本地运行测试时通过率看似 100%，合入 CI 或在另一台机器上失败。
- **原因**：`smoke-xwayland` 依赖 `Xwayland`、`grim`、`convert`、`jq` 以及 `xterm` 或 `xlogo`；`smoke-capture` 依赖 `weston-simple-shm`。缺任一项脚本以 exit code 77（SKIP）退出，断言根本没有执行。
- **toplevel 列举不再是外部依赖**：过去两个脚本依赖 AUR 包 `lswt`（读 `ext-foreign-toplevel-list-v1`），本机没装就整条 SKIP。现在仓库自带 `wltop`（`tests/smoke/wltop.c`，meson 目标 `tests/wltop`），由 meson 把它的路径作为脚本参数传入。它输出 lswt 兼容的 `-j` JSON（每个对象含 `app_id` / `title` / `identifier`），脚本优先用它，退回 PATH 里的 `wltop`，最后才是 `lswt`（兼容手动调用）。调试时可单独跑：`wltop`（表格）/`wltop -j`（JSON）/`wltop -j --wait-for APP_ID -t 2000`（等窗口出现）。
- **规范**：`meson test` 执行后务必核对 SKIP 名单，确认关键测试确实跑过；看到 SKIP 先读 testlog 里的具体 "not found" 行，别误判成代码回归。

### 宿主 X 会话环境下 `smoke-xwindow-control` 报授权失败
- **现象**：在宿主运行 X11 会话的桌面上运行 `smoke-xwindow-control` 时测试失败，日志出现 `Authorization required, but no authorization protocol specified`。
- **原因**：宿主已占用 `:0`，合成器内部拉起的 Xwayland 自动分配到 `:2`。wlroots 0.20 启动 Xwayland 时不再传递 `-auth` 参数，授权直接读取环境变量指向的 `/run/linglong/Xauthority`（其中仅有 `:0` 的凭据），导致探针连接 `:2` 报认证失败。CI 环境无宿主 X 会话，Xwayland 独占 `:0`，故 CI 无此问题。

---

## 3. Wayland 探针编写陷阱

### 自写探针必须 roundtrip 两次且使用 `prepare_read` 监听事件
- **第一次 roundtrip 的盲区**：探针发送 `wl_registry.bind` 时，出站请求被缓存在客户端缓冲区中。若在同一轮事件分发中 `wl_display.sync` 的完成回调已置位，dispatch 循环会立即退出，出站的 `bind` 请求并未 flush 到 socket。合成器端根本没有收到 bind，自然不会推送后续事件。必须进行第二次 roundtrip 才能确保所有 bind 请求刷新出去并收到对应 global 的初始状态。
- **事件接收规范**：接收服务端推送广播时，不能仅仅依赖 `wl_display_dispatch_pending()`（它只处理已读入客户端内存队列的事件，不从 socket 读数据）。正确模式是：
  ```c
  while (!done) {
      while (wl_display_prepare_read(display) != 0) {
          wl_display_dispatch_pending(display);
      }
      wl_display_flush(display);
      struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
      poll(&pfd, 1, timeout_ms);
      wl_display_read_events(display);
      wl_display_dispatch_pending(display);
  }
  ```
  参考样例：`tests/smoke/appearance_probe.c`。

### `smoke-switcher` 虚拟按键与代理生命周期
- **修饰键必须显式发送**：使用 `zwp_virtual_keyboard_v1` 发送按键时，wlroots 的 virtual-keyboard 后端将 key 事件的 `update_state` 设为 `false`，XKB 修饰键状态完全由 `zwp_virtual_keyboard_v1_modifiers` 请求单独驱动。若不发 modifiers，合成器侧修饰键掩码恒为 0，`Alt+Tab` 将无法匹配。
- **Proxy 对象必须显式销毁**：探针退出前必须调用销毁函数注销所有已 bind/create 的 proxy 对象。`wl_display_disconnect()` 不会自动释放它们，在 ASan 消毒器构建下 LeakSanitizer 会将其判为泄漏（退出码 134），导致测试误报失败。
- **最小化语义**：在 Woodland 的切换器中，最小化的窗口仍保留在候选列表中（标注为已最小化），提交切换时会自动恢复。断言时应检查 `get_tree` 的 `minimized` 状态翻转，而不是期望窗口从候选中被剔除。
