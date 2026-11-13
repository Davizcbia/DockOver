#pragma once
// Production taskbar recovery guardian and the shared recovery primitives.
//
// Roles:
//   Desktop Manager  - the only component that hides/shows Explorer's taskbar windows (single-instance gate).
//   Guardian process - started by the Manager before any hide; signals readiness; restores the taskbar from the
//                      write-ahead record if the Manager disappears without a clean stop, or on "Restore now".
//   docky-taskbar-restore.exe - independent user tool; restores directly when no Manager runs, otherwise asks
//                      the guardian. Never forces anything a Conflict/Pending/corrupt record does not prove.
//   Windows Explorer - owns Shell_TrayWnd / Shell_SecondaryTrayWnd. docky only calls ShowWindow on them; it never
//                      changes Explorer settings, the registry, ABM_SETSTATE or the Explorer process.
#include <windows.h>
#include <string>

namespace docky::taskbar {
inline constexpr wchar_t kManagerGate[] = L"Local\\LiquidDesktopManagerV05SingleInstance";
inline constexpr wchar_t kRecordLock[] = L"Local\\LiquidDesktopTaskbarRecordV1";      // serializes record + window ops
inline constexpr wchar_t kRestoreNowEvent[] = L"Local\\LiquidDesktopTaskbarRestoreNowV1"; // guardian: restore now
inline constexpr wchar_t kHideEvent[] = L"Local\\LiquidDesktopTaskbarHideV1";            // Manager commands
inline constexpr wchar_t kShowEvent[] = L"Local\\LiquidDesktopTaskbarShowV1";
inline constexpr wchar_t kToggleEvent[] = L"Local\\LiquidDesktopTaskbarToggleV1";

// ---- Shortcut recording lease ----
// While a Settings window records a shortcut it owns the named lease mutex. The Manager suspends (unregisters) its
// global hotkey for exactly as long as the lease is held, so the key being recorded can never toggle the taskbar.
// The lease ends on success, cancel, timeout, window close or Settings crash (abandoned mutex).
inline constexpr wchar_t kHotkeyLease[] = L"Local\\LiquidDesktopHotkeyRecordingV1";
inline constexpr wchar_t kHotkeyLeaseChanged[] = L"Local\\LiquidDesktopHotkeyLeaseChangedV1";  // Settings -> Manager
inline constexpr wchar_t kHotkeySuspended[] = L"Local\\LiquidDesktopHotkeySuspendedV1";       // Manager -> Settings ack

// Settings side. Acquire() fails when another window already records. Must be released on the owning thread.
class RecordingLease {
public:
    explicit RecordingLease(std::wstring prefix = L"") : prefix_(std::move(prefix)) {}
    ~RecordingLease() { Release(); }
    // ackMs: how long to wait for the Manager's suspension acknowledgement (0 = do not wait).
    // Returns false (with why) if another recording holds the lease. managerAcked reports the acknowledgement.
    bool Acquire(DWORD ackMs, bool& managerAcked, std::wstring& why);
    void Release();
    bool Held() const { return mutex_ != nullptr; }
private:
    std::wstring prefix_;
    HANDLE mutex_ = nullptr;
};

// Manager side: owns the change/ack events; keeps the lease handle to wait on while suspended.
class HotkeySuspension {
public:
    explicit HotkeySuspension(std::wstring prefix = L"");
    ~HotkeySuspension();
    HANDLE ChangedEvent() const { return changed_; }
    HANDLE LeaseHandle() const { return lease_; }   // non-null only while suspended; wait on it
    bool Suspended() const { return lease_ != nullptr; }
    // On ChangedEvent: returns true when a recording lease is now held by someone else (caller must unregister its
    // hotkey, then call Acknowledge()).
    bool OnChanged();
    void Acknowledge();
    // On LeaseHandle signaled/abandoned: the lease ended. Releases it; caller re-registers from current settings.
    void OnLeaseEnded();
private:
    std::wstring prefix_;
    HANDLE changed_ = nullptr, ack_ = nullptr, lease_ = nullptr;
};

std::wstring DefaultStatusPath();   // %LOCALAPPDATA%\LiquidDesktop\taskbar-status-v1.txt
std::wstring DefaultGuardianLog();  // %LOCALAPPDATA%\LiquidDesktop\taskbar-guardian.log

// RAII holder of the cross-process record lock (abandoned owners are accepted: the record is the authority).
class RecordLock {
public:
    explicit RecordLock(DWORD timeoutMs = 3000);
    ~RecordLock();
    bool Held() const { return held_; }
    RecordLock(const RecordLock&) = delete;
    RecordLock& operator=(const RecordLock&) = delete;
private:
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

enum class RecoverResult { NothingToDo, Restored, Refused };
// Conservative recovery of the window record: refuses an existing .tmp, corrupt/Pending/Conflict records and
// windows that no longer belong to the current shell process; idempotent. Takes the record lock itself.
RecoverResult RecoverTaskbarRecord(const std::wstring& recordPath, std::wstring& log);

// Status published by the Manager for Settings (key=value lines, atomic replace).
struct Status {
    DWORD managerPid = 0;
    std::wstring state = L"unavailable";   // visible | hidden | conflict | unavailable
    std::wstring guardian = L"stopped";    // ready | starting | stopped | failed
    std::wstring hotkey = L"disabled";     // registered | conflict | disabled | invalid
    std::wstring hotkeyText;
    std::wstring message;                  // last error / feedback, one line
};
bool WriteStatus(const std::wstring& path, const Status& s);
bool ReadStatus(const std::wstring& path, Status& s);
// True when the status file's Manager process is still running.
bool StatusIsLive(const Status& s);

// Guardian entry point (docky-taskbar-guardian.exe). Handles are inherited from the Manager.
int RunGuardian(HANDLE manager, HANDLE stop, HANDLE ready, const std::wstring& recordPath, const std::wstring& logPath);
}  // namespace docky::taskbar
