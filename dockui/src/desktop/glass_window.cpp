#include "glass_window.h"
#include "display_adapter.h"
#include "../capture_visible.h"
#include "../glass_params.h"
#include "../menu_surface.h"
#include "../log.h"
#include <algorithm>

namespace docky::desk {
namespace {
std::uint64_t Hash(const std::vector<std::uint32_t>& px) {  // FNV-1a over every 3rd pixel: enough to see a change
    std::uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < px.size(); i += 3) { h ^= px[i]; h *= 1099511628211ull; }
    return h ^ px.size();
}
}  // namespace

ID3D11Device* SharedUiDevice() {
    static Microsoft::WRL::ComPtr<ID3D11Device> device = [] {
        Microsoft::WRL::ComPtr<ID3D11Device> d;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        if (FAILED(docky::CreateRenderDevice(D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), &d, nullptr, nullptr)))
            DLOG(L"desk glass: shared UI device unavailable; each window creates its own");
        return d;
    }();
    return device.Get();
}

GlassWindow::~GlassWindow() { liveCapture_.SetDevice(nullptr); if (renderer_) renderer_->Shutdown(); if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_); }

bool GlassWindow::Create(const wchar_t* className, WNDPROC proc, DWORD extraExStyle, bool noActivate) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = proc; wc.lpszClassName = className; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | (noActivate ? WS_EX_NOACTIVATE : 0) | extraExStyle, className, L"DockOver Desktop", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    return hwnd_ != nullptr;
}

bool GlassWindow::Render(const RECT& screen, HWND insertAfter, D2D1_RECT_F plate, float radius,
                         const std::function<void(ID2D1DeviceContext*, bool)>& content) {
    lastScreen_ = screen; lastInsert_ = insertAfter; lastPlate_ = plate; lastRadius_ = radius;
    return Draw(&content);
}

bool GlassWindow::Redraw() { return rendered_ && content_ && Draw(nullptr); }

bool GlassWindow::PollLiveCapture(bool& changed) {
    liveCapture_.Request(true, MonitorFromRect(&lastScreen_, MONITOR_DEFAULTTONEAREST));
    const ULONGLONG now = GetTickCount64();
    bool published = false;
    if (now - healthAt_ >= 1000) {
        healthAt_ = now;
        liveCapture_.CheckHealth();
    }
    if (const HANDLE event = liveCapture_.CompletionEvent(); event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
        published = liveCapture_.CompleteCompare(lastScreen_);
    published = liveCapture_.ProcessCapture(lastScreen_) || published;
    const bool hasLiveFrame = frame_.texture && frame_.texture != texture_.Get();
    if (liveCapture_.State() == 2 && hasLiveFrame && !published) return true;
    dock::BackdropFrame captured;
    if (liveCapture_.State() == 2 && liveCapture_.Latest(lastScreen_, captured)) {
        changed = frame_.texture != captured.texture || frame_.frameId != captured.frameId;
        liveTexture_ = captured.texture;
        frame_ = captured;
        haveFrame_ = true;
        return true;
    }
    if (hasLiveFrame) {
        frame_ = {};
        liveTexture_.Reset();
        haveFrame_ = false;
        changed = true;
    }
    return false;
}

bool GlassWindow::CaptureExcludedScreen(const RECT& screen, std::vector<std::uint32_t>& pixels) {
    if (docky::capture_visible::Frozen()) return false;  // shown to a screenshot (capture_visible.h): keep the old image
    DWORD affinity = WDA_NONE;
    if (!GetWindowDisplayAffinity(hwnd_, &affinity)) return false;
    if (affinity != WDA_EXCLUDEFROMCAPTURE &&
        (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE) ||
         !GetWindowDisplayAffinity(hwnd_, &affinity) || affinity != WDA_EXCLUDEFROMCAPTURE)) return false;
    return CaptureScreen(screen, pixels);
}

bool GlassWindow::Draw(const std::function<void(ID2D1DeviceContext*, bool)>* content) {
    const RECT screen = lastScreen_; const HWND insertAfter = lastInsert_; const D2D1_RECT_F plate = lastPlate_; const float radius = lastRadius_;
    if (!hwnd_ || hidden_) return false;
    const UINT w = static_cast<UINT>(screen.right - screen.left), h = static_cast<UINT>(screen.bottom - screen.top);
    if (!w || !h) return false;
    RECT now{}; GetWindowRect(hwnd_, &now);
    if (!EqualRect(&now, &screen) || !IsWindowVisible(hwnd_))
        SetWindowPos(hwnd_, insertAfter, screen.left, screen.top, static_cast<int>(w), static_cast<int>(h), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UINT dpi = GetDpiForWindow(hwnd_); if (!dpi) dpi = 96;
    if (!renderer_) {
        renderer_ = std::make_unique<dock::Renderer>();
        renderer_->UseDevice(deviceSource_ && deviceSource_->renderer_ ? deviceSource_->renderer_->Device() : SharedUiDevice());
        if (FAILED(renderer_->Init(hwnd_, w, h, static_cast<float>(dpi)))) { renderer_.reset(); return false; }
        liveCapture_.SetDevice(renderer_->Device());
        liveCapture_.SetExcludedWindow(hwnd_);
        renderer_->SetSyncInterval(0);  // paced by the display ticker and the frame-latency waitable (see Draw)
        w_ = w; h_ = h; dpi_ = static_cast<float>(dpi);
    } else if (w != w_ || h != h_ || dpi != dpi_) {
        if (FAILED(renderer_->Resize(w, h, static_cast<float>(dpi)))) return false;
        w_ = w; h_ = h; dpi_ = static_cast<float>(dpi);
    }
    // Backdrop: the window plus the material's 96 DIP sampling margin, recomposed only when the window moves.
    const LONG m = static_cast<LONG>(96.f * dpi_ / 96.f);
    const RECT want{screen.left - m, screen.top - m, screen.right + m, screen.bottom + m};
    if (!haveFrame_ || !EqualRect(&want, &backdropRect_)) {
        bool changed = false;
        const bool done = live_ && PollLiveCapture(changed);
        if (!done) {
            std::vector<std::uint32_t> px;
            const bool got = (live_ && CaptureExcludedScreen(want, px)) || ComposeBackdrop(want, px);
            backdropHash_ = got ? Hash(px) : 0;
            haveFrame_ = got && liquid::popup::UploadBackdrop(renderer_->Device(), want, px, texture_, frameId_, frame_);
        }
        backdropRect_ = want;
    }
    dock::DockFrame geometry; geometry.dockLeft = plate.left; geometry.dockTop = plate.top; geometry.dockRight = plate.right; geometry.dockBottom = plate.bottom;
    dock::DockMetrics metrics;
    dock::GlassParameters glass = dock::RoleGlass(dock::GlassRole::Panel);
    if (halo_) {  // the blurred desktop around a menu-bar-style panel (Glass Studio > Menu bar popups > Background blur)
        glass = dock::RoleGlass(dock::GlassRole::BackdropBlur);
        glass.blurSpread = haloSpread_; glass.edgeSoftness = kHaloFeather;
    } else if (frosted_) {  // gallery / settings: Glass Studio category "Menu bar popups" (glass.json, re-read when changed)
        auto& store = dock::GlassSettingsStore::Get();
        store.ReloadIfChanged();
        glassVersion_ = store.Version();
        glass = store.Surface(dock::GlassSurface::MenuBarPanels);
        glass.material = dock::MaterialKind::Liquid; glass.dark = true; glass.debugView = 0; glass.continuousRender = false;
    }
    glass.radius = radius; glass.opacity = 1.f;
    dock::Scene scene;
    scene.frame = &geometry; scene.metrics = &metrics; scene.glass = &glass; scene.windowW = w / (dpi_ / 96.f); scene.windowH = h / (dpi_ / 96.f);
    scene.windowScreenRect = screen; scene.backdrop = haveFrame_ ? &frame_ : nullptr;
    scene.localGlass = &local_;  // used only while active (a pressed Glass Lab control)
    // Content: drawn into a cached bitmap (same size and DPI as the window) and composited; Redraw reuses it.
    const UINT cw = w, ch = h;
    scene.customDraw = [&](ID2D1DeviceContext* dc, bool gpu) {
        if (content || !content_ || contentGpu_ != gpu || contentW_ != cw || contentH_ != ch) {
            if (!content) return;  // nothing cached that fits: the caller repaints
            Microsoft::WRL::ComPtr<ID2D1Image> old; dc->GetTarget(&old);
            if (!content_ || contentW_ != cw || contentH_ != ch) {
                content_.Reset();
                const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
                if (FAILED(dc->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, props, &content_))) { content_.Reset(); (*content)(dc, gpu); return; }
                contentW_ = cw; contentH_ = ch;
            }
            dc->SetTarget(content_.Get()); dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            (*content)(dc, gpu);
            dc->SetTarget(old.Get()); dc->SetTransform(D2D1::Matrix3x2F::Identity());
            contentGpu_ = gpu;
        }
        dc->DrawImage(content_.Get());
    };
    // The swap chain allows one queued frame (frame-latency waitable). Presenting without waiting for it made Present spin
    // in the driver (40 % of a core with a panel and its halo). A backdrop-only redraw skips a frame that is not ready yet
    // and is retried next display frame; a full repaint waits for it (a kernel wait, no CPU).
    if (HANDLE ready = renderer_->Waitable()) {
        if (WaitForSingleObject(ready, content ? 50 : 0) == WAIT_TIMEOUT) { pendingRedraw_ = true; return false; }
    }
    pendingRedraw_ = false;
    const bool ok = SUCCEEDED(renderer_->Render(scene));
    rendered_ = ok;
    {   // debug: draws per second of this window
        ++drawCount_; cpuSum_ += renderer_->LastCpuMs(); presentSum_ += renderer_->LastPresentMs(); const ULONGLONG t = GetTickCount64();
        if (!drawSince_) drawSince_ = t;
        if (t - drawSince_ >= 2000) { DLOG_DEBUG(L"desk glass: %ls %.1f draws/s, render cpu %.2f ms, present %.2f ms avg (content %ls), %ux%u", halo_ ? L"halo" : L"panel", drawCount_ * 1000.0 / (t - drawSince_), drawCount_ ? cpuSum_ / drawCount_ : 0.0, drawCount_ ? presentSum_ / drawCount_ : 0.0, content ? L"repainted" : L"cached", w, h); drawCount_ = 0; cpuSum_ = 0; presentSum_ = 0; drawSince_ = t; }
    }
    if (ok && !halo_) UpdateHalo(screen, radius);
    return ok;
}

// Menu bar popups draw a blur of the desktop behind the whole panel that reaches past its edge and fades out
// (control_center.cpp: kBlurReach 28 + kBlurFeather 34 DIP). Live frosted panels get the same: a click-through window
// directly below the panel, excluded from capture like the panel, drawing only that blur.
void GlassWindow::UpdateHalo(const RECT& screen, float radius) {
    float spread = 0;
    if (frosted_ && live_) {
        auto& store = dock::GlassSettingsStore::Get();
        const auto p = store.Surface(dock::GlassSurface::MenuBarPanels);
        if (!p.reducedTransparency && !store.Current().reducedTransparency) spread = p.backdropBlur;
    }
    if (spread <= 0.001f) { if (haloWin_) haloWin_->Hide(); return; }
    if (!haloWin_) {
        haloWin_ = std::make_unique<GlassWindow>();
        haloWin_->halo_ = true; haloWin_->deviceSource_ = this;
        const DWORD top = (GetWindowLongW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? WS_EX_TOPMOST : 0;
        if (!haloWin_->Create(L"DockyDesktopGlassHalo", DefWindowProcW, WS_EX_LAYERED | WS_EX_TRANSPARENT | top, true)) { haloWin_.reset(); return; }
        SetLayeredWindowAttributes(haloWin_->Hwnd(), 0, 255, LWA_ALPHA);  // layered + transparent: clicks pass through the blur ring
        haloWin_->SetLiveBackdrop(true);
    }
    const float k = dpi_ / 96.f;
    const LONG pad = static_cast<LONG>(std::lround((kHaloReach + kHaloFeather) * k));
    RECT r{screen.left - pad, screen.top - pad, screen.right + pad, screen.bottom + pad};
    // Redrawn here only when its place or blur changed; a changing desktop behind it is handled by RefreshLiveBackdrop
    // (redrawing it on every panel draw presented it twice per frame).
    if (haloWin_->Visible() && haloWin_->haloSpread_ == spread && EqualRect(&haloWin_->lastScreen_, &r)) {
        if (GetWindow(hwnd_, GW_HWNDNEXT) != haloWin_->Hwnd())  // keep it right below the panel (only when that changed)
            SetWindowPos(haloWin_->Hwnd(), hwnd_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    haloWin_->haloSpread_ = spread;
    const float wd = (r.right - r.left) / k, hd = (r.bottom - r.top) / k;
    haloWin_->Show();
    haloWin_->Render(r, hwnd_, D2D1::RectF(kHaloFeather, kHaloFeather, wd - kHaloFeather, hd - kHaloFeather), radius + kHaloReach,
                     [](ID2D1DeviceContext*, bool) {});
}

void GlassWindow::SetLiveBackdrop(bool live) {
    live_ = live;
    if (!live) liveCapture_.Request(false, nullptr);
    if (hwnd_) SetWindowDisplayAffinity(hwnd_, live ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    backdropRect_ = RECT{};
}

bool GlassWindow::RefreshLiveBackdrop() {
    if (!live_ || !Visible() || !renderer_) return false;
    if (haloWin_ && haloWin_->Visible() && haloWin_->RefreshLiveBackdrop()) haloWin_->Redraw();
    bool changed = false;
    if (PollLiveCapture(changed)) return pendingRedraw_ || changed;
    if (pendingRedraw_ || changed || !haveFrame_) return rendered_;
    const ULONGLONG now = GetTickCount64();  // GDI fallback: a full readback of the screen, so at most 5 per second
    if (now - gdiAt_ < 200) return false;
    gdiAt_ = now;
    std::vector<std::uint32_t> px;
    if (!CaptureExcludedScreen(backdropRect_, px)) return false;
    const std::uint64_t h = Hash(px);
    if (h == backdropHash_) return false;
    backdropHash_ = h;
    haveFrame_ = liquid::popup::UploadBackdrop(renderer_->Device(), backdropRect_, px, texture_, frameId_, frame_);
    return true;
}

bool GlassWindow::GlassChanged() {
    if (!frosted_ || !Visible()) return false;
    auto& store = dock::GlassSettingsStore::Get();
    store.ReloadIfChanged();
    const bool changed = store.Version() != glassVersion_;
    if (changed) DLOG(L"desk glass: tuning changed (version %llu); repaint", store.Version());
    return changed;
}

void GlassWindow::Hide() {
    hidden_ = true;
    if (frame_.texture && frame_.texture != texture_.Get()) { haveFrame_ = false; frame_ = {}; }
    liveTexture_.Reset();
    liveCapture_.Request(false, nullptr);
    if (haloWin_) haloWin_->Hide();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

}  // namespace docky::desk
