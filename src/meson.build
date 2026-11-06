subdir('util')
# trace 只依赖 util，要排在所有会写轨迹的模块之前
subdir('trace')
subdir('output')
subdir('cursor')
subdir('capture')
subdir('shell')
subdir('seat')
subdir('workspace')
subdir('layer')
subdir('ssd')
# effect 只依赖 ssd（读标题栏可见性/矩形），放在它后面
subdir('effect')
# -Dxwayland=disabled 会同时关掉 wlroots 子项目的 xwayland 支持，
# 此时 shell 里的 X11 分支也一并编译掉，不能引用 xwayland 符号
xwayland_libs = []
if get_option('xwayland').allowed()
  subdir('xwayland')
  xwayland_libs = [woodland_xwayland_lib]
endif
subdir('ipc')
subdir('protocol')
subdir('config')
subdir('ime')
subdir('session_lock')
subdir('server')
subdir('wctl')

woodland_bin = executable('woodland',
  'main.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server],
  install: true,
  install_rpath: get_option('prefix') / get_option('libdir'),
  link_with: [server_lib, config_lib, ime_lib, session_lock_lib, util_lib, trace_lib, output_lib, cursor_lib, shell_lib, seat_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, ipc_lib, protocol_lib] + xwayland_libs)
