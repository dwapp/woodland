# 拖拽（DnD）。只依赖 scene 与 trace：hit-test 直接问场景，不经过 shell 的
# 视图判定（拖到 panel/Dock 上也该有 drop 目标），所以 shell / seat 可以
# 反过来链它，而不是互相依赖。
dnd_lib = static_library('woodland-dnd', 'dnd.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server],
  link_with: [util_lib, trace_lib])
