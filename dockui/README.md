# DockOver — Windows Dock and Menu Bar

一个原生 Windows 11 Dock（V0.2 按 macOS 实测比例还原，见 `docs/v0.2-visual-gap-report.md`）：屏幕底部居中，显示真实应用图标，悬停时平滑放大并联动相邻图标，点击启动程序。不依赖 Qt，也不修改 `prototype/`（P0 捕获原型）或 Qt Liquid Glass Lab。

## 运行

推荐从统一入口构建并启动 Dock 与 Menu Bar：

```powershell
powershell -ExecutionPolicy Bypass -File dockui\build.ps1 -Run
```

构建后也可直接运行同一构建目录中的 LiquidDesktopManager.exe。三个程序须来自同一构建；独立 Dock 或 Menu Bar 已运行时，Manager 会提示用户先自行关闭，不接管或结束它们。通过 Menu Bar 的 Quit DockOver 退出托管组件。Windows 任务栏保持不变；当前生产守护尚未完成，Manager 即使收到 --hide-taskbar 也会拒绝隐藏。现有独立恢复工具仅用于保守处理旧记录，不能保证恢复所有异常状态。构建目录默认按工作树独立生成。工具链要求 Visual Studio 2022 MSVC、CMake 和 Ninja。

| 操作 | 效果 |
| --- | --- |
| 悬停 | 图标放大，邻近图标联动放大，名称标签淡入 |
| 左键点击 | 未运行：启动并弹跳；已运行：激活其窗口 |
| 拖放文件到 Dock | 固定该程序（exe、lnk 或任意文件） |
| 右键 | 打开、从 Dock 移除、打开文件所在位置、编辑配置、重新载入、退出 |
| Esc | 退出（需要 Dock 处于焦点，例如点过 Dock 之后） |
| F1 / F2 / F3 / F4 | 诊断叠加层、几何边框、重新载入配置、状态写入日志。正常外观不显示任何诊断内容 |

固定项保存在 `%LOCALAPPDATA%\LiquidDock\pinned.txt`，每行一项，格式为 `目标 | 参数 | 名称`，后两项可省略。目标可以是 exe、lnk，也可以是 `shell:AppsFolder\<AUMID>`（UWP 应用）；单独一行 `---` 表示分隔线。首次运行时，固定项取自资源管理器、Edge、用户的任务栏固定项、记事本、计算器和设置。日志写在同目录的 `dock.log`。命令行参数：`--config`、`--log`、`--diag`、`--exit-after <秒>`。

## P0.3 Liquid Glass 与 Glass Studio

- **材质**：底板默认使用 **Liquid Glass**，即 GPU 上独立编写的 HLSL。它依据 Glass Lab 的效果需求实现，参数名与 Lab 的 JSON 一致；它并不复制 Lab 的 GPL 源码，详见 `docs/p0.3-glass-lab-port.md`。**Smoked Glass**（V0.2）保留为回退材质。
- **背景**：WGC 捕获集成之前，背景是**模拟的**（静态壁纸、网格、渐变或纯色），所以看不到 Dock 后面的窗口。
- **Glass Studio**：右键 Dock，选择 **Glass Settings…**。修改实时生效；可以保存、加载、重命名、删除预设，可以恢复默认值或把当前参数保存为默认，可以粘贴 Glass Lab 导出的 JSON，可以开启持续渲染查看 FPS、CPU 和 GPU。参数保存在 `%LOCALAPPDATA%\LiquidDock\glass.json`。
- **命令行**：`--material liquid|smoked`、`--backdrop wallpaper|grid|gradient|dark|none`、`--debug-view 0-6`、`--continuous`、`--studio`、`--glass-config <path>`。这些设置只对本次运行有效，除非在 Studio 中保存。
- **验收脚本**：`verify_glass.ps1`（材质、调试视图、基准、Studio 全流程）。

## 结构

- `src/dock_model.*`：布局与动画模型，不依赖平台，单位为 DIP。放大效果采用余弦衰减；指针所在的内容保持不动，两侧图标向外扩展；弹跳和悬停过渡都按真实时间推进。
- `src/liquid_glass.cpp`、`shaders/*.hlsl`：Liquid Glass 材质（模糊纹理缓存 + 玻璃 Pass）；`src/backdrop_sim.*`：模拟背景；`src/glass_params.*`：参数模型、存储与预设；`src/glass_studio.*`：调参窗口。
- `src/material.*`：底板材质接口 `IPlateMaterial`。V0.2 只有临时的 `SmokeMaterial`（深烟灰、无模糊）；P0.3 的 GPU Liquid Glass 将实现同一接口，并读取 `PlatePaint.backdrop`。
- `src/renderer.*`：D3D11 设备，加 `CreateSwapChainForComposition`（预乘 Alpha、FLIP_SEQUENTIAL、帧延迟等待对象、最大延迟 1），加 DirectComposition 和 Direct2D/DirectWrite 绘制。图标阴影在加载时预渲染一次。
- `src/shell_items.*`：配置读写；通过 `IShellItemImageFactory` 取得 256px 真实图标（自动处理未预乘 Alpha，并裁掉透明边）；`ShellExecuteEx` 在工作线程中启动程序；用 EnumWindows 检测哪些程序在运行。
- `src/backdrop.h`：UI 与捕获模块之间的边界接口，见下一节。V0.1 只有 `NullBackdropSource`。
- `src/main.cpp`：Win32 窗口和消息循环。窗口样式为 `WS_EX_NOREDIRECTIONBITMAP | TOPMOST | TOOLWINDOW`。

## 动画时钟与按需渲染

- 静止时线程阻塞在 `GetMessage` 中，不呈现任何帧。
- 动画期间用 `MsgWaitForMultipleObjectsEx` 同时等待 DXGI 帧延迟对象和输入，并以 `Present(1,0)` 按显示器刷新率呈现。dt 由 QPC 真实时间计算。整个过程不使用 16ms 定时器。唯一的定时器是 1 秒一次的运行状态轮询，它不驱动动画。
- 窗口区域：静止时只覆盖玻璃底板及阴影，底板之外的点击会穿透到桌面；悬停或动画期间覆盖整个窗口，动画结束后恢复。

## UI 与捕获模块的边界

- 捕获模块（P0.2 WGC，由 Codex 负责）在自己的线程或 FrameArrived 回调中产生帧，并且要在捕获中排除 Dock 窗口本身。
- UI 每帧通过 `IBackdropSource::Latest()` 非阻塞地取最新帧。没有新帧时，复用上一帧或使用着色回退材质，绝不等待捕获。
- 捕获帧率不得限制 UI 帧率，重复帧也不得计为新的捕获帧。
- V0.1 的玻璃是半透明着色、光泽渐变加高光描边，**没有背景模糊或折射**。这部分属于 P0.3。

## V0.2 几何（DIP，等于 macOS pt）

- **静止**：图标 46，间距 14（pitch 60），底板高 76，内边距 15，squircle 圆角 22，距屏幕底 5.5。
- **放大**：最大 2.26，余弦衰减半径 4 格；格宽 = pitch×s，所以间距一起扩大；底部上移 (s-1)×0.11×46；底板高度不变。
- **分隔线**：1 px × 56，分隔格 28。
- **指示点**：直径 4，在底边上方 6。
- **标签**：胶囊 25 高，三角 16×6，尖端离图标顶部 16。
- **投影**：图标无投影。

## V0.1 已验证（2026-11-05，单屏 2560x1440、96 DPI、240Hz、RTX 5070 Ti Laptop）

验证脚本为 `verify.ps1`，使用真实光标和键盘输入，截图只截取 Dock 区域。

- Dock 显示在屏幕底部居中，可见 6、7 和 11 个真实图标（exe、lnk、UWP 应用）。
- 悬停放大、联动、名称标签：在截图中均可见。
- 点击启动记事本：产生了新 PID；运行指示点按预期出现，进程关闭后消失。
- 静止时，底板之外的点击可以穿透；悬停时整个窗口可以接收点击。
- 增加图标后窗口变宽并重新居中，透明效果没有被破坏。
- F1 叠加层可以显示和关闭；Esc 退出，退出码为 0。
- 动画帧率：在连续悬停扫过期间，日志记录为 238.5–240.5 fps；DXGI 统计为 238 次呈现对应 238 个 vblank。

## 未验证与限制

- 多 DPI 与 WM_DPICHANGED、多显示器、HDR、真实设备丢失、任务栏不在底部或副屏的情况、长时间运行与资源泄漏，均未测试。
- 启动程序时，偶尔出现最长 15–19ms 的帧间隔。
- 空闲之后的头几帧，DXGI 统计数据过期，vblank 差值不可信；只有连续动画窗口内的数字有效。
- `verify.ps1` 中图标的位置按 96 DPI 和默认尺寸计算。
- 只读取 `.lnk` 的固定项；UWP 应用固定到任务栏的项目不会自动导入。
- 首次运行可能导入用户自己的任务栏快捷方式，如果某个应用的图标本身留白较多，它会显示得较小。
