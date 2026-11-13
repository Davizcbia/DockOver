// Real docky-taskbar-guardian.exe process tests with isolated records. Records only reference windows that are
// not live taskbars of the current shell (other PID / destroyed handle), so no real taskbar window is changed.
#include "taskbar_guardian.h"
#include "taskbar_window.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace liquid::desktop;
using namespace docky::taskbar;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static std::wstring SelfDir() { wchar_t p[MAX_PATH]{}; GetModuleFileNameW(nullptr, p, MAX_PATH); std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/") + 1); }
static std::wstring ReadAll(const std::wstring& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); std::string b = s.str(); return std::wstring(b.begin(), b.end()); }

struct Run {
    PROCESS_INFORMATION fakeManager{}, guardian{};
    HANDLE stop = nullptr, ready = nullptr;
    bool Start(const std::wstring& record, const std::wstring& log, bool managerAlreadyGone = false) {
        // A sleeping child stands in for the Manager; its process handle is what the guardian watches.
        std::wstring cmd = L"\"" + SelfDir() + L"DockyTaskbarGuardianTest.exe\" --sleep";
        STARTUPINFOW si{sizeof(si)};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &fakeManager)) return false;
        if (managerAlreadyGone) { TerminateProcess(fakeManager.hProcess, 9); WaitForSingleObject(fakeManager.hProcess, 3000); }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        stop = CreateEventW(&sa, TRUE, FALSE, nullptr); ready = CreateEventW(&sa, TRUE, FALSE, nullptr);
        HANDLE mgr = nullptr;
        DuplicateHandle(GetCurrentProcess(), fakeManager.hProcess, GetCurrentProcess(), &mgr, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0);
        std::wstring g = L"\"" + SelfDir() + L"docky-taskbar-guardian.exe\" --manager " + std::to_wstring(reinterpret_cast<ULONG_PTR>(mgr)) +
                         L" --stop " + std::to_wstring(reinterpret_cast<ULONG_PTR>(stop)) + L" --ready " + std::to_wstring(reinterpret_cast<ULONG_PTR>(ready)) +
                         L" --record \"" + record + L"\" --log \"" + log + L"\"";
        STARTUPINFOW gsi{sizeof(gsi)};
        const bool ok = CreateProcessW(nullptr, g.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &gsi, &guardian) != FALSE;
        CloseHandle(mgr);
        return ok;
    }
    bool WaitReady() { HANDLE w[]{ready, guardian.hProcess}; return WaitForMultipleObjects(2, w, FALSE, 5000) == WAIT_OBJECT_0; }
    DWORD Exit(DWORD ms = 8000) { if (WaitForSingleObject(guardian.hProcess, ms) != WAIT_OBJECT_0) { TerminateProcess(guardian.hProcess, 99); return 99; } DWORD c = 0; GetExitCodeProcess(guardian.hProcess, &c); return c; }
    ~Run() {
        if (fakeManager.hProcess) { TerminateProcess(fakeManager.hProcess, 0); CloseHandle(fakeManager.hProcess); CloseHandle(fakeManager.hThread); }
        if (guardian.hProcess) { if (WaitForSingleObject(guardian.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(guardian.hProcess, 98); CloseHandle(guardian.hProcess); CloseHandle(guardian.hThread); }
        if (stop) CloseHandle(stop); if (ready) CloseHandle(ready);
    }
};

// A Hidden record that the guardian can close without touching a window: the HWND belongs to a destroyed test
// window and the PID is not the shell's, so recovery classifies it as "gone".
static void WriteOwnedRecord(const std::wstring& path) {
    HWND w = CreateWindowExW(0, L"STATIC", L"x", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    const ULONG_PTR hwnd = reinterpret_cast<ULONG_PTR>(w); DestroyWindow(w);
    TaskbarWindowOperation op; op.phase = RecordPhase::Active;
    op.windows.push_back({{L"Shell_TrayWnd", hwnd, GetCurrentProcessId(), 12345, true}, true, WindowState::Hidden});
    std::wstring e; SaveWindowOperation(path, op, e);
}
static RecordPhase Phase(const std::wstring& path) { TaskbarWindowOperation op; std::wstring e; LoadWindowOperation(path, op, e); return op.phase; }

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--sleep") { Sleep(60000); return 0; }
    // The guardian refuses to recover while another Manager owns the gate; do not run next to a live docky.
    HANDLE gate = OpenMutexW(SYNCHRONIZE, FALSE, kManagerGate);
    if (gate) { CloseHandle(gate); std::puts("SKIP: a docky Manager is running; guardian tests need the Manager gate free"); return 0; }
    wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"docky-guardian-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const auto log = dir + L"\\guardian.log";
    {   // handshake + clean stop on a closed record
        const auto rec = dir + L"\\closed.txt"; { std::ofstream f(rec); f << "liquid-taskbar-windows 1\nphase closed\n"; }
        Run r; CHECK(r.Start(rec, log)); CHECK(r.WaitReady());
        SetEvent(r.stop); CHECK(r.Exit() == 0);
    }
    {   // Manager crash while "hidden": guardian recovers and closes the record
        const auto rec = dir + L"\\crash.txt"; WriteOwnedRecord(rec);
        Run r; CHECK(r.Start(rec, log)); CHECK(r.WaitReady());
        TerminateProcess(r.fakeManager.hProcess, 7);
        CHECK(r.Exit() == 0); CHECK(Phase(rec) == RecordPhase::Closed);
    }
    {   // "Restore now" while the Manager is alive, then clean stop
        const auto rec = dir + L"\\now.txt"; WriteOwnedRecord(rec);
        Run r; CHECK(r.Start(rec, log)); CHECK(r.WaitReady());
        HANDLE now = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRestoreNowEvent); CHECK(now != nullptr);
        if (now) { SetEvent(now); CloseHandle(now); }
        for (int i = 0; i < 50 && Phase(rec) != RecordPhase::Closed; ++i) Sleep(100);
        CHECK(Phase(rec) == RecordPhase::Closed);
        CHECK(WaitForSingleObject(r.guardian.hProcess, 0) == WAIT_TIMEOUT);  // still guarding
        SetEvent(r.stop); CHECK(r.Exit() == 0);
    }
    {   // Pending (ambiguous) record after a crash: refused and preserved byte for byte
        const auto rec = dir + L"\\pending.txt";
        TaskbarWindowOperation op; op.phase = RecordPhase::Active;
        op.windows.push_back({{L"Shell_TrayWnd", 101, GetCurrentProcessId(), 1, true}, false, WindowState::Pending});
        std::wstring e; SaveWindowOperation(rec, op, e); const auto before = ReadAll(rec);
        Run r; CHECK(r.Start(rec, log)); CHECK(r.WaitReady());
        TerminateProcess(r.fakeManager.hProcess, 7);
        CHECK(r.Exit() == 6); CHECK(ReadAll(rec) == before);
    }
    {   // corrupt record: refused and preserved
        const auto rec = dir + L"\\corrupt.txt"; { std::ofstream f(rec); f << "liquid-taskbar-windows 1\nphase active\nw broken\n"; }
        const auto before = ReadAll(rec);
        Run r; CHECK(r.Start(rec, log)); CHECK(r.WaitReady());
        TerminateProcess(r.fakeManager.hProcess, 7);
        CHECK(r.Exit() == 6); CHECK(ReadAll(rec) == before);
    }
    {   // Manager already gone: never signals ready
        const auto rec = dir + L"\\gone.txt"; { std::ofstream f(rec); f << "liquid-taskbar-windows 1\nphase closed\n"; }
        Run r; CHECK(r.Start(rec, log, true));
        CHECK(r.Exit() == 4); CHECK(WaitForSingleObject(r.ready, 0) == WAIT_TIMEOUT);
    }
    std::printf(failures ? "FAILED (%d)\n" : "PASS: guardian handshake, clean stop, crash recovery, restore-now, Pending/corrupt preserved, no ready after Manager exit\n", failures);
    return failures ? 1 : 0;
}
