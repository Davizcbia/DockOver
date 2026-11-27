#pragma once
// D3D11 + DXGI composition swap chain + DirectComposition + Direct2D renderer for the Dock window.
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include "status_group.h"
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <functional>

#include "backdrop.h"
#include "dock_model.h"
#include "material.h"
#include "menu_model.h"
#include <map>

namespace dock {

using Microsoft::WRL::ComPtr;

struct IconVisual {
    ID2D1Bitmap1* icon = nullptr;
    ID2D1Bitmap1* shadow = nullptr;
    float shadowPadRatio = 0.0f;  // shadow bitmap padding / icon bitmap width
    bool running = false;
    bool separator = false;
    bool external = false;        // drawn by another window (minimized-window tile): no placeholder plate
    float opacity = 1.0f;
    ID2D1Bitmap1* alt = nullptr;  // cross-faded state icon (Downloads while its Fan is open)
    float altBlend = 0.0f;
    // The status group (status_group.h): drawn live into the tile's body instead of a bitmap.
    const std::vector<std::wstring>* statusCells = nullptr;
    const dock::statusgroup::State* statusState = nullptr;
    int statusHover = -1;
};

struct MenuRowVisual {
    std::wstring text; bool enabled=false,checked=false,separator=false,submenu=false,hover=false;
    std::shared_ptr<const liquid::menu::Icon> icon;  // 16 DIP, left of the text (grid: 26 DIP, centred in its cell)
};
// Grid popups (the Dock's Hidden Icons): square cells in rows, the hovered item's name in a footer strip below (DIP).
inline constexpr float kMenuGridPad = 10.f, kMenuGridCell = 44.f, kMenuGridIcon = 26.f, kMenuGridFooter = 30.f, kMenuGridMinWidth = 200.f;
// Optional locally sampled overlay, requested by custom UI after its opaque D2D
// pass. The source is the current window's own pixels, never desktop capture.
struct LocalGlassOverlay {
    bool active = false;
    D2D1_RECT_F plate{};
    D2D1_RECT_F sampleBounds{};
    GlassParameters params{};
    float pointerX = 0, pointerY = 0;
    int cacheKey = 0;
    float press = 1.f;
    float controlScale = 1.f; // full-size Lab control rendered at this output scale
    std::function<void(ID2D1DeviceContext*)> fallback;
    std::function<void(ID2D1DeviceContext*)> after;
};
struct Scene {
    // Optional custom content drawn over the material on the same D2D target.
    std::function<void(ID2D1DeviceContext*, bool)> customDraw;
    LocalGlassOverlay* localGlass = nullptr;
    const std::vector<LocalGlassOverlay>* chromeGlass = nullptr;
    const DockFrame* frame = nullptr;
    const DockMetrics* metrics = nullptr;
    std::vector<IconVisual> icons;
    bool menuMode = false;
    bool opaqueFallback = false;
    COLORREF opaqueBackground = RGB(36,36,40), opaqueForeground = RGB(255,255,255);
    std::vector<MenuRowVisual> menuRows;
    int menuGridColumns = 0;  // > 0: menuRows are grid cells, this many per row (kMenuGrid* layout)
    std::wstring menuFooter;  // grid: the text in the footer strip
    int dragIndex = -1;
    bool showInsertion = false;
    float insertionX = 0;
    int labelIndex = -1;
    std::wstring label;
    float labelAlpha = 0.0f;
    bool showDiag = false;
    std::wstring diagText;
    bool showBounds = false;
    float windowW = 0, windowH = 0;
    const BackdropFrame* backdrop = nullptr;  // passed through to the plate material
    const GlassParameters* glass = nullptr;   // consistent per-frame snapshot
    RECT windowScreenRect{};
    float pointerX = 0, pointerY = 0, hover = 0, press = 0;  // window px, smoothed
};

class GpuTimer {
public:
    HRESULT Init(ID3D11Device* device);
    void Release();
    void Begin(ID3D11DeviceContext* ctx);
    void Mark(ID3D11DeviceContext* ctx);
    void End(ID3D11DeviceContext* ctx);
    bool Poll(ID3D11DeviceContext* ctx, float& frameMs, float& glassMs);

private:
    static constexpr int kSlots = 6;
    struct Slot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> stamps[3];
        bool pending = false;
    };
    Slot slots_[kSlots];
    int write_ = 0;
    bool active_ = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    HRESULT Init(HWND hwnd, UINT pxW, UINT pxH, float dpi, bool aboveChildWindows = true);
    void Shutdown();
    bool Ready() const { return dc_ != nullptr; }
    HRESULT Resize(UINT pxW, UINT pxH, float dpi);
    HRESULT SetRoundedVisualClip(float radiusPx);
    void RequestSnapshot(const std::wstring& path) { snapshotPath_ = path; }
    void RequestPngSnapshot() { pngSnapshotRequested_ = true; }
    HRESULT Render(const Scene& scene);  // draws, presents (sync interval 1)
    bool LabelBounds(const Scene& s, const ItemGeom& g, D2D1_RECT_F& bounds);
    // Low-latency layering (opt-in, the Dock; see renderer.cpp): PrepareUi draws the D2D UI into a cached layer before
    // the desktop frame is taken; Compose then draws only the GPU glass, lays the cached layer over it and presents.
    // Compose returns S_FALSE when it cannot draw this scene (no cached layer, no backdrop, glass or label glass failed):
    // the caller then uses Render.
    bool CanCompose(const Scene& scene) const;
    HRESULT PrepareUi(const Scene& scene);
    HRESULT Compose(const Scene& scene);
    bool UiValid() const { return uiValid_; }
    void InvalidateUi() { uiValid_ = false; }
    float LastUiMs() const { return lastUiMs_; }
    uint64_t ComposedFrames() const { return composedFrames_; }
    HRESULT SetVisualOpacity(float opacity);
    HRESULT SetVisualOffsetY(float px);  // auto-hide slide (DirectComposition only)
    HANDLE Waitable() const { return waitable_; }
    HRESULT CreateIcon(const std::vector<uint32_t>& pixels, UINT w, UINT h, ComPtr<ID2D1Bitmap1>& icon,
                       ComPtr<ID2D1Bitmap1>& shadow, float& shadowPadRatio);
    HRESULT QueryFrameStats(UINT& presentCount, UINT& presentRefreshCount, UINT& syncRefreshCount);
    // The newest Present's number, and which Present reached the screen at which QPC time (DXGI frame statistics).
    bool PresentTiming(UINT& lastPresentCount, UINT& shownPresentCount, LONGLONG& shownQpc);
    D3D_FEATURE_LEVEL FeatureLevel() const { return featureLevel_; }
    const wchar_t* MaterialName() const { return material_ ? material_->Name() : L"none"; }
    ID3D11Device* Device() const { return d3d_.Get(); }
    bool LiquidAvailable() const { return liquid_ != nullptr; }
    bool LastUsedGpu() const { return lastUsedGpu_; }
    uint64_t PresentCalls() const { return presentCalls_; }
    float LastCpuMs() const { return lastCpuMs_; }
    float LastPresentMs() const { return lastPresentMs_; }  // time spent in Present (blocked or spinning)
    // 1 (default): Present waits for vblank. 0: for callers that pace themselves on the frame-latency waitable (and a
    // display tick): Present then never blocks; some drivers spin the CPU while Present waits (6.5 ms per call measured).
    void SetSyncInterval(UINT interval) { syncInterval_ = interval; }
    // Before Init: render on an existing device (another renderer of the same thread) instead of creating one. Each extra
    // device reading a cross-device shared texture added a GPU synchronisation that made Present block ~7 ms.
    void UseDevice(ID3D11Device* device) { useDevice_ = device; }
    bool PollGpuTimes(float& frameMs, float& glassMs);

private:
    void RenderGlassLayers(const Scene& scene);  // GPU plate + label glass; sets lastUsedGpu_ / labelUsedGpu_
    HRESULT FinishFrame(const Scene& scene, HRESULT drawResult, double t0);
    HRESULT PresentFrame(const Scene& scene);  // local/chrome glass overlays, snapshots, Present
    ComPtr<ID2D1Bitmap1> uiBitmap_;              // cached UI layer (low-latency path)
    UINT uiW_ = 0, uiH_ = 0;
    float uiDpi_ = 0.0f;
    bool uiValid_ = false;
    float lastUiMs_ = 0.0f;
    uint64_t composedFrames_ = 0;
    HRESULT CreateTarget();
    ComPtr<ID3D11Texture2D> SnapshotSource();
    bool RenderLocalGlass(const LocalGlassOverlay& overlay);
    void DrawScene(const Scene& scene, bool drawPlate);
    ComPtr<ID2D1PathGeometry> ContinuousRect(D2D1_RECT_F r, float radius);
    void DrawLabel(const Scene& s, const ItemGeom& g);
    bool labelUsedGpu_ = false;

    void SaveSnapshot();
    void SavePngSnapshot();
    bool pngSnapshotRequested_ = false;
    std::wstring snapshotPath_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    bool sharedGraphicsMemory_ = false;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDCompositionEffectGroup> opacityEffect_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> labelFormat_;
    ComPtr<IDWriteTextFormat> diagFormat_;
    ComPtr<IDWriteTextFormat> menuRowFormat_;  // menu rows: trailing ellipsis
    std::map<const liquid::menu::Icon*, std::pair<std::shared_ptr<const liquid::menu::Icon>, ComPtr<ID2D1Bitmap1>>> menuIcons_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::unique_ptr<IPlateMaterial> material_;  // Smoked Glass (D2D) fallback
    std::unique_ptr<IPlateMaterial> liquid_;    // Liquid Glass (GPU)
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    struct Output;
    std::unique_ptr<Output> output_;
    struct LocalGlassCache {ComPtr<ID3D11Texture2D> scene;UINT width=0,height=0;std::unique_ptr<IPlateMaterial> material;};
    std::array<LocalGlassCache,4> localGlassCaches_;
    uint64_t localSceneFrame_ = 0;
    UINT targetW_ = 0, targetH_ = 0;
    GpuTimer timer_;
    bool lastUsedGpu_ = false;
    uint64_t presentCalls_ = 0;
    float lastCpuMs_ = 0.0f, lastPresentMs_ = 0.0f;
    UINT syncInterval_ = 1;
    Microsoft::WRL::ComPtr<ID3D11Device> useDevice_;
};

}  // namespace dock
