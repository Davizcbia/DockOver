#pragma once
// Control Center V0.1: macOS-style panel of floating Liquid Glass modules opened from the Menu Bar.
// Runs on its own thread (window, D3D11/DComp/D2D, live WGC backdrop, message loop); system calls
// run on a separate MTA worker (system_controls). Unavailable capabilities are hidden or shown as
// links to Windows Settings, never as working toggles.
#include <windows.h>
#include <string>

namespace liquid::cc {

// anchor: screen rectangle (physical pixels) of the Menu Bar's Control Center icon.
// notify/notifyMessage: posted after the panel changed system state or closed (Menu Bar refresh).
void SetSettingsPath(const std::wstring& path);
void SetWorkshopRoot(const std::wstring& path);
void SetVisualCaptureTest(bool enabled);
void Toggle(const RECT& anchor, HWND notify, UINT notifyMessage);
// Reuses the Control Center audio worker/renderer for a compact sound panel.
void ToggleSound(const RECT& anchor, HWND notify, UINT notifyMessage);
// Wi-Fi detail uses the same state, rendering and controls as Control Center.
void ToggleWifi(const RECT& anchor, HWND notify, UINT notifyMessage);
// Updates an already-open panel after Menu Bar slot widths change.
void UpdateAnchor(const RECT& anchor);
bool IsOpen();
void Shutdown();
// Creates the shared D3D11 device on a background thread (it took ~165-205 ms per open), so the first
// Control Center opening does not wait for it. Safe to call more than once.
void Prewarm();

}  // namespace liquid::cc
