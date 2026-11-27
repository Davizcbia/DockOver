#pragma once
// Display-paced ticks for windows whose animation or backdrop update was driven by WM_TIMER. Windows timers cannot fire
// faster than about every 10 ms (USER_TIMER_MINIMUM, ~100 Hz at best, 64 Hz with the default tick), so a 240 Hz display
// showed menus, Stacks and widget glides at a fraction of its rate.
//
// A Ticker posts one message per compositor frame (DCompositionWaitForCompositorClock on Windows 11, DwmFlush before)
// to its window while it runs: 240 messages per second on a 240 Hz display, 120 on 120 Hz. At most one tick is queued:
// the window calls Ack() when it handles the message, so a slow frame drops ticks instead of piling them up. A stopped
// ticker's thread sleeps on an event and costs nothing.
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

namespace docky::vblank {

// ---- Frame-rate caps (Settings > General > Frame rate) -------------------------------------------------------------
// 0 = unlimited (the display's refresh rate). Animation: every animated surface. Capture: how often live backdrops
// (Windows Graphics Capture, desktop duplication) take a new picture of the desktop.
inline std::atomic<int>& AnimationCapRef() { static std::atomic<int> v{0}; return v; }
inline std::atomic<int>& CaptureCapRef() { static std::atomic<int> v{0}; return v; }
inline void SetCaps(int animationFps, int captureFps) { AnimationCapRef() = animationFps; CaptureCapRef() = captureFps; }
inline int AnimationCap() { return AnimationCapRef().load(std::memory_order_relaxed); }
inline int CaptureCap() { return CaptureCapRef().load(std::memory_order_relaxed); }
inline double Seconds() {
    static const double f = [] { LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); return static_cast<double>(q.QuadPart); }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return static_cast<double>(c.QuadPart) / f;
}
inline ULONGLONG AnimationMillis() { return static_cast<ULONGLONG>(Seconds() * 1000.0); }
// The display frame period in seconds, measured by the tickers (1/240 on a 240 Hz display).
inline std::atomic<double>& FramePeriod() {
    static std::atomic<double> p{[] {  // starts at the display's refresh period (DWM), refined by the tickers
        DWM_TIMING_INFO t{}; t.cbSize = sizeof(t);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &t)) && t.rateRefresh.uiNumerator && t.rateRefresh.uiDenominator)
            return static_cast<double>(t.rateRefresh.uiDenominator) / t.rateRefresh.uiNumerator;
        return 1.0 / 60.0;
    }()};
    return p;
}
// Minimum time between two frames under a cap, aligned to display frames: a quarter display frame of slack, so 60 fps on a
// 240 Hz display is exactly every 4th frame (no 4-5-4 judder). 0 when unlimited.
inline double CapInterval(int fps) { return fps > 0 ? (std::max)(0.0, 1.0 / fps - 0.25 * FramePeriod().load()) : 0.0; }

// A render loop's gate for the animation cap: Remaining() is how long the next frame must still wait (0 = now).
class Gate {
public:
    double Remaining() const { const double i = CapInterval(AnimationCap()); if (i <= 0 || last_ <= 0) return 0; const double left = i - (Seconds() - last_); return left > 0 ? left : 0; }
    void Mark() { last_ = Seconds(); }
private:
    double last_ = 0;
};
// High-resolution one-shot timer for throttled loops (message waits have 1-16 ms granularity).
inline HANDLE CreatePaceTimer() { return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }
inline void Arm(HANDLE timer, double seconds) { LARGE_INTEGER due{}; due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7) - 1; SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); }
// Waits `seconds` precisely on this thread (no messages): for worker threads.
inline void Pause(double seconds) {
    if (seconds <= 0) return;
    thread_local HANDLE timer = CreatePaceTimer();
    if (timer) { Arm(timer, seconds); WaitForSingleObject(timer, static_cast<DWORD>(seconds * 1000) + 50); } else Sleep(static_cast<DWORD>(seconds * 1000));
}

// One compositor frame. Exposed for loops that pace themselves on their own thread.
inline void WaitFrame(HWND target = nullptr) {
    // The compositor clock can follow a different output on mixed-refresh desktops.
    // Keep one output per pacing thread and reopen it when the window moves or the driver resets.
    if (target && GetSystemMetrics(SM_CMONITORS) > 1) {
        using Microsoft::WRL::ComPtr;
        thread_local HMONITOR previous{};
        thread_local ComPtr<IDXGIOutput> output;
        thread_local double retry = 0;
        thread_local unsigned immediateReturns = 0;
        const auto monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
        if (monitor != previous || (!output && Seconds() >= retry)) {
            previous = monitor; output.Reset(); immediateReturns = 0; retry = Seconds() + 2;
            using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
            static const auto create = [] {
                auto dll = LoadLibraryW(L"dxgi.dll");
                return dll ? reinterpret_cast<CreateFactory>(GetProcAddress(dll,"CreateDXGIFactory1")) : nullptr;
            }();
            ComPtr<IDXGIFactory1> factory;
            if (monitor && create && SUCCEEDED(create(IID_PPV_ARGS(&factory)))) {
                for (UINT i=0; !output; ++i) {
                    ComPtr<IDXGIAdapter1> adapter;
                    if (FAILED(factory->EnumAdapters1(i,&adapter))) break;
                    for (UINT j=0;;++j) {
                        ComPtr<IDXGIOutput> candidate;
                        if (FAILED(adapter->EnumOutputs(j,&candidate))) break;
                        DXGI_OUTPUT_DESC desc{};
                        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor==monitor) {
                            output=candidate; break;
                        }
                    }
                }
            }
        }
        if (output) {
            const double before = Seconds();
            if (SUCCEEDED(output->WaitForVBlank())) {
                if (Seconds()-before > 0.00005) { immediateReturns=0; return; }
                if (++immediateReturns < 3) return;
            }
            output.Reset(); retry=Seconds()+2;
        }
    }
    using WaitClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    static const WaitClock wait = [] {
        HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
        return dcomp ? reinterpret_cast<WaitClock>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")) : nullptr;
    }();
    if (wait) { if (wait(0, nullptr, 100) != WAIT_FAILED) return; }
    if (FAILED(DwmFlush())) Sleep(4);  // composition off (remote session): a short, bounded wait
}

class Ticker {
public:
    Ticker() = default;
    Ticker(const Ticker&) = delete;
    Ticker& operator=(const Ticker&) = delete;
    ~Ticker() {
        if (!state_) return;
        state_->quit = true; state_->run = false;
        SetEvent(state_->wake);
    }
    // Posts `msg` to `window` once per display frame until Stop(). Starting a running ticker only retargets it.
    void Start(HWND window, UINT msg) {
        if (!state_) {
            state_ = std::make_shared<State>();
            std::thread([s = state_] {
                double lastFrame = 0, lastPost = 0;
                while (!s->quit) {
                    if (!s->run) { WaitForSingleObject(s->wake, INFINITE); lastFrame = 0; continue; }
                    WaitFrame(s->window.load());
                    const double now = Seconds();
                    if (lastFrame > 0) { const double d = now - lastFrame; if (d > 0.001 && d < 0.1) FramePeriod() = FramePeriod().load() * 0.9 + d * 0.1; }
                    lastFrame = now;
                    if (now - lastPost < CapInterval(AnimationCap())) continue;  // the animation cap skips display frames
                    if (s->run && !s->quit && !s->pending.exchange(true)) {
                        lastPost = now;
                        if (!PostMessageW(s->window.load(), s->msg.load(), 0, 0)) s->pending = false;
                    }
                }
                CloseHandle(s->wake);
            }).detach();
        }
        if (state_->run && state_->window == window && state_->msg == msg) return;
        state_->window = window; state_->msg = msg;
        state_->pending = false;
        if (!state_->run.exchange(true)) SetEvent(state_->wake);
    }
    void Stop() { if (state_) state_->run = false; }
    bool Running() const { return state_ && state_->run; }
    void Ack() { if (state_) state_->pending = false; }  // call first thing when handling the tick message

private:
    struct State {
        std::atomic<bool> run{false}, quit{false}, pending{false};
        std::atomic<HWND> window{nullptr};
        std::atomic<UINT> msg{0};
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    };
    std::shared_ptr<State> state_;
};

// The display tick message every user of Ticker in this code base posts (each window handles its own).
constexpr UINT kMsgVBlank = WM_APP + 230;

}  // namespace docky::vblank
