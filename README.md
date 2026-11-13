# DockOver

Windows 11 上仿 macOS 风格的 Dock、菜单栏和 Launchpad，带液态玻璃（Liquid Glass）材质。另有独立的 DockOver Desktop（桌面小组件与文件叠放）。

下载：见 [Releases](https://github.com/Davizcbia/DockOver/releases)，解压后运行 `DockOver.exe`。程序自带更新检查（设置 → About）。

## 目录

| 路径 | 内容 |
| --- | --- |
| `dockui/` | Windows 主程序源码（C++20、Direct2D/Direct3D 11、DirectComposition）：Dock、菜单栏、Launchpad、控制中心、设置、Desktop、更新器 |
| `dockui/docs/` | 设计与实现文档 |
| `core/`、`liquid glass test/` | 液态玻璃材质的 Qt/QML 实验台和着色器（参考用，不是主程序入口） |
| `prototype/` | 早期合成原型 |
| `handoffs/` | 开发交接记录 |

## 构建

需要 Windows 11、Visual Studio 2022（Desktop development with C++、MSVC x64、Windows 10/11 SDK，含 fxc）以及 VS 自带的 CMake 和 Ninja。

```bat
build_docky.cmd
```

输出在 `build\`，入口是 `build\LiquidDesktopManager.exe`。只编译，不会启动程序或修改任何设置。

打包便携版（与 Releases 相同的布局）：

```powershell
powershell -ExecutionPolicy Bypass -File package_docky.ps1 -Release
```

版本号在 `dockui/CMakeLists.txt` 的 `DOCKY_VERSION`。

## 资源说明

本仓库不包含 Apple 的图标、字体或其他设计资源。个人参考用的扩展包（`.dockypack`）和描摹图标没有再分发授权，不在仓库中；程序在没有它们时使用自带图标。
