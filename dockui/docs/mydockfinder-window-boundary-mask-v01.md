# MyDockFinder：窗口边界与图标遮罩底色（逆向研究 v0.1）

执行者：Claude Code，2026-11-10。只读研究：未修改 MyDockFinder 的安装文件和配置，未复制其代码或资源。目标 `Dock_64.exe` SHA-256 `f7cc483e…34ff`（与 v0.1 自定义研究时的版本不同，程序已更新）。工具：REA + Ghidra 12.1.4（启动时限调到 40 分钟后分析完成）。

## 1. “吸附” = 窗口边界（`[normal] border`）

| 观察 | 证据 |
| --- | --- |
| 设置界面文字 `windowsborder=Enable window boundaries to prevent windows from covering the Dock`；同时提示启用时不能调整边缘距离、不能开自动隐藏 | `lang/English.ini` 第 529、820、821 行 |
| 实际配置键是 `[normal] border`；界面开关回调保存后调用 `FUN_1400d6a30(新值)`；改变 Dock 尺寸后若 border 开启也会调用一次 | `FUN_140071030`、`FUN_14006c390`（ev_c1a3…、ev_5b3d…） |
| `FUN_1400d6a30` 遍历每个显示器的 Dock：关闭时移除，开启时建立对象并调用 `FUN_140120dd0` | ev_9585… |
| `FUN_140120dd0`（类 `dock::MyDockBar`）：Dock 位置 1/2/3/4 映射为边 0/3/2/1（ABE_LEFT/BOTTOM/RIGHT/TOP），用 `FUN_140120c80` 算出的矩形调用 `FUN_140004060` | ev_c842… |
| `FUN_140004060`：`SHAppBarMessage(ABM_NEW)`、`ABM_SETAUTOHIDEBAR`(lParam 0)、`ABM_QUERYPOS`、`ABM_SETPOS`，回调消息 0xDDD5 | ev_b8c0… |
| `FUN_140004180`：已注册时再次 `QUERYPOS`/`SETPOS` 并按系统返回矩形移动 Dock | ev_9a95… |
| `FUN_140120c80`：预留厚度 = 显示器边 − 边缘距离 − Dock 尺寸（沉浸模式时有偏移） | ev_e52f… |
| `FUN_140120f80`（定时任务）：同一边若有任务栏，扣除其厚度后重新 SETPOS | ev_332b… |
| 附近的 `SetWinEventHook(0x8000–0x8003)` 只监听资源管理器（任务栏）进程的对象创建/销毁/显示/隐藏 | `FUN_140121e80`（ev_7689…） |
| `Dockmod64.dll` 是 `WH_GETMESSAGE` 钩子（导出 GetMsgProc*），不导入 SetWindowPos/MoveWindow；该类钩子看不到被“发送”的 WM_MOVING | 导出 ev_650c…、导入 ev_d3ce… |

结论（已检查范围内）：MyDockFinder 的窗口边界只是把 Dock 那一条注册为对应屏幕边的 AppBar，没有发现拖动过程中的阻力或吸附代码。用户感到的“吸附”应来自 Windows 对 AppBar/工作区的处理（推测，未实测）。

未检查：其它未被 `border` 路径调用的窗口移动代码；`SetWindowsHookExA/W` 两处调用（0x14012034a、0x1401204aa、0x140475aec）的钩子类型与用途。

## 2. 图标遮罩底色

- 字符串与资源：`iconmask`、`iconmasktype`、`icomask`、函数名 `Switchdark_icomask_`；`themes\*\icons\*_mask.png` 只针对个别图标（editico、settings）。
- `FUN_14012e130` 中名为 `icomask` 的 Composition 效果图（Backdrop 模糊 20、icovisual）属于 Dock 空闲沉浸背景，不是图标底色（`FUN_14012e960` 为其 500 ms 淡入淡出）。
- 直接证据：`cache.gpuico` 中已处理的 58×58 图标（本机 `iconmask=1`，深色）。非方形图标的主体占 6–51 像素；底板为均匀色，且 ≈ 图标自身按透明度加权平均色 × 0.37：

| 图标（缓存文件） | 图标平均色 | 底色 |
| --- | --- | --- |
| ca55ea6c… | 166,183,178 | 60,66,65 |
| 231577f9… | 196,182,175 | 72,67,64 |
| d15fdc64… | 168,168,168 | 62,62,62 |
| 7b755d5d… | 179,189,218 | 68,72,84 |
| 7f902ac9… | 165,168,173 | 67,68,70 |

例外：自带深色方形背景的图标（如 6565732…）底色为其背景延伸。浅色模式没有缓存样本，规则未知。

## 3. docky 的实现

- `main.cpp` `App::UpdateDockAppBar`：底部 AppBar，厚度 = Dock 板上沿到屏幕底；同一边已有任务栏时只补差额；自动隐藏时不预留；退出与窗口销毁时 ABM_REMOVE；ABN_POSCHANGED 0.5 s 节流。替换了上一轮“松手后把窗口抬到 Dock 上沿”的 WinEvent 钩子。
- `icon_mask.h` `MaskBackingColor`：深色 = 平均色 × 0.37（实测）；浅色 = 255 − (255 − 平均色) × 0.37（推测，非实测）。
- 本机验证：启动后主屏工作区为 y 30–1359（菜单栏 30 px；底部任务栏 48 px + Dock 补 33 px，共 81 px = Dock 板高 76 + 底边距约 5）。
