# 只给 Xwayland 的 xdg-output global 需要 xdg-output 协议的服务端头与接口表。
# 接口表用 private-code 生成（同 src/protocol 对 xdg-shell 的做法）。
xwl_scanner = find_program('wayland-scanner', native: true)
xwl_wp_datadir = dependency('wayland-protocols', version: '>=1.41').get_variable('pkgdatadir')
xwl_xdg_output_xml = xwl_wp_datadir / 'unstable' / 'xdg-output' / 'xdg-output-unstable-v1.xml'
xwl_proto_srcs = [
  custom_target('xwl-xdg-output-server-header',
    input: xwl_xdg_output_xml, output: 'xdg-output-unstable-v1-protocol.h',
    command: [xwl_scanner, 'server-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xwl-xdg-output-server-code',
    input: xwl_xdg_output_xml, output: 'xdg-output-unstable-v1-protocol.c',
    command: [xwl_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

woodland_xwayland_lib = static_library('woodland-xwayland',
  ['xwayland.c', 'xwayland_outputs.c', xwl_proto_srcs],
  include_directories: woodland_inc,
  dependencies: [scene_deps, wayland_server, xwayland_dep],
  link_with: [shell_lib, ssd_lib, util_lib])
