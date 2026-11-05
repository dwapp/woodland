config_lib = static_library('woodland-config', [
    'config.c',
    'toml.c',
    'apply.c',
  ],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, xkbcommon])
