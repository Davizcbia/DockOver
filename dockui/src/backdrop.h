#pragma once
// Architecture boundary between the Dock UI and the capture / glass-material module.
//
// Rules (see .ai/architecture.md):
//  * The UI owns its own real-time animation clock (QPC + DXGI frame-latency waitable object).
//    A backdrop source must never drive, throttle or block UI frames.
//  * The UI pulls the most recent backdrop frame when it renders. If no new frame exists it reuses
//    the previous one (or the tinted fallback material); it never waits for capture.
//  * The capture module (P0.2 WGC, owned by Codex) produces frames on its own thread / FrameArrived
//    callback, shares textures on the UI's D3D11 device (or via a shared handle), and excludes the
//    Dock window from its own capture.
//  * Live capture and simulated sources implement this same boundary.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace dock {

struct BackdropFrame {
    ID3D11Texture2D* texture = nullptr;  // not owned; valid until the next Latest() call
    RECT screenRect{};                   // physical-pixel desktop rectangle covered by the texture
    uint64_t frameId = 0;                // increases only for genuinely new captured content
    double captureTimeSeconds = 0.0;     // QPC-based capture timestamp
};

class IBackdropSource {
public:
    virtual ~IBackdropSource() = default;
    // Called by the UI with the screen rectangle it is about to draw. Must be non-blocking.
    virtual bool Latest(const RECT& wantedScreenRect, BackdropFrame& out) = 0;
    // Window the source must exclude from capture (the Dock itself).
    virtual void SetExcludedWindow(HWND hwnd) = 0;
    virtual const wchar_t* Name() const = 0;
};

class NullBackdropSource final : public IBackdropSource {
public:
    bool Latest(const RECT&, BackdropFrame&) override { return false; }
    void SetExcludedWindow(HWND) override {}
    const wchar_t* Name() const override { return L"none (tinted fallback; capture pending P0.2/P0.3)"; }
};

}  // namespace dock
