#include "taskbar_controller.h"
#include "taskbar_guardian.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace docky::taskbar {
using namespace liquid::desktop;

const wchar_t* StateName(State s) { return s == State::Hidden ? L"hidden" : s == State::Conflict ? L"conflict" : L"visible"; }

Backend Backend::Real(std::function<bool()> guardianReady) {
    Backend b;
    b.capture = [] { return CaptureTaskbarWindows(); };
    b.ops = WindowOps::Real();
    const auto observe = b.ops.observe;
    // Only windows owned by the current shell process are ever touched.
    b.ops.observe = [observe](const TaskbarWindowRecord& wanted, TaskbarWindowRecord& now) {
        DWORD shellPid = 0;
        if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
        return shellPid && wanted.pid == shellPid && observe(wanted, now);
    };
    b.guardianReady = std::move(guardianReady);
    auto held = std::make_shared<std::unique_ptr<RecordLock>>();
    b.lock = [held] { *held = std::make_unique<RecordLock>(); return (*held)->Held(); };
    b.unlock = [held] { held->reset(); };
    b.work = WorkAreaOps::Real();
    b.windowRect = [](ULONG_PTR hwnd, RECT& out) { HWND h = reinterpret_cast<HWND>(hwnd); return IsWindow(h) && GetWindowRect(h, &out); };
    b.now = [] { return GetTickCount64(); };
    return b;
}

// A <record>.tmp is only written for milliseconds by SaveWindowOperation, and the main record is replaced
// atomically, so the main record is always the last committed state. Called with the record lock held: a temp
// file older than a few seconds is left over from an interrupted save (crash, power loss) and would otherwise block
// every later hide or show. It is moved aside, never deleted or overwritten, so its content stays for review.
bool SetAsideStaleTempRecord(const std::wstring& path, std::wstring& log) {
    const std::wstring tmp = path + L".tmp";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(tmp.c_str(), GetFileExInfoStandard, &a)) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    FILETIME nowFt{}; GetSystemTimeAsFileTime(&nowFt);
    auto ticks = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const ULONGLONG written = (std::max)(ticks(a.ftLastWriteTime), ticks(a.ftCreationTime)), now = ticks(nowFt);
    if (now < written || now - written < 10ull * 10000000ull) { log += L"temp record is recent; preserved\n"; return false; }
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (int i = 0; i < 20; ++i) {
        const std::wstring aside = tmp + L".stale-" + stamp + (i ? L"-" + std::to_wstring(i) : L"");
        if (MoveFileExW(tmp.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) { log += L"stale temp record moved aside: " + aside + L"\n"; return true; }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    log += L"stale temp record could not be moved aside; preserved\n";
    return false;
}

Controller::Controller(std::wstring recordPath, Backend backend) : path_(std::move(recordPath)), b_(std::move(backend)) {}

bool Controller::Locked(const std::function<bool()>& body) {
    if (b_.lock && !b_.lock()) { if (b_.unlock) b_.unlock(); log_ += L"record lock busy\n"; return false; }
    const bool r = body();
    if (b_.unlock) b_.unlock();
    return r;
}

void Controller::Adopt() {
    TaskbarWindowOperation op; std::wstring error;
    if (!LoadWindowOperation(path_, op, error) || op.phase == RecordPhase::Conflict) { state_ = State::Conflict; return; }
    state_ = op.phase == RecordPhase::Active ? State::Hidden : State::Visible;
}

// Free the work area of the monitors whose taskbar docky currently hides (opt-in only).
void Controller::ApplyWorkLocked() {
    if (!freeWork_ || !b_.work.monitors || !b_.work.setWork || !b_.windowRect) return;
    TaskbarWindowOperation op; std::wstring error;
    if (!LoadWindowOperation(path_, op, error) || op.phase != RecordPhase::Active) return;
    std::vector<RECT> rects;
    for (const auto& w : op.windows) { RECT r{}; if (w.state == WindowState::Hidden && b_.windowRect(w.original.hwnd, r)) rects.push_back(r); }
    if (!rects.empty()) ApplyWorkArea(WorkAreaPathFor(path_), rects, b_.work, log_);
}

void Controller::SetFreeWorkArea(bool on) {
    if (freeWork_ == on) return;
    freeWork_ = on;
    if (state_ != State::Hidden) return;
    Locked([&] {
        if (on) ApplyWorkLocked();
        else if (b_.work.monitors) RestoreWorkArea(WorkAreaPathFor(path_), b_.work, log_);
        return true;
    });
}

bool Controller::Hide(std::wstring& why) {
    if (state_ == State::Hidden) return true;
    if (!b_.guardianReady || !b_.guardianReady()) { why = L"Recovery guardian is not ready; the taskbar was not hidden."; return false; }
    bool ok = false;
    Locked([&] {
        if (!SetAsideStaleTempRecord(path_, log_)) { why = L"A taskbar record is being written; try again in a few seconds."; return false; }
        TaskbarWindowOperation prev; std::wstring error;
        if (!LoadWindowOperation(path_, prev, error)) { state_ = State::Conflict; why = L"The taskbar record is unreadable (" + error + L"); nothing was hidden."; return false; }
        if (prev.phase == RecordPhase::Conflict) { state_ = State::Conflict; why = L"A taskbar Conflict record needs review; nothing was hidden."; return false; }
        if (prev.phase == RecordPhase::Active) { state_ = State::Hidden; ok = true; return true; }  // already ours
        const auto current = b_.capture();
        if (PlanHide(current).empty()) { why = L"No visible Windows taskbar was found."; return false; }
        const int hidden = HideTaskbarWindows(path_, current, b_.ops, log_);
        if (hidden <= 0) { why = L"Windows did not hide the taskbar; nothing changed."; return false; }
        state_ = State::Hidden; newWindowPolls_ = 0; ok = true;
        ApplyWorkLocked();
        return true;
    });
    if (!ok && why.empty()) why = L"The record is busy; try again.";
    return ok;
}

bool Controller::ShowLocked(std::wstring& why) {
    // Work area first, so maximized windows never overlap the taskbar while it reappears. A refused work-area
    // record never blocks showing the taskbar.
    if (b_.work.monitors && b_.work.setWork) RestoreWorkArea(WorkAreaPathFor(path_), b_.work, log_);
    TaskbarWindowOperation op; std::wstring error;
    if (!LoadWindowOperation(path_, op, error)) { state_ = State::Conflict; why = L"The taskbar record is unreadable; use Restore Windows taskbar now."; return false; }
    if (op.phase != RecordPhase::Closed && !SetAsideStaleTempRecord(path_, log_)) { why = L"A taskbar record is being written; try again in a few seconds."; return false; }
    if (op.phase == RecordPhase::Closed) { state_ = State::Visible; return true; }
    if (op.phase == RecordPhase::Conflict || !IsConservativeRecoveryRecord(op)) { state_ = State::Conflict; why = L"The taskbar record is ambiguous and was preserved; nothing was forced."; return false; }
    if (!RestoreTaskbarWindows(path_, b_.ops, log_)) { state_ = State::Conflict; why = L"A taskbar window could not be confirmed; the record was kept as Conflict."; return false; }
    state_ = State::Visible; newWindowPolls_ = 0;
    return true;
}

bool Controller::Show(std::wstring& why) {
    bool ok = false;
    Locked([&] { ok = ShowLocked(why); return ok; });
    if (!ok && why.empty()) why = L"The record is busy; try again.";
    return ok;
}

bool Controller::Poll(std::wstring& log) {
    if (state_ != State::Hidden) return false;
    const State before = state_;
    Locked([&] {
        TaskbarWindowOperation op; std::wstring error;
        if (!LoadWindowOperation(path_, op, error)) { state_ = State::Conflict; log += L"poll: record unreadable -> Conflict\n"; return true; }
        if (op.phase == RecordPhase::Closed) {
            if (b_.work.monitors && b_.work.setWork) RestoreWorkArea(WorkAreaPathFor(path_), b_.work, log);
            state_ = State::Visible; log += L"poll: record closed elsewhere (guardian or restore tool) -> visible\n"; return true;
        }
        if (op.phase == RecordPhase::Conflict) { state_ = State::Conflict; log += L"poll: Conflict record -> Conflict\n"; return true; }
        std::wstring why;
        if (!b_.guardianReady || !b_.guardianReady()) {  // never stay hidden without a recovery guardian
            ShowLocked(why); log += L"poll: guardian lost -> restored " + why + L"\n"; return true;
        }
        if (b_.work.monitors && b_.work.setWork) {
            // Explorer reverting to exactly the recorded original may be answered a few times per minute; any other
            // value (user / other program / display change) is released and never overwritten.
            const ULONGLONG t = b_.now ? b_.now() : GetTickCount64();
            while (!workReapplies_.empty() && t - workReapplies_.front() > 60000) workReapplies_.pop_front();
            int again = 0;
            ReleaseExternallyChangedWorkArea(WorkAreaPathFor(path_), b_.work, log,
                                             freeWork_ && static_cast<int>(workReapplies_.size()) < kMaxRehides, &again);
            for (int i = 0; i < again; ++i) workReapplies_.push_back(t);
        }
        // Windows docky hid: still present and hidden, shown by someone else, or gone (Explorer restarted).
        int ownedHidden = 0, shownExternally = 0;
        for (const auto& w : op.windows) {
            if (w.state != WindowState::Hidden) continue;
            TaskbarWindowRecord now;
            const bool exists = b_.ops.observe(w.original, now);
            const auto plan = PlanRestore(w.original, exists, now);
            if (plan == WindowRestore::Restore) ++ownedHidden;
            else if (plan == WindowRestore::AlreadyVisible) ++shownExternally;
        }
        const auto current = b_.capture();
        auto recorded = [&](const TaskbarWindowRecord& c) {
            return std::any_of(op.windows.begin(), op.windows.end(), [&](const OwnedWindow& w) {
                return w.original.hwnd == c.hwnd && w.original.pid == c.pid && w.original.processStart == c.processStart; });
        };
        const bool unowned = std::any_of(current.begin(), current.end(), [&](const TaskbarWindowRecord& c) { return c.visible && !recorded(c); });
        if (shownExternally > 0 && ownedHidden == 0 && !unowned) {
            // The user or another program showed the taskbar: respect it; close our record without re-hiding.
            ShowLocked(why); log += L"poll: taskbar shown externally; not re-hidden -> visible\n"; return true;
        }
        if (unowned) {
            // Explorer restarted or a monitor was added: wait until the new window persists for two polls.
            if (++newWindowPolls_ < 2) { log += L"poll: new taskbar window seen; waiting for it to settle\n"; return true; }
            newWindowPolls_ = 0;
            const ULONGLONG t = b_.now ? b_.now() : GetTickCount64();
            while (!rehides_.empty() && t - rehides_.front() > 60000) rehides_.pop_front();
            if (static_cast<int>(rehides_.size()) >= kMaxRehides) {  // Explorer keeps rebuilding: stop, never loop
                ShowLocked(why); state_ = State::Visible;
                log += L"poll: taskbar rebuilt repeatedly; left visible (hide again with the shortcut)\n"; return true;
            }
            rehides_.push_back(t);
            if (!ShowLocked(why)) { log += L"poll: re-hide aborted: " + why + L"\n"; return true; }
            const auto fresh = b_.capture();
            const int hidden = HideTaskbarWindows(path_, fresh, b_.ops, log);
            state_ = hidden > 0 ? State::Hidden : State::Visible;
            if (hidden > 0) ApplyWorkLocked();
            log += hidden > 0 ? L"poll: taskbar windows recreated; hidden again\n" : L"poll: re-hide failed; left visible\n";
            return true;
        }
        newWindowPolls_ = 0;
        if (ownedHidden == 0 && shownExternally == 0) {  // every owned window is gone and none replaced yet
            log += L"poll: owned taskbar windows gone; waiting for Explorer\n";
        }
        return true;
    });
    return state_ != before;
}
}  // namespace docky::taskbar
