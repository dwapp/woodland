# selfcheck.c 放在 shell 里而不是单独一个库：它和 shell/focus/placement
# 互引用（状态改了才检查），拆出去只会让静态库出现循环依赖。
shell_lib = static_library('woodland-shell',
  ['shell.c', 'toplevel_state.c', 'interactive.c', 'cursor.c',
   'focus.c', 'placement.c', 'selfcheck.c', 'snap_preview.c', 'window_menu.c'],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, pangocairo, cairo],
  # effect_lib：窗口菜单的圆角直接问 woodland_effect_frame_corners()，避免出现
  # "菜单自己再读一遍配置"的第二套算法（effect 只依赖 ssd，不成环）。
  link_with: [capture_lib, util_lib, trace_lib, dnd_lib, effect_lib])
