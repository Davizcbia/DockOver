// Simulated window-level taskbar hide/restore with the persistent ownership record.
// Uses fake window ops only: never changes any real window. Real capture is read-only.
#include <windows.h>

#include <cstdio>
#include <map>
#include <string>

#include "taskbar_window.h"

using namespace liquid::desktop;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

struct World {
    std::map<ULONG_PTR, TaskbarWindowRecord> windows;
    std::map<ULONG_PTR, bool> failSet;
    int calls = 0;
    WindowOps Ops() {
        WindowOps o;
        o.observe = [this](const TaskbarWindowRecord& r, TaskbarWindowRecord& now) {
            auto it = windows.find(r.hwnd);
            if (it == windows.end()) return false;
            now = it->second;
            return true;
        };
        o.setVisible = [this](ULONG_PTR h, bool show) {
            ++calls;
            if (failSet[h] || !windows.count(h)) return false;
            windows[h].visible = show;
            return true;
        };
        return o;
    }
};
static std::vector<TaskbarWindowRecord> All(const World& w) {
    std::vector<TaskbarWindowRecord> v;
    for (auto& [h, r] : w.windows) v.push_back(r);
    return v;
}
static World Two() {
    World w;
    w.windows[0x100] = {L"Shell_TrayWnd", 0x100, 42, 1000, true};
    w.windows[0x200] = {L"Shell_SecondaryTrayWnd", 0x200, 42, 1000, true};
    return w;
}
static RecordPhase Phase(const std::wstring& p) { TaskbarWindowOperation op; std::wstring e; LoadWindowOperation(p, op, e); return op.phase; }

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"liquid-taskbar-window-test.txt";
    std::wstring log;
    auto reset = [&] { DeleteFileW(path.c_str()); DeleteFileW((path + L".tmp").c_str()); log.clear(); };

    {  // 1. Normal hide + restore (primary + secondary), idempotent restore, record closed
        reset(); World w = Two();
        CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 2);
        CHECK(!w.windows[0x100].visible && !w.windows[0x200].visible && Phase(path) == RecordPhase::Active);
        CHECK(RestoreTaskbarWindows(path, w.Ops(), log));
        CHECK(w.windows[0x100].visible && w.windows[0x200].visible && Phase(path) == RecordPhase::Closed);
        const int calls = w.calls;
        CHECK(RestoreTaskbarWindows(path, w.Ops(), log) && w.calls == calls);  // idempotent
        CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    {  // 2. Manager crash: record persisted Active; next start restores from disk
        reset(); World w = Two();
        HideTaskbarWindows(path, All(w), w.Ops(), log);
        World restarted = w;  // same Explorer, fresh Manager reading the file
        CHECK(RestoreTaskbarWindows(path, restarted.Ops(), log) && restarted.windows[0x100].visible);
    }
    {  // 3. Explorer restarted: same handle value, new process -> skip, no ShowWindow
        reset(); World w = Two();
        HideTaskbarWindows(path, All(w), w.Ops(), log);
        for (auto& [h, r] : w.windows) { r.processStart = 2000; r.visible = true; }
        const int calls = w.calls;
        CHECK(RestoreTaskbarWindows(path, w.Ops(), log) && w.calls == calls && Phase(path) == RecordPhase::Closed);
        reset(); World g = Two();
        HideTaskbarWindows(path, All(g), g.Ops(), log);
        g.windows.clear();  // handles invalid
        CHECK(RestoreTaskbarWindows(path, g.Ops(), log) && Phase(path) == RecordPhase::Closed);
    }
    {  // 4. HWND reused by another app -> Conflict kept, no action, further hide refused
        reset(); World w = Two();
        HideTaskbarWindows(path, All(w), w.Ops(), log);
        w.windows[0x100] = {L"Notepad", 0x100, 7, 5, false};
        CHECK(!RestoreTaskbarWindows(path, w.Ops(), log) && Phase(path) == RecordPhase::Conflict);
        CHECK(!w.windows[0x100].visible);  // never touched
        const int calls = w.calls;
        CHECK(!RestoreTaskbarWindows(path, w.Ops(), log) && w.calls == calls && Phase(path) == RecordPhase::Conflict);
        CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 0 && Phase(path) == RecordPhase::Conflict);
    }
    {  // 5. Multi-monitor partial failure: secondary not hidden -> only primary restored
        reset(); World w = Two(); w.failSet[0x200] = true;
        CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 1);
        w.failSet[0x200] = false;
        const int before = w.calls;
        CHECK(RestoreTaskbarWindows(path, w.Ops(), log) && w.calls == before + 1 && w.windows[0x100].visible);
    }
    {  // 6. External program re-showed the taskbar -> left alone
        reset(); World w = Two();
        HideTaskbarWindows(path, All(w), w.Ops(), log);
        w.windows[0x100].visible = true;
        const int calls = w.calls;
        CHECK(RestoreTaskbarWindows(path, w.Ops(), log) && w.calls == calls + 1);  // only the secondary
    }
    {  // 7. Already-hidden taskbar is never taken over
        reset(); World w = Two(); w.windows[0x200].visible = false;
        CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 1);
        RestoreTaskbarWindows(path, w.Ops(), log);
        CHECK(!w.windows[0x200].visible);
    }
    {  // 8. Old / corrupt record: refused, file kept byte-for-byte
        reset();
        FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"wb"); std::fputs("phase=2 legacy\n", f); std::fclose(f);
        World w = Two();
        CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 0 && w.calls == 0);
        CHECK(!RestoreTaskbarWindows(path, w.Ops(), log));
        f = nullptr; _wfopen_s(&f, path.c_str(), L"rb"); char b[64]{}; std::fread(b, 1, 63, f); std::fclose(f);
        CHECK(std::string(b) == "phase=2 legacy\n");
    }
    {  // 9. A captured HWND is reused or externally hidden before hide: never touch either window.
        reset(); World w = Two(); auto captured = All(w);
        w.windows[0x100].pid = 77; w.windows[0x100].processStart = 2000;
        w.windows[0x200].visible = false;
        CHECK(HideTaskbarWindows(path, captured, w.Ops(), log) == 0);
        CHECK(w.calls == 0 && w.windows[0x100].visible && !w.windows[0x200].visible);
        CHECK(Phase(path) == RecordPhase::Closed);
    }
    {  // 10. A failed show remains Conflict, rather than silently declaring recovery complete.
        reset(); World w = Two(); CHECK(HideTaskbarWindows(path, All(w), w.Ops(), log) == 2);
        w.failSet[0x100] = true;
        CHECK(!RestoreTaskbarWindows(path, w.Ops(), log));
        CHECK(!w.windows[0x100].visible && w.windows[0x200].visible);
        CHECK(Phase(path) == RecordPhase::Conflict);
    }
    {  // 11. Pending is ambiguous even if a window is hidden; do not change it.
        reset(); World w = Two(); TaskbarWindowOperation pending; pending.phase=RecordPhase::Active;
        for(const auto& record:All(w))pending.windows.push_back({record,false,WindowState::Pending});
        std::wstring error; CHECK(SaveWindowOperation(path,pending,error)); w.windows[0x100].visible=false;
        CHECK(!RestoreTaskbarWindows(path,w.Ops(),log));
        CHECK(!w.windows[0x100].visible && w.windows[0x200].visible && w.calls==0);
        CHECK(Phase(path)==RecordPhase::Conflict);
    }
    {  // 12. Duplicate start refuses an Active record without changing owned windows.
        reset(); World w = Two(); CHECK(HideTaskbarWindows(path,All(w),w.Ops(),log)==2);
        const int before=w.calls; CHECK(HideTaskbarWindows(path,All(w),w.Ops(),log)==0);
        CHECK(w.calls==before && Phase(path)==RecordPhase::Active);
        CHECK(RestoreTaskbarWindows(path,w.Ops(),log));
    }
    {  // 13. A failed write-ahead must make no call to hide any window.
        reset(); World w=Two(); const std::wstring invalid=path+L".missing\\record";
        CHECK(HideTaskbarWindows(invalid,All(w),w.Ops(),log)==0 && w.calls==0);
        CHECK(w.windows[0x100].visible && w.windows[0x200].visible);
    }
    {  // 14. Never overwrite or remove an existing recovery temp file.
        reset(); World w=Two(); const auto temp=path+L".tmp";
        FILE* f=nullptr; _wfopen_s(&f,temp.c_str(),L"wb"); CHECK(f != nullptr);
        if (f) { std::fputs("sentinel",f); std::fclose(f); }
        CHECK(HideTaskbarWindows(path,All(w),w.Ops(),log)==0 && w.calls==0);
        f=nullptr; _wfopen_s(&f,temp.c_str(),L"rb"); char data[16]{};
        CHECK(f != nullptr); if(f){std::fread(data,1,15,f);std::fclose(f);}
        CHECK(std::string(data)=="sentinel");
    }
    {  // 15. Real recovery rejects ambiguous, conflicting and non-shell records.
        TaskbarWindowOperation op;op.phase=RecordPhase::Active;auto original=Two().windows[0x100];
        op.windows.push_back({original,true,WindowState::Hidden});CHECK(IsConservativeRecoveryRecord(op));
        op.windows[0].state=WindowState::Pending;CHECK(!IsConservativeRecoveryRecord(op));
        op.windows[0].state=WindowState::Conflict;CHECK(!IsConservativeRecoveryRecord(op));
        op.windows[0].state=WindowState::Hidden;op.windows[0].original.className=L"OtherWindow";CHECK(!IsConservativeRecoveryRecord(op));
        op.windows.clear();CHECK(!IsConservativeRecoveryRecord(op));
    }
    {  // 16. Invalid encoded booleans are not accepted as a recovery record.
        reset();FILE* f=nullptr;_wfopen_s(&f,path.c_str(),L"wb");CHECK(f!=nullptr);
        if(f){std::fputs("liquid-taskbar-windows 1\nphase active\nw Shell_TrayWnd 1 1 1 2 1 1\n",f);std::fclose(f);}
        TaskbarWindowOperation op;std::wstring error;CHECK(!LoadWindowOperation(path,op,error));
    }
    reset();
    const auto now = CaptureTaskbarWindows();  // read-only
    std::printf("capture: %zu taskbar window(s)\n", now.size());
    for (const auto& w : now) std::printf("  %ls visible=%d plan=%d\n", w.className.c_str(), w.visible, static_cast<int>(ObserveAndPlanRestore(w)));
    std::printf(g_fail ? "taskbar window: %d FAILED\n" : "taskbar window: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
