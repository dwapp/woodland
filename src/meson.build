subdir('util')
subdir('output')
subdir('cursor')
subdir('shell')
subdir('seat')
subdir('workspace')
subdir('layer')
subdir('ssd')
subdir('server')

woodland_bin = executable('woodland',
  'main.c',
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server],
  link_with: [server_lib, util_lib, output_lib, cursor_lib, shell_lib, seat_lib, workspace_lib, layer_lib, ssd_lib])
