// DockOver Black Hole Visualizer (docky-blackhole.exe): a standalone test entry for the DockDrop black hole visuals.
//
//  * Overlay: one click-through, top-most, capture-excluded window per hole covering the hole's monitor. Its content is
//    a small DirectComposition swap chain (only the lens region) positioned with the visual's transform, so moving the
//    hole changes a shader constant and a visual offset in the same frame (no window moves, no resize while dragging).
//  * Desktop: the low-latency desktop-duplication path of DockOver (SharedDesktop owner mode): duplication lives on the
//    render device and is pumped on the render thread right before drawing. The overlay is WDA_EXCLUDEFROMCAPTURE, so
//    it never samples itself (no capture recursion). Wallpaper Engine and any other animated wallpaper is part of the
//    composed desktop and arrives at its own frame rate.
//  * Render thread: paced by the compositor clock, independent of the UI thread (panel drags and modal loops never stall
//    it). The panel (Win32 controls) only edits parameters.
//  * No DWM injection, no hooks, no drivers: public DXGI / DirectComposition / Win32 only.
//
// Command line (all optional):
//   --quality high|balanced|low   --test-pattern   --no-panel   --size <DIP per M>   --incl <deg>   --lens <M>
//   --disk / --no-disk   --glow <0..1>   --tidal <0..3>   --no-shred   --drift <0..1>
//   --demo infall|emit|loop|multi-infall|multi-emit|multi
//   --x <px> --y <px>  (physical screen px of the hole centre)   --pump-wait <ms>   --log <file>
//   --snapshot <file.bmp> [--snapshot-after <s>]   --exit-after <s>   --bench <s per mode> [--bench-out <file>]
//   --quit  (closes a running instance)
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <commoncontrols.h>
#include <ole2.h>
#include <memory>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../desktop/duplication.h"
#include "../log.h"
#include "../vblank.h"
#include "blackhole_anim.h"
#include "blackhole_payload.h"
#include "blackhole_renderer.h"
#include "schwarzschild_lut.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;
using namespace docky::blackhole;

namespace {

constexpr wchar_t kOverlayClass[] = L"DockOverBlackHoleOverlay";
constexpr wchar_t kPanelClass[] = L"DockOverBlackHolePanel";
constexpr UINT kMsgStats = WM_APP + 1;
constexpr UINT_PTR kHitTimer = 1, kStatsTimer = 2;
constexpr int kHotkeyPanel = 1;

enum class Quality { High = 0, Balanced = 1, Low = 2 };
const wchar_t* QualityName(Quality q) { return q == Quality::High ? L"High quality" : q == Quality::Balanced ? L"Balanced" : L"Low power"; }

struct Params {
    float sizeDip = 15.f;     // DIP per gravitational radius M (shadow radius = 5.2 M)
    float inclDeg = 80.f;     // 0 face-on, 90 edge-on (GARGANTUA ~ 80-85)
    // Source-plane distance (M): Einstein radius 2 sqrt(D) M. ~7 puts the Einstein ring right at the shadow edge, so
    // the desktop is pulled in and stretched progressively towards the hole with no mirrored band ("doughnut") around it.
    float lens = 7.f;
    float diskIn = 6.f;       // ISCO
    float diskOut = 16.f;
    float diskOpacity = 0.92f;
    float diskBrightness = 1.f;
    float spin = 1.f;
    float doppler = 0.55f;
    float heat = 0.f;
    float regionMul = 5.f;    // lens field radius in shadow radii
    float taper = 0.55f;
    float tidal = 3.f;        // payload tidal stretch; the icon keeps stretching while it shreds (0: keeps its shape)
    float smoothing = 0.5f;   // hole motion inertia (0 = rigid)
    bool showDisk = false;    // the horizontal accretion disk is optional (off: only the circular lens)
    bool shred = true;        // falling icons are torn into pixels (tidal disruption); emitted ones assemble from pixels
    float drift = 0.6f;       // slow sky parallax of an orbiting observer (0 = still), in units of 3 M
    bool retrograde = false;
    Quality quality = Quality::High;
};

struct AnimCommand { HolePhase phase; float dx = 0, dy = 0; };
struct PayloadCommand {
    enum Kind { Icon, Hover, Release, Cancel, Emit, Demo } kind = Icon;
    int slot = 0;
    float dx = 0, dy = 0, half = 0;  // px, relative to the hole (target) centre; Demo: dx = 1 infall, 2 emit
    double delay = 0;                // Release: stagger (s)
    std::shared_ptr<std::vector<uint32_t>> pixels;
};

struct Stats {
    double fps = 0, cpuMs = 0, gpuMs = 0, latMs = 0, latP95 = 0, latEstMs = 0, desktopFps = 0, refreshHz = 0;
    int spp = 0, targetW = 0, targetH = 0;
    bool owner = false, desktop = false, batterySaver = false;
    Quality effective = Quality::Balanced;
    HolePhase phase = HolePhase::Hidden;
};

struct Shared {
    std::mutex m;
    Params p;
    double holeX = 0, holeY = 0;  // physical screen px
    RECT monitorRect{};
    float dpiScale = 1.f;
    bool testPattern = false;
    bool reducedMotion = false, highContrast = false;
    std::deque<AnimCommand> commands;
    std::deque<PayloadCommand> payloadCommands;
    std::vector<std::shared_ptr<std::vector<uint32_t>>> demoIcons;  // folder, image, archive
    int demoMode = 0;  // RunDemo modes: 0 none, 1 infall, 2 emit, 3 loop, 4/5/6 the same with three files
    std::wstring snapshotPath;
    Stats stats;
    bool visible = true;
    std::atomic<bool> quit{false};
    std::atomic<unsigned> paramVersion{0};
};

Shared g;
HWND g_overlay = nullptr, g_panel = nullptr;
bool g_dragging = false, g_tilting = false;
POINT g_dragStart{};
double g_grabDx = 0, g_grabDy = 0;
float g_tiltStart = 0;
bool g_interactive = false;

// Options
int g_pumpWaitMs = 1;
double g_exitAfter = 0, g_snapshotAfter = 1.5, g_benchSeconds = 0;
std::wstring g_benchOut;
std::wstring g_seqDir; double g_seqInterval = 0.1; int g_seqCount = 0;

double Now() { return docky::vblank::Seconds(); }

bool ReducedMotionSetting() { BOOL on = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0); return !on; }
bool HighContrastSetting() { HIGHCONTRASTW hc{sizeof(hc)}; return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON); }
bool BatterySaverOn() { SYSTEM_POWER_STATUS s{}; return GetSystemPowerStatus(&s) && s.SystemStatusFlag == 1; }

float MonitorScale(HMONITOR m) { UINT x = 96, y = 96; if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y))) x = 96; return x / 96.f; }

// UI thread: the hole moved; put the overlay on the hole's monitor.
void PlaceOverlay() {
    POINT c{static_cast<LONG>(std::lround(g.holeX)), static_cast<LONG>(std::lround(g.holeY))};
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    const float scale = MonitorScale(mon);
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(g.m);
        // Keep the centre on some monitor.
        g.holeX = std::clamp(g.holeX, static_cast<double>(mi.rcMonitor.left), static_cast<double>(mi.rcMonitor.right - 1));
        g.holeY = std::clamp(g.holeY, static_cast<double>(mi.rcMonitor.top), static_cast<double>(mi.rcMonitor.bottom - 1));
        moved = !EqualRect(&g.monitorRect, &mi.rcMonitor);
        g.monitorRect = mi.rcMonitor; g.dpiScale = scale;
    }
    if (moved && g_overlay) {
        SetWindowPos(g_overlay, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        DLOG(L"blackhole: overlay on monitor (%ld,%ld)-(%ld,%ld), scale %.2f", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, scale);
    }
    g.paramVersion++;
}

void PostAnim(HolePhase phase, float dx = 0, float dy = 0) {
    std::lock_guard<std::mutex> lock(g.m);
    g.commands.push_back({phase, dx, dy});
}
void PostPayload(PayloadCommand c) { std::lock_guard<std::mutex> lock(g.m); g.payloadCommands.push_back(std::move(c)); }

// ---- File icons (jumbo system image list, 256 px, premultiplied BGRA) ----
std::shared_ptr<std::vector<uint32_t>> IconPixels(int sysIndex) {
    constexpr int S = static_cast<int>(LensRenderer::kPayloadSize);
    auto out = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(S) * S, 0u);
    ComPtr<IImageList> list;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&list)))) return nullptr;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(sysIndex, ILD_TRANSPARENT, &icon)) || !icon) return nullptr;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        memset(bits, 0, static_cast<size_t>(S) * S * 4);
        ok = DrawIconEx(dc, 0, 0, icon, S, S, 0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        SelectObject(dc, old);
        const uint32_t* src = static_cast<const uint32_t*>(bits);
        bool anyAlpha = false;
        for (int i = 0; i < S * S; ++i) if (src[i] >> 24) { anyAlpha = true; break; }
        for (int i = 0; i < S * S; ++i) {
            uint32_t px = src[i];
            if (!anyAlpha) px = (px & 0xFFFFFFu) ? (px | 0xFF000000u) : 0;  // icon without an alpha channel
            (*out)[i] = px;  // DrawIconEx onto a zeroed 32-bit DIB leaves premultiplied BGRA
        }
        DeleteObject(dib);
    }
    DeleteDC(dc); DestroyIcon(icon);
    return ok ? out : nullptr;
}
std::shared_ptr<std::vector<uint32_t>> StockIcon(SHSTOCKICONID id) {
    SHSTOCKICONINFO info{sizeof(info)};
    if (FAILED(SHGetStockIconInfo(id, SHGSI_SYSICONINDEX, &info))) return nullptr;
    return IconPixels(info.iSysImageIndex);
}
std::shared_ptr<std::vector<uint32_t>> FileIcon(const wchar_t* path) {
    SHFILEINFOW fi{};
    if (!SHGetFileInfoW(path, 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX)) return StockIcon(SIID_DOCNOASSOC);
    auto px = IconPixels(fi.iIcon);
    return px ? px : StockIcon(SIID_DOCNOASSOC);
}

// ---- GPU utilisation of this process (Windows "GPU Engine" counters, the same source Task Manager uses) ----
class GpuCounter {
public:
    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &q_) != ERROR_SUCCESS) return false;
        wchar_t path[128]; swprintf_s(path, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
        DWORD len = 0;
        PdhExpandWildCardPathW(nullptr, path, nullptr, &len, 0);
        std::vector<wchar_t> buf(len + 2);
        if (PdhExpandWildCardPathW(nullptr, path, buf.data(), &len, 0) != ERROR_SUCCESS) return false;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) { PDH_HCOUNTER c{}; if (PdhAddEnglishCounterW(q_, p, 0, &c) == ERROR_SUCCESS) counters_.push_back(c); }
        PdhCollectQueryData(q_);
        return !counters_.empty();
    }
    double Sample() {  // percent, summed over this process's engines since the previous call
        if (!q_ || PdhCollectQueryData(q_) != ERROR_SUCCESS) return -1;
        double sum = 0;
        for (auto c : counters_) { PDH_FMT_COUNTERVALUE v{}; if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) sum += v.doubleValue; }
        return sum;
    }
    ~GpuCounter() { if (q_) PdhCloseQuery(q_); }
private:
    PDH_HQUERY q_ = nullptr;
    std::vector<PDH_HCOUNTER> counters_;
};

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}
double Mean(const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }

// =====================================================================================================================
// Render thread
// =====================================================================================================================
class RenderThread {
public:
    void Run() {
        if (!Init()) { DLOG_ERROR(L"blackhole: renderer initialisation failed"); g.quit = true; PostMessageW(g_panel ? g_panel : g_overlay, WM_CLOSE, 0, 0); return; }
        animator_.onFinished = [](HolePhase) {};
        animator_.Spawn(Now());
        if (g_benchSeconds > 0) RunBench(); else Loop(0);
        Shutdown();
    }

private:
    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &ctx_))) return false;
        ComPtr<IDXGIDevice> dxgi; device_.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory_));
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(g_overlay, TRUE, &target_)) || FAILED(dcomp_->CreateVisual(&visual_))) return false;
        visual_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        target_->SetRoot(visual_.Get());
        if (!lens_.Init(device_.Get())) return false;
        {
            std::lock_guard<std::mutex> lock(g.m);
            testPattern_ = g.testPattern;
        }
        if (testPattern_) {
            const RECT r = g.monitorRect;
            lens_.UseTestPattern(static_cast<UINT>(r.right - r.left), static_cast<UINT>(r.bottom - r.top));
        } else {
            auto& sd = docky::desk::SharedDesktop::Get();
            sd.AddUser(); user_ = true;
            sd.SetOwner(device_.Get());  // duplication on this device, pumped on this thread (one frame less latency)
        }
        return true;
    }

    void Shutdown() {
        if (!testPattern_) {
            auto& sd = docky::desk::SharedDesktop::Get();
            sd.SetOwner(nullptr);
            if (user_) sd.RemoveUser();
        }
        view_.Reset();
        if (visual_) { visual_->SetContent(nullptr); dcomp_->Commit(); }
    }

    struct Frame {  // per-frame samples for statistics
        double interval = 0, cpu = 0, latency = -1, latencyEst = -1; bool newDesktop = false;
    };

    // One frame. Returns false when nothing was drawn (hidden).
    bool Tick(Frame& f, Quality forced, bool forceMode) {
        Params p; double hx, hy; RECT mon; float scale; bool reduced, contrast; std::wstring snapshot;
        std::deque<AnimCommand> cmds;
        std::deque<PayloadCommand> pcmds;
        int demoMode = 0; std::vector<std::shared_ptr<std::vector<uint32_t>>> demoIcons;
        {
            std::lock_guard<std::mutex> lock(g.m);
            p = g.p; hx = g.holeX; hy = g.holeY; mon = g.monitorRect; scale = g.dpiScale; reduced = g.reducedMotion; contrast = g.highContrast;
            cmds.swap(g.commands);
            pcmds.swap(g.payloadCommands);
            demoMode = g.demoMode; demoIcons = g.demoIcons;
            if (!g.snapshotPath.empty() && Now() - start_ >= g_snapshotAfter) { snapshot.swap(g.snapshotPath); }
        }
        const double now = Now();
        if (!g_seqDir.empty() && seqIndex_ < g_seqCount && now - start_ >= g_snapshotAfter + seqIndex_ * g_seqInterval) {
            wchar_t name[64]; swprintf_s(name, L"\\frame_%03d.bmp", seqIndex_++);
            snapshot = g_seqDir + name;
        }
        animator_.SetReducedMotion(reduced);
        reduced_ = reduced;
        for (const auto& c : cmds) {
            switch (c.phase) {
            case HolePhase::Spawn: animator_.Spawn(now); break;
            case HolePhase::Idle: animator_.Idle(now); break;
            case HolePhase::Attract: animator_.Attract(now, c.dx, c.dy); break;
            case HolePhase::Consume: animator_.Consume(now); break;
            case HolePhase::Emit: animator_.Emit(now); break;
            case HolePhase::Collapse: animator_.Collapse(now); break;
            default: break;
            }
        }
        // ---- payloads (file icons): commands from the UI / drop target, the demo script, physics ----
        const double kNow = p.sizeDip * scale * std::max(animator_.Last().mass, 0.2f);
        payloads_.tuning.tidal = p.tidal;
        payloads_.tuning.shred = p.shred;
        for (auto& c : pcmds) {
            switch (c.kind) {
            case PayloadCommand::Icon: if (c.pixels) { lens_.SetPayloadIcon(c.slot, c.pixels->data()); debris_.SetIcon(c.slot, c.pixels->data()); } break;
            case PayloadCommand::Hover:
                if (payloads_.StateOf(c.slot) != PayloadSystem::State::Hover && animator_.Phase() != HolePhase::Attract) animator_.Attract(now, c.dx, c.dy);
                animator_.SetAttractTarget(c.dx, c.dy);
                payloads_.Hover(c.slot, c.dx, c.dy, c.half, now);
                break;
            case PayloadCommand::Release: pendingRelease_.push_back({c.slot, now + c.delay}); break;
            case PayloadCommand::Cancel:
                payloads_.Cancel(c.slot, now);
                if (animator_.Phase() == HolePhase::Attract) animator_.Idle(now);
                break;
            case PayloadCommand::Emit: StartEmit(c.slot, c.dx, c.dy, c.half, now, kNow, p.shred); animator_.Emit(now); break;
            case PayloadCommand::Demo:
                demoOnce_ = static_cast<int>(c.dx); demoChain_ = demoOnce_ == 7; if (demoChain_) demoOnce_ = 4;
                demoStart_ = now; demoIconSet_ = false; demoEmitted_ = false;
                break;
            }
        }
        RunDemo(now, demoMode, demoIcons, p, scale);
        for (auto it = pendingEmit_.begin(); it != pendingEmit_.end();) {
            if (now >= it->at) { StartEmit(it->slot, it->dx, it->dy, it->half, now, it->k, p.shred); it = pendingEmit_.erase(it); } else ++it;
        }
        for (auto it = pendingRelease_.begin(); it != pendingRelease_.end();) {
            if (now >= it->second) {
                PayloadGpu card;  // shredding: the icon's pixels fall in freely, spaghettified (blackhole_debris.h)
                double vx = 0, vy = 0;
                if (p.shred && !reduced && payloads_.Handover(it->first, card, vx, vy))
                    debris_.Drop(it->first, card, vx / std::max(kNow, 1e-3), vy / std::max(kNow, 1e-3), now, p.tidal);
                else payloads_.Release(it->first, now, kNow);
                it = pendingRelease_.erase(it);
            } else ++it;
        }
        const HoleVisual v = animator_.Update(now);
        const bool visible = animator_.Phase() != HolePhase::Hidden && v.opacity > 0.f && v.mass > 0.01f;
        {
            std::lock_guard<std::mutex> lock(g.m);
            g.visible = visible;
        }
        // Desktop usage follows visibility: a hidden hole releases the duplication (SharedDesktop stops it after 2 s).
        if (!testPattern_) {
            auto& sd = docky::desk::SharedDesktop::Get();
            if (visible && !user_) { sd.AddUser(); user_ = true; }
            if (!visible && user_) { sd.RemoveUser(); user_ = false; }
        }
        if (!visible) {
            if (drawnVisible_) { ClearOnce(); drawnVisible_ = false; }
            return false;
        }

        // Spin integrates time (so changing the speed never jumps the flow map).
        const double dt = lastTick_ > 0 ? std::min(0.1, now - lastTick_) : 0.0;
        lastTick_ = now;
        spinTime_ += dt * p.spin * v.spin;

        Quality q = forceMode ? forced : p.quality;
        const bool saver = BatterySaverOn();
        if (!forceMode && saver) q = Quality::Low;
        const float renderScale = q == Quality::Low ? 0.6f : 1.f;
        const uint32_t spp = q == Quality::High ? 4u : q == Quality::Balanced ? 2u : 1u;

        // Geometry (desktop px = monitor-local physical px).
        const double kBase = p.sizeDip * scale;
        const double k = kBase * v.mass;
        const float diskOut = std::max(p.diskOut, p.diskIn + 1.5f);
        const double regionM = std::max<double>(p.regionMul * kCriticalB, p.showDisk ? diskOut * 1.12 : 0.0);
        const double R = regionM * k;
        const double Ralloc = regionM * kBase * 1.3;  // headroom for Attract / Consume pulses: no resize mid-animation
        const UINT side = static_cast<UINT>(std::clamp(std::ceil(2 * Ralloc * renderScale) + 2, 16.0, 8192.0));
        if (renderScale != allocScale_) { forceResize_ = true; allocScale_ = renderScale; }  // mode switch: exact size, no hysteresis
        if (!EnsureSwapChain(side)) return false;

        // Hole motion with a little inertia (critically damped spring towards the dragged position).
        if (!EqualRect(&mon, &springMon_) || springT_ <= 0 || reduced || p.smoothing <= 0.01f) { sx_ = hx; sy_ = hy; svx_ = svy_ = 0; springMon_ = mon; }
        else {
            const double w = 60.0 - 45.0 * std::clamp<double>(p.smoothing, 0, 1);
            for (double left = dt; left > 1e-6; left -= 0.004) {
                const double h = std::min(left, 0.004);
                svx_ += (w * w * (hx - sx_) - 2 * w * svx_) * h; svy_ += (w * w * (hy - sy_) - 2 * w * svy_) * h;
                sx_ += svx_ * h; sy_ += svy_ * h;
            }
        }
        springT_ = now;
        const double holeDx = sx_ - mon.left + v.leanX, holeDy = sy_ - mon.top + v.leanY;

        PayloadGpu items[PayloadSystem::kSlots];
        int swallowed = 0, emitted = 0;
        int itemCount = payloads_.Update(now, dt, k, items, swallowed, emitted, reduced);
        if (debris_.Update(now, dt, regionM, debrisOut_) > 0) ++swallowed;
        if (swallowed > 0) animator_.Consume(now);
        lens_.PrepareDebris(side);
        lens_.SetDebris(debrisOut_, static_cast<float>(regionM), side);
        // Integer region origin so that the weak-field rim samples desktop texels exactly (pixel-identical edge).
        const double ox = std::floor(holeDx - side / renderScale / 2), oy = std::floor(holeDy - side / renderScale / 2);

        // Desktop: pump the duplication right before drawing (low-latency owner mode), then use the newest image.
        const double t0 = Now();
        int64_t usedPresentQpc = 0;
        if (!testPattern_) {
            auto& sd = docky::desk::SharedDesktop::Get();
            sd.Want(mon);
            if (sd.Owned()) sd.PumpOwned(static_cast<UINT>(g_pumpWaitMs));
            if (sd.Valid()) {
                if (!view_ || viewGen_ != sd.Generation()) {
                    view_.Reset();
                    if (HANDLE h = sd.SharedHandle()) device_->OpenSharedResource(h, IID_PPV_ARGS(&view_));
                    viewGen_ = sd.Generation();
                }
                const uint64_t serial = sd.Serial();
                // The lens samples the region itself plus, for strongly bent rays, up to D * pi gravitational radii
                // further out: desktop changes entirely outside that area (a video elsewhere) cost no copy at all.
                const double reach = R + (p.lens * v.lens) * k * 3.1416 + 8;
                const RECT area{static_cast<LONG>(hx - reach), static_cast<LONG>(hy - reach), static_cast<LONG>(hx + reach), static_cast<LONG>(hy + reach)};
                const bool regenerated = viewGen_ != copiedGen_;
                if (view_ && serial != lastSerial_ && (regenerated || lastSerial_ == 0 || sd.Changed(area, lastSerial_))) {
                    lens_.UpdateDesktop(view_.Get(), serial + (static_cast<uint64_t>(viewGen_) << 40));
                    copiedGen_ = viewGen_; f.newDesktop = true; ++desktopFrames_;
                    usedPresentQpc = sd.LastPresentQpc();
                }
                lastSerial_ = serial;
            }
            owner_ = sd.Owned();
        }

        LensConstants c{};
        c.regionOrigin[0] = static_cast<float>(ox); c.regionOrigin[1] = static_cast<float>(oy);
        c.holeCenter[0] = static_cast<float>(holeDx); c.holeCenter[1] = static_cast<float>(holeDy);
        c.renderScale = renderScale;
        c.pxPerM = static_cast<float>(std::max(k, 0.05));
        const float incl = std::clamp(p.inclDeg, 0.f, 89.9f) * 3.14159265f / 180.f;
        c.sinIncl = std::sin(incl); c.cosIncl = std::cos(incl);
        c.lensDistance = p.lens * v.lens;
        c.regionRadius = static_cast<float>(std::min(R, Ralloc));
        c.diskIn = std::max(p.diskIn, 3.05f); c.diskOut = diskOut;
        c.diskOpacity = p.diskOpacity;
        c.diskBrightness = p.diskBrightness * v.disk;
        c.time = static_cast<float>(std::fmod(spinTime_, 1.0e4));
        c.doppler = contrast ? 0.f : p.doppler;
        c.heatTint = contrast ? 0.f : p.heat;
        c.ringFlash = v.ringFlash;
        c.opacity = v.opacity;
        c.taperStart = p.taper;
        c.showDisk = p.showDisk ? 1.f : 0.f;
        c.spp = spp;
        c.flowPeriod = 4.f;
        c.spinSign = p.retrograde ? -1.f : 1.f;
        c.overlayRadius = c.regionRadius;
        if (!reduced && p.drift > 0) {  // an observer slowly orbiting the hole (one turn in 24 s)
            const double a = 2 * 3.14159265358979 * (now - start_) / 24.0, amp = 3.0 * p.drift * k;
            c.skyDrift[0] = static_cast<float>(amp * std::cos(a)); c.skyDrift[1] = static_cast<float>(amp * std::sin(a));
        }
        c.payloadLodBias = 0.f;
        c.payloadCount = static_cast<uint32_t>(itemCount);
        for (int i = 0; i < itemCount; ++i) {
            c.payloadA[i][0] = items[i].x; c.payloadA[i][1] = items[i].y; c.payloadA[i][2] = items[i].half; c.payloadA[i][3] = items[i].stretch;
            c.payloadB[i][0] = items[i].opacity; c.payloadB[i][1] = items[i].g; c.payloadB[i][2] = items[i].slice; c.payloadB[i][3] = items[i].shear;
        }

        ComPtr<ID3D11Texture2D> back;
        swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> rtv;
        device_->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        lens_.Render(rtv.Get(), side, side, c);
        if (!snapshot.empty()) {
            const bool ok = lens_.SaveBmp(back.Get(), snapshot);
            DLOG(L"blackhole: snapshot %ls -> %ls (origin %.0f,%.0f hole %.3f,%.3f radius %.2f scale %.2f side %u)", snapshot.c_str(), ok ? L"saved" : L"failed",
                 ox, oy, holeDx, holeDy, c.regionRadius, renderScale, side);
        }

        D2D_MATRIX_3X2_F m{1.f / renderScale, 0, 0, 1.f / renderScale, static_cast<float>(ox), static_cast<float>(oy)};
        visual_->SetTransform(m);
        const HRESULT pr = swap_->Present(0, 0);
        LARGE_INTEGER pq{}; QueryPerformanceCounter(&pq);
        dcomp_->Commit();
        drawnVisible_ = SUCCEEDED(pr);
        f.cpu = (Now() - t0) * 1000.0;
        if (usedPresentQpc > 0) {
            LARGE_INTEGER fq{}; QueryPerformanceFrequency(&fq);
            f.latency = (pq.QuadPart - usedPresentQpc) * 1000.0 / fq.QuadPart;
            DCOMPOSITION_FRAME_STATISTICS fs{};
            if (SUCCEEDED(dcomp_->GetFrameStatistics(&fs)) && fs.nextEstimatedFrameTime.QuadPart > pq.QuadPart && fs.timeFrequency.QuadPart)
                f.latencyEst = f.latency + (fs.nextEstimatedFrameTime.QuadPart - pq.QuadPart) * 1000.0 / fs.timeFrequency.QuadPart;
            if (f.latency > 250) f.latency = f.latencyEst = -1;  // stale timestamp (desktop idle for a while)
        }
        lastSpp_ = spp; lastQuality_ = q; batterySaver_ = saver;
        return true;
    }

    // Scripted payloads (--demo, panel buttons): a folder icon approaches from the field's edge and is released
    // (infall), or comes out of the hole (emit). Mode 3 alternates every 4.5 s.
    // Scripted payloads (--demo, panel buttons). Modes: 1 infall, 2 emit, 3 loop (1/2 every 7.5 s); 4 three files
    // dropped together, 5 three files emitted, 6 loop (4/5 every 9 s); 7 (panel) = 4 then 5 once. The multi-file cases
    // go through the same path as a real drop of several files: a fanned stack on slots 0..2, all let go at once (as
    // DropTarget::Drop does: a staggered release left the later cards standing still under the released cursor), and
    // emits staggered by 0.28 s to three destinations.
    void RunDemo(double now, int mode, const std::vector<std::shared_ptr<std::vector<uint32_t>>>& icons, const Params& p, float scale) {
        int active = demoOnce_;
        double t = now - demoStart_;
        if (!active && mode) {
            if (demoStart_ <= 0) demoStart_ = start_ + 1.2;
            t = now - demoStart_;
            if (t < 0) return;
            const double period = mode >= 4 ? 9.0 : 7.5;  // the whole pass (teardrop, stream, ring, disc, wind) takes ~7 s
            const int cycle = static_cast<int>(t / period);
            active = mode == 3 ? (cycle % 2 ? 2 : 1) : mode == 6 ? (cycle % 2 ? 5 : 4) : mode;
            t -= cycle * period;
            if (cycle != demoCycle_) { demoCycle_ = cycle; demoIconSet_ = false; demoEmitted_ = false; }
        }
        if (!active || icons.empty() || t < 0) return;
        const bool multi = active >= 4;
        const int count = multi ? 3 : 1;
        auto slotOf = [&](int i) { return multi ? i : 3; };
        if (!demoIconSet_) {
            for (int i = 0; i < count; ++i) {
                const auto& icon = icons[std::min<size_t>(multi ? static_cast<size_t>(i) : 0, icons.size() - 1)];
                if (icon) { lens_.SetPayloadIcon(slotOf(i), icon->data()); debris_.SetIcon(slotOf(i), icon->data()); }
            }
            demoIconSet_ = true;
        }
        const double k = p.sizeDip * scale;
        const double field = p.regionMul * kCriticalB * k;
        const float half = 30.f * scale;
        const double ang = -0.55;  // upper right
        if (active == 1 || active == 4) {  // approach for 1.2 s (0.92 -> 0.5 of the field, curving slightly), then drop
            if (t <= 1.2) {
                // Still moving inward when dropped (like a real drag): no ease-out, so the fall carries straight on.
                const double f = t / 1.2, e = 0.75 * f + 0.25 * f * f;
                const double r = field * (0.92 - 0.42 * e), a = ang + 0.35 * e;
                const float dx = static_cast<float>(r * std::cos(a)), dy = static_cast<float>(r * std::sin(a));
                if (payloads_.StateOf(slotOf(0)) != PayloadSystem::State::Hover && animator_.Phase() != HolePhase::Attract) animator_.Attract(now, dx, dy);
                animator_.SetAttractTarget(dx, dy);
                for (int i = 0; i < count; ++i)  // the drop target's fanned stack
                    payloads_.Hover(slotOf(i), dx + static_cast<float>(9.0 * i * scale), dy - static_cast<float>(7.0 * i * scale), half, now);
            } else if (payloads_.StateOf(slotOf(0)) == PayloadSystem::State::Hover) {
                for (int i = 0; i < count; ++i) pendingRelease_.push_back({slotOf(i), now});
                if (demoChain_) { demoChain_ = false; demoOnce_ = 5; demoStart_ = now + 6.5; demoEmitted_ = false; }
                else demoOnce_ = 0;
            }
        } else if ((active == 2 || active == 5) && !demoEmitted_) {
            for (int i = 0; i < count; ++i) {
                const double a = ang + 2.6 + (multi ? (i - 1) * 0.5 : 0.0), r = field * (multi ? 0.6 + 0.04 * i : 0.62);
                pendingEmit_.push_back({slotOf(i), now + 0.28 * i, static_cast<float>(r * std::cos(a)), static_cast<float>(r * std::sin(a)), half, k});
            }
            animator_.Emit(now);
            demoEmitted_ = true;
            demoOnce_ = 0;
        }
    }

    // Emit: with shredding the icon assembles from pixels climbing out of the photon ring; the card appears on arrival.
    void StartEmit(int slot, float dx, float dy, float half, double now, double k, bool shred) {
        const bool assemble = shred && !reduced_;
        payloads_.Emit(slot, dx, dy, half, now, k, assemble);
        if (assemble && k > 0) debris_.Assemble(slot, dx / k, dy / k, half / k, now, payloads_.EmitDuration(slot));
        else debris_.Clear(slot);
    }

    bool EnsureSwapChain(UINT side) {
        const bool force = forceResize_; forceResize_ = false;
        if (swap_ && side == side_) return true;
        if (swap_ && !force && side < side_ && side > side_ * 0.6) return true;  // shrink only when much smaller (hysteresis)
        if (swap_) {
            ctx_->ClearState(); ctx_->Flush();
            if (SUCCEEDED(swap_->ResizeBuffers(2, side, side, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))) { side_ = side; return true; }
            swap_.Reset(); if (waitable_) { CloseHandle(waitable_); waitable_ = nullptr; }
        }
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = side; d.Height = side; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (FAILED(factory_->CreateSwapChainForComposition(device_.Get(), &d, nullptr, &swap_))) return false;
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(swap_.As(&sc2))) { sc2->SetMaximumFrameLatency(1); waitable_ = sc2->GetFrameLatencyWaitableObject(); }
        visual_->SetContent(swap_.Get());
        side_ = side;
        DLOG(L"blackhole: lens swap chain %ux%u", side, side);
        return true;
    }

    void ClearOnce() {
        if (!swap_) return;
        ComPtr<ID3D11Texture2D> back; swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> rtv; device_->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        const float clear[4] = {};
        ctx_->ClearRenderTargetView(rtv.Get(), clear);
        swap_->Present(0, 0); dcomp_->Commit();
    }

    // Waits for the next display frame: the swap chain's latency waitable (never more than one frame queued), then the
    // compositor clock. Quality changes the render cost, so every mode can follow the screen.
    void Pace() {
        if (waitable_ && lastPresent_ > 0) WaitForSingleObjectEx(waitable_, 100, FALSE);
        docky::vblank::WaitFrame(g_overlay);
    }

    void Publish(const std::vector<Frame>& recent) {
        Stats s;
        std::vector<double> iv, cpu, lat, est;
        for (const auto& f : recent) { iv.push_back(f.interval); cpu.push_back(f.cpu); if (f.latency >= 0) lat.push_back(f.latency); if (f.latencyEst >= 0) est.push_back(f.latencyEst); }
        const double meanIv = Mean(iv);
        s.fps = meanIv > 0 ? 1000.0 / meanIv : 0; s.cpuMs = Mean(cpu); s.gpuMs = gpuAvg_;
        s.latMs = Mean(lat); s.latP95 = Percentile(lat, 0.95); s.latEstMs = Mean(est);
        s.spp = static_cast<int>(lastSpp_); s.targetW = s.targetH = static_cast<int>(side_);
        s.owner = owner_; s.desktop = lens_.HasDesktop(); s.effective = lastQuality_; s.batterySaver = batterySaver_;
        s.phase = animator_.Phase(); s.refreshHz = 1.0 / docky::vblank::FramePeriod().load();
        s.desktopFps = desktopRate_;
        std::lock_guard<std::mutex> lock(g.m);
        g.stats = s;
    }

    void Loop(double seconds, Quality forced = Quality::Balanced, bool forceMode = false, std::vector<Frame>* out = nullptr) {
        start_ = start_ > 0 ? start_ : Now();
        const double begin = Now();
        std::vector<Frame> recent;
        double statsAt = Now(); uint64_t deskAt = desktopFrames_;
        while (!g.quit) {
            if (seconds > 0 && Now() - begin >= seconds) break;
            if (g_exitAfter > 0 && Now() - start_ >= g_exitAfter) { g.quit = true; PostMessageW(g_panel ? g_panel : g_overlay, WM_CLOSE, 0, 0); break; }
            Pace();
            Frame f;
            if (!Tick(f, forced, forceMode)) { Sleep(15); lastPresent_ = 0; continue; }
            const double now = Now();
            f.interval = lastPresent_ > 0 ? (now - lastPresent_) * 1000.0 : 0;
            lastPresent_ = now;
            const double gpu = lens_.PollGpuMs();
            if (gpu >= 0) { gpuAvg_ = gpuAvg_ > 0 ? gpuAvg_ * 0.9 + gpu * 0.1 : gpu; if (out) gpuSamples_.push_back(gpu); }
            if (f.interval > 0) { recent.push_back(f); if (out) out->push_back(f); }
            if (recent.size() > 240) recent.erase(recent.begin(), recent.begin() + (recent.size() - 240));
            if (now - statsAt >= 0.5) {
                desktopRate_ = (desktopFrames_ - deskAt) / (now - statsAt); deskAt = desktopFrames_; statsAt = now;
                Publish(recent);
                if (g_panel) PostMessageW(g_panel, kMsgStats, 0, 0);
            }
        }
    }

    void RunBench() {
        GpuCounter counter; const bool pdh = counter.Open();
        std::wstring report = L"DockOver Black Hole Visualizer benchmark\r\n";
        wchar_t line[4096];
        {
            std::lock_guard<std::mutex> lock(g.m);
            swprintf_s(line, L"source: %ls, hole size %.1f DIP/M, incl %.1f deg, lens D %.1f M, DPI scale %.2f, refresh ~%.1f Hz\r\n",
                       g.testPattern ? L"test pattern" : L"live desktop (DXGI duplication)", g.p.sizeDip, g.p.inclDeg, g.p.lens, g.dpiScale,
                       1.0 / docky::vblank::FramePeriod().load());
            report += line;
        }
        swprintf_s(line, L"Schwarzschild LUT build: %.1f ms\r\n", lens_.LutBuildMs()); report += line;
        Loop(1.5, Quality::Balanced, true);  // warm-up (spawn animation, duplication start)
        for (Quality q : {Quality::High, Quality::Balanced, Quality::Low}) {
            std::vector<Frame> frames; gpuSamples_.clear();
            const uint64_t desk0 = desktopFrames_;
            counter.Sample();
            const double t0 = Now();
            Loop(g_benchSeconds, q, true, &frames);
            const double elapsed = Now() - t0;
            const double util = pdh ? counter.Sample() : -1;
            if (g.quit) break;
            std::vector<double> iv, cpu, lat, est;
            for (const auto& f : frames) { iv.push_back(f.interval); cpu.push_back(f.cpu); if (f.latency >= 0) lat.push_back(f.latency); if (f.latencyEst >= 0) est.push_back(f.latencyEst); }
            swprintf_s(line,
                L"\r\n[%ls] spp=%u target=%ux%u frames=%zu in %.1fs -> %.1f fps\r\n"
                L"  frame interval ms: mean %.3f  p95 %.3f  p99 %.3f  max %.3f\r\n"
                L"  CPU per frame ms (pump+draw+present): mean %.3f  p95 %.3f\r\n"
                L"  GPU lens pass ms (timestamps): mean %.3f  p95 %.3f  (n=%zu)\r\n"
                L"  GPU utilisation of this process (PDH GPU Engine): %ls\r\n"
                L"  desktop frames consumed: %llu (%.1f/s)\r\n"
                L"  desktop composed -> our Present call ms: mean %.2f  p95 %.2f  (n=%zu)\r\n"
                L"  desktop composed -> estimated next composition ms: mean %.2f  p95 %.2f  (n=%zu)\r\n",
                QualityName(q), lastSpp_, side_, side_, frames.size(), elapsed, frames.size() / std::max(elapsed, 1e-3),
                Mean(iv), Percentile(iv, 0.95), Percentile(iv, 0.99), iv.empty() ? 0 : *std::max_element(iv.begin(), iv.end()),
                Mean(cpu), Percentile(cpu, 0.95), Mean(gpuSamples_), Percentile(gpuSamples_, 0.95), gpuSamples_.size(),
                util >= 0 ? (std::to_wstring(util).substr(0, 5) + L" %").c_str() : L"unavailable",
                static_cast<unsigned long long>(desktopFrames_ - desk0), (desktopFrames_ - desk0) / std::max(elapsed, 1e-3),
                Mean(lat), Percentile(lat, 0.95), lat.size(), Mean(est), Percentile(est, 0.95), est.size());
            report += line;
        }
        report += L"\r\nNotes: latency figures start at the duplication frame's LastPresentTime (when DWM composed the desktop) and end at\r\n"
                  L"our Present call / DirectComposition's estimate of the next composition; they are not photon-to-photon measurements.\r\n";
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_benchOut.c_str(), L"wb") == 0 && f) {
            fputs("\xEF\xBB\xBF", f);
            const int n = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string u(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, u.data(), n, nullptr, nullptr);
            fputs(u.c_str(), f); fclose(f);
        }
        DLOG(L"blackhole: benchmark written to %ls", g_benchOut.c_str());
        g.quit = true;
        PostMessageW(g_panel ? g_panel : g_overlay, WM_CLOSE, 0, 0);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDXGISwapChain1> swap_;
    HANDLE waitable_ = nullptr;
    UINT side_ = 0;
    LensRenderer lens_;
    HoleAnimator animator_;
    PayloadSystem payloads_;
    DebrisSystem debris_;
    std::vector<DebrisGpu> debrisOut_;
    bool reduced_ = false;
    std::vector<std::pair<int, double>> pendingRelease_;
    double demoStart_ = 0; int demoOnce_ = 0, demoCycle_ = -1; bool demoIconSet_ = false, demoEmitted_ = false, demoChain_ = false;
    struct PendingEmit { int slot; double at; float dx, dy, half; double k; };
    std::vector<PendingEmit> pendingEmit_;
    double sx_ = 0, sy_ = 0, svx_ = 0, svy_ = 0, springT_ = 0; RECT springMon_{};
    int seqIndex_ = 0;
    ComPtr<ID3D11Texture2D> view_;
    unsigned viewGen_ = 0, copiedGen_ = ~0u;
    float allocScale_ = 1.f;
    bool forceResize_ = false;
    uint64_t lastSerial_ = 0, desktopFrames_ = 0;
    double desktopRate_ = 0;
    bool testPattern_ = false, user_ = false, owner_ = false, drawnVisible_ = false, batterySaver_ = false;
    double start_ = 0, lastTick_ = 0, spinTime_ = 0, lastPresent_ = 0, gpuAvg_ = 0;
    uint32_t lastSpp_ = 0;
    Quality lastQuality_ = Quality::Balanced;
    std::vector<double> gpuSamples_;
};

// =====================================================================================================================
// Overlay input (UI thread)
// =====================================================================================================================
double ShadowRadiusPx() {
    std::lock_guard<std::mutex> lock(g.m);
    return kCriticalB * g.p.sizeDip * g.dpiScale;
}

double FieldRadiusPx() {
    std::lock_guard<std::mutex> lock(g.m);
    const double m = std::max<double>(g.p.regionMul * kCriticalB, g.p.showDisk ? std::max(g.p.diskOut, g.p.diskIn + 1.5f) * 1.12 : 0.0);
    return m * g.p.sizeDip * g.dpiScale;
}

bool g_dropActive = false;

void UpdateHitTest() {
    if (!g_overlay) return;
    POINT c{}; GetCursorPos(&c);
    double hx, hy; bool visible;
    { std::lock_guard<std::mutex> lock(g.m); hx = g.holeX; hy = g.holeY; visible = g.visible; }
    const double r = std::max(ShadowRadiusPx() * 1.25, 24.0);
    const double d = std::hypot(c.x - hx, c.y - hy);
    // While the left button is held elsewhere (someone drags a file towards the hole), the whole lens field accepts
    // OLE drag-and-drop so the icon can be attracted. The press itself already went to the other window, so this
    // never steals a click; mouse capture keeps ordinary drags (window moves, text selection) with their owner.
    const bool held = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    const bool want = g_dragging || g_tilting || g_dropActive || (visible && (d <= r || (held && d <= FieldRadiusPx() * 0.97)));
    if (want == g_interactive) return;
    g_interactive = want;
    LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
    ex = want ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex);
}

// ---- OLE drop target: a dragged file is attracted, stretched, and swallowed when dropped (visual only) ----
class DropTarget final : public IDropTarget {
public:
    DropTarget() { CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_)); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        g_dropActive = true;
        count_ = 0;
        std::vector<std::wstring> files;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM sm{};
        if (data && SUCCEEDED(data->GetData(&fe, &sm))) {
            if (HDROP drop = static_cast<HDROP>(sm.hGlobal)) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n && i < 3; ++i) { wchar_t path[MAX_PATH * 2]; if (DragQueryFileW(drop, i, path, MAX_PATH * 2)) files.push_back(path); }
            }
            ReleaseStgMedium(&sm);
        }
        count_ = files.empty() ? 1 : static_cast<int>(files.size());
        for (int i = 0; i < count_; ++i) {
            PayloadCommand c; c.kind = PayloadCommand::Icon; c.slot = i;
            c.pixels = files.empty() ? StockIcon(SIID_DOCNOASSOC) : FileIcon(files[i].c_str());
            PostPayload(std::move(c));
        }
        Hover(pt);
        if (helper_) { POINT p{pt.x, pt.y}; helper_->DragEnter(g_overlay, data, &p, *effect); helper_->Show(FALSE); }  // we draw the icon
        *effect = Effect(*effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        Hover(pt);
        if (helper_) { POINT p{pt.x, pt.y}; helper_->DragOver(&p, *effect); }
        *effect = Effect(*effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        for (int i = 0; i < count_; ++i) { PayloadCommand c; c.kind = PayloadCommand::Cancel; c.slot = i; PostPayload(c); }
        if (helper_) helper_->DragLeave();
        g_dropActive = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        Hover(pt);
        // Visual only: nothing is copied or moved (DockDrop will transfer later). COPY leaves the source untouched.
        for (int i = 0; i < count_; ++i) { PayloadCommand c; c.kind = PayloadCommand::Release; c.slot = i; c.delay = 0; PostPayload(c); }  // all at once: each keeps its motion
        if (helper_) { POINT p{pt.x, pt.y}; helper_->Drop(data, &p, *effect); }
        *effect = Effect(*effect);
        g_dropActive = false;
        return S_OK;
    }

private:
    static DWORD Effect(DWORD allowed) { return (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_NONE; }
    void Hover(POINTL pt) {
        double hx, hy; float scale; { std::lock_guard<std::mutex> lock(g.m); hx = g.holeX; hy = g.holeY; scale = g.dpiScale; }
        for (int i = 0; i < count_; ++i) {  // several files: a small fanned stack
            PayloadCommand c; c.kind = PayloadCommand::Hover; c.slot = i;
            c.dx = static_cast<float>(pt.x - hx + 9.0 * i * scale); c.dy = static_cast<float>(pt.y - hy - 7.0 * i * scale); c.half = 30.f * scale;
            PostPayload(c);
        }
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<IDropTargetHelper> helper_;
    int count_ = 0;
};

void RefreshAccessibility() {
    std::lock_guard<std::mutex> lock(g.m);
    g.reducedMotion = ReducedMotionSetting();
    g.highContrast = HighContrastSetting();
}

void SyncPanel();

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        POINT c{}; GetCursorPos(&c);
        { double hx, hy; { std::lock_guard<std::mutex> lock(g.m); hx = g.holeX; hy = g.holeY; }
          if (std::hypot(c.x - hx, c.y - hy) > std::max(ShadowRadiusPx() * 1.25, 24.0)) return 0; }
        { std::lock_guard<std::mutex> lock(g.m); g_grabDx = c.x - g.holeX; g_grabDy = c.y - g.holeY; }
        g_dragging = true;
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONDBLCLK: PostAnim(HolePhase::Consume); return 0;
    case WM_RBUTTONDOWN: {
        GetCursorPos(&g_dragStart); g_tilting = true;
        { std::lock_guard<std::mutex> lock(g.m); g_tiltStart = g.p.inclDeg; }
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT c{}; GetCursorPos(&c);
        if (g_dragging) {
            { std::lock_guard<std::mutex> lock(g.m); g.holeX = c.x - g_grabDx; g.holeY = c.y - g_grabDy; }
            PlaceOverlay();
        } else if (g_tilting) {
            const float k = 0.35f / (GetDpiForWindow(h) / 96.f);
            { std::lock_guard<std::mutex> lock(g.m); g.p.inclDeg = std::clamp(g_tiltStart - (c.y - g_dragStart.y) * k, 0.f, 89.5f); }
            g.paramVersion++;
        }
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP:
        if (g_dragging || g_tilting) { g_dragging = g_tilting = false; ReleaseCapture(); SyncPanel(); }
        return 0;
    case WM_CAPTURECHANGED: g_dragging = g_tilting = false; return 0;
    case WM_MOUSEWHEEL: {
        const float steps = GET_WHEEL_DELTA_WPARAM(w) / static_cast<float>(WHEEL_DELTA);
        {
            std::lock_guard<std::mutex> lock(g.m);
            if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) g.p.lens = std::clamp(g.p.lens * std::pow(1.15f, steps), 0.f, 60.f);
            else if (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) g.p.inclDeg = std::clamp(g.p.inclDeg + steps * 3.f, 0.f, 89.5f);
            else g.p.sizeDip = std::clamp(g.p.sizeDip * std::pow(1.1f, steps), 2.f, 40.f);
        }
        g.paramVersion++; SyncPanel();
        return 0;
    }
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        { std::lock_guard<std::mutex> lock(g.m); SetRectEmpty(&g.monitorRect); }
        PlaceOverlay();
        return 0;
    case WM_SETTINGCHANGE: RefreshAccessibility(); return 0;
    case WM_CLOSE: g.quit = true; DestroyWindow(h); return 0;
    case WM_DESTROY: if (!g_panel) PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// =====================================================================================================================
// Parameter panel (UI thread)
// =====================================================================================================================
struct SliderDef { const wchar_t* name; float Params::*field; float lo, hi; const wchar_t* fmt; };
const SliderDef kSliders[] = {
    {L"Size (DIP per M)", &Params::sizeDip, 2.f, 40.f, L"%.1f"},
    {L"Lens strength: source distance D (M)", &Params::lens, 0.f, 60.f, L"%.1f"},
    {L"Lens field radius (x shadow)", &Params::regionMul, 3.f, 12.f, L"%.1f"},
    {L"Lens taper start (x field)", &Params::taper, 0.3f, 0.85f, L"%.2f"},
    {L"Tidal stretch of approaching icons", &Params::tidal, 0.f, 3.f, L"%.2f"},
    {L"Hole motion inertia", &Params::smoothing, 0.f, 1.f, L"%.2f"},
    {L"Sky drift (slowly orbiting observer)", &Params::drift, 0.f, 1.f, L"%.2f"},
    {L"Disk: viewing angle (deg)", &Params::inclDeg, 0.f, 89.5f, L"%.1f"},
    {L"Disk: inner radius (M, ISCO = 6)", &Params::diskIn, 3.1f, 14.f, L"%.2f"},
    {L"Disk: outer radius (M)", &Params::diskOut, 8.f, 40.f, L"%.1f"},
    {L"Disk: opacity", &Params::diskOpacity, 0.f, 1.f, L"%.2f"},
    {L"Disk: brightness", &Params::diskBrightness, 0.2f, 2.5f, L"%.2f"},
    {L"Disk: rotation speed", &Params::spin, 0.f, 4.f, L"%.2f"},
    {L"Disk: Doppler beaming", &Params::doppler, 0.f, 1.f, L"%.2f"},
    {L"Disk: accretion glow (0 = pure desktop)", &Params::heat, 0.f, 1.f, L"%.2f"},
};
constexpr int kSliderCount = sizeof(kSliders) / sizeof(kSliders[0]);
enum : int { IdSlider = 100, IdLabel = 200, IdQuality = 300, IdShowDisk = 310, IdRetro = 311, IdShred = 312, IdReset = 320, IdSnap = 321,
             IdAnim = 400, IdStats = 500, IdDemoFall = 330, IdDemoEmit = 331, IdDemoMulti = 332 };
HWND g_sliders[kSliderCount], g_labels[kSliderCount], g_quality[3], g_showDisk, g_retro, g_shred, g_reset, g_snap, g_anim[6], g_stats, g_hint, g_demoFall, g_demoEmit, g_demoMulti;
HFONT g_font = nullptr;
bool g_syncing = false;

void SetLabel(int i, float value) {
    wchar_t v[32]; swprintf_s(v, kSliders[i].fmt, value);
    std::wstring s = std::wstring(kSliders[i].name) + L":  " + v;
    SetWindowTextW(g_labels[i], s.c_str());
}

void SyncPanel() {
    if (!g_panel) return;
    Params p; { std::lock_guard<std::mutex> lock(g.m); p = g.p; }
    g_syncing = true;
    for (int i = 0; i < kSliderCount; ++i) {
        const float v = p.*(kSliders[i].field);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround((v - kSliders[i].lo) / (kSliders[i].hi - kSliders[i].lo) * 1000)));
        SetLabel(i, v);
    }
    for (int i = 0; i < 3; ++i) SendMessageW(g_quality[i], BM_SETCHECK, static_cast<int>(p.quality) == i ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_showDisk, BM_SETCHECK, p.showDisk ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_retro, BM_SETCHECK, p.retrograde ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_shred, BM_SETCHECK, p.shred ? BST_CHECKED : BST_UNCHECKED, 0);
    g_syncing = false;
}

void LayoutPanel(HWND h) {
    const float s = GetDpiForWindow(h) / 96.f;
    auto px = [&](float v) { return static_cast<int>(std::lround(v * s)); };
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{sizeof(ncm)}; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(h));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    int y = 10;
    const int x = px(12), w = px(360);
    for (int i = 0; i < kSliderCount; ++i) {
        SetWindowPos(g_labels[i], nullptr, x, px(static_cast<float>(y)), w, px(18), SWP_NOZORDER);
        SetWindowPos(g_sliders[i], nullptr, x - px(4), px(y + 17.f), w + px(8), px(24), SWP_NOZORDER);
        y += 43;
    }
    for (int i = 0; i < 3; ++i) SetWindowPos(g_quality[i], nullptr, x + px(120.f * i), px(static_cast<float>(y)), px(118), px(22), SWP_NOZORDER);
    y += 28;
    SetWindowPos(g_showDisk, nullptr, x, px(static_cast<float>(y)), px(150), px(22), SWP_NOZORDER);
    SetWindowPos(g_retro, nullptr, x + px(160), px(static_cast<float>(y)), px(200), px(22), SWP_NOZORDER);
    y += 26;
    SetWindowPos(g_shred, nullptr, x, px(static_cast<float>(y)), px(360), px(22), SWP_NOZORDER);
    y += 30;
    for (int i = 0; i < 6; ++i) SetWindowPos(g_anim[i], nullptr, x + px(120.f * (i % 3)), px(y + 30.f * (i / 3)), px(114), px(26), SWP_NOZORDER);
    y += 64;
    SetWindowPos(g_demoFall, nullptr, x, px(static_cast<float>(y)), px(174), px(26), SWP_NOZORDER);
    SetWindowPos(g_demoEmit, nullptr, x + px(186), px(static_cast<float>(y)), px(174), px(26), SWP_NOZORDER);
    y += 32;
    SetWindowPos(g_demoMulti, nullptr, x, px(static_cast<float>(y)), px(360), px(26), SWP_NOZORDER);
    y += 32;
    SetWindowPos(g_reset, nullptr, x, px(static_cast<float>(y)), px(174), px(26), SWP_NOZORDER);
    SetWindowPos(g_snap, nullptr, x + px(186), px(static_cast<float>(y)), px(174), px(26), SWP_NOZORDER);
    y += 34;
    SetWindowPos(g_stats, nullptr, x, px(static_cast<float>(y)), w, px(118), SWP_NOZORDER);
    y += 122;
    SetWindowPos(g_hint, nullptr, x, px(static_cast<float>(y)), w, px(68), SWP_NOZORDER);
    y += 76;
    EnumChildWindows(h, [](HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, f, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(g_font));
    RECT r{0, 0, px(384), px(static_cast<float>(y))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), FALSE, 0, GetDpiForWindow(h));
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateStats() {
    Stats s; Params p; bool tp;
    { std::lock_guard<std::mutex> lock(g.m); s = g.stats; p = g.p; tp = g.testPattern; }
    wchar_t text[768];
    swprintf_s(text,
        L"%ls%ls   phase: %ls\r\n"
        L"%.1f fps (display ~%.0f Hz)   CPU %.2f ms   GPU %.3f ms\r\n"
        L"desktop: %ls, %.1f new frames/s\r\n"
        L"desktop composed -> Present: %.1f ms (p95 %.1f), -> est. display %.1f ms\r\n"
        L"lens target %dx%d px, %d spp (adaptive at the photon ring)",
        QualityName(s.effective), s.batterySaver ? L" (battery saver)" : L"", PhaseName(s.phase),
        s.fps, s.refreshHz, s.cpuMs, s.gpuMs,
        tp ? L"test pattern" : (s.desktop ? (s.owner ? L"DXGI duplication, owner (low latency)" : L"DXGI duplication, shared") : L"waiting"), s.desktopFps,
        s.latMs, s.latP95, s.latEstMs, s.targetW, s.targetH, s.spp);
    SetWindowTextW(g_stats, text);
    (void)p;
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_panel = h;  // SyncPanel below needs it (CreateWindowEx has not returned yet)
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        for (int i = 0; i < kSliderCount; ++i) {
            g_labels[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdLabel + i)), inst, nullptr);
            g_sliders[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSlider + i)), inst, nullptr);
            SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        }
        const wchar_t* q[3] = {L"High quality", L"Balanced", L"Low power"};
        for (int i = 0; i < 3; ++i) g_quality[i] = CreateWindowExW(0, L"BUTTON", q[i], WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0), 0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdQuality + i)), inst, nullptr);
        g_showDisk = CreateWindowExW(0, L"BUTTON", L"Accretion disk (optional)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_GROUP, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdShowDisk), inst, nullptr);
        g_retro = CreateWindowExW(0, L"BUTTON", L"Reverse disk rotation", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdRetro), inst, nullptr);
        g_shred = CreateWindowExW(0, L"BUTTON", L"Shred icons into pixels (tidal disruption)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdShred), inst, nullptr);
        const wchar_t* a[6] = {L"Spawn", L"Idle", L"Attract", L"Consume", L"Emit", L"Collapse"};
        for (int i = 0; i < 6; ++i) g_anim[i] = CreateWindowExW(0, L"BUTTON", a[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdAnim + i)), inst, nullptr);
        g_demoFall = CreateWindowExW(0, L"BUTTON", L"Drop a test icon in", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdDemoFall), inst, nullptr);
        g_demoEmit = CreateWindowExW(0, L"BUTTON", L"Emit a test icon", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdDemoEmit), inst, nullptr);
        g_demoMulti = CreateWindowExW(0, L"BUTTON", L"Multi-file test: 3 files in, then 3 out", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdDemoMulti), inst, nullptr);
        g_reset = CreateWindowExW(0, L"BUTTON", L"Reset parameters", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdReset), inst, nullptr);
        g_snap = CreateWindowExW(0, L"BUTTON", L"Save lens snapshot (BMP)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdSnap), inst, nullptr);
        g_stats = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, h, reinterpret_cast<HMENU>(IdStats), inst, nullptr);
        g_hint = CreateWindowExW(0, L"STATIC", L"On the hole: drag = move, wheel = size, Ctrl+wheel = lens, Shift+wheel or right-drag = disk angle, "
                                 L"double-click = Consume. Drag files onto the field: they stretch and fall in (nothing is copied). "
                                 L"Ctrl+Alt+B shows/hides this panel.", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, h, nullptr, inst, nullptr);
        LayoutPanel(h);
        SyncPanel();
        SetTimer(h, kStatsTimer, 250, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutPanel(h);
        return 0;
    }
    case WM_HSCROLL: {
        if (g_syncing) return 0;
        for (int i = 0; i < kSliderCount; ++i) if (reinterpret_cast<HWND>(l) == g_sliders[i]) {
            const float t = static_cast<float>(SendMessageW(g_sliders[i], TBM_GETPOS, 0, 0)) / 1000.f;
            const float v = kSliders[i].lo + t * (kSliders[i].hi - kSliders[i].lo);
            { std::lock_guard<std::mutex> lock(g.m); g.p.*(kSliders[i].field) = v; }
            SetLabel(i, v); g.paramVersion++;
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id >= IdQuality && id < IdQuality + 3) { std::lock_guard<std::mutex> lock(g.m); g.p.quality = static_cast<Quality>(id - IdQuality); }
        else if (id == IdShowDisk) { std::lock_guard<std::mutex> lock(g.m); g.p.showDisk = SendMessageW(g_showDisk, BM_GETCHECK, 0, 0) == BST_CHECKED; }
        else if (id == IdShred) { std::lock_guard<std::mutex> lock(g.m); g.p.shred = SendMessageW(g_shred, BM_GETCHECK, 0, 0) == BST_CHECKED; }
        else if (id == IdRetro) { std::lock_guard<std::mutex> lock(g.m); g.p.retrograde = SendMessageW(g_retro, BM_GETCHECK, 0, 0) == BST_CHECKED; }
        else if (id == IdDemoFall || id == IdDemoEmit || id == IdDemoMulti) {
            PayloadCommand c; c.kind = PayloadCommand::Demo; c.dx = id == IdDemoFall ? 1.f : id == IdDemoEmit ? 2.f : 7.f; PostPayload(c);
        }
        else if (id == IdReset) { { std::lock_guard<std::mutex> lock(g.m); const Quality q = g.p.quality; g.p = Params{}; g.p.quality = q; } SyncPanel(); }
        else if (id == IdSnap) {
            wchar_t path[MAX_PATH]; GetTempPathW(MAX_PATH, path);
            SYSTEMTIME t; GetLocalTime(&t);
            wchar_t name[96]; swprintf_s(name, L"blackhole-%04d%02d%02d-%02d%02d%02d.bmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            std::lock_guard<std::mutex> lock(g.m); g.snapshotPath = std::wstring(path) + name;
            g_snapshotAfter = 0;
            SetWindowTextW(g_snap, L"Saved to %TEMP%");
        } else if (id >= IdAnim && id < IdAnim + 6) {
            const HolePhase phases[6] = {HolePhase::Spawn, HolePhase::Idle, HolePhase::Attract, HolePhase::Consume, HolePhase::Emit, HolePhase::Collapse};
            POINT c{}; GetCursorPos(&c);
            double hx, hy; { std::lock_guard<std::mutex> lock(g.m); hx = g.holeX; hy = g.holeY; }
            // Attract preview: the payload is where the panel's button is (a stand-in for a dragged file).
            PostAnim(phases[id - IdAnim], static_cast<float>(c.x - hx), static_cast<float>(c.y - hy));
        }
        g.paramVersion++;
        return 0;
    }
    case WM_TIMER:
        if (w == kStatsTimer) UpdateStats();
        return 0;
    case kMsgStats: return 0;
    case WM_SETTINGCHANGE: RefreshAccessibility(); return 0;
    case WM_CLOSE: g.quit = true; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK HiddenProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER && w == kHitTimer) { UpdateHitTest(); return 0; }
    if (msg == WM_HOTKEY && w == kHotkeyPanel && g_panel) { ShowWindow(g_panel, IsWindowVisible(g_panel) ? SW_HIDE : SW_SHOW); return 0; }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool noPanel = false, quitOther = false, havePos = false;
    std::wstring logPath, snapshot;
    double x = 0, y = 0;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--quality") { const auto q = next(); g.p.quality = q == L"high" ? Quality::High : q == L"low" ? Quality::Low : Quality::Balanced; }
        else if (a == L"--test-pattern") g.testPattern = true;
        else if (a == L"--no-panel") noPanel = true;
        else if (a == L"--size") g.p.sizeDip = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--incl") g.p.inclDeg = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--lens") g.p.lens = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--no-disk") g.p.showDisk = false;
        else if (a == L"--glow") g.p.heat = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--disk") g.p.showDisk = true;
        else if (a == L"--no-shred") g.p.shred = false;
        else if (a == L"--drift") g.p.drift = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--tidal") g.p.tidal = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--demo") {
            const auto d = next();
            g.demoMode = d == L"emit" ? 2 : d == L"loop" ? 3 : d == L"multi-infall" ? 4 : d == L"multi-emit" ? 5 : d == L"multi" ? 6 : 1;
        }
        else if (a == L"--snapshot-seq") g_seqDir = next();
        else if (a == L"--snapshot-interval") g_seqInterval = std::max(0.005, _wtof(next().c_str()));
        else if (a == L"--snapshot-count") g_seqCount = std::clamp(_wtoi(next().c_str()), 0, 2000);
        else if (a == L"--x") { x = _wtof(next().c_str()); havePos = true; }
        else if (a == L"--y") { y = _wtof(next().c_str()); havePos = true; }
        else if (a == L"--pump-wait") g_pumpWaitMs = std::clamp(_wtoi(next().c_str()), 0, 8);
        else if (a == L"--log") logPath = next();
        else if (a == L"--snapshot") snapshot = next();
        else if (a == L"--snapshot-after") g_snapshotAfter = _wtof(next().c_str());
        else if (a == L"--exit-after") g_exitAfter = _wtof(next().c_str());
        else if (a == L"--bench") g_benchSeconds = std::max(1.0, _wtof(next().c_str()));
        else if (a == L"--bench-out") g_benchOut = next();
        else if (a == L"--quit") quitOther = true;
    }
    if (argv) LocalFree(argv);

    if (quitOther) {
        for (const wchar_t* cls : {kPanelClass, kOverlayClass}) if (HWND w = FindWindowW(cls, nullptr)) PostMessageW(w, WM_CLOSE, 0, 0);
        return 0;
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DockOverBlackHoleVisualizer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (HWND w = FindWindowW(kPanelClass, nullptr)) { ShowWindow(w, SW_SHOW); SetForegroundWindow(w); } return 0; }

    if (logPath.empty()) {
        wchar_t* local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            std::wstring dir = std::wstring(local) + L"\\DockyBlackHole";
            CreateDirectoryW(dir.c_str(), nullptr);
            logPath = dir + L"\\blackhole.log";
            if (g_benchOut.empty()) g_benchOut = dir + L"\\bench.txt";
            CoTaskMemFree(local);
        }
    }
    dock::Log::Get().Open(logPath);
    if (g_benchOut.empty()) g_benchOut = L"blackhole-bench.txt";
    DLOG(L"blackhole: start (quality %ls, %ls)", QualityName(g.p.quality), g.testPattern ? L"test pattern" : L"live desktop");
    g.snapshotPath = snapshot;
    RefreshAccessibility();

    OleInitialize(nullptr);  // drag-and-drop target and the shell's icon lists
    g.demoIcons = {StockIcon(SIID_FOLDER), StockIcon(SIID_IMAGEFILES), StockIcon(SIID_ZIPFILE)};
    if (!g_seqDir.empty() && g_seqCount == 0) g_seqCount = 30;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);

    if (!havePos) {  // default: centre of the primary monitor's work area
        POINT o{0, 0}; MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = (mi.rcWork.left + mi.rcWork.right) / 2.0; y = (mi.rcWork.top + mi.rcWork.bottom) / 2.0;
    }
    g.holeX = x; g.holeY = y;

    WNDCLASSW wc{}; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpfnWndProc = OverlayProc; wc.lpszClassName = kOverlayClass; wc.style = CS_DBLCLKS; RegisterClassW(&wc);
    // Click-through except on the hole itself (UpdateHitTest), top-most, never activated, excluded from every capture
    // (desktop duplication, WGC, screenshots): the lens never sees itself.
    g_overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kOverlayClass, L"DockOver Black Hole", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g_overlay) return 1;
    SetLayeredWindowAttributes(g_overlay, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE))
        DLOG_WARN(L"blackhole: WDA_EXCLUDEFROMCAPTURE unavailable; the lens would capture itself");
    PlaceOverlay();
    DropTarget* dropTarget = new DropTarget;
    if (FAILED(RegisterDragDrop(g_overlay, dropTarget))) DLOG_WARN(L"blackhole: drag-and-drop target unavailable");

    WNDCLASSW hc{}; hc.hInstance = inst; hc.lpfnWndProc = HiddenProc; hc.lpszClassName = L"DockOverBlackHoleTimer"; RegisterClassW(&hc);
    HWND timerWnd = CreateWindowExW(0, hc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    SetTimer(timerWnd, kHitTimer, 15, nullptr);
    RegisterHotKey(timerWnd, kHotkeyPanel, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B');

    if (!noPanel) {
        WNDCLASSW pc{}; pc.hInstance = inst; pc.lpfnWndProc = PanelProc; pc.lpszClassName = kPanelClass; pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        pc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); RegisterClassW(&pc);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(POINT{static_cast<LONG>(x), static_cast<LONG>(y)}, MONITOR_DEFAULTTONEAREST), &mi);
        g_panel = CreateWindowExW(0, kPanelClass, L"DockOver Black Hole Visualizer", WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
                                  mi.rcWork.left + 24, mi.rcWork.top + 24, 400, 900, nullptr, nullptr, inst, nullptr);
        ShowWindow(g_panel, SW_SHOWNOACTIVATE);
    }

    RenderThread renderer;
    std::thread rt([&] { renderer.Run(); });
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_panel && IsDialogMessageW(g_panel, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    g.quit = true;
    rt.join();
    if (g_overlay && IsWindow(g_overlay)) { RevokeDragDrop(g_overlay); DestroyWindow(g_overlay); }
    dropTarget->Release();
    UnregisterHotKey(timerWnd, kHotkeyPanel);
    DLOG(L"blackhole: exit");
    dock::Log::Get().Close();
    OleUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
