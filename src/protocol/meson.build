# treeland 系协议的代码生成 + 协议族编组。
#
# XML 是本仓库内的只读副本（protocols/treeland/，MIT），不用系统安装的
# treeland-protocols：CI 里没有那个包，靠系统路径会让这些协议在 CI 上编不出来。
# 同步办法见 protocols/treeland/README.md。
wayland_scanner = find_program('wayland-scanner', native: true)
treeland_xml_dir = meson.project_source_root() / 'protocols' / 'treeland'

# ext-background-effect-v1 还是 staging，拿系统 wayland-protocols 里的那份。
# 1.41 起才有这个协议；机器上太老就直接 configure 失败，别留个"编出来没有
# 模糊能力位"的哑构建。
wayland_protos = dependency('wayland-protocols', version: '>=1.41')
wp_datadir = wayland_protos.get_variable('pkgdatadir')
bg_effect_xml = wp_datadir / 'staging' / 'ext-background-effect' / 'ext-background-effect-v1.xml'

proto_srcs = []
foreach p : [['treeland-decoration-unstable-v1', 'treeland-decoration'],
             ['treeland-wallpaper-shell-unstable-v1', 'treeland-wallpaper-shell'],
             ['treeland-wallpaper-manager-unstable-v1', 'treeland-wallpaper-manager'],
             ['treeland-compositor-action-unstable-v1', 'treeland-compositor-action']]
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

# 同一套生成规则套在系统 XML 上（前缀不同，避免和 treeland 那份撞名）
proto_srcs += custom_target('background-effect-server-header',
  input: bg_effect_xml, output: 'ext-background-effect-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
proto_srcs += custom_target('background-effect-server-code',
  input: bg_effect_xml, output: 'ext-background-effect-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
protocol_lib = static_library('woodland-protocol',
  'protocol.c',
  'idle.c',
  'output.c',
  'input.c',
  'window.c',
  'drm_lease.c',
  'decoration.c',
  'background_effect.c',
  'wallpaper.c',
  'wallpaper_config.c',
  'compositor_action.c',
  proto_srcs,
  # '.'：生成的头文件落在 build/src/protocol/，就近取
  include_directories: [woodland_inc, include_directories('.')],
  dependencies: [scene_deps, wayland_server, wayland_protos],
  link_with: [trace_lib, effect_lib])
