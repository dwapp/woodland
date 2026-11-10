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

# wlroots（锁定 0.20.x，必须先于 scenefx 解析以确保 subproject override 正常生效；
# 否则 scenefx 内部 dependency 会先命中系统 wlroots 导致后续 override 异常）
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

# scenefx（随 effects 选项，默认启用；关闭时走 wlroots 直出，wrap 文件保留作占位）
scenefx = dependency('scenefx-0.5', fallback: 'scenefx',
                     required: get_option('effects'))

wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pangocairo = dependency('pangocairo')
cairo = dependency('cairo')
# sd_notify / sd_listen_fds（socket activation + Type=notify 就绪报告）。
# 只在 user session 的 systemd 场景有意义，headless 冒烟下是空操作。
libsystemd_dep = dependency('libsystemd')
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

install_data(
  'resources/woodland.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
  'resources/woodland-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

# systemd 用户单元：代替 treeland user 模式的 compositor 服务。
# Type=notify + socket activation（woodland.socket → woodland.service），
# 与 treeland.service(org.deepin.Compositor1, Type=dbus) 的区别在于 woodland
# 没有 D-Bus 总线名，靠 sd_notify(READY=1) 报告就绪。
systemd_dep = dependency('systemd', required: false)
if systemd_dep.found()
  systemd_user_unit_dir = systemd_dep.get_variable('systemduserunitdir')
  # ExecStart 要跟随实际安装目录（prefix 默认 /usr/local，发行版打包常设 /usr），
  # 用独立的 configuration_data 展开 @BINDIR@（conf_data 已用于 config.h，不能复用）。
  unit_data = configuration_data()
  unit_data.set('BINDIR', get_option('prefix') / get_option('bindir'))
  configure_file(
    input: 'resources/systemd/woodland.service.in',
    output: 'woodland.service',
    configuration: unit_data,
    install: true,
    install_dir: systemd_user_unit_dir,
  )
  install_data(
    'resources/systemd/woodland.socket',
    install_dir: systemd_user_unit_dir,
  )
endif
