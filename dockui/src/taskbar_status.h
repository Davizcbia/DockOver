#pragma once
#include <windows.h>

namespace liquid::desktop {
// Shell appbar flags are global observations, not per-monitor visibility.
struct TaskbarStatus {
    UINT_PTR rawFlags = 0;
    bool primaryWindowFound = false; // Best-effort Shell_TrayWnd lookup.
    bool autoHideFlag = false;
    bool alwaysOnTopFlag = false;    // Not returned on Windows 7 and later.
};

// Read-only: no taskbar state, shell process or user settings are changed.
TaskbarStatus QueryTaskbarStatus() noexcept;
}
