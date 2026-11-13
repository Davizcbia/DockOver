#include "taskbar_status.h"
#include "taskbar_snapshot.h"
#include <cstdio>
#include <cwchar>

namespace {
void WriteStatus(FILE* out, const liquid::desktop::TaskbarStatus& s) {
    std::fwprintf(out, L"taskbar: primary_window_found=%ls\n", s.primaryWindowFound ? L"yes" : L"no");
    std::fwprintf(out, L"taskbar: appbar_raw_flags=0x%llX\n", static_cast<unsigned long long>(s.rawFlags));
    std::fwprintf(out, L"taskbar: ABS_AUTOHIDE=%ls\n", s.autoHideFlag ? L"set" : L"not_set");
    std::fwprintf(out, L"taskbar: ABS_ALWAYSONTOP=%ls (not returned on Windows 7+)\n", s.alwaysOnTopFlag ? L"set" : L"not_set");
    std::fwprintf(out, L"taskbar: flags do not verify taskbar visibility on each monitor; primary lookup is best-effort\n");
    if (!s.primaryWindowFound) std::fwprintf(out, L"taskbar: Shell_TrayWnd not found; absence may indicate Explorer or session limitations\n");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && (std::wcscmp(argv[1], L"--snapshot") == 0 || std::wcscmp(argv[1], L"--read-snapshot") == 0)) {
        if (argc > 3) { std::fwprintf(stderr, L"Usage: --snapshot [path] | --read-snapshot [path]\n"); return 2; }
        const bool capture = std::wcscmp(argv[1], L"--snapshot") == 0;
        std::wstring error;
        const std::wstring path = argc == 3 ? argv[2] : liquid::desktop::DefaultTaskbarSnapshotPath(error);
        if (path.empty()) { std::fwprintf(stderr, L"snapshot: %ls\n", error.c_str()); return 1; }
        liquid::desktop::TaskbarStateSnapshot snapshot;
        if (capture) {
            snapshot = liquid::desktop::CaptureTaskbarSnapshot();
            if (!snapshot.valid) { std::fwprintf(stderr, L"snapshot query invalid: %ls (%lu)\n", snapshot.errorMessage.c_str(), snapshot.errorCode); return 1; }
            if (!liquid::desktop::SaveTaskbarSnapshotNew(path, snapshot, error)) { std::fwprintf(stderr, L"snapshot save: %ls\n", error.c_str()); return 1; }
        } else if (!liquid::desktop::LoadTaskbarSnapshot(path, snapshot, error)) {
            std::fwprintf(stderr, L"snapshot load: %ls\n", error.c_str()); return 1;
        }
        std::fwprintf(stdout, L"snapshot: path=%ls version=%u valid=%d raw=0x%llX autohide=%d primary=%d captured_filetime_utc=%llu phase=observed owner=none\n",
            path.c_str(), snapshot.formatVersion, snapshot.valid, static_cast<unsigned long long>(snapshot.rawFlags),
            snapshot.autoHide, snapshot.primaryWindowFound, static_cast<unsigned long long>(snapshot.capturedFileTimeUtc));
        std::fwprintf(stdout, L"snapshot: recovery not executed; Shell query has no distinct failure indicator and is not per-monitor visibility\n");
        return 0;
    }
    if (argc != 1 && !(argc == 3 && std::wcscmp(argv[1], L"--log") == 0)) {
        std::fwprintf(stderr, L"Usage: LiquidTaskbarStatus.exe [--log path]\n");
        return 2;
    }
    FILE* log = nullptr;
    if (argc == 3 && _wfopen_s(&log, argv[2], L"w") != 0) {
        std::fwprintf(stderr, L"Could not open diagnostic log.\n");
        return 1;
    }
    const auto status = liquid::desktop::QueryTaskbarStatus();
    WriteStatus(stdout, status);
    if (log) { WriteStatus(log, status); if (std::fclose(log) != 0) return 1; }
    return 0;
}
