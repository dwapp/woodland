output_lib = static_library('woodland-output', ['output.c', 'output_config.c'],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server],
  link_with: [trace_lib, util_lib])
