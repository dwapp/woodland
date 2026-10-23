project('woodland', 'c',
  version: '0.1.0',
  license: 'MIT',
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

# scenefx（随 effects 选项，M0 默认不启用；wrap 文件保留作占位）
scenefx = dependency('scenefx-0.5', fallback: 'scenefx',
                     required: get_option('effects'))

# wlroots（锁定 0.20.x）
wlroots_version = ['>=0.20.0', '<0.21.0']
subproject('wlroots', default_options: ['examples=false'],
           required: false, version: wlroots_version)
wlroots = dependency('wlroots-0.20', version: wlroots_version,
                     fallback: 'wlroots')

wayland_server = dependency('wayland-server')

# 特性开关 → config.h
conf_data = configuration_data()
conf_data.set10('WOODLAND_HAVE_EFFECTS',
                get_option('effects').enabled() and scenefx.found())
configure_file(output: 'config.h', configuration: conf_data)

subdir('include')
subdir('src')
subdir('tests')
