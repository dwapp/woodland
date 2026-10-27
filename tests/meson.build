if get_option('test').enabled()
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
  test('smoke-capture',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_capture_smoke.sh',
              woodland_bin.full_path()],
       is_parallel: false,
       timeout: 60)

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
