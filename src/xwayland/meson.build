woodland_xwayland_lib = static_library('woodland-xwayland',
  'xwayland.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, xwayland_dep],
  link_with: [shell_lib, ssd_lib, util_lib])
