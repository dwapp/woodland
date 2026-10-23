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
