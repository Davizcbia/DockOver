#pragma once
#include <windows.h>

namespace liquid::popup {
// Observe clicks without taking activation from the app that the user clicked.
class DismissWatch {
public:
    static constexpr UINT ClickMessage = WM_APP + 249;
    static constexpr UINT FocusMessage = WM_APP + 250;

    ~DismissWatch() { Stop(); }
    void Start(HWND window) {
        Stop();
        active_ = this;
        window_ = window;
        acceptedForeground_ = GetForegroundWindow();
        mouse_ = SetWindowsHookExW(WH_MOUSE_LL, Mouse, GetModuleHandleW(nullptr), 0);
        foreground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, Foreground, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
    void Stop() {
        if (mouse_) UnhookWindowsHookEx(mouse_);
        if (foreground_) UnhookWinEvent(foreground_);
        if (active_ == this) active_ = nullptr;
        mouse_ = nullptr;
        foreground_ = nullptr;
        window_ = nullptr;
    }
    bool LostFocus() {
        if (!window_) return false;
        const HWND foreground = GetForegroundWindow();
        if (foreground == window_) acceptedForeground_ = window_;
        return foreground && foreground != acceptedForeground_;
    }
    bool HasMouseHook() const { return mouse_ != nullptr; }

private:
    static LRESULT CALLBACK Mouse(int code, WPARAM event, LPARAM data) {
        if (code == HC_ACTION && active_ &&
            (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN ||
             event == WM_MBUTTONDOWN || event == WM_XBUTTONDOWN)) {
            const auto point = reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt;
            PostMessageW(active_->window_, ClickMessage, static_cast<WPARAM>(point.x), point.y);
        }
        return CallNextHookEx(nullptr, code, event, data);
    }
    static void CALLBACK Foreground(HWINEVENTHOOK hook, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
        if (active_ && active_->foreground_ == hook) PostMessageW(active_->window_, FocusMessage, 0, 0);
    }

    inline static thread_local DismissWatch* active_ = nullptr;
    HWND window_ = nullptr;
    HWND acceptedForeground_ = nullptr;
    HHOOK mouse_ = nullptr;
    HWINEVENTHOOK foreground_ = nullptr;
};
}
