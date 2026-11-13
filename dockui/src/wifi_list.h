#pragma once
// Wi-Fi network list shared by the Menu Bar and docky-wifi-list.exe.
//
// Why a helper: the Menu Bar runs with the local sparse-package identity (needed for borderless capture). Windows gates
// WLAN network lists for packaged processes behind the location capability, so WlanGetAvailableNetworkList returns no
// networks there (measured 2026-11-10: unpackaged probe 8 networks, docky 0), while Windows Settings lists them. A child
// exe without the msix manifest element runs without package identity (verified: docky-taskbar-guardian has none), so
// the Menu Bar asks docky-wifi-list.exe for the list instead of declaring location access.
// Wire format, one network per line (UTF-8, '\n'):  ssidHex \t profileHex \t signal \t secured \t connected \t bssType \t guid
#include <windows.h>
#include <combaseapi.h>
#include <wlanapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "system_controls.h"

namespace liquid::controls::wifilist {

inline std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
inline std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 15]; }
    return out;
}
inline bool Unhex(const std::string& h, std::string& out) {
    if (h.size() % 2) return false;
    out.clear();
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < h.size(); i += 2) {
        const int a = v(h[i]), b = v(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out += static_cast<char>(a * 16 + b);
    }
    return true;
}

// The interface docky uses: the connected one, else the first.
inline bool PickInterface(HANDLE h, GUID& guid) {
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    bool ok = false;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
        if (list->dwNumberOfItems) {
            guid = list->InterfaceInfo[0].InterfaceGuid;
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i)
                if (list->InterfaceInfo[i].isState == wlan_interface_state_connected) guid = list->InterfaceInfo[i].InterfaceGuid;
            ok = true;
        }
        WlanFreeMemory(list);
    }
    return ok;
}

inline std::wstring SsidText(const DOT11_SSID& s) {
    return Wide(std::string(reinterpret_cast<const char*>(s.ucSSID), s.uSSIDLength < DOT11_SSID_MAX_LENGTH ? s.uSSIDLength : DOT11_SSID_MAX_LENGTH));
}

// Returns the WLAN error code; out holds the merged, de-duplicated networks.
inline DWORD List(HANDLE h, const GUID& guid, std::vector<Network>& out) {
    out.clear();
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    const DWORD rc = WlanGetAvailableNetworkList(h, &guid, 0, nullptr, &list);
    if (rc != ERROR_SUCCESS || !list) return rc != ERROR_SUCCESS ? rc : ERROR_NOT_FOUND;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_AVAILABLE_NETWORK& a = list->Network[i];
        Network n;
        n.ssid = SsidText(a.dot11Ssid);
        if (n.ssid.empty()) continue;  // hidden networks are not listed
        n.signal = static_cast<int>(a.wlanSignalQuality);
        n.secured = a.bSecurityEnabled != FALSE;
        n.connected = (a.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
        if (a.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) n.profile = a.strProfileName;
        n.bssType = static_cast<int>(a.dot11BssType);
        n.adapter = guid;
        MergeNetwork(out, n);
    }
    WlanFreeMemory(list);
    return ERROR_SUCCESS;
}

inline std::string Encode(const std::vector<Network>& networks) {
    std::string out;
    for (const auto& n : networks) {
        wchar_t g[64]{};
        StringFromGUID2(n.adapter, g, 64);
        char line[160];
        std::snprintf(line, sizeof(line), "\t%d\t%d\t%d\t%d\t", n.signal, n.secured ? 1 : 0, n.connected ? 1 : 0, n.bssType);
        out += Hex(Utf8(n.ssid)) + "\t" + Hex(Utf8(n.profile)) + line + Utf8(g) + "\n";
    }
    return out;
}

inline bool Decode(const std::string& text, std::vector<Network>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t s = 0;
        for (;;) { const size_t t = line.find('\t', s); f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s)); if (t == std::string::npos) break; s = t + 1; }
        if (f.size() != 7) return false;
        std::string ssid, profile;
        if (!Unhex(f[0], ssid) || !Unhex(f[1], profile)) return false;
        Network n;
        n.ssid = Wide(ssid); n.profile = Wide(profile);
        n.signal = std::atoi(f[2].c_str()); n.secured = f[3] == "1"; n.connected = f[4] == "1"; n.bssType = std::atoi(f[5].c_str());
        if (FAILED(CLSIDFromString(Wide(f[6]).c_str(), &n.adapter))) return false;
        if (!n.ssid.empty()) out.push_back(std::move(n));
    }
    return true;
}

}  // namespace liquid::controls::wifilist
