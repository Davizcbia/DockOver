#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <wrl/client.h>

#include "../material.h"

namespace docky::pairing {

// The same window-local Glass Lab material for the device picker and the native phone controls.
class PairingGlass {
public:
    bool Init(ID3D11Device* device) {
        device_ = device;
        device->GetImmediateContext(&context_);
        material_ = dock::CreateLiquidGlassMaterial();
        if (SUCCEEDED(material_->CreateGpuResources(device))) return true;
        material_.reset();
        return false;
    }

    bool Prepare(UINT width, UINT height) {
        if (!material_ || width < 8 || height < 8) return false;
        if (backdrop_ && width_ == width && height_ == height && studioBackdrop_) return true;
        const auto smooth = [](float low, float high, float value) {
            const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const auto byte = [](float f) { return static_cast<uint32_t>(std::lround(f * 255.0f)); };
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
        for (UINT y = 0; y < height; ++y) {
            // Match studioDark() in bubble.hlsl. Only window-local pixels enter the lens.
            const float v = (y + 0.5f) / height;
            const float grey = std::lerp(std::lerp(0.46f, 0.30f, smooth(0, 0.6f, v)),
                0.10f, smooth(0.45f, 0.95f, v));
            for (UINT x = 0; x < width; ++x) {
                const float u = (x + 0.5f) / width - 0.5f;
                const float level = grey * (1.0f - 0.25f * u * u);
                pixels[static_cast<size_t>(y) * width + x] = 0xff000000u |
                    (byte(level * 0.95f) << 16) | (byte(level * 0.97f) << 8) | byte(level);
            }
        }
        if (!PreparePixels(width, height, pixels.data())) return false;
        studioBackdrop_ = true;
        return true;
    }

    bool PreparePixels(UINT width, UINT height, const uint32_t* pixels) {
        if (!material_ || width < 8 || height < 8) return false;
        panelView_.Reset();
        if (width_ != width || height_ != height) {
            panel_ = {};
            scratch_ = {};
        }
        controls_.clear();
        panelDpi_ = 0;
        studioBackdrop_ = false;
        auto desc = TextureDesc(width, height);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{pixels, static_cast<UINT>(width * sizeof(uint32_t)), 0};
        if (FAILED(device_->CreateTexture2D(&desc, &data, backdrop_.ReleaseAndGetAddressOf()))) return false;
        width_ = width;
        height_ = height;
        ++frameId_;
        return true;
    }

    static dock::GlassParameters PanelMaterial() {
        auto panel = dock::GlassLabDefaults();
        panel.shadowAmount = 0;
        panel.frost = 0;
        panel.saturation = 1;
        panel.bodyTone = 0;
        return panel;
    }

    static dock::GlassParameters ControlMaterial(float height) {
        auto control = dock::ControlLensGlass(height);
        control.blurSpread = PanelMaterial().blurSpread;
        control.shadowAmount = 0.6f;
        control.staticReflection = true;
        control.glare = 0.85f;
        control.staticBottomGlow = 1.1f;
        return control;
    }

    bool Draw(ID3D11RenderTargetView* target, float dpi, D2D1_RECT_F rect, const dock::GlassParameters& params,
              float x = 0, float y = 0, bool hover = false, bool pressed = false, const RECT* clip = nullptr) {
        const RECT bounds{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        const dock::BackdropFrame bg{backdrop_.Get(), bounds, frameId_};
        dock::GpuPaint paint{context_.Get(), target, width_, height_, bounds, dpi, rect, &bg, &params};
        paint.pointerX = x;
        paint.pointerY = y;
        paint.hover = hover ? 1.0f : 0.0f;
        paint.press = pressed ? 1.0f : 0.0f;
        paint.clipPx = clip;
        return material_->RenderGpu(paint);
    }

    template<class PaintLayers>
    bool CopyLayers(const RECT& rect, std::vector<uint32_t>& output, PaintLayers paintLayers) {
        if (!backdrop_ || !MakeSurface(scratch_) || rect.left < 0 || rect.top < 0 ||
            rect.right > static_cast<LONG>(width_) || rect.bottom > static_cast<LONG>(height_)) return false;
        context_->CopyResource(scratch_.texture.Get(), backdrop_.Get());
        return paintLayers(scratch_.target.Get()) && ReadPixels(rect, output);
    }

    ID3D11ShaderResourceView* Frame(float dpi) {
        if (!backdrop_) return nullptr;
        if (panelDpi_ == dpi && panelView_) return panelView_.Get();
        if (!MakeSurface(panel_)) return nullptr;
        panelView_.Reset();
        if (FAILED(device_->CreateShaderResourceView(panel_.texture.Get(), nullptr, &panelView_))) return nullptr;
        const float clear[4]{};
        context_->ClearRenderTargetView(panel_.target.Get(), clear);
        auto params = PanelMaterial();
        params.radius = 37.0f;
        if (!Draw(panel_.target.Get(), dpi, {2, 2, width_ / dpi - 2, height_ / dpi - 2}, params)) {
            panelView_.Reset();
            return nullptr;
        }
        panelDpi_ = dpi;
        controls_.clear();
        return panelView_.Get();
    }

    bool CopyControl(UINT id, const RECT& rect, float dpi, bool focused, bool pressed, uint32_t* output) {
        if (rect.left < 0 || rect.top < 0 || rect.right > static_cast<LONG>(width_) ||
            rect.bottom > static_cast<LONG>(height_) || !Frame(dpi)) return false;
        const auto height = rect.bottom - rect.top;
        auto& cached = controls_[id * 4 + (focused ? 2 : 0) + (pressed ? 1 : 0)];
        if (cached.pixels.empty() || !EqualRect(&rect, &cached.rect)) {
            if (!MakeSurface(scratch_)) return false;
            context_->CopyResource(scratch_.texture.Get(), panel_.texture.Get());
            auto params = ControlMaterial(height / dpi);
            params.radius = std::min(height / dpi * 0.5f, 21.0f);
            const D2D1_RECT_F plate{(rect.left + 0.6f) / dpi, (rect.top + 0.6f) / dpi,
                (rect.right - 0.6f) / dpi, (rect.bottom - 0.6f) / dpi};
            if (!Draw(scratch_.target.Get(), dpi, plate, params, (rect.left + rect.right) * 0.5f,
                    (rect.top + rect.bottom) * 0.5f, focused, pressed, &rect)) return false;
            if (!ReadPixels(rect, cached.pixels)) return false;
            cached.rect = rect;
        }
        std::memcpy(output, cached.pixels.data(), cached.pixels.size() * sizeof(uint32_t));
        return true;
    }

private:
    bool ReadPixels(const RECT& rect, std::vector<uint32_t>& pixels) {
        const UINT width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (!width || !height) return false;
        if (!staging_ || stageWidth_ != width || stageHeight_ != height) {
            staging_.Reset();
            auto desc = TextureDesc(width, height);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) return false;
            stageWidth_ = width;
            stageHeight_ = height;
        }
        const D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
            static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, scratch_.texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        pixels.resize(static_cast<size_t>(width) * height);
        for (UINT row = 0; row < height; ++row)
            std::memcpy(pixels.data() + static_cast<size_t>(row) * width,
                static_cast<const BYTE*>(map.pData) + static_cast<size_t>(row) * map.RowPitch, width * sizeof(uint32_t));
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    };
    struct ControlPixels { RECT rect{}; std::vector<uint32_t> pixels; };

    static D3D11_TEXTURE2D_DESC TextureDesc(UINT width, UINT height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return desc;
    }

    bool MakeSurface(Surface& surface) {
        if (surface.target) return true;
        auto desc = TextureDesc(width_, height_);
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        surface.texture.Reset();
        return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &surface.texture)) &&
            SUCCEEDED(device_->CreateRenderTargetView(surface.texture.Get(), nullptr, &surface.target));
    }

    std::unique_ptr<dock::IPlateMaterial> material_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backdrop_, staging_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> panelView_;
    Surface panel_, scratch_;
    std::map<UINT, ControlPixels> controls_;
    UINT width_ = 0, height_ = 0, stageWidth_ = 0, stageHeight_ = 0;
    uint64_t frameId_ = 0;
    bool studioBackdrop_ = false;
    float panelDpi_ = 0;
};

}  // namespace docky::pairing
