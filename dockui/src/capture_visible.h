#pragma once
// Screenshots that show the glass.
//
// Every glass window (Dock, menu bar, menus, panels, Desktop widgets, the minimize overlay) is excluded from capture
// (WDA_EXCLUDEFROMCAPTURE): the glass reads the screen behind it through desktop duplication or Windows Graphics
// Capture, and a window that is in that image would sample itself. The same flag kept every screenshot tool from seeing
// those windows.
//
// When a screenshot key is pressed (PrtScn with any modifiers, Win+Shift+S), the Dock sets the named manual-reset event
// below for a few seconds. Every docky process watches it (Watch): while it is set, the glass stops taking new desktop
// frames (Frozen) and the process's excluded windows are shown to capture; afterwards they are excluded again and the
// glass resumes 150 ms later, so no frame that contains a glass window reaches a backdrop. The glass stands still for
// those seconds, which a snipping overlay hides anyway (it shows a still image of the screen).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"

namespace docky::capture_visible {

inline constexpr wchar_t kEventName[] = L"Local\\DockOverCaptureVisibleV1";
inline constexpr ULONGLONG kVisibleMs = 3000;  // Snipping Tool takes its still image within about a second of the key

inline std::atomic<bool>& FrozenFlag() { static std::atomic<bool> f{false}; return f; }
// True while backdrops must not take new desktop frames (glass windows are, or were just, visible to capture).
inline bool Frozen() { return FrozenFlag().load(std::memory_order_acquire); }

// Starts this process's watcher once (cheap to call repeatedly).
inline void Watch() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, kEventName);
            if (!ev) return;
            const DWORD pid = GetCurrentProcessId();
            struct Ctx { DWORD pid; std::vector<HWND> shown; } ctx{pid, {}};
            while (WaitForSingleObject(ev, INFINITE) == WAIT_OBJECT_0) {
                FrozenFlag().store(true, std::memory_order_release);  // before any window becomes capturable
                ctx.shown.clear();
                EnumWindows([](HWND w, LPARAM l) -> BOOL {
                    auto* c = reinterpret_cast<Ctx*>(l);
                    DWORD owner = 0; GetWindowThreadProcessId(w, &owner);
                    DWORD affinity = WDA_NONE;
                    if (owner != c->pid || !GetWindowDisplayAffinity(w, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE) return TRUE;
                    if (SetWindowDisplayAffinity(w, WDA_NONE)) c->shown.push_back(w);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&ctx));
                DLOG(L"screenshot: %zu glass windows visible to capture, glass frozen", ctx.shown.size());
                while (WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) Sleep(50);
                for (HWND w : ctx.shown) if (IsWindow(w)) SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE);
                Sleep(150);  // frames composed before the exclusion took effect are dropped
                FrozenFlag().store(false, std::memory_order_release);
                DLOG(L"screenshot: glass windows excluded from capture again");
            }
        }).detach();
    });
}

// The Dock: a low-level keyboard hook that only looks for the screenshot keys (it never blocks or changes a key).
class Trigger {
public:
    static void Install() {
        auto& t = Get();
        if (t.hook_) return;
        if (!t.event_) t.event_ = CreateEventW(nullptr, TRUE, FALSE, kEventName);
        if (!t.kick_) {
            t.kick_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            std::thread([] { Get().ResetLoop(); }).detach();
        }
        t.hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(nullptr), 0);
    }
    static void Remove() { auto& t = Get(); if (t.hook_) { UnhookWindowsHookEx(t.hook_); t.hook_ = nullptr; } }
    static void Fire() {
        auto& t = Get();
        if (!t.event_ || !t.kick_) return;
        t.until_.store(GetTickCount64() + kVisibleMs);
        SetEvent(t.event_); SetEvent(t.kick_);
    }

private:
    static Trigger& Get() { static Trigger t; return t; }
    static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    static LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l) {
        if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
            const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
            if (k->vkCode == VK_SNAPSHOT || (k->vkCode == 'S' && (Down(VK_LWIN) || Down(VK_RWIN)) && Down(VK_SHIFT))) Fire();
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    void ResetLoop() {
        while (WaitForSingleObject(kick_, INFINITE) == WAIT_OBJECT_0) {
            for (ULONGLONG now = GetTickCount64(); now < until_.load(); now = GetTickCount64())
                Sleep(static_cast<DWORD>((std::min)(until_.load() - now, 100ULL)));
            ResetEvent(event_);
        }
    }
    HHOOK hook_ = nullptr;
    HANDLE event_ = nullptr, kick_ = nullptr;
    std::atomic<ULONGLONG> until_{0};
};

}  // namespace docky::capture_visible
