# treeland 系协议的代码生成 + 协议族编组。
#
# XML 是本仓库内的只读副本（protocols/treeland/，MIT），不用系统安装的
# treeland-protocols：CI 里没有那个包，靠系统路径会让这些协议在 CI 上编不出来。
# 同步办法见 protocols/treeland/README.md。
wayland_scanner = find_program('wayland-scanner', native: true)
treeland_xml_dir = meson.project_source_root() / 'protocols' / 'treeland'

proto_srcs = []
foreach p : [['treeland-decoration-unstable-v1', 'treeland-decoration']]
  proto_xml = treeland_xml_dir / (p[0] + '.xml')
  # 服务端只需要 header + 接口表：server-header 给实现用的函数声明，
  # private-code 给 wl_interface 定义（客户端桩代码一概不生成）
  proto_srcs += custom_target(p[1] + '-server-header',
    input: proto_xml, output: p[0] + '-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  proto_srcs += custom_target(p[1] + '-server-code',
    input: proto_xml, output: p[0] + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

protocol_lib = static_library('woodland-protocol',
  'protocol.c',
  'idle.c',
  'output.c',
  'input.c',
  'window.c',
  'drm_lease.c',
  'decoration.c',
  proto_srcs,
  # '.'：生成的头文件落在 build/src/protocol/，decoration.c 用
  # #include "treeland-decoration-unstable-v1-protocol.h" 就近取
  include_directories: [woodland_inc, include_directories('.')],
  dependencies: [wlroots, wayland_server],
  link_with: [trace_lib])
