# link_with trace_lib：seat.c 写 vt 轨迹（标签清单见 include/trace.h）。
# 链在库上而不是每个单测的链接列表里——拉入 seat.o 的测试自动带上它。
seat_lib = static_library('woodland-seat', 'seat.c',
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, xkbcommon],
  link_with: [trace_lib, dnd_lib])
