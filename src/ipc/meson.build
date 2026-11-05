ipc_lib = static_library('woodland-ipc', ['ipc.c', 'commands.c'],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server],
  link_with: [util_lib, trace_lib, shell_lib, output_lib, workspace_lib, ssd_lib])
