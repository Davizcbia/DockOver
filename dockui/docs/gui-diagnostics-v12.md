# GUI diagnostics V0.12

All mechanisms are local and explicit opt-in. They do not install hooks, create a network service, hide the taskbar, or accept remote commands. Use an isolated test process and newly created files; do not point test options at user configuration.

## Window inventory
Run the executable built beside the target component:

```powershell
.\docky-gui-diagnostics.exe --dev --pid <Dock-or-MenuBar-pid>
```

The PID must belong to `LiquidDock.exe` or `LiquidMenuBar.exe` in the diagnostic executable's own build directory. The tool enumerates top-level windows of that PID only and prints HWND, class, PID, `IsWindowVisible`, per-window DPI, physical screen rectangle, monitor identity and monitor bounds. It recognizes Dock, Menu Bar, Launchpad, Control Center/sound panel, icon editor and unified popup. A `visible=0-or-not-in-this-process` summary is scoped to the requested PID, not a claim about all processes. The Control Center class is shared with the sound panel; see `panel_anchor` below.

## Menu Bar local trace and deterministic startup states
Start `LiquidMenuBar.exe` directly with a fresh `--dev-diag <trace-path>`; the trace uses CREATE_NEW and refuses an existing file. No trace or timer is enabled without it. Optional explicit startup arguments:

- `--dev-hover logo|app|wifi|sound|battery|control|clock` sets the initial hover state.
- `--dev-popup wifi|sound|battery|control` opens that component at startup.
- `--dev-next wifi|sound|battery|control` selects a second target after three seconds; selecting the same target tests toggle-close. It requires `--dev-popup`.
- `--dev-key escape|down|left|space` posts a single fixed key after two seconds **only to an identified window in this same process**. It requires `--dev-popup`. `left` and `space` on the sound panel change the real Windows volume or mute state: use only when deliberately testing and restore the original state.
- `--dev-png <new-path>` exports the Menu Bar's own layered-window pixels as a straight-alpha PNG after 1.5 seconds. It never overwrites an existing file.
- `--dev-popup-bmp <new-path>` requests the existing GPU back-buffer snapshot for a Wi-Fi or battery unified popup after 1.5 seconds. It refuses an existing path and requires that popup to have a GPU surface. It is BMP, not PNG.
- Existing `LIQUID_CC_SNAPSHOT_DIR` emits Control Center/sound panel BMPs; the Dock already supports `--snapshot` BMP and a separate on-demand PNG export.

Trace fields include heading/status hover, app hover, active menu index, popup HWND, popup selected/hovered row, foreground HWND, panel open state and `panel_anchor` (1 sound, 3 Control Center, -1 none), plus physical client slot rectangles. `active_menu=-10` is Wi-Fi, `-12` battery, nonnegative values are heading indices, and `-1` is none. Popup row -1 means no selection. Sound/Control Center internal hover is not yet traced.

These screenshots are the application's own submitted frame, not a final DWM screen composite. GPU glass frames may include sampled background pixels and popup contents may include real system data; keep them in a private test directory. Menu Bar PNG uses premultiplied-to-straight alpha conversion.

## Isolated Dock and icon editor
The Dock's existing `--launchpad`, `--launchpad-config`, `--desktop-settings`, `--icon-config`, `--glass-config`, `--downloads-path`, `--downloads-settings`, `--config`, `--snapshot` and `--exit-after` support test fixtures. New `--diag --dev-icon-editor` opens the first eligible app icon editor only when both `--config` and `--icon-config` are also explicitly supplied. Use new temporary paths; Apply writes to the supplied icon config, whereas Cancel discards the edit. Without the explicit `--diag` flag, the development action is ignored even if diagnostics was enabled in saved settings.

## Limitations
This is window and component state inspection, not OS-level mouse automation. Automated popup injections do not prove real mouse hit-testing, click-outside behavior, multi-DPI/multi-monitor behavior, or actual final desktop composition. Per-pixel layered transparency still makes initially empty portions of Menu Bar slots click-through, so complete agreement between the whole hover capsule and hit area remains unresolved. Do not fill the full bar with nearly transparent pixels as a workaround.
