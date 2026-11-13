# docky 扩展接口 V0.1：菜单栏状态项、桌面小组件、隐藏图标

本文是第三方接入 docky 的参考。两个宿主彼此独立、各自可选：

| 宿主 | 程序 | 接收 | 窗口类名（FindWindowW） |
| --- | --- | --- | --- |
| 菜单栏 | `LiquidMenuBar.exe`（docky 的一部分） | 状态项 `statusItem.*` | `DockyMenuBarExtrasHostV1` |
| docky Desktop | `DockyDesktop.exe`（独立程序） | 小组件 `widget.*` | `DockyDesktopHostV1` |

宿主未运行时所有调用都返回失败，不会出错。宿主启动时广播注册消息 `dockyHostStarted`（`RegisterWindowMessageW`），收到后重新发送即可。

## 三种接入方式

1. **C/C++**：复制单头文件 `sdk/docky_extension.h`（只依赖 `<windows.h>` 和 C++17 标准库）。
2. **脚本（PowerShell、Python、Node、批处理）**：`docky-ext.exe`。
   - `docky-ext send menubar @item.json` 发送一条消息；`send desktop` 同理。
   - `docky-ext serve menubar @item.json` 以本进程为事件接收者发送，然后把每个事件作为一行 JSON 打印到标准输出，直到进程结束；进程退出后状态项自动消失，宿主重启后自动重发。
   - `docky-ext gallery` 打开 docky Desktop 的小组件画廊。
   - 退出码：0 成功，2 用法错误，3 宿主未运行或消息被拒绝。
   - PowerShell 5.1 会去掉命令行参数里的双引号，JSON 请用 `@文件` 传入。
3. **免代码清单**：`%LOCALAPPDATA%\docky\extensions\<文件夹>\extension.json`。宿主每 5–10 秒重新读取，新增、修改、删除都会自动生效。示例见 `examples/extensions/hello`（状态项 + 由脚本提供数据的小组件）。开发时可用环境变量 `DOCKY_EXTENSIONS_ROOT` 指向其他目录。

## 传输

`WM_COPYDATA`，`dwData = 0x444B5831`（'DKX1'），`lpData` 为一个 UTF-8 JSON 对象，最大 256 KB。宿主处理成功返回 1，拒绝返回 0。宿主发出的事件以同样方式发送到消息里的 `notifyHwnd`（省略时使用 `WM_COPYDATA` 的 `wParam`）。`notifyHwnd` 窗口销毁后，对应的状态项或小组件内容会在几秒内移除。

## 菜单栏状态项

```json
{"v":1,"type":"statusItem.set","id":"com.example.timer","title":"25:00",
 "icon":"C:\\path\\icon.svg","template":true,"tooltip":"Pomodoro","notifyHwnd":123456,
 "menu":[{"id":"start","title":"Start"},{"separator":true},{"id":"stop","title":"Stop","enabled":false,"checked":false}]}
{"v":1,"type":"statusItem.remove","id":"com.example.timer"}
```

- `id`：1–128 个字符，限 `A-Z a-z 0-9 . - _`，推荐反向域名。
- `title`：可选短文本，最多 32 个字符，建议 12 个以内。`icon`：PNG/ICO/SVG/JPG，正方形，至少 32 px。两者至少要有一个。
- `template`（默认 true）：单色模板图，按菜单栏墨色重新着色，深色或浅色主题都清楚；设为 false 时保留原色。
- `menu`：最多 50 项。有菜单时点击状态项打开 docky 统一样式的菜单；没有菜单时发送点击事件。
- 事件：`{"type":"statusItem.clicked","id":"…","button":"left|right"}`，`{"type":"statusItem.menu","id":"…","item":"<菜单项 id>"}`。
- 安全：推送的消息不能让 docky 运行程序（会忽略 `run`）。只有用户自己放进扩展目录的清单可以用 `run`/`args` 启动程序，工作目录为清单所在文件夹。

## 菜单栏右侧的布局与自定义

从右到左依次为：时钟、内置项（控制中心、电池、Wi-Fi、声音，间距取自 macOS 实测）、扩展状态项、用户提升到菜单栏的托盘图标、隐藏图标按钮（∨）。

- **Menu Bar Items** 子菜单位于 docky（左上角标志）菜单和隐藏图标弹窗底部，可以打开或关闭每个内置项、每个扩展项和每个托盘图标，也可以关闭隐藏图标按钮本身。
- 规则：内置项默认显示；扩展项默认显示，可移入隐藏图标；托盘图标默认在隐藏图标里，可提升到菜单栏。任何隐藏项都在隐藏图标弹窗里，不会无法访问。
- 设置保存在 `%LOCALAPPDATA%\LiquidDesktop\menubar-items.json`（`builtins`、`hiddenIcons`、`trayHost`、`shown`、`hidden`），可手工编辑；`trayHost:false` 会完全关闭托盘接管。

## 隐藏图标（Windows 通知区域图标）

菜单栏在独立线程上创建一个隐藏的顶层 `Shell_TrayWnd` 窗口。各程序的 `Shell_NotifyIcon` 通过 `FindWindow("Shell_TrayWnd")` 找到它（它在 Z 序中位于 Explorer 任务栏之上，每 250 ms 重新确认）。每条消息都原样转发给 Explorer，并把 Explorer 的返回值交回调用者，所以 Explorer 的托盘和 AppBar（包括 docky 自己的 Dock 和菜单栏）照常工作，docky 只是保留了一份图标列表。启动时广播 `TaskbarCreated`，让程序重新添加图标。Explorer 重启后再次广播。docky 退出或崩溃后窗口随之消失，程序会直接与 Explorer 通信，不会持久修改任何系统状态。

- 左键点击一行会把单击转发给图标所有者（NOTIFYICON_VERSION_4 时使用 `NIN_SELECT`），右键转发为上下文菜单（`WM_CONTEXTMENU`）。转发前对该进程调用 `AllowSetForegroundWindow`。
- `Shell_NotifyIconGetRect` 返回图标在菜单栏中的位置，或在打开的弹窗中的对应行，便于程序把自己的浮出窗口放在旁边。
- Windows 11 自带的音量、网络、电池图标不是 `Shell_NotifyIcon` 图标，不会出现；docky 有自己的对应项。
- 已知限制：极短时间内，Explorer 把任务栏提到最前时，个别图标更新可能直接发给 Explorer，从而漏收；之后的更新会自动补上。不响应 `TaskbarCreated` 的程序在 docky 启动前已添加的图标不会出现，直到它下次更新图标。

## 桌面小组件（docky Desktop）

```json
{"v":1,"type":"widget.set","id":"com.example.todo","title":"To Do","size":"small","template":"list","accent":"#FF9F0A",
 "notifyHwnd":123456,"data":{"items":[{"title":"Ship","checked":true},{"title":"Docs","detail":"today","checked":false}]}}
{"v":1,"type":"widget.remove","id":"com.example.todo"}
{"v":1,"type":"widget.gallery"}
```

尺寸（DIP，按用户的 macOS 26 桌面实测）：`small` 164×164，`medium` 346×164，`large` 346×346。圆角 22；间距（默认 18）、吸附距离（默认 12）和边距（默认 16）在 Widget Settings 中调整。网格单位为（164 + 间距）/ 2，从边距起算。拖动时，小组件距离邻居的对齐边或间距线在吸附距离以内会被吸住；松手后以缓出动画落到最近的空闲位置。调整间距或边距后，已有小组件保持网格位置并按新间距重新排列。

| template | data |
| --- | --- |
| `text`（默认） | `headline`（大号数字或短语）、`body`（自动换行）、`footnote` |
| `list` | `items`: `[{title, detail?, checked?}]`，行高 26；点击发送所在行号 |
| `gauge` | `value` 0–1、`label`（环中心文字，默认百分比）、`detail` |
| `image` | `path`（清单中可用相对路径）；图片以 cover 方式铺满，标题叠在底部 |

- 第一次注册的小组件会自动放到桌面一次，之后由用户在画廊或右键菜单中添加或移除。点击事件：`{"type":"widget.clicked","id":"…","item":"<行号或 -1>"}`。
- 小组件自己的设置（与 macOS 的"编辑小组件"相同）：`widget.set` 或清单里可以带 `settings`，最多 12 项：
  ```json
  "settings":[{"key":"sound","title":"Play sound","type":"toggle","value":true},
              {"key":"length","title":"Length","type":"choice","options":["15 min","25 min","50 min"],"value":"25 min"},
              {"key":"reset","title":"Statistics","type":"button","options":["Reset"]}]
  ```
  `type` 为 `toggle`（开关）、`choice`（2–4 个选项的分段控件，`value` 可写序号或选项文字）或 `button`（`options[0]` 是按钮文字）。有设置的小组件，右键菜单会出现 Edit "<名称>"…，在小组件旁打开只属于它的液态玻璃设置页。用户的选择由 docky Desktop 保存（desktop.json 的 `extensionSettings`）。每次改动发送 `{"type":"widget.setting","id":"…","key":"…","value":true|<序号>|null}` 给 notifyHwnd；小组件重新注册时，与声明值不同的已保存值会立即补发。清单小组件没有窗口可通知：改动后会重新运行 `command`，当前选择通过环境变量 `DOCKY_WIDGET_SETTINGS`（JSON 对象，例如 `{"sound":true,"length":1}`）传入。
- 清单小组件可以提供 `command` 和 `refresh`（秒，最少 10）：宿主在扩展文件夹中运行该命令（不显示窗口，最多 10 秒），把标准输出的 JSON 对象作为 `data`。超时只结束这个子进程。

## 叠放与玻璃

- 叠放默认按 macOS 的方式在桌面上原地展开：文件依次占用后面的桌面格子。单击选中，双击打开，可以拖出，右键是 Explorer 菜单，再次点击叠放即可收起。在托盘菜单 Open Stacks 中可以改为 In a Glass Panel，在叠放旁边打开液态玻璃面板。
- 叠放开启时会隐藏 Explorer 自己的桌面图标，避免文件显示两次。隐藏只用 `ShowWindow`，每 2 秒检查一次；退出或崩溃时恢复，不修改 Explorer 设置。
- docky Desktop 的菜单和叠放面板使用 docky 的 Liquid Glass 材质，并以只读方式沿用 docky 的玻璃参数（Glass Studio 中的菜单栏弹窗项）。玻璃背后的画面由 docky Desktop 自己合成：清晰的壁纸，加上它自己的小组件和叠放窗口。因此不需要屏幕捕获、不会出现捕获边框，也不需要包身份。玻璃下方如果有其他程序的窗口，不会显示在玻璃里。

## 小组件外观、画廊与设置

- 默认为 Solid 样式，对应 macOS 的全彩样式：时钟用浅色底板，其他小组件用主题底板；也可以改为 Glass（模糊的壁纸）。内置小组件包括数字时钟、模拟时钟、日历（今日事件、明天及之后的事件）、电池（2×2 圆环）和系统（CPU、内存、磁盘）。
- 编辑小组件时，屏幕底部会出现液态玻璃画廊：左侧是搜索框和来源列表（All Widgets、Clock、Calendar、Batteries、System 及第三方），右侧是各尺寸的预览。可以把预览拖到桌面或直接点击添加；拖回画廊即取消。画廊打开期间，每个小组件左上角显示删除按钮。点 Done 或按 Esc 关闭画廊。
- 编辑小组件时，如果有程序窗口挡着，会先显示桌面（等同于 Win+D），关闭画廊或设置后再把窗口还原，和 macOS 的编辑方式一样。原因是玻璃只能显示 docky Desktop 自己合成的画面（壁纸和它自己的窗口），压在别的窗口上时看起来像把那个窗口穿透了。
- 设置分成两类，都是液态玻璃面板，背景每 100 ms 重新采样，后面的窗口移动时会跟着变化：
  - Widgets & Stacks Settings（托盘菜单、画廊、小组件右键菜单，或 `DockyDesktop.exe --settings`）：样式、间距、吸附距离、边距、玻璃浓度，以及叠放的开关、展开方式、分组方式和隐藏桌面图标。
  - 各小组件自己的页面（右键 Edit "Weather"… 等，在小组件旁打开）：天气是单位和城市，电量是设备勾选，日历是订阅；第三方小组件显示它声明的 `settings`。
- 这两类面板的玻璃材质可以在 Glass Settings（玻璃调参）的 Surface 下拉框中选 "Widget gallery & settings" 单独调整，保存在 glass.json 的 `surfaces.widgetPanels`，docky Desktop 会实时应用。
- 日历事件来自 `%LOCALAPPDATA%\DockyDesktop\calendars.txt`：每行一个 iCalendar（.ics）链接或文件，可以在后面加 `#RRGGBB` 指定颜色。Outlook、Google、iCloud 和 Canvas 都提供这种链接。程序启动时和之后每 30 分钟拉取一次，支持全天事件、UTC 和本地时间，以及基本的 RRULE（DAILY、WEEKLY 加 BYDAY、MONTHLY、YEARLY，COUNT、UNTIL、INTERVAL）。Windows 自带的日历没有供未打包程序读取的接口，所以采用订阅链接。

## 天气

- Weather 小组件有小、中、大三种尺寸，布局与 macOS 相同：小号是地点、温度、天气状况和最高/最低温度；中号多一行逐小时预报；大号再多 6 天预报，带同一刻度的温度区间条。背景按天气和昼夜着色，图标由代码绘制。
- 位置默认用 Windows 定位（需要打开"设置 > 隐私和安全性 > 位置"中的"允许桌面应用访问你的位置"）。也可以在天气小组件右键 Edit "Weather"… 中点 Set City…，在 `%LOCALAPPDATA%\DockyDesktop\weather-city.txt` 里写一个城市名，写了城市就优先使用城市。
- 预报来自 Open-Meteo（免费，无需账号或密钥）；自动定位时的地名来自 OpenStreetMap Nominatim。只发送约 1 公里精度的坐标，或你填写的城市名；每 15 分钟刷新一次。温度单位可以在设置中选 °C 或 °F，默认跟随 Windows 的度量单位设置。

## 配件电量

- Batteries 小组件和菜单栏电量项显示这台电脑和配件。蓝牙键盘、鼠标、耳机、手写笔通过设备属性 `{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2` 获取电量，也就是"设置 > 蓝牙"里显示的数值；Xbox 和 XInput 手柄通过 XInputGetBatteryInformation 获取。设备类型根据同一物理设备的其他节点判断（例如 Mouse、Keyboard、AudioEndpoint 类）。只剩蓝牙节点在线的设备视为已断开，不显示。
- 2.4 GHz 接收器和 USB 配件直接从 HID 接口读取（只读查询，不需要驱动或厂商软件）：Razer（VID 1532，91 字节特性报告，电量命令 0x07/0x80，充电 0x07/0x84，已在 HyperSpeed 接收器上实测）；Logitech Unifying / Bolt / Lightspeed（HID++ 2.0 长报告，特性 0x1004、0x1000 或 0x1001，名称和类型取自 0x0005，尚未实机验证）；以及在特性报告中提供标准 Battery Strength（用法页 0x06，用法 0x20）的其他设备。Razer 鼠标休眠时接收器回复超时，此时保留最后一次读数最多 45 分钟（接收器需仍插着）。
- 菜单栏：每个配件是一个独立的状态项（id `com.docky.desktop.battery.<名称>`），图标是设备模板图，标题是百分比；菜单显示名称、电量（充电时注明），以及 Battery Settings…（打开 docky Desktop 设置）和 Bluetooth & Devices Settings…。
- 电量小组件右键 Edit "Batteries"…（或菜单栏电量项的 Battery Settings…）：总开关 Show in menu bar，下面每个设备一行，分别勾选是否在小组件（Widget）和菜单栏（Menu Bar）中显示，按设备名称保存在 desktop.json 的 `widgetPrefs.batteryWidgetOff` / `batteryMenuOff`。这台电脑只有小组件选项（菜单栏有自己的电池项）。`DockyDesktop.exe --list-batteries <文件>` 可以输出检测结果，用于诊断。

## 两个程序的联动

- docky Desktop 是独立的程序（`DockyDesktop.exe`，单实例，设置在 `%LOCALAPPDATA%\DockyDesktop\desktop.json`），docky 的 Desktop Manager 不会启动它，两者可以分别安装和退出。
- docky 运行时，docky Desktop 跟随 docky 的外观设置（Settings > Appearance），否则跟随系统主题。
- docky Desktop 运行时，菜单栏的 docky 菜单会出现 **Edit Desktop Widgets…**；docky Desktop 自己的托盘图标会出现在菜单栏的隐藏图标中，可以在那里打开它的菜单。
- 再次启动 `DockyDesktop.exe` 会打开正在运行实例的小组件画廊。
