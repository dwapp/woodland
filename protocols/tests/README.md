# 测试专用协议

这里只放**测试客户端**要用的协议 XML，合成器侧不用它们。

| 文件 | 来源 | 为什么放在这里 |
|---|---|---|
| `wlr-layer-shell-unstable-v1.xml` | `subprojects/wlroots/protocol/`（随 wlroots 版本） | Meson 不允许 `tests/` 直接引用嵌套子项目里的文件（`Sandbox violation: ... from a nested subproject`），而背景模糊要覆盖面板/Dock 这类 layer surface，冒烟里必须有真客户端。 |

升级 wlroots 子项目时如果这个协议的版本变了，把同名文件重新拷一份过来即可；它只影响测试客户端，
合成器用的是子项目里自己编译的那份。
