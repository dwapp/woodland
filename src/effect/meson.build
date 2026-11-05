# 圆角与背景模糊：默认构建只留状态与换算，effects 构建才真的画
effect_lib = static_library('effect',
  'effect.c',
  include_directories: woodland_inc,
  # 只是头文件与链接顺序：effect 读 SSD 的标题栏可见性与矩形，SSD 不反过来依赖它
  dependencies: scene_deps,
  link_with: [ssd_lib])
