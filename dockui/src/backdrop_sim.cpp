#include "backdrop_sim.h"

#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dock {
namespace {

uint32_t Pack(float r, float g, float b) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b);
}

void HueToRgb(float h, float& r, float& g, float& b) {
    h = h - std::floor(h);
    r = std::clamp(std::fabs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    g = std::clamp(2.0f - std::fabs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    b = std::clamp(2.0f - std::fabs(h * 6.0f - 4.0f), 0.0f, 1.0f);
}

void BuildGrid(std::vector<uint32_t>& px, UINT w, UINT h) {
    // Checkerboard + thin white grid lines + diagonal hue bands: shows refraction, dispersion, blur.
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const bool check = ((x / 32) + (y / 32)) & 1;
            float v = check ? 0.30f : 0.18f;
            float r = v, g = v, b = v;
            if (x % 64 == 0 || y % 64 == 0) r = g = b = 0.92f;
            const float band = std::fmod((x + y) / 220.0f, 1.0f);
            if (band < 0.18f) {
                float hr, hg, hb;
                HueToRgb((x + y) / 1400.0f, hr, hg, hb);
                r = r * 0.35f + hr * 0.65f;
                g = g * 0.35f + hg * 0.65f;
                b = b * 0.35f + hb * 0.65f;
            }
            px[static_cast<size_t>(y) * w + x] = Pack(r, g, b);
        }
}

void BuildGradient(std::vector<uint32_t>& px, UINT w, UINT h) {
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            float r, g, b;
            HueToRgb(static_cast<float>(x) / w, r, g, b);
            const float k = 0.35f + 0.65f * static_cast<float>(y) / h;
            px[static_cast<size_t>(y) * w + x] = Pack(r * k, g * k, b * k);
        }
}

// Renders the wallpaper of the given monitor rectangle the way the desktop places it.
bool BuildWallpaper(std::vector<uint32_t>& px, UINT w, UINT h, const RECT& monitorRect) {
    ComPtr<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) return false;
    COLORREF bg = 0;
    dw->GetBackgroundColor(&bg);
    const uint32_t bgPx = 0xFF000000u | (GetRValue(bg) << 16) | (GetGValue(bg) << 8) | GetBValue(bg);
    std::fill(px.begin(), px.end(), bgPx);
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    dw->GetPosition(&pos);
    UINT count = 0;
    dw->GetMonitorDevicePathCount(&count);
    std::wstring path;
    for (UINT i = 0; i < count && path.empty(); ++i) {
        LPWSTR id = nullptr;
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        RECT r{};
        if (SUCCEEDED(dw->GetMonitorRECT(id, &r)) && EqualRect(&r, &monitorRect)) {
            LPWSTR wp = nullptr;
            if (SUCCEEDED(dw->GetWallpaper(id, &wp)) && wp) {
                path = wp;
                CoTaskMemFree(wp);
            }
        }
        CoTaskMemFree(id);
    }
    if (path.empty()) {
        DLOG(L"backdrop: no wallpaper image for this monitor; using background colour");
        return true;
    }
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT iw = 0, ih = 0;
    conv->GetSize(&iw, &ih);
    if (!iw || !ih) return false;

    double sx = 1.0, sy = 1.0;
    switch (pos) {
    case DWPOS_FIT: sx = sy = std::min(static_cast<double>(w) / iw, static_cast<double>(h) / ih); break;
    case DWPOS_STRETCH: sx = static_cast<double>(w) / iw; sy = static_cast<double>(h) / ih; break;
    case DWPOS_CENTER:
    case DWPOS_TILE: sx = sy = 1.0; break;
    default: sx = sy = std::max(static_cast<double>(w) / iw, static_cast<double>(h) / ih); break;  // FILL, SPAN
    }
    const UINT dw2 = std::max<UINT>(1, static_cast<UINT>(std::lround(iw * sx)));
    const UINT dh2 = std::max<UINT>(1, static_cast<UINT>(std::lround(ih * sy)));
    ComPtr<IWICBitmapSource> src = conv;
    ComPtr<IWICBitmapScaler> scaler;
    if (dw2 != iw || dh2 != ih) {
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(conv.Get(), dw2, dh2, WICBitmapInterpolationModeFant)))
            return false;
        src = scaler;
    }
    std::vector<uint32_t> img(static_cast<size_t>(dw2) * dh2);
    if (FAILED(src->CopyPixels(nullptr, dw2 * 4, static_cast<UINT>(img.size() * 4), reinterpret_cast<BYTE*>(img.data()))))
        return false;
    if (pos == DWPOS_TILE) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) px[static_cast<size_t>(y) * w + x] = img[(y % dh2) * dw2 + (x % dw2)];
        return true;
    }
    const int ox = (static_cast<int>(w) - static_cast<int>(dw2)) / 2;
    const int oy = (static_cast<int>(h) - static_cast<int>(dh2)) / 2;
    for (UINT y = 0; y < h; ++y) {
        const int sy2 = static_cast<int>(y) - oy;
        if (sy2 < 0 || sy2 >= static_cast<int>(dh2)) continue;
        for (UINT x = 0; x < w; ++x) {
            const int sx2 = static_cast<int>(x) - ox;
            if (sx2 < 0 || sx2 >= static_cast<int>(dw2)) continue;
            px[static_cast<size_t>(y) * w + x] = img[static_cast<size_t>(sy2) * dw2 + sx2] | 0xFF000000u;
        }
    }
    return true;
}

}  // namespace

SimulatedBackdropSource::~SimulatedBackdropSource() = default;

void SimulatedBackdropSource::SetDevice(ID3D11Device* device) {
    device_ = device;
    texture_.Reset();
    if (device_ && !lastPixels_.empty()) {
        // Re-upload after a device reset without decoding again.
        std::lock_guard<std::mutex> lock(pending_->mutex);
        pending_->pixels = lastPixels_;
        pending_->width = lastW_;
        pending_->height = lastH_;
        pending_->rect = rect_;
        pending_->ok = true;
        pending_->ready = true;
        pending_->generation = generation_;
    }
}

void SimulatedBackdropSource::Request(BackdropKind kind, HMONITOR monitor) {
    if (kind == kind_ && monitor == monitor_) return;
    kind_ = kind;
    monitor_ = monitor;
    texture_.Reset();
    lastPixels_.clear();
    if (kind == BackdropKind::None) {
        state_ = 0;
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) {
        state_ = 3;
        return;
    }
    const unsigned gen = ++generation_;
    state_ = 1;
    auto pending = pending_;
    const RECT rect = mi.rcMonitor;
    const HWND notify = GlassSettingsStore::Get().DockWindow();
    std::thread([pending, kind, rect, gen, notify] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const UINT w = static_cast<UINT>(rect.right - rect.left), h = static_cast<UINT>(rect.bottom - rect.top);
        std::vector<uint32_t> px(static_cast<size_t>(w) * h, 0xFF101014u);
        bool ok = true;
        const LARGE_INTEGER t0 = [] { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v; }();
        switch (kind) {
        case BackdropKind::Wallpaper: ok = BuildWallpaper(px, w, h, rect); break;
        case BackdropKind::Grid: BuildGrid(px, w, h); break;
        case BackdropKind::Gradient: BuildGradient(px, w, h); break;
        default: break;  // Dark: already filled
        }
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        DLOG(L"backdrop: simulated %ls built %ux%u in %.0f ms (%ls)", BackdropName(kind), w, h,
             (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, ok ? L"ok" : L"FAILED, dark fallback");
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (gen >= pending->generation) {
                pending->generation = gen;
                pending->pixels = std::move(px);
                pending->width = w;
                pending->height = h;
                pending->rect = rect;
                pending->ok = ok;
                pending->ready = true;
            }
        }
        if (notify) PostMessageW(notify, kMsgGlassChanged, 0, 0);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

bool SimulatedBackdropSource::Latest(const RECT&, BackdropFrame& out) {
    if (kind_ == BackdropKind::None || !device_) return false;
    {
        std::lock_guard<std::mutex> lock(pending_->mutex);
        if (pending_->ready && pending_->generation == generation_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = pending_->width;
            td.Height = pending_->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{pending_->pixels.data(), pending_->width * 4, 0};
            texture_.Reset();
            if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &texture_))) {
                rect_ = pending_->rect;
                ++frameId_;
                lastPixels_ = std::move(pending_->pixels);
                lastW_ = pending_->width;
                lastH_ = pending_->height;
                state_ = pending_->ok ? 2 : 3;
            } else {
                state_ = 3;
            }
            pending_->pixels.clear();
            pending_->ready = false;
        }
    }
    if (!texture_) return false;
    out.texture = texture_.Get();
    out.screenRect = rect_;
    out.frameId = frameId_;
    out.captureTimeSeconds = 0.0;
    return true;
}

const wchar_t* SimulatedBackdropSource::Name() const { return BackdropName(kind_); }

}  // namespace dock
