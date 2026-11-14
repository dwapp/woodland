subdir('util')
# trace 只依赖 util，要排在所有会写轨迹的模块之前
subdir('trace')
subdir('output')
subdir('cursor')
# dnd 排在 seat / shell 之前：那两个都要反着调它（seat 起监听器，
# shell 的光标层拿 active 做守卫），而它只依赖 scene 与 trace。
subdir('dnd')
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
# 客户端侧：非 DDE 会话下由合成器自动启动（见 src/server/server.c）
subdir('shortcutd')

woodland_bin = executable('woodland',
  'main.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server],
  install: true,
  install_rpath: get_option('prefix') / get_option('libdir'),
  link_with: [server_lib, config_lib, ime_lib, session_lock_lib, util_lib, trace_lib, output_lib, cursor_lib, dnd_lib, shell_lib, seat_lib, workspace_lib, layer_lib, ssd_lib, effect_lib, capture_lib, ipc_lib, protocol_lib] + xwayland_libs)
