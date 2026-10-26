if get_option('test').enabled()
  subdir('unit')
  test('smoke-headless',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_headless.sh',
              woodland_bin.full_path()])
  test('smoke-protocol',
       find_program('sh'),
       args: [meson.current_source_dir() / 'smoke' / 'run_protocol_smoke.sh',
              woodland_bin.full_path()])
endif
