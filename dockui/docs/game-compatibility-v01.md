# Game Compatibility Mode v0.1（2026-11-20）

## 1. 功能

开关位于 Settings › Menu Bar › **Game compatibility**（设置键 `dock.gameCompatibility`，默认关闭）。开启后，以下两种情况任一出现，DockOver 都会暂停：

1. **原有的全屏游戏检测**（Menu Bar）：前台为全屏窗口，并且处于 D3D 独占全屏，或游戏占用了鼠标指针。
2. **新增的游戏列表**：Settings › Menu Bar › Games list › **Edit list…**，打开设置文件旁的 `game-compat.txt`。
   - 每行写一个 `.exe` 名，`#` 开头为注释。
   - 首次打开时自动创建模板，示例全部为注释状态。
   - 列表中的程序**正在运行**时生效，不要求全屏或在前台。

暂停期间：

| 组件 | 行为 |
|---|---|
| Dock | 隐藏；关闭弹出菜单和 Downloads Fan；停止桌面捕获，2 秒后释放 DXGI 复制；暂停 Genie 覆盖层和 Win 键钩子；每秒的后台轮询降到每 5 秒一次 |
| Menu Bar | 隐藏；关闭弹出面板；卸下位置钩子和原始鼠标输入；禁止从顶部边缘呼出 |
| DockOver Desktop | 小组件暂停刷新和数据轮询 |
| Control Center | 没有直接关闭接口；当游戏夺走焦点时，它会按原有逻辑自行关闭 |

列出的程序退出后，约 1–2 秒内全部恢复。

## 2. 检测方式（遵守反作弊相关约束）

- 每个进程每 2 秒取一次 Toolhelp 进程快照，只比较可执行文件名（`game_compat.h`）。
- **不打开任何进程句柄，不读写其他进程，不检测反作弊进程本身**，只匹配用户写入列表的名称。
- 不注入、不使用内核钩子、不修改系统文件或驱动，也不隐藏 DockOver 自身。

## 3. 实测（2026-11-20，独立测试 Dock 加设置副本，用 `lab.exe` 充当“游戏”）

| 事件 | 结果 |
|---|---|
| 游戏启动 | 1.8 s 后检测到；Dock 隐藏，捕获停止；再过 2 s 释放 DXGI 复制 |
| 游戏退出 | 1.3 s 后恢复，重新开始捕获 |
| 单元测试（`LiquidDesktopSettingsTest`） | 覆盖注释、BOM、完整路径、大小写、模板生成、按名称找到运行中的进程 |

**未实测**：

- 在真实的 PUBG / Fortnite / Valorant 中运行（本机未运行这些游戏）。
- Menu Bar 与 DockOver Desktop 在列表游戏下的实际表现：代码已编译，单元逻辑与 Dock 相同，但没有做端到端测试。

## 4. 反作弊公开信息（2026-11-20 调研，只读取官方页面）

| 来源 | 官方明确说明 | 未找到 / 无法确认 |
|---|---|---|
| **BattlEye**（PUBG 使用）FAQ 与支持页 | 一般只因真正的作弊而封禁；非作弊覆盖层“generally supported”；“No one is banned for using non-hack programs (like Fraps, overlays, etc.)”；可能会把某些程序（例如宏工具）踢出（不封禁）；会阻止带已知漏洞的内核驱动；游戏目录中的 d3d9/dxgi/dsound.dll 会触发拦截提示；对合法文件被拦截，可通过联系页反馈 | 没有提到截屏 / 录屏、DXGI 桌面复制、Dock 或任务栏替代工具；没有白名单流程。support.pubg.com 返回 403 |
| **Easy Anti-Cheat**（Fortnite 使用） | easy.ac 只说明产品定位、通过启发式与玩家举报检测、开发者通过付费渠道联系、提供申诉入口 | “Software & System Compatibility” FAQ 的正文没有读到；Epic 帮助中心返回 403；EOS 文档页为空（需要 JavaScript）。**对覆盖层或捕获工具没有任何公开结论** |
| **Riot Vanguard**（Valorant 使用） | Vanguard 包含客户端和内核驱动；会阻止“vulnerable vectors”并给出 Windows 通知；“VAN: Incompatible Software”要求卸载相关软件或驱动；VAN: RESTRICTION 涉及 TPM、Secure Boot、HVCI 等系统要求；唯一的反馈途径是提交工单并附日志 | 没有提到覆盖层、录屏或置顶窗口；“Vanguard Restrictions”页面返回 404 |

**结论**：三家都没有公开说明 Dock 类工具或桌面复制与其兼容或不兼容。DockOver 只使用公开 API，不注入、不带驱动、不修改游戏目录，这符合 BattlEye 公开说明中“非作弊程序”的特征。但这只是推测，**不能保证兼容**。

建议的保守用法：

- 打开 Game compatibility，并把常玩的反作弊游戏加入列表，让 DockOver 在游戏运行期间不捕获、不显示覆盖层。
- 如果被拦截，按各厂商的官方渠道反馈：BattlEye 联系页；EAC 申诉或商务渠道；Riot 提交工单并附日志。
