subdir('util')
subdir('server')

woodland_bin = executable('woodland',
  'main.c',
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server],
  link_with: [server_lib, util_lib])
