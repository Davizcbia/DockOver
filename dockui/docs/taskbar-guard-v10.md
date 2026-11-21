# DockOver V0.10 — taskbar safety interim

The production watchdog is **not integrated**. Both default and explicit `--hide-taskbar` paths currently refuse to hide; the prior unguarded hide call was removed from the Manager path. Existing startup recovery behavior for an old record remains. Do not enable hiding until the complete guardian/first-Present/transaction protocol in `taskbar-guard-v09.md` is implemented and verified. No real taskbar hiding is authorized for this task.

## Independent recovery command

After building the worktree, from PowerShell run:

```powershell
& "<this-worktree-build-directory>\docky-taskbar-restore.exe"
```

It opens the legacy `%LOCALAPPDATA%\LiquidDesktop\taskbar-windows-v1.txt` record without starting Dock or Manager. It creates the Manager's single-instance gate for its whole operation, refuses if that gate already exists, and refuses to overwrite an existing temporary record. It validates taskbar class, recorded PID and process creation time, original visibility and current Explorer PID; it delegates identity checks and conservative restore to the existing recovery logic. Missing or closed records do nothing. Conflict, corrupt, malformed and Pending entries are refused and retained: Pending cannot distinguish a crash before hiding from a later user action. Failed show remains Conflict. It does not alter Explorer's auto-hide setting or touch non-shell windows. Manager and recovery must not run concurrently; this gate coordinates these current processes but is not yet the cross-process journal lock needed for a future watchdog.

For isolated tests only: `docky-taskbar-restore.exe --simulate --record <test-record>`. Simulation uses in-memory fake window operations, but rewrites the explicitly provided test record; never use a real user record for simulation. `dockui/tests/taskbar_restore_cli_test.ps1 -Exe <path>` creates a fresh temporary fixture and tests recovery, ambiguous/corrupt/conflict refusal and temp preservation.

## Remaining production requirements

First successful Present readiness must be signaled without polling HWND alone. The guard must be a separate authenticated same-user process and hold reliable Manager/Dock process handles. Manager and guard must serialize ownership and record transitions across hide, process crash, restore and duplicate startup, including guard death mid-operation. A session-specific authenticated journal must record all owners and identities and stay recoverable at every write/action boundary. Explorer recreation and display changes require conservative handling. The manual tool currently refuses ambiguous Pending/Conflict records and thus is not a complete emergency solution for every crash. Real Windows taskbar behavior and GUI/DPI remain untested, and real hiding requires explicit later user authorization.
