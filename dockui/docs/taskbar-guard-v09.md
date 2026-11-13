# docky V0.9 taskbar guardian — isolated prototype

## Current status

Default taskbar hiding remains **off**. The existing explicit `--hide-taskbar` path is not protected by the new probe and must not be considered crash-safe. The probe is compiled only into `DockyTaskbarGuardProbeTest`; it does not call `ShowWindow`, touch the taskbar, access recovery records or run as a docky service. No real taskbar hide was attempted in this work.

The probe takes duplicated kernel process handles for a simulated Manager and Dock plus a restore-request event. It tests that a separate process detects Manager/Dock death and simulated normal shutdown, and only signals ready if neither target nor request was already signaled at its initial check. The test harness duplicates only its chosen handles through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, terminates only mock processes it created, and models taskbar visibility with an event. Its simulated owner observes guard failure and restores that event. This demonstrates process-notification plumbing; it is not a production recovery implementation or a proof against all races.

## Existing ownership record

The current record describes each taskbar window by HWND, Explorer PID and process creation time, class, original visibility and state. It writes Pending before hide and restores Hidden/Pending only after identity checks. This round rechecks identity/visibility immediately before hide and keeps failed restore as Conflict instead of falsely closing the record. The system auto-hide setting is read but not changed. A missing or recreated Explorer window is not adopted. Multiple captured shell taskbars are recorded separately.

There are important remaining hazards. A visibility change between observation and `ShowWindow` cannot be fully attributed to docky; Pending after a crash is inherently ambiguous. A final record write can fail. No interprocess lock serializes Manager and a future watcher over the window record. The current single-instance Manager mutex is not held by an independent watcher after Manager death. User/external changes, Explorer recreation, display changes and new taskbars need an explicit safe ownership policy. If showing a window fails the record now stays Conflict; no automatic retry is claimed.

## Proposed production protocol — not implemented

1. Manager acquires a per-user session gate held until restoration is conclusively complete. Other starts must not modify any active/conflicted record. Guard and Manager need a clear transfer protocol, including abandonment when either dies.
2. Dock explicitly acknowledges the first successful rendered Present; a visible HWND alone is not proof. Manager launches a separate watchdog with least-privilege inherited process handles, validated record path and a ready handshake. Verify process/window ownership, same user/session and build identity. Refuse to hide if any step fails.
3. One cross-process ownership mutex must serialize every record transition, window action and recovery. Write-ahead records must be durably committed before hiding, and failures must fail closed. After readiness and immediately before/after acquisition/hide, check that Dock, Manager and watchdog still live. Define recovery of every write/action crash boundary, including late guardian death.
4. One party owns restore at a time. On graceful exit, Dock death or Manager death, the guard acquires that mutex and conservatively restores only still-identical windows docky owns; it reports completion before release. Manager watches the guard and requests recovery or remains visible if guard fails. A durable manual recovery tool must work without starting Dock and must inspect/resolve Conflict without guessing.
5. Explorer restart invalidates old identities; never transfer ownership to a new process or HWND. Re-enumeration and monitor additions must not hide new shell windows until safely journaled and guarded; on uncertainty, restore owned windows and leave new ones alone. Preserve the user's current auto-hide configuration and user-initiated visibility changes.
6. Exercise these cases using test-owned windows/records and real process termination: readiness races, Manager/Dock/guard death at each journal boundary, partial hide/show failures, duplicate startups, stale locks, handle reuse, Explorer replacement, display additions/removal and manual recovery. Only after these and an explicit user confirmation may the real Windows taskbar be tested.

## Verification this round

`DockyTaskbarGuardProbeTest` passed simulated manager crash, Dock crash, normal restore request, guard crash with simulated owner fallback, pre-dead manager/Dock, invalid handles and simulated visibility restoration. `LiquidTaskbarWindowTest` used fake window operations for multi-window restoration, Explorer replacement, handle reuse, user visibility change, hide revalidation and failed restoration; `LiquidTaskbarOwnershipTest` exercised its separate autohide-state ownership and cross-process exclusion model. These tests are not an end-to-end test of a production guardian.
