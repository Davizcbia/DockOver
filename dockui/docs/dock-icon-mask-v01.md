# Dock 图标遮罩

统一设置文件 %LOCALAPPDATA%/LiquidDesktop/settings.json 使用 schemaVersion=1，字段 dock.useMacIconMask；缺失默认 true。示例：{"schemaVersion":1,"dock":{"useMacIconMask":false}}。建议由支持安全写入的设置工具调用 liquid::settings::WriteBool；不要覆盖已有设置或未知字段。Dock 检测到设置变更后重建图标。测试可用 --desktop-settings <隔离 JSON 路径> 及会话参数 --mac-icon-mask on|off；后者优先于文件，仅作用于本次进程。

该效果只作用于 Dock 上传纹理：不更改源图标。方形不透明图标按连续超椭圆裁剪，透明或不规则图标按比例缩小并铺中性底板；边缘多点采样。PNG/ICO 使用现有解码路径；不影响 Launchpad 网格或 Windows 系统图标。外观尚需多 DPI 与实际图标目测。

V0.3：非方形输入先按比例放入方形纹理；最小遮罩纹理为 128 像素、边缘 4×4 覆盖采样。四角透明但边缘充分不透明的已有圆角底板优先保留；其他透明主体等比放入中性底板。图标缩放仍由 Dock 的现有 D2D 绘制使用同一纹理完成；关闭遮罩直接上传原始纹理。对真实 PNG/ICO、亮暗背景和多 DPI 的视觉效果仍需验收。
