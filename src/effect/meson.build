# 圆角与背景模糊：scenefx 恒在，画不画由 [effects] 运行期决定
effect_lib = static_library('effect',
  'effect.c',
  include_directories: woodland_inc,
  # 只是头文件与链接顺序：effect 读 SSD 的标题栏可见性与矩形，SSD 不反过来依赖它
  dependencies: scene_deps,
  link_with: [ssd_lib])
