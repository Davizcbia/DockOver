#include "taskbar_window.h"

#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <sstream>

namespace liquid::desktop {
namespace {
ULONGLONG ProcessStart(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    FILETIME c{}, e{}, k{}, u{};
    ULONGLONG v = 0;
    if (GetProcessTimes(p, &c, &e, &k, &u)) v = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    CloseHandle(p);
    return v;
}
TaskbarWindowRecord Describe(HWND h) {
    TaskbarWindowRecord r;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    r.className = cls;
    r.hwnd = reinterpret_cast<ULONG_PTR>(h);
    GetWindowThreadProcessId(h, &r.pid);
    r.processStart = ProcessStart(r.pid);
    r.visible = IsWindowVisible(h) != FALSE;
    return r;
}
struct EnumCtx { DWORD shellPid; std::vector<TaskbarWindowRecord>* out; };
BOOL CALLBACK EnumTaskbars(HWND h, LPARAM l) {
    auto* c = reinterpret_cast<EnumCtx*>(l);
    wchar_t cls[64]{};
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->shellPid) c->out->push_back(Describe(h));  // ignore look-alike windows of other tools
    return TRUE;
}
const wchar_t* PhaseName(RecordPhase p) { return p == RecordPhase::Active ? L"active" : p == RecordPhase::Conflict ? L"conflict" : L"closed"; }
}  // namespace

std::vector<TaskbarWindowRecord> CaptureTaskbarWindows() {
    std::vector<TaskbarWindowRecord> out;
    DWORD shellPid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    if (!shellPid) return out;
    EnumCtx ctx{shellPid, &out};
    EnumWindows(EnumTaskbars, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::vector<TaskbarWindowRecord> PlanHide(const std::vector<TaskbarWindowRecord>& current) {
    std::vector<TaskbarWindowRecord> plan;
    for (const auto& w : current)
        if (w.visible && w.hwnd && w.pid && w.processStart) plan.push_back(w);
    return plan;
}

WindowRestore PlanRestore(const TaskbarWindowRecord& rec, bool exists, const TaskbarWindowRecord& now) {
    if (!exists) return WindowRestore::Gone;
    if (now.pid != rec.pid || now.processStart != rec.processStart)
        return now.className == rec.className ? WindowRestore::Gone : WindowRestore::Mismatch;
    if (now.className != rec.className) return WindowRestore::Mismatch;
    return now.visible ? WindowRestore::AlreadyVisible : WindowRestore::Restore;
}

WindowRestore ObserveAndPlanRestore(const TaskbarWindowRecord& rec) {
    TaskbarWindowRecord now;
    const bool exists = WindowOps::Real().observe(rec, now);
    return PlanRestore(rec, exists, now);
}

bool IsConservativeRecoveryRecord(const TaskbarWindowOperation& op) noexcept {
    if (op.phase == RecordPhase::Conflict || (op.phase == RecordPhase::Active && op.windows.empty())) return false;
    for (const auto& w : op.windows) {
        const auto& r = w.original;
        if ((r.className != L"Shell_TrayWnd" && r.className != L"Shell_SecondaryTrayWnd") ||
            !r.hwnd || !r.pid || !r.processStart || !r.visible || w.state == WindowState::Pending ||
            w.state == WindowState::Conflict || (w.state == WindowState::Hidden && !w.hiddenByUs)) return false;
    }
    return true;
}

WindowOps WindowOps::Real() {
    WindowOps o;
    o.observe = [](const TaskbarWindowRecord& rec, TaskbarWindowRecord& now) {
        HWND h = reinterpret_cast<HWND>(rec.hwnd);
        if (!IsWindow(h)) return false;
        now = Describe(h);
        return true;
    };
    o.setVisible = [](ULONG_PTR hwnd, bool show) {
        HWND h = reinterpret_cast<HWND>(hwnd);
        ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);  // single call; never retried against Explorer
        return (IsWindowVisible(h) != FALSE) == show;
    };
    return o;
}

std::wstring DefaultTaskbarWindowRecordPath() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) return {};
    std::wstring dir = std::wstring(p) + L"\\LiquidDesktop";
    CoTaskMemFree(p);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\taskbar-windows-v1.txt";
}

// Format: "liquid-taskbar-windows 1", "phase <active|closed|conflict>", then per window:
// "w <class> <hwnd> <pid> <start> <origVisible> <hiddenByUs> <state>"
bool LoadWindowOperation(const std::wstring& path, TaskbarWindowOperation& out, std::wstring& error) {
    out = {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UTF-8") != 0 || !f) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        error = L"record unreadable";
        return false;
    }
    std::wstring all;
    wchar_t buf[512];
    while (fgetws(buf, 512, f)) all += buf;
    fclose(f);
    std::wistringstream in(all);
    std::wstring tag, phase;
    int version = 0;
    if (!(in >> tag >> version) || tag != L"liquid-taskbar-windows" || version != 1) { error = L"unknown record format (kept)"; return false; }
    if (!(in >> tag >> phase) || tag != L"phase") { error = L"corrupt record (kept)"; return false; }
    out.phase = phase == L"active" ? RecordPhase::Active : phase == L"conflict" ? RecordPhase::Conflict : RecordPhase::Closed;
    if (phase != L"active" && phase != L"conflict" && phase != L"closed") { error = L"corrupt phase (kept)"; return false; }
    while (in >> tag) {
        if (tag != L"w") { error = L"corrupt window line (kept)"; return false; }
        OwnedWindow w;
        unsigned long long hwnd = 0, start = 0;
        int vis = 0, ours = 0, st = 0;
        unsigned long pid = 0;
        if (!(in >> w.original.className >> hwnd >> pid >> start >> vis >> ours >> st) || (vis != 0 && vis != 1) || (ours != 0 && ours != 1) || st < 0 || st > 5) { error = L"corrupt window line (kept)"; return false; }
        w.original.hwnd = static_cast<ULONG_PTR>(hwnd);
        w.original.pid = pid;
        w.original.processStart = start;
        w.original.visible = vis != 0;
        w.hiddenByUs = ours != 0;
        w.state = static_cast<WindowState>(st);
        out.windows.push_back(w);
    }
    return true;
}

bool SaveWindowOperation(const std::wstring& path, const TaskbarWindowOperation& op, std::wstring& error) {
    std::wstring text = L"liquid-taskbar-windows 1\nphase " + std::wstring(PhaseName(op.phase)) + L"\n";
    for (const auto& w : op.windows) {
        text += L"w " + w.original.className + L" " + std::to_wstring(static_cast<unsigned long long>(w.original.hwnd)) + L" " +
                std::to_wstring(w.original.pid) + L" " + std::to_wstring(w.original.processStart) + L" " + (w.original.visible ? L"1" : L"0") +
                L" " + (w.hiddenByUs ? L"1" : L"0") + L" " + std::to_wstring(static_cast<int>(w.state)) + L"\n";
    }
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = L"cannot write temp record"; return false; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    const bool ok = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        error = L"atomic replace failed";
        return false;
    }
    return true;
}

int HideTaskbarWindows(const std::wstring& path, const std::vector<TaskbarWindowRecord>& current, const WindowOps& ops, std::wstring& log) {
    TaskbarWindowOperation prev;
    std::wstring error;
    if (!LoadWindowOperation(path, prev, error)) { log += L"hide refused: " + error + L"\n"; return 0; }
    if (prev.phase != RecordPhase::Closed) { log += L"hide refused: previous record is " + std::wstring(PhaseName(prev.phase)) + L"\n"; return 0; }
    TaskbarWindowOperation op;
    op.phase = RecordPhase::Active;
    for (const auto& w : PlanHide(current)) op.windows.push_back({w, false, WindowState::Pending});
    if (op.windows.empty()) { log += L"hide: no visible taskbar window to take over\n"; return 0; }
    if (!SaveWindowOperation(path, op, error)) { log += L"hide refused: " + error + L"\n"; return 0; }  // write-ahead
    int hidden = 0;
    for (auto& w : op.windows) {
        // The captured HWND may have been destroyed, reused, or changed by the user since capture.
        // Re-check identity and original visibility immediately before touching it.
        TaskbarWindowRecord observed;
        if (!ops.observe(w.original, observed) || !observed.visible ||
            observed.hwnd != w.original.hwnd || observed.pid != w.original.pid ||
            observed.processStart != w.original.processStart || observed.className != w.original.className) {
            w.state = WindowState::NotHidden;
            log += L"hide " + w.original.className + L": identity or visibility changed (left untouched)\n";
            continue;
        }
        const bool ok = ops.setVisible(w.original.hwnd, false);
        w.hiddenByUs = ok;
        w.state = ok ? WindowState::Hidden : WindowState::NotHidden;  // partial failure: that window stays as is
        hidden += ok ? 1 : 0;
        log += L"hide " + w.original.className + (ok ? L": hidden\n" : L": not hidden (left untouched)\n");
    }
    if (!hidden) op.phase = RecordPhase::Closed;
    if (!SaveWindowOperation(path, op, error)) log += L"warning: record update failed: " + error + L"\n";
    return hidden;
}

bool RestoreTaskbarWindows(const std::wstring& path, const WindowOps& ops, std::wstring& log) {
    TaskbarWindowOperation op;
    std::wstring error;
    if (!LoadWindowOperation(path, op, error)) { log += L"restore stopped: " + error + L"\n"; return false; }
    if (op.phase == RecordPhase::Conflict) { log += L"restore stopped: Conflict record kept; no taskbar action\n"; return false; }
    if (op.phase == RecordPhase::Closed) return true;  // nothing owned: idempotent no-op
    bool conflict = false;
    for (auto& w : op.windows) {
        // Pending cannot prove that docky changed visibility; preserve it for review.
        if (w.state == WindowState::Pending) {
            conflict = true;
            log += L"restore " + w.original.className + L": Pending ownership ambiguous; left untouched\n";
            continue;
        }
        if (w.state != WindowState::Hidden) continue;
        TaskbarWindowRecord now;
        const bool exists = ops.observe(w.original, now);
        switch (PlanRestore(w.original, exists, now)) {
        case WindowRestore::Restore: {
            const bool ok = ops.setVisible(w.original.hwnd, true);
            w.state = ok ? WindowState::Restored : WindowState::Conflict;
            if (!ok) conflict = true;  // retain unresolved ownership; do not falsely mark the operation closed
            log += L"restore " + w.original.className + (ok ? L": shown\n" : L": show not confirmed; Conflict retained\n");
            break;
        }
        case WindowRestore::AlreadyVisible: w.state = WindowState::Skipped; log += L"restore " + w.original.className + L": already visible (external)\n"; break;
        case WindowRestore::Gone: w.state = WindowState::Skipped; log += L"restore " + w.original.className + L": gone or Explorer restarted\n"; break;
        case WindowRestore::Mismatch: w.state = WindowState::Conflict; conflict = true; log += L"restore " + w.original.className + L": identity mismatch -> Conflict\n"; break;
        }
    }
    op.phase = conflict ? RecordPhase::Conflict : RecordPhase::Closed;
    if (!SaveWindowOperation(path, op, error)) { log += L"warning: record update failed: " + error + L"\n"; return false; }
    return !conflict;
}

}  // namespace liquid::desktop
