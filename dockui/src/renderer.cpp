#include "renderer.h"

#include <d2d1effects.h>
#include <wincodec.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>

#include "design_tokens.h"
#include "log.h"
#include "color_output.h"
#include "display_adapter.h"

namespace dock {
namespace {

double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

}  // namespace

struct Renderer::Output {
    docky::color::ColorOutput colors;
    ComPtr<ID3D11RenderTargetView> swapTarget;
    bool scRgb = false;
    float white = 1.f;
    ULONGLONG nextWhiteCheck = 0;
    HMONITOR monitor = nullptr;
};

Renderer::Renderer() = default;
Renderer::~Renderer() { Shutdown(); }

// Non-blocking GPU timing: timestamps at frame start, after the glass passes and before Present,
// inside a disjoint query. Results are read a few frames later with DONOTFLUSH; disjoint
// intervals (clock changes) are discarded, so only reliable numbers are reported.
HRESULT GpuTimer::Init(ID3D11Device* device) {
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
    for (Slot& slot : slots_) {
        HRESULT hr;
        if (FAILED(hr = device->CreateQuery(&dj, &slot.disjoint))) return hr;
        for (auto& t : slot.stamps)
            if (FAILED(hr = device->CreateQuery(&ts, &t))) return hr;
        slot.pending = false;
    }
    return S_OK;
}

void GpuTimer::Release() {
    for (Slot& slot : slots_) {
        slot.disjoint.Reset();
        for (auto& t : slot.stamps) t.Reset();
        slot.pending = false;
    }
    active_ = false;
}

void GpuTimer::Begin(ID3D11DeviceContext* ctx) {
    Slot& slot = slots_[write_];
    active_ = slot.disjoint && !slot.pending;
    if (!active_) return;
    ctx->Begin(slot.disjoint.Get());
    ctx->End(slot.stamps[0].Get());
}

void GpuTimer::Mark(ID3D11DeviceContext* ctx) {
    if (active_) ctx->End(slots_[write_].stamps[1].Get());
}

void GpuTimer::End(ID3D11DeviceContext* ctx) {
    if (!active_) return;
    Slot& slot = slots_[write_];
    ctx->End(slot.stamps[2].Get());
    ctx->End(slot.disjoint.Get());
    slot.pending = true;
    write_ = (write_ + 1) % kSlots;
    active_ = false;
}

bool GpuTimer::Poll(ID3D11DeviceContext* ctx, float& frameMs, float& glassMs) {
    bool got = false;
    for (int k = 0; k < kSlots; ++k) {
        Slot& slot = slots_[(write_ + k) % kSlots];  // oldest first
        if (!slot.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(slot.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        UINT64 t[3] = {};
        bool ok = true;
        for (int i = 0; i < 3; ++i)
            ok = ok && ctx->GetData(slot.stamps[i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok) break;
        slot.pending = false;
        if (dj.Disjoint || dj.Frequency == 0) continue;
        frameMs = static_cast<float>((t[2] - t[0]) * 1000.0 / dj.Frequency);
        glassMs = static_cast<float>((t[1] - t[0]) * 1000.0 / dj.Frequency);
        got = true;
    }
    return got;
}

HRESULT Renderer::Init(HWND hwnd, UINT pxW, UINT pxH, float dpi, bool aboveChildWindows) {
    hwnd_ = hwnd;
    dpi_ = dpi;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = S_OK;
    if (useDevice_) { d3d_ = useDevice_; featureLevel_ = d3d_->GetFeatureLevel(); }  // shared with another renderer on this thread
    else {
        hr = docky::CreateRenderDevice(D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), &d3d_,
                                        &featureLevel_, nullptr);
        if (FAILED(hr)) return hr;
    }
    sharedGraphicsMemory_ = docky::UsesUnifiedMemory(d3d_.Get());
    if (FAILED(hr = d3d_.As(&dxgiDevice_))) return hr;
    ComPtr<IDXGIAdapter> renderAdapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(dxgiDevice_->GetAdapter(&renderAdapter)) && SUCCEEDED(renderAdapter->GetDesc(&adapterDesc)))
        DLOG(L"render adapter: %ls (LUID %08lx:%08lx)", adapterDesc.Description,
             static_cast<unsigned long>(adapterDesc.AdapterLuid.HighPart), adapterDesc.AdapterLuid.LowPart);
    d3d_->GetImmediateContext(&ctx_);

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                      reinterpret_cast<void**>(d2dFactory_.GetAddressOf())))) return hr;
    if (FAILED(hr = d2dFactory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_))) return hr;
    if (FAILED(hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return hr;
    auto output = std::make_unique<Output>();
    if (output->colors.Init(d3d_.Get()) &&
        output->colors.Bind(dc_.Get(), std::max<UINT>(1, pxW), std::max<UINT>(1, pxH))) {
        output_ = std::move(output);
        if (dc_->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT)) {
            D2D1_RENDERING_CONTROLS controls{};
            dc_->GetRenderingControls(&controls);
            controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
            dc_->SetRenderingControls(&controls);
        }
    }

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return hr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max<UINT>(1, pxW);
    desc.Height = std::max<UINT>(1, pxH);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (output_) {
        desc.Format = docky::color::SceneFormat;
        output_->scRgb = SUCCEEDED(factory->CreateSwapChainForComposition(dxgiDevice_.Get(), &desc, nullptr, &swapChain_)) &&
                         docky::color::EnableScRgb(swapChain_.Get());
    }
    if (!output_ || !output_->scRgb) {
        swapChain_.Reset();
        desc.Format = docky::color::SdrFormat;
        if (FAILED(hr = factory->CreateSwapChainForComposition(dxgiDevice_.Get(), &desc, nullptr, &swapChain_))) return hr;
    }
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swapChain_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }

    if (FAILED(hr = CreateTarget())) return hr;
    DLOG(L"renderer colour: %ls, %ls working surface",
         output_ && output_->scRgb ? L"FP16 scRGB" : L"BGRA8",
         output_ ? L"FP16" : L"BGRA8");

    if (FAILED(hr = DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcomp_)))) return hr;
    if (FAILED(hr = dcomp_->CreateTargetForHwnd(hwnd, aboveChildWindows ? TRUE : FALSE, &dcompTarget_))) return hr;
    if (FAILED(hr = dcomp_->CreateVisual(&visual_))) return hr;
    if (FAILED(hr = visual_->SetContent(swapChain_.Get()))) return hr;
    if (FAILED(hr = dcompTarget_->SetRoot(visual_.Get()))) return hr;
    if (FAILED(hr = dcomp_->Commit())) return hr;

    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return hr;
    if (FAILED(hr = dwrite_->CreateTextFormat(liquid::design::type::Family, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              liquid::design::type::Label, L"",
                                              &labelFormat_)))
        return hr;
    labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (SUCCEEDED(dwrite_->CreateTextFormat(liquid::design::type::Family, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            liquid::design::type::Label, L"", &menuRowFormat_))) {
        menuRowFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        menuRowFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(menuRowFormat_.Get(), &ellipsis))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            menuRowFormat_->SetTrimming(&trimming, ellipsis.Get());
        }
    }
    if (FAILED(hr = dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"",
                                              &diagFormat_)))
        return hr;

    if (FAILED(hr = dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) return hr;
    material_ = CreateSmokeMaterial();
    if (FAILED(hr = material_->CreateDeviceResources(dc_.Get()))) return hr;
    liquid_ = CreateLiquidGlassMaterial();
    if (FAILED(hr = liquid_->CreateGpuResources(d3d_.Get()))) {
        DLOG(L"liquid glass: GPU resources failed 0x%08X; Smoked Glass fallback only", hr);
        liquid_.reset();
    }
    if (FAILED(timer_.Init(d3d_.Get()))) timer_.Release();
    return S_OK;
}

void Renderer::Shutdown() {
    // Detach the swap chain from DWM before releasing the composition objects.
    if (visual_) visual_->SetContent(nullptr);
    if (dcompTarget_) dcompTarget_->SetRoot(nullptr);
    if (dcomp_) dcomp_->Commit();
    if (dc_) dc_->SetTarget(nullptr);
    uiBitmap_.Reset(); uiValid_ = false; uiW_ = uiH_ = 0;
    if (material_) material_->ReleaseDeviceResources();
    material_.reset();
    if (liquid_) liquid_->ReleaseGpuResources();
    liquid_.reset();
    timer_.Release();
    rtv_.Reset();
    for(auto& cache:localGlassCaches_){if(cache.material)cache.material->ReleaseGpuResources();cache.material.reset();cache.scene.Reset();cache.width=cache.height=0;}
    brush_.Reset();
    diagFormat_.Reset();
    menuRowFormat_.Reset();
    menuIcons_.clear();
    labelFormat_.Reset();
    dwrite_.Reset();
    opacityEffect_.Reset();
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    target_.Reset();
    output_.reset();
    dc_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    if (waitable_) {
        CloseHandle(waitable_);
        waitable_ = nullptr;
    }
    swapChain_.Reset();
    if (ctx_) ctx_->ClearState();
    ctx_.Reset();
    dxgiDevice_.Reset();
    d3d_.Reset();
}

HRESULT Renderer::CreateTarget() {
    ComPtr<ID3D11Texture2D> buffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC bd{};
    buffer->GetDesc(&bd);
    if (output_) {
        if ((!output_->colors.Bitmap() || targetW_ != bd.Width || targetH_ != bd.Height) &&
            !output_->colors.Bind(dc_.Get(), bd.Width, bd.Height)) return E_FAIL;
        if (FAILED(hr = d3d_->CreateRenderTargetView(buffer.Get(), nullptr, &output_->swapTarget))) return hr;
        target_ = output_->colors.Bitmap();
        buffer = output_->colors.Texture();
    } else {
    ComPtr<IDXGISurface> surface;
    if (FAILED(hr = buffer.As(&surface))) return hr;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return hr;
    }
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi_, dpi_);
    targetW_ = bd.Width;
    targetH_ = bd.Height;
    return d3d_->CreateRenderTargetView(buffer.Get(), nullptr, &rtv_);
}

HRESULT Renderer::Resize(UINT pxW, UINT pxH, float dpi) {
    dpi_ = dpi;
    uiValid_ = false;  // the cached UI layer is rebuilt at the new size
    dc_->SetTarget(nullptr);
    target_.Reset();
    rtv_.Reset();
    if (output_) output_->swapTarget.Reset();
    if (ctx_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
    }
    HRESULT hr = swapChain_->ResizeBuffers(2, std::max<UINT>(1, pxW), std::max<UINT>(1, pxH), DXGI_FORMAT_UNKNOWN,
                                           DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) return hr;
    return CreateTarget();
}

HRESULT Renderer::CreateIcon(const std::vector<uint32_t>& pixels, UINT w, UINT h, ComPtr<ID2D1Bitmap1>& icon,
                             ComPtr<ID2D1Bitmap1>& shadow, float& shadowPadRatio) {
    icon.Reset();
    shadow.Reset();
    shadowPadRatio = 0.0f;
    if (pixels.empty() || w == 0 || h == 0) return E_INVALIDARG;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    HRESULT hr = dc_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, &props, &icon);
    if (FAILED(hr)) return hr;
    // macOS Dock icons carry no drop shadow (measured); keep the code path but disable it.
    constexpr bool kIconShadows = false;
    if (!kIconShadows) return S_OK;

    // Pre-render a soft drop shadow once per icon so per-frame cost stays a single bitmap draw.
    const float sigma = std::max(2.0f, w * 0.05f);
    const UINT pad = static_cast<UINT>(sigma * 3.0f + 0.5f);
    ComPtr<ID2D1Effect> fx;
    if (FAILED(dc_->CreateEffect(CLSID_D2D1Shadow, &fx))) return S_OK;
    fx->SetInput(0, icon.Get());
    fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);
    fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.55f));
    const D2D1_BITMAP_PROPERTIES1 tprops = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96,
        96);
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w + 2 * pad, h + 2 * pad), nullptr, 0, &tprops, &shadow))) return S_OK;
    ComPtr<ID2D1Image> old;
    dc_->GetTarget(&old);
    dc_->SetTarget(shadow.Get());
    dc_->SetDpi(96, 96);
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->DrawImage(fx.Get(), D2D1::Point2F(static_cast<float>(pad), static_cast<float>(pad)));
    hr = dc_->EndDraw();
    dc_->SetTarget(old.Get());
    dc_->SetDpi(dpi_, dpi_);
    if (FAILED(hr)) {
        shadow.Reset();
        return S_OK;
    }
    shadowPadRatio = static_cast<float>(pad) / static_cast<float>(w);
    return S_OK;
}

HRESULT Renderer::QueryFrameStats(UINT& presentCount, UINT& presentRefreshCount, UINT& syncRefreshCount) {
    DXGI_FRAME_STATISTICS stats{};
    HRESULT hr = swapChain_->GetFrameStatistics(&stats);
    if (SUCCEEDED(hr)) {
        presentCount = stats.PresentCount;
        presentRefreshCount = stats.PresentRefreshCount;
        syncRefreshCount = stats.SyncRefreshCount;
    }
    return hr;
}

bool Renderer::PresentTiming(UINT& lastPresentCount, UINT& shownPresentCount, LONGLONG& shownQpc) {
    if (!swapChain_ || FAILED(swapChain_->GetLastPresentCount(&lastPresentCount))) return false;
    DXGI_FRAME_STATISTICS stats{};
    if (swapChain_->GetFrameStatistics(&stats) != S_OK) return false;
    shownPresentCount = stats.PresentCount; shownQpc = stats.SyncQPCTime.QuadPart;
    return true;
}

ComPtr<ID3D11Texture2D> Renderer::SnapshotSource() {
    if (output_) return output_->colors.SdrSnapshot();
    ComPtr<ID3D11Texture2D> source;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&source));
    return source;
}

// One-shot SDR diagnostic export of this renderer's own surface.
// WIC expects straight BGRA: undo premultiplication so translucent edges do not acquire dark halos.
void Renderer::SavePngSnapshot() {
    pngSnapshotRequested_ = false;
    ComPtr<ID3D11Texture2D> source = SnapshotSource(), staging;
    if (!source) { DLOG(L"snapshot png: SDR source unavailable"); return; }
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    HRESULT hr = d3d_->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) { DLOG(L"snapshot png: staging failed 0x%08X", hr); return; }
    ctx_->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; hr = ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { DLOG(L"snapshot png: map failed 0x%08X", hr); return; }
    const UINT stride=desc.Width*4; std::vector<BYTE> straight(static_cast<size_t>(stride)*desc.Height);
    for(UINT y=0;y<desc.Height;++y){const BYTE* src=static_cast<const BYTE*>(map.pData)+static_cast<size_t>(y)*map.RowPitch; BYTE* dst=straight.data()+static_cast<size_t>(y)*stride;
        for(UINT x=0;x<desc.Width;++x){const unsigned a=src[x*4+3];dst[x*4+3]=static_cast<BYTE>(a);
            for(unsigned c=0;c<3;++c)dst[x*4+c]=a?static_cast<BYTE>((std::min)(255u,(static_cast<unsigned>(src[x*4+c])*255u+a/2u)/a)):0;}}
    ctx_->Unmap(staging.Get(),0);
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IStream> stream;
    hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=CreateStreamOnHGlobal(nullptr,TRUE,&stream);
    if(SUCCEEDED(hr))hr=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(hr))hr=encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,nullptr);
    if(SUCCEEDED(hr))hr=frame->Initialize(nullptr);
    if(SUCCEEDED(hr))hr=frame->SetSize(desc.Width,desc.Height);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&format);
    if(SUCCEEDED(hr) && !IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))hr=E_FAIL;
    if(SUCCEEDED(hr))hr=frame->WritePixels(desc.Height,stride,static_cast<UINT>(straight.size()),straight.data());
    if(SUCCEEDED(hr))hr=frame->Commit();
    if(SUCCEEDED(hr))hr=encoder->Commit();
    STATSTG stat{}; HGLOBAL memory=nullptr;
    if(SUCCEEDED(hr))hr=stream->Stat(&stat,STATFLAG_NONAME);
    if(SUCCEEDED(hr))hr=GetHGlobalFromStream(stream.Get(),&memory);
    if(FAILED(hr)||stat.cbSize.HighPart){DLOG(L"snapshot png: encoding failed 0x%08X",hr);return;}
    wchar_t dir[32768]{}; DWORD n=GetEnvironmentVariableW(L"LIQUID_DOCK_SNAPSHOT_DIR",dir,static_cast<DWORD>(std::size(dir)));
    std::wstring folder;
    if(n && n<std::size(dir))folder.assign(dir,n);
    else {n=GetEnvironmentVariableW(L"LOCALAPPDATA",dir,static_cast<DWORD>(std::size(dir))); if(!n||n>=std::size(dir)){DLOG(L"snapshot png: LOCALAPPDATA unavailable");return;} folder.assign(dir,n);folder+=L"\\LiquidDesktop\\debug\\snapshots";}
    const int mk=SHCreateDirectoryExW(nullptr,folder.c_str(),nullptr);
    if(mk!=ERROR_SUCCESS && mk!=ERROR_ALREADY_EXISTS && mk!=ERROR_FILE_EXISTS){DLOG(L"snapshot png: directory failed %d",mk);return;}
    SYSTEMTIME tm{};GetLocalTime(&tm); HANDLE file=INVALID_HANDLE_VALUE;std::wstring path;
    for(unsigned i=0;i<1000 && file==INVALID_HANDLE_VALUE;++i){wchar_t name[160]{};
        swprintf_s(name,L"\\dock-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u.png",tm.wYear,tm.wMonth,tm.wDay,tm.wHour,tm.wMinute,tm.wSecond,tm.wMilliseconds,GetCurrentProcessId(),i);
        path=folder+name;file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS)break;}
    if(file==INVALID_HANDLE_VALUE){DLOG(L"snapshot png: create failed %lu",GetLastError());return;}
    void* bytes=GlobalLock(memory);DWORD written=0; const DWORD size=stat.cbSize.LowPart;
    bool ok=bytes && WriteFile(file,bytes,size,&written,nullptr) && written==size;
    if(bytes)GlobalUnlock(memory);if(!CloseHandle(file))ok=false;
    DLOG(L"snapshot png: saved=%d path=%ls (DComp backbuffer, not desktop composite)",ok,path.c_str());
}

void Renderer::SaveSnapshot() {
    ComPtr<ID3D11Texture2D> source = SnapshotSource(), staging;
    if (!source) { DLOG(L"snapshot: SDR source unavailable"); return; }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    HRESULT hr = d3d_->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) { DLOG(L"snapshot: staging failed 0x%08X", hr); return; }
    ctx_->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { DLOG(L"snapshot: map failed 0x%08X", hr); return; }
    FILE* file = nullptr;
    bool saved = false;
    if (_wfopen_s(&file, snapshotPath_.c_str(), L"wb") == 0) {
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + desc.Width * desc.Height * 4;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biSizeImage = desc.Width * desc.Height * 4;
        saved = fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(&info, sizeof(info), 1, file) == 1;
        for (UINT y = 0; saved && y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            saved = fwrite(row, 1, desc.Width * 4, file) == desc.Width * 4;
        }
        if (fclose(file) != 0) saved = false;
    }
    ctx_->Unmap(staging.Get(), 0);
    DLOG(L"snapshot: saved=%d path=%ls", saved, snapshotPath_.c_str());
}

bool Renderer::RenderLocalGlass(const LocalGlassOverlay& overlay) {
    if (!overlay.active || !liquid_ || !ctx_ || !rtv_ || !swapChain_ || !d3d_) return false;
    auto& cache=localGlassCaches_[std::clamp(overlay.cacheKey,0,static_cast<int>(localGlassCaches_.size())-1)];
    if(!cache.material){cache.material=CreateLiquidGlassMaterial();if(FAILED(cache.material->CreateGpuResources(d3d_.Get()))){cache.material.reset();return false;}}
    ComPtr<ID3D11Texture2D> source;
    if (output_) source = output_->colors.Texture();
    else if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&source)))) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    const float scale=dpi_/96.f;
    const int margin=static_cast<int>(std::ceil(96.f*scale));
    const int x0=std::max(0,static_cast<int>(std::floor(overlay.sampleBounds.left*scale))-margin);
    const int y0=std::max(0,static_cast<int>(std::floor(overlay.sampleBounds.top*scale))-margin);
    const int x1=std::min(static_cast<int>(desc.Width),static_cast<int>(std::ceil(overlay.sampleBounds.right*scale))+margin);
    const int y1=std::min(static_cast<int>(desc.Height),static_cast<int>(std::ceil(overlay.sampleBounds.bottom*scale))+margin);
    if(x1<=x0||y1<=y0)return false;
    const UINT width=static_cast<UINT>(x1-x0),height=static_cast<UINT>(y1-y0);
    if(!cache.scene||cache.width!=width||cache.height!=height){
        D3D11_TEXTURE2D_DESC copy{};copy.Width=width;copy.Height=height;copy.MipLevels=1;copy.ArraySize=1;
        copy.Format=desc.Format;copy.SampleDesc.Count=1;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> next;if(FAILED(d3d_->CreateTexture2D(&copy,nullptr,&next)))return false;
        cache.scene=next;cache.width=width;cache.height=height;
    }
    ctx_->OMSetRenderTargets(0,nullptr,nullptr);
    const D3D11_BOX box{static_cast<UINT>(x0),static_cast<UINT>(y0),0,static_cast<UINT>(x1),static_cast<UINT>(y1),1};
    ctx_->CopySubresourceRegion(cache.scene.Get(),0,0,0,0,source.Get(),0,&box);
    BackdropFrame frame;frame.texture=cache.scene.Get();frame.screenRect=RECT{x0,y0,x1,y1};frame.frameId=++localSceneFrame_;
    GpuPaint paint;paint.context=ctx_.Get();paint.target=rtv_.Get();paint.targetWidth=targetW_;paint.targetHeight=targetH_;
    paint.windowScreenRect=RECT{0,0,static_cast<LONG>(targetW_),static_cast<LONG>(targetH_)};
    // The Lab draws a 116x50 control then scales it to 40%. Map that
    // virtual coordinate space back to the actual pixel position without
    // changing any optical material settings.
    const float controlScale=std::clamp(overlay.controlScale,.1f,1.f);
    paint.dpiScale=scale*controlScale;
    paint.plateDip=D2D1::RectF(overlay.plate.left/controlScale,overlay.plate.top/controlScale,
                               overlay.plate.right/controlScale,overlay.plate.bottom/controlScale);
    GlassParameters localParams=overlay.params;localParams.radius/=controlScale;
    paint.backdrop=&frame;paint.params=&localParams;
    paint.pointerX=overlay.pointerX;paint.pointerY=overlay.pointerY;paint.press=overlay.press;
    return cache.material->RenderGpu(paint);
}

HRESULT Renderer::Render(const Scene& scene) {
    const double t0 = NowMs();
    timer_.Begin(ctx_.Get());
    uiValid_ = false;  // this frame draws the UI straight into the back buffer; the cached layer may be out of date
    // GPU material first (it writes the whole back buffer), then D2D content on top. If the GPU
    // material is unavailable this frame (no backdrop yet, Smoked selected, failure) the D2D smoke
    // fallback draws the plate instead.
    RenderGlassLayers(scene);
    timer_.Mark(ctx_.Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (!lastUsedGpu_) dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (scene.customDraw) scene.customDraw(dc_.Get(), lastUsedGpu_);
    else DrawScene(scene, !lastUsedGpu_);
    HRESULT hr = dc_->EndDraw();
    return FinishFrame(scene, hr, t0);
}

// ---- Low-latency layering (opt-in, the Dock) -----------------------------------------------------------------------
// The UI (icons, labels' text, indicators, diagnostics) is drawn by D2D into a cached layer before the newest desktop
// frame is taken; when that frame arrives only the GPU glass is drawn and the cached layer is laid over it. Drawing the
// UI cost 1-8 ms per frame, longer than the ~1 ms DWM leaves between handing out a desktop frame and composing the next
// one, so it had to move out of that window (experiments/hostbackdrop-lab/REPORT-dxgi-latency.md).
bool Renderer::CanCompose(const Scene& scene) const {
    return liquid_ && rtv_ && dc_ && scene.glass && scene.glass->material == MaterialKind::Liquid && !scene.customDraw &&
           !scene.localGlass && !scene.chromeGlass && !scene.menuMode && !scene.opaqueFallback && scene.frame;
}

HRESULT Renderer::PrepareUi(const Scene& scene) {
    if (!CanCompose(scene)) { uiValid_ = false; return S_FALSE; }
    const double t0 = NowMs();
    if (!uiBitmap_ || uiW_ != targetW_ || uiH_ != targetH_ || uiDpi_ != dpi_) {
        uiDpi_ = dpi_;
        uiBitmap_.Reset();
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
        HRESULT hr = dc_->CreateBitmap(D2D1::SizeU(std::max<UINT>(1, targetW_), std::max<UINT>(1, targetH_)), nullptr, 0, props, &uiBitmap_);
        if (FAILED(hr)) { uiValid_ = false; return hr; }
        uiW_ = targetW_; uiH_ = targetH_;
    }
    dc_->SetTarget(uiBitmap_.Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    labelUsedGpu_ = true;  // Compose draws the label plate on the GPU; if it cannot, the caller renders the full path
    DrawScene(scene, false);
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(target_.Get());
    uiValid_ = SUCCEEDED(hr);
    lastUiMs_ = static_cast<float>(NowMs() - t0);
    return hr;
}

HRESULT Renderer::Compose(const Scene& scene) {
    if (!uiValid_ || !CanCompose(scene) || !scene.backdrop) return S_FALSE;
    const double t0 = NowMs();
    timer_.Begin(ctx_.Get());
    RenderGlassLayers(scene);
    const bool wantsLabel = scene.labelIndex >= 0 && scene.labelIndex < static_cast<int>(scene.frame->items.size()) &&
                            scene.labelAlpha > 0.01f && !scene.label.empty();
    if (!lastUsedGpu_ || (wantsLabel && !labelUsedGpu_)) { timer_.Mark(ctx_.Get()); timer_.End(ctx_.Get()); return S_FALSE; }
    timer_.Mark(ctx_.Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->DrawBitmap(uiBitmap_.Get(), nullptr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    const HRESULT hr = dc_->EndDraw();
    ++composedFrames_;
    return FinishFrame(scene, hr, t0);
}

HRESULT Renderer::FinishFrame(const Scene& scene, HRESULT hr, double t0) {
    timer_.End(ctx_.Get());
    lastCpuMs_ = static_cast<float>(NowMs() - t0);
    if (FAILED(hr)) return hr;
    return PresentFrame(scene);
}

void Renderer::RenderGlassLayers(const Scene& scene) {
    lastUsedGpu_ = false;
    labelUsedGpu_ = false;
    const GlassParameters* glass = scene.glass;
    GlassParameters lowPowerGlass;
    if (glass && glass->blurDownsample < 4 && sharedGraphicsMemory_) {
        lowPowerGlass = *glass;
        lowPowerGlass.blurDownsample = 4;   // one quarter of the half-resolution blur buffer's pixels
        glass = &lowPowerGlass;
    }
    if (liquid_ && glass && glass->material == MaterialKind::Liquid && scene.backdrop && rtv_) {
        const float clear[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv_.Get(), clear);
        GpuPaint g;
        g.context = ctx_.Get();
        g.target = rtv_.Get();
        g.targetWidth = targetW_;
        g.targetHeight = targetH_;
        g.windowScreenRect = scene.windowScreenRect;
        g.dpiScale = dpi_ / 96.0f;
        g.plateDip = D2D1::RectF(scene.frame->dockLeft, scene.frame->dockTop, scene.frame->dockRight,
                                 scene.frame->dockBottom);
        g.backdrop = scene.backdrop;
        g.params = glass;
        g.pointerX = scene.pointerX;
        g.pointerY = scene.pointerY;
        g.hover = scene.hover;
        g.press = scene.press;
        lastUsedGpu_ = liquid_->RenderGpu(g);
        if (lastUsedGpu_ && !scene.customDraw && scene.labelIndex >= 0 &&
            scene.labelIndex < static_cast<int>(scene.frame->items.size()) &&
            scene.labelAlpha > 0.01f && !scene.label.empty()) {
            D2D1_RECT_F labelBounds{};
            if (LabelBounds(scene, scene.frame->items[scene.labelIndex], labelBounds)) {
                GlassParameters labelGlass = RoleGlass(GlassRole::DockLabel, *glass);
                Materialize(labelGlass, scene.labelAlpha);
                g.plateDip = labelBounds;
                g.params = &labelGlass;
                g.hover = g.press = 0.0f;
                labelUsedGpu_ = liquid_->RenderGpu(g);
            }
        }
    }
}

HRESULT Renderer::PresentFrame(const Scene& scene) {
    HRESULT hr = S_OK;
    if (scene.localGlass && scene.localGlass->active) {
        const bool rendered=RenderLocalGlass(*scene.localGlass);
        static bool logged=false,last=false;if(!logged||last!=rendered){DLOG(L"settings local glass: rendered=%d",rendered);logged=true;last=rendered;}
        const auto& finish=rendered?scene.localGlass->after:scene.localGlass->fallback;
        if(finish){dc_->BeginDraw();dc_->SetTransform(D2D1::Matrix3x2F::Identity());finish(dc_.Get());
            hr=dc_->EndDraw();if(FAILED(hr))return hr;}

    }
    if(scene.chromeGlass) for(const auto& overlay:*scene.chromeGlass) {
        if(!overlay.active)continue;
        const bool rendered=RenderLocalGlass(overlay);
        const auto& finish=rendered?overlay.after:overlay.fallback;
        if(finish){dc_->BeginDraw();dc_->SetTransform(D2D1::Matrix3x2F::Identity());finish(dc_.Get());
            hr=dc_->EndDraw();if(FAILED(hr))return hr;}
    }
    if (pngSnapshotRequested_) SavePngSnapshot();
    if (!snapshotPath_.empty()) {
        SaveSnapshot();
        snapshotPath_.clear();
    }
    if (output_) {
        if (output_->scRgb) {
            const auto monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            const auto now = GetTickCount64();
            if (monitor != output_->monitor || now >= output_->nextWhiteCheck) {
                output_->monitor = monitor;
                output_->white = docky::color::DisplayWhite(monitor);
                output_->nextWhiteCheck = now + 1000;
            }
        }
        if (!output_->colors.Resolve(output_->swapTarget.Get(), output_->scRgb ? output_->white : 0.f)) return E_FAIL;
    }
    ++presentCalls_;
    const double p0 = NowMs();
    const HRESULT presented = swapChain_->Present(syncInterval_, 0);
    lastPresentMs_ = static_cast<float>(NowMs() - p0);
    return presented;
}

HRESULT Renderer::SetRoundedVisualClip(float radiusPx) {
    if (!visual_ || !dcomp_) return E_FAIL;
    if (radiusPx <= 0.f) { HRESULT hr=visual_->SetClip(static_cast<IDCompositionClip*>(nullptr)); return SUCCEEDED(hr)?dcomp_->Commit():hr; }
    ComPtr<IDCompositionRectangleClip> clip;
    HRESULT hr=dcomp_->CreateRectangleClip(&clip); if(FAILED(hr))return hr;
    if(FAILED(hr=clip->SetLeft(0.f)) || FAILED(hr=clip->SetTop(0.f)) ||
       FAILED(hr=clip->SetRight(static_cast<float>(targetW_))) || FAILED(hr=clip->SetBottom(static_cast<float>(targetH_))) ||
       FAILED(hr=clip->SetTopLeftRadiusX(radiusPx)) || FAILED(hr=clip->SetTopLeftRadiusY(radiusPx)) ||
       FAILED(hr=clip->SetTopRightRadiusX(radiusPx)) || FAILED(hr=clip->SetTopRightRadiusY(radiusPx)) ||
       FAILED(hr=clip->SetBottomLeftRadiusX(radiusPx)) || FAILED(hr=clip->SetBottomLeftRadiusY(radiusPx)) ||
       FAILED(hr=clip->SetBottomRightRadiusX(radiusPx)) || FAILED(hr=clip->SetBottomRightRadiusY(radiusPx)) ||
       FAILED(hr=visual_->SetClip(clip.Get()))) return hr;
    return dcomp_->Commit();
}


HRESULT Renderer::SetVisualOpacity(float opacity) {
    if(!visual_||!dcomp_)return E_FAIL;
    if(!opacityEffect_){HRESULT created=dcomp_->CreateEffectGroup(&opacityEffect_);if(FAILED(created))return created;created=visual_->SetEffect(opacityEffect_.Get());if(FAILED(created))return created;}
    HRESULT hr=opacityEffect_->SetOpacity(std::clamp(opacity,0.0f,1.0f));
    return SUCCEEDED(hr)?dcomp_->Commit():hr;
}

HRESULT Renderer::SetVisualOffsetY(float px) {
    if(!visual_||!dcomp_)return E_FAIL;
    HRESULT hr=visual_->SetOffsetY(px);
    return SUCCEEDED(hr)?dcomp_->Commit():hr;
}

bool Renderer::PollGpuTimes(float& frameMs, float& glassMs) {
    return ctx_ && timer_.Poll(ctx_.Get(), frameMs, glassMs);
}

// Continuous-curvature ("squircle") rounded rectangle. Fitted to the macOS plate corner profile:
// the curve starts ~1.14 r from the corner (a circle would start at r) with control points 0.43 r
// from the corner, giving the longer, softer tail measured in the reference captures.
ComPtr<ID2D1PathGeometry> Renderer::ContinuousRect(D2D1_RECT_F r, float radius) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2dFactory_->CreatePathGeometry(&path))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;
    const float w = r.right - r.left, h = r.bottom - r.top;
    const float e = std::min({radius * 1.14f, w * 0.5f, h * 0.5f});
    const float a = e * (0.43f / 1.14f);
    const float L = r.left, T = r.top, R = r.right, B = r.bottom;
    sink->BeginFigure(D2D1::Point2F(L + e, T), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(R - e, T));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(R - a, T), D2D1::Point2F(R, T + a), D2D1::Point2F(R, T + e)));
    sink->AddLine(D2D1::Point2F(R, B - e));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(R, B - a), D2D1::Point2F(R - a, B), D2D1::Point2F(R - e, B)));
    sink->AddLine(D2D1::Point2F(L + e, B));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(L + a, B), D2D1::Point2F(L, B - a), D2D1::Point2F(L, B - e)));
    sink->AddLine(D2D1::Point2F(L, T + e));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(L, T + a), D2D1::Point2F(L + a, T), D2D1::Point2F(L + e, T)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

void Renderer::DrawScene(const Scene& s, bool drawPlate) {
    const DockFrame& f = *s.frame;
    const DockMetrics& m = *s.metrics;
    const float scale = dpi_ / 96.0f;
    const float px = 1.0f / scale;
    const D2D1_RECT_F plate = D2D1::RectF(f.dockLeft, f.dockTop, f.dockRight, f.dockBottom);

    // Plate: geometry is computed here, appearance is delegated to the material.
    const float radius = s.glass ? s.glass->radius : m.radius;
    ComPtr<ID2D1PathGeometry> shape = drawPlate ? ContinuousRect(plate, radius) : nullptr;
    ComPtr<ID2D1PathGeometry> rim, inner;
    if (drawPlate) {
        rim = ContinuousRect(
            D2D1::RectF(plate.left + 0.5f * px, plate.top + 0.5f * px, plate.right - 0.5f * px, plate.bottom - 0.5f * px),
            radius - 0.5f * px);
        inner = ContinuousRect(
            D2D1::RectF(plate.left + 2.0f * px, plate.top + 2.0f * px, plate.right - 2.0f * px, plate.bottom - 2.0f * px),
            radius - 2.0f * px);
    }
    if (shape && s.opaqueFallback) {
        const COLORREF bg=s.opaqueBackground,fg=s.opaqueForeground;
        brush_->SetColor(D2D1::ColorF(GetRValue(bg)/255.f,GetGValue(bg)/255.f,GetBValue(bg)/255.f,1.f));dc_->FillGeometry(shape.Get(),brush_.Get());
        brush_->SetColor(D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f));dc_->DrawGeometry(shape.Get(),brush_.Get(),px);
    } else if (material_ && shape) {
        PlatePaint paint;
        paint.shape = shape.Get();
        paint.rimShape = rim ? rim.Get() : shape.Get();
        paint.innerShape = inner.Get();
        paint.bounds = plate;
        paint.radius = radius;
        paint.dpiScale = scale;
        paint.backdrop = s.backdrop;
        if (!s.menuMode) material_->DrawShadow(dc_.Get(), paint);
        material_->DrawBody(dc_.Get(), paint);
        material_->DrawRim(dc_.Get(), paint);
    }

    if(s.menuMode){
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if(s.menuGridColumns>0){
            // Grid (the Dock's Hidden Icons): app icons in square cells, a soft highlight on the hovered one, a dot under
            // icons kept in the Dock (checked), and the hovered app's name in the footer.
            const COLORREF fg=s.opaqueForeground;const auto ink=D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f);
            auto inkAt=[&](float a){return s.opaqueFallback?D2D1::ColorF(ink.r,ink.g,ink.b,a):D2D1::ColorF(1,1,1,a);};
            IDWriteTextFormat* format=menuRowFormat_?menuRowFormat_.Get():labelFormat_.Get();
            const auto align=format->GetTextAlignment();format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            const int cols=s.menuGridColumns;const float cell=kMenuGridCell,left=(s.windowW-cell*static_cast<float>(cols))*.5f;
            for(size_t i=0;i<s.menuRows.size();++i){const auto& row=s.menuRows[i];if(row.separator)continue;
                const float x=left+cell*static_cast<float>(i%static_cast<size_t>(cols)),y=kMenuGridPad+cell*static_cast<float>(i/static_cast<size_t>(cols));
                const D2D1_POINT_2F c=D2D1::Point2F(x+cell*.5f,y+cell*.5f);
                if(row.hover&&row.enabled){brush_->SetColor(inkAt(s.opaqueFallback?0.14f:0.20f));dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x+2,y+2,x+cell-2,y+cell-2),10,10),brush_.Get());}
                brush_->SetColor(inkAt(row.enabled?0.96f:0.42f));
                ID2D1Bitmap1* bitmap=nullptr;
                if(row.icon&&row.icon->w&&row.icon->h&&row.icon->pixels.size()==static_cast<size_t>(row.icon->w)*row.icon->h){
                    auto found=menuIcons_.find(row.icon.get());
                    if(found==menuIcons_.end()){
                        if(menuIcons_.size()>96)menuIcons_.clear();
                        ComPtr<ID2D1Bitmap1> made;const D2D1_BITMAP_PROPERTIES1 props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.f,96.f);
                        if(SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(row.icon->w,row.icon->h),row.icon->pixels.data(),row.icon->w*4,&props,&made)))found=menuIcons_.emplace(row.icon.get(),std::make_pair(row.icon,made)).first;
                    }
                    if(found!=menuIcons_.end())bitmap=found->second.second.Get();
                }
                if(bitmap){
                    const float side=kMenuGridIcon,iw=static_cast<float>(row.icon->w),ih=static_cast<float>(row.icon->h),k=side/(std::max)(iw,ih);
                    const float w=std::round(iw*k),h=std::round(ih*k),ix=std::round(c.x-w*.5f),iy=std::round(c.y-h*.5f);
                    const D2D1_RECT_F dest=D2D1::RectF(ix,iy,ix+w,iy+h);
                    if(row.icon->templ){const auto mode=dc_->GetAntialiasMode();dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);dc_->FillOpacityMask(bitmap,brush_.Get(),&dest,nullptr);dc_->SetAntialiasMode(mode);}
                    else dc_->DrawBitmap(bitmap,dest,row.enabled?1.f:.5f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr,nullptr);
                }else{  // no artwork: the name's first letter in a rounded outline
                    const float h=kMenuGridIcon*.5f;const D2D1_RECT_F box=D2D1::RectF(c.x-h,c.y-h,c.x+h,c.y+h);
                    dc_->DrawRoundedRectangle(D2D1::RoundedRect(box,6,6),brush_.Get(),1.5f);
                    const std::wstring letter=row.text.empty()?L"?":std::wstring(1,static_cast<wchar_t>(towupper(row.text[0])));
                    dc_->DrawTextW(letter.c_str(),static_cast<UINT32>(letter.size()),format,box,brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if(row.checked){brush_->SetColor(inkAt(0.85f));dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x,y+cell-4.5f),2.f,2.f),brush_.Get());}
            }
            if(!s.menuFooter.empty()){
                brush_->SetColor(inkAt(0.82f));
                const float top=s.windowH-kMenuGridFooter;
                dc_->DrawTextW(s.menuFooter.c_str(),static_cast<UINT32>(s.menuFooter.size()),format,D2D1::RectF(kMenuGridPad,top,s.windowW-kMenuGridPad,s.windowH-6),brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            format->SetTextAlignment(align);
            return;
        }
        for(size_t i=0;i<s.menuRows.size();++i){const auto& row=s.menuRows[i];const float top=6.0f+24.0f*static_cast<float>(i);
            const COLORREF fg=s.opaqueForeground;const auto ink=D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f);
            if(row.separator){brush_->SetColor(s.opaqueFallback?D2D1::ColorF(ink.r,ink.g,ink.b,0.35f):D2D1::ColorF(1,1,1,0.19f));dc_->FillRectangle(D2D1::RectF(14,top+11,s.windowW-14,top+12),brush_.Get());continue;}
            if(row.hover && row.enabled){brush_->SetColor(D2D1::ColorF(0.f,105.f/255.f,249.f/255.f,1.f));dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(6,top,s.windowW-6,top+24),8,8),brush_.Get());}
            brush_->SetColor(s.opaqueFallback&&!(row.hover&&row.enabled)?D2D1::ColorF(ink.r,ink.g,ink.b,row.enabled?1.f:0.55f):D2D1::ColorF(1,1,1,row.enabled?0.96f:0.42f));
            float textLeft=16;
            if(row.icon&&row.icon->w&&row.icon->h&&row.icon->pixels.size()==static_cast<size_t>(row.icon->w)*row.icon->h){
                // Row icons (Hidden Icons, extension menus): 16 DIP, vertically centred, text moves right by 24.
                // Template artwork is recoloured to the row's text colour; colour artwork is drawn as is.
                const auto key=row.icon.get();auto found=menuIcons_.find(key);
                if(found==menuIcons_.end()){
                    if(menuIcons_.size()>96)menuIcons_.clear();
                    const std::vector<uint32_t>& iconPx=row.icon->pixels;
                    ComPtr<ID2D1Bitmap1> bitmap;const D2D1_BITMAP_PROPERTIES1 props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.f,96.f);
                    if(SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(row.icon->w,row.icon->h),iconPx.data(),row.icon->w*4,&props,&bitmap)))found=menuIcons_.emplace(key,std::make_pair(row.icon,bitmap)).first;
                }
                if(found!=menuIcons_.end()){
                    const float side=16.f,iw=static_cast<float>(row.icon->w),ih=static_cast<float>(row.icon->h),k=side/(std::max)(iw,ih);
                    const float w=iw*k,h=ih*k,x=16.f+(side-w)*.5f,y=top+12.f-h*.5f;
                    const float alpha=row.enabled?1.f:.5f;(void)alpha;
                    const D2D1_RECT_F dest=D2D1::RectF(x,y,x+w,y+h);
                    if(row.icon->templ){  // alpha as a mask, filled with the text brush (follows hover and theme)
                        const auto mode=dc_->GetAntialiasMode();dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                        dc_->FillOpacityMask(found->second.second.Get(),brush_.Get(),&dest,nullptr);dc_->SetAntialiasMode(mode);
                    }else dc_->DrawBitmap(found->second.second.Get(),dest,alpha,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,nullptr,nullptr);
                }
                textLeft=40;
            }
            std::wstring text=(row.checked?L"✓  ":L"")+row.text;
            IDWriteTextFormat* format=menuRowFormat_?menuRowFormat_.Get():labelFormat_.Get();
            const float textRight=s.windowW-(row.submenu?30.f:14.f);
            dc_->DrawTextW(text.c_str(),static_cast<UINT32>(text.size()),format,D2D1::RectF(textLeft,top,textRight,top+24),brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if(row.submenu)dc_->DrawTextW(L"›",1,format,D2D1::RectF(s.windowW-26,top,s.windowW-12,top+24),brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }

    // Separators: 1 pt line, icon + 10 tall, ~27% white (measured 77/255 on the 12/255 plate).
    for (const ItemGeom& g : f.items) {
        if (!g.separator) continue;
        const float x = std::round(g.cx * scale) / scale;
        const COLORREF fg=s.opaqueForeground;
        brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f):D2D1::ColorF(1,1,1,0.27f));
        dc_->FillRectangle(D2D1::RectF(x - 0.5f * px, g.lineTop, x + 0.5f * px, g.lineBottom), brush_.Get());
    }

    if (s.showInsertion) {
        const COLORREF fg=s.opaqueForeground;
        brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f):D2D1::ColorF(1,1,1,0.85f));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(s.insertionX-1.0f, f.dockTop+8.0f,
            s.insertionX+1.0f, f.dockBottom-8.0f), 1.0f, 1.0f), brush_.Get());
    }
    // Draw a dragged icon last so it remains above neighbouring icons.
    const size_t iconCount=std::min(f.items.size(),s.icons.size());
    const bool hasDrag=s.dragIndex>=0 && static_cast<size_t>(s.dragIndex)<iconCount;
    for (size_t pass = 0; pass < iconCount; ++pass) {
        const size_t i=hasDrag ? (pass+1==iconCount ? static_cast<size_t>(s.dragIndex) :
            (pass>=static_cast<size_t>(s.dragIndex) ? pass+1 : pass)) : pass;
        const ItemGeom& g = f.items[i];
        const IconVisual& v = s.icons[i];
        if (g.separator) continue;
        const float bodyW = g.width > 0.0f ? g.width : g.size;  // wider than tall only for the status group
        const D2D1_RECT_F dest = D2D1::RectF(g.left, g.top, g.left + bodyW, g.top + g.size);
        if (v.statusCells && v.statusState) {
            dock::statusgroup::Draw(dc_.Get(), dwrite_.Get(), dest, *v.statusCells, *v.statusState, v.statusHover, v.opacity);
            continue;
        }
        if (v.shadow) {
            const float pad = v.shadowPadRatio * g.size;
            const float dy = g.size * 0.04f;
            dc_->DrawBitmap(v.shadow,
                            D2D1::RectF(dest.left - pad, dest.top - pad + dy, dest.right + pad, dest.bottom + pad + dy),
                            0.65f * v.opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        }
        if (v.icon) {
            const float blend = v.alt ? std::clamp(v.altBlend, 0.0f, 1.0f) : 0.0f;
            if (blend < 1.0f) dc_->DrawBitmap(v.icon, dest, v.opacity * (1.0f - blend), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            if (blend > 0.0f) dc_->DrawBitmap(v.alt, dest, v.opacity * blend, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        } else if (!v.external) {
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.30f * v.opacity));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(dest, g.size * 0.22f, g.size * 0.22f), brush_.Get());
        }
        if (v.running) {
            // Four-DIP running indicator, centred slightly higher above the plate bottom.
            const COLORREF fg=s.opaqueForeground;
            brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f):D2D1::ColorF(1,1,1,0.55f));
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.cx, f.dockBottom - 9.0f), 2.0f, 2.0f), brush_.Get());
        }
    }

    if (s.labelIndex >= 0 && s.labelIndex < static_cast<int>(f.items.size()) && s.labelAlpha > 0.01f &&
        !s.label.empty())
        DrawLabel(s, f.items[s.labelIndex]);

    // F2: geometry bounds.
    if (s.showBounds) {
        brush_->SetColor(D2D1::ColorF(1, 0.2f, 0.2f, 0.9f));
        dc_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, s.windowW - 0.5f, s.windowH - 0.5f), brush_.Get(), 1.0f);
        brush_->SetColor(D2D1::ColorF(0.2f, 1, 0.3f, 0.9f));
        dc_->DrawRectangle(plate, brush_.Get(), 1.0f);
        brush_->SetColor(D2D1::ColorF(1, 0.9f, 0.2f, 0.8f));
        for (const ItemGeom& g : f.items) {
            if (g.separator) continue;
            dc_->DrawRectangle(D2D1::RectF(g.left, g.top, g.left + (g.width > 0.0f ? g.width : g.size), g.top + g.size), brush_.Get(), 1.0f);
        }
    }

    // F1: diagnostics overlay (hidden in normal appearance).
    if (s.showDiag && !s.diagText.empty()) {
        brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.72f));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(4, 4, std::min(s.windowW - 4.0f, 560.0f), 64), 6, 6),
                                  brush_.Get());
        brush_->SetColor(D2D1::ColorF(0.6f, 1.0f, 0.6f, 1.0f));
        dc_->DrawTextW(s.diagText.c_str(), static_cast<UINT32>(s.diagText.size()), diagFormat_.Get(),
                       D2D1::RectF(10, 7, std::min(s.windowW - 8.0f, 556.0f), 62), brush_.Get());
    }
}

bool Renderer::LabelBounds(const Scene& s, const ItemGeom& g, D2D1_RECT_F& bounds) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.label.c_str(), static_cast<UINT32>(s.label.size()), labelFormat_.Get(),
                                         600.0f, 40.0f, &layout))) return false;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) return false;
    const float w = std::max(29.0f, tm.widthIncludingTrailingWhitespace + 26.0f);
    const float bottom = g.restTop - 22.0f;
    const float x = std::clamp(g.cx - w * 0.5f, 2.0f, std::max(2.0f, s.windowW - w - 2.0f));
    bounds = D2D1::RectF(x, bottom - 25.0f, x + w, bottom);
    return bounds.top >= 0.0f;
}

// Name label: capsule (25 tall, fully rounded) with a 16x6 pointer whose tip is 16 above the icon,
// measured from the macOS captures. Drawn as one path so the pointer has no seam.
void Renderer::DrawLabel(const Scene& s, const ItemGeom& g) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.label.c_str(), static_cast<UINT32>(s.label.size()), labelFormat_.Get(),
                                         600.0f, 40.0f, &layout)))
        return;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    const float h = 25.0f, r = h * 0.5f, pointerW = 16.0f, pointerH = 6.0f, gapToIcon = 16.0f;
    const float w = std::max(h + 4.0f, tm.widthIncludingTrailingWhitespace + 26.0f);
    const float tipY = g.restTop - gapToIcon;
    const float bottom = tipY - pointerH;
    const float top = bottom - h;
    const float x = std::clamp(g.cx - w * 0.5f, 2.0f, std::max(2.0f, s.windowW - w - 2.0f));
    const float tipX = std::clamp(g.cx, x + r + pointerW * 0.5f, x + w - r - pointerW * 0.5f);
    if (top < 0.0f) return;

    if (labelUsedGpu_) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * s.labelAlpha));
        dc_->DrawTextLayout(D2D1::Point2F(x + (w - tm.widthIncludingTrailingWhitespace) * 0.5f,
                                          top + (h - tm.height) * 0.5f), layout.Get(), brush_.Get());
        return;
    }
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2dFactory_->CreatePathGeometry(&path))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;
    const auto arc = [&](float ex, float ey) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(ex, ey), D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    };
    sink->BeginFigure(D2D1::Point2F(x + r, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w - r, top));
    arc(x + w, top + r);
    arc(x + w - r, bottom);
    sink->AddLine(D2D1::Point2F(tipX + pointerW * 0.5f, bottom));
    sink->AddLine(D2D1::Point2F(tipX, tipY));
    sink->AddLine(D2D1::Point2F(tipX - pointerW * 0.5f, bottom));
    sink->AddLine(D2D1::Point2F(x + r, bottom));
    arc(x, top + r);
    arc(x + r, top);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;

    const float a = s.opaqueFallback ? 1.0f : s.labelAlpha;
    const COLORREF bg=s.opaqueBackground,fg=s.opaqueForeground;
    // Opaque system fallback is independent of the Liquid Glass parameter snapshot.
    brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(bg)/255.f,GetGValue(bg)/255.f,GetBValue(bg)/255.f,1.f):D2D1::ColorF(0.205f,0.205f,0.220f,0.80f*a));
    dc_->FillGeometry(path.Get(), brush_.Get());
    brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f):D2D1::ColorF(1,1,1,0.16f*a));
    dc_->DrawGeometry(path.Get(), brush_.Get(), 96.0f / dpi_);
    brush_->SetColor(s.opaqueFallback?D2D1::ColorF(GetRValue(fg)/255.f,GetGValue(fg)/255.f,GetBValue(fg)/255.f,1.f):D2D1::ColorF(1,1,1,0.96f*a));
    dc_->DrawTextLayout(
        D2D1::Point2F(x + (w - tm.widthIncludingTrailingWhitespace) * 0.5f, top + (h - tm.height) * 0.5f), layout.Get(),
        brush_.Get());
}

}  // namespace dock
