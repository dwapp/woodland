shell_lib = static_library('woodland-shell',
  ['shell.c', 'focus.c', 'placement.c'],
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server],
  link_with: [capture_lib])
