#pragma once
#include "backdrop.h"
#include <wrl/client.h>
#include <d3d11_4.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>
#include <string>

namespace docky::desk { class SharedDesktop; }

namespace dock {
constexpr UINT kMsgCaptureAvailable = WM_APP + 71;
// Live monitor backdrops prefer DXGI desktop duplication. Window capture and monitor fallback use
// cursor-disabled borderless WGC.
// WGC sessions start only after permission and border/cursor properties have been checked; otherwise
// the requesting surface retains its static/tinted fallback rather than starting a bordered capture.
// DOCKY_CAPTURE=wgc forces WGC and never falls back to duplication.
// Borderless capture consent (package identity). Ask early (process start, on the UI thread) so the first capture never
// starts with the yellow border while the asynchronous answer is pending. Granted: Windows allows IsBorderRequired(false).
void PrewarmBorderlessCapture();
bool BorderlessCaptureGranted();
// Configure every WGC session before StartCapture; never continue with cursor or border enabled.
bool PrepareCleanCaptureSession(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session);
class WgcBackdropSource final : public IBackdropSource {
public:
    ~WgcBackdropSource() override;
    void SetDevice(ID3D11Device* device);
    void SetDiagnostics(bool enabled) { diagnostics_ = enabled; }
    void Request(bool enabled, HMONITOR monitor);
    // Before Request: capture this window (with its child windows) instead of the whole monitor, e.g. the desktop's
    // wallpaper layer (Progman, which also hosts Wallpaper Engine) so app windows are not in the image. Null = monitor.
    void SetSourceWindow(HWND window) { if (window != sourceWindow_) { Stop(); monitor_ = nullptr; state_ = 0; sourceWindow_ = window; } }
    bool CheckHealth();
    bool Latest(const RECT& wantedScreenRect, BackdropFrame& out) override;
    void SetExcludedWindow(HWND hwnd) override { hwnd_ = hwnd; }
    const wchar_t* Name() const override { return dxgi_ ? L"Live desktop (DXGI duplication)" : L"Live desktop (WGC)"; }
    bool UsingDuplication() const { return dxgi_ != nullptr; }
    // The shared monitor duplication this source reads (null with WGC or when off): the low-latency Dock owns it.
    docky::desk::SharedDesktop* Duplication() const { return dxgi_; }
    // QueryPerformanceCounter of the published image's LastPresentTime (when DWM showed it); 0 unknown. For the
    // capture-to-screen latency counter.
    int64_t PublishedLastPresentQpc() const { return publishedLp_; }
    // Dev measurement only (--dev-isolated + DOCKY_LATENCY_PROBE_OFFSET=px): the window stays visible to capture and
    // samples the area `px` to the left, so an external probe can compare its glass with the source in one screenshot.
    static int ProbeOffset();
    int State() const { return state_; }
    HANDLE CompletionEvent() const { return inFlight_ ? (retryMap_ ? retryTimer_ : gpuEvent_) : nullptr; }
    bool ProcessCapture(const RECT& wanted);
    bool CompleteCompare(const RECT& wanted);

private:
    struct Pending {
        std::mutex mutex;
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
        std::atomic<bool> active{true}, posted{false}, closed{false};
        std::atomic<uint64_t> callbacks{0}, acquired{0}, dirtyFrames{0}, emptyDirtyFrames{0};
        std::atomic<uint64_t> roiDirtyChecked{0}, roiDirtyIntersections{0}, skippedOutsideRoi{0};
        RECT roi{};  // capture-surface (monitor-relative) pixels; guarded by mutex
        bool roiValid = false;
        bool inspectDirty = false;
        bool skipOutsideRoi = false;  // V0.16: OS reports dirty regions, so frames that cannot touch the ROI need no wake-up
        bool suppressFrames = false;
        bool frameDirtyHit = false;  // latest frame: the OS reported dirty rectangles that touch the ROI
        bool closeAfterFrame = false;
        uint64_t sequence = 0;
        HWND notify = nullptr;
    };
    bool CompareRoi(ID3D11Texture2D* source = nullptr, bool commit = true);
    void InitCompare();
    void SubmitCompare(UINT width, UINT height);
    void DiscardCompare();
    void PublishCandidate();
    void Fail(const wchar_t* reason);
    void StartOrWait(HMONITOR monitor, bool tryDuplication = true);
    bool Start(HMONITOR monitor, bool tryDuplication);
    bool StartDuplication(HMONITOR monitor);
    bool ProcessDuplication(const RECT& wanted);
    void Stop();
    docky::desk::SharedDesktop* dxgi_ = nullptr;  // set while the monitor is read by desktop duplication
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dxgiView_;  // the shared desktop image opened on device_
    unsigned dxgiGeneration_ = 0;
    uint64_t dxgiSeen_ = 0, dxgiCopies_ = 0;
    bool wgcRefusedLogged_ = false;
    // Capture latency: from the moment Windows produced the frame to its publication (both paths), for the log.
    double latencySum_ = 0, latencyMax_ = 0; uint64_t latencyCount_ = 0;
    int64_t candidateLp_ = 0, publishedLp_ = 0;  // LastPresentTime (QPC) of the candidate / published duplication image
    int64_t lastDrawQpc_ = 0;  // QueryPerformanceCounter of the caller's last frame (Latest)
    void NotePublished();
    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    HWND sourceWindow_ = nullptr;
    int startedCap_ = 0;  // capture cap (fps, 0 = none) the running session was started with
    RECT monitorRect_{}, copiedRect_{};
    int state_ = 0;  // 0 off, 1 starting, 2 frames, 3 failed, 4 waiting for the borderless consent answer
    std::chrono::steady_clock::time_point consentWaitSince_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_, candidate_, normalizationInput_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    uint64_t fenceValue_ = 0;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compareShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> normalizeShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> normalizeConstants_;
    float captureWhite_ = 1.f;
    ULONGLONG nextWhiteCheck_ = 0;
    bool floatingCapture_ = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> result_, resultReadback_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resultUav_;
    HANDLE gpuEvent_ = nullptr, retryTimer_ = nullptr;
    bool retryMap_ = false;
    unsigned mapRetries_ = 0;
    bool inFlight_ = false;
    RECT candidateRect_{};
    uint64_t contentVersion_ = 0, compareCount_ = 0, publishedCount_ = 0;
    uint64_t candidateId_ = 0;
    double candidateTime_ = 0;
    std::chrono::steady_clock::time_point compareStart_{};
    double compareTotalMs_ = 0, compareMaxMs_ = 0;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice runtimeDevice_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame current_{nullptr};
    winrt::event_token frameToken_{}, closedToken_{};
    std::shared_ptr<Pending> pending_;
    uint64_t currentId_ = 0, copiedId_ = 0;
    bool currentDirtyHit_ = false;  // current_ is known to have changed inside the ROI: publish without the GPU compare
    double capturedAt_ = 0, publishedTime_ = 0;
    bool diagnostics_ = false;
    uint64_t diagnosticSamples_ = 0, diagnosticChanges_ = 0;
    std::vector<unsigned char> previousPixels_;
    RECT previousRect_{};
    std::wstring testFault_;
    std::chrono::steady_clock::time_point startedAt_{}, failedAt_{};
};
}
