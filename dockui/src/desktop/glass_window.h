#pragma once
// A desktop-level window drawn with docky's Liquid Glass material (dock::Renderer, the same GPU material as the
// Dock, menus and Control Center). Desktop widgets can use a composed backdrop, while the popups use live capture.
#include "canvas.h"
#include "../backdrop_wgc.h"
#include "../renderer.h"
#include <functional>
#include <memory>

namespace docky::desk {

// One D3D device for every glass window and menu of docky Desktop (all on its UI thread). Each additional device reading
// the cross-device desktop image added a GPU synchronisation that made Present block for ~7 ms (40-60 % CPU).
ID3D11Device* SharedUiDevice();

class GlassWindow {
public:
    ~GlassWindow();
    // A popup with WS_EX_NOREDIRECTIONBITMAP (composition swap chain); `proc` receives its messages.
    bool Create(const wchar_t* className, WNDPROC proc, DWORD extraExStyle = 0, bool noActivate = true);
    HWND Hwnd() const { return hwnd_; }
    // Shows the window at `screen` (physical px) and draws the glass plate `plate` (DIP, window coordinates) with
    // corner radius `radius`, then `content` on top (gpu = false: the material was unavailable; draw a fallback plate).
    bool Render(const RECT& screen, HWND insertAfter, D2D1_RECT_F plate, float radius,
                const std::function<void(ID2D1DeviceContext*, bool gpu)>& content);
    // Draws the last Render again with a new backdrop, reusing its content (kept in a bitmap): a moving backdrop costs a
    // glass pass, not a repaint of the gallery's previews or the settings controls.
    bool Redraw();
    // Hide() also blocks Render() from showing the window again until Show(): a repaint triggered while hiding (the
    // WM_ACTIVATE that hiding the active window sends) re-showed a closed gallery that then ignored all input.
    void Hide();
    void Show() { hidden_ = false; }
    bool Visible() const { return hwnd_ && !hidden_ && IsWindowVisible(hwnd_); }
    void InvalidateBackdrop() { backdropRect_ = RECT{}; }
    // Large panels (gallery, settings): heavier frost and a darker body so text stays legible over any wallpaper,
    // like the macOS widget gallery; the edge refraction is kept.
    void SetFrosted(bool frosted) { frosted_ = frosted; }
    // Top-most panels (gallery, settings): the backdrop is what is really on screen behind the panel (CaptureScreen,
    // with this window excluded from capture). Desktop-level panels keep the composed wallpaper backdrop.
    void SetLiveBackdrop(bool live);
    // Live panels: true when what is behind the panel changed (then Redraw). Cheap enough to call every frame: it uses
    // the shared borderless WGC/DXGI pipeline, with a GDI screen-copy fallback at up to 5 frames per second.
    bool RefreshLiveBackdrop();
    // True when the Glass Studio tuning of these panels changed since the last Render (repaint then).
    bool GlassChanged();
    void RequestSnapshot(const std::wstring& path) { if (renderer_) renderer_->RequestSnapshot(path); }  // development (BMP)
    // A Liquid Glass lens over this window's own pixels (a pressed Glass Lab switch or slider thumb). The content callback
    // of Render fills it in (it runs inside the render); Redraw keeps the last one, matching the cached content.
    dock::LocalGlassOverlay& LocalGlass() { return local_; }
private:
    HWND hwnd_ = nullptr;
    std::unique_ptr<dock::Renderer> renderer_;
    dock::LocalGlassOverlay local_;
    UINT w_ = 0, h_ = 0; float dpi_ = 96;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    bool frosted_ = false, live_ = false, hidden_ = false;
    std::uint64_t frameId_ = 0; RECT backdropRect_{}; dock::BackdropFrame frame_{}; bool haveFrame_ = false; std::uint64_t backdropHash_ = 0; unsigned long long glassVersion_ = 0;
    // Content cache (Redraw) and the last Render's placement.
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> content_; bool contentGpu_ = false; UINT contentW_ = 0, contentH_ = 0;
    RECT lastScreen_{}; HWND lastInsert_ = nullptr; D2D1_RECT_F lastPlate_{}; float lastRadius_ = 0; bool rendered_ = false;
    dock::WgcBackdropSource liveCapture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> liveTexture_;
    ULONGLONG gdiAt_ = 0, healthAt_ = 0;
    bool PollLiveCapture(bool& changed);
    bool CaptureExcludedScreen(const RECT& screen, std::vector<std::uint32_t>& pixels);
    // Background blur halo (menu bar popups' look), a second, click-through window below the panel.
    static constexpr float kHaloReach = 28.f, kHaloFeather = 34.f;
    std::unique_ptr<GlassWindow> haloWin_; bool halo_ = false; float haloSpread_ = 0; GlassWindow* deviceSource_ = nullptr;
    unsigned drawCount_ = 0; ULONGLONG drawSince_ = 0; double cpuSum_ = 0, presentSum_ = 0; bool pendingRedraw_ = false;
    void UpdateHalo(const RECT& screen, float radius);
    bool Draw(const std::function<void(ID2D1DeviceContext*, bool)>* content);
};

}  // namespace docky::desk
