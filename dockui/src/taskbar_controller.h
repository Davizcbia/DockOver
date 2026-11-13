#pragma once
// Desktop Manager taskbar state machine (docky "full hide", distinct from Windows' own auto-hide setting).
//
//   Visible --Hide()--> Hidden          only when the guardian is ready and the record is Closed
//   Hidden  --Show()--> Visible         restores only windows docky hid that are still docky's and hidden
//   Hidden  --Poll()--> Visible         record closed elsewhere (guardian / restore tool), all hidden windows were
//                                       shown by the user or another program (never re-hidden), or guardian lost
//   Hidden  --Poll()--> Hidden          Explorer restart / new monitor taskbar: re-hide after it settled
//   any     --conflict--> Conflict      ambiguous/corrupt record: no further automatic hide until resolved
//
// Every window operation runs under the cross-process record lock and the write-ahead record of taskbar_window.
// Optional work-area release (taskbar_workarea) is applied after hiding and restored before showing; Explorer
// rebuilds are re-hidden at most kMaxRehides times per minute to rule out hide/show flicker loops.
#include "taskbar_window.h"
#include "taskbar_workarea.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace docky::taskbar {
struct Backend {
    std::function<std::vector<liquid::desktop::TaskbarWindowRecord>()> capture;  // current shell's taskbar windows
    liquid::desktop::WindowOps ops;                                              // observe / ShowWindow
    std::function<bool()> guardianReady;                                         // process alive + handshake done
    std::function<bool()> lock;                                                  // acquire record lock (true = held)
    std::function<void()> unlock;
    WorkAreaOps work;                                                            // monitor topology / SPI_SETWORKAREA
    std::function<bool(ULONG_PTR hwnd, RECT& out)> windowRect;                  // taskbar window rect (even hidden)
    std::function<ULONGLONG()> now;                                              // ms clock (rate limit)
    static Backend Real(std::function<bool()> guardianReady);
};
inline constexpr int kMaxRehides = 3;  // per 60 s

enum class State { Visible, Hidden, Conflict };
const wchar_t* StateName(State s);

class Controller {
public:
    Controller(std::wstring recordPath, Backend backend);
    State Current() const { return state_; }
    // Each returns true on success; `why` receives a one-line, user-facing reason on failure.
    bool Hide(std::wstring& why);
    bool Show(std::wstring& why);
    bool Toggle(std::wstring& why) { return state_ == State::Hidden ? Show(why) : Hide(why); }
    // Periodic supervision (about once per second). Returns true when the state changed; `log` gets details.
    bool Poll(std::wstring& log);
    // Synchronize with an existing record at startup (e.g. Hidden left by a previous session was recovered).
    void Adopt();
    // Opt-in work-area release. Changing it while hidden applies / restores immediately.
    void SetFreeWorkArea(bool on);
    bool FreeWorkArea() const { return freeWork_; }
    const std::wstring& LastLog() const { return log_; }
private:
    bool Locked(const std::function<bool()>& body);
    bool ShowLocked(std::wstring& why);
    void ApplyWorkLocked();
    std::wstring path_;
    Backend b_;
    State state_ = State::Visible;
    bool freeWork_ = false;
    int newWindowPolls_ = 0;  // consecutive polls that saw an unowned visible taskbar window while Hidden
    std::deque<ULONGLONG> rehides_;
    std::deque<ULONGLONG> workReapplies_;  // Explorer reverting the freed work area: at most kMaxRehides per minute
    std::wstring log_;
};
}  // namespace docky::taskbar
