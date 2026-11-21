# Liquid Glass 材质与背景延迟研究（2026-11-20）

范围：只读研究。没有修改代码，没有运行新的测量。目标是为 DockOver 的 Liquid Glass 材质改进和背景延迟优化定方向。

## 1. 现状（以代码为准）

### 1.1 材质与捕获已经解耦

- `src/backdrop.h` 定义了 `IBackdropSource` 和 `BackdropFrame`：一张 D3D11 纹理、屏幕矩形、frameId 和捕获时间。UI 只拉取最新帧，不等待捕获。
- `src/material.h` 定义了 `IPlateMaterial` 和 `GpuPaint`。`src/liquid_glass.cpp` 只依赖 `BackdropFrame`，不认识 DXGI 或 WGC。
- 结论：材质和捕获的边界已经存在，不需要重构。未来接入系统级合成，只需新增一种背景提供方式（第 5 节）。

### 1.2 GPU 管线（`liquid_glass.cpp`、`shaders/glass.hlsl`、`shaders/blur.hlsl`）

1. **ROI**：取窗口矩形外扩 96 DIP 作为采样区域。
2. **降采样**：以 q = 2 或 4 降采样到 RGBA16F 纹理。
3. **模糊**：两遍可分离高斯，最多 64 个采样点。
4. **缓存**：模糊结果按 frameId、ROI、q 和 sigma 缓存，背景不变就不重算。
5. **玻璃**：一个全屏三角形绘制玻璃，用 scissor 限制在板块周围。
6. **Shader 已有效果**：
   - 超椭圆 SDF 和有限差分法线
   - Snell 折射和 RGB 色散
   - 磨砂与清晰图像混合，饱和度和亮度调整
   - 深色/浅色主体色调，hairline 描边
   - 两种高光：固定上下高光（macOS 测量值），或随指针移动的动态光（Fresnel 加 Blinn-Phong）
   - 指针按压/悬停的透镜形变和微弱发光
   - 固定强度的投影，降低透明度模式
   - 高光压缩（knee）

### 1.3 背景链路（便携版默认 DXGI 复制，身份签名版用 WGC）

1. DWM 合成第 N 帧。
2. `desktop/duplication.h` 的后台线程从 `AcquireNextFrame` 拿到该帧，把脏矩形拷进共享整屏纹理。
3. 后台线程 `PostMessage(kMsgCaptureAvailable)` 通知 Dock 的 UI 线程，`main.cpp` 在 783 行处理。
4. `ProcessDuplication`（`backdrop_wgc.cpp:469`）：
   - 等待 `Fresh(roi)`。
   - **每一帧都 `CreateTexture2D` 新建一张 ROI 纹理**，再拷贝。
   - 某些情况要先跑一次 GPU 比较（fence 往返，日志记录平均 8 ms、最大 16–234 ms）。
5. 拷贝完成后置 `needsFrame_`，等交换链的 frame-latency waitable 信号，再执行 `Frame()`：模糊、玻璃、D2D 绘制，然后 `Present(1)`。
6. DWM 最早在下一个合成帧（N+1）显示，通常是 N+2。

现有日志只测了“捕获帧到发布”这一段：短测试运行中平均 5–15 ms，最大 20–56 ms（`%LOCALAPPDATA%\LiquidDock\dock.log`、`menubar.log`）。**端到端延迟（源内容上屏到 Dock 玻璃里显示该内容）从未测量过。**

## 2. 关于“0 ms、0 额外帧”的物理限制（关键结论）

任何用户态捕获，不论 DXGI 还是 WGC，拿到的都是 DWM **已经合成好的**第 N 帧。Dock 用它渲染后，最早只能进入第 N+1 次合成。所以：

- **捕获路线的理论下限是 1 个合成帧**（240 Hz 下 4.17 ms）。只能逼近这个下限，不可能到 0。
- 要做到 0 帧滞后，采样必须发生在 DWM 的同一次合成内部。不注入 DWM 的前提下，唯一的公开途径是 Windows.UI.Composition 的 `Compositor.CreateHostBackdropBrush()`（Win32 通过 `ICompositorDesktopInterop` 使用）。系统 Acrylic 就基于这条路。
  - **官方能力**：它是公开 API，在 DWM 内对窗口后方内容采样。效果图只能用系统内置效果：GaussianBlur、Saturation、ColorMatrix、Blend、Composite、Transform2D、Tint、Exposure、各类 Lighting 等。
  - **限制**：不能运行自定义 HLSL，也没有 DisplacementMap，所以**无法做真正的折射**。
  - **待验证（推测）**：
    - 在置顶 tool window 上能否工作
    - 关闭“透明效果”或开启省电模式后是否退化
    - 多显示器和 Wallpaper Engine 下的表现
    - GPU 成本
- 这对反作弊没有影响：使用的是公开 API，不注入、不改系统。

## 3. Apple 资料：官方说法与推测分开

来源：WWDC25-219、WWDC25-310、WWDC26-102、Adopting Liquid Glass、backgroundExtensionEffect 文档。这些页面的文字稿没有逐句时间戳，所以 8:01、10:44、12:05 只能对应到章节（Adaptivity 6:00、Principles 10:31）。WWDC26 的 22:26–23:25 段落无法凭文字稿定位，下面引用的是文字稿中 Liquid Glass 部分的原话。**所有资料都没有给出任何着色器数值。**

| 主题 | Apple 明确说的 | 我们的推测或实现方式 |
|---|---|---|
| 折射 | 透镜是材质的主要定义方式。材质会“bends, shapes, and concentrates light”。元素越大，透镜和折射越明显，散射越柔和 | 折射集中在边缘，并带聚光增亮。强度随尺寸变化 |
| 环境色溢出 | 侧边栏等大元素受周围环境影响，附近彩色内容的光会溢到表面，也会渗进阴影 | 对板块外侧一圈做大半径模糊采样，叠加到边缘带和阴影 |
| 动态高光 | 环境里的光源照在材质上，高光随几何形状响应。交互时光源会移动，光沿材质轮廓游走。有时响应设备运动 | 虚拟光源配合运动引擎动画，沿轮廓形成细高光线 |
| 交互发光 | 材质“illuminates from within”。光从指尖下方开始，扩散到整个元素，并波及附近的玻璃元素 | 以接触点为中心、半径随时间扩大的发光，带衰减包络。需要跨元素共享发光源 |
| WWDC26 更新 | 增加了“darkened edge”和更亮的 specular highlights，用来强化深度和分离感。调整了漫射，让复杂背景更易读。新增从 ultra clear 到 fully tinted 的滑块 | 内侧暗边带（吸收），在其外侧叠加更亮的高光线 |
| 自适应 | 多层材质持续调整色调、阴影和动态范围。小元素会随背景在亮暗之间翻转，大元素不翻转 | 用 ROI 平均亮度驱动，加迟滞和过渡动画 |
| 阴影 | 在文字上方时阴影加深，在纯浅色背景上变浅。元素越大，阴影越深 | 用局部亮度方差决定阴影不透明度 |
| Regular / Clear | 两者不要混用。Clear 没有自适应，需要配一层暗化层 | 做成两套参数预设 |
| 分组 | Glass 不能直接采样其他 Glass。用 Container 合成一次采样，提升性能 | 现有实现已在同一窗口内按 ROI 共用模糊结果 |
| 背景延展 | 把内容镜像复制到安全区外，再叠加模糊，主要用于侧边栏下方 | 适用于 DockOver 设置窗口的侧边栏，不涉及桌面 |
| 无障碍 | Reduce Transparency 让玻璃更磨砂，Increase Contrast 让元素以黑白为主并加对比边框，Reduce Motion 减弱效果并关闭弹性 | 目前只实现了 reducedTransparency |

## 4. 差距分析（`glass.hlsl` 对照 Apple）

1. **暗边加强高光（WWDC26）**：目前暗化只靠描边乘色，高光只有固定上下线或指针光。缺少内侧暗边带，也缺少沿整个轮廓、由光源方向加权的细高光线。
2. **环境色溢出**：没有实现。
3. **阴影自适应**：`shadowAmount` 是固定值。低成本做法：模糊纹理的 alpha 通道现在恒为 1，可以改存亮度平方，`var = blur(L²) − blur(L)²`，几乎不增加开销。
4. **交互发光**：目前只是指针处的微弱高斯，没有扩散过程，也没有跨元素传播。
5. **尺寸相关透镜**：`depth` 只随 bevel 变化，没有体现“越大越明显、越柔”。
6. **连续自适应**：`dark` 是按角色固定的布尔值。adaptive ink 每 2 秒用 CPU 采样一次屏幕，没有用 GPU 统计亮度，也没有迟滞过渡。
7. **无障碍**：缺 Increase Contrast，Reduce Motion 也没有接到材质。

## 5. 分阶段计划（每阶段都编译、测试并记录测量结果）

### P0 端到端延迟测量（先做，不改视觉）

在测量模式下，Dock 为每次 Present 记录三项数据：

- 所用背景帧的源合成时间：DXGI 的 `LastPresentTime`，WGC 的 `SystemRelativeTime`
- 本次 Present 的 QPC 时间
- 该次 Present 实际上屏的时间：交换链 `GetFrameStatistics` 的 `SyncQPCTime` 和 `PresentRefreshCount`

**滞后帧数 =（Dock 上屏时间 − 源合成时间）÷ 刷新周期。**

测试源用逐帧变色的测试窗口，以及 Wallpaper Engine 120 FPS 视频壁纸。测试分辨率：60/144/240 Hz。

### P1 捕获路线逼近 1 帧下限（复用现有代码）

1. 把 `PostMessage` 换成事件句柄，接入 `MsgWaitForMultipleObjects`，不再排在输入消息之后。
2. ROI 纹理改为 2–3 张的环形复用，去掉每帧 `CreateTexture2D`。
3. 进一步减少需要走 GPU 比较的路径，避免 fence 往返卡住新帧。
4. 改为准时渲染：等新背景到达后，在本合成周期截止前提交。基于 `DCompositionWaitForCompositorClock` 和合成时序，而不是 waitable 一放行就渲染。
5. 大半径模糊改用 mip 链或 dual-Kawase，降低 GPU 占用。

### P2 材质质量（参数进入 Glass Studio；新参数默认值保持现有外观不变）

暗边加高光线、阴影自适应、环境色溢出、交互发光扩散、尺寸相关透镜、带迟滞的连续明暗自适应、Regular/Clear 预设、Increase Contrast 和 Reduce Motion。

### P3 零帧原型（独立测试程序，不进 Dock）

验证混合方案：

- **主体模糊和色调**：由 HostBackdropBrush 提供，0 帧滞后。
- **折射**：只在窄边缘带使用捕获纹理，1 帧滞后，视觉上远不如整面明显。
- **高光、暗边、发光**：在自己的交换链里绘制，不依赖背景。

拿到实测数据后再决定是否接入 Dock。

### P4 系统级接口预留

新增一个背景提供方式的抽象，与 `IBackdropSource` 并列，例如 `HostBackdrop`。今后系统级合成器可以作为第三种实现接入。材质只依赖几何、参数和“背景来源”三项输入。

## 6. 兼容性与风险

- **反作弊**：现有方案只用 DXGI 复制和 WGC，与 OBS、Discord 的做法相同，不注入、不改驱动。P3 的 HostBackdropBrush 也是公开 API。
  - **待验证**：全屏游戏期间捕获是否完全停止（应当零 GPU 占用）。
- **多显示器**：`SharedDesktop::ForMonitor` 按显示器分别实例化。
  - **待测**：跨显示器 ROI，以及不同刷新率混合的环境。
- **Wallpaper Engine**：WGC/DXGI 拿到的是合成结果，可以包含动态壁纸。
  - **待测**：120 FPS 源下的唯一帧数，以及暂停、切换时是否出现黑屏。
