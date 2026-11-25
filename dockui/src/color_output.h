#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <vector>

#include "color_output_ps.h"
#include "color_output_vs.h"

namespace docky::color {

inline constexpr DXGI_FORMAT SceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT SdrFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
inline constexpr auto ScRgb = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

inline float DisplayWhite(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 1.f;
    UINT32 count = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &count, &modeCount) != ERROR_SUCCESS) return 1.f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &count, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return 1.f;
    for (UINT32 i = 0; i < count; ++i) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || wcscmp(info.szDevice, name.viewGdiDeviceName)) continue;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 advanced{};
        advanced.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(advanced), path.targetInfo.adapterId, path.targetInfo.id};
        bool hdr = false;
        if (DisplayConfigGetDeviceInfo(&advanced.header) == ERROR_SUCCESS) {
            hdr = advanced.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
            legacy.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(legacy), path.targetInfo.adapterId, path.targetInfo.id};
            hdr = DisplayConfigGetDeviceInfo(&legacy.header) == ERROR_SUCCESS && legacy.advancedColorEnabled;
        }
        if (!hdr) return 1.f;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) return white.SDRWhiteLevel / 1000.f;
        return 2.5f;
    }
    return 1.f;
}

inline bool EnableScRgb(IDXGISwapChain1* swap) {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> advanced;
    UINT support = 0;
    return SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&advanced))) &&
           SUCCEEDED(advanced->CheckColorSpaceSupport(ScRgb, &support)) &&
           (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
           SUCCEEDED(advanced->SetColorSpace1(ScRgb));
}

// Keeps the existing SDR material math in a floating-point working texture.
// Conversion happens at the final output, including SDR screenshot export.
class ColorOutput {
    template<typename T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    bool Init(ID3D11Device* device) {
        device_ = device;
        device_->GetImmediateContext(&context_);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = TRUE;
        return SUCCEEDED(device_->CreateVertexShader(g_ColorOutputVS, sizeof(g_ColorOutputVS), nullptr, &vs_)) &&
               SUCCEEDED(device_->CreatePixelShader(g_ColorOutputPS, sizeof(g_ColorOutputPS), nullptr, &ps_)) &&
               SUCCEEDED(device_->CreateBuffer(&desc, nullptr, &constants_)) &&
               SUCCEEDED(device_->CreateRasterizerState(&raster, &raster_));
    }

    bool Bind(ID2D1DeviceContext* dc, UINT w, UINT h) {
        if (!vs_ || !ps_ || !constants_ || !raster_ || !w || !h) return false;
        if (bitmap_ && width_ == w && height_ == h) return true;
        bitmap_.Reset();
        srv_.Reset();
        texture_.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = SceneFormat;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Ptr<IDXGISurface> surface;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(SceneFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &texture_)) ||
            FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &srv_)) ||
            FAILED(texture_.As(&surface)) ||
            FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap_))) return false;
        width_ = w;
        height_ = h;
        return true;
    }

    ID2D1Bitmap1* Bitmap() const { return bitmap_.Get(); }
    ID3D11Texture2D* Texture() const { return texture_.Get(); }

    float WhiteForWindow(HWND window) {
        const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        const auto now = GetTickCount64();
        if (monitor != monitor_ || now >= nextWhiteCheck_) {
            monitor_ = monitor;
            nextWhiteCheck_ = now + 1000;
            white_ = DisplayWhite(monitor);
        }
        return white_;
    }

    Ptr<ID3D11Texture2D> SdrSnapshot() {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = SdrFormat;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11RenderTargetView> target;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture)) ||
            FAILED(device_->CreateRenderTargetView(texture.Get(), nullptr, &target)) ||
            !Resolve(target.Get(), 0.f)) return {};
        return texture;
    }

    struct Region { RECT rect; float white; };

    bool Resolve(ID3D11RenderTargetView* target, float white, const std::vector<Region>& regions = {}) {
        if (!target || !srv_) return false;
        const D3D11_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetViewports(1, &viewport);
        context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        const RECT full{0, 0, LONG(width_), LONG(height_)};
        bool ok = true;
        if (regions.empty()) {
            ok = Draw(full, white);
        } else {
            constexpr float empty[4]{};
            context_->ClearRenderTargetView(target, empty);
            for (const auto& region : regions) {
                RECT intersection{};
                if (IntersectRect(&intersection, &full, &region.rect)) ok = Draw(intersection, region.white) && ok;
            }
        }
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(0, 1, &none);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->RSSetState(nullptr);
        return ok;
    }

private:
    bool Draw(const RECT& rectangle, float white) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
        const float value[4]{white};
        std::memcpy(map.pData, value, sizeof(value));
        context_->Unmap(constants_.Get(), 0);
        context_->RSSetScissorRects(1, &rectangle);
        context_->Draw(3, 0);
        return true;
    }

    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> ps_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11Texture2D> texture_;
    Ptr<ID3D11ShaderResourceView> srv_;
    Ptr<ID2D1Bitmap1> bitmap_;
    UINT width_ = 0;
    UINT height_ = 0;
    HMONITOR monitor_ = nullptr;
    ULONGLONG nextWhiteCheck_ = 0;
    float white_ = 1.f;
};

}  // namespace docky::color
