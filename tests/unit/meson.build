test_log = executable('test_log', 'test_log.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: util_lib)
test('log', test_log)

test_output = executable('test_output', 'test_output.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: output_lib)
test('output', test_output)

test_cursor = executable('test_cursor', 'test_cursor.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: cursor_lib)
test('cursor', test_cursor)

test_seat = executable('test_seat', 'test_seat.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [cursor_lib, output_lib, shell_lib, seat_lib, workspace_lib, layer_lib, ssd_lib])
test('seat', test_seat)

test_workspace = executable('test_workspace', 'test_workspace.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  # workspace 切换会回调焦点回退（focus.c 在 shell_lib 里）
  link_with: [output_lib, shell_lib, workspace_lib, ssd_lib, layer_lib, seat_lib, capture_lib])
test('workspace', test_workspace)

test_view = executable('test_view', 'test_view.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, shell_lib, layer_lib, ssd_lib, workspace_lib,
              capture_lib])
test('view', test_view)

test_layer = executable('test_layer', 'test_layer.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, layer_lib, shell_lib, seat_lib, workspace_lib, ssd_lib])
test('layer', test_layer)

test_ssd = executable('test_ssd', 'test_ssd.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [ssd_lib, shell_lib, output_lib, layer_lib, workspace_lib,
              capture_lib])
test('ssd', test_ssd)

test_placement = executable('test_placement', 'test_placement.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, shell_lib, ssd_lib, workspace_lib, layer_lib,
              capture_lib])
test('placement', test_placement)

test_focus = executable('test_focus', 'test_focus.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, shell_lib, ssd_lib, workspace_lib, layer_lib,
              capture_lib])
test('focus', test_focus)

test_capture = executable('test_capture', 'test_capture.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [capture_lib])
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
  link_with: [output_lib, shell_lib, ssd_lib, workspace_lib, layer_lib,
              capture_lib, util_lib])
test('selfcheck', test_selfcheck)
