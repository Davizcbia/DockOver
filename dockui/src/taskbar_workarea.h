#pragma once
// Optional "free work area while docky hides the taskbar" (default off; Settings opt-in).
//
// Mechanism: SystemParametersInfo(SPI_SETWORKAREA, SPIF_SENDCHANGE) WITHOUT SPIF_UPDATEINIFILE, i.e. a non-persistent
// per-monitor change that Explorer re-derives on its own after a restart / display change. Alternatives rejected:
// ABM_SETSTATE(ABS_AUTOHIDE) changes the user's persistent taskbar setting; unregistering Explorer's appbar is not
// possible from outside Explorer.
//
// Ownership: a write-ahead record stores, per monitor (device name + monitor rect), the original and the applied work
// rect. A monitor is restored ONLY while its current work rect still equals the applied one (value identity), so a
// later change by Explorer, the user or another program is never overwritten; there is no re-apply loop.
#include "taskbar_guardian.h"

#include <functional>
#include <string>
#include <vector>

namespace docky::taskbar {
struct MonitorArea { std::wstring device; RECT monitor{}; RECT work{}; };
struct WorkAreaOps {
    std::function<std::vector<MonitorArea>()> monitors;   // current topology, physical pixels
    // Apply to the monitor containing `work`; true = verified. broadcast=false for docky's own value (Explorer reverts
    // a broadcast change within ~0.5 s while its hidden appbar is registered); broadcast=true when restoring so
    // Explorer and maximized windows re-derive the normal layout.
    std::function<bool(const RECT& work, bool broadcast)> setWork;
    // After docky changed a monitor's work area from `from` to `to`: windows maximized to exactly `from` (same frame
    // insets) are moved to the equivalent maximized rect of `to`; others are untouched. Returns the count. Optional.
    std::function<int(const RECT& monitor, const RECT& from, const RECT& to)> refitMaximized;
    static WorkAreaOps Real();
};
enum class WorkState { Pending = 0, Applied = 1, Restored = 2, Skipped = 3 };
struct WorkEntry { std::wstring device; RECT monitor{}; RECT original{}; RECT applied{}; WorkState state = WorkState::Pending; };
struct WorkAreaOperation { bool active = false; std::vector<WorkEntry> entries; };

std::wstring WorkAreaPathFor(const std::wstring& windowRecordPath);  // "<record>.workarea.txt"
bool LoadWorkArea(const std::wstring& path, WorkAreaOperation& out, std::wstring& error);  // missing = inactive
bool SaveWorkArea(const std::wstring& path, const WorkAreaOperation& op, std::wstring& error);

// Work rect with only the hidden taskbar's own thickness given back on the edge it occupies. Returns false when
// nothing would change (e.g. Windows auto-hide already leaves no reserved strip).
bool FreedWork(const RECT& monitor, const RECT& work, const RECT& taskbar, RECT& out);
// taskbars: screen rects of the taskbar windows docky hid. Refuses when a previous record is still active.
int ApplyWorkArea(const std::wstring& path, const std::vector<RECT>& taskbars, const WorkAreaOps& ops, std::wstring& log);
// Restores monitors whose work rect is still exactly docky's; others are skipped. Idempotent.
RecoverResult RestoreWorkArea(const std::wstring& path, const WorkAreaOps& ops, std::wstring& log);
// While hidden: an applied monitor whose work rect changed is released (never re-applied). True = record changed.
// If allowReapply is true, a monitor reverted to exactly its recorded original (Explorer re-deriving its own appbar
// space) is set to docky's value again instead; the caller bounds how often this may happen. `reapplied` counts them.
bool ReleaseExternallyChangedWorkArea(const std::wstring& path, const WorkAreaOps& ops, std::wstring& log,
                                      bool allowReapply = false, int* reapplied = nullptr);
}  // namespace docky::taskbar
