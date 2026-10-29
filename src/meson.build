subdir('util')
subdir('output')
subdir('cursor')
subdir('capture')
subdir('shell')
subdir('seat')
subdir('workspace')
subdir('layer')
subdir('ssd')
# -Dxwayland=disabled 会同时关掉 wlroots 子项目的 xwayland 支持，
# 此时 shell 里的 X11 分支也一并编译掉，不能引用 xwayland 符号
xwayland_libs = []
if get_option('xwayland').allowed()
  subdir('xwayland')
  xwayland_libs = [woodland_xwayland_lib]
endif
subdir('ipc')
subdir('protocol')
subdir('server')
subdir('wctl')

woodland_bin = executable('woodland',
  'main.c',
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server],
  link_with: [server_lib, util_lib, output_lib, cursor_lib, shell_lib, seat_lib, workspace_lib, layer_lib, ssd_lib, capture_lib, ipc_lib, protocol_lib] + xwayland_libs)
