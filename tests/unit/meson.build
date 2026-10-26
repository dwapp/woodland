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
  link_with: [cursor_lib, output_lib, shell_lib, seat_lib, workspace_lib, layer_lib])
test('seat', test_seat)

test_workspace = executable('test_workspace', 'test_workspace.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [workspace_lib])
test('workspace', test_workspace)

test_view = executable('test_view', 'test_view.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, shell_lib, layer_lib])
test('view', test_view)

test_layer = executable('test_layer', 'test_layer.c',
  dependencies: [wlroots, wayland_server],
  include_directories: woodland_inc,
  link_with: [output_lib, layer_lib, shell_lib, seat_lib, workspace_lib])
test('layer', test_layer)
