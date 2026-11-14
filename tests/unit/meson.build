test_log = executable('test_log', 'test_log.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('log', test_log)

test_output = executable('test_output', 'test_output.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, session_lock_lib, cursor_lib, shell_lib, layer_lib, ssd_lib, effect_lib, workspace_lib, capture_lib, util_lib])
test('output', test_output)

test_cursor = executable('test_cursor', 'test_cursor.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [cursor_lib, util_lib])
test('cursor', test_cursor)

test_seat = executable('test_seat', 'test_seat.c',
  dependencies: [scene_deps, wayland_server, xkbcommon],
  include_directories: woodland_inc,
  # seat.c 现在会调 protocol 层的 idle / 快捷键抑制钩子
  link_with: [cursor_lib, output_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, protocol_lib, util_lib])
test('seat', test_seat)

test_workspace = executable('test_workspace', 'test_workspace.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  # workspace 切换会回调焦点回退（focus.c 在 shell_lib 里）
  link_with: [output_lib, cursor_lib, shell_lib, workspace_lib, ssd_lib, effect_lib, layer_lib, seat_lib, session_lock_lib, ime_lib, capture_lib, util_lib])
test('workspace', test_workspace)

test_view = executable('test_view', 'test_view.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, layer_lib, ssd_lib, effect_lib, workspace_lib, session_lock_lib, capture_lib, util_lib] + xwayland_libs)
test('view', test_view)

test_layer = executable('test_layer', 'test_layer.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, layer_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, ssd_lib, effect_lib, util_lib])
test('layer', test_layer)

test_ssd = executable('test_ssd', 'test_ssd.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [ssd_lib, effect_lib, cursor_lib, shell_lib, output_lib, layer_lib, workspace_lib, session_lock_lib, capture_lib, util_lib])
test('ssd', test_ssd)

# 圆角/模糊的"意图 → 参数"换算：不建 compositor，两种构建都要过
test_effect = executable('test_effect', 'test_effect.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  # effect_lib 在前：它引用 ssd_lib 的标题栏判定，静态库要按这个顺序解符号
  link_with: [effect_lib, ssd_lib, config_lib, cursor_lib, shell_lib, output_lib, layer_lib, workspace_lib, session_lock_lib, capture_lib, util_lib])
test('effect', test_effect)

test_placement = executable('test_placement', 'test_placement.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, effect_lib, workspace_lib, layer_lib, session_lock_lib, capture_lib, util_lib])
test('placement', test_placement)

test_focus = executable('test_focus', 'test_focus.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, effect_lib, workspace_lib, layer_lib, session_lock_lib, capture_lib, util_lib])
test('focus', test_focus)

test_capture = executable('test_capture', 'test_capture.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [capture_lib, util_lib])
test('capture', test_capture)

test_json = executable('test_json', 'test_json.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('json', test_json)

# selfcheck.c 编译在 shell_lib 里（和 focus/placement 同处），
# 所以这里链 shell_lib 而不是单独的库
test_selfcheck = executable('test_selfcheck', 'test_selfcheck.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, effect_lib, workspace_lib, layer_lib,
              session_lock_lib, capture_lib, util_lib])
test('selfcheck', test_selfcheck)

# trace 是叶子模块（只链 util），所以这里单独链它，不用把 shell/output 拖进来
test_trace = executable('test_trace', 'test_trace.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [trace_lib, util_lib])
test('trace', test_trace)

# listener 同样是 util 叶子模块
test_listener = executable('test_listener', 'test_listener.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('listener', test_listener)

test_drm_lease = executable('test_drm_lease', 'test_drm_lease.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [protocol_lib, output_lib, cursor_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib])
test('drm_lease', test_drm_lease)

test_pointer_constraints = executable('test_pointer_constraints', 'test_pointer_constraints.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [cursor_lib, protocol_lib, output_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib])
test('pointer_constraints', test_pointer_constraints)

if wayland_client_dep.found()
  test_syncobj = executable('test_syncobj', 'test_syncobj.c',
    dependencies: [scene_deps, wayland_server, wayland_client_dep],
    include_directories: woodland_inc,
    link_with: [protocol_lib, output_lib, cursor_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, capture_lib, util_lib])
  test('syncobj', test_syncobj)
endif
sec_ctx_xml = wp_datadir / 'staging' / 'security-context' / 'security-context-v1.xml'
test_sec_ctx_srcs = ['test_security_context.c']
test_sec_ctx_deps = [scene_deps, wayland_server]
test_sec_ctx_c_args = []
if wayland_client_dep.found() and wayland_scanner.found() and wp_datadir != '' and fs.exists(sec_ctx_xml)
  test_sec_ctx_deps += wayland_client_dep
  test_sec_ctx_c_args += ['-DTEST_HAS_SECURITY_CONTEXT_CLIENT=1']
  test_sec_ctx_srcs += custom_target('security-context-client-header',
    input: sec_ctx_xml,
    output: 'security-context-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  test_sec_ctx_srcs += custom_target('security-context-client-code',
    input: sec_ctx_xml,
    output: 'security-context-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endif

test_security_context = executable('test_security_context', test_sec_ctx_srcs,
  c_args: test_sec_ctx_c_args,
  dependencies: test_sec_ctx_deps,
  include_directories: woodland_inc,
  link_with: [protocol_lib] + xwayland_libs + [output_lib, cursor_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib, trace_lib, ipc_lib])
test('security_context', test_security_context)


test_config = executable('test_config', 'test_config.c',
  dependencies: [scene_deps, wayland_server, xkbcommon],
  include_directories: woodland_inc,
  link_with: [config_lib, util_lib, trace_lib, output_lib, cursor_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, ipc_lib, protocol_lib])
test('config', test_config)

# treeland-compositor-action 的动作表 / 工作区映射：纯逻辑，不建 display。
# protocol_lib 里的 compositor_action.c.o 会拉入 workspace 切换、trace 和
# wlroots 日志符号，所以链接列表跟 test_workspace 一样长。
test_compositor_action = executable('test_compositor_action', 'test_compositor_action.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [protocol_lib, workspace_lib, trace_lib, util_lib, shell_lib,
              output_lib, cursor_lib, layer_lib, ssd_lib, effect_lib, seat_lib,
              session_lock_lib, ime_lib, capture_lib])
test('compositor_action', test_compositor_action)

# treeland-appearance / -manager：set_* 分档表、越界裁剪、字体描述串的拆分合成。
# 纯逻辑，不建 display；裁剪上界来自 cursor.h / ssd.h，字体串解析在 ssd_lib
# （pango），所以链接列表跟 test_compositor_action 一样长。
test_appearance = executable('test_appearance', 'test_appearance.c',
  dependencies: [scene_deps, wayland_server, cairo, pangocairo],
  include_directories: woodland_inc,
  link_with: [protocol_lib, workspace_lib, trace_lib, util_lib, shell_lib,
              output_lib, cursor_lib, layer_lib, ssd_lib, effect_lib, seat_lib,
              session_lock_lib, ime_lib, capture_lib])
test('appearance', test_appearance)

# treeland-foreign-toplevel-manager-v3：state 数组编码（纯函数）。
# 链接列表跟 test_compositor_action 一样长——protocol_lib 里的族文件互相引用
# shell 的窗口状态机。
test_foreign_toplevel = executable('test_foreign_toplevel', 'test_foreign_toplevel.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [protocol_lib, workspace_lib, trace_lib, util_lib, shell_lib,
              output_lib, cursor_lib, layer_lib, ssd_lib, effect_lib, seat_lib,
              session_lock_lib, ime_lib, capture_lib])
test('foreign_toplevel', test_foreign_toplevel)

# treeland-xwindow-control-v1：fixed 取整、目标坐标饱和、状态默认值。
# 纯逻辑，不建 display；xwindow_control.c.o 会经 security_context.c.o 拉入
# wlroots 的 security-context / global-filter 符号，链接列表跟
# test_compositor_action 一样长。
test_xwindow_control = executable('test_xwindow_control', 'test_xwindow_control.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [protocol_lib, workspace_lib, trace_lib, util_lib, shell_lib,
              output_lib, cursor_lib, layer_lib, ssd_lib, effect_lib, seat_lib,
              session_lock_lib, ime_lib, capture_lib])
test('xwindow_control', test_xwindow_control)

# treeland-shortcut-manager-v3：PortableText 解析 / 有效性 / 匹配 / 动作分档。
# 纯逻辑，但 shortcut.c.o 会拉入窗口与工作区动作，链接列表跟
# test_compositor_action 一样长；xkbcommon 只被本测试直接用到。
test_shortcut = executable('test_shortcut', 'test_shortcut.c',
  dependencies: [scene_deps, wayland_server, xkbcommon],
  include_directories: woodland_inc,
  link_with: [protocol_lib, workspace_lib, trace_lib, util_lib, shell_lib,
              output_lib, cursor_lib, layer_lib, ssd_lib, effect_lib, seat_lib,
              session_lock_lib, ime_lib, capture_lib, ipc_lib])
test('shortcut', test_shortcut)

vk_xml = meson.project_source_root() + '/subprojects/wlroots/protocol/virtual-keyboard-unstable-v1.xml'
im_xml = meson.project_source_root() + '/subprojects/wlroots/protocol/input-method-unstable-v2.xml'

test_ime_srcs = ['test_ime.c']
test_ime_deps = [scene_deps, wayland_server, xkbcommon]
if wayland_client_dep.found() and wayland_scanner.found()
  test_ime_deps += wayland_client_dep
  foreach p : [[vk_xml, 'virtual-keyboard-unstable-v1'],
               [im_xml, 'input-method-unstable-v2']]
    test_ime_srcs += custom_target(p[1] + '-client-header',
      output: p[1] + '-client-protocol.h',
      command: [wayland_scanner, 'client-header', p[0], '@OUTPUT@'])
    test_ime_srcs += custom_target(p[1] + '-client-code',
      output: p[1] + '-protocol.c',
      command: [wayland_scanner, 'private-code', p[0], '@OUTPUT@'])
  endforeach
endif

test_ime = executable('test_ime', test_ime_srcs,
  dependencies: test_ime_deps,
  include_directories: woodland_inc,
  link_with: [ime_lib, seat_lib, session_lock_lib, cursor_lib, output_lib, shell_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, protocol_lib, util_lib])
test('ime', test_ime)

if wayland_client_dep.found() and wayland_scanner.found() and wp_datadir != ''
  session_lock_xml = wp_datadir / 'staging' / 'ext-session-lock' / 'ext-session-lock-v1.xml'
  if fs.exists(session_lock_xml)
    test_session_lock_srcs = ['test_session_lock.c']
    test_session_lock_srcs += custom_target('ext-session-lock-v1-client-header',
      output: 'ext-session-lock-v1-client-protocol.h',
      command: [wayland_scanner, 'client-header', session_lock_xml, '@OUTPUT@'])
    test_session_lock_srcs += custom_target('ext-session-lock-v1-client-code',
      output: 'ext-session-lock-v1-protocol.c',
      command: [wayland_scanner, 'private-code', session_lock_xml, '@OUTPUT@'])

    fontconfig_dep = dependency('fontconfig', required: false)
    test_session_lock = executable('test_session_lock', test_session_lock_srcs,
      dependencies: [scene_deps, wayland_server, wayland_client_dep, xkbcommon, fontconfig_dep, cairo, pangocairo],
      include_directories: woodland_inc,
      link_with: [session_lock_lib, output_lib, cursor_lib, shell_lib, seat_lib, ime_lib, protocol_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib])
    test('session_lock', test_session_lock)
  endif
endif

if wayland_client_dep.found() and wayland_scanner.found()
  gamma_xml = meson.project_source_root() + '/subprojects/wlroots/protocol/wlr-gamma-control-unstable-v1.xml'
  if fs.exists(gamma_xml)
    test_gamma_control_srcs = ['test_gamma_control.c']
    test_gamma_control_srcs += custom_target('wlr-gamma-control-unstable-v1-client-header',
      output: 'wlr-gamma-control-unstable-v1-client-protocol.h',
      command: [wayland_scanner, 'client-header', gamma_xml, '@OUTPUT@'])
    test_gamma_control_srcs += custom_target('wlr-gamma-control-unstable-v1-client-code',
      output: 'wlr-gamma-control-unstable-v1-protocol.c',
      command: [wayland_scanner, 'private-code', gamma_xml, '@OUTPUT@'])

    test_gamma_control = executable('test_gamma_control', test_gamma_control_srcs,
      dependencies: [scene_deps, wayland_server, wayland_client_dep],
      include_directories: woodland_inc,
      link_with: [protocol_lib, output_lib, cursor_lib, shell_lib, seat_lib, ime_lib, session_lock_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib])
    test('gamma_control', test_gamma_control)
  endif

if wayland_client_dep.found() and wayland_scanner.found() and wp_datadir != ''
  tearing_xml = wp_datadir / 'staging' / 'tearing-control' / 'tearing-control-v1.xml'
  if fs.exists(tearing_xml)
    test_tearing_srcs = ['test_tearing.c']
    test_tearing_srcs += custom_target('tearing-control-v1-client-header',
      output: 'tearing-control-v1-client-protocol.h',
      command: [wayland_scanner, 'client-header', tearing_xml, '@OUTPUT@'])
    test_tearing_srcs += custom_target('tearing-control-v1-client-code',
      output: 'tearing-control-v1-protocol.c',
      command: [wayland_scanner, 'private-code', tearing_xml, '@OUTPUT@'])

    test_tearing = executable('test_tearing', test_tearing_srcs,
      dependencies: [scene_deps, wayland_server, wayland_client_dep],
      include_directories: [woodland_inc, include_directories('../../src/protocol')],
      link_with: [protocol_lib, output_lib, cursor_lib, shell_lib, seat_lib, ime_lib, session_lock_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, util_lib, config_lib, trace_lib])
    test('tearing', test_tearing)
  endif
endif
endif

# dnd 同样是叶子模块（scene + trace）。真拖拽没法 headless 驱动，这里只
# 驱动状态机：图标树的开关/置顶、空图标、重复 destroy。
test_dnd = executable('test_dnd', 'test_dnd.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: [dnd_lib, trace_lib, util_lib])
test('dnd', test_dnd)
test_window_menu = executable('test_window_menu', 'test_window_menu.c',
  dependencies: [scene_deps, wayland_server, pangocairo, cairo, xkbcommon],
  include_directories: woodland_inc,
  link_with: [shell_lib, output_lib, cursor_lib, workspace_lib, ssd_lib, effect_lib, layer_lib, session_lock_lib, capture_lib, util_lib, trace_lib, dnd_lib] + xwayland_libs)
test('window_menu', test_window_menu)

# 环境探测：只是字符串判断，但 woodland_desktop_is_dde() 决定合成器要不要
# 在会话里拉 woodland-shortcutd（判错会抢走 DDE 的全局快捷键）。
test_env = executable('test_env', 'test_env.c',
  dependencies: [scene_deps, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('env', test_env)

# woodland-shortcutd 的配置层与命令启动器：不连 display，只测"用户写了什么、
# 我们打算注册什么、命令怎么起"。协议交互由 smoke-shortcutd 覆盖。
test_shortcutd_config = executable('test_shortcutd_config', 'test_shortcutd_config.c',
  include_directories: woodland_inc,
  link_with: shortcutd_core_lib)
test('shortcutd_config', test_shortcutd_config)

test_shortcutd_spawn = executable('test_shortcutd_spawn', 'test_shortcutd_spawn.c',
  include_directories: woodland_inc,
  link_with: shortcutd_core_lib)
test('shortcutd_spawn', test_shortcutd_spawn)
