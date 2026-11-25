#pragma once
// Plate material boundary. The renderer computes the plate geometry and hands it to an
// IPlateMaterial; the material decides how the plate looks. Two paths exist:
//  * D2D path (DrawShadow/DrawBody/DrawRim): SmokeMaterial, the V0.2 fallback (no blur).
//  * GPU path (UsesGpu/RenderGpu): LiquidGlassMaterial (P0.3), which draws the whole plate with its
//    own HLSL passes into the back buffer before D2D adds icons and labels. RenderGpu returning
//    false makes the renderer fall back to the D2D path for that frame.
#include <d2d1_1.h>
#include <d3d11.h>

#include <memory>

#include "backdrop.h"
#include "glass_params.h"

namespace dock {

// The GPU SDF and Applications outline share this corner profile.
inline constexpr float kGlassCornerExtent = 1.18f;
inline constexpr float kGlassCornerExponent = 2.3f;

struct PlatePaint {
    ID2D1Geometry* shape = nullptr;       // continuous-corner plate outline, DIPs
    ID2D1Geometry* rimShape = nullptr;    // outline inset by half a stroke, for the hairline rim
    ID2D1Geometry* innerShape = nullptr;  // outline inset ~1.5 DIP, for the inner glow band
    D2D1_RECT_F bounds{};
    float radius = 0.0f;
    float dpiScale = 1.0f;                // physical pixels per DIP
    const BackdropFrame* backdrop = nullptr;  // null until a capture source exists
};

struct GpuPaint {
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    UINT targetWidth = 0, targetHeight = 0;
    RECT windowScreenRect{};              // physical pixels
    float dpiScale = 1.0f;
    D2D1_RECT_F plateDip{};               // plate rectangle in window DIPs
    const BackdropFrame* backdrop = nullptr;
    const GlassParameters* params = nullptr;
    float pointerX = 0, pointerY = 0;     // window pixels (smoothed)
    float hover = 0, press = 0;
    float rotationDeg = 0;                // plate rotation about its centre (Downloads Fan capsules); 0 elsewhere
    const RECT* clipPx = nullptr;         // optional window-local viewport, in physical pixels
};

class IPlateMaterial {
public:
    virtual ~IPlateMaterial() = default;
    virtual HRESULT CreateDeviceResources(ID2D1DeviceContext* dc) = 0;
    virtual void ReleaseDeviceResources() = 0;
    virtual void DrawShadow(ID2D1DeviceContext* dc, const PlatePaint& p) = 0;
    virtual void DrawBody(ID2D1DeviceContext* dc, const PlatePaint& p) = 0;
    virtual void DrawRim(ID2D1DeviceContext* dc, const PlatePaint& p) = 0;
    virtual const wchar_t* Name() const = 0;

    virtual bool UsesGpu() const { return false; }
    virtual HRESULT CreateGpuResources(ID3D11Device*) { return S_OK; }
    virtual void ReleaseGpuResources() {}
    virtual bool RenderGpu(const GpuPaint&) { return false; }
};

std::unique_ptr<IPlateMaterial> CreateSmokeMaterial();
std::unique_ptr<IPlateMaterial> CreateLiquidGlassMaterial();

}  // namespace dock
