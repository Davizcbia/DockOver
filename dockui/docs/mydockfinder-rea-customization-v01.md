# MyDockFinder 自定义系统研究 V0.1（图标 DIY、Menu Bar Logo、Launchpad 密度）

执行者：Claude Code，2026-11-07。只读研究；未修改 MyDockFinder 安装文件或配置；未复制其代码、反编译结果或图标资源。
用户本机的应用名称、路径和 Logo 文件位置属于个人数据，本文只记录字段名、格式和统计数据。

## 0. 环境与方法
- 安装位置：`C:\Program Files (x86)\Steam\steamapps\common\MyDockFinder`（来自 Uninstall 注册表项 "MyDockFinder"；Steam 库 libraryfolders.vdf 中还有 `Z:\SteamLibrary`，未检查）。
- 配置目录：所有配置都写在安装目录内（不在 %APPDATA%）。语言文件提示 "If MyDockFinder is installed on the C: drive, run it as administrator"，说明它直接写 Program Files 下的文件。
- 主程序：`Dock_64.exe`，12,832,488 字节，SHA-256 `7d1e85eed148a4c8dd4a2a1002fd5de4b58e3f6067cdf849a9fd069f98389e8e`，x86_64 PE，MFC/C++。另有 dock.exe、Mydock.exe、Dockmod*.exe/dll。
- 方法：
  1. 只读检查配置和资源目录。
  2. 对缓存文件名做哈希假设验证（CRC32 / MD5）。
  3. 读取 `lang\English.ini` 中的界面字符串，确认功能。
  4. 用 REA 打开 Dock_64.exe：Ghidra 完整分析在 330 秒启动时限内未完成（provider_timeout），所以没有得到函数级的调用关系。
  5. 改为直接扫描 PE 中的 UTF-16 字符串，得到配置键在 .rdata 中的 RVA（见第 4 节）。
- 项目里此前没有 MyDockFinder 逆向文档；只有 `.ai/decisions.md` 中“视觉参考 MyDockFinder、独立实现”的决定，以及 Codex menubar 交接引用的公开用户指南。

## 1. Dock 图标 DIY 的实际机制
### 1.1 配置：`ico.ini`（另有 `ico_bak.ini` 作为完整备份，两者同为 11652 字节）
- 每个 Dock 项目是一个 `[icoN]` 段（N 从 1 递增，即 Dock 顺序）；本机共 28 段。观察到的字段：

| 字段 | 含义（观察） | 出现次数 |
|---|---|---|
| `tag` | 显示名 | 27 |
| `appname` | 小写 exe 文件名；特殊项如 `launchpad\|`、`controlpanel` | 27 |
| `filepath` | 启动目标：exe 路径，或 UWP 的 AUMID（`包族名!App`） | 22 |
| `realpath` | 解析后的真实 exe 路径（UWP 指向 WindowsApps 下的 exe） | 22 |
| `virtualpath` | Shell 身份：AUMID，或 `microsoft.windows.explorer` 这类 Shell ID | 19 |
| `icopath` | 自定义图标的绝对路径（PNG） | 5 |
| `UWP` | 1 = UWP 应用 | 12 |
| `delimiter` | 分隔条 | 1 |

- 自定义图标没有单独的覆盖表，而是写在该 Dock 项目所在段的 `icopath` 字段里，所以图标跟着 Dock 项目走。恢复默认就是删除 `icopath` 字段（按字段语义推断，未实际操作验证）。
- 应用身份：同一段同时保存 `appname`（小写 exe 名）、`realpath`、`virtualpath`（AUMID / Shell ID）和 `filepath`。运行中的窗口可以按 exe 名或 AUMID 匹配到固定项，从而共享图标。匹配的具体顺序没有在二进制中验证（推断，可信度中）。
- 窗口类名：配置中没有出现，没有证据表明它被用作身份标识。

### 1.2 图标来源和格式
- `png\`：7 个文件，均为 200×200 PNG，按小写 exe 名命名（如 `systemsettings.png`）；`icopath` 指向这里。这里既有程序自带的图标，也有用户放入的图标，区别不明。
- `runpng\`：1 个，按 exe 名命名，推测是“运行中状态”的图标变体（未验证）。
- `single icon\`：空目录；语言文件中有 "Single icon" 和 Workshop 上传图标，推测用于存放从 Workshop 下载的单个图标（未验证）。
- 格式：本机只见到 PNG。没有看到 ICNS 的证据，ICO 是否支持未确认。
- 主题图标包 `themes\<名称>\icons\`：用应用键名命名的 PNG（如 `chrome.png`、`calendar.png`），并有 `_dark` 深色变体（如 `calendar_dark.png`、`ControlPlanel_dark.png`；macOS Tahoe 26 主题 251 个图标中有 9 个 `_dark`）。
  - `icons\config.ini` 的 `[cal]` 段描述动态日历图标的字体、颜色和偏移。
  - `Icon Template\blank_dark.png / blank_light.png` 是制作图标用的底板。
  - `theme.ini` 的 `[config]` 段有 `name` 和 `PublishedFileId`（Workshop）。
  - 当前图标主题保存在 `config.ini [themes] dockicons=`。

### 1.3 图标编辑器（来自 English.ini，属于界面证据）
- `Icon Designer title=Icon Editor`
- `icosize=Icon scale`、`icooffsetx` / `icooffsety`（水平、垂直偏移）
- `iconshadow`（使用图标阴影）、`iconshell=Use icon mask`
- `selectthemes_icon=Icon theme`
- `workshopshare=Upload this icon to the Workshop?`
- 结论：单个图标支持缩放、偏移、遮罩、阴影，并可分享到 Workshop。这些参数保存在哪里没有找到：ico.ini 中没有看到对应字段，可能只在编辑后烘焙进 PNG。待验证。

### 1.4 缓存（文件名哈希已验证的部分标为“已确认”）
- `cache.ico\`：27 个文件，文件名为十进制数字，内容是原始 BGRA 像素（262144 = 256×256×4；160000 = 200×200×4），与 Dock 项目数量相近。
- `cache.gpuico\`：272 个原始像素文件，文件名为 32 位十六进制。大小集中在 13456（58×58×4，147 个）和 67600（130×130×4，105 个），另有 99792 / 126672 等。
  - 58 正好等于 `config.ini [normal] defaultsize=58`（Dock 静止尺寸），130 等于 `[Q27G4ZD] padico_size=130`（Launchpad 尺寸）。
  - 推断：按实际显示像素尺寸预先缩放并缓存，即多分辨率、DPI 感知的缓存（可信度中高）。
  - 文件名不是 ico.ini 或 Pinned 中任何路径字符串（UTF-8 / UTF-16 / ANSI）的 MD5（0/272 命中），可能还包含尺寸，或对内容取哈希。未确认。
- `cache.foldico\`：105 个，大小为 15376（62×62×4）或 13456（58×58×4），推测是 Launchpad 文件夹或文件类型的小图标缓存。
- `launchpad\png\`（45 个）和 `launchpad\cache_shell\`（126 个）：文件名为 `显示名_XXXXXXXX.png`，其中 XXXXXXXX = CRC32(UTF-16LE("`<安装目录>\launchpad\Pinned\<名称>.lnk`" 的完整路径))。**5/5 样本验证命中，已确认**。
  - cache_shell 应是 Shell 提取的图标，png 应是用户自定义或高清替换的图标（推断）。
- 缓存失效：语言文件中有 `resetini_padicocache=Launchpad icon cache` 和 `resetini_padico=Launchpad icon profile`，说明可以手动清理缓存和配置。是否按文件时间戳自动失效没有证据。
- DPI：`config.ini [normal] HDicons=1`、`iconmask=1`、`iconmasktype=0`；还有按显示器保存的 `defaultsize` 和 `maxsize`（见第 3 节）。

## 2. Menu Bar 左上角 Logo
- 配置：`config.ini [finder]` 中 `logopath=<图片绝对路径>`（本机是一个 97 字符的图片路径）、`logotype=1`。
- 界面：`logotype=Left button style`、`diylogo=Custom`，即左侧按钮样式可以从几种预设中选，“Custom”则使用 `logopath`。
- 主题可以自带 Logo：`themes\MacOS Golden Gate\logo\logo.png`（96×96，32bpp ARGB），Monterey 和 macOS Tahoe 26 主题也有 `logo\` 目录。
- 同段相关键：`autocolor=1`（推测按背景自动切换明暗颜色），`findermenu=1`，`finderheight=35`，`finderbutton_w=4`，`finderhidemenutime=20`，`finderbgtype=1`。
- 未确认：自定义图片是否被转成单色轮廓（autocolor 可能对模板图重新着色）、居中和缩放规则、是否即时生效。需要在二进制或真实界面中验证。
- 恢复默认：推测是把 logotype 改回预设值；没有发现单独的“重置 Logo”字符串。

## 3. Launchpad 密度和布局
- 设置（English.ini）：
  - `padicosize=Launchpad icon size`
  - `padicospacex=Icon horizontal spacing`、`padicospacey=Icon vertical spacing`
  - `hidepadicostring=Hide icon labels`
  - `padsortname=Sort by name`、`padsort=Auto-arrange icons`、`Grid=Grid`
  - `padicotype=Choose which icons to display in Launchpad`，选项为 `_normal` 已添加、`_startmenu` 开始菜单、`_desktop` 桌面应用；`padicotypetips` 提示切换来源需要重新设置图标
  - `padicoblacklist`（编辑已移除的开始菜单应用）
  - `customize_enabled_foldersize=Use a custom icon size in folder views`
  - `padscreenname=Same as Dock`（Launchpad 显示在哪个屏幕）
- 配置：按显示器型号分段，如 `[LG ULTRAGEAR]`、`[Q27G4ZD]`，键带分辨率后缀：
  - `defaultsize=62`、`defaultsize_2560x1440=62`、`maxsize=140`、`maxsize_2560x1440=140`（Dock 尺寸）
  - `padico_size=170`（另一台显示器为 130）
  - `padicospace_width=100`；`padicospace_height` 只在二进制中出现，本机配置里没有，可能取默认值
- 没有行数、列数设置。界面和配置中都没有 rows / columns 字段，推断网格由屏幕可用尺寸、图标尺寸和间距推算（可信度中高）。默认值和取值范围在二进制分析未完成的情况下无法确认。
- 布局持久化：`padconfig.xml`
  - 结构为 `<pad><pageN><icoN path="…lnk 或 url">显示名</icoN>…</pageN></pad>`；文件夹为 `<icoN name="文件夹名">` 内嵌子 `<icoN path>`。
  - 本机 4 页，分别为 31、33（含 3 个文件夹）、35、6 项，最大 35，符合 7×5 的容量（推断）。
  - 页面是写死的，改变密度后如何重新分页没有证据。
  - 缺陷：中日韩显示名按 ANSI 写出、没有正确转义，导致 XML 无法解析（本机实测：标签不匹配）。
- `padconfig.ini [pinned]`：“lnk 路径=显示名”的映射（自定义显示名）。
- `launchpad\Pinned\`：119 个 .lnk / .url。MyDockFinder 把 Launchpad 条目复制成自己的快捷方式集合。
- 搜索布局、拖动命中区域、多显示器分页规则：没有找到配置证据，未确认。

## 4. 关键证据与可信度
| 证据 | 位置 | 可信度 |
|---|---|---|
| ico.ini 字段 tag/appname/filepath/realpath/virtualpath/icopath/UWP/delimiter | 安装目录 ico.ini | 高（直接观察） |
| icopath 字符串 | Dock_64.exe .rdata RVA 0x84A1A0；virtualpath 0x84A188；realpath 0x84A158 | 高（字符串存在；引用函数未解析） |
| logopath / logotype / diylogo | .rdata RVA 0x842E78 / 0x845528 / 0x844438；config.ini [finder] | 高 |
| padico_size / padicospace_width / padicospace_height | .rdata RVA 0x857ED0 / 0x857EE8 / 0x857F10；config.ini 显示器段 | 高 |
| padconfig.xml / ico.ini 文件名 | .rdata RVA 0x840840 / 0x840688（另有 0x843262、0x844570） | 高 |
| cache.gpuico / cache.foldico / cache_shell | .rdata RVA 0x85C038 / 0x85C018 / 0x843394 等 | 高 |
| Launchpad 缓存名 = 显示名_CRC32(UTF-16LE 完整 lnk 路径) | 5/5 验证 | 高 |
| gpuico 按显示像素尺寸缓存（58、130 与配置一致） | 文件大小统计 | 中高 |
| 行列数由尺寸和间距推算 | 无 rows/cols 键 | 中高 |
| 7×5 每页 | padconfig.xml 每页最多 35 | 中 |
| 运行中应用与固定项的匹配顺序 | 无二进制证据 | 低（推断） |
| Ghidra 函数级调用链 | 分析超时，未获得 | — |

## 5. DockOver 当前能力对照
| 能力 | DockOver 现状 | MyDockFinder | 差距 |
|---|---|---|---|
| 单项自定义图标 | 已有：custom_icons.h 的 CustomIconStore（id→路径，存于 %LOCALAPPDATA%\LiquidDock\custom-icons.json），Dock 右键 Change Icon / Reset Icon，按 IconFileStamp 检测文件变化并刷新，WIC 解码（PNG、ICO、JPG 等，保持宽高比、预乘 alpha） | icopath 写在 ico.ini 的项目段 | Liquid 的独立覆盖表更好（不耦合 Dock 顺序，固定或运行都能复用）。缺少缩放、偏移、遮罩、阴影的单项编辑参数 |
| 稳定 ID | ApplicationItemId(target, args)；运行中匹配用 exePath / exeBase（shell_items） | appname + realpath + virtualpath(AUMID) | Liquid 缺少 AUMID 作为身份（UWP / 打包应用）；运行中的临时项是否用同一个 ID 取覆盖需要核对 |
| DPI 多分辨率缓存 | 每次构建图标时从源图解码（BuildIcons），没有磁盘缓存 | gpuico 按显示尺寸缓存原始像素 | 可以增加内存中按尺寸缓存；磁盘缓存可选 |
| 图标遮罩 | icon_mask.h 超椭圆遮罩，可在设置中开关 | iconmask / iconmasktype | 相当；缺少单项“不用遮罩”的开关 |
| 图标主题包 | 无 | themes\*\icons 按键名命名，带 _dark 变体，可导入导出，有 Workshop | 需要新增（V0.2 之后） |
| Menu Bar Logo | 默认四格矢量；LIQUID_MENU_LOGO / menu-logo.txt；PNG/ICO 按模板（alpha 轮廓）渲染；失败回退 | logotype 预设 + diylogo / logopath；主题可自带 logo | 缺少设置界面、预设选择和主题 Logo；Liquid 的模板渲染已经保证明暗可读 |
| Launchpad 网格 | launchpad.cpp:40 常量 kCols=7、kRows=5；布局为扁平有序节点，分页由推导得出；文件夹、拖动、重命名、合并新应用；JSON 原子替换写入 | 图标尺寸和横纵间距可调，按显示器和分辨率保存；行列数推算；页面写死在 XML | 需要新增密度参数和推导网格；Liquid 的扁平模型天然支持重新分页 |
| 配置导入导出 | 无 | loadini / oldini / Steam Cloud | 后续 |

## 6. 推荐的独立实现架构
- 图标：IconProvider 统一流程为“自定义覆盖 → 主题包（含 _dark 变体）→ 系统 Shell 图标”。
  - 覆盖：沿用 CustomIconStore，并增加可选的 `scale`、`offsetX/offsetY`、`mask`（继承 / 开 / 关）字段。JSON 每项从字符串扩展为对象，同时兼容旧格式的字符串值。
  - 身份：在 ApplicationItemId 的基础上，对打包应用增加 AUMID 规范化（`aumid:<小写>`）。运行中窗口先按 AUMID、再按 exe 路径找固定项 ID，再取覆盖。
  - 缓存：在内存中按 (id, 像素尺寸, 遮罩, 源文件时间戳) 缓存，失效后发出 IconsChanged 通知（复用现有 CheckCustomIconFiles 定时检查）。
- Menu Bar Logo：LogoSource { Builtin（四格）| Custom(path) | Theme(name) }，保存在 desktop_settings 中；兼容现有的环境变量和 menu-logo.txt（作为最高或次高优先级）。保持模板渲染。
- Launchpad 密度：LaunchpadDensity { Comfortable, Standard, Compact, Custom{iconDip, spacingX, spacingY} }，保存在 desktop_settings 的 launchpad 字段中，可按显示器覆盖（以显示器设备名或 EDID 名为键，参考 MyDockFinder 的按显示器分段）。
  - 列数 = floor((可用宽度 − 2×外边距) / (图标 + 横向间距))，行数类推，并限制在 [4..10]×[3..7]。
  - 分页由扁平节点列表推导，只改变每页容量，不改变节点顺序和文件夹。

## 7. 无法确认的问题
- Ghidra 没有完成分析，所以以下内容都没有二进制证据：图标加载入口函数、应用身份匹配顺序、缓存哈希的组成、Logo 是否单色化、autocolor 的具体行为、Launchpad 行列计算公式、取值范围和默认值、搜索布局、拖动命中区域、多显示器规则。
  - 下一次可以把 REA_GHIDRA_STARTUP_TIMEOUT_MS 设为 1500000 以上，或关闭耗时的分析器后重试，从上表的 RVA 开始追踪引用。
- ICO / ICNS 支持；单项编辑参数（缩放、偏移）的保存位置。
- Z:\SteamLibrary 下是否还有另一份安装。
