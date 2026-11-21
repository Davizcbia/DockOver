#pragma once
// Renders shaders/silk.hlsl (liquid satin) into a texture on the shared D3D11 device and exposes it as a Direct2D
// bitmap for the lyrics side. Call Render() outside a Direct2D BeginDraw/EndDraw.
#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "silk_ps.h"
#include "silk_vs.h"

namespace nowplaying {

class SilkRenderer {
public:
    bool Init(ID3D11Device* device) {
        device_ = device; device->GetImmediateContext(&context_);
        if (FAILED(device->CreateVertexShader(g_SilkVS, sizeof(g_SilkVS), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(g_SilkPS, sizeof(g_SilkPS), nullptr, &ps_))) return false;
        D3D11_BUFFER_DESC b{}; b.ByteWidth = sizeof(Constants); b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER; b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&b, nullptr, &cb_));
    }
    bool Ready() const { return ps_ != nullptr; }

    // palette: up to 6 colours, alpha = the colour's share of the artwork (all 1 = equal). twist: smoothed beat.
    // Returns the bitmap (owned here) or nullptr.
    ID2D1Bitmap1* Render(ID2D1DeviceContext* dc, UINT width, UINT height, float time, float twist, const std::vector<D2D1_COLOR_F>& palette) {
        if (!ps_ || !width || !height || palette.empty()) return nullptr;
        if (!texture_ || width != w_ || height != h_) {
            texture_.Reset(); rtv_.Reset(); bitmap_.Reset();
            D3D11_TEXTURE2D_DESC d{}; d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<IDXGISurface> surface;
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &texture_)) || FAILED(device_->CreateRenderTargetView(texture_.Get(), nullptr, &rtv_)) ||
                FAILED(texture_.As(&surface)) || FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap_))) { texture_.Reset(); return nullptr; }
            w_ = width; h_ = height;
        }
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return nullptr;
        Constants c{}; c.resolution[0] = float(width); c.resolution[1] = float(height); c.time = time; c.twist = twist;
        const size_t n = std::min<size_t>(6, palette.size());
        // Weight = share^0.7 x colourfulness: the main colour still leads, but a small vivid accent (the orange on a black
        // cover) gets real room instead of disappearing into the dominant dark.
        std::vector<D2D1_COLOR_F> sorted(palette.begin(), palette.begin() + n);
        std::stable_sort(sorted.begin(), sorted.end(), [](const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) { return a.a > b.a; });
        float total = 0;
        for (auto& s : sorted) {
            const float chroma = std::max({s.r, s.g, s.b}) - std::min({s.r, s.g, s.b});
            s.a = std::max(.03f, std::pow(std::clamp(s.a, 0.f, 1.f), .7f) * (.35f + 1.3f * chroma));
            total += s.a;
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) { return a.a > b.a; });
        for (size_t i = 0; i < n; ++i) { c.colors[i][0] = sorted[i].r; c.colors[i][1] = sorted[i].g; c.colors[i][2] = sorted[i].b; c.colors[i][3] = sorted[i].a / total; }
        c.colorCount = float(n);
        std::memcpy(map.pData, &c, sizeof(c)); context_->Unmap(cb_.Get(), 0);
        ID3D11RenderTargetView* rtv = rtv_.Get();
        const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        context_->OMSetRenderTargets(1, &rtv, nullptr); context_->RSSetViewports(1, &vp);
        context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0); context_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cb = cb_.Get(); context_->PSSetConstantBuffers(0, 1, &cb);
        context_->Draw(3, 0);
        ID3D11RenderTargetView* none = nullptr; context_->OMSetRenderTargets(1, &none, nullptr);
        return bitmap_.Get();
    }

private:
    struct Constants { float resolution[2]; float time, twist; float colors[6][4]; float colorCount; float padding[3]; };
    Microsoft::WRL::ComPtr<ID3D11Device> device_; Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_; Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_; Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    UINT w_ = 0, h_ = 0;
};

}  // namespace nowplaying
