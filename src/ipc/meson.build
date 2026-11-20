math_dep = meson.get_compiler('c').find_library('m', required: false)

ipc_lib = static_library('woodland-ipc', ['ipc.c', 'commands.c', 'ctl.c', 'inspect.c', 'wait.c'],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, xkbcommon, math_dep],
  link_with: [util_lib, trace_lib, shell_lib, output_lib, workspace_lib, ssd_lib])
