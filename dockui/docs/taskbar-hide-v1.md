# Windows taskbar hiding (production v1)

DockOver can fully hide Windows' own taskbar while it runs. This is separate from the Windows setting
"Automatically hide the taskbar", which DockOver never changes.

## Components and responsibilities

| Component | Role |
|---|---|
| `LiquidDesktopManager.exe` | Only owner of taskbar state (single-instance gate `Local\LiquidDesktopManagerV05SingleInstance`). Hides/shows via `ShowWindow` on Explorer's `Shell_TrayWnd` / `Shell_SecondaryTrayWnd`, registers the global shortcut, publishes `%LOCALAPPDATA%\LiquidDesktop\taskbar-status-v1.txt`. |
| `docky-taskbar-guardian.exe` | Started by the Manager after the Dock's first frame. Signals readiness; **nothing is hidden before that**. Restores the taskbar if the Manager exits without a clean stop, and on "Restore now". Logs to `taskbar-guardian.log`. |
| `docky-taskbar-restore.exe` | Standalone user tool. Without a running Manager it restores directly; with one, it asks the guardian. |
| Windows Explorer | Owns the taskbar windows. DockOver never changes Explorer settings, the registry, `ABM_SETSTATE`, the work area or the Explorer process. |

All three DockOver parts share the write-ahead record `%LOCALAPPDATA%\LiquidDesktop\taskbar-windows-v1.txt`
(and the named lock `Local\LiquidDesktopTaskbarRecordV1`). Only windows DockOver hid, that still belong to the same
Explorer process and are still hidden, are ever shown again. Corrupt, Pending or Conflict records and existing
`.tmp` files are preserved and never acted on.

## Behaviour

- Hide on startup (Settings › Windows Taskbar) is a preference; it is applied once when DockOver starts and again only
  when Explorer recreates its taskbar (restart / new monitor) while DockOver keeps it hidden.
- Showing the taskbar by any means (shortcut, Settings, the user, another program) is respected and never undone
  automatically.
- Default shortcut Ctrl + Alt + T (`RegisterHotKey`, no keyboard hook). It can be recorded, cleared or disabled in
  Settings; conflicts with other apps are reported there. Win-key, Alt+Tab-style and F12 shortcuts are refused.
- Exit (Quit, Dock closed, Ctrl+C/console close) restores the taskbar first. If the Manager crashes, the guardian
  restores it. If the guardian dies while the taskbar is hidden, the Manager restores it and disables hiding.

## Shortcut recording (v2)

While Settings records a shortcut it holds the lease `Local\LiquidDesktopHotkeyRecordingV1` and pings the Manager,
which unregisters its hotkey, drops queued `WM_HOTKEY` messages and acknowledges before keys are captured. Pressing
the current shortcut is therefore recorded, never toggles the taskbar. The lease ends on success, Esc, Backspace /
Delete (clear), 20 s timeout, leaving the page, window deactivation, window close or a Settings crash (abandoned
mutex); the Manager then re-registers from the saved settings. Only one Settings window can record at a time.

## Free work area (v2, opt-in, default off)

Settings › Windows Taskbar › "Free work area when taskbar is hidden" (with confirmation). While DockOver hides the
taskbar, each affected monitor's work area is extended by exactly that taskbar's thickness on its edge using
`SPI_SETWORKAREA` without `SPIF_UPDATEINIFILE` (never saved to the profile). Measured on Windows 11 (2026-11-08):
with the `WM_SETTINGCHANGE` broadcast Explorer re-derives its hidden appbar's strip within ~0.5 s, so DockOver applies
its value without the broadcast and restores with it. Windows maximized to the previous work area (exact frame
shape) are refit asynchronously (`SetWindowPos` + `SWP_ASYNCWINDOWPOS`, still maximized) on apply and on restore.
If Explorer later reverts a monitor to exactly the recorded original, DockOver frees it again at most 3 times per
minute; any other value is treated as a user/program change. Showing the taskbar lets Explorer re-derive its own
work area. A write-ahead
record (`taskbar-windows-v1.txt.workarea.txt`) keeps original and applied rects per monitor (device + monitor rect).
A monitor is restored only while its work rect still equals DockOver's value; a change by Explorer, the user, a display
change or another app is released and never re-applied (no loops). Restore happens before the taskbar is shown, on
exit, by the guardian after a crash and by the restore tool. Monitors where Windows auto-hide already reserves nothing
are left unchanged. Real-desktop acceptance (user-approved, single 2560×1440 monitor): 22/22 — freed on startup and
by shortcut, stable for 10 s, maximized windows grow and shrink back, foreign values never overwritten, restored on
shortcut show, normal exit and by the guardian after a Manager crash. Multi-monitor work areas: simulation only.

## Explorer and multiple monitors (v2)

Taskbar windows are tracked per window (HWND + Explorer PID + process start), so a restarted Explorer's new windows
are never confused with old handles. New/rebuilt taskbars are re-hidden only while DockOver's state is Hidden, after two
stable polls, at most 3 times per minute; then DockOver leaves the taskbar visible instead of looping. Manager, guardian
and restore tool are per-monitor DPI aware so taskbar and work-area rects are physical pixels.

## MyDockFinder comparison (behaviour only)

Local config `config.ini`: `hidetaskbar=1`, `hidealltaskbar=0` (secondary taskbars are a separate option),
`[hotkey] hotkey_showhidetaskbar1=2 / 2=72` (RegisterHotKey format: Ctrl+H). `Dock_64.exe` imports
`SystemParametersInfoW`, `SHAppBarMessage`, `RegisterHotKey`/`UnregisterHotKey`, `ShowWindow(Async)`,
`SetWinEventHook`, `RegisterWindowMessageW`, `EnumDisplayMonitors`/`GetMonitorInfoW`, `GetShellWindow`, and contains
the strings `Shell_TrayWnd` and `Shell_SecondaryTrayWnd`. Which SPI action it uses was not determined (no disassembly;
MyDockFinder was not run because it would change the real taskbar).

## Manual recovery

Settings › Windows Taskbar › **Restore Windows taskbar now**, or run `docky-taskbar-restore.exe` from the DockOver
folder. If both report a preserved/ambiguous record, the taskbar was not provably hidden by DockOver; showing it is
then a manual decision (e.g. restarting Explorer from Task Manager).

## Known limits

- Without the opt-in, the work area reserved by the hidden taskbar stays reserved.
- A crash in the few milliseconds between the write-ahead "Pending" entry and the hide leaves an ambiguous record
  that is deliberately not acted on automatically.
