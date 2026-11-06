# protocols/treeland

从 treeland 生态抄过来的协议定义（只读副本，**不修改**）。

## 为什么放在仓库里

wlroots 与 scenefx 走 meson wrap 下载；协议定义不属于"依赖"，但它们是构建
产物（`wayland-scanner` 的输入）的一部分。放在仓库里是为了：

- CI（`archlinux:base-devel`）里没有 `treeland-protocols` 包，靠系统路径
  会让 treeland 协议在 CI 上直接编不出来；
- 版本可追溯——升级协议就是升级这几个文件，diff 里看得见。

## 来源

| 文件 | 上游 |
|---|---|
| `treeland-decoration-unstable-v1.xml` | `treeland-protocols/public/` |
| `treeland-wallpaper-shell-unstable-v1.xml` | `treeland-protocols/internal/` |
| `treeland-wallpaper-manager-unstable-v1.xml` | `treeland-protocols/dde/` |
| `treeland-compositor-action-unstable-v1.xml` | `treeland-protocols/dde/` |

安装后的路径是 `/usr/share/treeland-protocols/`。许可证是 **MIT**
（见每个文件头部的 `<copyright>` 块），与 GPL-2.0-only 兼容。

## 同步

```bash
cp /usr/share/treeland-protocols/treeland-decoration-unstable-v1.xml \
   /usr/share/treeland-protocols/treeland-wallpaper-shell-unstable-v1.xml \
   /usr/share/treeland-protocols/treeland-wallpaper-manager-unstable-v1.xml \
   /usr/share/treeland-protocols/treeland-compositor-action-unstable-v1.xml \
   protocols/treeland/
```

抄完跑一次 `meson compile -C build && meson test -C build`：这些协议都是
experimental，上游可能在**不升版本号**的情况下改接口，编译期或冒烟测试是
唯一能提前发现漂移的地方（`tests/smoke/deco_probe.c`、`tests/smoke/wallpaper_probe.c`、
`tests/smoke/action_probe.c` 按名字调请求）。
