#pragma once
// GPU Genie for minimize_dock (measured 2026-11-11 on Windows 11 26200):
//  * once a window is minimized, PrintWindow is black and a Windows.Graphics.Capture item for the window yields no
//    frames, but a DWM thumbnail of it still shows the last content;
//  * so the window is captured by placing its DWM thumbnail in an off-screen host window (no redirection bitmap, so
//    the rounded corners stay transparent) and capturing that host once with Windows.Graphics.Capture (15-30 ms);
//  * the Genie is then one continuous mesh of that texture (with mips), drawn on a render thread into a
//    composition swap chain paced by its frame-latency waitable object, so it runs at the display's rate.
// MyDockFinder's Dock_64.exe uses the same building blocks (Direct3D11CaptureFramePool, GraphicsCaptureItem,
// D3DCompile; no DwmGetDxSharedSurface, which fails for ordinary windows on this build anyway).
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace dock::minimize::gpu {

// Genie shape. t: 0 = the window at `from`, 1 = fitted into the tile `to`. u: source row (0 top .. 1 bottom).
// Rows nearer the Dock set off first; a row narrows toward the tile's width the further it has travelled.
struct Row { double y, left, right; };
inline double Smooth(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
inline Row GenieRow(const RECT& from, const RECT& to, double t, double u) {
    const double H = (std::max)(1L, from.bottom - from.top);
    const double travel = (std::max)(1.0, static_cast<double>(to.bottom - from.top));
    const double rt = std::clamp(t * 1.6 - (1.0 - u) * 0.6, 0.0, 1.0), k = Smooth(rt);
    const double y = (from.top + u * H) * (1.0 - k) + (to.top + u * (to.bottom - to.top)) * k;
    const double reach = std::clamp((y - from.top) / travel, 0.0, 1.0);
    const double a = Smooth(t / 0.35) * std::pow(Smooth(reach), 0.85);
    return Row{y, from.left + (to.left - from.left) * a, from.right + (to.right - from.right) * a};
}
// The window fitted into a Dock tile with a little padding (macOS shows the window, not the app icon).
inline RECT FitTile(const RECT& tile, SIZE source) {
    const double tw = (tile.right - tile.left) * 0.90, th = (tile.bottom - tile.top) * 0.90;
    const double sw = (std::max)(1L, source.cx), sh = (std::max)(1L, source.cy), k = (std::min)(tw / sw, th / sh);
    const double cx = (tile.left + tile.right) * 0.5, cy = (tile.top + tile.bottom) * 0.5, w = sw * k, h = sh * k;
    return RECT{static_cast<LONG>(std::lround(cx - w * 0.5)), static_cast<LONG>(std::lround(cy - h * 0.5)),
                static_cast<LONG>(std::lround(cx + w * 0.5)), static_cast<LONG>(std::lround(cy + h * 0.5))};
}

// Starts the render thread. When an animation ends (or could not start) it posts `message` to `notify` with
// wParam = MAKEWPARAM(direction + 1, ok ? 1 : 0) and lParam = the window.
bool Start(HWND notify, UINT message);
void Stop();
// Queues a Genie (+1 minimize, -1 restore). The window must already be minimized. `normal` is its restored rectangle.
void Animate(HWND window, const RECT& normal, int direction, double seconds);
// The tile's icon rectangle in screen pixels (updated every Dock frame; the tile moves while the Dock makes room).
void SetTile(HWND window, const RECT& tile);
// The window is gone or restored: cancel its animation and drop its cached texture.
void Forget(HWND window);
// Captures a still-visible window now. Many apps (WinUI/UWP such as Terminal, Notepad, Settings) drop their content
// as soon as they are minimized, so the DWM thumbnail taken after minimize starts is blank (white) for them; the next
// minimize Genie of this window uses this picture instead when it is at most a few seconds old.
void Snapshot(HWND window);

}  // namespace dock::minimize::gpu
