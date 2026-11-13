#pragma once
// docky extension SDK v1 - single header, no dependencies beyond <windows.h> and the C++17 standard library.
// Copy this file into your project. Protocol reference: docs/extensions-v01.md.
//
//   docky Menu Bar  : status items on the right side of the menu bar (icon and/or short title, optional menu).
//   docky Desktop   : desktop widgets rendered from a template (text, list, gauge, image).
//
// Both hosts are optional and independent: every call returns false when the host is not running, so an
// app can call these at start-up and again when it receives the registered "dockyHostStarted" broadcast.
//
// Minimal example:
//   docky_sdk::StatusItem item; item.id = "com.example.timer"; item.title = "25:00"; item.notify = myHwnd;
//   item.menu = {{"start", "Start"}, {"stop", "Stop"}};
//   docky_sdk::SetStatusItem(item);
//   ... in your WndProc:
//   case WM_COPYDATA: { docky_sdk::Event e; if (docky_sdk::ReadEvent(lParam, e)) { /* e.type, e.id, e.item */ return 1; } }
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace docky_sdk {

inline constexpr ULONG_PTR kCopyDataMagic = 0x444B5831;  // 'DKX1'
inline constexpr wchar_t kMenuBarHostClass[] = L"DockyMenuBarExtrasHostV1";
inline constexpr wchar_t kDesktopHostClass[] = L"DockyDesktopHostV1";
// Broadcast (RegisterWindowMessageW) by each host when it starts: re-send your items/widgets then.
inline UINT HostStartedMessage() { static const UINT m = RegisterWindowMessageW(L"dockyHostStarted"); return m; }

struct MenuItem {
    std::string id, title;           // UTF-8
    bool separator = false, enabled = true, checked = false;
};
struct StatusItem {
    std::string id;                  // unique, reverse-DNS recommended ("com.example.timer")
    std::string title;               // optional short text shown in the bar (keep it under ~12 characters)
    std::wstring iconPath;           // optional PNG/ICO/SVG/JPG; square artwork, at least 32 px
    bool templateIcon = true;        // true: monochrome template tinted to the bar's ink (recommended)
    std::string tooltip;
    std::vector<MenuItem> menu;      // empty: a click sends "statusItem.clicked"
    HWND notify = nullptr;           // receives events; the item is removed when this window is destroyed
};
struct Widget {
    std::string id, title;
    std::string size = "small";      // small 170x170, medium 364x170, large 364x376 (DIP)
    std::string templ = "text";      // text | list | gauge | image
    std::string dataJson = "{}";     // template data as a JSON object, see docs
    std::string accent;              // optional "#RRGGBB"
    HWND notify = nullptr;
};
struct Event { std::string type, id, item, button; };

namespace detail {
inline std::string Escape(const std::string& s) {
    std::string o; o.reserve(s.size() + 2); o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + '"';
}
inline std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline bool Send(const wchar_t* hostClass, HWND from, const std::string& json) {
    HWND host = FindWindowW(hostClass, nullptr);
    if (!host) return false;
    COPYDATASTRUCT cds{kCopyDataMagic, static_cast<DWORD>(json.size()), const_cast<char*>(json.data())};
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(host, WM_COPYDATA, reinterpret_cast<WPARAM>(from), reinterpret_cast<LPARAM>(&cds),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &r) != 0 && r == 1;
}
inline std::string Hwnd(HWND h) { return std::to_string(static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(h))); }
// Reads one string field of a flat event object. Events never nest, so this is sufficient for them.
inline std::string Field(const std::string& json, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k); if (at == std::string::npos) return {};
    at = json.find(':', at + k.size()); if (at == std::string::npos) return {};
    at = json.find('"', at); if (at == std::string::npos) return {};
    std::string out;
    for (size_t i = at + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { out += json[++i]; continue; }
        if (json[i] == '"') break;
        out += json[i];
    }
    return out;
}
}  // namespace detail

inline bool SetStatusItem(const StatusItem& s) {
    using namespace detail;
    std::string j = "{\"v\":1,\"type\":\"statusItem.set\",\"id\":" + Escape(s.id);
    if (!s.title.empty()) j += ",\"title\":" + Escape(s.title);
    if (!s.iconPath.empty()) j += ",\"icon\":" + Escape(Utf8(s.iconPath));
    j += std::string(",\"template\":") + (s.templateIcon ? "true" : "false");
    if (!s.tooltip.empty()) j += ",\"tooltip\":" + Escape(s.tooltip);
    if (s.notify) j += ",\"notifyHwnd\":" + Hwnd(s.notify);
    j += ",\"menu\":[";
    for (size_t i = 0; i < s.menu.size(); ++i) {
        const auto& m = s.menu[i];
        if (i) j += ',';
        if (m.separator) { j += "{\"separator\":true}"; continue; }
        j += "{\"id\":" + Escape(m.id) + ",\"title\":" + Escape(m.title) + ",\"enabled\":" + (m.enabled ? "true" : "false") +
             ",\"checked\":" + (m.checked ? "true" : "false") + "}";
    }
    j += "]}";
    return Send(kMenuBarHostClass, s.notify, j);
}
inline bool RemoveStatusItem(const std::string& id, HWND from = nullptr) {
    return detail::Send(kMenuBarHostClass, from, "{\"v\":1,\"type\":\"statusItem.remove\",\"id\":" + detail::Escape(id) + "}");
}
inline bool SetWidget(const Widget& w) {
    using namespace detail;
    std::string j = "{\"v\":1,\"type\":\"widget.set\",\"id\":" + Escape(w.id) + ",\"title\":" + Escape(w.title) +
                    ",\"size\":" + Escape(w.size) + ",\"template\":" + Escape(w.templ) + ",\"data\":" + (w.dataJson.empty() ? "{}" : w.dataJson);
    if (!w.accent.empty()) j += ",\"accent\":" + Escape(w.accent);
    if (w.notify) j += ",\"notifyHwnd\":" + Hwnd(w.notify);
    j += "}";
    return Send(kDesktopHostClass, w.notify, j);
}
inline bool RemoveWidget(const std::string& id, HWND from = nullptr) {
    return detail::Send(kDesktopHostClass, from, "{\"v\":1,\"type\":\"widget.remove\",\"id\":" + detail::Escape(id) + "}");
}
// Call from WM_COPYDATA. Returns true for docky events (return 1 from your WndProc then).
inline bool ReadEvent(LPARAM l, Event& e) {
    const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (!cds || cds->dwData != kCopyDataMagic || !cds->lpData || !cds->cbData) return false;
    const std::string j(static_cast<const char*>(cds->lpData), cds->cbData);
    e.type = detail::Field(j, "type"); e.id = detail::Field(j, "id");
    e.item = detail::Field(j, "item"); e.button = detail::Field(j, "button");
    return !e.type.empty();
}

}  // namespace docky_sdk
