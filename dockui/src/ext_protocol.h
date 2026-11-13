#pragma once
// docky extension protocol v1 (internal side). Third parties use sdk/docky_extension.h, which mirrors the
// constants below without depending on docky sources; docs/extensions-v01.md is the reference.
//
// Transport: WM_COPYDATA, dwData = kCopyDataMagic, payload = one UTF-8 JSON object (no terminator needed).
// Hosts (each is an invisible top-level window found by class name, so FindWindowW works from any process):
//   Menu Bar (LiquidMenuBar.exe)  -> status items  ("statusItem.set" / "statusItem.remove")
//   docky Desktop (DockyDesktop.exe) -> widgets    ("widget.set" / "widget.remove" / "widget.gallery")
// Events go back the same way to the sender's "notifyHwnd" (statusItem.clicked, statusItem.menu, widget.clicked).
// The two apps are independent programs; neither needs the other. They only find each other through these windows.
#include <windows.h>
#include <string>

namespace docky::ext {
inline constexpr ULONG_PTR kCopyDataMagic = 0x444B5831;  // 'DKX1'
inline constexpr wchar_t kMenuBarHostClass[] = L"DockyMenuBarExtrasHostV1";
inline constexpr wchar_t kDesktopHostClass[] = L"DockyDesktopHostV1";
inline constexpr size_t kMaxPayload = 256 * 1024;

inline bool SendJson(HWND target, HWND from, const std::string& utf8, UINT timeoutMs = 500) {
    if (!target || !IsWindow(target) || utf8.empty() || utf8.size() > kMaxPayload) return false;
    COPYDATASTRUCT cds{kCopyDataMagic, static_cast<DWORD>(utf8.size()), const_cast<char*>(utf8.data())};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(from), reinterpret_cast<LPARAM>(&cds),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &result) != 0 && result == 1;
}
// Returns the payload of a protocol WM_COPYDATA, or false for any other WM_COPYDATA.
inline bool ReadJson(LPARAM l, std::string& out) {
    const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (!cds || cds->dwData != kCopyDataMagic || !cds->lpData || !cds->cbData || cds->cbData > kMaxPayload) return false;
    out.assign(static_cast<const char*>(cds->lpData), cds->cbData);
    return true;
}
// %LOCALAPPDATA%\docky\extensions : one folder per extension, each with an extension.json manifest.
// DOCKY_EXTENSIONS_ROOT overrides it (development and tests).
inline std::wstring ExtensionsRoot() {
    wchar_t over[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"DOCKY_EXTENSIONS_ROOT", over, MAX_PATH) && over[0]) return over;
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::wstring(local) + L"\\docky\\extensions";
}
inline HWND FindMenuBarHost() { return FindWindowW(kMenuBarHostClass, nullptr); }
inline HWND FindDesktopHost() { return FindWindowW(kDesktopHostClass, nullptr); }
}  // namespace docky::ext
