project('woodland', 'c',
  version: '0.1.0',
  license: 'GPL-2.0-only',
  meson_version: '>=1.3',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
  ],
)

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
], language: 'c')

# scenefx（随 effects 选项，默认启用；关闭时走 wlroots 直出，wrap 文件保留作占位）
scenefx = dependency('scenefx-0.5', fallback: 'scenefx',
                     required: get_option('effects'))
# wlroots（锁定 0.20.x）
wlroots_version = ['>=0.20.0', '<0.21.0']
wlroots_sub = subproject('wlroots', default_options: ['examples=false'],
                         required: false, version: wlroots_version)
wlroots = dependency('wlroots-0.20', version: wlroots_version,
                     fallback: 'wlroots')

# wlroots 作为子项目构建时，wayland-scanner 生成的协议头落在
# build/subprojects/wlroots/protocol/，而它的 declare_dependency 只导出
# include/ —— wlr/types/wlr_layer_shell_v1.h 里是 #include "wlr-layer-
# shell-unstable-v1-protocol.h"，靠同目录/-I 找不到就编译失败。
# 系统安装的 wlroots 把协议头装进 /usr/include，所以只有 fallback 才需要补。
if wlroots_sub.found() and wlroots.type_name() == 'internal'
  wlroots = declare_dependency(
    dependencies: wlroots,
    include_directories: include_directories('subprojects/wlroots/protocol'),
  )
endif

wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pangocairo = dependency('pangocairo')
cairo = dependency('cairo')
rsvg_dep = dependency('librsvg-2.0', version: '>=2.52', required: false)
# Xwayland：依赖只提供 Xwayland 可执行文件路径（wlroots 编译期读取）。
# 代码始终参与编译（view 访问器里要引用 wlroots 的 xwayland API），
# -Dxwayland=disabled 只表示运行时不创建 Xwayland、不接 X 客户端。
xwayland_dep = dependency('xwayland', required: false)
conf_data = configuration_data()
conf_data.set10('WOODLAND_HAVE_XWAYLAND',
                get_option('xwayland').allowed())

conf_data.set10('WOODLAND_HAVE_EFFECTS',
                get_option('effects').enabled() and scenefx.found())
conf_data.set10('WOODLAND_HAVE_LIBRSVG', rsvg_dep.found())
configure_file(output: 'config.h', configuration: conf_data)

# scene 走哪套实现由 effects 决定：scenefx 换掉 wlroots 的 scene 与 renderer，
# 两边的符号同名，链接顺序决定谁生效——scenefx 必须排在 wlroots 之前。
# 需要 scene 的模块统一依赖 scene_deps，不要在各自的 meson.build 里写死 wlroots。
scene_deps = [wlroots]
if get_option('effects').enabled() and scenefx.found()
  scene_deps = [scenefx, wlroots]
endif

subdir('include')
subdir('src')
subdir('tests')
