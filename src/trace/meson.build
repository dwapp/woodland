# trace 是叶子模块：只依赖 util（json），任何模块都可以链它而不会成环。
# 反过来它**不许**链 shell/output/protocol —— 那会让"随手加一行轨迹"
# 变成一次架构改动。
trace_lib = static_library('woodland-trace', ['trace.c'],
  include_directories: woodland_inc,
  dependencies: [wlroots, wayland_server],
  link_with: [util_lib])
