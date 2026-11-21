# DockOver Design System V0.1

共享 C++ 声明：src/design_tokens.h。所有尺寸以 DIP 表示，颜色为 sRGB；Windows 自带 Segoe UI，不依赖 SF Pro。数值是本项目的设计起点，不是 Apple 规范的固定数值。

- Typography：Title 26、Body 14、Secondary 12、Caption 11、Numeric 14 DIP；需在 DPI、字体缩放和本地化下检查截断。
- Spacing：4/8/12/16/24/32 DIP；Settings 专用布局和命中区域使用同一组常量。
- Radius：Window 16、Card 12、Button 8、Menu 12、Pill 14 DIP；系统窗口圆角目前由 DWM 控制，Window token 尚未驱动它。
- Colors：Light / Dark 均有 background、navigation、surface、primary、secondary、divider、accent 及状态颜色。Settings 目前只使用 Light；Dark 尚未接入系统主题和对比模式，不声称支持暗色。
- Interaction：hover、pressed、selected、disabled、focused 的语义颜色及透明度/焦点描边参数；Settings 当前只使用 selected，其他状态 token 是后续接口，不代表控件已具备交互或辅助功能。
- Materials：Navigation、Content、Popover、CompactControl、Fallback 的语义角色及建议透明度/背景需求，不会自动产生玻璃或捕获桌面。Settings 仍用 Direct2D 不透明绘制，不迁移 D3D11。

Settings 次要文字 #595D69 对白色约 6.57:1、对导航底色 #E6E7EB 约 5.32:1、对背景 #F4F4F7 约 6:1；数值为理论 sRGB 对比度，不等于已完成实际视觉、系统高对比与文字缩放验收。

## 迁移映射（仅建议，未改组件）

| 组件 | 当前来源 | 后续 token 候选 | 注意 |
|---|---|---|---|
| Dock | dock_model.h 几何；glass_params、renderer 材质与文字 | spacing、radius、typography、Navigation/Fallback | 保留 46/60 DIP 及动画比例，用户预设和点击区域不能被 token 静默改写。 |
| Menu Bar / 弹出菜单 | menu_bar.cpp 的 13 DIP / 固定白字；menu_surface.cpp 的固定玻璃参数 | typography、颜色可读性、Popover/Fallback、hover | 保持顶部栏全宽透明；浅背景自适应需独立设计和实测。 |
| Launchpad | launchpad.cpp 的 7×5、字体和玻璃参数，静态桌面快照 | typography、spacing、Popover/CompactControl | 网格可自适应；材质角色不会替换现有快照及捕获逻辑。 |
| Control Center | control_center.cpp 的 11–15 DIP 字号、状态色及每模块玻璃 | typography、interaction、CompactControl/Fallback | 先统一语义状态和可读性，再评估 WGC / 材质性能，保留系统操作行为。 |
| Settings | settings_window.cpp 引用共享亮色、排版、间距和卡片/开关半径 | 进一步接入 Dark、焦点、hover、UI Automation | 当前仅诊断开关，重启 Dock 生效；本轮不新增可用设置。 |

下一步：为键盘、UI Automation、Windows 高对比/文本缩放补齐运行时行为；明确如何切换亮暗色及无透明回退；跨组件以同壁纸、多 DPI、亮暗背景和实际 GPU 成本验证后再迁移 token。
