# docky0.1.0 — 个人源码整合快照

此目录是当前开发源码的个人整合快照，文件夹名不表示已经发布或完成 0.1.0 产品验收。`dockui` 是实际 Windows 生产源码；`core`、`liquid glass test`、`prototype` 为相关源码和实验台，不是主程序入口。保留源码中原有未提交工作，不包含原工作树 Git 元数据、构建目录或用户配置。

## 无边框采集版本（本机）

`run_borderless.cmd` 或 `build-borderless\LiquidDesktopManager.exe` 启动带本机包身份的版本。2026-11-09 已为当前 Windows 用户注册身份包，用户已允许无边框采集；Windows 可能仍在其他程序采集同一屏幕时显示边框。先正常退出旧版再启动。`run_borderless.cmd` 在自身窗口中直接运行管理器，运行期间保持该窗口打开，关闭窗口或 Ctrl+C 会正常退出 docky（原先用 `start` 另开控制台会间歇出现 0xc0000142）。该版本依赖固定位置 `build-borderless`，不能直接移动。退出时管理器会正常关闭它启动的 Dock、菜单栏和守护进程；如果组件无响应，不强制结束。构建、授权和证书细节见 `dockui\identity\README.md`。

## 当前可运行版本

`build-current\LiquidDesktopManager.exe` 是 2026-11-09 从此目录最新源码构建并验证后复制的一套程序，包含电池平滑边缘及最新 Dock/Menu Bar 代码。先正常退出旧版，再启动它。`build-current` 是现成程序副本，不是 CMake 构建目录；以后运行下方脚本会更新 `build`，不会自动更新 `build-current`。新版三道圆角 Wi-Fi 在 `local_private_packs\apple-combined-2026-v3.dockypack`，需在 Workshop 导入并于全局槽选择。

## 构建

在 Windows 11 上双击 `build_docky.cmd`，或在终端执行它。需要 Visual Studio 2022 的 Desktop development with C++、MSVC x64、Windows 10/11 SDK（含 fxc），以及 Visual Studio 捆绑的 CMake 和 Ninja。默认输出到本目录 `build`，不自动启动程序、不安装扩展包，也不修改 Windows 任务栏或用户设置。若要指定其他输出目录：`build_docky.cmd -BuildDir "C:\path\to\build"`。

构建完成后统一入口是 `build\LiquidDesktopManager.exe`，设置窗口是 `build\LiquidDesktopSettings.exe`。请先确认其他 docky 实例的状态，避免重复运行。源码目录 README 中部分历史状态可能过时，以 `handoffs` 的最近交接和实际代码为准。

## 扩展包

`local_private_packs` 保存从本机 Workshop 复制的独立个人参考包；它们不会被构建、自动安装或嵌入主程序。Dock/顶部菜单包为当前选中的 `apple-dock-and-menubar-v4`；另保留本地顶部菜单参考包和最新设置图标包 `settings-reference-local-02`。打包时设置图标槽未选择自定义包，因此整合不改变该选择。可在 Workshop 手动导入。历史示例也保留在 `dockui/examples`。

部分资源来自 Apple 或用户提供的参考截图，许可和商标授权未确认，仅供本机个人参考，不能据此公开分发或放进安装包；独立成包并不代表获得再分发权。主程序有自带回退图标，不依赖这些个人包。

最新功能验证和已知限制参见 `handoffs/2026-11-09-codex-settings-continuous-lab-2f60.md`。尤其是 Settings 按下玻璃尚未证明与 Qt Glass Lab 完全一致。

## docky Desktop（桌面小组件与文件叠放，独立程序）

`run_desktop.cmd` 或 `build-desktop\DockyDesktop.exe` 启动。它与 docky（Dock + 菜单栏）分开安装、分开启动和退出，不依赖包身份，也不会被 Desktop Manager 启动。两者同时运行时可以联动：菜单栏 docky 菜单中有 Edit Desktop Widgets…，docky Desktop 的托盘图标显示在菜单栏的隐藏图标里，并跟随 docky 的外观设置。第三方接入（菜单栏状态项、小组件、脚本工具 `docky-ext.exe`、免代码清单）见 `dockui\docs\extensions-v01.md`，示例在 `build-desktop\examples\hello`。
