#pragma once
// DXGI desktop duplication of one monitor as a live, GPU-only glass backdrop for docky Desktop (which has no package
// identity, so Windows Graphics Capture would draw the yellow capture frame). Windows excluded from capture
// (WDA_EXCLUDEFROMCAPTURE: the panels and menus themselves) are not in the image.
//
// One duplication per process (Windows refuses a second one of the same monitor, E_INVALIDARG). SharedDesktop owns it on
// its own device and keeps an up-to-date copy of the monitor, copying only the rectangles Windows reports as changed, in a
// texture shared with every consumer's device. A DesktopDuplication is one consumer (a panel, its blur halo, a menu): it
// reads that texture and redraws only when a change touched its own area, at up to the display rate.
//
// Frames are taken on a background thread that sleeps until Windows has a new one (no polling on the UI thread). Changes
// that lie entirely inside docky Desktop's own windows are ignored: a panel's own redraw is reported as a changed region
// even though the panel is excluded from the image, and reacting to it redrew the panel forever at the display rate.
//
// docky (Dock, menus, Control Center, Launchpad, Settings) uses one SharedDesktop per monitor (ForMonitor) through
// WgcBackdropSource: listeners get a window message when a change touches their area, and ChangedKind tells real changes
// from the consumer's own (excluded) window redrawing, which only needs a GPU comparison.
//
// HDR / advanced-colour desktops: duplication hands out FP16 scRGB frames; changed rectangles are converted on the GPU to
// sRGB at the monitor's SDR white level (the "SDR content brightness" setting), so the glass looks as on an SDR desktop.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../backdrop.h"
#include "../capture_visible.h"
#include "../log.h"
#include "../vblank.h"

namespace docky::desk {

class SharedDesktop {
public:
    // Never destroyed: consumers living in static objects may still release it during exit.
    static SharedDesktop& Get() { static SharedDesktop* s = new SharedDesktop; return *s; }
    // One per monitor (docky's glass backdrops). Never destroyed, like Get().
    static SharedDesktop& ForMonitor(HMONITOR monitor) {
        static std::mutex m; static auto* all = new std::map<HMONITOR, SharedDesktop*>;
        std::lock_guard<std::mutex> lock(m);
        auto& d = (*all)[monitor];
        if (!d) { d = new SharedDesktop; d->listenersOnly_ = true; }  // every consumer registers its area (Listen)
        return *d;
    }

    // Consumers: the monitor they need and whether duplication is available for it (started on the background thread).
    bool Want(const RECT& area) {
        std::unique_lock<std::mutex> lock(mutex_);
        wanted_ = area;
        const POINT c{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        if (owner_ && GetCurrentThreadId() == ownerThread_) {  // owner: create it here, on the render device
            if (dup_ && PtInRect(&outputRect_, c)) return true;
            if (failed_ && GetTickCount64() - failedAt_ < 5000) return false;
            if (Ensure(area)) { changed_.notify_all(); return true; }
            RevertOwnerLocked();  // continues below with the background thread
        }
        if (!owner_ && !thread_.joinable()) thread_ = std::thread([this] { Run(); });
        if (!dup_) wake_.notify_all();
        if (dup_ && PtInRect(&outputRect_, c)) return true;
        if (failed_) return false;  // unavailable: never block the caller (GDI fallback); the thread retries every 5 s
        changed_.wait_for(lock, std::chrono::milliseconds(150), [&] { return (dup_ && PtInRect(&outputRect_, c)) || failed_.load(); });
        return dup_ && PtInRect(&outputRect_, c);
    }
    // Waits up to `ms` for the first image.
    bool WaitValid(UINT ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return valid_ || !dup_; });
        return valid_;
    }
    // A consumer's own window (excluded from the image): changes entirely inside it are its own redraws.
    void SetOwn(const void* consumer, const RECT& window) { std::lock_guard<std::mutex> lock(mutex_); if (IsRectEmpty(&window)) own_.erase(consumer); else own_[consumer] = window; }

    // ---- Owner mode (low-latency Dock, opt-in) ---------------------------------------------------------------------
    // Measured 2026-11-20 (experiments/hostbackdrop-lab/REPORT-dxgi-latency.md): handing the desktop image from the
    // duplication's own device to the glass renderer's device costs one display frame however it is synchronised (keyed
    // mutex, shared texture + Flush). With an owner, the duplication lives on the owner's render device and the owner's
    // thread takes the frames (PumpOwned) right before it renders, so its glass shows the desktop one frame after it was
    // composed instead of two. Every other consumer keeps working unchanged through the shared image. The background
    // thread parks while an owner exists. Device loss / shutdown: SetOwner(nullptr) on the owner thread.
    void SetOwner(ID3D11Device* device) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (device == ownerDevice_.Get()) return;
        // Park the background thread first: it holds mutex_ from one frame to the next, so merely waiting for !pumping_
        // starved this call forever (the Dock hung when Low latency glass was switched on again, 2026-11-20).
        handover_ = true;
        idle_.wait(lock, [&] { return !pumping_; });  // the background thread is not inside Pump
        handover_ = false;
        Stop(); image_.Reset(); rtv_.Reset(); shared_ = nullptr; vs_.Reset(); ps_.Reset(); cb_.Reset(); scissor_.Reset();
        device_.Reset(); ctx_.Reset(); history_.clear(); stale_.clear(); failed_ = false; ++generation_;
        ownerDevice_ = device; ownerThread_ = device ? GetCurrentThreadId() : 0; owner_ = device != nullptr;
        if (device) { device_ = device; device->GetImmediateContext(&ctx_); }
        else if (users_ > 0 && !thread_.joinable()) thread_ = std::thread([this] { Run(); });
        DLOG(L"desk glass: duplication owner %ls", device ? L"set (pumped on the render thread)" : L"cleared (background thread)");
        wake_.notify_all(); changed_.notify_all();
    }
    bool Owned() const { return owner_; }
    // Owner: a frame is worth waiting for (consumers exist or the duplication still has to be released).
    bool OwnerNeedsPump() const { std::lock_guard<std::mutex> lock(mutex_); return owner_ && (users_ > 0 || dup_); }
    // QueryPerformanceCounter value of the newest frame's LastPresentTime (when DWM showed it); 0 when unknown.
    std::int64_t LastPresentQpc() const { return lastPresentQpc_; }
    // Owner thread only: create the duplication if needed, take the next frame waiting at most `timeoutMs`, copy it into
    // the shared image and notify listeners (the owner is a listener too). True when a new frame was taken.
    bool PumpOwned(UINT timeoutMs) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!owner_ || GetCurrentThreadId() != ownerThread_) return false;
            if (users_ == 0) {  // released 2 s after the last consumer (same as the background thread)
                if (dup_) { if (!idleSince_) idleSince_ = GetTickCount64(); else if (GetTickCount64() - idleSince_ >= 2000) { Stop(); history_.clear(); idleSince_ = 0; DLOG(L"desk glass: desktop duplication released"); } }
                return false;
            }
            idleSince_ = 0;
            if (!dup_) {
                if (failed_ && GetTickCount64() - failedAt_ < 5000) return false;
                const bool ok = Ensure(wanted_);
                changed_.notify_all();
                if (!ok) { RevertOwnerLocked(); return false; }
            }
        }
        const std::uint64_t before = serial_;
        Pump(timeoutMs);
        std::lock_guard<std::mutex> lock(mutex_);
        const ULONGLONG now = GetTickCount64();
        for (auto& kv : listeners_) {  // own-window redraws that were held back (as in Run)
            Listener& l = kv.second;
            if (l.ownPending && !l.posted && (!l.large || now - l.lastOwnPost >= 33)) { l.ownPending = false; l.posted = true; l.lastOwnPost = now; PostMessageW(l.hwnd, l.msg, 0, 0); }
        }
        return serial_ != before;
    }
private:
    // The owner's device cannot duplicate this monitor (another adapter, rotated output, ...): back to the background
    // thread with its own device. Under mutex_, on the owner thread.
    void RevertOwnerLocked() {
        DLOG(L"desk glass: owner device cannot duplicate this monitor; background thread takes over (no low-latency path)");
        Stop(); image_.Reset(); rtv_.Reset(); shared_ = nullptr; vs_.Reset(); ps_.Reset(); cb_.Reset(); scissor_.Reset();
        device_.Reset(); ctx_.Reset(); ownerDevice_.Reset(); ownerThread_ = 0; owner_ = false; failed_ = false; ++generation_;
        if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
        wake_.notify_all();
    }
public:

private:
    // Duplicates the monitor that contains `area` (background thread). False: unavailable.
    bool Ensure(const RECT& area) {
        const POINT c{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        if (dup_ && PtInRect(&outputRect_, c)) return true;
        if (failed_ && GetTickCount64() - failedAt_ < 5000) return false;  // retry later (a mode change can fix it)
        Stop();
        using Microsoft::WRL::ComPtr;
        if (!device_) {
            if (owner_) return Fail(E_FAIL);
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &ctx_)))
                return Fail(E_FAIL);
        }
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))) return Fail(E_NOINTERFACE);
        ComPtr<IDXGIOutput> output;
        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i, output.Reset()) {
            DXGI_OUTPUT_DESC od{}; output->GetDesc(&od);
            if (!PtInRect(&od.DesktopCoordinates, c)) continue;
            if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) return Fail(E_NOTIMPL);
            HRESULT hr = E_NOINTERFACE;
            ComPtr<IDXGIOutput5> o5; ComPtr<IDXGIOutput1> o1;
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
            if (SUCCEEDED(output.As(&o5))) hr = o5->DuplicateOutput1(device_.Get(), 0, 2, formats, &dup_);
            else if (SUCCEEDED(output.As(&o1))) hr = o1->DuplicateOutput(device_.Get(), &dup_);
            if (FAILED(hr)) return Fail(hr);
            outputRect_ = od.DesktopCoordinates;
            outputName_ = od.DeviceName;
            sdrWhite_ = SdrWhite(outputName_.c_str());
            nextWhiteCheck_ = GetTickCount64() + 1000;
            floatSharingFailed_ = false;
            ++generation_;  // consumers reopen the shared texture
            failed_ = false; retry_ = false;
            DLOG(L"desk glass: desktop duplication on (%ld,%ld)-(%ld,%ld), SDR white %.2f", outputRect_.left, outputRect_.top, outputRect_.right, outputRect_.bottom, sdrWhite_);
            return true;
        }
        return Fail(DXGI_ERROR_NOT_FOUND);
    }
public:
    bool Active() const { return dup_ != nullptr; }
    bool Valid() const { return valid_; }
    RECT OutputRect() const { std::lock_guard<std::mutex> lock(mutex_); return outputRect_; }
    HANDLE SharedHandle() const { std::lock_guard<std::mutex> lock(mutex_); return shared_; }
    unsigned Generation() const { return generation_; }
    std::uint64_t Serial() const { return serial_; }
    std::int64_t FrameQpc() const { return frameQpc_; }  // QueryPerformanceCounter when the newest frame was acquired
private:

    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (owner_) { idleSince_ = 0; wake_.wait(lock); continue; }  // the owner's thread takes the frames
            if (handover_) { idle_.notify_all(); wake_.wait_for(lock, std::chrono::milliseconds(5)); continue; }  // SetOwner pending
            if (users_ == 0) {
                // Kept 2 s after the last consumer stops (no frames taken meanwhile): a backdrop that restarts right away
                // (the Dock after Launchpad, a menu reopened) finds it running instead of waiting for a new duplication.
                if (dup_) {
                    if (!idleSince_) idleSince_ = GetTickCount64();
                    const ULONGLONG idle = GetTickCount64() - idleSince_;
                    if (idle < 2000) { wake_.wait_for(lock, std::chrono::milliseconds(2000 - idle)); continue; }
                    Stop(); history_.clear(); DLOG(L"desk glass: desktop duplication released");
                }
                idleSince_ = 0; wake_.wait(lock); continue;
            }
            idleSince_ = 0;
            if (!dup_) {
                if (failed_ && GetTickCount64() - failedAt_ < 5000) { retry_ = true; wake_.wait_for(lock, std::chrono::milliseconds(500)); continue; }
                const RECT want = wanted_;
                const bool ok = Ensure(want);
                changed_.notify_all();
                if (!ok) continue;
            }
            pumping_ = true;  // SetOwner waits until the frame below is finished
            lock.unlock();
            LARGE_INTEGER a{}, b{}, f{}; QueryPerformanceCounter(&a);
            const std::uint64_t before = serial_;
            Pump(100);  // sleeps until Windows has a new frame (or 100 ms)
            QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
            if (serial_ != before) { ++statFrames_; statMs_ += (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart - lastWaitMs_;
                // Settings > General > Frame rate > capture: wait out the rest of the interval before the next frame.
                if (const int cap = docky::vblank::CaptureCap(); cap > 0) {
                    const double spent = (b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) - lastWaitMs_ / 1000.0;
                    docky::vblank::Pause(1.0 / cap - spent);
                }
            }
            {  // own-window redraws that were held back (at most one wake-up per listener every 33 ms)
                std::lock_guard<std::mutex> relock(mutex_);
                const ULONGLONG now = GetTickCount64();
                for (auto& kv : listeners_) {
                    Listener& l = kv.second;
                    if (l.ownPending && !l.posted && (!l.large || now - l.lastOwnPost >= 33)) { l.ownPending = false; l.posted = true; l.lastOwnPost = now; PostMessageW(l.hwnd, l.msg, 0, 0); }
                }
            }
            if (GetTickCount64() - statSince_ >= 2000) { DLOG_DEBUG(L"desk glass: duplication %.1f frames/s, %.2f ms work per frame, %zu own windows", statFrames_ / 2.0, statFrames_ ? statMs_ / statFrames_ : 0.0, own_.size()); statFrames_ = 0; statMs_ = 0; statSince_ = GetTickCount64(); }
            lock.lock();
            pumping_ = false; idle_.notify_all();
        }
    }
    // Takes the next frame if there is one (waits up to `timeoutMs`); copies the changed rectangles into the shared image.
    // Each consumer later asks Changed(area, since) whether one of them touched its area.
    void Pump(UINT timeoutMs) {
        if (!dup_) return;  // a second consumer in the same frame just finds no new frame (cheap timeout)
        using Microsoft::WRL::ComPtr;
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> res;
        LARGE_INTEGER w0{}, w1{}, wf{}; QueryPerformanceCounter(&w0);
        const HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
        QueryPerformanceCounter(&w1);
        if (SUCCEEDED(hr)) { frameQpc_ = w1.QuadPart; if (info.LastPresentTime.QuadPart) { lastPresentQpc_ = info.LastPresentTime.QuadPart; ++acquired_; accumulated_ += info.AccumulatedFrames; } }
        QueryPerformanceFrequency(&wf); lastWaitMs_ = (w1.QuadPart - w0.QuadPart) * 1000.0 / wf.QuadPart;
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
        if (FAILED(hr)) { DLOG(L"desk glass: duplication lost (0x%08X)", static_cast<unsigned>(hr)); std::lock_guard<std::mutex> lock(mutex_); Stop(); changed_.notify_all(); return; }
        // Dev fault injection (--dev-isolated + DOCKY_TEST_DUP_LOST_AFTER=n): after n frames, behave as if Windows had
        // taken the duplication away (secure desktop, UAC, lock screen, mode change: DXGI_ERROR_ACCESS_LOST), once.
        static const unsigned long long lostAfter = [] { wchar_t v[16]{}; return wcsstr(GetCommandLineW(), L"--dev-isolated") && GetEnvironmentVariableW(L"DOCKY_TEST_DUP_LOST_AFTER", v, 16) ? _wtoi64(v) : 0; }();
        static bool injected = false;
        if (lostAfter && !injected && acquired_ >= lostAfter) {
            injected = true; dup_->ReleaseFrame();
            DLOG(L"desk glass: duplication lost (0x%08X) [test fault]", static_cast<unsigned>(DXGI_ERROR_ACCESS_LOST));
            std::lock_guard<std::mutex> lock(mutex_); Stop(); changed_.notify_all(); return;
        }
        if (docky::capture_visible::Frozen()) {  // glass windows visible to a screenshot: this frame may contain them
            { std::lock_guard<std::mutex> lock(mutex_); needFull_ = true; needFullSince_ = GetTickCount64(); }
            dup_->ReleaseFrame(); return;
        }
        if (info.LastPresentTime.QuadPart != 0 || !valid_) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC source{};
                tex->GetDesc(&source);
                bool whiteChanged = false;
                const auto now = GetTickCount64();
                if (source.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && now >= nextWhiteCheck_) {
                    const float white = SdrWhite(outputName_.c_str());
                    whiteChanged = white != sdrWhite_;
                    sdrWhite_ = white;
                    nextWhiteCheck_ = now + 1000;
                }
                std::vector<RECT> changed;
                bool full = !valid_ || needFull_ || whiteChanged || !Changes(info, changed);
                // Per-monitor instances (the Dock, its menus and panels) copy only what some consumer looks at: a video or
                // a scrolling page elsewhere on the screen then costs no GPU copy at all. Changes nobody watches are kept
                // as stale areas; a consumer that later looks at one waits for the next full copy (Fresh).
                if (!full && listenersOnly_) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    std::vector<RECT> kept;
                    for (const RECT& r : changed) {
                        RECT s = r; OffsetRect(&s, outputRect_.left, outputRect_.top);
                        bool watched = false;
                        for (const auto& kv : listeners_) { RECT area = kv.second.area, hit{}; InflateRect(&area, 8, 8); if (IntersectRect(&hit, &s, &area)) { watched = true; break; } }
                        if (watched) kept.push_back(r); else stale_.push_back(s);
                    }
                    if (stale_.size() > 32) { RECT u{}; for (const RECT& s : stale_) UnionRect(&u, &u, &s); stale_.assign(1, u); }
                    changed.swap(kept);
                    if (changed.empty()) { if (dup_) dup_->ReleaseFrame(); return; }
                }
                if (GetTickCount64() - excludedAt_ >= 100) RefreshExcluded();  // at most 10 times a second, only when copying
                if (full) { changed.assign(1, RECT{0, 0, outputRect_.right - outputRect_.left, outputRect_.bottom - outputRect_.top}); }
                if (Copy(tex.Get(), changed)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (full) { stale_.clear(); needFull_ = false; }
                    const bool first = !valid_;
                    valid_ = true; ++serial_;
                    const size_t firstNew = history_.size();
                    for (auto r : changed) {
                        OffsetRect(&r, outputRect_.left, outputRect_.top);
                        bool self = false;
                        for (const auto& kv : own_) if (Inside(r, kv.second)) { self = true; break; }
                        if (!self) for (const auto& e : excluded_) if (Inside(r, e)) { self = true; break; }
                        history_.push_back({serial_, r, self});
                    }
                    if (full) history_.push_back({serial_, RECT{LONG_MIN / 2, LONG_MIN / 2, LONG_MAX / 2, LONG_MAX / 2}, false});
                    Notify(firstNew, full);
                    while (history_.size() > 512) history_.erase(history_.begin());
                    if (first) changed_.notify_all();
                }
            }
        }
        if (dup_) dup_->ReleaseFrame();
    }
public:
    // Did a change after `since` touch `area` (screen px)? Consumers too far behind count as changed.
    bool Changed(const RECT& area, std::uint64_t since) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (since == 0 || (!history_.empty() && history_.front().serial > since + 1)) return serial_ > since;
        for (auto it = history_.rbegin(); it != history_.rend() && it->serial > since; ++it) { RECT hit{}; if (!it->self && IntersectRect(&hit, &it->rect, &area)) return true; }
        return false;
    }
    // Like Changed, for a consumer whose own window (`own`, excluded from the image) lies over `area`: 0 nothing changed,
    // 1 only changes inside the own window (usually its own redraw: the pixels behind it may be unchanged), 3 only changes
    // inside windows excluded from capture, at least one of them another window (another docky window redrawing, such as
    // the animating Dock under Control Center: compare before publishing), 2 a real change.
    int ChangedKind(const RECT& area, std::uint64_t since, const RECT& own) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial_ <= since) return 0;
        if (since == 0 || (!history_.empty() && history_.front().serial > since + 1)) return 2;
        int kind = 0;
        for (auto it = history_.rbegin(); it != history_.rend() && it->serial > since; ++it) {
            RECT hit{};
            if (!IntersectRect(&hit, &it->rect, &area)) continue;
            if (Inside(it->rect, own)) { if (kind == 0) kind = 1; }
            else if (it->self) kind = 3;
            else return 2;
        }
        return kind;
    }
    // Window message `msg` to `hwnd` when a change touches `area`; at most one until Listen is called again (re-arm).
    // Changes only inside `own` (the listener's own excluded window) wake it at most every 33 ms.
    void Listen(const void* key, HWND hwnd, UINT msg, const RECT& area, const RECT& own) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!needFull_) for (const RECT& s : stale_) { RECT hit{}; if (IntersectRect(&hit, &s, &area)) { needFull_ = true; needFullSince_ = GetTickCount64(); break; } }
        Listener& l = listeners_[key];
        l.hwnd = hwnd; l.msg = msg; l.area = area; l.own = own; l.posted = false;
        // A large area (Launchpad) wakes for self-only changes at most every 33 ms: comparing a whole screen at the display
        // rate is wasted work. Small areas (Dock, menus) compare every frame.
        const long long a = static_cast<long long>(area.right - area.left) * (area.bottom - area.top);
        const long long o = static_cast<long long>(outputRect_.right - outputRect_.left) * (outputRect_.bottom - outputRect_.top);
        l.large = o > 0 && a * 4 > o;
    }
    void Unlisten(const void* key) { std::lock_guard<std::mutex> lock(mutex_); listeners_.erase(key); }
    // False while `area` holds changes nobody watched when they happened: the next frame copies the whole screen (and
    // notifies everyone), so the caller waits for it instead of showing old pixels. A newly shown window always produces
    // that frame; after 200 ms without one the image is used as it is.
    bool Fresh(const RECT& area) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!needFull_ || GetTickCount64() - needFullSince_ > 200) return true;
        for (const RECT& s : stale_) { RECT hit{}; if (IntersectRect(&hit, &s, &area)) return false; }
        return true;
    }
private:

private:
    struct Change { std::uint64_t serial; RECT rect; bool self; };
    struct Listener { HWND hwnd = nullptr; UINT msg = 0; RECT area{}, own{}; bool posted = false, ownPending = false, large = false; ULONGLONG lastOwnPost = 0; };
    static bool Inside(const RECT& r, RECT own) { if (IsRectEmpty(&own)) return false; InflateRect(&own, 2, 2); RECT u{}; UnionRect(&u, &own, &r); return EqualRect(&u, &own) != FALSE; }
    // Under mutex_: wake the listeners whose area the changes from history_[firstNew] on touch.
    void Notify(size_t firstNew, bool full) {
        const ULONGLONG now = GetTickCount64();
        for (auto& kv : listeners_) {
            Listener& l = kv.second;
            if (l.posted) continue;
            int kind = full ? 2 : 0;
            for (size_t i = firstNew; i < history_.size() && kind < 2; ++i) {
                RECT hit{};
                if (!IntersectRect(&hit, &history_[i].rect, &l.area)) continue;
                kind = (history_[i].self || Inside(history_[i].rect, l.own)) ? (std::max)(kind, 1) : 2;
            }
            if (kind == 2 || (kind == 1 && (!l.large || now - l.lastOwnPost >= 33))) {
                if (kind == 1) l.lastOwnPost = now;
                l.posted = true; l.ownPending = false; PostMessageW(l.hwnd, l.msg, 0, 0);
            } else if (kind == 1) l.ownPending = true;
        }
    }
    std::map<const void*, Listener> listeners_;
    bool listenersOnly_ = false;               // ForMonitor: copy only changes inside listener areas (see Pump)
    std::vector<RECT> stale_;                  // screen areas changed but not copied (nobody watched them)
    std::atomic<bool> needFull_{false}; ULONGLONG needFullSince_ = 0;  // a listener looks at a stale area: copy everything next frame
    // Windows excluded from capture (docky's Dock, menus, panels, in any process): Windows reports their redraws as changed
    // regions although the image behind them is unchanged. A change entirely inside one is "self" (checked with a GPU
    // comparison by the consumer); any other change is real and published at once (comparing every change added
    // ~9 ms of latency to the Dock's glass, measured 2026-11-13).
    std::vector<RECT> excluded_; ULONGLONG excludedAt_ = 0;
    void RefreshExcluded() {
        std::vector<RECT> found;
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
            DWORD affinity = WDA_NONE;
            if (!GetWindowDisplayAffinity(w, &affinity) || affinity == WDA_NONE) return TRUE;
            RECT r{}; if (GetWindowRect(w, &r) && !IsRectEmpty(&r)) reinterpret_cast<std::vector<RECT>*>(l)->push_back(r);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        std::lock_guard<std::mutex> lock(mutex_);
        // A window covering (nearly) the whole monitor, such as the Dock's Genie overlay (shown all the time, transparent,
        // drawing only while a window minimizes), contains every change, so "inside an excluded window" says nothing
        // about it; counting it sent every real change through the consumers' GPU comparison, about 9 ms (2 refreshes at
        // 240 Hz) before publication (measured 2026-11-20). Both paths; DOCKY_KEEP_FULLSCREEN_EXCLUDED=1 restores the old
        // classification (A/B test, fallback).
        static const bool keepOld = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"DOCKY_KEEP_FULLSCREEN_EXCLUDED", v, 4) && v[0] == L'1'; }();
        if (!keepOld) {
            const long long screen = static_cast<long long>(outputRect_.right - outputRect_.left) * (outputRect_.bottom - outputRect_.top);
            found.erase(std::remove_if(found.begin(), found.end(), [&](const RECT& r) {
                RECT in{}; if (!IntersectRect(&in, &r, &outputRect_)) return false;
                return screen > 0 && static_cast<long long>(in.right - in.left) * (in.bottom - in.top) * 10 >= screen * 9;
            }), found.end());
        }
        excluded_.swap(found); excludedAt_ = GetTickCount64();
    }
    SharedDesktop() { docky::capture_visible::Watch(); }  // screenshots: glass windows shown, frames held (capture_visible.h)
    bool Fail(HRESULT why) {
        if (!failed_) DLOG(L"desk glass: desktop duplication unavailable (0x%08X); GDI screen copy", static_cast<unsigned>(why));
        failed_ = true; failedAt_ = GetTickCount64(); Stop();
        return false;
    }
    void Stop() { dup_.Reset(); valid_ = false; }
    static float SdrWhite(const wchar_t* gdiName) {
        UINT32 paths = 0, modes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 2.5f;
        std::vector<DISPLAYCONFIG_PATH_INFO> p(paths); std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return 2.5f;
        for (UINT32 i = 0; i < paths; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{}; source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; source.header.size = sizeof(source);
            source.header.adapterId = p[i].sourceInfo.adapterId; source.header.id = p[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiName) != 0) continue;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 advanced{};
            advanced.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(advanced), p[i].targetInfo.adapterId, p[i].targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&advanced.header) == ERROR_SUCCESS &&
                advanced.activeColorMode != DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR) return 1.f;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{}; white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL; white.header.size = sizeof(white);
            white.header.adapterId = p[i].targetInfo.adapterId; white.header.id = p[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) return white.SDRWhiteLevel / 1000.f;
        }
        return 2.5f;
    }
    // Output-relative rectangles that changed (dirty rects plus move destinations); false when unknown.
    bool Changes(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<RECT>& out) {
        if (!info.TotalMetadataBufferSize) return info.LastPresentTime.QuadPart == 0;  // pointer-only frame: nothing changed
        meta_.resize(info.TotalMetadataBufferSize); UINT used = 0;
        if (FAILED(dup_->GetFrameMoveRects(static_cast<UINT>(meta_.size()), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data()), &used))) return false;
        const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
        for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) out.push_back(moves[i].DestinationRect);
        if (FAILED(dup_->GetFrameDirtyRects(static_cast<UINT>(meta_.size()), reinterpret_cast<RECT*>(meta_.data()), &used))) return false;
        const auto* dirty = reinterpret_cast<const RECT*>(meta_.data());
        for (UINT i = 0; i < used / sizeof(RECT); ++i) out.push_back(dirty[i]);
        return true;
    }
    bool Copy(ID3D11Texture2D* tex, const std::vector<RECT>& rects) {
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        const bool hdr = td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!hdr && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) { DLOG(L"desk glass: duplication format %d unsupported", static_cast<int>(td.Format)); Fail(E_NOTIMPL); return false; }
        const auto format = hdr && listenersOnly_ && !floatSharingFailed_ ?
            DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_TEXTURE2D_DESC have{}; if (image_) image_->GetDesc(&have);
        if (!image_ || have.Width != td.Width || have.Height != td.Height || have.Format != format) {
            image_.Reset(); rtv_.Reset(); shared_ = nullptr;
            D3D11_TEXTURE2D_DESC nd{}; nd.Width = td.Width; nd.Height = td.Height; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = format;
            nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; nd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            Microsoft::WRL::ComPtr<IDXGIResource> r;
            if (FAILED(device_->CreateTexture2D(&nd, nullptr, &image_)) ||
                FAILED(image_.As(&r)) || FAILED(r->GetSharedHandle(&shared_))) {
                image_.Reset();
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    floatSharingFailed_ = true;
                    return Copy(tex, rects);
                }
                return false;
            }
            ++generation_;
            DLOG(L"desk glass colour: capture=%d shared=%d", static_cast<int>(td.Format), static_cast<int>(format));
            if (rects.size() != 1 || rects[0].left != 0 || rects[0].top != 0 ||
                rects[0].right != static_cast<LONG>(td.Width) || rects[0].bottom != static_cast<LONG>(td.Height)) {
                const RECT all{0, 0, static_cast<LONG>(td.Width), static_cast<LONG>(td.Height)};
                return Copy(tex, std::vector<RECT>{all});
            }
        }
        if (!hdr) {
            for (const RECT& r : rects) {
                const D3D11_BOX box{static_cast<UINT>((std::max)(0L, r.left)), static_cast<UINT>((std::max)(0L, r.top)), 0,
                                    static_cast<UINT>((std::min)(static_cast<LONG>(td.Width), r.right)), static_cast<UINT>((std::min)(static_cast<LONG>(td.Height), r.bottom)), 1};
                if (box.right > box.left && box.bottom > box.top) ctx_->CopySubresourceRegion(image_.Get(), 0, box.left, box.top, 0, tex, 0, &box);
            }
        } else if (!Convert(tex, td, rects)) return false;
        ctx_->Flush();  // consumers on other devices read the shared image
        return true;
    }
    // Normalize to the SDR glass material's white level. Per-monitor GPU
    // consumers keep FP16 precision. The legacy shared desktop uses BGRA8.
    bool Convert(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& td, const std::vector<RECT>& rects) {
        if (!vs_ && !BuildShaders()) { Fail(E_FAIL); return false; }
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device_->CreateShaderResourceView(tex, nullptr, &srv))) { DLOG(L"desk glass: FP16 frame not readable"); Fail(E_ACCESSDENIED); return false; }
        if (!rtv_ && FAILED(device_->CreateRenderTargetView(image_.Get(), nullptr, &rtv_))) return false;
        D3D11_RENDER_TARGET_VIEW_DESC target{};
        rtv_->GetDesc(&target);
        const float constants[4] = {1.0f / (sdrWhite_ > 0.1f ? sdrWhite_ : 2.5f),
            target.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? 1.f : 0.f, 0, 0};
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
        std::memcpy(map.pData, constants, sizeof(constants)); ctx_->Unmap(cb_.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1};
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx_->RSSetViewports(1, &vp); ctx_->RSSetState(scissor_.Get());
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0); ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, srv.GetAddressOf()); ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        for (size_t i = 0; i < rects.size(); i += D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
            const UINT n = static_cast<UINT>((std::min)(rects.size() - i, static_cast<size_t>(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)));
            for (UINT k = 0; k < n; ++k) { ctx_->RSSetScissorRects(1, &rects[i + k]); ctx_->Draw(3, 0); }  // one scissor per draw
        }
        ID3D11ShaderResourceView* none = nullptr; ctx_->PSSetShaderResources(0, 1, &none);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->RSSetState(nullptr);  // owner mode shares this context with the renderer: leave no scissor state behind
        return true;
    }
    bool BuildShaders() {
        static const char kSource[] =
            "cbuffer C : register(b0) { float invWhite; float dither; float2 pad; };\n"
            "Texture2D<float4> src : register(t0);\n"
            "float4 VS(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
            "float3 Srgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }\n"
            "float Threshold(float2 pos) { uint2 p = uint2(pos) & 7u; uint x = p.x ^ p.y, y = p.y; uint k = ((x & 1u) << 5) | ((y & 1u) << 4) | ((x & 2u) << 2) | ((y & 2u) << 1) | ((x & 4u) >> 1) | ((y & 4u) >> 2); return ((float(k) + 0.5) / 64.0 - 0.5) / 255.0; }\n"
            "float4 PS(float4 pos : SV_Position) : SV_Target { float3 c = Srgb(saturate(src.Load(int3(pos.xy, 0)).rgb * invWhite)); if (dither > 0) c = floor(saturate(c + Threshold(pos.xy)) * 255.0 + 0.5) / 255.0; return float4(c, 1); }\n";
        static const pD3DCompile compile = [] {
            HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
            return m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
        }();
        if (!compile) { DLOG(L"desk glass: d3dcompiler_47 unavailable; no HDR conversion"); return false; }
        Microsoft::WRL::ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(compile(kSource, sizeof(kSource) - 1, "hdr", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kSource, sizeof(kSource) - 1, "hdr", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err))) { DLOG(L"desk glass: HDR shader compile failed"); return false; }
        if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) return false;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) return false;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
        return SUCCEEDED(device_->CreateRasterizerState(&rd, &scissor_));
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> image_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> scissor_;
    HANDLE shared_ = nullptr;
    std::vector<BYTE> meta_;
    std::vector<Change> history_;
    RECT outputRect_{};
    float sdrWhite_ = 2.5f;
    std::wstring outputName_;
    ULONGLONG nextWhiteCheck_ = 0;
    bool floatSharingFailed_ = false;
    std::atomic<bool> failed_{false}, valid_{false};
    bool retry_ = false;
    ULONGLONG failedAt_ = 0;
    int users_ = 0;
    RECT wanted_{};
    std::map<const void*, RECT> own_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, changed_, idle_;
    std::thread thread_;
    Microsoft::WRL::ComPtr<ID3D11Device> ownerDevice_;  // owner mode: the render device the duplication lives on
    DWORD ownerThread_ = 0;
    std::atomic<bool> owner_{false};
    bool pumping_ = false;                             // background thread inside Pump (under mutex_)
    bool handover_ = false;                            // SetOwner waiting: the background thread takes no new frame
    std::atomic<std::int64_t> lastPresentQpc_{0};
public:
    // Desktop frames taken / frames Windows composed meanwhile (AccumulatedFrames): their ratio shows skipped frames.
    std::atomic<std::uint64_t> acquired_{0}, accumulated_{0};
private:
    ULONGLONG idleSince_ = 0;  // when the last consumer stopped (0: in use)
    unsigned statFrames_ = 0; double statMs_ = 0, lastWaitMs_ = 0; ULONGLONG statSince_ = 0;  // debug statistics
public:
    // Consumers in use; the duplication is released when the last one stops (no panel or menu open).
    void AddUser() { std::lock_guard<std::mutex> lock(mutex_); ++users_; wake_.notify_all(); }
    void RemoveUser() { std::lock_guard<std::mutex> lock(mutex_); if (users_ > 0) --users_; wake_.notify_all(); }
private:
    std::atomic<unsigned> generation_{0};
    std::atomic<std::uint64_t> serial_{0};
    std::atomic<std::int64_t> frameQpc_{0};
};

// One consumer: the shared desktop image opened on this consumer's device.
class DesktopDuplication {
public:
    ~DesktopDuplication() { Stop(); }
    bool Start(ID3D11Device* device, const RECT& area) {
        auto& d = SharedDesktop::Get();
        if (!user_) { user_ = true; d.AddUser(); }
        if (!device || !d.Want(area)) { view_.Reset(); return false; }
        if (device != device_.Get()) { device_ = device; view_.Reset(); }
        return true;
    }
    void Stop() { view_.Reset(); seen_ = 0; if (user_) { user_ = false; SharedDesktop::Get().SetOwn(this, RECT{}); SharedDesktop::Get().RemoveUser(); } }
    // This consumer's own window (its redraws are not changes behind it).
    void SetOwnWindow(const RECT& window) { SharedDesktop::Get().SetOwn(this, window); }
    void Reset() { Stop(); }
    bool Active() const { return SharedDesktop::Get().Active(); }
    bool Failed() const { return !SharedDesktop::Get().Active(); }
    bool Covers(const RECT& r) const { const RECT o = SharedDesktop::Get().OutputRect(); RECT in{}; IntersectRect(&in, &r, &o); return Active() && EqualRect(&in, &r); }
    bool Valid() const { return SharedDesktop::Get().Valid() && Open(); }
    // True when the desktop changed inside `area` since this consumer last looked.
    bool Poll(const RECT& area, UINT timeoutMs) {
        auto& d = SharedDesktop::Get();
        if (!d.Valid() && timeoutMs) d.WaitValid(timeoutMs);  // frames arrive on SharedDesktop's thread
        if (!d.Valid() || !Open()) return false;
        const bool changed = d.Changed(area, seen_);
        seen_ = d.Serial();
        return changed;
    }
    bool Frame(dock::BackdropFrame& out) const {
        auto& d = SharedDesktop::Get();
        if (!d.Valid() || !Open()) return false;
        out.texture = view_.Get(); out.screenRect = d.OutputRect(); out.frameId = d.Serial() * 4096 + generation_; out.captureTimeSeconds = 0;
        return true;
    }

private:
    bool Open() const {
        auto& d = SharedDesktop::Get();
        if (view_ && generation_ == d.Generation()) return true;
        view_.Reset();
        if (!device_ || !d.SharedHandle()) return false;
        if (FAILED(device_->OpenSharedResource(d.SharedHandle(), IID_PPV_ARGS(&view_)))) return false;
        generation_ = d.Generation();
        return true;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> view_;
    mutable unsigned generation_ = 0;
    std::uint64_t seen_ = 0;
    bool user_ = false;
};

}  // namespace docky::desk
