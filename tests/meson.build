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

  # treeland 壁纸协议（manager / notifier / shell）。XML 用的是仓库内副本
  # （protocols/treeland/），所以不像上面几项那样依赖系统里装了对应的包。
  # 另外要 ext-workspace 的客户端头：切工作区没法用键盘（headless 没设备），
  # 只能走这一个协议；它来自 wayland-protocols，缺了就不注册这个测试。
  wp_shell_xml = meson.project_source_root() / 'protocols' / 'treeland' / 'treeland-wallpaper-shell-unstable-v1.xml'
  wp_manager_xml = meson.project_source_root() / 'protocols' / 'treeland' / 'treeland-wallpaper-manager-unstable-v1.xml'
  ext_workspace_xml = wp_datadir / 'staging' / 'ext-workspace' / 'ext-workspace-v1.xml'

  if wayland_client_dep.found() and wayland_scanner.found() and fs.exists(ext_workspace_xml)
    wp_probe_srcs = ['smoke/wallpaper_probe.c']
    foreach p : [[wp_shell_xml, 'treeland-wallpaper-shell-unstable-v1'],
                 [wp_manager_xml, 'treeland-wallpaper-manager-unstable-v1'],
                 [ext_workspace_xml, 'ext-workspace-v1']]
      wp_probe_srcs += custom_target(p[1] + '-client-header',
        input: p[0], output: p[1] + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
      wp_probe_srcs += custom_target(p[1] + '-client-code',
        input: p[0], output: p[1] + '-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
    endforeach

    wallpaper_probe = executable('wallpaper_probe', wp_probe_srcs,
                                 dependencies: [wayland_client_dep])
    test('smoke-wallpaper',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_wallpaper_smoke.sh',
                woodland_bin.full_path(),
                wallpaper_probe.full_path(),
                wctl_bin.full_path()],
         is_parallel: false,
         timeout: 60)
  endif

  # treeland-compositor-action：bind / trigger / destroy + 工作区动作接线。
  # 探针只需要仓库内 XML 和 wayland-client，不依赖系统 wayland-protocols；
  # 缺依赖就不构建探针、不注册测试，免得把构建搞挂。
  action_xml = meson.project_source_root() / 'protocols' / 'treeland' / 'treeland-compositor-action-unstable-v1.xml'
  if wayland_client_dep.found() and wayland_scanner.found() and fs.exists(action_xml)
    action_probe_srcs = [
      'smoke/action_probe.c',
      custom_target('treeland-compositor-action-client-header',
        input: action_xml,
        output: 'treeland-compositor-action-unstable-v1-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('treeland-compositor-action-client-code',
        input: action_xml,
        output: 'treeland-compositor-action-unstable-v1-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
    ]

    action_probe = executable('action_probe', action_probe_srcs,
                               dependencies: [wayland_client_dep])
    test('smoke-compositor-action',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_action_smoke.sh',
                woodland_bin.full_path(),
                action_probe.full_path(),
                wctl_bin.full_path()],
         is_parallel: false,
         timeout: 90)
  endif

  # treeland-shortcut-manager-v3：acquire 独占、bind_failure、upsert、手势
  # 只登记、capture 的失败与取消。探针要一个真正 map 的 toplevel 来拿键盘
  # 焦点，所以除仓库内 XML 外还要系统 wayland-protocols 的 xdg-shell；
  # 缺依赖就不注册，免得把构建搞挂。
  shortcut_xml = meson.project_source_root() / 'protocols' / 'treeland' / 'treeland-shortcut-manager-unstable-v3.xml'
  shortcut_xdg_xml = wp_datadir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
  if wayland_client_dep.found() and wp_dep.found() and wayland_scanner.found() and fs.exists(shortcut_xml) and fs.exists(shortcut_xdg_xml)
    shortcut_probe_srcs = [
      'smoke/shortcut_probe.c',
      custom_target('shortcut-manager-client-header',
        input: shortcut_xml,
        output: 'treeland-shortcut-manager-unstable-v3-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('shortcut-manager-client-code',
        input: shortcut_xml,
        output: 'treeland-shortcut-manager-unstable-v3-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
      # 前缀与 deco_probe 的 xdg-shell 目标错开：同一个 build 目录里
      # custom_target 的输出名不能重复。
      custom_target('shortcut-xdg-shell-client-header',
        input: shortcut_xdg_xml,
        output: 'shortcut-xdg-shell-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('shortcut-xdg-shell-client-code',
        input: shortcut_xdg_xml,
        output: 'shortcut-xdg-shell-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
    ]

    shortcut_probe = executable('shortcut_probe', shortcut_probe_srcs,
                                dependencies: [wayland_client_dep])
    test('smoke-shortcut',
         find_program('sh'),
         args: [meson.current_source_dir() / 'smoke' / 'run_shortcut_smoke.sh',
                woodland_bin.full_path(),
                shortcut_probe.full_path(),
                wctl_bin.full_path()],
         is_parallel: false,
         timeout: 120)
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

  # treeland-decoration：客户端探针 + wctl 断言（边距/覆盖值/协议错误）。
  # 需要 wayland-client、wayland-protocols 里的 xdg-shell / xdg-decoration，
  # 以及仓库自带的 treeland XML；缺任何一样就不注册，免得把构建搞挂。
  if wayland_client_dep.found() and wp_dep.found() and wayland_scanner.found()
    deco_probe_srcs = ['smoke/deco_probe.c']
    deco_xmls = [
      [wp_datadir / 'stable' / 'xdg-shell' / 'xdg-shell.xml', 'xdg-shell'],
      [wp_datadir / 'unstable' / 'xdg-decoration' / 'xdg-decoration-unstable-v1.xml',
       'xdg-decoration-unstable-v1'],
      [meson.project_source_root() / 'protocols' / 'treeland' /
       'treeland-decoration-unstable-v1.xml', 'treeland-decoration-unstable-v1'],
      [wp_datadir / 'staging' / 'ext-background-effect' / 'ext-background-effect-v1.xml',
       'ext-background-effect-v1'],
      # layer surface 也能用 ext-background-effect（面板/Dock），探针 --layer 走这条。
      # 用仓库里的副本：Meson 不让 tests/ 引用嵌套子项目里的文件。
      [meson.project_source_root() / 'protocols' / 'tests' /
       'wlr-layer-shell-unstable-v1.xml', 'wlr-layer-shell-unstable-v1'],
    ]
    deco_xml_ok = true
    foreach p : deco_xmls
      if not fs.exists(p[0])
        deco_xml_ok = false
      endif
    endforeach
    if deco_xml_ok
      foreach p : deco_xmls
        deco_probe_srcs += custom_target(p[1] + '-client-header',
          input: p[0], output: p[1] + '-client-protocol.h',
          command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
        deco_probe_srcs += custom_target(p[1] + '-client-code',
          input: p[0], output: p[1] + '-protocol.c',
          command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
      endforeach
      deco_probe = executable('deco_probe', deco_probe_srcs,
                              dependencies: [wayland_client_dep])
      test('smoke-decoration',
           find_program('sh'),
           args: [meson.current_source_dir() / 'smoke' / 'run_deco_smoke.sh',
                  woodland_bin.full_path(),
                  deco_probe.full_path(),
                  wctl_bin.full_path()],
           is_parallel: false,
           timeout: 90)
      # ext-background-effect：blur region 的双缓冲/裁剪/撤除。同一个探针，
      # 多走一遍 blur 参数；脚本按 [effects] 配置跑"开"和"关"两遍。
      test('smoke-bg-effect',
           find_program('sh'),
           args: [meson.current_source_dir() / 'smoke' / 'run_bg_effect_smoke.sh',
                  woodland_bin.full_path(),
                  deco_probe.full_path(),
                  wctl_bin.full_path()],
           is_parallel: false,
           timeout: 90)

      # treeland-appearance / -manager：bind 即推送、set_* 广播、越界 clamp、
      # 枚举外值忽略、只记录项不假装生效；再借 deco_probe 的顶层窗口确认
      # 标题栏高度真的改到了 SSD 边距上（不是只改了配置）。
      appearance_probe_srcs = ['smoke/appearance_probe.c']
      foreach p : [[meson.project_source_root() / 'protocols' / 'treeland' /
                    'treeland-appearance-unstable-v1.xml', 'treeland-appearance-unstable-v1'],
                   [meson.project_source_root() / 'protocols' / 'treeland' /
                    'treeland-appearance-manager-unstable-v1.xml',
                    'treeland-appearance-manager-unstable-v1']]
        appearance_probe_srcs += custom_target(p[1] + '-client-header',
          input: p[0], output: p[1] + '-client-protocol.h',
          command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
        appearance_probe_srcs += custom_target(p[1] + '-client-code',
          input: p[0], output: p[1] + '-protocol.c',
          command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
      endforeach
      appearance_probe = executable('appearance_probe', appearance_probe_srcs,
                                    dependencies: [wayland_client_dep])
      test('smoke-appearance',
           find_program('sh'),
           args: [meson.current_source_dir() / 'smoke' / 'run_appearance_smoke.sh',
                  woodland_bin.full_path(),
                  appearance_probe.full_path(),
                  wctl_bin.full_path(),
                  deco_probe.full_path()],
           is_parallel: false,
           timeout: 90)
    endif
  endif
endif
