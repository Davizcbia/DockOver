// docky-taskbar-guardian: production taskbar recovery guardian. Started only by LiquidDesktopManager with
// inherited handles: --manager <process handle> --stop <event> --ready <event> --record <path> [--log <path>].
#include "taskbar_guardian.h"
#include "liquid_build_id.h"
#include <windows.h>
#include <cstdlib>
#include <string>
static volatile const char marker[] = LIQUID_BUILD_MARKER;
static HANDLE ParseHandle(const wchar_t* s) {
    wchar_t* end = nullptr;
    const auto v = _wcstoui64(s, &end, 10);
    return (!end || *end || !v) ? nullptr : reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(v));
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // physical taskbar and work-area rects
    (void)marker[0];
    HANDLE manager = nullptr, stop = nullptr, ready = nullptr;
    std::wstring record, log = docky::taskbar::DefaultGuardianLog();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring a = argv[i];
        if (a == L"--manager") manager = ParseHandle(argv[i + 1]);
        else if (a == L"--stop") stop = ParseHandle(argv[i + 1]);
        else if (a == L"--ready") ready = ParseHandle(argv[i + 1]);
        else if (a == L"--record") record = argv[i + 1];
        else if (a == L"--log") log = argv[i + 1];
        else return 2;
    }
    if ((argc - 1) % 2) return 2;
    return docky::taskbar::RunGuardian(manager, stop, ready, record, log);
}
