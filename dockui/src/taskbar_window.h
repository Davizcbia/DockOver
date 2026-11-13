#pragma once
// Taskbar Integration V0.3: window-level, non-persistent taskbar hiding with a write-ahead ownership
// record. ShowWindow(SW_HIDE/SW_SHOWNA) on Explorer's Shell_TrayWnd / Shell_SecondaryTrayWnd only.
// No ABM_SETSTATE, no registry, never touches Explorer's process. The legacy autohide record
// (taskbar_ownership) is left as is; its Conflict/Abandoned states still block hiding.
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace liquid::desktop {

// One taskbar window (Shell_TrayWnd = primary, Shell_SecondaryTrayWnd = other monitors).
struct TaskbarWindowRecord {
    std::wstring className;
    ULONG_PTR hwnd = 0;          // stored as integer for the operation record
    DWORD pid = 0;               // owning Explorer process
    ULONGLONG processStart = 0;  // process creation FILETIME: detects Explorer restart / PID reuse
    bool visible = false;        // IsWindowVisible (original state when recorded)
};

// Enumerates current taskbar windows of the shell process (GetShellWindow's PID). Read-only.
std::vector<TaskbarWindowRecord> CaptureTaskbarWindows();
// Only windows that are visible now may be taken over (never "restore" what was already hidden).
std::vector<TaskbarWindowRecord> PlanHide(const std::vector<TaskbarWindowRecord>& current);

enum class WindowRestore { Restore, AlreadyVisible, Gone, Mismatch };
WindowRestore PlanRestore(const TaskbarWindowRecord& recorded, bool windowExists, const TaskbarWindowRecord& observed);
WindowRestore ObserveAndPlanRestore(const TaskbarWindowRecord& recorded);

// ---- Ownership record (write-ahead, atomic replace) ----
enum class WindowState { Pending, Hidden, NotHidden, Restored, Skipped, Conflict };
struct OwnedWindow {
    TaskbarWindowRecord original;
    bool hiddenByUs = false;
    WindowState state = WindowState::Pending;
};
enum class RecordPhase { Active, Closed, Conflict };
struct TaskbarWindowOperation {
    RecordPhase phase = RecordPhase::Closed;
    std::vector<OwnedWindow> windows;
};
// Validate untrusted legacy records before using real window operations.
bool IsConservativeRecoveryRecord(const TaskbarWindowOperation& op) noexcept;
// Injectable system access so every path can be simulated. Real() uses IsWindow/ShowWindow.
struct WindowOps {
    std::function<bool(const TaskbarWindowRecord& rec, TaskbarWindowRecord& now)> observe;  // false = window gone
    std::function<bool(ULONG_PTR hwnd, bool show)> setVisible;                              // true = verified
    static WindowOps Real();
};

std::wstring DefaultTaskbarWindowRecordPath();
// Missing file -> true with phase Closed. Unknown/corrupt -> false (file is never deleted or rewritten).
bool LoadWindowOperation(const std::wstring& path, TaskbarWindowOperation& out, std::wstring& error);
// Creates <path>.tmp exclusively, flushes, then MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH).
// An existing temp file is retained and causes failure.
bool SaveWindowOperation(const std::wstring& path, const TaskbarWindowOperation& op, std::wstring& error);

// Records intent first (Active/Pending), then hides each window once and records the outcome.
// Refuses when an unfinished or Conflict record exists. Returns number of windows hidden.
int HideTaskbarWindows(const std::wstring& path, const std::vector<TaskbarWindowRecord>& current, const WindowOps& ops, std::wstring& log);
// Idempotent: restores only windows we hid that are still ours and hidden; external shows, Explorer
// restarts and invalid handles are skipped; identity mismatch or ambiguous Pending -> Conflict.
// Returns false when the record is Conflict or unreadable.
bool RestoreTaskbarWindows(const std::wstring& path, const WindowOps& ops, std::wstring& log);

}  // namespace liquid::desktop
