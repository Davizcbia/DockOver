// Simulated-taskbar tests for the Manager state machine, hotkey model and taskbar settings persistence.
// No real taskbar window is observed or changed: the backend operates on an in-memory window table.
#include "taskbar_controller.h"
#include "taskbar_guardian.h"
#include "taskbar_hotkey.h"
#include "desktop_settings.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace docky::taskbar;
using namespace liquid::desktop;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

struct FakeShell {
    std::map<ULONG_PTR, TaskbarWindowRecord> windows;  // hwnd -> state (exists while present)
    DWORD pid = 4000; ULONGLONG start = 777;
    bool guardian = true;
    void Add(ULONG_PTR hwnd, const wchar_t* cls) { windows[hwnd] = {cls, hwnd, pid, start, true}; }
    void RestartExplorer(ULONG_PTR firstHwnd, int count) {  // old windows vanish, new process, new visible windows
        windows.clear(); ++pid; start += 100;
        for (int i = 0; i < count; ++i) Add(firstHwnd + i, i == 0 ? L"Shell_TrayWnd" : L"Shell_SecondaryTrayWnd");
    }
    bool Visible(ULONG_PTR h) const { auto i = windows.find(h); return i != windows.end() && i->second.visible; }
    Backend Make() {
        Backend b;
        b.capture = [this] { std::vector<TaskbarWindowRecord> v; for (auto& [h, r] : windows) v.push_back(r); return v; };
        b.ops.observe = [this](const TaskbarWindowRecord& want, TaskbarWindowRecord& now) {
            auto i = windows.find(want.hwnd); if (i == windows.end()) return false; now = i->second; return true; };
        b.ops.setVisible = [this](ULONG_PTR h, bool show) { auto i = windows.find(h); if (i == windows.end()) return false; i->second.visible = show; return true; };
        b.guardianReady = [this] { return guardian; };
        return b;
    }
};

static std::wstring TempDir() {
    wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"docky-taskbar-controller-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(d.c_str(), nullptr); return d;
}
static RecordPhase Phase(const std::wstring& path) { TaskbarWindowOperation op; std::wstring e; LoadWindowOperation(path, op, e); return op.phase; }

int wmain() {
    const auto dir = TempDir();
    // ---- state machine on a simulated two-monitor taskbar ----
    {
        const auto rec = dir + L"\\record-a.txt";
        FakeShell shell; shell.Add(100, L"Shell_TrayWnd"); shell.Add(200, L"Shell_SecondaryTrayWnd");
        Controller c(rec, shell.Make()); std::wstring why, log;
        shell.guardian = false;
        CHECK(!c.Hide(why) && c.Current() == State::Visible && shell.Visible(100));  // no guardian -> no hide
        shell.guardian = true;
        CHECK(c.Hide(why) && c.Current() == State::Hidden);
        CHECK(!shell.Visible(100) && !shell.Visible(200));                            // primary + secondary
        CHECK(Phase(rec) == RecordPhase::Active);
        CHECK(c.Toggle(why) && c.Current() == State::Visible && shell.Visible(100) && shell.Visible(200));
        CHECK(Phase(rec) == RecordPhase::Closed);
        CHECK(c.Toggle(why) && c.Current() == State::Hidden);
        // user / other program shows both windows: respected, not re-hidden; record closed
        shell.windows[100].visible = shell.windows[200].visible = true;
        CHECK(c.Poll(log) && c.Current() == State::Visible && Phase(rec) == RecordPhase::Closed);
        CHECK(!c.Poll(log) && shell.Visible(100));
        // Explorer restart while hidden: new windows re-hidden only after they persisted for two polls
        CHECK(c.Hide(why));
        shell.RestartExplorer(300, 2);
        c.Poll(log); CHECK(c.Current() == State::Hidden && shell.Visible(300));   // settling
        c.Poll(log); CHECK(c.Current() == State::Hidden && !shell.Visible(300) && !shell.Visible(301));
        CHECK(Phase(rec) == RecordPhase::Active);
        // monitor added while hidden: the new secondary taskbar is hidden too
        shell.Add(400, L"Shell_SecondaryTrayWnd");
        c.Poll(log); c.Poll(log); CHECK(c.Current() == State::Hidden && !shell.Visible(400) && !shell.Visible(300));
        // guardian lost while hidden: restore immediately
        shell.guardian = false;
        CHECK(c.Poll(log) && c.Current() == State::Visible && shell.Visible(300) && shell.Visible(400));
        shell.guardian = true;
        // record closed elsewhere (guardian "Restore now" / restore tool) -> Visible without touching windows
        CHECK(c.Hide(why));
        { TaskbarWindowOperation op; std::wstring e; LoadWindowOperation(rec, op, e); op.phase = RecordPhase::Closed; SaveWindowOperation(rec, op, e); }
        CHECK(c.Poll(log) && c.Current() == State::Visible);
        for (auto& [h, r] : shell.windows) r.visible = true;
    }
    // ---- corrupt and Pending records are never forced ----
    {
        const auto rec = dir + L"\\record-b.txt";
        { std::ofstream f(rec); f << "garbage"; }
        FakeShell shell; shell.Add(100, L"Shell_TrayWnd");
        Controller c(rec, shell.Make()); std::wstring why;
        CHECK(!c.Hide(why) && c.Current() == State::Conflict && shell.Visible(100));
        { std::ifstream f(rec); std::string s; f >> s; CHECK(s == "garbage"); }  // preserved byte for byte
        const auto pend = dir + L"\\record-c.txt";
        TaskbarWindowOperation op; op.phase = RecordPhase::Active; op.windows.push_back({{L"Shell_TrayWnd", 100, shell.pid, shell.start, true}, false, WindowState::Pending});
        std::wstring e; CHECK(SaveWindowOperation(pend, op, e));
        shell.windows[100].visible = false;
        Controller p(pend, shell.Make()); p.Adopt();
        CHECK(!p.Show(why) && p.Current() == State::Conflict && !shell.Visible(100));  // ambiguous: left as is
        std::wstring log; CHECK(RecoverTaskbarRecord(pend, log) == RecoverResult::Refused);
    }
    // ---- hotkey model ----
    {
        using namespace docky::hotkey;
        CHECK(Validate(kDefaultModifiers, kDefaultKey) == Validity::Ok);
        CHECK(Text(kDefaultModifiers, kDefaultKey) == L"Ctrl + Alt + T");
        CHECK(Validate(MOD_ALT, VK_TAB) == Validity::Reserved);
        CHECK(Validate(MOD_ALT, VK_F4) == Validity::Reserved);
        CHECK(Validate(MOD_CONTROL | MOD_SHIFT, VK_ESCAPE) == Validity::Reserved);
        CHECK(Validate(MOD_CONTROL | MOD_ALT, VK_F12) == Validity::Reserved);
        CHECK(Validate(MOD_WIN, 'T') == Validity::UsesWinKey);
        CHECK(Validate(MOD_SHIFT, 'T') == Validity::NeedsCtrlOrAlt);
        CHECK(Validate(MOD_CONTROL, VK_CONTROL) == Validity::ModifierOnly);
        CHECK(Validate(MOD_CONTROL | MOD_ALT, 0) == Validity::Empty);
        // real registration conflict: the second registration of the same combination must fail
        const UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, vk = VK_F9;
        if (RegisterHotKey(nullptr, 1, mods, vk)) {
            CHECK(!RegisterHotKey(nullptr, 2, mods, vk) && GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED);
            CHECK(UnregisterHotKey(nullptr, 1));
            CHECK(RegisterHotKey(nullptr, 2, mods, vk) && UnregisterHotKey(nullptr, 2));  // free again after unregister
        } else std::puts("note: Ctrl+Alt+Shift+F9 is held by another app; conflict check skipped");
    }
    // ---- settings persistence (isolated file) ----
    {
        using namespace liquid::settings;
        const auto path = dir + L"\\settings.json";
        auto r = Load(path); CHECK(r.status == LoadStatus::Missing);
        CHECK(!r.value.taskbar.hideOnStartup && r.value.taskbar.hotkeyEnabled && r.value.taskbar.hotkeyModifiers == 3 && r.value.taskbar.hotkeyKey == 'T');
        CHECK(WriteBool(path, "taskbar", "hideOnStartup", true) == SaveStatus::Saved);
        CHECK(WriteTaskbarInt(path, "hotkeyModifiers", MOD_CONTROL | MOD_SHIFT) == SaveStatus::Saved);
        CHECK(WriteTaskbarInt(path, "hotkeyKey", 'K') == SaveStatus::Saved);
        CHECK(WriteBool(path, "taskbar", "hotkeyEnabled", false) == SaveStatus::Saved);
        CHECK(WriteTaskbarInt(path, "hotkeyModifiers", MOD_WIN) == SaveStatus::Invalid);   // Win never stored
        CHECK(WriteTaskbarInt(path, "hotkeyKey", 999) == SaveStatus::Invalid);
        r = Load(path);
        CHECK(r.status == LoadStatus::Loaded && r.value.taskbar.hideOnStartup && !r.value.taskbar.hotkeyEnabled);
        CHECK(r.value.taskbar.hotkeyModifiers == (MOD_CONTROL | MOD_SHIFT) && r.value.taskbar.hotkeyKey == 'K');
        const auto bad = dir + L"\\settings-bad.json";
        { std::ofstream f(bad); f << "{\"schemaVersion\":1,\"taskbar\":{\"hotkeyKey\":\"T\"}}"; }
        CHECK(Load(bad).status == LoadStatus::Invalid);
    }
    // ---- status file round trip ----
    {
        const auto path = dir + L"\\status.txt";
        Status s; s.managerPid = 1234; s.state = L"hidden"; s.guardian = L"ready"; s.hotkey = L"conflict"; s.hotkeyText = L"Ctrl + Alt + T"; s.message = L"in use";
        CHECK(WriteStatus(path, s));
        Status r; CHECK(ReadStatus(path, r) && r.managerPid == 1234 && r.state == L"hidden" && r.guardian == L"ready" && r.hotkey == L"conflict" && r.hotkeyText == L"Ctrl + Alt + T");
        r.managerPid = GetCurrentProcessId(); CHECK(!StatusIsLive(r));  // a non-Manager process is never "docky"
    }
    // ---- shortcut recording lease (real named objects, isolated prefix; threads stand in for Settings windows) ----
    {
        const std::wstring prefix = L"DockyTest" + std::to_wstring(GetCurrentProcessId());
        HotkeySuspension manager(prefix);
        CHECK(!manager.OnChanged() && !manager.Suspended());                      // nobody records
        HANDLE acquired = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        struct Ctx { std::wstring prefix; HANDLE acquired, release; bool ok = false, acked = false; bool abandon = false; } ctx{prefix, acquired, release};
        auto recorder = [](void* p) -> DWORD {
            auto* c = static_cast<Ctx*>(p); RecordingLease lease(c->prefix); std::wstring why;
            c->ok = lease.Acquire(1500, c->acked, why); SetEvent(c->acquired);
            WaitForSingleObject(c->release, 10000);
            if (c->abandon) ExitThread(0);  // simulate a crashed Settings: the mutex is abandoned, never released
            return 0;
        };
        // Manager reacts to the change event in its own loop; emulate that on a helper thread.
        HANDLE mgrThread = nullptr;
        struct M { HotkeySuspension* s; bool suspended = false; } m{&manager};
        mgrThread = CreateThread(nullptr, 0, [](void* p) -> DWORD { auto* m = static_cast<M*>(p);
            if (WaitForSingleObject(m->s->ChangedEvent(), 3000) == WAIT_OBJECT_0 && m->s->OnChanged()) { m->suspended = true; m->s->Acknowledge(); }
            return 0; }, &m, 0, nullptr);
        HANDLE t1 = CreateThread(nullptr, 0, recorder, &ctx, 0, nullptr);
        WaitForSingleObject(acquired, 5000); WaitForSingleObject(mgrThread, 5000);
        CHECK(ctx.ok && ctx.acked && m.suspended && manager.Suspended());       // handshake: suspended before recording
        // a second Settings window cannot record at the same time
        { RecordingLease other(prefix); bool acked = false; std::wstring why; CHECK(!other.Acquire(0, acked, why) && !why.empty()); }
        // while suspended the Manager's combination is free for the Settings trial registration
        if (RegisterHotKey(nullptr, 7, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F8)) { CHECK(UnregisterHotKey(nullptr, 7)); }
        // lease ends -> the lease handle the Manager waits on becomes signaled
        SetEvent(release); WaitForSingleObject(t1, 5000);
        DWORD w = WaitForSingleObject(manager.LeaseHandle(), 3000); CHECK(w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
        manager.OnLeaseEnded(); CHECK(!manager.Suspended());
        // crashed recorder: abandoned mutex also ends the suspension
        ResetEvent(acquired); ResetEvent(release); ctx.abandon = true; ctx.ok = ctx.acked = false; m.suspended = false;
        mgrThread = CreateThread(nullptr, 0, [](void* p) -> DWORD { auto* m = static_cast<M*>(p);
            if (WaitForSingleObject(m->s->ChangedEvent(), 3000) == WAIT_OBJECT_0 && m->s->OnChanged()) { m->suspended = true; m->s->Acknowledge(); }
            return 0; }, &m, 0, nullptr);
        HANDLE t2 = CreateThread(nullptr, 0, recorder, &ctx, 0, nullptr);
        WaitForSingleObject(acquired, 5000); WaitForSingleObject(mgrThread, 5000); CHECK(manager.Suspended());
        SetEvent(release); WaitForSingleObject(t2, 5000);
        w = WaitForSingleObject(manager.LeaseHandle(), 3000); CHECK(w == WAIT_ABANDONED || w == WAIT_OBJECT_0);
        manager.OnLeaseEnded(); CHECK(!manager.Suspended());
        for (HANDLE h : {acquired, release, mgrThread, t1, t2}) CloseHandle(h);
    }
    // ---- work area (simulated topology: 3 monitors, one with Windows auto-hide) ----
    {
        struct FakeWork {
            std::vector<MonitorArea> mons; std::wstring failDevice; int broadcasts = 0, silent = 0;
            WorkAreaOps Make() { WorkAreaOps o;
                o.monitors = [this] { return mons; };
                o.setWork = [this](const RECT& r, bool broadcast) { const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                    for (auto& m : mons) if (PtInRect(&m.monitor, c)) { if (m.device == failDevice) return false; m.work = r; ++(broadcast ? broadcasts : silent); return true; } return false; };
                return o; }
            MonitorArea& M(const wchar_t* d) { for (auto& m : mons) if (m.device == d) return m; return mons[0]; }
        };
        auto same = [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; };
        FakeWork fw;
        fw.mons = {{L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, {0, 0, 2560, 1392}},       // taskbar bottom 48 px
                   {L"\\\\.\\DISPLAY2", {2560, 0, 4480, 1080}, {2560, 0, 4480, 1032}}, // secondary taskbar bottom
                   {L"\\\\.\\DISPLAY3", {-1920, 0, 0, 1080}, {-1920, 0, 0, 1080}}};    // Windows auto-hide: nothing reserved
        const std::vector<RECT> bars{{0, 1392, 2560, 1440}, {2560, 1032, 4480, 1080}, {-1920, 1078, 0, 1080}};
        const auto wpath = dir + L"\\work.txt"; std::wstring log;
        RECT freed{}; CHECK(FreedWork({0, 0, 1000, 800}, {0, 40, 1000, 800}, {0, 0, 1000, 40}, freed) && freed.top == 0);  // top taskbar
        CHECK(FreedWork({0, 0, 1000, 800}, {60, 0, 1000, 800}, {0, 0, 60, 800}, freed) && freed.left == 0);               // left taskbar
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 2);
        CHECK(fw.silent == 2 && fw.broadcasts == 0);   // docky's value is set without the WM_SETTINGCHANGE broadcast
        CHECK(same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1440}) && same(fw.M(L"\\\\.\\DISPLAY2").work, {2560, 0, 4480, 1080}));
        // Explorer re-derives exactly the original on DISPLAY1: re-applied only when the caller allows it
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1392};
        { int again = 0; CHECK(!ReleaseExternallyChangedWorkArea(wpath, fw.Make(), log, true, &again) && again == 1 && same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1440})); }
        // a third value (another program) is released even when re-apply is allowed
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1400};
        { int again = 0; CHECK(ReleaseExternallyChangedWorkArea(wpath, fw.Make(), log, true, &again) && again == 0 && same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1400})); }
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1440};  // back to docky's value for the following checks (entry released)
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Restored && fw.broadcasts == 1);  // restore broadcasts
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1392};
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 2);
        CHECK(same(fw.M(L"\\\\.\\DISPLAY3").work, {-1920, 0, 0, 1080}));                                         // auto-hide monitor untouched
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 0);                                                  // never twice
        // Explorer re-derives DISPLAY2 (e.g. display change): released while hidden, never re-applied or overwritten
        fw.M(L"\\\\.\\DISPLAY2").work = {2560, 0, 4480, 1032};
        CHECK(ReleaseExternallyChangedWorkArea(wpath, fw.Make(), log));
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Restored);
        CHECK(same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1392}) && same(fw.M(L"\\\\.\\DISPLAY2").work, {2560, 0, 4480, 1032}));
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::NothingToDo);                            // idempotent
        // another program changes DISPLAY1 after docky applied: restore leaves it alone
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 2);
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1300};
        RestoreWorkArea(wpath, fw.Make(), log);
        CHECK(same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1300}) && same(fw.M(L"\\\\.\\DISPLAY2").work, {2560, 0, 4480, 1032}));
        // monitor unplugged while applied: nothing to restore there, others restored
        fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1392};
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 2);
        auto unplugged = fw.mons; fw.mons.erase(fw.mons.begin() + 1);
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Restored && same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1392}));
        fw.mons = unplugged; fw.M(L"\\\\.\\DISPLAY1").work = {0, 0, 2560, 1392}; fw.M(L"\\\\.\\DISPLAY2").work = {2560, 0, 4480, 1032};  // monitor back, Windows defaults
        // one monitor fails to restore: the others still restore and the record stays active for retry
        CHECK(ApplyWorkArea(wpath, bars, fw.Make(), log) == 2);
        fw.failDevice = L"\\\\.\\DISPLAY2";
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Refused && same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1392}));
        fw.failDevice.clear();
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Restored && same(fw.M(L"\\\\.\\DISPLAY2").work, {2560, 0, 4480, 1032}));
        // Pending (crash between write-ahead and SPI): restored only if the current value is exactly docky's
        { WorkAreaOperation op; op.active = true; op.entries.push_back({L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, {0, 0, 2560, 1392}, {0, 0, 2560, 1440}, WorkState::Pending});
          std::wstring e; CHECK(SaveWorkArea(wpath, op, e)); }
        RestoreWorkArea(wpath, fw.Make(), log); CHECK(same(fw.M(L"\\\\.\\DISPLAY1").work, {0, 0, 2560, 1392}));  // was never applied: untouched
        // corrupt record: preserved, nothing changed
        { std::ofstream f(wpath); f << "docky-workarea 1\nactive 1\nm broken\n"; }
        CHECK(RestoreWorkArea(wpath, fw.Make(), log) == RecoverResult::Refused);
        { std::ifstream f(wpath); std::stringstream s; s << f.rdbuf(); CHECK(s.str().find("m broken") != std::string::npos); }
    }
    // ---- controller: 3 monitors, unplug while hidden, partial failure, work area wiring, Explorer rebuild limit ----
    {
        const auto rec = dir + L"\\record-m.txt";
        FakeShell shell; shell.Add(10, L"Shell_TrayWnd"); shell.Add(20, L"Shell_SecondaryTrayWnd"); shell.Add(30, L"Shell_SecondaryTrayWnd");
        ULONGLONG clock = 1000;
        auto b = shell.Make(); b.now = [&clock] { return clock; };
        Controller c(rec, b); std::wstring why, log;
        CHECK(c.Hide(why) && !shell.Visible(10) && !shell.Visible(20) && !shell.Visible(30));
        shell.windows.erase(30);                                                   // monitor 3 unplugged while hidden
        c.Poll(log); CHECK(c.Current() == State::Hidden && !shell.Visible(10));
        CHECK(c.Show(why) && shell.Visible(10) && shell.Visible(20));
        CHECK(c.Hide(why));
        bool failOnce = true; auto set = b.ops.setVisible;                         // monitor 2 refuses to show once
        auto b2 = b; b2.ops.setVisible = [&](ULONG_PTR h, bool show) { if (h == 20 && show && failOnce) { failOnce = false; return false; } return set(h, show); };
        Controller c2(rec, b2); c2.Adopt();
        CHECK(!c2.Show(why) && c2.Current() == State::Conflict && shell.Visible(10) && !shell.Visible(20));  // others still restored
        shell.windows[20].visible = true;
        // Explorer rebuilding the taskbar again and again: re-hidden at most kMaxRehides times per minute
        const auto rec2 = dir + L"\\record-r.txt";
        FakeShell s2; s2.Add(100, L"Shell_TrayWnd"); auto b3 = s2.Make(); b3.now = [&clock] { return clock; };
        Controller r(rec2, b3); CHECK(r.Hide(why));
        int rehidden = 0;
        for (int i = 0; i < 5; ++i) {
            s2.RestartExplorer(200 + i * 10, 1); clock += 2000;
            r.Poll(log); r.Poll(log);
            if (r.Current() == State::Hidden && !s2.Visible(200 + i * 10)) ++rehidden;
        }
        CHECK(rehidden == kMaxRehides && r.Current() == State::Visible && s2.Visible(240));  // stopped, left visible
    }
    // ---- controller + work area: Explorer keeps re-deriving the strip -> re-freed at most kMaxRehides per minute ----
    {
        const auto rec = dir + L"\\record-w.txt";
        FakeShell shell; shell.Add(10, L"Shell_TrayWnd");
        MonitorArea mon{L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, {0, 0, 2560, 1392}};
        int silent = 0, broadcast = 0; ULONGLONG clock = 5000;
        auto b = shell.Make(); b.now = [&clock] { return clock; };
        b.work.monitors = [&mon] { return std::vector<MonitorArea>{mon}; };
        b.work.setWork = [&](const RECT& r, bool bc) { mon.work = r; ++(bc ? broadcast : silent); return true; };
        b.windowRect = [](ULONG_PTR, RECT& r) { r = {0, 1392, 2560, 1440}; return true; };
        Controller c(rec, b); std::wstring why, log;
        CHECK(c.Hide(why) && mon.work.bottom == 1392);   // option off: work area untouched
        CHECK(c.Show(why));
        c.SetFreeWorkArea(true);
        CHECK(c.Hide(why) && mon.work.bottom == 1440 && silent == 1);
        int refreed = 0;
        for (int i = 0; i < 5; ++i) { mon.work.bottom = 1392; clock += 1000; c.Poll(log); if (mon.work.bottom == 1440) ++refreed; }
        CHECK(refreed == kMaxRehides && mon.work.bottom == 1392);  // then released, Explorer's value kept
        CHECK(c.Show(why) && mon.work.bottom == 1392 && broadcast == 0);  // released entry: nothing to restore
        int refits = 0; b.work.refitMaximized = [&](const RECT&, const RECT& from, const RECT& to) { ++refits; return from.bottom != to.bottom ? 1 : 0; };
        Controller c3(rec, b); c3.SetFreeWorkArea(true);
        CHECK(c3.Hide(why) && refits == 1); CHECK(c3.Show(why) && refits == 2);  // maximized windows refit on apply and restore
        CHECK(c.Hide(why) && mon.work.bottom == 1440);
        const int before = broadcast;
        c.SetFreeWorkArea(false); CHECK(mon.work.bottom == 1392 && broadcast == before + 1);  // switching off restores immediately
        CHECK(c.Show(why));
    }
    std::printf(failures ? "FAILED (%d)\n" : "PASS: hide/show/toggle (primary+secondary), guardian gate, external show respected, Explorer restart and monitor add re-hide, guardian loss restore, external close, corrupt/Pending preserved, hotkey validation + real conflict, settings persistence, status file, recording lease (suspend/ack/exclusive/release/abandoned), work area (3 monitors, auto-hide monitor, external change, unplug, partial failure, Pending identity, corrupt), 3-monitor hide/unplug/partial restore, rebuild rate limit\n", failures);
    return failures ? 1 : 0;
}
