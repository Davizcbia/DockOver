#include "backdrop_wgc.h"
#include "glass_params.h"
#include "log.h"
#include "vblank.h"
#include "roi_compare_cs.h"
#include "capture_normalize_cs.h"
#include "color_output.h"
#include "sdr_dither.h"
#include <DirectXPackedVector.h>
#include "desktop/duplication.h"
#include "capture_visible.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <appmodel.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace dock {
namespace {
// Request from the calling window's initialized apartment; never block its message loop.
// Process-lifetime storage keeps the async operation alive while the Windows prompt is open.
using BorderlessOperation=winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus>;
struct BorderlessRequest {std::atomic<int> status{-1};BorderlessOperation operation{nullptr};};
BorderlessRequest& Borderless(){static auto* state=new BorderlessRequest;return *state;}
std::atomic<int>& BorderlessState(){return Borderless().status;}
void BeginBorderlessRequest(){
    auto& shared=Borderless();int expected=-1;
    if(!shared.status.compare_exchange_strong(expected,0))return;
    UINT32 length=0;const LONG identity=GetCurrentPackageFullName(&length,nullptr);
    if(identity!=ERROR_INSUFFICIENT_BUFFER){shared.status.store(3);DLOG(L"wgc borderless: no package identity; borderless WGC unavailable");return;}
    try {
        DLOG(L"wgc borderless: requesting consent on capture window apartment");
        shared.operation=GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
        shared.operation.Completed([](auto const& operation,winrt::Windows::Foundation::AsyncStatus status) noexcept {
            auto& result=BorderlessState();
            try {
                if(status!=winrt::Windows::Foundation::AsyncStatus::Completed){DLOG(L"wgc borderless: consent operation status %d error 0x%08X",static_cast<int>(status),static_cast<unsigned>(operation.ErrorCode().value));result.store(3);return;}
                const auto access=operation.GetResults();
                using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
                DLOG(L"wgc borderless: consent result %d",static_cast<int>(access));
                result.store(access==AppCapabilityAccessStatus::Allowed?1:2);
            } catch(const winrt::hresult_error& e){DLOG(L"wgc borderless: completion failed 0x%08X",static_cast<unsigned>(e.code().value));result.store(3);}
              catch(...){DLOG(L"wgc borderless: completion unavailable");result.store(3);}
        });
    } catch(const winrt::hresult_error& e){DLOG(L"wgc borderless: request failed 0x%08X",static_cast<unsigned>(e.code().value));shared.status.store(3);}
      catch(...){DLOG(L"wgc borderless: request unavailable");shared.status.store(3);}
}
}
bool DuplicationAllowed(){wchar_t v[16]{};return !(GetEnvironmentVariableW(L"DOCKY_CAPTURE",v,16)&&_wcsicmp(v,L"wgc")==0);}
void PrewarmBorderlessCapture(){BeginBorderlessRequest();}
bool BorderlessCaptureGranted(){return BorderlessState().load()==1;}
bool PrepareCleanCaptureSession(const GraphicsCaptureSession& session){
    if(!BorderlessCaptureGranted())return false;
    try {
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if(!ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsBorderRequired") ||
           !ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsCursorCaptureEnabled"))return false;
        session.IsBorderRequired(false);session.IsCursorCaptureEnabled(false);
        if(session.IsBorderRequired()||session.IsCursorCaptureEnabled())return false;
        DLOG(L"wgc: clean session verified (border=0 cursor=0)");return true;
    } catch(const winrt::hresult_error& e){DLOG(L"wgc: clean session refused 0x%08X",static_cast<unsigned>(e.code().value));return false;}
}
WgcBackdropSource::~WgcBackdropSource() { Stop(); if (gpuEvent_) CloseHandle(gpuEvent_); if (retryTimer_) CloseHandle(retryTimer_); }

void WgcBackdropSource::SetDevice(ID3D11Device* device) {
    Stop();
    device_ = device;
    context_.Reset();
    if (device_) { device_->GetImmediateContext(&context_); InitCompare(); }
    else { context4_.Reset(); fence_.Reset(); compareShader_.Reset(); result_.Reset(); resultReadback_.Reset(); resultUav_.Reset(); }
    monitor_ = nullptr;
}

void WgcBackdropSource::Request(bool enabled, HMONITOR monitor) {
    if (!enabled) {
        if (monitor_ || state_) Stop();
        monitor_ = nullptr;
        state_ = 0;
        return;
    }
    if (monitor == monitor_) return;
    Stop();
    monitor_ = monitor;
    StartOrWait(monitor);
}

void WgcBackdropSource::Fail(const wchar_t* reason) {
    DLOG(L"wgc: %ls; fallback", reason);
    Stop();
    state_ = 3;
    failedAt_ = std::chrono::steady_clock::now();
}

bool WgcBackdropSource::CheckHealth() {
    const auto now = std::chrono::steady_clock::now();
    if (state_ == 4 && monitor_) {
        if (BorderlessState().load() == 0 && now - consentWaitSince_ < std::chrono::seconds(3)) return false;
        state_ = Start(monitor_, false) ? 1 : 3;
        if (state_ == 3) failedAt_ = now;
        return true;
    }
    if ((state_ == 1 || state_ == 2) && monitor_ && !dxgi_ && startedCap_ != docky::vblank::CaptureCap()) {  // capture cap changed
        DLOG(L"wgc: capture cap %d -> %d fps; restarting capture", startedCap_, docky::vblank::CaptureCap());
        const auto monitor = monitor_; Stop(); state_ = Start(monitor, false) ? 1 : 3;
        if (state_ == 3) failedAt_ = now;
        return true;
    }
    if (state_ == 1 && now - startedAt_ > std::chrono::seconds(3)) {
        if (dxgi_) {
            const auto monitor = monitor_;
            DLOG(L"capture: DXGI first frame timeout; trying borderless WGC");
            Stop();
            StartOrWait(monitor, false);
        } else Fail(L"first frame timeout");
        return true;
    }
    if (state_ == 3 && monitor_ && device_ && now - failedAt_ > std::chrono::seconds(5)) {
        DLOG(L"wgc: retry after failure");
        StartOrWait(monitor_);
        return true;
    }
    return false;
}

int WgcBackdropSource::ProbeOffset() {
    static const int offset = [] {
        wchar_t v[16]{};
        if (!wcsstr(GetCommandLineW(), L"--dev-isolated") || !GetEnvironmentVariableW(L"DOCKY_LATENCY_PROBE_OFFSET", v, 16)) return 0;
        const int px = _wtoi(v);
        if (px) DLOG(L"capture: latency probe mode, sampling %d px to the left, window visible to capture (dev only)", px);
        return px;
    }();
    return offset;
}

bool WgcBackdropSource::StartDuplication(HMONITOR monitor) {
    // The requesting window must not see itself: duplication honours WDA_EXCLUDEFROMCAPTURE like WGC.
    DWORD affinity = 0;
    if (ProbeOffset()) SetWindowDisplayAffinity(hwnd_, WDA_NONE);  // dev probe: samples elsewhere, so it never sees itself
    else if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE) || !GetWindowDisplayAffinity(hwnd_, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE) {
        DLOG(L"capture: window exclusion failed; no desktop duplication");
        return false;
    }
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;
    auto& d = docky::desk::SharedDesktop::ForMonitor(monitor);
    d.AddUser();
    if (!d.Want(info.rcMonitor)) {
        d.RemoveUser();
        DLOG(L"capture: desktop duplication unavailable for (%ld,%ld)-(%ld,%ld)", info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom);
        return false;
    }
    dxgi_ = &d;
    monitorRect_ = d.OutputRect();
    dxgiView_.Reset(); dxgiGeneration_ = 0; dxgiSeen_ = 0;
    RECT own{}; GetWindowRect(hwnd_, &own);
    d.Listen(this, hwnd_, kMsgCaptureAvailable, monitorRect_, own);
    if (d.Valid()) PostMessageW(hwnd_, kMsgCaptureAvailable, 0, 0);  // another backdrop already runs it: copy now
    startedAt_ = std::chrono::steady_clock::now();
    startedCap_ = docky::vblank::CaptureCap();
    DLOG(L"capture: desktop duplication of monitor (%ld,%ld)-(%ld,%ld) (no capture frame)", monitorRect_.left, monitorRect_.top, monitorRect_.right, monitorRect_.bottom);
    return true;
}

void WgcBackdropSource::StartOrWait(HMONITOR monitor, bool tryDuplication) {
    if (Start(monitor, tryDuplication)) {
        state_ = 1;
    } else if (BorderlessState().load() == 0) {
        state_ = 4;
        consentWaitSince_ = std::chrono::steady_clock::now();
    } else {
        state_ = 3;
        failedAt_ = std::chrono::steady_clock::now();
    }
}

bool WgcBackdropSource::Start(HMONITOR monitor, bool tryDuplication) {
    if (!hwnd_ || !device_ || !monitor) return false;
    if (tryDuplication && !sourceWindow_ && DuplicationAllowed() && StartDuplication(monitor)) return true;
    // WGC draws Windows' yellow frame around the screen or window unless borderless capture is granted (package identity
    // and the user's consent). Never show it: without the grant there is no live capture.
    BeginBorderlessRequest();
    const int borderless = BorderlessState().load();
    if (borderless == 0) return false;
    if (borderless != 1) {
        if (!wgcRefusedLogged_) DLOG(L"wgc: borderless capture not granted; live capture off (no yellow frame)");
        wgcRefusedLogged_ = true;
        return false;
    }
    wchar_t fault[64]{};
    GetEnvironmentVariableW(L"DOCK_WGC_TEST_FAULT", fault, 64);
    SetEnvironmentVariableW(L"DOCK_WGC_TEST_FAULT", nullptr);
    testFault_ = fault;
    try {
        if (testFault_ == L"start") throw hresult_error(E_FAIL, L"Injected startup failure");
        // Capturing a whole monitor must leave the requesting window out (it would see itself). A single source window
        // (the wallpaper layer) cannot contain it, so it stays visible to screenshots then.
        DWORD affinity = 0;
        if (sourceWindow_) SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        else if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE) ||
            !GetWindowDisplayAffinity(hwnd_, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE)
            throw hresult_error(E_FAIL, L"Dock capture exclusion failed");
        if (!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_NOTIMPL);
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (sourceWindow_) {  // one window (and its children); frames start at the window's top-left corner
            if (!IsWindow(sourceWindow_) || !GetWindowRect(sourceWindow_, &monitorRect_)) throw hresult_error(E_INVALIDARG, L"capture window gone");
            check_hresult(interop->CreateForWindow(sourceWindow_, guid_of<GraphicsCaptureItem>(), put_abi(item_)));
        } else {
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            monitorRect_ = info.rcMonitor;
            check_hresult(interop->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item_)));
        }
        ComPtr<IDXGIDevice> dxgi;
        check_hresult(device_.As(&dxgi));
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        runtimeDevice_ = inspectable.as<IDirect3DDevice>();
        UINT formatSupport = 0;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16;
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        floatingCapture_ = SUCCEEDED(device_->CheckFormatSupport(docky::color::SceneFormat, &formatSupport)) &&
            (formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
            SUCCEEDED(device_->CreateComputeShader(g_CaptureNormalizeCS, sizeof(g_CaptureNormalizeCS), nullptr, &normalizeShader_)) &&
            SUCCEEDED(device_->CreateBuffer(&constants, nullptr, &normalizeConstants_));
        if (floatingCapture_) {
            try {
                pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice_,
                    DirectXPixelFormat::R16G16B16A16Float, 3, item_.Size());
            } catch (const hresult_error&) {
                floatingCapture_ = false;
            }
        }
        if (!floatingCapture_) pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice_,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, item_.Size());
        nextWhiteCheck_ = 0;
        DLOG(L"wgc colour: %ls capture", floatingCapture_ ? L"FP16 scRGB" : L"BGRA8 SDR");
        pending_ = std::make_shared<Pending>();
        pending_->notify = hwnd_;
        pending_->suppressFrames = testFault_ == L"timeout";
        pending_->closeAfterFrame = testFault_ == L"closed";
        pending_->inspectDirty = diagnostics_ &&
            winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.Direct3D11CaptureFrame", L"DirtyRegions");
        std::weak_ptr<Pending> weak = pending_;
        closedToken_ = item_.Closed([weak](auto&&, auto&&) noexcept {
            if (auto p = weak.lock()) {
                p->closed = true;
                PostMessageW(p->notify, kMsgCaptureAvailable, 0, 0);
            }
        });
        frameToken_ = pool_.FrameArrived([weak](auto const& pool, auto&&) noexcept {
            auto p = weak.lock();
            if (!p || !p->active) return;
            ++p->callbacks;
            try {
                if (p->suppressFrames) return;
                auto frame = pool.TryGetNextFrame();
                if (!frame) return;
                const uint64_t acquired = ++p->acquired;
                if (p->closeAfterFrame && acquired > 3) {
                    p->closed = true;
                    PostMessageW(p->notify, kMsgCaptureAvailable, 0, 0);
                    return;
                }
                auto dirty = p->inspectDirty ? frame.DirtyRegions() : nullptr;
                if (p->inspectDirty) {
                    ++p->dirtyFrames;
                    if (!dirty || dirty.Size() == 0) ++p->emptyDirtyFrames;
                }
                std::unique_lock<std::mutex> lock(p->mutex);
                if (!p->active || acquired <= p->sequence) return;
                bool intersects = false;
                if (p->inspectDirty && p->roiValid) {
                    ++p->roiDirtyChecked;
                    if (dirty) for (const auto& r : dirty) {
                        RECT rect{r.X, r.Y, r.X + r.Width, r.Y + r.Height};
                        RECT intersection{};
                        if (IntersectRect(&intersection, &rect, &p->roi)) { intersects = true; break; }
                    }
                    if (intersects) ++p->roiDirtyIntersections;                }
                // V0.16: skip the main-thread wake-up only when the OS reported concrete dirty rectangles and none of
                // them touches the Dock's ROI. The frame is still stored, so any later pull (ROI change, next relevant
                // frame) uses the newest capture; missing/empty dirty data always takes the conservative path.
                const bool provablyOutside = p->skipOutsideRoi && p->roiValid && dirty && dirty.Size() > 0 && !intersects;
                p->frame = frame;
                p->sequence = acquired;
                p->frameDirtyHit = p->inspectDirty && p->roiValid && dirty && dirty.Size() > 0 && intersects;
                lock.unlock();
                if (provablyOutside) { ++p->skippedOutsideRoi; return; }
                if (!p->posted.exchange(true)) PostMessageW(p->notify, kMsgCaptureAvailable, 0, 0);
            } catch (...) {
                p->closed = true;
                PostMessageW(p->notify, kMsgCaptureAvailable, 0, 0);
            }
        });
        session_ = pool_.CreateCaptureSession(item_);
        if(!PrepareCleanCaptureSession(session_))throw hresult_error(E_ACCESSDENIED,L"Cursorless borderless session unavailable");
        // V0.16: ask the OS for dirty rectangles (Windows 11 24H2+) so unrelated screen updates do not wake the Dock.
        wchar_t skipEnv[8]{};  // LIQUID_WGC_DIRTY_SKIP=0 restores the pre-V0.16 behaviour (A/B measurement, fallback)
        const bool skipAllowed = !(GetEnvironmentVariableW(L"LIQUID_WGC_DIRTY_SKIP", skipEnv, 8) && skipEnv[0] == L'0');
        if (skipAllowed && testFault_.empty() && winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"DirtyRegionMode")) {
            try {
                session_.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly);
                pending_->inspectDirty = pending_->skipOutsideRoi = true;
                DLOG(L"wgc: dirty-region reporting on; frames outside the Dock ROI skip processing");
            } catch (...) { DLOG(L"wgc: dirty-region reporting unavailable; processing every frame"); }
        }
        // Capture cadence: the 2026-11-10 log shows publishes every 16-20 ms (about 60 fps) on a 240 Hz display,
        // so content moving behind the Dock (a dragged window) updated only every ~4 refreshes. Newer Windows
        // exposes MinUpdateInterval; its default is not documented, so request the minimum and log both values.
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
            try {
                const auto before = session_.MinUpdateInterval();
                // Settings > General > Frame rate > capture: at most that many desktop frames per second (1 ms = no cap).
                startedCap_ = docky::vblank::CaptureCap();
                const auto interval = startedCap_ > 0 ? std::chrono::microseconds(1000000 / startedCap_) : std::chrono::microseconds(1000);
                session_.MinUpdateInterval(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(interval));
                DLOG(L"wgc: MinUpdateInterval %.2f ms -> %.2f ms",
                     std::chrono::duration<double, std::milli>(before).count(),
                     std::chrono::duration<double, std::milli>(session_.MinUpdateInterval()).count());
            } catch (const winrt::hresult_error& e) { DLOG(L"wgc: MinUpdateInterval unavailable 0x%08X", static_cast<unsigned>(e.code().value)); }
        } else DLOG(L"wgc: MinUpdateInterval not supported on this Windows build");
        session_.StartCapture();
        startedAt_ = std::chrono::steady_clock::now();
        DLOG(sourceWindow_ ? L"wgc: started window (%ld,%ld)-(%ld,%ld), cursor disabled" : L"wgc: started monitor (%ld,%ld)-(%ld,%ld), cursor disabled",
             monitorRect_.left, monitorRect_.top, monitorRect_.right, monitorRect_.bottom);
        return true;
    } catch (const hresult_error& e) {
        DLOG(L"wgc: start failed 0x%08X", static_cast<unsigned>(e.code().value));
        Stop();
        return false;
    }
}

void WgcBackdropSource::NotePublished() {
    LARGE_INTEGER now{}, f{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    const double ms = (static_cast<double>(now.QuadPart) / f.QuadPart - publishedTime_) * 1000.0;
    if (publishedTime_ > 0 && ms >= 0 && ms < 5000) { latencySum_ += ms; latencyMax_ = (std::max)(latencyMax_, ms); ++latencyCount_; }
    DLOG_DEBUG(L"capture latency sample %.2f ms", ms);
}

void WgcBackdropSource::Stop() {
    if (latencyCount_) DLOG(L"capture latency (%ls): %llu publishes, %.2f ms avg, %.2f ms max from the captured frame to publication",
                            dxgi_ ? L"duplication" : L"wgc", static_cast<unsigned long long>(latencyCount_), latencySum_ / latencyCount_, latencyMax_);
    latencySum_ = latencyMax_ = 0; latencyCount_ = 0;
    if (dxgi_) {
        DLOG(L"capture: desktop duplication consumer stopped after %llu copies (%llu published)", static_cast<unsigned long long>(dxgiCopies_), static_cast<unsigned long long>(publishedCount_));
        dxgi_->Unlisten(this); dxgi_->RemoveUser(); dxgi_ = nullptr; dxgiView_.Reset(); dxgiSeen_ = 0; dxgiCopies_ = 0;
    }
    if (pending_) DLOG(L"wgc skip: frames_outside_roi_skipped=%llu", static_cast<unsigned long long>(pending_->skippedOutsideRoi.load()));
    if (pending_) DLOG(L"wgc stats: callbacks=%llu acquired=%llu last_sequence=%llu roi_samples=%llu roi_changes=%llu dirty_reports=%llu empty_dirty=%llu roi_dirty_checked=%llu roi_dirty_intersect=%llu; displayed frames not measured",
        static_cast<unsigned long long>(pending_->callbacks.load()),
        static_cast<unsigned long long>(pending_->acquired.load()),
        static_cast<unsigned long long>(copiedId_),
        static_cast<unsigned long long>(diagnosticSamples_),
        static_cast<unsigned long long>(diagnosticChanges_),
        static_cast<unsigned long long>(pending_->dirtyFrames.load()),
        static_cast<unsigned long long>(pending_->emptyDirtyFrames.load()),
        static_cast<unsigned long long>(pending_->roiDirtyChecked.load()),
        static_cast<unsigned long long>(pending_->roiDirtyIntersections.load()));
    DLOG(L"gpu compare: submitted=%llu published=%llu avg_completion_ms=%.3f max_completion_ms=%.3f",
        static_cast<unsigned long long>(compareCount_), static_cast<unsigned long long>(publishedCount_),
        compareCount_ ? compareTotalMs_ / compareCount_ : 0.0, compareMaxMs_);
    inFlight_ = false; retryMap_ = false;
    if (retryTimer_) CancelWaitableTimer(retryTimer_);
    if (gpuEvent_) ResetEvent(gpuEvent_);
    candidate_.Reset();
    normalizeShader_.Reset();
    normalizeConstants_.Reset();
    floatingCapture_ = false;
    if (pending_) pending_->active = false;
    try { if (pool_ && pending_) pool_.FrameArrived(frameToken_); } catch (...) {}
    try { if (item_ && pending_) item_.Closed(closedToken_); } catch (...) {}
    current_ = nullptr;
    try { if (session_) session_.Close(); } catch (...) {}
    try { if (pool_) pool_.Close(); } catch (...) {}
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    runtimeDevice_ = nullptr;
    pending_.reset();
    texture_.Reset();
    copiedRect_ = {};
    previousRect_ = {};
    previousPixels_.clear();
    diagnosticSamples_ = diagnosticChanges_ = 0;
    currentId_ = copiedId_ = candidateId_ = 0;
    if (hwnd_ && IsWindow(hwnd_)) SetWindowDisplayAffinity(hwnd_, WDA_NONE);
}

bool WgcBackdropSource::CompareRoi(ID3D11Texture2D* source, bool commit) {
    if (!source) source = texture_.Get();
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check_hresult(device_->CreateTexture2D(&desc, nullptr, &staging));
    context_->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const bool sdrBitmap = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const size_t rowBytes = static_cast<size_t>(desc.Width) * (sdrBitmap ? 4 : 8);
    const size_t bytes = rowBytes * desc.Height;
    const bool roiChanged = !EqualRect(&previousRect_, &copiedRect_);
    bool changed = previousPixels_.size() != bytes || roiChanged;
    if (previousPixels_.size() != bytes) previousPixels_.resize(bytes);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* src = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        auto* dst = previousPixels_.data() + y * rowBytes;
        if (std::memcmp(src, dst, rowBytes) != 0) changed = true;
        if (commit) std::memcpy(dst, src, rowBytes);
    }
    context_->Unmap(staging.Get(), 0);
    if (commit) previousRect_ = copiedRect_;
    if (commit && (diagnosticSamples_ == 0 || roiChanged)) {
        wchar_t path[32768]{};
        DWORD n = GetEnvironmentVariableW(L"DOCK_WGC_ROI_DUMP", path, 32768);
        if (n && n < 32768) {
            FILE* file = nullptr;
            bool saved = false;
            if (_wfopen_s(&file, path, L"wb") == 0) {
                const size_t bmpBytes = static_cast<size_t>(desc.Width) * desc.Height * 4;
                const auto* pixels = previousPixels_.data();
                std::vector<unsigned char> sdrPixels;
                if (!sdrBitmap) {
                    sdrPixels.resize(bmpBytes);
                    for (UINT y = 0; y < desc.Height; ++y) {
                        for (UINT x = 0; x < desc.Width; ++x) {
                            const auto offset = static_cast<size_t>(y) * desc.Width + x;
                            DirectX::PackedVector::HALF half[4]{};
                            std::memcpy(half, pixels + offset * sizeof(half), sizeof(half));
                            for (unsigned c = 0; c < 3; ++c) {
                                const float level = DirectX::PackedVector::XMConvertHalfToFloat(half[2 - c]) * 255.f;
                                sdrPixels[offset * 4 + c] = static_cast<unsigned char>(docky::color::Quantize8(level, x, y));
                            }
                            sdrPixels[offset * 4 + 3] = 255;
                        }
                    }
                    pixels = sdrPixels.data();
                }
                BITMAPFILEHEADER header{};
                BITMAPINFOHEADER info{};
                header.bfType = 0x4D42;
                header.bfOffBits = sizeof(header) + sizeof(info);
                header.bfSize = header.bfOffBits + static_cast<DWORD>(bmpBytes);
                info.biSize = sizeof(info);
                info.biWidth = static_cast<LONG>(desc.Width);
                info.biHeight = -static_cast<LONG>(desc.Height);
                info.biPlanes = 1;
                info.biBitCount = 32;
                info.biSizeImage = static_cast<DWORD>(bmpBytes);
                saved = fwrite(&header, sizeof(header), 1, file) == 1 &&
                    fwrite(&info, sizeof(info), 1, file) == 1 &&
                    fwrite(pixels, 1, bmpBytes, file) == bmpBytes;
                if (fclose(file) != 0) saved = false;
            }
            DLOG(L"wgc compare: ROI bitmap saved=%d path=%ls", saved, path);
        }
    }
    ++diagnosticSamples_;
    if (changed) ++diagnosticChanges_;
    DLOG(L"wgc compare: sequence=%llu qpc_seconds=%.7f exact_roi_changed=%d sample=%llu",
        static_cast<unsigned long long>(source == texture_.Get() ? copiedId_ : candidateId_), capturedAt_, changed,
        static_cast<unsigned long long>(diagnosticSamples_));
    return changed;
}

void WgcBackdropSource::InitCompare() {
    context4_.Reset(); fence_.Reset(); compareShader_.Reset(); result_.Reset(); resultReadback_.Reset(); resultUav_.Reset();
    if (!gpuEvent_) gpuEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!retryTimer_) retryTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    ComPtr<ID3D11Device5> device5;
    if (!gpuEvent_ || !retryTimer_ || FAILED(context_.As(&context4_)) || FAILED(device_.As(&device5)) ||
        FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return;
    fenceValue_ = 0;
    HRESULT hr = device_->CreateComputeShader(g_RoiCompareCS, sizeof(g_RoiCompareCS), nullptr, &compareShader_);
    D3D11_BUFFER_DESC b{}; b.ByteWidth = 4; b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_UNORDERED_ACCESS; b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&b, nullptr, &result_);
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = 1; u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(result_.Get(), &u, &resultUav_);
    b.Usage = D3D11_USAGE_STAGING; b.BindFlags = 0; b.CPUAccessFlags = D3D11_CPU_ACCESS_READ; b.MiscFlags = 0;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&b, nullptr, &resultReadback_);
    if (FAILED(hr)) { DLOG(L"gpu compare init failed 0x%08X", hr); compareShader_.Reset(); }
}

void WgcBackdropSource::SubmitCompare(UINT width, UINT height) {
    ComPtr<ID3D11ShaderResourceView> a, b;
    check_hresult(device_->CreateShaderResourceView(texture_.Get(), nullptr, &a));
    check_hresult(device_->CreateShaderResourceView(candidate_.Get(), nullptr, &b));
    const UINT zero[4]{}; context_->ClearUnorderedAccessViewUint(resultUav_.Get(), zero);
    ID3D11ShaderResourceView* views[] = {a.Get(), b.Get()};
    ID3D11UnorderedAccessView* uav = resultUav_.Get();
    context_->CSSetShader(compareShader_.Get(), nullptr, 0);
    context_->CSSetShaderResources(0, 2, views);
    context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context_->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    ID3D11ShaderResourceView* none[2]{}; ID3D11UnorderedAccessView* noUav = nullptr;
    context_->CSSetShaderResources(0, 2, none); context_->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
    context_->CopyResource(resultReadback_.Get(), result_.Get());
    ResetEvent(gpuEvent_); retryMap_ = false; mapRetries_ = 0; inFlight_ = true; ++compareCount_;
    compareStart_ = std::chrono::steady_clock::now();
    ++fenceValue_;
    check_hresult(context4_->Signal(fence_.Get(), fenceValue_));
    check_hresult(fence_->SetEventOnCompletion(fenceValue_, gpuEvent_));
    context_->Flush();
}

bool WgcBackdropSource::ProcessDuplication(const RECT& wantedIn) {
    if (!device_ || !context_) return false;
    RECT wanted = wantedIn; OffsetRect(&wanted, -ProbeOffset(), 0);  // dev probe only (0 otherwise)
    if (Log::Get().Enabled(LogLevel::Debug)) { LARGE_INTEGER t{}, f{}; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        if (const int64_t q = dxgi_->FrameQpc()) DLOG_DEBUG(L"capture wake %.2f ms after the frame", (t.QuadPart - q) * 1000.0 / f.QuadPart); }
    if (FAILED(device_->GetDeviceRemovedReason())) { Fail(L"device lost"); return true; }
    auto& d = *dxgi_;
    const RECT output = d.OutputRect();
    if (!IsRectEmpty(&output) && !EqualRect(&output, &monitorRect_)) {  // display mode changed: start over
        monitorRect_ = output; texture_.Reset(); copiedRect_ = {}; dxgiSeen_ = 0;
    }
    const LONG m = static_cast<LONG>(std::ceil(96.0 * GetDpiForWindow(hwnd_) / 96.0));
    RECT e{wanted.left - m, wanted.top - m, wanted.right + m, wanted.bottom + m}, roi{}, own{};
    GetWindowRect(hwnd_, &own);
    const bool hasRoi = IntersectRect(&roi, &e, &monitorRect_) != FALSE;
    d.Listen(this, hwnd_, kMsgCaptureAvailable, hasRoi ? roi : RECT{}, own);  // re-arm the wake-up for this area
    // A real change behind the window (kind 2) while a GPU comparison of an own-window redraw is in flight: drop the
    // comparison (it only decided whether the older image was worth publishing) and take the newer frame now. Waiting
    // for it delayed live wallpaper frames by ~2 ms on average, up to ~50 ms (measured 2026-11-20). Only changes outside
    // the window can cut in, so the window's own redraws still cannot drive each other.
    if (inFlight_ && hasRoi && d.Valid() && d.ChangedKind(roi, dxgiSeen_, own) == 2) {
        inFlight_ = false; retryMap_ = false; candidate_.Reset();
        if (retryTimer_) CancelWaitableTimer(retryTimer_);
        if (gpuEvent_) ResetEvent(gpuEvent_);
    }
    if (!hasRoi || inFlight_ || !d.Valid() || !d.Fresh(roi)) return false;  // Fresh: wait for the full copy (SharedDesktop)
    if (!dxgiView_ || dxgiGeneration_ != d.Generation()) {
        dxgiView_.Reset();
        const HANDLE shared = d.SharedHandle();
        if (!shared || FAILED(device_->OpenSharedResource(shared, IID_PPV_ARGS(&dxgiView_)))) { dxgiView_.Reset(); return false; }
        dxgiGeneration_ = d.Generation();
        texture_.Reset(); copiedRect_ = {};
    }
    const bool moved = !texture_ || !EqualRect(&roi, &copiedRect_);
    const uint64_t serial = d.Serial();  // before the copy: a change during it is caught next time
    int kind = moved ? 2 : d.ChangedKind(roi, dxgiSeen_, own);
    if (kind && ProbeOffset()) kind = 2;  // dev probe: the window is not excluded, so "own window" changes are real
    dxgiSeen_ = serial;
    if (kind == 0) return false;
    try {
        const UINT width = roi.right - roi.left, height = roi.bottom - roi.top;
        D3D11_TEXTURE2D_DESC shared{};
        dxgiView_->GetDesc(&shared);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = shared.Format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        candidate_.Reset(); check_hresult(device_->CreateTexture2D(&desc, nullptr, &candidate_));
        const D3D11_BOX box{static_cast<UINT>(roi.left - monitorRect_.left), static_cast<UINT>(roi.top - monitorRect_.top), 0,
                            static_cast<UINT>(roi.right - monitorRect_.left), static_cast<UINT>(roi.bottom - monitorRect_.top), 1};
        context_->CopySubresourceRegion(candidate_.Get(), 0, 0, 0, 0, dxgiView_.Get(), 0, &box);
        ++dxgiCopies_;
        LARGE_INTEGER now{}, freq{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
        candidateRect_ = roi; candidateId_ = serial;
        const int64_t frameQpc = d.FrameQpc();  // when the newest desktop frame was taken
        candidateTime_ = static_cast<double>(frameQpc ? frameQpc : now.QuadPart) / freq.QuadPart;
        candidateLp_ = d.LastPresentQpc();
        // New area or a real change: publish now. A change only inside windows excluded from capture (this one or another
        // docky window: Windows reports their redraws although the image behind them is the same) is compared on the GPU
        // first and published only when the pixels changed; publishing those directly made two docky windows redraw each
        // other at the display rate (measured 2026-11-13).
        // Only inside this window, but it has not drawn for about 2.5 display frames: it is not its own redraw (which
        // Windows reports within a frame or two of the Present), so it is content behind it. 2.5 frames + 4 ms: 14 ms at
        // 240 Hz, 35 ms (the cap) at 60 Hz. Kind 3 (inside another excluded window, e.g. the animating Dock under Control
        // Center) always compares: publishing it unchecked redrew Control Center ~64 times a second while the Dock animated,
        // and that panel's separate GPU work cost the Dock about a quarter of its frames (measured 2026-11-18).
        const double ownWindow = (std::min)(0.035, 2.5 * docky::vblank::FramePeriod().load() + 0.004);
        const bool drewRecently = lastDrawQpc_ && static_cast<double>(now.QuadPart - lastDrawQpc_) < ownWindow * freq.QuadPart;
        if (moved || kind == 2 || (kind == 1 && !drewRecently) || !compareShader_ || !context4_ || !fence_) {
            texture_ = candidate_; candidate_.Reset(); copiedRect_ = roi; copiedId_ = candidateId_; publishedTime_ = candidateTime_; publishedLp_ = candidateLp_;
            ++contentVersion_; ++publishedCount_; state_ = 2; NotePublished();
            DLOG_DEBUG(L"capture: duplication publish content=%llu roi=(%ld,%ld)-(%ld,%ld)", static_cast<unsigned long long>(contentVersion_), roi.left, roi.top, roi.right, roi.bottom);
            return true;
        }
        SubmitCompare(width, height);
        return false;
    } catch (const hresult_error& e) { DLOG(L"capture: duplication copy failed 0x%08X", static_cast<unsigned>(e.code().value)); Fail(L"duplication copy error"); return true; }
}

bool WgcBackdropSource::ProcessCapture(const RECT& wanted) {
    if (dxgi_) return ProcessDuplication(wanted);  // frozen screenshots: SharedDesktop holds its frames
    if (!pending_ || !device_ || !context_) return false;
    if (docky::capture_visible::Frozen()) {  // glass visible to a screenshot: drop frames that may contain it
        std::lock_guard<std::mutex> lock(pending_->mutex); pending_->frame = nullptr; pending_->posted = false;
        return false;
    }
    if (pending_->closed || FAILED(device_->GetDeviceRemovedReason())) { Fail(L"capture closed or device lost"); return true; }
    {
        std::unique_lock<std::mutex> lock(pending_->mutex, std::try_to_lock);
        if (!lock) { PostMessageW(hwnd_, kMsgCaptureAvailable, 0, 0); return false; }
        // V0.17: publish the ROI for the dirty-region filter BEFORE taking the frame. Publishing it after the copy left a
        // window where a frame touching only the newly added ROI area was filtered against the old ROI and skipped.
        { const LONG m = static_cast<LONG>(std::ceil(96.0 * GetDpiForWindow(hwnd_) / 96.0));
          RECT e{wanted.left - m, wanted.top - m, wanted.right + m, wanted.bottom + m}, r{};
          if (IntersectRect(&r, &e, &monitorRect_)) {
              pending_->roi = RECT{r.left - monitorRect_.left, r.top - monitorRect_.top, r.right - monitorRect_.left, r.bottom - monitorRect_.top};
              pending_->roiValid = true;
          } else pending_->roiValid = false; }
        pending_->posted = false;
        if (inFlight_) return false;
        if (pending_->frame) { current_ = std::move(pending_->frame); currentId_ = pending_->sequence; currentDirtyHit_ = pending_->frameDirtyHit;
            capturedAt_ = current_.SystemRelativeTime().count() / 10000000.0; }
    }
    if (!current_ || currentId_ == candidateId_) return false;
    try {
        auto size = current_.ContentSize();
        if (testFault_ == L"size" || size.Width != monitorRect_.right-monitorRect_.left || size.Height != monitorRect_.bottom-monitorRect_.top) { Fail(L"capture size changed"); return true; }
        if (testFault_ == L"texture") { Fail(L"injected unavailable GPU texture"); return true; }
        LONG margin = static_cast<LONG>(std::ceil(96.0 * GetDpiForWindow(hwnd_) / 96.0));
        RECT expanded{wanted.left-margin,wanted.top-margin,wanted.right+margin,wanted.bottom+margin}, roi{};
        if (!IntersectRect(&roi, &expanded, &monitorRect_)) return false;
        UINT width=roi.right-roi.left, height=roi.bottom-roi.top;
        auto access=current_.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> source; check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
        D3D11_TEXTURE2D_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=sourceDesc.Format; desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_BOX box{static_cast<UINT>(roi.left-monitorRect_.left),static_cast<UINT>(roi.top-monitorRect_.top),0,static_cast<UINT>(roi.right-monitorRect_.left),static_cast<UINT>(roi.bottom-monitorRect_.top),1};
        candidate_.Reset();
        if (floatingCapture_ && desc.Format == docky::color::SceneFormat) {
            ComPtr<ID3D11Texture2D> cropped;
            check_hresult(device_->CreateTexture2D(&desc, nullptr, &cropped));
            context_->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
            desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            check_hresult(device_->CreateTexture2D(&desc, nullptr, &candidate_));
            ComPtr<ID3D11ShaderResourceView> input;
            ComPtr<ID3D11UnorderedAccessView> output;
            check_hresult(device_->CreateShaderResourceView(cropped.Get(), nullptr, &input));
            check_hresult(device_->CreateUnorderedAccessView(candidate_.Get(), nullptr, &output));
            const auto now = GetTickCount64();
            if (now >= nextWhiteCheck_) {
                const HMONITOR monitor = sourceWindow_ ? MonitorFromWindow(sourceWindow_, MONITOR_DEFAULTTONEAREST) : monitor_;
                captureWhite_ = docky::color::DisplayWhite(monitor);
                nextWhiteCheck_ = now + 1000;
            }
            D3D11_MAPPED_SUBRESOURCE map{};
            check_hresult(context_->Map(normalizeConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map));
            const float parameters[4]{1.f / captureWhite_};
            std::memcpy(map.pData, parameters, sizeof(parameters));
            context_->Unmap(normalizeConstants_.Get(), 0);
            context_->CSSetShader(normalizeShader_.Get(), nullptr, 0);
            context_->CSSetConstantBuffers(0, 1, normalizeConstants_.GetAddressOf());
            context_->CSSetShaderResources(0, 1, input.GetAddressOf());
            context_->CSSetUnorderedAccessViews(0, 1, output.GetAddressOf(), nullptr);
            context_->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            ID3D11ShaderResourceView* noInput = nullptr;
            ID3D11UnorderedAccessView* noOutput = nullptr;
            context_->CSSetShaderResources(0, 1, &noInput);
            context_->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
            context_->CSSetShader(nullptr, nullptr, 0);
        } else {
            check_hresult(device_->CreateTexture2D(&desc, nullptr, &candidate_));
            context_->CopySubresourceRegion(candidate_.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
        }
        candidateRect_=roi; candidateId_=currentId_; candidateTime_=capturedAt_;
        // Latency: the GPU compare round trip measured 8 ms average / 16 ms max, and up to 234 ms in one session
        // (log 2026-11-10), and while it is in flight newer frames wait. When the OS already reported dirty
        // rectangles inside the ROI the content is known to have changed, so publish this frame immediately;
        // the compare only remains for frames without usable dirty data.
        if (!texture_ || !EqualRect(&roi,&copiedRect_) || !compareShader_ || !context4_ || !fence_ || currentDirtyHit_) {
            texture_=candidate_; candidate_.Reset(); copiedRect_=roi; copiedId_=candidateId_; publishedTime_=candidateTime_; publishedLp_=candidateLp_;
            ++contentVersion_; ++publishedCount_; state_=2; NotePublished();
            if (diagnostics_) CompareRoi();
            // Per frame, so debug only (it was ~240 lines/s with a video wallpaper: 564 MB of log).
            DLOG_DEBUG(L"wgc: baseline or conservative publish capture=%llu content=%llu roi=(%ld,%ld)-(%ld,%ld)", copiedId_, contentVersion_, roi.left, roi.top, roi.right, roi.bottom);
            return true;
        }
        SubmitCompare(width,height); return false;
    } catch (const hresult_error& e) { DLOG(L"wgc: frame failed 0x%08X", static_cast<unsigned>(e.code().value)); Fail(L"frame error"); return true; }
}

bool WgcBackdropSource::CompleteCompare(const RECT& wanted) {
    if (!inFlight_) return false;
    if (!fence_ || fence_->GetCompletedValue() < fenceValue_) {
        if (fence_) fence_->SetEventOnCompletion(fenceValue_, gpuEvent_);
        return false;
    }
    double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compareStart_).count();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr=context_->Map(resultReadback_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++mapRetries_ <= 100) {
        LARGE_INTEGER due{}; due.QuadPart = -10000;
        retryMap_ = true; SetWaitableTimer(retryTimer_, &due, 0, nullptr, nullptr, FALSE);
        return false;
    }
    inFlight_ = false; retryMap_ = false;
    compareTotalMs_+=elapsed; compareMaxMs_=std::max(compareMaxMs_,elapsed);
    bool changed=true;
    if (SUCCEEDED(hr)) { changed=*static_cast<const UINT*>(mapped.pData)!=0; context_->Unmap(resultReadback_.Get(),0); }
    else DLOG(L"gpu compare read unavailable 0x%08X; conservative publish",hr);
    LONG margin=static_cast<LONG>(std::ceil(96.0*GetDpiForWindow(hwnd_)/96.0));
    RECT expanded{wanted.left-margin,wanted.top-margin,wanted.right+margin,wanted.bottom+margin}, roi{};
    bool valid=IntersectRect(&roi,&expanded,&monitorRect_) && EqualRect(&roi,&candidateRect_);
    bool published=false;
    if (diagnostics_ && valid) {
        const bool exactChanged = CompareRoi(candidate_.Get(), changed);
        if (exactChanged != changed) DLOG(L"ERROR gpu comparison disagrees with exact diagnostic GPU=%d exact=%d", changed, exactChanged);
    }
    if (changed && valid) { texture_=candidate_; copiedRect_=candidateRect_; copiedId_=candidateId_;
        publishedTime_=candidateTime_; publishedLp_=candidateLp_; ++contentVersion_; ++publishedCount_; published=true; NotePublished(); DLOG_DEBUG(L"wgc: content publish capture=%llu content=%llu", static_cast<unsigned long long>(copiedId_), static_cast<unsigned long long>(contentVersion_)); }
    candidate_.Reset();
    if (!valid) candidateId_=0;
    PostMessageW(hwnd_,kMsgCaptureAvailable,0,0);
    return published;
}

bool WgcBackdropSource::Latest(const RECT& wantedIn, BackdropFrame& out) {
    { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); lastDrawQpc_ = t.QuadPart; }  // the caller is drawing a frame now
    const int probe = dxgi_ ? ProbeOffset() : 0;
    RECT wanted = wantedIn; OffsetRect(&wanted, -probe, 0);
    if ((!pending_ && !dxgi_) || !texture_) return false;
    LONG margin=static_cast<LONG>(std::ceil(96.0*GetDpiForWindow(hwnd_)/96.0));
    RECT expanded{wanted.left-margin,wanted.top-margin,wanted.right+margin,wanted.bottom+margin}, roi{};
    if (!IntersectRect(&roi,&expanded,&monitorRect_)) { candidateId_=0; PostMessageW(hwnd_,kMsgCaptureAvailable,0,0); return false; }
    if (!EqualRect(&roi,&copiedRect_)) {
        // The window moved or resized (e.g. the Dock re-laid out on a desktop switch): ask for a copy of the new area, but
        // keep drawing with the previous one meanwhile (its screenRect still maps correctly; uncovered edges clamp for a
        // frame). Returning nothing here made the Dock draw its smoked fallback for a few frames: a white flash.
        candidateId_=0; PostMessageW(hwnd_,kMsgCaptureAvailable,0,0);
        RECT common{};const auto area=[](const RECT& r){return static_cast<long long>(r.right-r.left)*(r.bottom-r.top);};
        if (!IntersectRect(&common,&roi,&copiedRect_) || area(common)*2 < area(roi)) return false;  // a different place: wait
    }
    out.texture=texture_.Get(); out.screenRect=copiedRect_; OffsetRect(&out.screenRect, probe, 0);  // probe: shown shifted
    out.frameId=contentVersion_; out.captureTimeSeconds=publishedTime_; return true;
}
}
