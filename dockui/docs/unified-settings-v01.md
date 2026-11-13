# Liquid Desktop 统一设置 V0.1

## 现有来源盘点

以下路径以 %LOCALAPPDATA% 为基准；可由 Dock 命令行覆盖的路径不会改写全局默认。“实时”指已有 UI 交互在运行时更新，不代表外部文件变更自动同步。

| 模块 / 路径 | 格式、读写位置 | 缺省值及运行时更新 |
|---|---|---|
| Dock：LiquidDock/pinned.txt（--config 可覆盖） | UTF-8 每行 target、参数、名称，--- 分隔；main.cpp 的 LoadItems，shell_items.cpp 的 LoadPinnedConfig / SavePinnedConfig | 缺失或空时按系统和内置应用生成默认固定项；拖放、移除、Keep、排序后保存；F3 可重新加载。 |
| Dock 几何、材质启动选项 | main.cpp / 模型中的代码与命令行；不属于持久化清单 | 默认由代码及 Manager 参数决定；--diag 等本次有效，F1 可切换诊断；Dock 默认材质/背景与 Manager 的 liquid/live 覆盖需区分。 |
| Launchpad：LiquidDock/launchpad.json（--launchpad-config 可覆盖） | JSON；launchpad.cpp 调用 launchpad_model.cpp 的 Load/Save | AppsFolder 发现列表形成默认布局；首开、拖排/文件夹变更保存并实时更新；损坏时现有实现尝试移至 .corrupt。 |
| Downloads：LiquidDock/downloads-stack-v1.txt（--downloads-settings 可覆盖） | LiquidDownloads 1 文本；downloads_stack.cpp LoadSettings/SaveSettings，main.cpp 调用 | Grid、Date、Stack；菜单变更实时生效并保存；路径默认 Windows Downloads，--downloads-path 可覆盖。 |
| 图标：LiquidDock/custom-icons.json（--icon-config 可覆盖） | 版本化 JSON；custom_icons.cpp CustomIconStore Load/Set/Remove/Save | 缺省为空，回退系统图标；右键变更实时生效；保存检测文件冲突。 |
| 玻璃：LiquidDock/glass.json（--glass-config 可覆盖） | JSON；glass_params.cpp GlassSettingsStore Load/Save；Studio 操作 | GlassLabDefaults() 及 GlassParameters；Studio 可实时更改，用户保存默认/预设才写入；Launchpad 读取 Dock 进程的玻璃参数。 |
| Menu Bar | 无专用设置文件；menu_bar.cpp 参数及 menu_surface.cpp 的代码常量 | 透明栏和固定菜单材质值；无用户设置持久化或外部实时更新。--log 是日志路径。 |
| Control Center | 无专用设置文件；control_center.cpp 的代码常量和 system_controls.cpp 系统接口 | 玻璃模块参数固定；系统音量/亮度/无线电由系统持有，面板可实时操作；系统当前状态不是本应用用户设置。 |
| 任务栏 | Manager --hide-taskbar 默认关闭；LiquidDesktop/taskbar-operation-v1.txt、taskbar-windows-v1.txt 为恢复记录 | 恢复记录独立于用户设置；不得作为普通偏好覆盖或迁移。 |

## 版本化清单（V0.1 历史快照；V0.2 以文末说明为准）

读取位置 %LOCALAPPDATA%/LiquidDesktop/settings.json。该路径默认不存在；缺失使用内存默认值。当前仅实现只读，所有六个分类可省略，非对象或 schemaVersion 不为数字 1 会整份回退。读取上限 1 MiB，文件损坏、不可读或未来版本保留原文件并回退，不重写。每个进程启动时独立读取，尚无运行时通知或一致快照。

示例：{"schemaVersion":1,"dock":{"diagnostics":false},"menuBar":{},"launchpad":{},"controlCenter":{},"appearance":{},"taskbar":{}}

当前唯一生效字段为 dock.diagnostics，缺省 false；--diag 可强制开启。其余对象只是预留，现有文件仍是各功能数据的唯一来源。Manager、Dock、Menu Bar 都调用同一只读解析器；Manager 仅报告读取状态，Dock 使用诊断值，Menu Bar 尚无字段消费。独立运行仍可读取同一路径。本轮不创建、写入或迁移任何统一设置文件，因此没有并发写入，也没有实现迁移备份、原子写入或跨进程锁。

后续写入必须先设计 Manager 单写者或跨进程互斥和版本/内容冲突检测；持锁下重新读取、验证版本、先备份旧文件，再在同目录创建唯一临时文件、刷盘并原子替换，失败保留旧文件与备份。迁移必须逐项验证、先备份来源、允许恢复，不能将任务栏恢复记录和 PID 纳入设置。再设计变更通知、运行中订阅与设置 UI；尚未实现这些能力。

build.ps1 -Run -Config/-Log/-Diag 现在经 Manager 的 --dock-config/--dock-log/--dock-diag 转发给 Dock；独立 Dock 参数保持不变。

## V0.2 安全写入与限制

WriteBool 对六个分类内的单个布尔字段进行持锁的读取、合并和原子替换；同目录 .lock 文件用无共享打开实现跨进程互斥，写入唯一临时文件并 FlushFileBuffers 后替换。该锁只约束遵守协议的程序，外部直接写入不受保护；同一字段的并发更新按持锁顺序后写者生效，不提供交互编辑的冲突提示。未知 JSON 的原始字节保留；不支持的版本、损坏内容、类型冲突均拒绝写入。当前只提供布尔写入 API，没有 UI 和通用设置编辑器。

BackupLegacy 为调用方显式创建源文件旁 .liquid-desktop-v02.bak，已有备份绝不覆盖；创建时源文件阻止共享写入，副本写入独立临时文件、刷盘后不覆盖式改名。已存在备份不代表与当前源一致，调用迁移程序必须先验证，不能盲目沿用。本轮未实现任何自动旧配置迁移、迁移标记、冲突判定或重复迁移功能，也不自动调用备份。上文盘点仍是旧配置来源，任务栏恢复记录不迁移。

Manager、Dock、Menu Bar 每秒检查文件身份、时间及大小的变化并重新读取、记录状态；检测只报告变化，不热应用参数。轮询可能合并短时间内多次变化，不等于可靠事件队列；不使用 Hook。文件版本检测单元测试通过，尚未对三个运行进程同时变更设置进行端到端测试。
