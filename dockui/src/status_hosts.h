#pragma once
// Hosts for the notification-area icons (tray_host) and third-party status items (extension protocol) when the Dock's
// status group stands in for the menu bar. Only one process may host them: whoever holds kHostsMutex. The Menu Bar
// takes it while it runs; the Dock takes it only while the menu bar is turned off (Settings > Menu Bar > Show menu
// bar) and gives it back as soon as the menu bar is turned on again. Both use the same tray window and the same
// extension host window class, so apps and extensions need no changes.
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "menubar_extras.h"
#include "tray_host.h"

namespace docky::statushost {

inline constexpr wchar_t kHostsMutex[] = L"Local\\DockOverStatusHostsV1";

// Called about once a second from the Dock: `wanted` true while the menu bar is off. Starts the hosts once the mutex
// is free (the menu bar may still be closing), stops them and releases it when no longer wanted. `trayMessage` is
// posted to `owner` when the tray icon list changes.
void Maintain(HWND owner, UINT trayMessage, bool wanted);
bool Active();
// The extension status items currently registered (pushed by apps or from manifests); empty while not active.
const std::vector<extras::StatusItem>& Items();
// Hidden Icons: every tray icon and extension status item as a grid of icons above `bottomLeft` (the hovered one's
// name below). A left click clicks the tray icon or opens the extension item; a right click offers the app's own menu
// and "Keep in Dock", which calls `keep` with the item's status cell id (the tray key "tray:<exe>#<id>" or
// "ext:<id>"). Items whose cell id is in `kept` carry a dot. With nothing to show it is a one-line note instead.
using KeepToggle = std::function<void(const std::wstring& cellId)>;
void ShowHiddenIcons(HWND owner, POINT bottomLeft, const std::vector<std::wstring>& kept = {}, KeepToggle keep = {});
// Tray icons for cells kept in the Dock: the hosted icons (empty while the menu bar hosts them), and a click forwarded to
// the icon whose key is `key` (`anchor`, screen px, is where apps that ask see their icon). False when it is not there.
std::vector<tray::Icon> TrayIcons();
bool ClickTray(const std::wstring& key, bool right, const RECT& anchor);
// An extension status item's own cell: its menu (or click event / program) above `bottomLeft`.
void ActivateExtension(HWND owner, const std::wstring& id, POINT bottomLeft);
void Shutdown();

}  // namespace docky::statushost
