#pragma once
// GPU renderer for the Downloads Fan (V0.9): D3D11 swap chain on DirectComposition, real Liquid Glass
// name capsules (LiquidGlassMaterial over the live WGC desktop backdrop), D2D for icons and text.
// Only the Dock links it (it registers a factory); test executables keep the GDI+ layered fallback.
#include <windows.h>
#include <d2d1_1.h>

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dock::downloads {

class IFanRenderer {
public:
    virtual ~IFanRenderer() = default;
    // hwnd must be a WS_EX_NOREDIRECTIONBITMAP popup. False = unavailable (caller falls back).
    virtual bool Init(HWND hwnd, UINT width, UINT height, float dpiScale) = 0;
    virtual bool BeginFrame() = 0;                       // clears to transparent
    // Liquid Glass capsule (live backdrop); smoked opaque-ish fallback when glass/capture is unavailable.
    // angleDeg rotates the capsule about its centre (clockwise on screen), following the Fan arc.
    // key identifies the capsule across frames (adaptive text contrast); INT_MIN = not tracked.
    virtual void Capsule(const D2D1_RECT_F& dip, float opacity, bool hover, float angleDeg = 0, int key = INT_MIN) = 0;
    virtual void BeginOverlay() = 0;                     // starts D2D drawing after all glass passes
    // Premultiplied BGRA pixels; id must change whenever the pixels change.
    virtual void Image(uint64_t id, const uint32_t* pixels, UINT w, UINT h, const D2D1_MATRIX_3X2_F& transformDip,
                       const D2D1_RECT_F& dstDip, float opacity) = 0;
    virtual void Text(const std::wstring& text, const D2D1_RECT_F& dip, float opacity, float angleDeg = 0, int key = INT_MIN) = 0;
    virtual void Note(const std::wstring& line) = 0;     // diagnostic line in the Dock log
    virtual void EndFrame() = 0;                         // EndDraw + Present (does not wait for vblank)
    virtual bool OnCaptureMessage() = 0;                 // kMsgCaptureAvailable posted to the panel; true = new frame
    virtual bool PollCapture() = 0;                      // non-blocking; true when a new backdrop frame is ready
    virtual bool ComparePending() const { return false; } // a backdrop compare is in flight: poll again next frame
    virtual bool GlassActive() const = 0;                // last frame used real glass
    virtual void Snapshot(const std::wstring& path) = 0; // test-only readback (window is excluded from capture)
    virtual double LastGlassMs() const = 0;              // CPU time of the glass passes in the last frame
};

using FanRendererFactory = std::function<std::unique_ptr<IFanRenderer>()>;
void SetFanRendererFactory(FanRendererFactory factory);

// Dock side: creates the factory and pre-creates the D3D11 device on a background thread.
FanRendererFactory MakeGpuFanRendererFactory();

}  // namespace dock::downloads
