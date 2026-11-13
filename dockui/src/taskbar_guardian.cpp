#include "taskbar_guardian.h"
#include "taskbar_window.h"
#include "taskbar_workarea.h"

#include <shlobj.h>
#include <cstdio>
#include <sstream>

namespace docky::taskbar {
using namespace liquid::desktop;
namespace {
std::wstring LocalDir() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) return {};
    std::wstring dir = std::wstring(p) + L"\\LiquidDesktop";
    CoTaskMemFree(p);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
void Log(const std::wstring& path, const std::wstring& line) {
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, GetCurrentProcessId(), line.c_str());
    fclose(f);
}
}  // namespace

namespace {
// Test isolation: the prefix goes after the "Local\" namespace (a backslash elsewhere makes the name invalid).
std::wstring Named(const std::wstring& prefix, const wchar_t* base) {
    const std::wstring b = base, ns = L"Local\\";
    return b.compare(0, ns.size(), ns) == 0 ? ns + prefix + b.substr(ns.size()) : prefix + b;
}
}  // namespace
bool RecordingLease::Acquire(DWORD ackMs, bool& managerAcked, std::wstring& why) {
    managerAcked = false;
    if (mutex_) return true;
    HANDLE m = CreateMutexW(nullptr, FALSE, Named(prefix_, kHotkeyLease).c_str());
    if (!m) { why = L"The recording lock is unavailable."; return false; }
    const DWORD w = WaitForSingleObject(m, 0);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) { CloseHandle(m); why = L"Another Settings window is recording a shortcut."; return false; }
    mutex_ = m;
    HANDLE ack = OpenEventW(SYNCHRONIZE, FALSE, Named(prefix_, kHotkeySuspended).c_str());
    if (HANDLE changed = OpenEventW(EVENT_MODIFY_STATE, FALSE, Named(prefix_, kHotkeyLeaseChanged).c_str())) { SetEvent(changed); CloseHandle(changed); }
    if (ack) { managerAcked = ackMs && WaitForSingleObject(ack, ackMs) == WAIT_OBJECT_0; CloseHandle(ack); }
    return true;
}
void RecordingLease::Release() {
    if (!mutex_) return;
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
}

HotkeySuspension::HotkeySuspension(std::wstring prefix) : prefix_(std::move(prefix)) {
    changed_ = CreateEventW(nullptr, FALSE, FALSE, Named(prefix_, kHotkeyLeaseChanged).c_str());
    ack_ = CreateEventW(nullptr, TRUE, FALSE, Named(prefix_, kHotkeySuspended).c_str());
    if (ack_) ResetEvent(ack_);
}
HotkeySuspension::~HotkeySuspension() {
    if (lease_) CloseHandle(lease_);
    if (ack_) { ResetEvent(ack_); CloseHandle(ack_); }
    if (changed_) CloseHandle(changed_);
}
bool HotkeySuspension::OnChanged() {
    if (lease_) return false;  // already suspended; the lease handle tells us when it ends
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, Named(prefix_, kHotkeyLease).c_str());
    if (!m) return false;
    const DWORD w = WaitForSingleObject(m, 0);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { ReleaseMutex(m); CloseHandle(m); return false; }  // nobody records
    lease_ = m;
    return true;
}
void HotkeySuspension::Acknowledge() { if (ack_) SetEvent(ack_); }
void HotkeySuspension::OnLeaseEnded() {
    if (!lease_) return;
    ReleaseMutex(lease_);  // the wait that reported the end acquired it
    CloseHandle(lease_);
    lease_ = nullptr;
    if (ack_) ResetEvent(ack_);
}

std::wstring DefaultStatusPath() { const auto d = LocalDir(); return d.empty() ? L"" : d + L"\\taskbar-status-v1.txt"; }
std::wstring DefaultGuardianLog() { const auto d = LocalDir(); return d.empty() ? L"" : d + L"\\taskbar-guardian.log"; }

RecordLock::RecordLock(DWORD timeoutMs) {
    mutex_ = CreateMutexW(nullptr, FALSE, kRecordLock);
    if (!mutex_) return;
    const DWORD w = WaitForSingleObject(mutex_, timeoutMs);
    held_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}
RecordLock::~RecordLock() {
    if (held_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}

RecoverResult RecoverTaskbarRecord(const std::wstring& path, std::wstring& log) {
    if (path.empty()) { log += L"recover: record path unavailable; no window action\n"; return RecoverResult::Refused; }
    RecordLock lock;
    if (!lock.Held()) { log += L"recover: record lock busy; no window action\n"; return RecoverResult::Refused; }
    const DWORD tmp = GetFileAttributesW((path + L".tmp").c_str());
    const DWORD tmpError = GetLastError();
    if (tmp != INVALID_FILE_ATTRIBUTES || (tmpError != ERROR_FILE_NOT_FOUND && tmpError != ERROR_PATH_NOT_FOUND)) {
        log += L"recover: temporary record exists or is inaccessible; preserved, no window action\n";
        return RecoverResult::Refused;
    }
    // Work area first (only monitors still carrying exactly docky's value); its outcome never blocks the taskbar.
    const auto work = RestoreWorkArea(WorkAreaPathFor(path), WorkAreaOps::Real(), log);
    TaskbarWindowOperation op;
    std::wstring error;
    if (!LoadWindowOperation(path, op, error)) { log += L"recover: " + error + L"; no window action\n"; return RecoverResult::Refused; }
    if (op.phase == RecordPhase::Closed) return work == RecoverResult::Restored ? RecoverResult::Restored : RecoverResult::NothingToDo;
    if (!IsConservativeRecoveryRecord(op)) { log += L"recover: Conflict or ambiguous (Pending) record preserved; no window action\n"; return RecoverResult::Refused; }
    DWORD shellPid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    if (!shellPid) { log += L"recover: shell identity unavailable; no window action\n"; return RecoverResult::Refused; }
    auto ops = WindowOps::Real();
    const auto observe = ops.observe;
    // Only windows of the current shell process may be shown; a restarted Explorer owns new, visible windows.
    ops.observe = [observe, shellPid](const TaskbarWindowRecord& wanted, TaskbarWindowRecord& now) {
        return wanted.pid == shellPid && observe(wanted, now);
    };
    const bool ok = RestoreTaskbarWindows(path, ops, log);
    return ok ? RecoverResult::Restored : RecoverResult::Refused;
}

bool WriteStatus(const std::wstring& path, const Status& s) {
    if (path.empty()) return false;
    std::wstring text = L"docky-taskbar-status 1\nmanagerPid=" + std::to_wstring(s.managerPid) + L"\nstate=" + s.state +
                        L"\nguardian=" + s.guardian + L"\nhotkey=" + s.hotkey + L"\nhotkeyText=" + s.hotkeyText +
                        L"\nmessage=" + s.message + L"\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    const std::wstring tmp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool ReadStatus(const std::wstring& path, Status& s) {
    s = {};
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[4096]{}; DWORD got = 0;
    const bool ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr) != FALSE;
    CloseHandle(f);
    if (!ok) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), nullptr, 0);
    std::wstring text(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), text.data(), n);
    std::wistringstream in(text);
    std::wstring line;
    if (!std::getline(in, line) || line != L"docky-taskbar-status 1") return false;
    while (std::getline(in, line)) {
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == L"managerPid") s.managerPid = static_cast<DWORD>(_wtoi64(value.c_str()));
        else if (key == L"state") s.state = value;
        else if (key == L"guardian") s.guardian = value;
        else if (key == L"hotkey") s.hotkey = value;
        else if (key == L"hotkeyText") s.hotkeyText = value;
        else if (key == L"message") s.message = value;
    }
    return true;
}

bool StatusIsLive(const Status& s) {
    if (!s.managerPid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.managerPid);
    if (!p) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    wchar_t image[MAX_PATH]{}; DWORD len = MAX_PATH;
    const bool isManager = QueryFullProcessImageNameW(p, 0, image, &len) && wcsstr(image, L"LiquidDesktopManager") != nullptr;
    CloseHandle(p);
    return alive && isManager;  // a reused PID of another program is not docky
}

int RunGuardian(HANDLE manager, HANDLE stop, HANDLE ready, const std::wstring& record, const std::wstring& logPath) {
    if (!manager || !stop || !ready || record.empty()) { Log(logPath, L"guardian: invalid arguments"); return 2; }
    HANDLE restoreNow = CreateEventW(nullptr, FALSE, FALSE, kRestoreNowEvent);
    if (!restoreNow) { Log(logPath, L"guardian: restore-now event unavailable"); return 3; }
    // Never advertise readiness if the Manager is already gone or a stop was requested.
    HANDLE early[]{manager, stop};
    if (WaitForMultipleObjects(2, early, FALSE, 0) != WAIT_TIMEOUT) { CloseHandle(restoreNow); Log(logPath, L"guardian: Manager gone before ready"); return 4; }
    if (!SetEvent(ready)) { CloseHandle(restoreNow); return 3; }
    Log(logPath, L"guardian: ready (record " + record + L")");
    int code = 0;
    for (;;) {
        HANDLE waits[]{manager, stop, restoreNow};
        const DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        std::wstring log;
        if (w == WAIT_OBJECT_0 + 2) {  // user asked: restore now (works even if the Manager is unresponsive)
            const auto r = RecoverTaskbarRecord(record, log);
            Log(logPath, L"guardian: restore-now result=" + std::to_wstring(static_cast<int>(r)) + L"\n" + log);
            continue;
        }
        if (w == WAIT_OBJECT_0 + 1) {  // clean stop: the Manager restored first; verify, recover if it could not
            const auto r = RecoverTaskbarRecord(record, log);
            Log(logPath, L"guardian: stop; verification result=" + std::to_wstring(static_cast<int>(r)) + L"\n" + log);
            code = r == RecoverResult::Refused ? 5 : 0;
            break;
        }
        if (w == WAIT_OBJECT_0) {  // the Manager exited without a clean stop (crash / kill)
            // Take the Manager gate so no new Manager or restore tool races this recovery. If one already started,
            // it performs its own startup recovery of the same record.
            HANDLE gate = CreateMutexW(nullptr, TRUE, kManagerGate);
            if (gate && GetLastError() == ERROR_ALREADY_EXISTS) {
                Log(logPath, L"guardian: Manager exited; another Manager/restore owns the gate; leaving recovery to it");
                CloseHandle(gate);
                break;
            }
            const auto r = RecoverTaskbarRecord(record, log);
            Log(logPath, L"guardian: Manager exited unexpectedly; recovery result=" + std::to_wstring(static_cast<int>(r)) + L"\n" + log);
            if (gate) { ReleaseMutex(gate); CloseHandle(gate); }
            code = r == RecoverResult::Refused ? 6 : 0;
            break;
        }
        Log(logPath, L"guardian: wait failed " + std::to_wstring(GetLastError()));
        code = 7;
        break;
    }
    CloseHandle(restoreNow);
    return code;
}
}  // namespace docky::taskbar
