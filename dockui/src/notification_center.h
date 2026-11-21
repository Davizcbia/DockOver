#pragma once
// Notification Center V0.1 (Menu Bar): the macOS-style column of Liquid Glass notification cards that opens from the
// menu bar clock, and the banners that slide in at the top right when a new notification arrives.
//
// Notifications come from Windows (UserNotificationListener), read on a background MTA thread. Capture is an opt-in
// setting (menuBar.captureNotifications). Windows grants the listener only after the user allows notification access;
// builds without package identity may be refused, and the panel then says so instead of showing an empty list.
// Everything else (windows, D3D/D2D/DComp, live WGC backdrop, animation) runs on one module UI thread.
#include <windows.h>
#include <string>

namespace docky::notify {

// Starts the module thread once (idempotent). `settingsPath`: the shared settings file (theme).
void Start(const std::wstring& settingsPath);
// Settings > Menu Bar > Capture notifications. Off: no reading, no banners; the panel explains how to turn it on.
void SetCapture(bool enabled);
// Settings > Menu Bar > Hide Windows notification banners: while capture works (on, access granted), Windows' own popup
// is moved off screen so only DockOver's banner appears. Notifications still reach Windows' notification list.
void SetHideSystemBanners(bool hide);
// Opens or closes the Notification Center. `anchor`: the clock's screen rectangle (physical px); clicks inside it
// do not count as "outside" (the menu bar toggles instead). `notifyMessage` is posted to `notify` with wParam 1
// when the panel opened and 0 when it closed (the menu bar redraws the clock pill).
void ToggleCenter(const RECT& anchor, HWND notify, UINT notifyMessage);
void CloseCenter();
bool IsCenterOpen();
// Development: shows a sample banner (and adds it to the list) without Windows notifications.
void DebugBanner(const std::wstring& app, const std::wstring& title, const std::wstring& body);
void Shutdown();

}  // namespace docky::notify
