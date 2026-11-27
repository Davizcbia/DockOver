#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <cwchar>
#include <shellapi.h>
#include <shlobj.h>
#include <initializer_list>

namespace docky::fullscreen {

inline constexpr wchar_t kMenuBarClass[] = L"LiquidDesktopMenuBarV01";
inline constexpr wchar_t kButtonWindowProperty[] = L"DockOver.ButtonFullscreenWindow";
inline constexpr UINT kDockChanged = WM_APP + 79;

inline bool OnMonitor(HWND w, HMONITOR monitor) {
    if (!w || !IsWindowVisible(w) || IsIconic(w) || MonitorFromWindow(w, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}

inline bool ShellOrDock(HWND w, DWORD shellPid) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == shellPid || pid == GetCurrentProcessId()) return true;
    wchar_t cls[64]{};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    for (const auto name : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                            L"XamlWindow", L"MultitaskingViewFrame", L"LiquidDockV01", kMenuBarClass,
                            L"LiquidLaunchpad", L"LiquidControlCenter", L"LiquidUnifiedPopup98d2"})
        if (std::wcscmp(cls, name) == 0) return true;
    return false;
}

inline bool CoversMonitor(HWND w, const RECT& screen) {
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) ||
        IsRectEmpty(&bounds)) {
        if (!GetWindowRect(w, &bounds)) return false;
    }
    // Win11 DWM visible bounds can be inset by one or two physical pixels.
    constexpr LONG tolerance = 2;
    return bounds.left <= screen.left + tolerance && bounds.top <= screen.top + tolerance &&
        bounds.right >= screen.right - tolerance && bounds.bottom >= screen.bottom - tolerance;
}

// A game can present a smaller framebuffer centred by the driver (pillarbox or letterbox).
inline bool ScaledGame(HWND w, const RECT& screen) {
    const auto style = GetWindowLongPtrW(w, GWL_STYLE);
    if ((style & (WS_CAPTION | WS_THICKFRAME)) || !(style & WS_POPUP)) return false;
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    QUERY_USER_NOTIFICATION_STATE state{};
    if (w == GetForegroundWindow() && SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN) return true;
    wchar_t cls[64]{};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"GLFW30") && wcscmp(cls, L"LWJGL")) return false;
    RECT r{};
    if (!GetWindowRect(w, &r)) return false;
    const bool anchored = abs(r.left-screen.left)<=2 && abs(r.top-screen.top)<=2;
    const bool topmost = (GetWindowLongPtrW(w,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
    if (anchored && topmost && r.right>r.left && r.bottom>r.top &&
        r.right<=screen.right+2 && r.bottom<=screen.bottom+2) return true;
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    DEVMODEW current{}; current.dmSize = sizeof(current);
    if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONULL), &info) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &current) &&
        r.right-r.left == static_cast<LONG>(current.dmPelsWidth) &&
        r.bottom-r.top == static_cast<LONG>(current.dmPelsHeight) &&
        abs(r.left-screen.left)<=2 && abs(r.top-screen.top)<=2) return true;
    const bool centred = abs((r.left-screen.left)-(screen.right-r.right)) <= 4 &&
                         abs((r.top-screen.top)-(screen.bottom-r.bottom)) <= 4;
    const bool spansAxis = abs(r.right-r.left-(screen.right-screen.left)) <= 4 ||
                           abs(r.bottom-r.top-(screen.bottom-screen.top)) <= 4;
    return centred && spansAxis;
}

inline HWND Find(HMONITOR monitor, HWND previous, HWND buttonWindow = nullptr) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return nullptr;
    DWORD shellPid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    const HWND foreground = GetForegroundWindow();
    const auto eligible = [&](HWND w) {
        return OnMonitor(w, monitor) && !ShellOrDock(w, shellPid) &&
            !(GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT));
    };
    const auto covers = [&](HWND w) {
        if (IsZoomed(w)) {
            if (w == buttonWindow) return true;
            if ((GetWindowLongPtrW(w, GWL_STYLE) & WS_CAPTION) == WS_CAPTION &&
                EqualRect(&info.rcMonitor, &info.rcWork)) return false;
        }
        return CoversMonitor(w, info.rcMonitor);
    };
    const bool frontApp = eligible(foreground);
    if (frontApp && (covers(foreground) || ScaledGame(foreground, info.rcMonitor))) return foreground;
    if (!frontApp && eligible(previous) && covers(previous)) return previous;
    // The foreground may belong to another monitor or a system overlay. Check the front app on this output.
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!eligible(w)) continue;
        const auto ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) continue;
        if (covers(w) || ((ex & WS_EX_TOPMOST) && ScaledGame(w,info.rcMonitor))) return w;
        if (!(ex & WS_EX_TOPMOST)) break;
    }
    return nullptr;
}

inline bool GameOwnsPointer() {
    CURSORINFO info{sizeof(info)};
    if (GetCursorInfo(&info) && info.flags == 0) return true;
    RECT clip{};
    if (!GetClipCursor(&clip)) return false;
    const LONG x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return clip.left > x || clip.top > y || clip.right < x + GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
        clip.bottom < y + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

}  // namespace docky::fullscreen
