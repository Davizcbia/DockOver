// docky-wifi-list.exe: prints the Wi-Fi network list (wifi_list.h wire format) for the packaged Menu Bar.
// No package identity (no msix manifest element), no UI, read-only except an optional scan request.
//   docky-wifi-list.exe [--scan]   --scan asks the adapter to rescan and waits (<= 4 s) for it to finish first.
#include "wifi_list.h"

#include <objbase.h>

namespace {
HANDLE g_scanDone = nullptr;
void WINAPI OnNotify(PWLAN_NOTIFICATION_DATA d, PVOID) {
    if (d && d->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM &&
        (d->NotificationCode == wlan_notification_acm_scan_complete || d->NotificationCode == wlan_notification_acm_scan_fail))
        SetEvent(g_scanDone);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool scan = false;
    for (int i = 1; i < argc; ++i) if (std::wstring(argv[i]) == L"--scan") scan = true;
    DWORD negotiated = 0;
    HANDLE h = nullptr;
    if (WlanOpenHandle(2, nullptr, &negotiated, &h) != ERROR_SUCCESS) return 2;
    GUID guid{};
    if (!liquid::controls::wifilist::PickInterface(h, guid)) { WlanCloseHandle(h, nullptr); return 3; }
    if (scan) {
        g_scanDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD previous = 0;
        WlanRegisterNotification(h, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, OnNotify, nullptr, nullptr, &previous);
        if (WlanScan(h, &guid, nullptr, nullptr, nullptr) == ERROR_SUCCESS) WaitForSingleObject(g_scanDone, 4000);
        WlanRegisterNotification(h, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, &previous);
    }
    std::vector<liquid::controls::Network> networks;
    const DWORD rc = liquid::controls::wifilist::List(h, guid, networks);
    WlanCloseHandle(h, nullptr);
    if (rc != ERROR_SUCCESS) return 4;
    const std::string text = liquid::controls::wifilist::Encode(networks);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    return 0;
}
