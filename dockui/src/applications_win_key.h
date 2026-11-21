#pragma once

#include <windows.h>
#include <atomic>
#include <thread>

namespace dock::applications {
constexpr UINT OpenFromWinKey = WM_APP + 241;

// Keep only the two logo-key states and whether the current press is still solo.
struct WinTap {
    unsigned held = 0;
    bool solo = false;

    bool Update(DWORD vk, bool down, bool enabled, bool otherHeld) {
        unsigned bit = vk == VK_LWIN ? 1u : vk == VK_RWIN ? 2u : 0u;
        if (!enabled || !bit) solo = false;
        if (!bit) return false;
        if (down) {
            if (!(held & bit)) solo = enabled && !held && !otherHeld;
            held |= bit;
            return false;
        }
        const bool fire = enabled && solo && held == bit;
        held &= ~bit;
        solo = false;
        return fire;
    }
};

class WinKeyListener {
public:
    bool Start(HWND target) {
        target_ = target;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return false;
        worker_ = std::thread([this,ready] {
            MSG message{};
            PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
            id_ = GetCurrentThreadId();
            current_ = this;
            HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, Callback, GetModuleHandleW(nullptr), 0);
            installed_ = hook != nullptr;
            SetEvent(ready);
            if (hook) {
                while (GetMessageW(&message,nullptr,0,0)>0) DispatchMessageW(&message);
                UnhookWindowsHookEx(hook);
            }
            current_ = nullptr;
        });
        WaitForSingleObject(ready,INFINITE);
        CloseHandle(ready);
        return installed_;
    }
    void SetEnabled(bool enabled) { enabled_.store(enabled); }
    void Stop() {
        if (!worker_.joinable()) return;
        if (installed_) PostThreadMessageW(id_,WM_QUIT,0,0);
        worker_.join();
    }
    ~WinKeyListener() { Stop(); }
private:
    static constexpr ULONG_PTR tag = 0x444f574b;
    static inline thread_local WinKeyListener* current_ = nullptr;
    HWND target_ = nullptr;
    std::thread worker_;
    DWORD id_ = 0;
    bool installed_ = false;
    WinTap tap_;
    std::atomic<bool> enabled_{true};

    static bool AnotherKeyHeld(DWORD logo) {
        // On a fresh logo press only. Also covers a key held before the listener started.
        for (int key=1;key<255;++key)
            if (static_cast<DWORD>(key)!=logo && (GetAsyncKeyState(key)&0x8000)) return true;
        return false;
    }

    static LRESULT CALLBACK Callback(int code, WPARAM message, LPARAM data) {
        if (code==HC_ACTION && current_) {
            const auto& k=*reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (k.dwExtraInfo!=tag) {
                auto& self=*current_;
                const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
                const bool logo=k.vkCode==VK_LWIN || k.vkCode==VK_RWIN;
                const bool enabled=self.enabled_.load();
                const bool other=enabled && down && logo && !self.tap_.held && AnotherKeyHeld(k.vkCode);
                if (self.tap_.Update(k.vkCode,down,enabled,other)) {
                    // Windows has seen the original modifier down. Mask Start, then release
                    // the same modifier exactly once. Chords never enter this branch.
                    INPUT keys[3]{};
                    for (auto& input:keys) {input.type=INPUT_KEYBOARD;input.ki.dwExtraInfo=tag;}
                    keys[0].ki.wVk=keys[1].ki.wVk=0xe8; // unassigned menu-mask key
                    keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
                    keys[2].ki.wVk=static_cast<WORD>(k.vkCode);
                    keys[2].ki.wScan=static_cast<WORD>(k.scanCode);
                    keys[2].ki.dwFlags=KEYEVENTF_KEYUP | (k.flags&LLKHF_EXTENDED ? KEYEVENTF_EXTENDEDKEY : 0);
                    UINT sent=SendInput(3,keys,sizeof(INPUT));
                    if (sent==3) {PostMessageW(self.target_,OpenFromWinKey,0,0);return 1;}
                    if (sent==1) SendInput(1,&keys[1],sizeof(INPUT));
                    // On failure let the physical release through.
                }
            }
        }
        return CallNextHookEx(nullptr,code,message,data);
    }
};
}
