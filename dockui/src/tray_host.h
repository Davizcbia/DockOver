#pragma once
// Notification-area (tray) icon host for the Menu Bar's Hidden Icons popup and promoted tray items.
//
// How it works (the established approach of alternative taskbars): Shell_NotifyIcon() in every app looks up
// FindWindow("Shell_TrayWnd") and sends it WM_COPYDATA. We own a hidden, top-most window of that class on a
// dedicated thread, so it is found first. Every message it receives is forwarded unchanged to Explorer's own
// taskbar and Explorer's answer is returned, so Explorer keeps working exactly as before (AppBars included);
// we only keep a copy of the icon list. When docky exits or crashes the window disappears and apps talk to
// Explorer directly again; nothing is persisted and no Explorer state is modified.
// At start "TaskbarCreated" is broadcast, which is the documented signal for apps to re-add their icons.
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace docky::tray {

// shell32 -> Shell_TrayWnd wire formats (fixed 32-bit handle fields on every architecture).
struct NotifyIconData32 {
    DWORD cbSize; DWORD hWnd; UINT uID; UINT uFlags; UINT uCallbackMessage; DWORD hIcon;
    WCHAR szTip[128]; DWORD dwState; DWORD dwStateMask; WCHAR szInfo[256];
    UINT uVersion; WCHAR szInfoTitle[64]; DWORD dwInfoFlags; GUID guidItem; DWORD hBalloonIcon;
};
struct TrayNotifyData { DWORD signature; DWORD message; NotifyIconData32 nid; };
struct IconIdentifier { DWORD magic; DWORD message; DWORD cbSize; DWORD padding; DWORD hWnd; UINT uID; GUID guidItem; };
inline constexpr DWORD kSignature = 0x34753423;

struct Icon {
    std::uint64_t serial = 0;    // unique per icon for this run
    HWND hwnd = nullptr; UINT uid = 0; GUID guid{}; bool hasGuid = false;
    UINT callback = 0, version = 0;
    std::wstring tip;            // full tooltip (may contain line breaks)
    std::vector<std::uint32_t> pixels; UINT w = 0, h = 0;  // premultiplied BGRA
    DWORD pid = 0; std::wstring exePath;
    std::wstring key;            // stable id for settings: "tray:<exe name>#<uid|guid>"
    bool hidden = false;         // NIS_HIDDEN
};

// Pure helpers (unit tested).
std::wstring KeyFor(const std::wstring& exePath, UINT uid, const GUID* guid);
std::wstring DisplayName(const Icon& icon);  // first tooltip line, else the exe name
struct Posted { UINT message; WPARAM w; LPARAM l; };
// Callback messages a click produces for an icon of the given NOTIFYICON_VERSION.
std::vector<Posted> ClickMessages(UINT callback, UINT version, UINT uid, bool right, POINT pt);
// Applies one NIM_* message to the list; returns true when the visible list changed. iconLoader turns an
// HICON into pixels (injected so tests need no real icons). Unknown identifiers on MODIFY are ignored.
using IconLoader = std::function<bool(DWORD hIcon, std::vector<std::uint32_t>& pixels, UINT& w, UINT& h)>;
using ProcessInfo = std::function<void(HWND hwnd, DWORD& pid, std::wstring& exePath)>;
bool Apply(std::vector<Icon>& icons, const TrayNotifyData& data, std::uint64_t& nextSerial,
           const IconLoader& loadIcon, const ProcessInfo& process, bool& accepted);

class Host {
public:
    // notify receives `message` (posted, coalesced) whenever the icon list changes.
    bool Start(HWND notify, UINT message);
    void Stop();
    bool Running() const;
    std::vector<Icon> Snapshot() const;
    // Forwards a left/right click to the icon's owner; pt is the screen point its menu should appear at.
    bool Click(std::uint64_t serial, bool right, POINT pt);
    // Where each icon is shown right now (screen px), answered to Shell_NotifyIconGetRect.
    void SetAnchors(const std::vector<std::pair<std::uint64_t, RECT>>& anchors);
};
Host& Get();

}  // namespace docky::tray
