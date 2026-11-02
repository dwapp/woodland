if get_option('test').enabled()
  fs = import('fs')
  wayland_client_dep = dependency('wayland-client', required: false)
  wp_dep = dependency('wayland-protocols', required: false)
  wayland_scanner = find_program('wayland-scanner', required: false)
  wp_datadir = ''
  if wp_dep.found()
    wp_datadir = wp_dep.get_variable(pkgconfig: 'pkgdatadir', internal: 'pkgdatadir')
  endif
  subdir('unit')

  # Xwayland 冒烟用的小客户端：建一个 override-redirect 窗口
  xcb_dep = dependency('xcb', required: false)
  xoverride_bin = disabler()
  if xcb_dep.found()
    xoverride_bin = executable('xoverride', 'smoke/xoverride.c',
                               dependencies: [xcb_dep])
  endif

  # 三个冒烟测试都要起一个真实的合成器进程；并发跑时本环境（pixman + 受限
  # /dev/shm）会以 SIGBUS 随机挂掉（未改动的 main 上同样复现），因此串行。
  test('smoke-headless',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_headless.sh',
              woodland_bin.full_path()],
       is_parallel: false)
  test('smoke-protocol',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_protocol_smoke.sh',
              woodland_bin.full_path()],
       is_parallel: false)
  # 调试接口（wctl）：只断言状态，不数像素
  test('smoke-ipc',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_ipc_smoke.sh',
              woodland_bin.full_path(),
              wctl_bin.full_path()],
       is_parallel: false,
       timeout: 60)

  test('smoke-capture',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_capture_smoke.sh',
              woodland_bin.full_path()],
       is_parallel: false,
       timeout: 60)

  # output-management：test 不许有副作用、apply 必须生效、多 head 必须可见。
  # 依赖 wlr-randr（没有则脚本自己 skip）。
  test('smoke-output-mgmt',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_output_mgmt_smoke.sh',
              woodland_bin.full_path()],
       is_parallel: false,
       timeout: 60)

  # idle-inhibit 的聚合回归（"最后一个抑制器销毁后 idle 必须恢复"）。
  # 需要 wayland-client 与 wayland-protocols 里的两份 xml：缺任何一样就
  # 不构建探针、不注册测试，免得把构建搞挂。
  if wayland_client_dep.found() and wp_dep.found() and wayland_scanner.found()
    idle_inhibit_xml = wp_datadir / 'unstable' / 'idle-inhibit' / 'idle-inhibit-unstable-v1.xml'
    idle_notify_xml = wp_datadir / 'staging' / 'ext-idle-notify' / 'ext-idle-notify-v1.xml'

    idle_probe_srcs = ['smoke/idle_probe.c']
    foreach p : [[idle_inhibit_xml, 'idle-inhibit-unstable-v1'],
                 [idle_notify_xml, 'ext-idle-notify-v1']]
      idle_probe_srcs += custom_target(p[1] + '-client-header',
        input: p[0], output: p[1] + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
      idle_probe_srcs += custom_target(p[1] + '-client-code',
        input: p[0], output: p[1] + '-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
    endforeach

    idle_probe = executable('idle_probe', idle_probe_srcs,
                            dependencies: [wayland_client_dep])
    test('smoke-idle-inhibit',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_idle_smoke.sh',
                woodland_bin.full_path(),
                idle_probe.full_path()],
         is_parallel: false,
         timeout: 60)
  endif

  # output-management 的事件顺序回归（apply 的 succeeded 必须先于新一轮
  # configuration + done）。协议 xml 属于 wlr-protocols（不在
  # wayland-protocols 里），只认系统安装位置：wrap 内的 wlroots 属于嵌套
  # 子项目，meson 不许跨子项目取文件。找不到就不注册这个测试。
  fs = import('fs')
  om_xml = '/usr/share/wlr-protocols/unstable/wlr-output-management-unstable-v1.xml'
  if fs.exists(om_xml) and wayland_client_dep.found() and wayland_scanner.found()
    om_probe_srcs = ['smoke/output_mgmt_probe.c',
      custom_target('wlr-output-management-client-header',
        input: om_xml, output: 'wlr-output-management-unstable-v1-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('wlr-output-management-client-code',
        input: om_xml, output: 'wlr-output-management-unstable-v1-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
    ]
    om_probe = executable('output_mgmt_probe', om_probe_srcs,
                          dependencies: [wayland_client_dep])
    test('smoke-output-mgmt-order',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_output_mgmt_order_smoke.sh',
                woodland_bin.full_path(),
                om_probe.full_path()],
         is_parallel: false,
         timeout: 60)
  endif

  if not is_disabler(xoverride_bin) and get_option('xwayland').allowed()
    test('smoke-xwayland',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_xwayland_smoke.sh',
                woodland_bin.full_path(),
                xoverride_bin.full_path()],
         is_parallel: false,
         timeout: 90)
  endif
endif
