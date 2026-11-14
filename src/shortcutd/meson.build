# woodland-shortcutd：treeland-shortcut-manager-v3 的客户端实现。
#
# 独立可执行文件，只依赖 libwayland-client——它要以普通 Wayland 客户端身份
# 运行（合成器 fork+exec 它，见 src/server/server.c:start_shortcut_client），
# 不链合成器的任何代码，也不能和合成器的内部结构扯上关系。
#
# 协议头由 wayland-scanner 从仓库内 XML 生成；target 名加 shortcutd_ 前缀，
# 避免与 tests/meson.build 里同名协议的 target 撞车。
shortcutd_xml = meson.project_source_root() / 'protocols' / 'treeland' / 'treeland-shortcut-manager-unstable-v3.xml'
shortcutd_scanner = find_program('wayland-scanner')

shortcutd_protocol = [
  custom_target('shortcutd-manager-client-header',
    input: shortcutd_xml,
    output: 'treeland-shortcut-manager-unstable-v3-client-protocol.h',
    command: [shortcutd_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('shortcutd-manager-client-code',
    input: shortcutd_xml,
    output: 'treeland-shortcut-manager-unstable-v3-protocol.c',
    command: [shortcutd_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

# 配置解析与命令启动器拆成静态库：单元测试直接链它，不用把 wayland-client
# 拖进测试，也方便以后给 wsd 加别的前端（比如带 GUI 的键位编辑器）。
#
# 复用 ../config/toml.c：合成器的配置本来就是 TOML，用户不该为快捷键再学
# 第二种格式；那份解析器只依赖 libc，编进客户端侧没有额外依赖。
shortcutd_core_lib = static_library('woodland-shortcutd-core',
  ['config.c', 'spawn.c', '../config/toml.c'],
  include_directories: [woodland_inc, include_directories('../config')])

shortcutd_bin = executable('woodland-shortcutd',
  ['main.c', shortcutd_protocol],
  include_directories: woodland_inc,
  dependencies: [wayland_client],
  link_with: shortcutd_core_lib,
  install: true)
