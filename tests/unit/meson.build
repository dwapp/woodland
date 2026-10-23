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
  link_with: [cursor_lib, output_lib, shell_lib, seat_lib])
test('seat', test_seat)
