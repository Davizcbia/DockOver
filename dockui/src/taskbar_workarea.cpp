#include "taskbar_workarea.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace docky::taskbar {
namespace {
bool Same(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
std::wstring R(const RECT& r) { return std::to_wstring(r.left) + L" " + std::to_wstring(r.top) + L" " + std::to_wstring(r.right) + L" " + std::to_wstring(r.bottom); }
const MonitorArea* Find(const std::vector<MonitorArea>& list, const WorkEntry& e) {
    for (const auto& m : list) if (m.device == e.device && Same(m.monitor, e.monitor)) return &m;  // topology change = not ours
    return nullptr;
}
struct Refit { RECT monitor, from, to; int moved = 0; };
BOOL CALLBACK RefitProc(HWND h, LPARAM l) {
    auto* r = reinterpret_cast<Refit*>(l);
    if (!IsWindowVisible(h) || !IsZoomed(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    RECT w{};
    if (!GetWindowRect(h, &w)) return TRUE;
    // A window maximized to `from` overhangs it by its frame on the sides and bottom (top may differ by caption
    // style). Only exactly that shape is refit; anything else is not ours to move.
    const LONG inset = r->from.left - w.left;
    if (inset < 0 || inset > 64 || w.right - r->from.right != inset || w.bottom - r->from.bottom != inset) return TRUE;
    POINT c{(w.left + w.right) / 2, (w.top + w.bottom) / 2};
    if (!PtInRect(&r->monitor, c)) return TRUE;
    const LONG top = r->to.top - (r->from.top - w.top);
    // Async: a hung application can never block the Manager or the guardian. The maximized state is kept.
    if (SetWindowPos(h, nullptr, r->to.left - inset, top, (r->to.right + inset) - (r->to.left - inset), (r->to.bottom + inset) - top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS)) ++r->moved;
    return TRUE;
}
BOOL CALLBACK EnumMon(HMONITOR m, HDC, LPRECT, LPARAM l) {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi)) reinterpret_cast<std::vector<MonitorArea>*>(l)->push_back({mi.szDevice, mi.rcMonitor, mi.rcWork});
    return TRUE;
}
}  // namespace

WorkAreaOps WorkAreaOps::Real() {
    WorkAreaOps o;
    o.monitors = [] { std::vector<MonitorArea> v; EnumDisplayMonitors(nullptr, nullptr, EnumMon, reinterpret_cast<LPARAM>(&v)); return v; };
    o.setWork = [](const RECT& work, bool broadcast) {
        RECT r = work;
        // Never SPIF_UPDATEINIFILE: the change is never written to the user profile. Measured on Windows 11: with the
        // WM_SETTINGCHANGE broadcast Explorer re-derives its (hidden) appbar space within ~0.5 s; without it the value
        // holds and newly maximized windows use it.
        if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &r, broadcast ? SPIF_SENDCHANGE : 0)) return false;
        MONITORINFO mi{sizeof(mi)};
        return GetMonitorInfoW(MonitorFromRect(&work, MONITOR_DEFAULTTONULL), &mi) && Same(mi.rcWork, work);
    };
    o.refitMaximized = [](const RECT& monitor, const RECT& from, const RECT& to) {
        Refit r{monitor, from, to};
        EnumWindows(RefitProc, reinterpret_cast<LPARAM>(&r));
        return r.moved;
    };
    return o;
}

std::wstring WorkAreaPathFor(const std::wstring& recordPath) { return recordPath.empty() ? L"" : recordPath + L".workarea.txt"; }

// Format: "docky-workarea 1", "active <0|1>", then "m <device> <monitor 4> <original 4> <applied 4> <state>".
bool LoadWorkArea(const std::wstring& path, WorkAreaOperation& out, std::wstring& error) {
    out = {};
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;  // never applied
        error = L"work-area record unavailable"; return false;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UTF-8") != 0 || !f) { error = L"work-area record unreadable"; return false; }
    std::wstring all; wchar_t buf[512];
    while (fgetws(buf, 512, f)) all += buf;
    fclose(f);
    std::wistringstream in(all);
    std::wstring tag; int version = 0, active = 0;
    if (!(in >> tag >> version) || tag != L"docky-workarea" || version != 1) { error = L"unknown work-area record (kept)"; return false; }
    if (!(in >> tag >> active) || tag != L"active" || (active != 0 && active != 1)) { error = L"corrupt work-area record (kept)"; return false; }
    out.active = active == 1;
    while (in >> tag) {
        WorkEntry e; int st = 0; RECT* rs[] = {&e.monitor, &e.original, &e.applied};
        if (tag != L"m" || !(in >> e.device)) { error = L"corrupt work-area entry (kept)"; return false; }
        for (RECT* r : rs) if (!(in >> r->left >> r->top >> r->right >> r->bottom)) { error = L"corrupt work-area entry (kept)"; return false; }
        if (!(in >> st) || st < 0 || st > 3) { error = L"corrupt work-area entry (kept)"; return false; }
        e.state = static_cast<WorkState>(st);
        out.entries.push_back(e);
    }
    return true;
}

bool SaveWorkArea(const std::wstring& path, const WorkAreaOperation& op, std::wstring& error) {
    std::wstring text = L"docky-workarea 1\nactive " + std::wstring(op.active ? L"1" : L"0") + L"\n";
    for (const auto& e : op.entries)
        text += L"m " + e.device + L" " + R(e.monitor) + L" " + R(e.original) + L" " + R(e.applied) + L" " + std::to_wstring(static_cast<int>(e.state)) + L"\n";
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);  // exclusive
    if (f == INVALID_HANDLE_VALUE) { error = L"cannot write temporary work-area record"; return false; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    const bool ok = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp.c_str()); error = L"atomic work-area replace failed"; return false; }
    return true;
}

bool FreedWork(const RECT& monitor, const RECT& work, const RECT& taskbar, RECT& out) {
    RECT tb{};
    if (!IntersectRect(&tb, &taskbar, &monitor)) return false;
    const LONG w = tb.right - tb.left, h = tb.bottom - tb.top;
    out = work;
    if (w >= h && tb.bottom >= monitor.bottom) out.bottom = std::min(monitor.bottom, work.bottom + h);
    else if (w >= h && tb.top <= monitor.top) out.top = std::max(monitor.top, work.top - h);
    else if (h > w && tb.left <= monitor.left) out.left = std::max(monitor.left, work.left - w);
    else if (h > w && tb.right >= monitor.right) out.right = std::min(monitor.right, work.right + w);
    else return false;  // floating / unknown placement: leave alone
    return !Same(out, work);
}

int ApplyWorkArea(const std::wstring& path, const std::vector<RECT>& taskbars, const WorkAreaOps& ops, std::wstring& log) {
    WorkAreaOperation prev; std::wstring error;
    if (!LoadWorkArea(path, prev, error)) { log += L"work area: " + error + L"; not applied\n"; return 0; }
    if (prev.active) { log += L"work area: previous change still recorded; not applied\n"; return 0; }
    const auto monitors = ops.monitors();
    WorkAreaOperation op; op.active = true;
    for (const auto& tb : taskbars) {
        const POINT c{(tb.left + tb.right) / 2, (tb.top + tb.bottom) / 2};
        for (const auto& m : monitors) {
            if (!PtInRect(&m.monitor, c)) continue;
            if (std::any_of(op.entries.begin(), op.entries.end(), [&](const WorkEntry& e) { return e.device == m.device; })) break;
            RECT freed{};
            if (FreedWork(m.monitor, m.work, tb, freed)) op.entries.push_back({m.device, m.monitor, m.work, freed, WorkState::Pending});
            else log += L"work area " + m.device + L": nothing reserved (e.g. Windows auto-hide); unchanged\n";
            break;
        }
    }
    if (op.entries.empty()) return 0;
    if (!SaveWorkArea(path, op, error)) { log += L"work area: " + error + L"; not applied\n"; return 0; }  // write-ahead
    int applied = 0;
    for (auto& e : op.entries) {
        const bool ok = ops.setWork(e.applied, false);
        if (ok && ops.refitMaximized) ops.refitMaximized(e.monitor, e.original, e.applied);  // maximized windows grow into the strip
        e.state = ok ? WorkState::Applied : WorkState::Skipped;
        applied += ok ? 1 : 0;
        log += L"work area " + e.device + (ok ? L": freed\n" : L": not confirmed; left as Windows set it\n");
    }
    if (!applied) op.active = false;
    if (!SaveWorkArea(path, op, error)) log += L"work area: warning: " + error + L"\n";
    return applied;
}

RecoverResult RestoreWorkArea(const std::wstring& path, const WorkAreaOps& ops, std::wstring& log) {
    if (GetFileAttributesW((path + L".tmp").c_str()) != INVALID_FILE_ATTRIBUTES) { log += L"work area: temporary record exists; preserved\n"; return RecoverResult::Refused; }
    WorkAreaOperation op; std::wstring error;
    if (!LoadWorkArea(path, op, error)) { log += L"work area: " + error + L"\n"; return RecoverResult::Refused; }
    if (!op.active) return RecoverResult::NothingToDo;
    const auto monitors = ops.monitors();
    bool failed = false, restored = false;
    for (auto& e : op.entries) {
        if (e.state != WorkState::Applied && e.state != WorkState::Pending) continue;
        const MonitorArea* m = Find(monitors, e);
        if (!m) { e.state = WorkState::Skipped; log += L"work area " + e.device + L": monitor gone or changed; nothing to restore\n"; continue; }
        if (!Same(m->work, e.applied)) {  // not (or no longer) docky's value: never overwrite
            e.state = WorkState::Skipped; log += L"work area " + e.device + L": changed by Windows or another program; left as is\n"; continue;
        }
        if (ops.setWork(e.original, true)) {
            e.state = WorkState::Restored; restored = true; log += L"work area " + e.device + L": restored\n";
            if (ops.refitMaximized) ops.refitMaximized(e.monitor, e.applied, e.original);  // never leave windows over the taskbar
        }
        else { failed = true; log += L"work area " + e.device + L": restore not confirmed; record kept\n"; }  // other monitors continue
    }
    if (!failed) op.active = false;
    if (!SaveWorkArea(path, op, error)) { log += L"work area: warning: " + error + L"\n"; return RecoverResult::Refused; }
    return failed ? RecoverResult::Refused : restored ? RecoverResult::Restored : RecoverResult::NothingToDo;
}

bool ReleaseExternallyChangedWorkArea(const std::wstring& path, const WorkAreaOps& ops, std::wstring& log, bool allowReapply, int* reapplied) {
    if (reapplied) *reapplied = 0;
    WorkAreaOperation op; std::wstring error;
    if (!LoadWorkArea(path, op, error) || !op.active) return false;
    const auto monitors = ops.monitors();
    bool changed = false;
    for (auto& e : op.entries) {
        if (e.state != WorkState::Applied) continue;
        const MonitorArea* m = Find(monitors, e);
        if (m && Same(m->work, e.applied)) continue;
        // Exactly the recorded original = Explorer re-deriving its own appbar space (not another program).
        if (m && allowReapply && Same(m->work, e.original) && ops.setWork(e.applied, false)) {
            if (ops.refitMaximized) ops.refitMaximized(e.monitor, e.original, e.applied);
            if (reapplied) ++*reapplied;
            log += L"work area " + e.device + L": Explorer re-derived the reserved strip; freed again\n";
            continue;
        }
        e.state = WorkState::Skipped; changed = true;
        log += L"work area " + e.device + L": changed externally (Explorer/user/display); released, not re-applied\n";
    }
    if (!changed) return false;
    op.active = std::any_of(op.entries.begin(), op.entries.end(), [](const WorkEntry& e) { return e.state == WorkState::Applied; });
    SaveWorkArea(path, op, error);
    return true;
}
}  // namespace docky::taskbar
