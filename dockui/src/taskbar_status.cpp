#include "taskbar_status.h"
#include <shellapi.h>

namespace liquid::desktop {
TaskbarStatus QueryTaskbarStatus() noexcept {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    const UINT_PTR flags = SHAppBarMessage(ABM_GETSTATE, &data);
    TaskbarStatus result{};
    result.rawFlags = flags;
    result.primaryWindowFound = FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
    result.autoHideFlag = (flags & ABS_AUTOHIDE) != 0;
    result.alwaysOnTopFlag = (flags & ABS_ALWAYSONTOP) != 0;
    return result;
}
}
