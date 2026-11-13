#pragma once
// Launchpad V0.1: full-screen application grid opened from the Dock. Runs on its own thread with its
// own window, D3D11/D2D/DirectComposition swap chain and message loop, so it never blocks the Dock's
// render loop. Uses the Application Registry (AppsFolder discovery), the Dock's icon loader and
// launcher (shell_items), the shared Liquid Glass material and parameters, and json_lite persistence.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dock::launchpad {

// Starts application discovery and icon loading in the background (call once at Dock start-up).
void Prewarm();
// Opens Launchpad on the Dock's monitor, or closes it when it is open. layoutPath: launchpad.json.
void Toggle(HWND dock, const std::wstring& layoutPath, const std::wstring& settingsPath = {});
// Posted to the Dock window: wParam 1 = Launchpad now covers the screen, 0 = closed.
constexpr UINT kMsgLaunchpadCovers = WM_APP + 232;
bool IsOpen();
// Closes Launchpad and stops background threads (Dock exit).
void Shutdown();
// Right-click on an application in Launchpad: posted to the Dock window, which shows the app menu (same popup
// as the Dock's) on its own thread. lParam = new AppMenuRequest (the Dock takes ownership and deletes it).
constexpr UINT kMsgLaunchpadAppMenu = WM_APP + 233;
struct AppMenuRequest {
    std::string appId;    // Launchpad id ("app:...")
    std::wstring target;  // what Launchpad launches (shell:AppsFolder\... or a shortcut path)
    std::wstring name;    // display name
    POINT screen{};       // physical screen pixels
};
// Thread-safe commands the Dock sends back to an open Launchpad (no-ops when it is closed).
void OpenApp(const std::string& appId);  // launches the app and closes Launchpad
void Dismiss();                          // closes Launchpad (before a dialog or Explorer opens)
void MenuClosed();                       // the app menu closed: Launchpad takes clicks again

// Custom icons (the Dock's custom-icons.json): Launchpad shows the same image overrides and edits as the Dock.
void SetCustomIconPath(const std::wstring& path);
// Reloads the custom icon settings and drops cached Launchpad icons of this Dock stable id.
void InvalidateIcon(const std::string& stableId);

// Generated Dock icon (premultiplied BGRA, size x size).
std::vector<std::uint32_t> MakeDockIcon(unsigned size);

}  // namespace dock::launchpad
