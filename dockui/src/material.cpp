#include "material.h"

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace dock {
namespace {

// Temporary V0.2 material. Measured macOS dark-mode plate ~= 46% blurred backdrop + 54% grey of
// luminance ~0.21. Without blur the same alpha-over formula is applied to the sharp backdrop, with
// slightly higher coverage to calm unblurred detail. Rim: 0.5 pt hairline (1 px here) brightest on
// top and bottom edges. Shadow: very weak, a few pixels.
class SmokeMaterial final : public IPlateMaterial {
public:
    HRESULT CreateDeviceResources(ID2D1DeviceContext* dc) override {
        HRESULT hr = dc->CreateSolidColorBrush(D2D1::ColorF(0.205f, 0.205f, 0.220f, 0.60f), &fill_);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &solid_))) return hr;
        const D2D1_GRADIENT_STOP rim[] = {{0.00f, D2D1::ColorF(1, 1, 1, 0.42f)},
                                          {0.18f, D2D1::ColorF(1, 1, 1, 0.20f)},
                                          {0.50f, D2D1::ColorF(1, 1, 1, 0.14f)},
                                          {0.82f, D2D1::ColorF(1, 1, 1, 0.18f)},
                                          {1.00f, D2D1::ColorF(1, 1, 1, 0.34f)}};
        ComPtr<ID2D1GradientStopCollection> stops;
        if (FAILED(hr = dc->CreateGradientStopCollection(rim, ARRAYSIZE(rim), &stops))) return hr;
        return dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}), stops.Get(), &rim_);
    }

    void ReleaseDeviceResources() override {
        fill_.Reset();
        solid_.Reset();
        rim_.Reset();
    }

    void DrawShadow(ID2D1DeviceContext* dc, const PlatePaint& p) override {
        const D2D1_RECT_F& b = p.bounds;
        const float alphas[] = {0.035f, 0.055f, 0.08f};
        const float grow[] = {3.0f, 1.8f, 0.8f};
        for (int k = 0; k < 3; ++k) {
            solid_->SetColor(D2D1::ColorF(0, 0, 0, alphas[k]));
            const float g = grow[k];
            dc->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(b.left - g, b.top - g + 1.0f, b.right + g, b.bottom + g + 1.0f),
                                  p.radius + g, p.radius + g),
                solid_.Get());
        }
    }

    void DrawBody(ID2D1DeviceContext* dc, const PlatePaint& p) override {
        // A future material would sample p.backdrop here; the smoke fill ignores it.
        dc->FillGeometry(p.shape, fill_.Get());
    }

    void DrawRim(ID2D1DeviceContext* dc, const PlatePaint& p) override {
        rim_->SetStartPoint(D2D1::Point2F(0, p.bounds.top));
        rim_->SetEndPoint(D2D1::Point2F(0, p.bounds.bottom));
        const float px = 1.0f / p.dpiScale;  // one physical pixel, matching the 0.5 pt hairline at 2x
        dc->DrawGeometry(p.rimShape, rim_.Get(), px);
        if (p.innerShape) {
            solid_->SetColor(D2D1::ColorF(1, 1, 1, 0.045f));
            dc->DrawGeometry(p.innerShape, solid_.Get(), 2.0f * px);
        }
    }

    const wchar_t* Name() const override { return L"smoke (temporary, no blur; P0.3 Liquid Glass pending)"; }

private:
    ComPtr<ID2D1SolidColorBrush> fill_;
    ComPtr<ID2D1SolidColorBrush> solid_;
    ComPtr<ID2D1LinearGradientBrush> rim_;
};

}  // namespace

std::unique_ptr<IPlateMaterial> CreateSmokeMaterial() { return std::make_unique<SmokeMaterial>(); }

}  // namespace dock
