test_log = executable('test_log', 'test_log.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('log', test_log)

test_output = executable('test_output', 'test_output.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, session_lock_lib, cursor_lib, shell_lib, layer_lib, ssd_lib, workspace_lib, capture_lib, util_lib])
test('output', test_output)

test_cursor = executable('test_cursor', 'test_cursor.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [cursor_lib, util_lib])
test('cursor', test_cursor)

test_seat = executable('test_seat', 'test_seat.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  # seat.c 现在会调 protocol 层的 idle / 快捷键抑制钩子
  link_with: [cursor_lib, output_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, protocol_lib, util_lib])
test('seat', test_seat)

test_workspace = executable('test_workspace', 'test_workspace.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  # workspace 切换会回调焦点回退（focus.c 在 shell_lib 里）
  link_with: [output_lib, cursor_lib, shell_lib, workspace_lib, ssd_lib, layer_lib, seat_lib, session_lock_lib, ime_lib, capture_lib, util_lib])
test('workspace', test_workspace)

test_view = executable('test_view', 'test_view.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, layer_lib, ssd_lib, workspace_lib, session_lock_lib, capture_lib, util_lib])
test('view', test_view)

test_layer = executable('test_layer', 'test_layer.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, layer_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, ssd_lib, util_lib])
test('layer', test_layer)

test_ssd = executable('test_ssd', 'test_ssd.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [ssd_lib, cursor_lib, shell_lib, output_lib, layer_lib, workspace_lib, session_lock_lib, capture_lib, util_lib])
test('ssd', test_ssd)

test_placement = executable('test_placement', 'test_placement.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, workspace_lib, layer_lib, session_lock_lib, capture_lib, util_lib])
test('placement', test_placement)

test_focus = executable('test_focus', 'test_focus.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, workspace_lib, layer_lib, session_lock_lib, capture_lib, util_lib])
test('focus', test_focus)

test_capture = executable('test_capture', 'test_capture.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [capture_lib, util_lib])
test('capture', test_capture)

test_json = executable('test_json', 'test_json.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('json', test_json)

# selfcheck.c 编译在 shell_lib 里（和 focus/placement 同处），
# 所以这里链 shell_lib 而不是单独的库
test_selfcheck = executable('test_selfcheck', 'test_selfcheck.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, cursor_lib, shell_lib, ssd_lib, workspace_lib, layer_lib,
              session_lock_lib, capture_lib, util_lib])
test('selfcheck', test_selfcheck)

# trace 是叶子模块（只链 util），所以这里单独链它，不用把 shell/output 拖进来
test_trace = executable('test_trace', 'test_trace.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [trace_lib, util_lib])
test('trace', test_trace)

# listener 同样是 util 叶子模块
test_listener = executable('test_listener', 'test_listener.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('listener', test_listener)

test_config = executable('test_config', 'test_config.c',
  dependencies: [wlroots, wayland_server, xkbcommon],
  include_directories: woodland_inc,
  link_with: [config_lib, util_lib, trace_lib, output_lib, cursor_lib, shell_lib, seat_lib, session_lock_lib, ime_lib, workspace_lib, layer_lib, ssd_lib, capture_lib, ipc_lib, protocol_lib])
test('config', test_config)

vk_xml = meson.project_source_root() + '/subprojects/wlroots/protocol/virtual-keyboard-unstable-v1.xml'
im_xml = meson.project_source_root() + '/subprojects/wlroots/protocol/input-method-unstable-v2.xml'

test_ime_srcs = ['test_ime.c']
test_ime_deps = [wlroots, wayland_server, xkbcommon]
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
  link_with: [ime_lib, seat_lib, session_lock_lib, cursor_lib, output_lib, shell_lib, workspace_lib, layer_lib, ssd_lib, protocol_lib, util_lib])
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

    test_session_lock = executable('test_session_lock', test_session_lock_srcs,
      dependencies: [wlroots, wayland_server, wayland_client_dep, xkbcommon],
      include_directories: woodland_inc,
      link_with: [session_lock_lib, output_lib, cursor_lib, shell_lib, seat_lib, ime_lib, protocol_lib, workspace_lib, layer_lib, ssd_lib, capture_lib, util_lib])
    test('session_lock', test_session_lock)
  endif
endif
