#include "launchpad.h"

#include <windowsx.h>
#include <imm.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app_registry.h"
#include "backdrop.h"
#include "backdrop_wgc.h"
#include "vblank.h"
#include "design_tokens.h"
#include "glass_params.h"
#include "icon_mask.h"
#include "launchpad_model.h"
#include "launchpad_density.h"
#include "desktop_settings.h"
#include "log.h"
#include "material.h"
#include "shell_items.h"
#include "custom_icons.h"

using Microsoft::WRL::ComPtr;

namespace dock::launchpad {
namespace {

constexpr UINT kMsgClose = WM_APP + 90;
constexpr UINT kMsgIcons = WM_APP + 91;
constexpr UINT kMsgApps = WM_APP + 92;
constexpr UINT kMsgOpenApp = WM_APP + 93;     // lParam: new std::string (app id), deleted by the handler
constexpr UINT kMsgMenuClosed = WM_APP + 94;  // the Dock's app menu closed
constexpr UINT_PTR kDensityTimer = 42;
constexpr wchar_t kClass[] = L"LiquidLaunchpad";

// The open Launchpad window of this process, or nullptr.
HWND OwnWindow() {
    HWND h = FindWindowW(kClass, L"DockOver Launchpad");
    if (!h) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId() ? h : nullptr;
}

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

float Ease(float v, float target, double dt, double tau) {
    const float k = static_cast<float>(1.0 - std::exp(-dt / std::max(tau, 1e-4)));
    v += (target - v) * k;
    return std::fabs(target - v) < 1e-3f ? target : v;
}

struct IconPixels {
    std::vector<uint32_t> px;
    UINT w = 0, h = 0;
    bool masked = false;  // the macOS tile mask was applied on the worker thread
    // Ready-made display sizes (premultiplied, square), resampled on the worker thread: the UI thread only uploads them.
    std::vector<uint32_t> grid, mini;
    UINT gridPx = 0, miniPx = 0;
};

// Area-weighted (box) resample of premultiplied pixels into an n x n square, the image fitted and centred: exact for
// shrinking, the case here (256 px sources to ~100 px tiles and ~20 px folder previews).
std::vector<uint32_t> AreaResample(const std::vector<uint32_t>& src, UINT w, UINT h, UINT n) {
    std::vector<uint32_t> out(static_cast<size_t>(n) * n, 0);
    if (!w || !h || !n || src.size() != static_cast<size_t>(w) * h) return out;
    const double scale = static_cast<double>(n) / (std::max)(w, h);
    const double ow = w * scale, oh = h * scale, ox = (n - ow) * 0.5, oy = (n - oh) * 0.5;
    const double step = 1.0 / scale;  // source pixels per output pixel
    for (UINT y = 0; y < n; ++y) {
        const double sy0 = (y - oy) * step, sy1 = sy0 + step;
        if (sy1 <= 0 || sy0 >= h) continue;
        for (UINT x = 0; x < n; ++x) {
            const double sx0 = (x - ox) * step, sx1 = sx0 + step;
            if (sx1 <= 0 || sx0 >= w) continue;
            double acc[4]{}, area = 0;
            for (int iy = (std::max)(0, static_cast<int>(sy0)); iy < (std::min)(static_cast<int>(h), static_cast<int>(std::ceil(sy1))); ++iy) {
                const double wy = (std::min)(sy1, iy + 1.0) - (std::max)(sy0, static_cast<double>(iy));
                if (wy <= 0) continue;
                for (int ix = (std::max)(0, static_cast<int>(sx0)); ix < (std::min)(static_cast<int>(w), static_cast<int>(std::ceil(sx1))); ++ix) {
                    const double wx = (std::min)(sx1, ix + 1.0) - (std::max)(sx0, static_cast<double>(ix));
                    if (wx <= 0) continue;
                    const double wgt = wx * wy; const uint32_t p = src[static_cast<size_t>(iy) * w + ix];
                    for (int c = 0; c < 4; ++c) acc[c] += ((p >> (8 * c)) & 255u) * wgt;
                    area += wgt;
                }
            }
            const double norm = step * step;  // coverage outside the image stays transparent
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c) v |= static_cast<uint32_t>((std::min)(255.0, acc[c] / norm + 0.5)) << (8 * c);
            (void)area;
            out[static_cast<size_t>(y) * n + x] = v;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Process-wide application list and icon cache, shared by every Launchpad opening.
class Shared {
public:
    static Shared& Get() {
        static Shared s;
        return s;
    }

    void RefreshAppsAsync() {
        std::lock_guard<std::mutex> lock(m_);
        if (discovering_ || stop_) return;
        if (discoverThread_.joinable()) discoverThread_.join();
        discovering_ = true;
        discoverThread_ = std::thread([this] { Discover(); });
    }

    std::vector<apps::AppEntry> Apps(bool* discovered) {
        std::lock_guard<std::mutex> lock(m_);
        if (discovered) *discovered = discovered_;
        return apps_;
    }

    // front: move these ids ahead of everything else (the visible page).
    void Request(const std::vector<std::pair<std::string, std::wstring>>& items, bool front) {
        std::lock_guard<std::mutex> lock(m_);
        EnsureWorkerLocked();
        std::vector<std::pair<std::string, std::wstring>> add;
        for (const auto& it : items) {
            if (icons_.count(it.first)) continue;
            if (queued_.count(it.first)) {
                if (!front) continue;
                for (auto q = queue_.begin(); q != queue_.end(); ++q)
                    if (q->first == it.first) {
                        queue_.erase(q);
                        break;
                    }
            }
            queued_.insert(it.first);
            add.push_back(it);
        }
        if (front)
            for (auto r = add.rbegin(); r != add.rend(); ++r) queue_.push_front(*r);
        else
            for (auto& a : add) queue_.push_back(a);
        if (!add.empty()) cv_.notify_one();
    }

    bool TakeIcon(const std::string& id, IconPixels& out) {
        std::lock_guard<std::mutex> lock(m_);
        auto it = icons_.find(id);
        if (it == icons_.end()) return false;
        out = it->second;
        return true;
    }

    // Icons cached before the display sizes were known (loaded at Dock start) get them in the background, so the first
    // opening does not resample them on the UI thread.
    void ResampleMissing() {
        const unsigned g = gridPx.load(), m = miniPx.load();
        if (!g || !m) return;
        std::thread([this, g, m] {
            std::vector<std::pair<std::string, IconPixels>> todo;
            {
                std::lock_guard<std::mutex> lock(m_);
                for (const auto& [id, px] : icons_) if (!px.px.empty() && (px.gridPx != g || px.miniPx != m)) todo.emplace_back(id, px);
            }
            for (auto& [id, px] : todo) {
                px.grid = AreaResample(px.px, px.w, px.h, g); px.gridPx = g;
                px.mini = AreaResample(px.px, px.w, px.h, m); px.miniPx = m;
                std::lock_guard<std::mutex> lock(m_);
                if (auto it = icons_.find(id); it != icons_.end()) it->second = std::move(px);
            }
            if (!todo.empty()) DLOG(L"launchpad: %zu cached icons resampled in the background", todo.size());
        }).detach();
    }

    size_t LoadedCount() {
        std::lock_guard<std::mutex> lock(m_);
        return icons_.size();
    }

    // The Dock's custom-icons.json. Read again on every change so Launchpad and the Dock show the same icons.
    void SetCustomIconPath(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(m_);
        iconPath_ = path;
        std::wstring error;
        if (!iconStore_.Load(iconPath_, error)) DLOG(L"launchpad: custom icons unavailable: %ls", error.c_str());
    }
    void InvalidateIcon(const std::string& stableId) {
        std::lock_guard<std::mutex> lock(m_);
        if (!iconPath_.empty()) {
            std::wstring error;
            if (!iconStore_.Load(iconPath_, error)) DLOG(L"launchpad: custom icons unavailable: %ls", error.c_str());
        }
        size_t dropped = 0;
        for (const auto& [appId, id] : stableOf_)
            if (id == stableId) dropped += icons_.erase(appId);
        if (dropped) DLOG(L"launchpad: custom icon changed, %zu cached icon(s) dropped", dropped);
    }

    void SetNotify(HWND h) { notify_.store(h); iconPosted_.store(false); }
    void IconsConsumed() { iconPosted_.store(false); }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (discoverThread_.joinable()) discoverThread_.join();
    }

    std::atomic<unsigned> iconPx{128};
    std::atomic<bool> maskIcons{false};  // Settings > Dock > macOS icon mask: applied on the worker thread, not in a frame
    std::atomic<unsigned> gridPx{0}, miniPx{0};  // display sizes the worker resamples to (0 = not known yet)

private:
    void EnsureWorkerLocked() {
        if (worker_.joinable() || stop_) return;
        worker_ = std::thread([this] { Worker(); });
    }

    void Discover() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const double t0 = Now();
        std::vector<apps::AppEntry> found;
        std::wstring error;
        const bool ok = apps::DiscoverApps(found, error);
        const double ms = (Now() - t0) * 1000.0;
        DLOG(L"launchpad: discovery %ls, %zu apps in %.0f ms", ok ? L"ok" : error.c_str(), found.size(), ms);
        std::vector<std::pair<std::string, std::wstring>> all;
        for (const auto& a : found) all.emplace_back(a.id, a.Target());
        {
            std::lock_guard<std::mutex> lock(m_);
            if (ok) {
                apps_ = std::move(found);
                discovered_ = true;
            }
            discovering_ = false;
        }
        if (ok) Request(all, false);  // background: load every icon once, in name order
        if (HWND h = notify_.load()) PostMessageW(h, kMsgApps, 0, 0);
        if (SUCCEEDED(co)) CoUninitialize();
    }

    void Worker() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::pair<std::string, std::wstring> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) break;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            PinnedItem item;
            item.target = job.second;
            ResolvePinnedItem(item, iconPx.load());
            // Same stable id as the Dock (App::ResolveItem): a custom image or edit made in either place shows in both.
            const std::string stable = ApplicationItemId(item.target, item.args);
            std::wstring customPath;
            bool hasEdit = false;
            IconEdit edit;
            {
                std::lock_guard<std::mutex> lock(m_);
                stableOf_[job.first] = stable;
                if (const auto* p = iconStore_.Find(stable)) customPath = *p;
                if (const auto* e = iconStore_.FindEdit(stable)) { edit = *e; hasEdit = true; }
            }
            if (!customPath.empty()) {
                std::vector<uint32_t> custom;
                UINT cw = 0, ch = 0;
                std::wstring error;
                if (LoadCustomIconPixels(customPath, custom, cw, ch, error)) {
                    item.pixels = std::move(custom);
                    item.iconW = cw;
                    item.iconH = ch;
                } else {
                    DLOG(L"launchpad: custom icon unavailable (%ls)", error.c_str());
                }
            }
            IconPixels px;
            px.px = std::move(item.pixels);
            px.w = item.iconW;
            px.h = item.iconH;
            if (hasEdit && !px.px.empty()) px.px = ApplyIconEdit(px.px, px.w, px.h, edit);
            const bool mask = hasEdit && edit.mask >= 0 ? edit.mask != 0 : maskIcons.load();
            if (mask && !px.px.empty()) {  // the macOS tile mask is CPU work: here, off the UI thread
                unsigned side = 0;
                auto masked = MakeMacStyleIcon(px.px, px.w, px.h, true, &side, true);  // the Launchpad background is dark
                if (side) { px.px = std::move(masked); px.w = px.h = side; }
                px.masked = true;
            }
            if (const unsigned g = gridPx.load(), m = miniPx.load(); g && m && !px.px.empty()) {
                px.grid = AreaResample(px.px, px.w, px.h, g); px.gridPx = g;
                px.mini = AreaResample(px.px, px.w, px.h, m); px.miniPx = m;
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                icons_[job.first] = std::move(px);
                queued_.erase(job.first);
            }
            if (HWND h = notify_.load())
                if (!iconPosted_.exchange(true)) PostMessageW(h, kMsgIcons, 0, 0);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::vector<apps::AppEntry> apps_;
    bool discovered_ = false, discovering_ = false, stop_ = false;
    std::unordered_map<std::string, IconPixels> icons_;
    std::wstring iconPath_;
    CustomIconStore iconStore_;
    std::unordered_map<std::string, std::string> stableOf_;  // Launchpad app id -> Dock stable id
    std::deque<std::pair<std::string, std::wstring>> queue_;
    std::unordered_set<std::string> queued_;
    std::thread worker_, discoverThread_;
    std::atomic<HWND> notify_{nullptr};
    std::atomic<bool> iconPosted_{false};
};

// ---------------------------------------------------------------------------------------------
struct Item {
    std::string key;     // node key (app id or "folder:<id>")
    std::string appId;   // for apps
    int node = -1;       // index in layout (-1 for search results)
    bool folder = false;
};

class Launchpad {
public:
    int Run(HWND dock, const std::wstring& layoutPath, const std::wstring& settingsPath);
    HWND hwnd = nullptr;

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);

    bool CaptureDesktop();
    HRESULT InitGraphics();
    HRESULT BuildBackground();
    void StartLiveBackground();
    bool RebuildLiveBackground();  // a newer captured frame -> blurred background (and glass backdrop)
    void ComputeGeometry();
    void RefreshDensity();
    void ApplyApps();
    void RebuildItems();
    void RequestVisibleIcons();
    void UploadIcons();
    void SaveLayout(const wchar_t* reason);
    bool Tick(double dt);
    void Frame();
    void Render();
    void DrawGrid(float alpha);
    void DrawIcon(const Item& it, D2D1_RECT_F r, float opacity, bool highlight);
    void DrawAppIcon(const std::string& appId, D2D1_RECT_F r, float opacity);
    void DrawLabel(const std::string& key, const std::wstring& text, D2D1_RECT_F icon, float opacity);
    bool DrawGlass(D2D1_RECT_F r, float radius, float appear, float dim = 1.0f);
    void DrawFolderContents(float alpha);
    D2D1_RECT_F SlotRect(int slot) const;              // virtual coordinates (page 0 origin)
    D2D1_RECT_F FolderPanel() const;
    D2D1_RECT_F FolderSlotRect(int i) const;
    D2D1_RECT_F SearchRect() const;
    int SlotAt(float x, float y, bool* center) const;  // screen DIPs, current page
    int FolderSlotAt(float x, float y) const;
    int ItemAt(float x, float y) const;
    std::wstring NameOf(const std::string& appId) const;
    std::wstring FolderName(int node) const;
    void Activate(int itemIndex);
    void Launch(const std::string& appId);
    void Close(const wchar_t* reason);
    void OpenFolder(int node);
    void CloseFolder();
    void CommitRename();
    void StartDrag();
    void UpdateDrag();
    void EndDrag();
    void SetPage(int page);
    void UpdateSearch();
    float Dip(int px) const { return static_cast<float>(px) / scale_; }

    HWND dock_ = nullptr;
    std::wstring layoutPath_, settingsPath_;
    bool maskIcons_ = false, hideLabels_ = false;
    bool showWindows_ = false;  // Settings > Launchpad > Show windows behind Launchpad (off: wallpaper only, live)  // Settings > Dock > macOS icon mask: the same rounded-square tiles as the Dock (reference look)
    liquid::settings::Revision settingsRevision_;
    DensityConfig density_;
    int cols_=7,rows_=5,perPage_=35;
    RECT mon_{};
    float scale_ = 1.0f, W_ = 0, H_ = 0;
    UINT pxW_ = 0, pxH_ = 0;
    double openedAt_ = 0, lastT_ = 0;

    // Graphics
    std::vector<uint32_t> snapshot_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIDevice> dxgi_;
    ComPtr<IDXGISwapChain1> swap_;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1PathGeometry> folderShape_;  // macOS icon outline (icon_mask.h), centred, half-size 1
    ID2D1Geometry* FolderShape();
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> labelFmt_, searchFmt_, titleFmt_, infoFmt_;
    ComPtr<IDWriteInlineObject> ellipsis_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID3D11Texture2D> bgTex_;
    ComPtr<ID2D1Bitmap1> bgBmp_;
    std::unique_ptr<IPlateMaterial> glass_;
    bool glassOk_ = false;
    // Live background: Windows Graphics Capture of the monitor (windows shown) or of the wallpaper layer (wallpaper only,
    // Wallpaper Engine included), blurred again whenever a new frame arrives. Declared after the device: destroyed first.
    WgcBackdropSource live_;
    docky::vblank::Ticker ticker_;  // paces animated frames
    // Frame profile (log): time per phase of the current frame, frames over one display frame, worst frame.
    double phase_[6]{};  // 0 tick, 1 live background, 2 pass 1 (grid), 3 glass, 4 pass 3, 5 present
    double uploadMs_ = 0; unsigned slowFrames_ = 0, slowLogged_ = 0;
    struct SlowFrame { double interval, total; double ph[6]; double icons; float page; double wait, dispatch; unsigned msgs; double loop; };
    double waitMs_ = 0, dispatchMs_ = 0, peekMs_ = 0; unsigned dispatched_ = 0;
    std::vector<SlowFrame> worst_;  // the 12 longest frame intervals of the session
    std::map<UINT, std::pair<double, unsigned>> slowMsgs_;  // message -> (longest ms, count over 2 ms)
public:
    std::map<UINT, std::pair<double, unsigned>> slowProc_;  // window procedure, sent messages included
private:
    bool liveDirty_ = false;
    std::uint64_t bgGeneration_ = 1;  // glass backdrop frame id: changes with every rebuilt background
    ComPtr<ID2D1Bitmap1> liveSrc_; ID3D11Texture2D* liveSrcTex_ = nullptr;
    unsigned liveRebuilds_ = 0;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>> icons_;  // full-size source bitmaps (null = no icon)
    // Icons resampled once to their exact display size (grid icon, folder preview icon) and drawn with a plain linear
    // blit. Drawing the 256 px sources with HIGH_QUALITY_CUBIC every frame was a multi-pass GPU resample per icon: the
    // grid alone held Launchpad near 200 fps with hitches while paging (measured 2026-11-12).
    struct Sized { ComPtr<ID2D1Bitmap1> grid, mini; };
    std::unordered_map<std::string, Sized> sized_;
    UINT gridPx_ = 0, miniPx_ = 0;
    bool drawing_ = false;  // inside Render's BeginDraw/EndDraw
    bool iconsPending_ = false;  // loaded icons waiting to be uploaded (one batch per frame)
    unsigned uiMasked_ = 0, workerSized_ = 0, gpuSized_ = 0;  // icon preparation paths (log)
    ComPtr<ID2D1Bitmap1> Resample(ID2D1Bitmap1* src, UINT px);
    const Sized* SizedIcon(const std::string& appId);
    UINT MiniPx() const { return (std::max)(8u, static_cast<UINT>(std::lround((icon_ - 2 * icon_ * 0.155f - 2 * icon_ * 0.055f) / 3.0f * scale_))); }
    std::unordered_map<std::string, ComPtr<IDWriteTextLayout>> labels_;

    // Data
    std::vector<apps::AppEntry> apps_;
    std::unordered_map<std::string, size_t> appIndex_;
    bool appsKnown_ = false;
    Layout layout_;
    bool layoutLoaded_ = false;
    std::vector<Item> items_;
    std::unordered_map<std::string, D2D1_POINT_2F> pos_;  // eased virtual positions

    // Geometry
    float icon_ = 96, cellW_ = 180, cellH_ = 180, gridLeft_ = 0, gridTop_ = 0, labelH_ = 18;

    // State
    float openT_ = 0, openTarget_ = 1;
    bool closing_ = false, reducedMotion_ = false;
    float page_ = 0;
    int pageTarget_ = 0, pageCount_ = 1;
    std::wstring query_;
    int folder_ = -1;          // layout index of the open (or closing) folder
    bool folderOpen_ = false;
    float folderT_ = 0;
    bool renaming_ = false;
    std::wstring renameText_;
    bool animating_ = true, needsFrame_ = true;
    unsigned long long frames_ = 0, animFrames_ = 0;
    double animSeconds_ = 0, maxInterval_ = 0;

    // Pointer / drag
    float mx_ = 0, my_ = 0, pressX_ = 0, pressY_ = 0;
    bool lbDown_ = false, swipe_ = false, dragActive_ = false, dragFromFolder_ = false;
    bool menuOpen_ = false;    // the Dock shows this Launchpad's app menu (right-click)
    double menuClosedAt_ = 0;  // the click that closed the menu must not also act on Launchpad
    void ShowAppMenu(float x, float y);
    int pressItem_ = -1, pressFolderSlot_ = -1;
    int selected_ = -1;  // keyboard selection (item index), -1 = none
    bool pressOnTitle_ = false, pressInPanel_ = false, pressOnSearch_ = false;
    float swipeStartPage_ = 0, grabX_ = 0, grabY_ = 0;
    std::string dragKey_;
    int dragNode_ = -1, dragFolderSlot_ = -1;
    int folderCandidate_ = -1, reorderSlot_ = -1;
    float restX_ = 0, restY_ = 0;  // pointer position where the current reorder dwell started
    double candidateSince_ = 0, edgeSince_ = 0, reorderSince_ = 0;
    bool folderHover_ = false;
    float wheelAcc_ = 0;
    double lastWheelFlip_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The window that shows the wallpaper without any app window: Progman (Windows 11 24H2 and later host the wallpaper,
// Wallpaper Engine's included, in a WorkerW child of Progman). Older builds use a separate top-level WorkerW without the
// desktop icon view behind Progman.
HWND WallpaperWindow() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman && FindWindowExW(progman, nullptr, L"WorkerW", nullptr)) return progman;
    HWND found = nullptr;
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        wchar_t cls[32]{}; GetClassNameW(w, cls, 32);
        if (!wcscmp(cls, L"WorkerW") && IsWindowVisible(w) && !FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(l) = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found ? found : progman;
}

bool Launchpad::CaptureDesktop() {
    // Snapshot of the monitor before Launchpad appears. GDI capture honours WDA_EXCLUDEFROMCAPTURE,
    // so the Dock and Menu Bar are not part of it; the image is blurred once and stays static while
    // Launchpad is open (as on macOS).
    const int w = mon_.right - mon_.left, h = mon_.bottom - mon_.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        HWND wallpaper = showWindows_ ? nullptr : WallpaperWindow();
        RECT wr{};
        if (wallpaper && GetWindowRect(wallpaper, &wr)) {  // wallpaper only: the wallpaper layer drawn by itself, no app windows
            SetViewportOrgEx(mem, wr.left - mon_.left, wr.top - mon_.top, nullptr);
            ok = PrintWindow(wallpaper, mem, PW_RENDERFULLCONTENT) != FALSE;
            SetViewportOrgEx(mem, 0, 0, nullptr);
        } else ok = BitBlt(mem, 0, 0, w, h, screen, mon_.left, mon_.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        snapshot_.assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + static_cast<size_t>(w) * h);
        for (uint32_t& p : snapshot_) p |= 0xFF000000u;
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) snapshot_.assign(static_cast<size_t>(w) * h, 0xFF15161Au);
    return ok;
}

HRESULT Launchpad::InitGraphics() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, &ctx_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, &ctx_);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = d3d_.As(&dxgi_))) return hr;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return hr;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = pxW_;
    sd.Height = pxH_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(hr = factory->CreateSwapChainForComposition(dxgi_.Get(), &sd, nullptr, &swap_))) return hr;
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                      reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))))
        return hr;
    if (FAILED(hr = d2dFactory_->CreateDevice(dxgi_.Get(), &d2dDevice_))) return hr;
    if (FAILED(hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return hr;
    ComPtr<IDXGISurface> surface;
    if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return hr;
    const float dpi = 96.0f * scale_;
    const auto tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surface.Get(), &tp, &target_))) return hr;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return hr;
    if (FAILED(hr = d3d_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) return hr;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(hr = DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_)))) return hr;
    if (FAILED(hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) return hr;
    if (FAILED(hr = dcomp_->CreateVisual(&visual_))) return hr;
    visual_->SetContent(swap_.Get());
    dcompTarget_->SetRoot(visual_.Get());
    dcomp_->Commit();
    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return hr;
    auto fmt = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        return dwrite_->CreateTextFormat(liquid::design::type::Family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
    };
    namespace type = liquid::design::type;
    if (FAILED(hr = fmt(type::Label, DWRITE_FONT_WEIGHT_MEDIUM, labelFmt_))) return hr;
    if (FAILED(hr = fmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL, searchFmt_))) return hr;  // small pill (macOS 27 reference)
    if (FAILED(hr = fmt(type::Title, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFmt_))) return hr;
    if (FAILED(hr = fmt(type::Field, DWRITE_FONT_WEIGHT_MEDIUM, infoFmt_))) return hr;
    for (IDWriteTextFormat* f : {labelFmt_.Get(), titleFmt_.Get()}) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    searchFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    searchFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    infoFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(labelFmt_.Get(), &ellipsis_))) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        labelFmt_->SetTrimming(&trim, ellipsis_.Get());
    }
    if (FAILED(hr = dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) return hr;
    glass_ = CreateLiquidGlassMaterial();
    glassOk_ = glass_ && SUCCEEDED(glass_->CreateGpuResources(d3d_.Get()));
    return BuildBackground();
}

HRESULT Launchpad::BuildBackground() {
    // Blurred, darkened desktop rendered once into a texture that is also the Liquid Glass backdrop,
    // so the search field and folder panel refract exactly what the user sees behind them.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = pxW_;
    td.Height = pxH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = d3d_->CreateTexture2D(&td, nullptr, &bgTex_);
    if (FAILED(hr)) return hr;
    ComPtr<IDXGISurface> surf;
    bgTex_.As(&surf);
    const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bgBmp_))) return hr;
    ComPtr<ID2D1Bitmap1> src;
    const auto sp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(hr = dc_->CreateBitmap(D2D1::SizeU(pxW_, pxH_), snapshot_.data(), pxW_ * 4, &sp, &src))) return hr;
    ComPtr<ID2D1Effect> blur;
    if (FAILED(hr = dc_->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return hr;
    blur->SetInput(0, src.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 34.0f * scale_);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
    dc_->SetTarget(bgBmp_.Get());
    dc_->SetDpi(96, 96);
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 1));
    dc_->DrawImage(blur.Get());
    brush_->SetColor(D2D1::ColorF(0.02f, 0.02f, 0.04f, 0.30f));
    dc_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(pxW_), static_cast<float>(pxH_)), brush_.Get());
    hr = dc_->EndDraw();
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(96.0f * scale_, 96.0f * scale_);
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    return hr;
}

void Launchpad::StartLiveBackground() {
    live_.SetDevice(d3d_.Get());
    live_.SetExcludedWindow(hwnd);  // Launchpad itself is never in the image; capture messages come here
    HWND source = showWindows_ ? nullptr : WallpaperWindow();
    live_.SetSourceWindow(source);
    live_.Request(true, MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
    DLOG(L"launchpad: live background %ls", showWindows_ ? L"of the screen (windows shown)" : source ? L"of the wallpaper layer (windows hidden)" : L"unavailable");
}

bool Launchpad::RebuildLiveBackground() {
    liveDirty_ = false;
    BackdropFrame frame;
    if (!bgBmp_ || !live_.Latest(mon_, frame) || !frame.texture) return false;
    if (frame.texture != liveSrcTex_ || !liveSrc_) {
        liveSrc_.Reset(); liveSrcTex_ = nullptr;
        ComPtr<IDXGISurface> surf;
        if (FAILED(frame.texture->QueryInterface(IID_PPV_ARGS(&surf)))) return false;
        const auto sp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &sp, &liveSrc_))) return false;
        liveSrcTex_ = frame.texture;
    }
    // The background is blurred heavily (sigma 34 DIP), so it is blurred at a quarter of the resolution and scaled back up:
    // the same look for a sixteenth of the work, every new frame.
    constexpr float kQ = 0.25f;
    ComPtr<ID2D1Effect> down, blur;
    if (FAILED(dc_->CreateEffect(CLSID_D2D1Scale, &down)) || FAILED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return false;
    down->SetInput(0, liveSrc_.Get());
    down->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(kQ, kQ));
    down->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_LINEAR);
    blur->SetInputEffect(0, down.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 34.0f * scale_ * kQ);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
    dc_->SetTarget(bgBmp_.Get());
    dc_->SetDpi(96, 96);
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 1));
    // Frame pixels start at frame.screenRect's corner; the background covers mon_.
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / kQ, 1.0f / kQ) *
                      D2D1::Matrix3x2F::Translation(static_cast<float>(frame.screenRect.left - mon_.left), static_cast<float>(frame.screenRect.top - mon_.top)));
    dc_->DrawImage(blur.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    brush_->SetColor(D2D1::ColorF(0.02f, 0.02f, 0.04f, 0.30f));
    dc_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(pxW_), static_cast<float>(pxH_)), brush_.Get());
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(96.0f * scale_, 96.0f * scale_);
    ++bgGeneration_; ++liveRebuilds_;
    return SUCCEEDED(hr);
}

void Launchpad::ComputeGeometry() {
    gridTop_ = H_ * 0.13f; labelH_ = 18.0f;
    if(density_.preset==DensityPreset::Default){
        // macOS 27 Launchpad reference (2026-11-10): icons 4.0% of the width, columns 9.83% of the width apart, rows 17.6%
        // of the height apart, first row's icon centre at 16.1% of the height; 7 x 5.
        cols_=7;rows_=5;icon_=std::clamp(std::min(W_*.040f,H_*.071f),56.f,120.f);
        cellW_=std::max(icon_*1.3f,W_*.0983f);cellH_=std::max(icon_+labelH_+16.f,H_*.176f);gridLeft_=(W_-cellW_*cols_)*.5f;
        gridTop_=H_*.161f-icon_*.5f-(cellH_-icon_-labelH_-8.0f)*.5f;
    }else{
        const auto grid=PlanDensity(W_,H_,density_);cols_=grid.columns;rows_=grid.rows;
        icon_=grid.icon;cellW_=grid.pitchX;cellH_=grid.pitchY;gridLeft_=(W_-grid.width)*.5f;
        gridTop_=H_*.513f-grid.height*.5f;  // same vertical centre as the default layout (search pill above, dots below)
    }
    perPage_=cols_*rows_;
    // Diagnostic geometry (DIP, window-relative) so tests can target real cells without guessing.
    DLOG(L"launchpad: grid %dx%d cell %.1fx%.1f origin %.1f,%.1f icon %.1f label %.1f", cols_, rows_, cellW_, cellH_, gridLeft_, gridTop_, icon_, labelH_);
}
void Launchpad::RefreshDensity(){
    const auto loaded=liquid::settings::Load(settingsPath_);
    settingsRevision_=liquid::settings::ReadRevision(settingsPath_);
    if(loaded.value.launchpad.hideLabels!=hideLabels_){hideLabels_=loaded.value.launchpad.hideLabels;needsFrame_=true;}
    if(loaded.status!=liquid::settings::LoadStatus::Loaded && loaded.status!=liquid::settings::LoadStatus::Missing)return;
    const auto& in=loaded.value.launchpad;DensityConfig next;
    next.preset=in.densityPreset==1?DensityPreset::Compact:in.densityPreset==2?DensityPreset::Spacious:in.densityPreset==3?DensityPreset::Custom:DensityPreset::Default;
    next.icon=static_cast<float>(in.iconSize);next.gapX=static_cast<float>(in.gapX);next.gapY=static_cast<float>(in.gapY);
    if(next.preset==density_.preset&&next.icon==density_.icon&&next.gapX==density_.gapX&&next.gapY==density_.gapY)return;
    density_=next;if(W_<=0||H_<=0)return;
    ComputeGeometry();Shared::Get().iconPx.store(static_cast<unsigned>(std::clamp(std::ceil(icon_*scale_),64.f,256.f)));
    pos_.clear();labels_.clear();RebuildItems();page_=static_cast<float>(pageTarget_);selected_=-1;needsFrame_=true;
}

D2D1_RECT_F Launchpad::SlotRect(int slot) const {
    const int page = slot / perPage_, idx = slot % perPage_, c = idx % cols_, r = idx / cols_;
    const float x = page * W_ + gridLeft_ + c * cellW_ + (cellW_ - icon_) * 0.5f;
    const float y = gridTop_ + r * cellH_ + (cellH_ - icon_ - labelH_ - 8.0f) * 0.5f;
    return D2D1::RectF(x, y, x + icon_, y + icon_);
}

D2D1_RECT_F Launchpad::SearchRect() const {
    // Reference: a small pill at the top centre, 9.2% of the width, 2.8% of the height, 3.2% from the top.
    const float w = std::clamp(W_ * 0.092f, 220.0f, 360.0f), h = std::clamp(H_ * 0.0276f, 28.0f, 40.0f), y = std::max(16.0f, H_ * 0.032f);
    return D2D1::RectF((W_ - w) * 0.5f, y, (W_ + w) * 0.5f, y + h);
}

D2D1_RECT_F Launchpad::FolderPanel() const {
    const int n = (folder_ >= 0 && folder_ < static_cast<int>(layout_.nodes.size())) ? static_cast<int>(layout_.nodes[folder_].apps.size()) : 0;
    const int cols = std::clamp(n, 3, 5), rows = std::clamp((n + cols - 1) / cols, 1, 5);
    const float w = cols * cellW_ * 0.92f + 56.0f, h = 76.0f + rows * cellH_ * 0.92f + 28.0f;
    const float x = (W_ - w) * 0.5f, y = std::max(H_ * 0.12f, (H_ - h) * 0.5f - 30.0f);
    return D2D1::RectF(x, y, x + w, y + h);
}

D2D1_RECT_F Launchpad::FolderSlotRect(int i) const {
    const D2D1_RECT_F p = FolderPanel();
    const int n = static_cast<int>(layout_.nodes[folder_].apps.size());
    const int cols = std::clamp(n, 3, 5);
    const float cw = cellW_ * 0.92f, ch = cellH_ * 0.92f, ic = icon_ * 0.92f;
    const float x = p.left + 28.0f + (i % cols) * cw + (cw - ic) * 0.5f;
    const float y = p.top + 76.0f + (i / cols) * ch + (ch - ic - labelH_ - 8.0f) * 0.5f;
    return D2D1::RectF(x, y, x + ic, y + ic);
}

int Launchpad::SlotAt(float x, float y, bool* center) const {
    if (center) *center = false;
    const float lx = x - gridLeft_, ly = y - gridTop_;
    if (lx < 0 || ly < 0 || lx >= cellW_ * cols_ || ly >= cellH_ * rows_) return -1;
    const int c = static_cast<int>(lx / cellW_), r = static_cast<int>(ly / cellH_);
    const int slot = pageTarget_ * perPage_ + r * cols_ + c;
    if (center) {
        const D2D1_RECT_F ir = SlotRect(slot);
        const float cx = (ir.left + ir.right) * 0.5f - pageTarget_ * W_, cy = (ir.top + ir.bottom) * 0.5f;
        *center = std::hypot(x - cx, y - cy) < icon_ * 0.30f;
    }
    return slot;
}

int Launchpad::ItemAt(float x, float y) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i / perPage_ != pageTarget_) continue;
        D2D1_RECT_F r = SlotRect(i);
        r.left -= page_ * W_;
        r.right -= page_ * W_;
        if (x >= r.left - 10 && x <= r.right + 10 && y >= r.top - 6 && y <= r.bottom + labelH_ + 10) return i;
    }
    return -1;
}

int Launchpad::FolderSlotAt(float x, float y) const {
    if (folder_ < 0) return -1;
    const int n = static_cast<int>(layout_.nodes[folder_].apps.size());
    for (int i = 0; i < n && i < 25; ++i) {
        const D2D1_RECT_F r = FolderSlotRect(i);
        if (x >= r.left - 12 && x <= r.right + 12 && y >= r.top - 8 && y <= r.bottom + labelH_ + 8) return i;
    }
    return -1;
}

std::wstring Launchpad::NameOf(const std::string& appId) const {
    auto it = appIndex_.find(appId);
    return it == appIndex_.end() ? std::wstring() : apps_[it->second].name;
}

std::wstring Launchpad::FolderName(int node) const {
    const std::wstring& n = layout_.nodes[node].folderName;
    return n.empty() ? std::wstring(L"Folder") : n;
}

// ---------------------------------------------------------------------------------------------
void Launchpad::ApplyApps() {
    bool known = false;
    apps_ = Shared::Get().Apps(&known);
    appsKnown_ = known;
    appIndex_.clear();
    std::vector<std::string> ids;
    for (size_t i = 0; i < apps_.size(); ++i) {
        appIndex_[apps_[i].id] = i;
        ids.push_back(apps_[i].id);
    }
    if (!known) return;
    if (!layoutLoaded_) {
        Layout saved;
        std::wstring error;
        bool exists = false;
        if (!Load(layoutPath_, saved, error, &exists)) {
            const std::wstring backup = layoutPath_ + L".corrupt";
            MoveFileExW(layoutPath_.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
            DLOG(L"launchpad: layout unusable (%ls); kept as .corrupt, using alphabetical order", error.c_str());
            saved = Layout{};
            exists = false;  // start over: the default below is written to a fresh file
        }
        layoutLoaded_ = true;
        layout_ = Merge(saved, ids);
        DLOG(L"launchpad: layout %ls, %zu nodes", exists ? L"loaded" : L"default (alphabetical)", layout_.nodes.size());
        if (!exists) SaveLayout(L"initial");  // first opening records the alphabetical order
    } else if (!dragActive_) {
        layout_ = Merge(layout_, ids);
    }
    RebuildItems();
}

void Launchpad::RebuildItems() {
    items_.clear();
    if (query_.empty()) {
        for (int i = 0; i < static_cast<int>(layout_.nodes.size()); ++i) {
            const Node& n = layout_.nodes[i];
            Item it;
            it.key = n.Key();
            it.folder = n.folder;
            it.appId = n.appId;
            it.node = i;
            items_.push_back(std::move(it));
        }
    } else {
        for (const Node& n : layout_.nodes) {
            auto consider = [&](const std::string& id) {
                if (apps::MatchesQuery(NameOf(id), query_)) {
                    Item it;
                    it.key = id;
                    it.appId = id;
                    items_.push_back(std::move(it));
                }
            };
            if (n.folder)
                for (const auto& a : n.apps) consider(a);
            else
                consider(n.appId);
        }
    }
    pageCount_ = std::max(1, static_cast<int>((items_.size() + perPage_ - 1) / perPage_));
    if (pageTarget_ >= pageCount_) pageTarget_ = pageCount_ - 1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        if (!pos_.count(items_[i].key)) {
            const D2D1_RECT_F r = SlotRect(i);
            pos_[items_[i].key] = D2D1::Point2F(r.left, r.top);
        }
    RequestVisibleIcons();
    needsFrame_ = true;
}

void Launchpad::RequestVisibleIcons() {
    std::vector<std::pair<std::string, std::wstring>> want;
    auto add = [&](const std::string& id) {
        auto it = appIndex_.find(id);
        if (it != appIndex_.end() && !icons_.count(id)) want.emplace_back(id, apps_[it->second].Target());
    };
    const int first = pageTarget_ * perPage_, last = std::min<int>(static_cast<int>(items_.size()), first + perPage_);
    for (int i = first; i < last; ++i) {
        if (!items_[i].folder) add(items_[i].appId);
        else
            for (const auto& a : layout_.nodes[items_[i].node].apps) add(a);
    }
    if (folder_ >= 0 && folder_ < static_cast<int>(layout_.nodes.size()))
        for (const auto& a : layout_.nodes[folder_].apps) add(a);
    if (!want.empty()) Shared::Get().Request(want, true);
    iconsPending_ = true; needsFrame_ = true;
}

void Launchpad::UploadIcons() {
    iconsPending_ = false;
    const double t0 = Now();
    struct Done { Launchpad* self; double t0; ~Done() { self->uploadMs_ += (Now() - t0) * 1000.0; } } done{this, t0};
    Shared::Get().IconsConsumed();
    int uploaded = 0;
    for (const auto& a : apps_) {
        if (icons_.count(a.id)) continue;
        IconPixels px;
        if (!Shared::Get().TakeIcon(a.id, px)) continue;
        ComPtr<ID2D1Bitmap1> bmp;
        if (maskIcons_ && !px.masked && !px.px.empty()) {  // only icons loaded before the worker knew the setting
            ++uiMasked_;
            unsigned side = 0;
            auto masked = MakeMacStyleIcon(px.px, px.w, px.h, true, &side, true);  // the Launchpad background is dark
            if (side) { px.px = std::move(masked); px.w = px.h = side; }
        }
        if (!px.px.empty()) {
            const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            dc_->CreateBitmap(D2D1::SizeU(px.w, px.h), px.px.data(), px.w * 4, &bp, &bmp);
        }
        icons_[a.id] = bmp;  // null = no icon available; placeholder stays
        if (bmp) {
            const UINT g = static_cast<UINT>(std::lround(icon_ * scale_)), m = MiniPx();
            if (g != gridPx_ || m != miniPx_) { sized_.clear(); gridPx_ = g; miniPx_ = m; }
            if (px.gridPx == g && px.miniPx == m && !px.grid.empty() && !px.mini.empty()) {  // made by the worker: upload only
                const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
                Sized sz;
                dc_->CreateBitmap(D2D1::SizeU(g, g), px.grid.data(), g * 4, &bp, &sz.grid);
                dc_->CreateBitmap(D2D1::SizeU(m, m), px.mini.data(), m * 4, &bp, &sz.mini);
                sized_[a.id] = std::move(sz);
                ++workerSized_;
            } else { ++gpuSized_; SizedIcon(a.id); }  // sizes unknown to the worker: resample on the GPU now, not in the frame that shows it
        }
        // At most ~2 ms per batch, then the rest after the next frame: a 24-icon batch took up to 9 ms (a dropped frame).
        if (++uploaded >= 24 || (Now() - t0) * 1000.0 > 2.0) {
            iconsPending_ = true;  // continue with the next batch after the next frame
            break;
        }
    }
    if (uploaded) needsFrame_ = true;
}

void Launchpad::SaveLayout(const wchar_t* reason) {
    std::wstring error;
    if (Save(layoutPath_, layout_, error))
        DLOG(L"launchpad: layout saved (%ls), %zu nodes", reason, layout_.nodes.size());
    else
        DLOG(L"launchpad: layout save FAILED (%ls): %ls", reason, error.c_str());
}

// ---------------------------------------------------------------------------------------------
void Launchpad::SetPage(int page) {
    const int p = std::clamp(page, 0, pageCount_ - 1);
    if (p != pageTarget_) {
        pageTarget_ = p;
        DLOG(L"launchpad: page %d/%d", p + 1, pageCount_);
        RequestVisibleIcons();
    }
    needsFrame_ = true;
}

void Launchpad::UpdateSearch() {
    pageTarget_ = 0;
    page_ = 0;
    RebuildItems();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const D2D1_RECT_F r = SlotRect(i);
        pos_[items_[i].key] = D2D1::Point2F(r.left, r.top);  // results appear in place, no fly-in
    }
    selected_ = (!query_.empty() && !items_.empty()) ? 0 : -1;  // first result is pre-selected, as on macOS
    DLOG(L"launchpad: search returned %zu results", items_.size());
}

void Launchpad::Launch(const std::string& appId) {
    auto it = appIndex_.find(appId);
    if (it == appIndex_.end()) return;
    PinnedItem p;
    p.target = apps_[it->second].Target();
    p.label = apps_[it->second].name;
    DLOG(L"launchpad: launching '%ls'", p.label.c_str());
    AllowSetForegroundWindow(ASFW_ANY);
    LaunchPinnedItemAsync(p);
    Close(L"launch");
}

// Right-click on an application (grid, search results or an open folder): the Dock shows the app menu, so
// Launchpad and the Dock offer the same commands (open, keep in Dock, custom icon, file location).
void Launchpad::ShowAppMenu(float x, float y) {
    if (closing_ || !dock_) return;
    std::string appId;
    if (folderOpen_) {
        const int slot = FolderSlotAt(x, y);
        if (folder_ >= 0 && folder_ < static_cast<int>(layout_.nodes.size()) && slot >= 0 &&
            slot < static_cast<int>(layout_.nodes[folder_].apps.size()))
            appId = layout_.nodes[folder_].apps[slot];
    } else {
        const int index = ItemAt(x, y);
        if (index >= 0 && index < static_cast<int>(items_.size()) && !items_[index].folder) appId = items_[index].appId;
    }
    const auto it = appIndex_.find(appId);
    if (appId.empty() || it == appIndex_.end()) return;
    auto request = std::make_unique<AppMenuRequest>();
    request->appId = appId;
    request->target = apps_[it->second].Target();
    request->name = apps_[it->second].name;
    GetCursorPos(&request->screen);
    if (PostMessageW(dock_, kMsgLaunchpadAppMenu, 0, reinterpret_cast<LPARAM>(request.get()))) {
        request.release();
        menuOpen_ = true;
        DLOG(L"launchpad: app menu for '%ls'", apps_[it->second].name.c_str());
    }
}

void Launchpad::Activate(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items_.size())) return;
    const Item& it = items_[itemIndex];
    if (it.folder) OpenFolder(it.node);
    else Launch(it.appId);
}

void Launchpad::Close(const wchar_t* reason) {
    if (closing_) return;
    if (renaming_) CommitRename();
    if (!closing_) PostMessageW(dock_, kMsgLaunchpadCovers, 0, 0);  // the Dock resumes its capture during the fade-out
    closing_ = true;
    openTarget_ = 0;
    animating_ = true;
    DLOG(L"launchpad: closing (%ls)", reason);
}

void Launchpad::OpenFolder(int node) {
    if (node < 0 || node >= static_cast<int>(layout_.nodes.size()) || !layout_.nodes[node].folder) return;
    folder_ = node;
    folderOpen_ = true;
    renaming_ = false;
    animating_ = true;
    RequestVisibleIcons();
    DLOG(L"launchpad: folder opened '%ls' (%zu apps)", FolderName(node).c_str(), layout_.nodes[node].apps.size());
}

void Launchpad::CloseFolder() {
    if (!folderOpen_) return;
    if (renaming_) CommitRename();
    folderOpen_ = false;
    animating_ = true;
    DLOG(L"launchpad: folder closed");
}

void Launchpad::CommitRename() {
    renaming_ = false;
    if (folder_ >= 0 && RenameFolder(layout_, folder_, renameText_)) {
        DLOG(L"launchpad: folder renamed to '%ls'", layout_.nodes[folder_].folderName.c_str());
        labels_.clear();
        SaveLayout(L"rename");
    }
    needsFrame_ = true;
}

// ---------------------------------------------------------------------------------------------
void Launchpad::StartDrag() {
    dragActive_ = true;
    folderCandidate_ = -1;
    reorderSlot_ = -1;
    folderHover_ = false;
    edgeSince_ = 0;
    if (dragFromFolder_) {
        dragFolderSlot_ = pressFolderSlot_;
        dragKey_ = layout_.nodes[folder_].apps[dragFolderSlot_];
        const D2D1_RECT_F r = FolderSlotRect(dragFolderSlot_);
        grabX_ = pressX_ - r.left;
        grabY_ = pressY_ - r.top;
        DLOG(L"launchpad: drag start in folder '%ls'", NameOf(dragKey_).c_str());
    } else {
        dragNode_ = items_[pressItem_].node;
        dragKey_ = items_[pressItem_].key;
        D2D1_RECT_F r = SlotRect(pressItem_);
        grabX_ = pressX_ - (r.left - page_ * W_);
        grabY_ = pressY_ - r.top;
        DLOG(L"launchpad: drag start '%ls'", items_[pressItem_].folder ? FolderName(dragNode_).c_str() : NameOf(dragKey_).c_str());
    }
}

void Launchpad::UpdateDrag() {
    const double now = Now();
    if (dragFromFolder_) {
        D2D1_RECT_F p = FolderPanel();
        const bool inside = mx_ >= p.left - 24 && mx_ <= p.right + 24 && my_ >= p.top - 24 && my_ <= p.bottom + 24;
        if (inside) {
            const int s = FolderSlotAt(mx_, my_);
            if (s >= 0 && s != dragFolderSlot_ && MoveWithinFolder(layout_, folder_, dragFolderSlot_, s)) dragFolderSlot_ = s;
            return;
        }
        // Dragged out of the folder: becomes a top-level item at the hovered slot.
        int insertAt = SlotAt(mx_, my_, nullptr);
        if (insertAt < 0 || insertAt > static_cast<int>(layout_.nodes.size())) insertAt = static_cast<int>(layout_.nodes.size());
        MoveOutOfFolder(layout_, folder_, dragFolderSlot_, insertAt);
        folderOpen_ = false;
        folderT_ = 0;
        folder_ = -1;
        dragFromFolder_ = false;
        RebuildItems();
        for (int i = 0; i < static_cast<int>(layout_.nodes.size()); ++i)
            if (!layout_.nodes[i].folder && layout_.nodes[i].appId == dragKey_) dragNode_ = i;
        DLOG(L"launchpad: moved '%ls' out of folder", NameOf(dragKey_).c_str());
        return;
    }
    // Hold at a screen edge to flip pages.
    if (mx_ < W_ * 0.03f || mx_ > W_ * 0.97f) {
        if (edgeSince_ == 0) edgeSince_ = now;
        else if (now - edgeSince_ > 0.55) {
            SetPage(pageTarget_ + (mx_ < W_ * 0.5f ? -1 : 1));
            edgeSince_ = now;
        }
    } else {
        edgeSince_ = 0;
    }
    bool center = false;
    int slot = SlotAt(mx_, my_, &center);
    if (slot < 0 || dragNode_ < 0) {
        folderCandidate_ = -1;
        folderHover_ = false;
        return;
    }
    if (slot >= static_cast<int>(layout_.nodes.size())) {
        slot = static_cast<int>(layout_.nodes.size()) - 1;
        center = false;
    }
    if (center && slot != dragNode_ && !layout_.nodes[dragNode_].folder) {
        if (folderCandidate_ != slot) {
            folderCandidate_ = slot;
            candidateSince_ = now;
        }
        folderHover_ = now - candidateSince_ >= 0.35;
        return;
    }
    folderCandidate_ = -1;
    folderHover_ = false;
    // Reorder only after the pointer rests on a new slot briefly, so passing over neighbours on the
    // way to a folder target does not shuffle them (as on macOS).
    if (slot == dragNode_) {
        reorderSlot_ = -1;
        return;
    }
    // The dwell counts only while the pointer is (nearly) still: any movement restarts it.
    const bool moved = std::hypot(mx_ - restX_, my_ - restY_) > 3.0f;
    if (reorderSlot_ != slot || moved) {
        reorderSlot_ = slot;
        reorderSince_ = now;
        restX_ = mx_;
        restY_ = my_;
        return;
    }
    if (now - reorderSince_ >= 0.15 && MoveNode(layout_, dragNode_, slot)) {
        dragNode_ = slot;
        reorderSlot_ = -1;
        RebuildItems();
    }
}

void Launchpad::EndDrag() {
    dragActive_ = false;
    // The dragged icon eases from where it was dropped into its slot.
    const float vx = mx_ - grabX_ + page_ * W_, vy = my_ - grabY_;
    if (dragFromFolder_) {
        dragFromFolder_ = false;
        SaveLayout(L"folder reorder");
    } else if (folderHover_ && folderCandidate_ >= 0 && dragNode_ >= 0) {
        const bool intoExisting = layout_.nodes[folderCandidate_].folder;
        const std::string id = "f" + std::to_string(static_cast<unsigned long long>(Now() * 1000.0)) + "_" +
                               std::to_string(layout_.nodes.size());
        const int f = DropOnto(layout_, dragNode_, folderCandidate_, L"Folder", id);
        if (f >= 0) {
            DLOG(L"launchpad: %ls '%ls'", intoExisting ? L"added to folder" : L"created folder", FolderName(f).c_str());
            SaveLayout(intoExisting ? L"add to folder" : L"create folder");
        }
    } else {
        SaveLayout(L"reorder");
    }
    pos_[dragKey_] = D2D1::Point2F(vx, vy);
    folderCandidate_ = -1;
    folderHover_ = false;
    dragNode_ = -1;
    RebuildItems();
    animating_ = true;
}

// ---------------------------------------------------------------------------------------------
bool Launchpad::Tick(double dt) {
    bool moving = false;
    openT_ = reducedMotion_ ? openTarget_ : Ease(openT_, openTarget_, dt, closing_ ? 0.055 : 0.075);
    moving |= openT_ != openTarget_;
    if (closing_ && openT_ <= 0.02f) {
        DestroyWindow(hwnd);
        return false;
    }
    if (!swipe_) {
        page_ = reducedMotion_ ? static_cast<float>(pageTarget_) : Ease(page_, static_cast<float>(pageTarget_), dt, 0.085);
        moving |= page_ != static_cast<float>(pageTarget_);
    }
    folderT_ = reducedMotion_ ? (folderOpen_ ? 1.0f : 0.0f) : Ease(folderT_, folderOpen_ ? 1.0f : 0.0f, dt, 0.06);
    moving |= folderT_ != (folderOpen_ ? 1.0f : 0.0f);
    if (!folderOpen_ && folderT_ == 0.0f && !dragFromFolder_) folder_ = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        auto& p = pos_[items_[i].key];
        const D2D1_RECT_F r = SlotRect(i);
        const float nx = reducedMotion_ ? r.left : Ease(p.x, r.left, dt, 0.07), ny = reducedMotion_ ? r.top : Ease(p.y, r.top, dt, 0.07);
        moving |= std::fabs(nx - r.left) > 0.25f || std::fabs(ny - r.top) > 0.25f;
        if (std::fabs(nx - r.left) <= 0.25f && std::fabs(ny - r.top) <= 0.25f) p = D2D1::Point2F(r.left, r.top);
        else p = D2D1::Point2F(nx, ny);
    }
    if (dragActive_) {
        UpdateDrag();
        moving = true;
    }
    return moving || swipe_;
}

bool Launchpad::DrawGlass(D2D1_RECT_F r, float radius, float appear, float dim) {
    // Shared Liquid Glass material + the user's Glass Studio parameters, over the blurred desktop.
    // `appear` materializes the glass; `dim` only lowers a formed surface (search behind a folder).
    if (glassOk_ && bgTex_) {
        GlassParameters gp = RoleGlass(GlassRole::Panel);
        gp.radius = std::min(radius, (r.bottom - r.top) * 0.5f);
        Materialize(gp, appear);
        gp.opacity *= std::clamp(dim, 0.0f, 1.0f);
        BackdropFrame bf;
        bf.texture = bgTex_.Get();
        bf.screenRect = mon_;
        bf.frameId = bgGeneration_;  // the live background changes: the glass re-blurs it
        GpuPaint g;
        g.context = ctx_.Get();
        g.target = rtv_.Get();
        g.targetWidth = pxW_;
        g.targetHeight = pxH_;
        g.windowScreenRect = mon_;
        g.dpiScale = scale_;
        g.plateDip = r;
        g.backdrop = &bf;
        g.params = &gp;
        g.pointerX = mx_ * scale_;
        g.pointerY = my_ * scale_;
        if (glass_->RenderGpu(g)) return true;
    }
    return false;
}

ComPtr<ID2D1Bitmap1> Launchpad::Resample(ID2D1Bitmap1* src, UINT px) {
    ComPtr<ID2D1Bitmap1> out;
    if (!src || !px) return out;
    const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, bp, &out))) return nullptr;
    ComPtr<ID2D1Image> old; dc_->GetTarget(&old);
    D2D1_MATRIX_3X2_F oldTransform; dc_->GetTransform(&oldTransform);
    float dx = 96, dy = 96; dc_->GetDpi(&dx, &dy);
    dc_->SetTarget(out.Get()); dc_->SetDpi(96, 96); dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    const bool own = !drawing_;  // icons are prepared when they load (outside a frame), or late inside one
    if (own) dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->DrawBitmap(src, D2D1::RectF(0, 0, static_cast<float>(px), static_cast<float>(px)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    if (own) dc_->EndDraw();
    dc_->SetTarget(old.Get()); dc_->SetDpi(dx, dy); dc_->SetTransform(oldTransform);
    return out;
}

const Launchpad::Sized* Launchpad::SizedIcon(const std::string& appId) {
    auto src = icons_.find(appId);
    if (src == icons_.end() || !src->second) return nullptr;
    // Display sizes in pixels: the grid icon and the folder preview (see DrawIcon). A size change (density setting)
    // rebuilds the cache.
    const UINT grid = static_cast<UINT>(std::lround(icon_ * scale_));
    const UINT mini = MiniPx();
    if (grid != gridPx_ || mini != miniPx_) { sized_.clear(); gridPx_ = grid; miniPx_ = mini; }
    auto it = sized_.find(appId);
    if (it != sized_.end()) return &it->second;
    Sized s{Resample(src->second.Get(), grid), Resample(src->second.Get(), mini)};
    return &sized_.emplace(appId, std::move(s)).first->second;
}

void Launchpad::DrawAppIcon(const std::string& appId, D2D1_RECT_F r, float opacity) {
    if (const Sized* s = SizedIcon(appId)) {
        // Grid size or folder preview size, whichever is nearer; drawn 1:1 at rest, linear while zooming or dragging.
        const float px = (r.right - r.left) * scale_;
        ID2D1Bitmap1* b = (s->mini && px < (gridPx_ + miniPx_) * 0.5f) ? s->mini.Get() : s->grid.Get();
        if (b) { dc_->DrawBitmap(b, r, opacity, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr); return; }
    }
    const float rad = (r.right - r.left) * 0.22f;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * opacity));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
}

void Launchpad::DrawLabel(const std::string& key, const std::wstring& text, D2D1_RECT_F icon, float opacity) {
    if (hideLabels_ || text.empty() || opacity <= 0.01f) return;  // Settings > Launchpad > Hide app names
    const float w = cellW_ - 8.0f;
    const std::string cacheKey = key + "|" + std::to_string(text.size()) + "|" + std::to_string(static_cast<int>(w));
    auto it = labels_.find(cacheKey);
    if (it == labels_.end()) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), labelFmt_.Get(), w, labelH_, &layout))) return;
        it = labels_.emplace(cacheKey, layout).first;
    }
    const float cx = (icon.left + icon.right) * 0.5f;
    const D2D1_POINT_2F o = D2D1::Point2F(cx - w * 0.5f, icon.bottom + 7.0f);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.45f * opacity));
    dc_->DrawTextLayout(D2D1::Point2F(o.x, o.y + 1.0f), it->second.Get(), brush_.Get());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * opacity));
    dc_->DrawTextLayout(o, it->second.Get(), brush_.Get());
}

// The macOS app-icon outline (measured from the official macOS 27 exports, icon_mask.h), which folders share on macOS; a
// circular corner (0.27 of the tile) was used before. Built once as a unit shape and scaled per tile.
ID2D1Geometry* Launchpad::FolderShape() {
    if (folderShape_ || !d2dFactory_) return folderShape_.Get();
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2dFactory_->CreatePathGeometry(&folderShape_)) || FAILED(folderShape_->Open(&sink))) { folderShape_.Reset(); return nullptr; }
    // One quadrant (x, y >= 0) from the right edge to the bottom edge, then mirrored: above the diagonal the half-width is
    // a function of y, below it the half-height a function of x (the outline is symmetric in both axes).
    std::vector<D2D1_POINT_2F> q;
    constexpr int kSteps = 96;
    for (int k = 0; k <= kSteps; ++k) { const float v = static_cast<float>(k) / kSteps, x = dock::NativeOutlineHalfWidth(v); if (x < v) break; q.push_back({x, v}); }
    for (int k = kSteps; k >= 0; --k) { const float u = static_cast<float>(k) / kSteps, y = dock::NativeOutlineHalfWidth(u); if (y < u) continue; q.push_back({u, y}); }
    std::vector<D2D1_POINT_2F> all;
    for (const auto& p : q) all.push_back({p.x, p.y});                                  // bottom right
    for (auto it = q.rbegin(); it != q.rend(); ++it) all.push_back({-it->x, it->y});  // bottom left
    for (const auto& p : q) all.push_back({-p.x, -p.y});                                // top left
    for (auto it = q.rbegin(); it != q.rend(); ++it) all.push_back({it->x, -it->y});  // top right
    sink->BeginFigure(all.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(all.data() + 1, static_cast<UINT32>(all.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) folderShape_.Reset();
    return folderShape_.Get();
}

void Launchpad::DrawIcon(const Item& it, D2D1_RECT_F r, float opacity, bool highlight) {
    if (highlight) {
        const float g = (r.right - r.left) * 0.10f;
        r = D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g);
    }
    if (!it.folder) {
        DrawAppIcon(it.appId, r, opacity);
        return;
    }
    // Folder: frosted rounded square with a 3x3 preview of its first applications.
    // macOS 27 folder (reference): a glass tile only slightly lighter than the blurred background, a faint light rim that is
    // brighter at the top, a soft shadow below, and nine small icons (19% of the tile) with generous padding.
    const float s = r.right - r.left, rad = s * 0.27f;
    ID2D1Geometry* shape = FolderShape();
    D2D1_MATRIX_3X2_F base; dc_->GetTransform(&base);
    // Draws the unit outline at `box` (inset by `inset` DIP); `stroke` > 0 outlines it with that DIP width.
    auto tile = [&](D2D1_RECT_F box, float inset, ID2D1Brush* b, float stroke) {
        const float hw = (box.right - box.left) * 0.5f - inset, hh = (box.bottom - box.top) * 0.5f - inset;
        if (!shape) {  // geometry unavailable: the former rounded square
            const auto rr = D2D1::RoundedRect(D2D1::RectF(box.left + inset, box.top + inset, box.right - inset, box.bottom - inset), rad - inset, rad - inset);
            if (stroke > 0) dc_->DrawRoundedRectangle(rr, b, stroke); else dc_->FillRoundedRectangle(rr, b);
            return;
        }
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(hw, hh) * D2D1::Matrix3x2F::Translation((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f) * base);
        if (stroke > 0) dc_->DrawGeometry(shape, b, stroke / hw); else dc_->FillGeometry(shape, b);
        dc_->SetTransform(base);
    };
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.16f * opacity));
    tile(D2D1::RectF(r.left, r.top + s * 0.02f, r.right, r.bottom + s * 0.03f), 0, brush_.Get(), 0);
    brush_->SetColor(highlight ? D2D1::ColorF(1, 1, 1, 0.30f * opacity) : D2D1::ColorF(0.62f, 0.66f, 0.72f, 0.16f * opacity));
    tile(r, 0, brush_.Get(), 0);
    {
        const D2D1_GRADIENT_STOP rim[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0.30f * opacity)}, {0.5f, D2D1::ColorF(1, 1, 1, 0.08f * opacity)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.16f * opacity)}};
        ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> rimBrush;
        const float w = 1.0f / scale_, h = w * 0.5f;
        if (SUCCEEDED(dc_->CreateGradientStopCollection(rim, 3, &stops)) &&
            SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, -1), D2D1::Point2F(0, 1)), stops.Get(), &rimBrush)))
            tile(r, h, rimBrush.Get(), w);  // gradient in unit space: top -1 .. bottom 1
    }
    const auto& apps = layout_.nodes[it.node].apps;
    const float pad = s * 0.155f, gap = s * 0.055f, mini = (s - 2 * pad - 2 * gap) / 3.0f;
    for (size_t i = 0; i < apps.size() && i < 9; ++i) {
        const float x = r.left + pad + (i % 3) * (mini + gap), y = r.top + pad + (i / 3) * (mini + gap);
        DrawAppIcon(apps[i], D2D1::RectF(x, y, x + mini, y + mini), opacity);
    }
}

void Launchpad::DrawGrid(float alpha) {
    if (!appsKnown_) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.8f * alpha));
        const wchar_t* t = L"Loading applications…";
        dc_->DrawTextW(t, static_cast<UINT32>(wcslen(t)), infoFmt_.Get(), D2D1::RectF(0, H_ * 0.45f, W_, H_ * 0.5f), brush_.Get());
        return;
    }
    if (items_.empty() && !query_.empty()) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.75f * alpha));
        const std::wstring t = L"No results for \"" + query_ + L"\"";
        dc_->DrawTextW(t.c_str(), static_cast<UINT32>(t.size()), infoFmt_.Get(), D2D1::RectF(0, H_ * 0.40f, W_, H_ * 0.45f), brush_.Get());
    }
    const int firstPage = static_cast<int>(std::floor(page_)), lastPage = static_cast<int>(std::ceil(page_));
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int pg = i / perPage_;
        if (dragActive_ && !dragFromFolder_ && items_[i].key == dragKey_) continue;
        if (pg < firstPage - 1 || pg > lastPage + 1) continue;
        const D2D1_POINT_2F p = pos_[items_[i].key];
        const D2D1_RECT_F r = D2D1::RectF(p.x - page_ * W_, p.y, p.x - page_ * W_ + icon_, p.y + icon_);
        if (r.right < -icon_ || r.left > W_ + icon_) continue;
        const bool highlight = dragActive_ && folderHover_ && items_[i].node == folderCandidate_;
        if (i == selected_ && !dragActive_) {
            // macOS: a rounded square around the icon itself (the name stays below it, outside the highlight).
            const float g = icon_ * 0.14f, side = icon_ + 2.0f * g;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.18f * alpha));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g), side * 0.24f, side * 0.24f), brush_.Get());
        }
        DrawIcon(items_[i], r, alpha, highlight);
        DrawLabel(items_[i].key, items_[i].folder ? FolderName(items_[i].node) : NameOf(items_[i].appId), r, alpha);
    }
    if (pageCount_ > 1) {
        const float y = H_ * 0.980f, gap = 14.0f;  // bottom edge for every preset (the Launchpad is above the Dock)
        const float x0 = W_ * 0.5f - (pageCount_ - 1) * gap * 0.5f;
        for (int i = 0; i < pageCount_; ++i) {
            const float sel = std::max(0.0f, 1.0f - std::fabs(page_ - i));
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (0.32f + 0.6f * sel) * alpha));
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + i * gap, y), 3.5f, 3.5f), brush_.Get());
        }
    }
}

void Launchpad::DrawFolderContents(float alpha) {
    if (folder_ < 0 || folder_ >= static_cast<int>(layout_.nodes.size())) return;
    const D2D1_RECT_F p = FolderPanel();
    const std::wstring title = renaming_ ? renameText_ + L"|" : FolderName(folder_);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.97f * alpha));
    dc_->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), titleFmt_.Get(),
                   D2D1::RectF(p.left, p.top + 18.0f, p.right, p.top + 60.0f), brush_.Get());
    if (renaming_) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.35f * alpha));
        dc_->DrawLine(D2D1::Point2F(p.left + 60, p.top + 60), D2D1::Point2F(p.right - 60, p.top + 60), brush_.Get(), 1.0f);
    }
    const auto& apps = layout_.nodes[folder_].apps;
    for (int i = 0; i < static_cast<int>(apps.size()) && i < 25; ++i) {
        if (dragActive_ && dragFromFolder_ && apps[i] == dragKey_) continue;
        const D2D1_RECT_F r = FolderSlotRect(i);
        DrawAppIcon(apps[i], r, alpha);
        DrawLabel(apps[i], NameOf(apps[i]), r, alpha);
    }
}

void Launchpad::Render() {
    double t = Now();
    auto lap = [&](int i) { const double n = Now(); phase_[i] = (n - t) * 1000.0; t = n; };
    if (liveDirty_) RebuildLiveBackground();
    lap(1);
    const float a = std::clamp(openT_, 0.0f, 1.0f);
    const float folderDim = 1.0f - 0.7f * folderT_;
    // Pass 1 (D2D): blurred desktop, grid, page dots.
    dc_->BeginDraw(); drawing_ = true;
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->DrawBitmap(bgBmp_.Get(), D2D1::RectF(0, 0, W_, H_), a, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    const float zoom = 1.0f + 0.08f * (1.0f - a);  // icons settle in from slightly larger, like macOS
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, D2D1::Point2F(W_ * 0.5f, H_ * 0.5f)));
    DrawGrid(a * folderDim);
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->EndDraw(); drawing_ = false;
    lap(2);

    // Pass 2 (D3D, shared Liquid Glass): search field and the open folder's panel.
    const D2D1_RECT_F sr = SearchRect();
    const bool searchGlass = DrawGlass(sr, (sr.bottom - sr.top) * 0.5f, a, folderDim);
    bool panelGlass = true;
    if (folder_ >= 0 && folderT_ > 0.01f) {
        D2D1_RECT_F p = FolderPanel();
        const float k = 0.94f + 0.06f * folderT_, cx = (p.left + p.right) * 0.5f, cy = (p.top + p.bottom) * 0.5f;
        p = D2D1::RectF(cx + (p.left - cx) * k, cy + (p.top - cy) * k, cx + (p.right - cx) * k, cy + (p.bottom - cy) * k);
        panelGlass = DrawGlass(p, 34.0f, a * folderT_);
    }

    lap(3);
    // Pass 3 (D2D): text and folder contents above the glass; dragged icon last.
    dc_->BeginDraw(); drawing_ = true;
    if (!searchGlass) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a * folderDim));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(sr, (sr.bottom - sr.top) * 0.5f, (sr.bottom - sr.top) * 0.5f), brush_.Get());
    }
    {
        const float alpha = a * folderDim;
        const float cy = (sr.top + sr.bottom) * 0.5f;
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.80f * alpha));
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(sr.left + 17.0f, cy - 1.0f), 4.5f, 4.5f), brush_.Get(), 1.4f);
        dc_->DrawLine(D2D1::Point2F(sr.left + 20.3f, cy + 2.3f), D2D1::Point2F(sr.left + 23.5f, cy + 5.5f), brush_.Get(), 1.5f);
        const std::wstring text = query_.empty() ? std::wstring(L"Search Applications") : query_ + (renaming_ ? L"" : L"|");
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (query_.empty() ? 0.62f : 0.96f) * alpha));
        dc_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), searchFmt_.Get(),
                       D2D1::RectF(sr.left + 31.0f, sr.top, sr.right - 14.0f, sr.bottom), brush_.Get());
    }
    if (folder_ >= 0 && folderT_ > 0.01f) {
        if (!panelGlass) {
            brush_->SetColor(D2D1::ColorF(0.2f, 0.2f, 0.22f, 0.55f * a * folderT_));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(FolderPanel(), 34, 34), brush_.Get());
        }
        DrawFolderContents(a * folderT_);
    }
    if (dragActive_) {
        const D2D1_RECT_F r = D2D1::RectF(mx_ - grabX_, my_ - grabY_, mx_ - grabX_ + icon_, my_ - grabY_ + icon_);
        const float g = icon_ * 0.06f;
        Item it;
        it.key = dragKey_;
        it.appId = dragKey_;
        if (!dragFromFolder_ && dragNode_ >= 0 && dragNode_ < static_cast<int>(layout_.nodes.size()) && layout_.nodes[dragNode_].folder) {
            it.folder = true;
            it.node = dragNode_;
        }
        DrawIcon(it, D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g), 0.92f, false);
    }
    dc_->EndDraw(); drawing_ = false;
    lap(4);
    // Paced by the frame-latency waitable in the loop: Present(1) waited for a vblank as well, so every frame took two
    // (120 fps on a 240 Hz display, measured 2026-11-12).
    swap_->Present(0, 0);
    lap(5);
}

void Launchpad::Frame() {
    const double now = Now();
    const double raw = now - lastT_;
    const double dt = std::clamp(raw, 0.0, 0.05);
    lastT_ = now;
    const bool wasAnimating = animating_;
    animating_ = Tick(dt);
    phase_[0] = (Now() - now) * 1000.0;
    if (!hwnd) return;
    Render();
    {   // a frame that took longer than one display frame: which phase
        double total = uploadMs_; for (double v : phase_) total += v;
        if (total > 1000.0 * docky::vblank::FramePeriod().load()) ++slowFrames_;
        if (frames_ > 3 && wasAnimating && raw < 0.2) {  // gaps inside an animation only (not the first frame after idle)
            SlowFrame f{raw * 1000.0, total, {phase_[0], phase_[1], phase_[2], phase_[3], phase_[4], phase_[5]}, uploadMs_, page_, waitMs_, dispatchMs_, dispatched_, peekMs_};
            worst_.push_back(f);
            std::sort(worst_.begin(), worst_.end(), [](const SlowFrame& a, const SlowFrame& b) { return a.total > b.total; });
            if (worst_.size() > 12) worst_.pop_back();
        }
        uploadMs_ = 0; waitMs_ = 0; dispatchMs_ = 0; dispatched_ = 0; peekMs_ = 0;
    }
    needsFrame_ = false;
    ++frames_;
    // Consecutive animated frames: real-time presentation rate (paced by the DXGI waitable/vblank).
    if (wasAnimating && raw < 0.1) {
        ++animFrames_;
        animSeconds_ += raw;
        maxInterval_ = std::max(maxInterval_, raw);
    }
}

// ---------------------------------------------------------------------------------------------
LRESULT CALLBACK Launchpad::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* self = static_cast<Launchpad*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = h;
    }
    auto* self = reinterpret_cast<Launchpad*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    const double t0 = Now();  // every message, sent ones included (they run inside PeekMessage, not DispatchMessage)
    const LRESULT r = self->Handle(m, w, l);
    const double ms = (Now() - t0) * 1000.0;
    if (ms > 2.0) { auto& e = self->slowProc_[m]; e.first = (std::max)(e.first, ms); ++e.second; }
    return r;
}

LRESULT Launchpad::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE: {
        mx_ = Dip(GET_X_LPARAM(l));
        my_ = Dip(GET_Y_LPARAM(l));
        if (lbDown_ && !dragActive_ && !swipe_) {
            const float th = 6.0f;
            if (std::hypot(mx_ - pressX_, my_ - pressY_) > th) {
                if (folderOpen_ && pressFolderSlot_ >= 0) {
                    dragFromFolder_ = true;
                    StartDrag();
                } else if (!folderOpen_ && pressItem_ >= 0 && query_.empty()) {
                    dragFromFolder_ = false;
                    StartDrag();
                } else if (!folderOpen_ && pressItem_ < 0) {
                    swipe_ = true;
                    swipeStartPage_ = page_;
                }
            }
        }
        if (swipe_) page_ = std::clamp(swipeStartPage_ - (mx_ - pressX_) / W_, -0.25f, pageCount_ - 0.75f);
        if (dragActive_ || swipe_) {
            animating_ = true;
            needsFrame_ = true;
        }
        return 0;
    }
    case WM_RBUTTONUP:
        if (!lbDown_ && !dragActive_ && !renaming_) ShowAppMenu(Dip(GET_X_LPARAM(l)), Dip(GET_Y_LPARAM(l)));
        return 0;
    case WM_LBUTTONDOWN: {
        // A click outside the open app menu only closes the menu (the popup sees it first and closes itself).
        if (menuOpen_ || Now() - menuClosedAt_ < 0.3) { menuOpen_ = false; return 0; }
        SetCapture(hwnd);
        lbDown_ = true;
        pressX_ = mx_ = Dip(GET_X_LPARAM(l));
        pressY_ = my_ = Dip(GET_Y_LPARAM(l));
        pressItem_ = -1;
        pressFolderSlot_ = -1;
        pressOnTitle_ = pressInPanel_ = pressOnSearch_ = false;
        if (folderOpen_) {
            const D2D1_RECT_F p = FolderPanel();
            pressInPanel_ = pressX_ >= p.left && pressX_ <= p.right && pressY_ >= p.top && pressY_ <= p.bottom;
            pressOnTitle_ = pressInPanel_ && pressY_ < p.top + 66.0f;
            pressFolderSlot_ = pressInPanel_ ? FolderSlotAt(pressX_, pressY_) : -1;
        } else {
            const D2D1_RECT_F s = SearchRect();
            pressOnSearch_ = pressX_ >= s.left && pressX_ <= s.right && pressY_ >= s.top && pressY_ <= s.bottom;
            pressItem_ = pressOnSearch_ ? -1 : ItemAt(pressX_, pressY_);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        // Clear the pressed state before releasing capture: ReleaseCapture sends WM_CAPTURECHANGED,
        // which must not treat this ordinary release as a lost capture.
        const bool wasDown = lbDown_;
        lbDown_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!wasDown) return 0;
        if (dragActive_) {
            EndDrag();
        } else if (swipe_) {
            swipe_ = false;
            const float moved = page_ - swipeStartPage_;
            const int base = static_cast<int>(std::lround(swipeStartPage_));
            SetPage(moved > 0.12f ? base + 1 : moved < -0.12f ? base - 1 : base);
        } else if (folderOpen_) {
            if (pressOnTitle_) {
                renaming_ = true;
                renameText_ = layout_.nodes[folder_].folderName.empty() ? L"Folder" : layout_.nodes[folder_].folderName;
            } else if (pressFolderSlot_ >= 0) {
                Launch(layout_.nodes[folder_].apps[pressFolderSlot_]);
            } else if (!pressInPanel_) {
                CloseFolder();
            } else if (renaming_) {
                CommitRename();
            }
        } else if (pressItem_ >= 0) {
            Activate(pressItem_);
        } else if (!pressOnSearch_) {
            int dot = -1;
            if (pageCount_ > 1) {
                const float y = H_ * 0.980f, gap = 14.0f, x0 = W_ * 0.5f - (pageCount_ - 1) * gap * 0.5f;
                for (int i = 0; i < pageCount_; ++i)
                    if (std::fabs(pressX_ - (x0 + i * gap)) <= 7.0f && std::fabs(pressY_ - y) <= 12.0f) dot = i;
            }
            if (dot >= 0) SetPage(dot);
            else Close(L"background click");
        }
        animating_ = needsFrame_ = true;
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (lbDown_ && reinterpret_cast<HWND>(l) != hwnd) {
            lbDown_ = false;
            if (dragActive_) EndDrag();
            swipe_ = false;
            SetPage(static_cast<int>(std::lround(page_)));
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (folderOpen_ || dragActive_) return 0;
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) * (m == WM_MOUSEWHEEL ? -1.0f : 1.0f);
        wheelAcc_ += delta;
        const double now = Now();
        if (std::fabs(wheelAcc_) >= WHEEL_DELTA && now - lastWheelFlip_ > 0.25) {
            SetPage(pageTarget_ + (wheelAcc_ > 0 ? 1 : -1));
            wheelAcc_ = 0;
            lastWheelFlip_ = now;
            animating_ = true;
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE:
            if (renaming_) {
                renaming_ = false;
                needsFrame_ = true;
            } else if (folderOpen_) {
                CloseFolder();
            } else if (!query_.empty()) {
                query_.clear();
                UpdateSearch();
            } else {
                Close(L"Escape");
            }
            break;
        case VK_PRIOR:
        case VK_NEXT:
            if (!renaming_ && !folderOpen_) {
                SetPage(pageTarget_ + (w == VK_NEXT ? 1 : -1));
                selected_ = -1;
                animating_ = true;
            }
            break;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN: {
            if (renaming_ || folderOpen_ || items_.empty()) break;
            if (GetKeyState(VK_CONTROL) < 0 && (w == VK_LEFT || w == VK_RIGHT)) {  // Ctrl+arrows: pages
                SetPage(pageTarget_ + (w == VK_RIGHT ? 1 : -1));
                selected_ = -1;
                animating_ = true;
                break;
            }
            const int n = static_cast<int>(items_.size());
            if (selected_ < 0 || selected_ / perPage_ != pageTarget_) {
                selected_ = std::min(pageTarget_ * perPage_, n - 1);
            } else {
                int next = selected_;
                if (w == VK_LEFT) next -= 1;
                else if (w == VK_RIGHT) next += 1;
                else if (w == VK_UP) next = (selected_ % perPage_) >= cols_ ? selected_ - cols_ : selected_;
                else next = (selected_ % perPage_) + cols_ < perPage_ && selected_ + cols_ < n ? selected_ + cols_ : selected_;
                selected_ = std::clamp(next, 0, n - 1);
            }
            if (selected_ / perPage_ != pageTarget_) SetPage(selected_ / perPage_);
            needsFrame_ = animating_ = true;
            break;
        }
        case VK_RETURN:
            if (renaming_) CommitRename();
            else if (!folderOpen_ && selected_ >= 0 && selected_ < static_cast<int>(items_.size())) Activate(selected_);
            break;
        default: break;
        }
        return 0;
    case WM_CHAR: {
        const wchar_t c = static_cast<wchar_t>(w);
        std::wstring& text = renaming_ ? renameText_ : query_;
        if (c == 8) {
            if (!text.empty()) text.pop_back();
        } else if (c >= 0x20 && c != 0x7F && text.size() < 64) {
            if (!renaming_ && folderOpen_) CloseFolder();  // typing searches the whole Launchpad
            text.push_back(c);
        } else {
            return 0;
        }
        if (!renaming_) UpdateSearch();
        needsFrame_ = true;
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !dragActive_ && !menuOpen_ && Now() - openedAt_ > 0.35) Close(L"deactivated");
        return 0;
    case WM_IME_STARTCOMPOSITION:
        // Keep IME composition and candidates (e.g. Pinyin) attached to the field being typed into.
        if (HIMC imc = ImmGetContext(hwnd)) {
            const D2D1_RECT_F r = renaming_ && folder_ >= 0 ? FolderPanel() : SearchRect();
            COMPOSITIONFORM cf{};
            cf.dwStyle = CFS_POINT;
            cf.ptCurrentPos = {static_cast<LONG>((r.left + 40.0f) * scale_), static_cast<LONG>((r.top + (renaming_ ? 64.0f : 40.0f)) * scale_)};
            ImmSetCompositionWindow(imc, &cf);
            CANDIDATEFORM cand{};
            cand.dwIndex = 0;
            cand.dwStyle = CFS_CANDIDATEPOS;
            cand.ptCurrentPos = cf.ptCurrentPos;
            ImmSetCandidateWindow(imc, &cand);
            ImmReleaseContext(hwnd, imc);
        }
        break;
    case kMsgCaptureAvailable:  // a new frame of the live background
        if (live_.ProcessCapture(mon_)) { liveDirty_ = true; needsFrame_ = true; }
        return 0;
    case WM_TIMER:
        if (w == kDensityTimer && live_.CheckHealth()) needsFrame_ = true;  // consent answered / restart after a failure
        if(w==kDensityTimer && !dragActive_ && !lbDown_ && !swipe_ && !folderOpen_ && !renaming_){
            const auto revision=liquid::settings::ReadRevision(settingsPath_);if(revision!=settingsRevision_)RefreshDensity();
        }
        return 0;
    case WM_SETTINGCHANGE: {
        BOOL animations=TRUE;
        if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0) && reducedMotion_ != !animations){reducedMotion_=!animations;animating_=needsFrame_=true;}
        break;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case kMsgClose:
        Close(L"toggle");
        return 0;
    case kMsgIcons:  // uploaded between frames by the loop (one ~2 ms batch per frame), never several batches in a row
        iconsPending_ = true; needsFrame_ = true;
        return 0;
    case kMsgApps:
        ApplyApps();
        return 0;
    case kMsgOpenApp: {
        std::unique_ptr<std::string> id(reinterpret_cast<std::string*>(l));
        menuOpen_ = false;
        if (id) Launch(*id);
        return 0;
    }
    case kMsgMenuClosed:
        if (menuOpen_) menuClosedAt_ = Now();
        menuOpen_ = false;
        if (GetForegroundWindow() != hwnd && !closing_) SetForegroundWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        needsFrame_ = true;
        return 0;
    }
    case WM_DESTROY:
        live_.Request(false, nullptr);
        Shared::Get().SetNotify(nullptr);
        PostQuitMessage(0);
        hwnd = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

int Launchpad::Run(HWND dock, const std::wstring& layoutPath, const std::wstring& settingsPath) {
    dock_ = dock;
    BOOL animations=TRUE;if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0))reducedMotion_=!animations;
    layoutPath_ = layoutPath;settingsPath_=settingsPath.empty()?liquid::settings::DefaultPath():settingsPath;
    {const auto loaded = liquid::settings::Load(settingsPath_).value; maskIcons_ = loaded.dock.useMacIconMask; hideLabels_ = loaded.launchpad.hideLabels; showWindows_ = loaded.launchpad.showWindows;}
    const double t0 = Now();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(dock ? dock : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    mon_ = mi.rcMonitor;
    pxW_ = static_cast<UINT>(mon_.right - mon_.left);
    pxH_ = static_cast<UINT>(mon_.bottom - mon_.top);
    const bool captured = CaptureDesktop();
    const double tCapture = Now();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"DockOver Launchpad", WS_POPUP,
                    mon_.left, mon_.top, static_cast<int>(pxW_), static_cast<int>(pxH_), nullptr, nullptr,
                    GetModuleHandleW(nullptr), this);
    if (!hwnd) {
        DLOG(L"launchpad: window creation failed %lu", GetLastError());
        return 1;
    }
    scale_ = GetDpiForWindow(hwnd) / 96.0f;
    W_ = pxW_ / scale_;
    H_ = pxH_ / scale_;
    RefreshDensity();ComputeGeometry();SetTimer(hwnd,kDensityTimer,1000,nullptr);
    Shared::Get().iconPx = static_cast<unsigned>(std::clamp(std::ceil(icon_ * scale_), 64.0f, 256.0f));
    Shared::Get().maskIcons = maskIcons_;
    Shared::Get().gridPx = static_cast<unsigned>(std::lround(icon_ * scale_));
    Shared::Get().miniPx = MiniPx();
    Shared::Get().ResampleMissing();
    const HRESULT hr = InitGraphics();
    if (FAILED(hr)) {
        DLOG(L"launchpad: graphics init failed 0x%08X", hr);
        DestroyWindow(hwnd);
        return 1;
    }
    Shared::Get().SetNotify(hwnd);
    StartLiveBackground();
    ApplyApps();
    Shared::Get().RefreshAppsAsync();  // picks up newly installed applications while open
    PostMessageW(dock_, kMsgLaunchpadCovers, 1, 0);  // the Dock hides its minimized-window previews (always-on-top overlay)
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    // Launchpad covers the Dock (its page dots sit where the Dock is): keep this window above it.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    openedAt_ = lastT_ = Now();
    DLOG(L"launchpad: open %.0fx%.0f DIP @%.2f, %zu apps %ls, %zu icons cached, desktop snapshot %ls %.0f ms, ready %.0f ms, glass %ls",
         W_, H_, scale_, apps_.size(), appsKnown_ ? L"" : L"(discovering)", Shared::Get().LoadedCount(),
         captured ? L"ok" : L"FAILED", (tCapture - t0) * 1000.0, (Now() - t0) * 1000.0, glassOk_ ? L"Liquid" : L"fallback");

    MSG msg{};
    for (;;) {
        if (!(animating_ || needsFrame_ || dragActive_ || swipe_ || iconsPending_)) {
            // Idle: wait for input or a live-background compare finishing on the GPU.
            if (HANDLE compare = live_.CompletionEvent()) {
                const DWORD r = MsgWaitForMultipleObjectsEx(1, &compare, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (r == WAIT_OBJECT_0) { if (live_.CompleteCompare(mon_)) { liveDirty_ = true; needsFrame_ = true; } continue; }
                if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) continue;
                if (msg.message == WM_QUIT) break;
            } else if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        // Animating: one frame per display frame (or per the frame-rate cap), from the display ticker (vblank.h). The
        // swap chain's waitable plus Present(1) took two display frames per frame here: 120 fps on a 240 Hz display.
        ticker_.Start(hwnd, docky::vblank::kMsgVBlank);
        bool quit = false, due = false;
        const double p0 = Now();
        HANDLE compare = live_.CompletionEvent();
        const double w0 = Now();
        const DWORD r = MsgWaitForMultipleObjectsEx(compare ? 1 : 0, compare ? &compare : nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        waitMs_ += (Now() - w0) * 1000.0;
        if (compare && r == WAIT_OBJECT_0 && live_.CompleteCompare(mon_)) liveDirty_ = true;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            if (msg.message == docky::vblank::kMsgVBlank) { ticker_.Ack(); due = true; continue; }
            TranslateMessage(&msg);
            const double m0 = Now();
            DispatchMessageW(&msg);
            const double mms = (Now() - m0) * 1000.0;
            dispatchMs_ += mms; ++dispatched_;
            if (mms > 2.0) { auto& e = slowMsgs_[msg.message]; e.first = (std::max)(e.first, mms); ++e.second; }
        }
        peekMs_ += (Now() - p0) * 1000.0 - (Now() - p0 > 0 ? 0 : 0);
        if (quit || !hwnd) break;
        if (due || r == WAIT_TIMEOUT) { Frame(); if (iconsPending_) UploadIcons(); }
        if (!(animating_ || needsFrame_ || dragActive_ || swipe_ || iconsPending_)) ticker_.Stop();
    }
    ticker_.Stop();
    if (glass_) glass_->ReleaseGpuResources();
    if (waitable_) CloseHandle(waitable_);
    PostMessageW(dock_, kMsgLaunchpadCovers, 0, 0);
    DLOG(L"launchpad: live background rebuilt %u times; %u frames over one display frame", liveRebuilds_, slowFrames_);
    DLOG(L"launchpad: icons prepared by the worker %u, resampled on the GPU %u, masked on the UI thread %u", workerSized_, gpuSized_, uiMasked_);
    for (const auto& [m, e] : slowMsgs_) DLOG_DEBUG(L"launchpad: message 0x%04X took up to %.2f ms (%u times over 2 ms)", m, e.first, e.second);
    for (const auto& [m, e] : slowProc_) DLOG_DEBUG(L"launchpad: window procedure 0x%04X took up to %.2f ms (%u times over 2 ms)", m, e.first, e.second);
    for (const auto& f : worst_)
        DLOG_DEBUG(L"launchpad: long frame %.2f ms (tick %.2f live %.2f grid %.2f glass %.2f text %.2f present %.2f icons %.2f; page %.2f)",
             f.total, f.ph[0], f.ph[1], f.ph[2], f.ph[3], f.ph[4], f.ph[5], f.icons, f.page);
    DLOG(L"launchpad: closed; %llu frames, animated %llu frames in %.2f s = %.1f fps, max interval %.2f ms", frames_,
         animFrames_, animSeconds_, animSeconds_ > 0 ? animFrames_ / animSeconds_ : 0.0, maxInterval_ * 1000.0);
    return 0;
}

// ---------------------------------------------------------------------------------------------
std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false};

void ThreadMain(HWND dock, std::wstring layoutPath, std::wstring settingsPath) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        Launchpad lp;
        lp.Run(dock, layoutPath, settingsPath);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    g_running.store(false);
}

}  // namespace

void Prewarm() {
    // Icons loaded in the background before the first opening are masked on the worker thread too.
    Shared::Get().maskIcons = liquid::settings::Load(liquid::settings::DefaultPath()).value.dock.useMacIconMask;
    Shared::Get().RefreshAppsAsync();
}

void Toggle(HWND dock, const std::wstring& layoutPath, const std::wstring& settingsPath) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HWND h = FindWindowW(kClass, L"DockOver Launchpad")) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) {
            PostMessageW(h, kMsgClose, 0, 0);
            return;
        }
    }
    if (g_running.load()) return;  // still opening or finishing a close animation
    if (g_thread.joinable()) g_thread.join();
    g_running.store(true);
    g_thread = std::thread(ThreadMain, dock, layoutPath, settingsPath);
}

bool IsOpen() { return g_running.load(); }

void OpenApp(const std::string& appId) {
    HWND h = OwnWindow();
    if (!h) return;
    auto id = std::make_unique<std::string>(appId);
    if (PostMessageW(h, kMsgOpenApp, 0, reinterpret_cast<LPARAM>(id.get()))) id.release();
}
void Dismiss() { if (HWND h = OwnWindow()) PostMessageW(h, kMsgClose, 0, 0); }
void MenuClosed() { if (HWND h = OwnWindow()) PostMessageW(h, kMsgMenuClosed, 0, 0); }
void SetCustomIconPath(const std::wstring& path) { Shared::Get().SetCustomIconPath(path); }
void InvalidateIcon(const std::string& stableId) { Shared::Get().InvalidateIcon(stableId); }

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (HWND h = FindWindowW(kClass, L"DockOver Launchpad")) PostMessageW(h, WM_CLOSE, 0, 0);
        if (g_thread.joinable()) g_thread.join();
    }
    Shared::Get().Stop();
}

std::vector<std::uint32_t> MakeDockIcon(unsigned size) {
    // Rounded dark-glass tile with a 3x3 grid of coloured rounded squares (an "apps" glyph).
    if (size < 16 || size > 1024) return {};
    std::vector<std::uint32_t> px(static_cast<size_t>(size) * size, 0);
    const float s = static_cast<float>(size), half = s * 0.5f, outer = s * 0.44f, rad = s * 0.20f;
    const float colors[9][3] = {{1.00f, 0.42f, 0.38f}, {1.00f, 0.72f, 0.28f}, {0.98f, 0.88f, 0.30f},
                                {0.38f, 0.85f, 0.45f}, {0.30f, 0.80f, 0.95f}, {0.36f, 0.56f, 1.00f},
                                {0.66f, 0.46f, 1.00f}, {1.00f, 0.45f, 0.78f}, {0.85f, 0.87f, 0.92f}};
    auto rbox = [](float px_, float py_, float cx, float cy, float h, float r) {
        const float qx = std::fabs(px_ - cx) - (h - r), qy = std::fabs(py_ - cy) - (h - r);
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
    };
    const float cell = outer * 2.0f * 0.78f / 3.0f, dot = cell * 0.36f;
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float a = std::clamp(0.5f - rbox(fx, fy, half, half, outer, rad), 0.0f, 1.0f);
            if (a <= 0) continue;
            const float g = 0.20f + 0.10f * (1.0f - fy / s);
            float r = g, gg = g * 1.03f, b = g * 1.12f;
            for (int i = 0; i < 9; ++i) {
                const float cx = half + (i % 3 - 1) * cell, cy = half + (i / 3 - 1) * cell;
                const float c = std::clamp(0.5f - rbox(fx, fy, cx, cy, dot, dot * 0.42f), 0.0f, 1.0f);
                r = r * (1 - c) + colors[i][0] * c;
                gg = gg * (1 - c) + colors[i][1] * c;
                b = b * (1 - c) + colors[i][2] * c;
            }
            auto ch = [a](float v) { return static_cast<std::uint32_t>(std::clamp(v * a, 0.0f, 1.0f) * 255.0f + 0.5f); };
            px[static_cast<size_t>(y) * size + x] =
                (static_cast<std::uint32_t>(a * 255.0f + 0.5f) << 24) | (ch(r) << 16) | (ch(gg) << 8) | ch(b);
        }
    return px;
}

}  // namespace dock::launchpad
