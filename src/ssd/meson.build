rsvg_deps = []
if rsvg_dep.found()
  rsvg_deps = [rsvg_dep]
endif

ssd_lib = static_library('ssd',
  'ssd.c',
  'text_buffer.c',
  'icon.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, pangocairo, cairo] + rsvg_deps)
