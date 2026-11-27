// GPU renderer for the Downloads Fan (V0.9). See fan_gpu.h.
// Pattern follows control_center.cpp: flip-model composition swap chain on a DirectComposition visual,
// LiquidGlassMaterial::RenderGpu per capsule over the WGC backdrop, then D2D (same back buffer) for
// icons and text. The window is excluded from capture by the WGC source.
#include "fan_gpu.h"
#include "display_adapter.h"

#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "backdrop_wgc.h"
#include "color_output.h"
#include "design_tokens.h"
#include "glass_params.h"
#include "log.h"
#include "material.h"

using Microsoft::WRL::ComPtr;

namespace dock::downloads {
namespace {

// One D3D11 device for every Fan opening (creating one took 165-210 ms in the Control Center measurements). The Dock
// hands over its own device (SetFanDevice): the Fan runs on the Dock's thread, and every extra hardware device costs
// about 37 driver threads plus its memory. Without one, a device of its own is made on first use.
std::mutex g_deviceMutex;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11Device> g_given;
HRESULT SharedDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_given && g_given->GetDeviceRemovedReason() == S_OK) { device = g_given; g_given->GetImmediateContext(&context); return S_OK; }
    if (g_device && g_device->GetDeviceRemovedReason() != S_OK) { g_device.Reset(); g_context.Reset(); }
    if (!g_device) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = docky::CreateRenderDevice(D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, &g_device, nullptr, &g_context);
        if (FAILED(hr)) { g_device.Reset(); g_context.Reset(); return hr; }
    }
    device = g_device;
    context = g_context;
    return S_OK;
}

double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

class GpuFanRenderer final : public IFanRenderer {
public:
    ~GpuFanRenderer() override {
        DLOG(L"fan gpu: frames=%llu with_backdrop=%llu with_glass=%llu wgc_state=%d captures=%llu", frameNo_, framesWithBackdrop_, framesWithGlass_, backdrop_.State(), captures_);
        DLOG(L"fan contrast: samples=%llu (dark ink %llu, light ink %llu) min=%.2f avg=%.2f below4.5=%llu mode_switches=%llu", measured_, darkSamples_, lightSamples_,
             measured_ ? minRatio_ : 0.0f, measured_ ? ratioSum_ / measured_ : 0.0, below_, switches_);
        backdrop_.Request(false, nullptr);
        backdrop_.SetDevice(nullptr);
        if (glass_) glass_->ReleaseGpuResources();
        if (ctx_) { ctx_->ClearState(); ctx_->Flush(); }  // shared device: leave no bindings behind
    }

    bool Init(HWND hwnd, UINT width, UINT height, float dpiScale) override {
        hwnd_ = hwnd; w_ = width; h_ = height; scale_ = dpiScale;
        const double t0 = NowMs();
        if (FAILED(SharedDevice(d3d_, ctx_))) { DLOG(L"fan gpu: no D3D11 device"); return false; }
        ComPtr<IDXGIDevice> dxgi;
        d3d_.As(&dxgi);
        ComPtr<IDXGIFactory2> f;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w_;
        sd.Height = h_;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        floatingScene_ = colors_.Init(d3d_.Get());
        if (floatingScene_) {
            sd.Format = docky::color::SceneFormat;
            scRgb_ = SUCCEEDED(f->CreateSwapChainForComposition(dxgi.Get(), &sd, nullptr, &swap_)) && docky::color::EnableScRgb(swap_.Get());
        }
        if (!scRgb_) {
            swap_.Reset();
            sd.Format = docky::color::SdrFormat;
            if (FAILED(f->CreateSwapChainForComposition(dxgi.Get(), &sd, nullptr, &swap_))) return false;
        }
        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                     reinterpret_cast<void**>(factory_.GetAddressOf()))))
            return false;
        if (FAILED(factory_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
        if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
        const float dpi = 96.0f * scale_;
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        if (floatingScene_ && colors_.Bind(dc_.Get(), w_, h_) && SUCCEEDED(d3d_->CreateRenderTargetView(back.Get(), nullptr, &outputRtv_))) {
            target_ = colors_.Bitmap();  // glass and text in FP16; EndFrame resolves to scRGB or dithered 8 bits
            back = colors_.Texture();
            if (dc_->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT)) {
                D2D1_RENDERING_CONTROLS controls{};
                dc_->GetRenderingControls(&controls);
                controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
                dc_->SetRenderingControls(&controls);
            }
        } else {
            if (scRgb_) return false;
            floatingScene_ = false;
            ComPtr<IDXGISurface> surface;
            if (FAILED(back.As(&surface))) return false;
            const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
            if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_))) return false;
        }
        dc_->SetTarget(target_.Get());
        dc_->SetDpi(dpi, dpi);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(d3d_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) return false;
        back_ = back;
        D3D11_TEXTURE2D_DESC sdsc{};
        back->GetDesc(&sdsc);
        sdsc.Usage = D3D11_USAGE_STAGING;
        sdsc.BindFlags = 0;
        sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sdsc.MiscFlags = 0;
        for (auto& sl : slots_) if (FAILED(d3d_->CreateTexture2D(&sdsc, nullptr, &sl.tex))) return false;
        for (int i = 0; i < 256; ++i) lut_[i] = Lin(i / 255.0f);
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
        dcomp_->CreateVisual(&visual_);
        visual_->SetContent(swap_.Get());
        dcompTarget_->SetRoot(visual_.Get());
        dcomp_->Commit();
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
            return false;
        dwrite_->CreateTextFormat(liquid::design::type::Family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, liquid::design::type::Label,
                                  L"", &text_);
        if (!text_) return false;
        text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(text_.Get(), &ellipsis))) {
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            text_->SetTrimming(&t, ellipsis.Get());
        }
        dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
        glass_ = CreateLiquidGlassMaterial();
        glassOk_ = glass_ && SUCCEEDED(glass_->CreateGpuResources(d3d_.Get()));
        GetWindowRect(hwnd_, &windowRect_);
        backdrop_.SetDevice(d3d_.Get());
        backdrop_.SetExcludedWindow(hwnd_);
        backdrop_.Request(true, MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        // Capsule material: GlassRole::FanCapsule (glass_params.cpp), resolved per capsule.
        DLOG(L"fan gpu: init %ux%u in %.1f ms, glass %ls", w_, h_, NowMs() - t0, glassOk_ ? L"ready" : L"unavailable");
        return true;
    }

    bool BeginFrame() override {
        const float clear[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv_.Get(), clear);
        haveFrame_ = glassOk_ && backdrop_.Latest(windowRect_, frame_);
        if (haveFrame_) ++framesWithBackdrop_;
        anyGlass_ = false;
        glassMs_ = 0;
        fallbacks_.clear();
        frameMeasures_.clear();
        ++frameNo_;
        ReadMeasurements();
        return true;
    }

    void Capsule(const D2D1_RECT_F& dip, float opacity, bool hover, float angleDeg, int key) override {
        if (opacity <= 0.003f) return;
        if (haveFrame_) {
            const double t0 = NowMs();
            GlassParameters gp = RoleGlass(GlassRole::FanCapsule);
            gp.radius = (dip.bottom - dip.top) * 0.5f;
            GpuPaint g;
            g.context = ctx_.Get();
            g.target = rtv_.Get();
            g.targetWidth = w_;
            g.targetHeight = h_;
            g.windowScreenRect = windowRect_;
            g.dpiScale = scale_;
            g.plateDip = dip;
            g.backdrop = &frame_;
            g.params = &gp;
            g.hover = hover ? 1.0f : 0.0f;
            g.rotationDeg = angleDeg;
            if (key != INT_MIN) {
                Contrast& c = State(key);
                gp.tintR = gp.tintG = 0.10f; gp.tintB = 0.11f;  // neutral graphite, like the reference fan capsules
                gp.tintStrength = std::min(c.s, 3.2f);
                // Measure the inner part of the capsule (away from rim highlights), in window pixels.
                const float cx = (dip.left + dip.right) * 0.5f * scale_, cy = (dip.top + dip.bottom) * 0.5f * scale_;
                const float hw = (dip.right - dip.left) * 0.30f * scale_, hh = (dip.bottom - dip.top) * 0.22f * scale_;
                const LONG l = std::max(0L, static_cast<LONG>(cx - hw)), t = std::max(0L, static_cast<LONG>(cy - hh));
                const LONG r = std::min(static_cast<LONG>(w_), static_cast<LONG>(cx + hw)), b = std::min(static_cast<LONG>(h_), static_cast<LONG>(cy + hh));
                if (r > l + 2 && b > t + 2 && opacity > 0.9f) frameMeasures_.push_back({key, D3D11_BOX{static_cast<UINT>(l), static_cast<UINT>(t), 0, static_cast<UINT>(r), static_cast<UINT>(b), 1}});
            }
            Materialize(gp, opacity);  // after the contrast tint so the tint materializes too
            const bool ok = glass_->RenderGpu(g);
            glassMs_ += NowMs() - t0;
            if (ok) { anyGlass_ = true; return; }
        }
        fallbacks_.push_back({dip, opacity, angleDeg});
    }

    void BeginOverlay() override {
        Slot& sl = slots_[slotIndex_];
        if (!sl.pending && !frameMeasures_.empty() && frameNo_ % 2 == 0) {
            for (const auto& m : frameMeasures_) ctx_->CopySubresourceRegion(sl.tex.Get(), 0, m.box.left, m.box.top, 0, back_.Get(), 0, &m.box);
            sl.items = frameMeasures_; sl.pending = true; slotIndex_ = (slotIndex_ + 1) % 3;
        }
        const double now = NowMs();
        for (auto& [k, c] : contrast_) { const double dt = c.lastT > 0 ? (now - c.lastT) : 0; c.lastT = now;
            const float target = static_cast<float>(c.mode), step = static_cast<float>(dt / 150.0);  // ~150 ms ink cross-fade
            c.textT = c.textT < target ? std::min(target, c.textT + step) : std::max(target, c.textT - step); }
        dc_->BeginDraw();
        drawing_ = true;
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        // Opaque-looking smoked capsule when there is no live backdrop or GPU glass (visually consistent shape).
        for (const auto& [r, a, angle] : fallbacks_) {
            const float rad = (r.bottom - r.top) * 0.5f;
            dc_->SetTransform(D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f)));
            brush_->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.18f, 0.88f * a));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.22f * a));
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get(), 1.0f / scale_);
        }
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void Image(uint64_t id, const uint32_t* pixels, UINT w, UINT h, const D2D1_MATRIX_3X2_F& transformDip, const D2D1_RECT_F& dstDip,
               float opacity) override {
        auto& slot = images_[id];
        if (!slot.bitmap && pixels) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc_->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4, &props, &slot.bitmap);
        }
        slot.lastFrame = frameNo_;
        if (!slot.bitmap) return;
        dc_->SetTransform(transformDip);
        dc_->DrawBitmap(slot.bitmap.Get(), dstDip, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void Text(const std::wstring& text, const D2D1_RECT_F& dip, float opacity, float angleDeg, int key) override {
        if (text.empty() || opacity <= 0.003f) return;
        dc_->SetTransform(D2D1::Matrix3x2F::Rotation(angleDeg, D2D1::Point2F((dip.left + dip.right) * 0.5f, (dip.top + dip.bottom) * 0.5f)));
        const auto len = static_cast<UINT32>(text.size());
        // White label with a soft dark lift so it stays readable over light wallpapers seen through the glass.
        float t = 0;  // 0 = light ink (#F7FAFF), 1 = dark ink (#172333)
        if (key != INT_MIN && anyGlass_) { auto it = contrast_.find(key); if (it != contrast_.end()) t = it->second.textT; }
        auto mix = [&](float a, float b) { return a + (b - a) * t; };
        brush_->SetColor(D2D1::ColorF(mix(0, 1), mix(0, 1), mix(0, 1), mix(0.35f, 0.30f) * opacity));  // soft lift opposite the ink
        dc_->DrawTextW(text.c_str(), len, text_.Get(), D2D1::RectF(dip.left, dip.top + 1.0f, dip.right, dip.bottom + 1.0f), brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush_->SetColor(D2D1::ColorF(mix(0xF7 / 255.f, 0x17 / 255.f), mix(0xFA / 255.f, 0x23 / 255.f), mix(1.0f, 0x33 / 255.f), opacity));
        dc_->DrawTextW(text.c_str(), len, text_.Get(), dip, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    void Note(const std::wstring& line) override { DLOG(L"%ls", line.c_str()); }

    void EndFrame() override {
        if (drawing_) { dc_->EndDraw(); drawing_ = false; }
        if (anyGlass_) ++framesWithGlass_;
        if (!snapshotPath_.empty()) WriteSnapshot();
        if (floatingScene_ && !colors_.Resolve(outputRtv_.Get(), scRgb_ ? colors_.WhiteForWindow(hwnd_) : 0.f)) return;
        swap_->Present(0, 0);  // paced by the panel's DwmFlush ticker; never blocks here
        if (frameNo_ % 240 == 0)
            for (auto it = contrast_.begin(); it != contrast_.end();) if (frameNo_ - it->second.seen > 480) it = contrast_.erase(it); else ++it;
        if (frameNo_ % 120 == 0)
            for (auto it = images_.begin(); it != images_.end();)
                if (frameNo_ - it->second.lastFrame > 120) it = images_.erase(it);
                else ++it;
    }

    bool OnCaptureMessage() override { ++captures_; return backdrop_.ProcessCapture(windowRect_); }
    bool PollCapture() override {
        if (++polls_ % 240 == 0) backdrop_.CheckHealth();
        return backdrop_.CompleteCompare(windowRect_);
    }
    bool ComparePending() const override { return backdrop_.CompletionEvent() != nullptr; }
    bool GlassActive() const override { return anyGlass_; }
    void Snapshot(const std::wstring& path) override { snapshotPath_ = path; }
    double LastGlassMs() const override { return glassMs_; }

private:
    void WriteSnapshot() {
        const std::wstring path = snapshotPath_;
        snapshotPath_.clear();
        ComPtr<ID3D11Texture2D> back, staging;
        if (floatingScene_) back = colors_.SdrSnapshot(); else swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        if (!back) return;
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        if (FAILED(d3d_->CreateTexture2D(&d, nullptr, &staging))) return;
        ctx_->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(d.Width);
            ih.biHeight = -static_cast<LONG>(d.Height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            DWORD wr = 0;
            WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
            WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
            for (UINT y = 0; y < d.Height; ++y) WriteFile(f, static_cast<const BYTE*>(m.pData) + y * m.RowPitch, d.Width * 4, &wr, nullptr);
            CloseHandle(f);
        }
        ctx_->Unmap(staging.Get(), 0);
    }

    struct CachedImage { ComPtr<ID2D1Bitmap1> bitmap; uint64_t lastFrame = 0; };
    // Adaptive contrast (V0.9.2). The finished glass (after all glass passes, before text) is measured per
    // capsule from the back buffer through a 3-deep staging ring read with DO_NOT_WAIT (no GPU stall). Text
    // colour follows the untinted glass luminance with hysteresis (Launcher inks: #172333 dark, #F7FAFF light)
    // and a small per-capsule tint controller keeps the measured luminance on the readable side (>= 4.5:1).
    struct Contrast { bool init = false; int mode = 0; float s = 0, L = 0, textT = 0; double since = 0, lastT = 0; uint64_t seen = 0; };
    struct Measure { int key; D3D11_BOX box; };
    struct Slot { ComPtr<ID3D11Texture2D> tex; std::vector<Measure> items; bool pending = false; };
    static float Lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    static constexpr float kDarkInkL = 0.0157f, kLightInkL = 0.9465f;  // #172333, #F7FAFF relative luminance
    static constexpr float kTintScale = 0.25f;                          // glass.hlsl: lerp(col, tint, strength * 0.25)
    void ReadMeasurements() {
        for (int n = 0; n < 3; ++n) {
            Slot& sl = slots_[(slotIndex_ + 1 + n) % 3];  // oldest first
            if (!sl.pending) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (ctx_->Map(sl.tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
            const double now = NowMs();
            for (const auto& it : sl.items) {
                double sum = 0; int cnt = 0;
                for (UINT y = it.box.top; y < it.box.bottom; y += 2) {
                    const BYTE* line = static_cast<const BYTE*>(m.pData) + y * m.RowPitch;
                    for (UINT x = it.box.left; x < it.box.right; x += 2) {
                        float r, g, b, a;
                        if (floatingScene_) {  // FP16 scene: R, G, B, A halves, premultiplied sRGB-encoded
                            const auto* h = reinterpret_cast<const DirectX::PackedVector::HALF*>(line) + x * 4;
                            a = DirectX::PackedVector::XMConvertHalfToFloat(h[3]);
                            if (a < 0.9f) continue;
                            r = std::clamp(DirectX::PackedVector::XMConvertHalfToFloat(h[0]) / a, 0.f, 1.f);
                            g = std::clamp(DirectX::PackedVector::XMConvertHalfToFloat(h[1]) / a, 0.f, 1.f);
                            b = std::clamp(DirectX::PackedVector::XMConvertHalfToFloat(h[2]) / a, 0.f, 1.f);
                        } else {
                            const uint32_t px = reinterpret_cast<const uint32_t*>(line)[x]; a = (px >> 24) / 255.f;
                            if (a < 0.9f) continue;  // only fully covered glass
                            b = (px & 255) / 255.f / a; g = ((px >> 8) & 255) / 255.f / a; r = ((px >> 16) & 255) / 255.f / a;
                        }
                        sum += 0.2126f * lut_[std::min(255, static_cast<int>(r * 255.f + 0.5f))] + 0.7152f * lut_[std::min(255, static_cast<int>(g * 255.f + 0.5f))] +
                               0.0722f * lut_[std::min(255, static_cast<int>(b * 255.f + 0.5f))];
                        ++cnt;
                    }
                }
                if (!cnt) continue;
                Update(contrast_[it.key], static_cast<float>(sum / cnt), now);
            }
            ctx_->Unmap(sl.tex.Get(), 0);
            sl.pending = false;
        }
    }
    void Update(Contrast& c, float L, double now) {
        const float tintL = c.mode ? 1.0f : 0.0045f;            // white veil for dark text, near-black veil for light text
        const float f = std::clamp(c.s * kTintScale, 0.0f, 0.8f);
        const float L0 = f < 0.99f ? std::clamp((L - f * tintL) / (1.0f - f), 0.0f, 1.0f) : L;  // estimated untinted glass
        (void)L0;
        // V0.9.3 (macOS reference): ink is always near-white; only the dark glass tint adapts.
        if (!c.init) { c.init = true; c.mode = 0; c.textT = 0; c.since = now; }
        // Integrator toward the readable side: light ink needs L <= 0.172, dark ink needs L >= 0.247 (4.5:1).
        // Darken until the finished glass is at most L 0.14 (white ink >= ~4.5:1); relax on dark backdrops.
        // The step is rate-limited so moving or animated wallpapers do not make the capsule flicker.
        c.s = std::clamp(c.s + std::clamp(2.5f * (L - 0.14f), -0.08f, 0.08f), 0.0f, 3.2f);
        c.L = L;
        // Contrast of the ink this capsule shows now, against the measured final glass.
        const float ink = c.textT >= 0.5f ? kDarkInkL : kLightInkL;
        const float ratio = (std::max(ink, L) + 0.05f) / (std::min(ink, L) + 0.05f);
        if (now - c.since > 400.0) { ++measured_; minRatio_ = std::min(minRatio_, ratio); if (ratio < 4.5f) ++below_; ratioSum_ += ratio;
            if (c.mode) ++darkSamples_; else ++lightSamples_; }
    }
    Contrast& State(int key) { auto& c = contrast_[key]; c.seen = frameNo_; return c; }
    std::map<int, Contrast> contrast_;
    std::array<Slot, 3> slots_;
    int slotIndex_ = 0;
    std::vector<Measure> frameMeasures_;
    std::array<float, 256> lut_{};
    ComPtr<ID3D11Texture2D> back_;
    uint64_t measured_ = 0, below_ = 0, switches_ = 0, darkSamples_ = 0, lightSamples_ = 0;
    double ratioSum_ = 0;
    float minRatio_ = 99.0f;
    HWND hwnd_ = nullptr;
    UINT w_ = 0, h_ = 0;
    float scale_ = 1.0f;
    RECT windowRect_{};
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_, outputRtv_;
    docky::color::ColorOutput colors_;
    bool floatingScene_ = false, scRgb_ = false;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> text_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    std::unique_ptr<IPlateMaterial> glass_;
    bool glassOk_ = false, haveFrame_ = false, anyGlass_ = false, drawing_ = false;
    WgcBackdropSource backdrop_;
    BackdropFrame frame_{};
    struct Fallback { D2D1_RECT_F r; float a, angle; };
    std::vector<Fallback> fallbacks_;
    std::map<uint64_t, CachedImage> images_;
    uint64_t frameNo_ = 0, polls_ = 0, framesWithBackdrop_ = 0, framesWithGlass_ = 0, captures_ = 0;
    double glassMs_ = 0;
    std::wstring snapshotPath_;
};

}  // namespace

FanRendererFactory MakeGpuFanRendererFactory() {
    // No prewarm device: the Dock hands over its own (SetFanDevice), which is ready before the first Fan opens.
    return [] { return std::unique_ptr<IFanRenderer>(new GpuFanRenderer()); };
}

void SetFanDevice(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    g_given = device;
    if (device) { g_device.Reset(); g_context.Reset(); }  // the fallback device is no longer needed
}

}  // namespace dock::downloads
