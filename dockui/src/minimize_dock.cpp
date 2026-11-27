#include "minimize_dock.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "capture_visible.h"
#include "genie_gpu.h"
#include "log.h"

namespace dock::minimize {
namespace {

constexpr int kBands = 96;                 // Genie bands (thumbnails) while animating; 96 keeps the funnel edge smooth
constexpr double kMinimizeSeconds = 0.52, kRestoreSeconds = 0.46;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kGpuDone = WM_APP + 1;      // genie_gpu: wParam MAKEWPARAM(direction + 1, ok), lParam window
constexpr wchar_t kClass[] = L"DockyMinimizeOverlay";

struct Entry {
    HWND window = nullptr;
    bool thumbnailVisible = false;
    HTHUMBNAIL tile = nullptr;             // live thumbnail shown in the Dock tile
    std::vector<HTHUMBNAIL> bands;         // Genie bands while animating
    SIZE source{};                         // thumbnail source size
    RECT normal{};                         // restored window rectangle (screen)
    RECT tileRect{};                       // Dock tile (screen); empty until the Dock has laid it out
    int direction = 0;                     // +1 minimizing, -1 restoring, 0 idle
    bool pendingGenie = false;             // minimize Genie waits for the first tile rectangle
    bool gpu = false;                      // animating on the GPU (genie_gpu); otherwise thumbnail bands
    LARGE_INTEGER start{};
    ULONGLONG addedAt=0;
};

HWND g_dock = nullptr, g_overlay = nullptr;
HWINEVENTHOOK g_hook = nullptr, g_foregroundHook = nullptr;
HHOOK g_mouseHook = nullptr;
HWND g_active = nullptr;                  // the eligible foreground window kept snapshotted (Genie on)
constexpr UINT_PTR kSnapshotTimer = 2;    // refresh its snapshot every few seconds while it stays in front
constexpr UINT kPress = WM_APP + 2;       // left button pressed at (wParam x, lParam y)
std::function<void(HWND, bool)> g_changed;
std::map<HWND, Entry> g_entries;
bool g_genie = false;
bool g_gameMode = false, g_covered = false;
RECT g_overlayRect{};

double Seconds(const LARGE_INTEGER& from) {
    LARGE_INTEGER now{}, f{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    return static_cast<double>(now.QuadPart - from.QuadPart) / static_cast<double>(f.QuadPart);
}
bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
RECT ToOverlay(const RECT& s) { return RECT{s.left - g_overlayRect.left, s.top - g_overlayRect.top, s.right - g_overlayRect.left, s.bottom - g_overlayRect.top}; }

// Restored rectangle in screen coordinates (SetWindowPlacement / GetWindowPlacement use workspace coordinates).
RECT NormalRect(HWND w) {
    WINDOWPLACEMENT wp{sizeof(wp)}; GetWindowPlacement(w, &wp);
    MONITORINFO mi{sizeof(mi)}; const POINT origin{0, 0};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = wp.rcNormalPosition;
    OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    return r;
}

bool Eligible(HWND w) {
    if (!w || GetAncestor(w, GA_ROOT) != w || !IsWindow(w)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    const LONG style = GetWindowLongW(w, GWL_STYLE), ex = GetWindowLongW(w, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || !(style & WS_CAPTION) || !(style & WS_MINIMIZEBOX)) return false;
    if (GetWindow(w, GW_OWNER)) return false;
    return GetWindowTextLengthW(w) > 0;
}

bool Cloaked(HWND w) { DWORD c = 0; return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0; }
// Windows that get a Dock tile when minimized: the taskbar's rule (top-level, visible, not a tool window, unowned unless
// WS_EX_APPWINDOW). Eligible (caption with a minimize button) only decides about Genie snapshots: requiring it here left
// windows without a standard caption (frameless apps minimized with Win+Down) with no tile, and with the taskbar hidden
// they could not be found again.
bool Dockable(HWND w) {
    if (!w || GetAncestor(w, GA_ROOT) != w || !IsWindow(w) || !IsWindowVisible(w)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & WS_EX_APPWINDOW) return true;
    return !(ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !GetWindow(w, GW_OWNER);
}

bool Show(HTHUMBNAIL t, const RECT& dest, const RECT* source, BYTE opacity, bool visible) {
    if (!t) return false;
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | (source ? DWM_TNP_RECTSOURCE : 0);
    p.rcDestination = ToOverlay(dest); if (source) p.rcSource = *source;
    p.fVisible = visible && !Empty(dest); p.opacity = opacity;
    return SUCCEEDED(DwmUpdateThumbnailProperties(t, &p));
}

RECT FitTile(const Entry& e) { return gpu::FitTile(e.tileRect, e.source); }

// The overlay covers the whole monitor only while a Genie plays. Otherwise it is only as large as the Dock's window
// (where the tiles are): a monitor-sized always-on-top window, even click-through, is what the Snipping Tool's window
// mode finds under the pointer everywhere, so it could only capture the full screen while the Dock ran.
RECT IdleOverlayRect() {
    RECT r{};
    if (g_dock && GetWindowRect(g_dock, &r) && !Empty(r)) return r;
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(g_dock ? g_dock : g_overlay, MONITOR_DEFAULTTOPRIMARY), &mi);
    return RECT{mi.rcMonitor.right - 1, mi.rcMonitor.bottom - 1, mi.rcMonitor.right, mi.rcMonitor.bottom};
}
void FitOverlay(bool full) {
    if (!g_overlay) return;
    RECT want{};
    if (full) {
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(g_dock ? g_dock : g_overlay, MONITOR_DEFAULTTOPRIMARY), &mi);
        want = mi.rcMonitor;
    } else {
        for (auto& [w, e] : g_entries) if (e.direction && !e.gpu) return;  // thumbnail-band Genie still playing
        want = IdleOverlayRect();
    }
    if (EqualRect(&want, &g_overlayRect)) return;
    g_overlayRect = want;
    SetWindowPos(g_overlay, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
    // Thumbnail destinations are relative to the overlay: put the resting tiles back where they were on screen.
    for (auto& [w, e] : g_entries) if (!e.direction && e.thumbnailVisible) Show(e.tile, FitTile(e), nullptr, 255, true);
}

// Fallback Genie (no GPU capture): horizontal bands of the window, each a DWM thumbnail placed along the funnel.
void Genie(Entry& e, double t) {
    const RECT from = e.normal, to = FitTile(e);
    for (int i = 0; i < kBands && i < static_cast<int>(e.bands.size()); ++i) {
        const double u0 = static_cast<double>(i) / kBands, u1 = static_cast<double>(i + 1) / kBands;
        const gpu::Row top = gpu::GenieRow(from, to, t, u0), mid = gpu::GenieRow(from, to, t, (u0 + u1) * 0.5);
        const double y0 = top.y, y1 = std::max(gpu::GenieRow(from, to, t, u1).y, y0 + 1.0);
        const RECT src{0, static_cast<LONG>(std::floor(u0 * e.source.cy)), e.source.cx, static_cast<LONG>(std::ceil(u1 * e.source.cy))};
        const RECT dst{static_cast<LONG>(std::lround(mid.left)), static_cast<LONG>(std::lround(y0)), static_cast<LONG>(std::lround(mid.right)), static_cast<LONG>(std::lround(y1)) + 1};
        Show(e.bands[i], dst, &src, 255, true);
    }
}

void DropBands(Entry& e) { for (HTHUMBNAIL b : e.bands) DwmUnregisterThumbnail(b); e.bands.clear(); }
void Remove(HWND w, bool notify) {
    auto it = g_entries.find(w); if (it == g_entries.end()) return;
    DropBands(it->second); if (it->second.tile) DwmUnregisterThumbnail(it->second.tile);
    g_entries.erase(it);
    gpu::Forget(w);
    if (notify && g_changed) g_changed(w, false);
}
bool StartBands(Entry& e) {
    DropBands(e);
    for (int i = 0; i < kBands; ++i) { HTHUMBNAIL b = nullptr; if (FAILED(DwmRegisterThumbnail(g_overlay, e.window, &b))) { DropBands(e); return false; } e.bands.push_back(b); }
    QueryPerformanceCounter(&e.start);
    SetTimer(g_overlay, kTimer, 7, nullptr);
    return true;
}
void Raise() { if (g_overlay) SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }

// An animation reached its end: a minimized window settles into its tile; a restored one comes back.
void Finish(HWND w) {
    auto it = g_entries.find(w); if (it == g_entries.end()) return;
    Entry& e = it->second; DropBands(e);
    const int direction = e.direction; e.direction = 0; e.gpu = false;
    if (direction > 0) { e.thumbnailVisible=Show(e.tile, FitTile(e), nullptr, 255, true); Raise(); FitOverlay(false); return; }
    ShowWindow(w, SW_RESTORE); SetForegroundWindow(w);
    DLOG(L"minimize: restored from the Dock");
    Remove(w, true);
    FitOverlay(false);
}

// Starts a Genie: on the GPU when possible (genie_gpu reports back with kGpuDone), else with thumbnail bands.
bool Begin(Entry& e, int direction) {  // the GPU Genie draws in its own window (genie_gpu)
    e.direction = direction; e.gpu = true;
    Show(e.tile, e.tileRect, nullptr, 0, false);
    gpu::SetTile(e.window, e.tileRect);
    gpu::Animate(e.window, e.normal, direction, direction > 0 ? kMinimizeSeconds : kRestoreSeconds);
    return true;
}
bool BeginBands(Entry& e) {
    FitOverlay(true);
    e.gpu = false;
    if (!StartBands(e)) return false;
    Raise(); Genie(e, e.direction > 0 ? 0.0 : 1.0);
    return true;
}

void Tick() {
    bool any = false;
    std::vector<HWND> done;
    for (auto& [w, e] : g_entries) {
        if (!e.direction || e.gpu) continue;
        const double total = e.direction > 0 ? kMinimizeSeconds : kRestoreSeconds, x = std::clamp(Seconds(e.start) / total, 0.0, 1.0);
        Genie(e, e.direction > 0 ? x : 1.0 - x);
        if (x < 1.0) any = true; else done.push_back(w);
    }
    for (HWND w : done) Finish(w);
    if (!any) KillTimer(g_overlay, kTimer);
}

// A press on a window's minimize caption button: snapshot it before the button-up minimizes it.
void OnPress(POINT pt) {
    HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (!g_genie || !root || !Eligible(root)) return;
    RECT frame{}, visible{}; if (!GetWindowRect(root, &frame)) return;
    if (FAILED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) visible = frame;
    const float k = GetDpiForWindow(root) / 96.f;
    if (pt.y < visible.top || pt.y >= visible.top + static_cast<LONG>(48 * k)) return;
    DWORD_PTR ht = HTNOWHERE; SendMessageTimeoutW(root, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG, 30, &ht);
    const LONG bw = static_cast<LONG>(46 * k);
    const bool third = pt.x >= visible.right - 3 * bw && pt.x < visible.right - 2 * bw;  // standard caption: min | max | close
    if (ht == HTMINBUTTON || (third && ht != HTCLOSE && ht != HTMAXBUTTON)) gpu::Snapshot(root);
}
LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_LBUTTONDOWN && g_overlay) {  // only posts: never slows the pointer
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
        PostMessageW(g_overlay, kPress, static_cast<WPARAM>(m->pt.x), static_cast<LPARAM>(m->pt.y));
    }
    return CallNextHookEx(nullptr, code, w, l);
}
bool CoversItsMonitor(HWND w) {
    MONITORINFO mi{sizeof(mi)}; RECT r{};
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(w, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND w, LONG object, LONG child, DWORD, DWORD) {
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !g_overlay) return;
    // Full-screen windows (games) are never snapshotted: a capture of a game frame every 3 s costs the game frames.
    if (g_gameMode || !g_genie || !w || !Eligible(w) || CoversItsMonitor(w)) { g_active = nullptr; KillTimer(g_overlay, kSnapshotTimer); return; }
    g_active = w;
    gpu::Snapshot(w);
    SetTimer(g_overlay, kSnapshotTimer, 3000, nullptr);
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == kTimer) { Tick(); return 0; }
    if (m == WM_TIMER && w == kSnapshotTimer) {
        if (!g_gameMode && g_genie && g_active && IsWindow(g_active) && !IsIconic(g_active) && GetForegroundWindow() == g_active && !CoversItsMonitor(g_active)) gpu::Snapshot(g_active);
        else { g_active = nullptr; KillTimer(h, kSnapshotTimer); }
        return 0;
    }
    if (m == kPress) { OnPress(POINT{static_cast<LONG>(static_cast<int>(w)), static_cast<LONG>(static_cast<int>(l))}); return 0; }
    if (m == kGpuDone) {
        const HWND window = reinterpret_cast<HWND>(l); const int direction = static_cast<int>(LOWORD(w)) - 1; const bool ok = HIWORD(w) != 0;
        auto it = g_entries.find(window);
        if (it == g_entries.end() || !it->second.gpu || it->second.direction != direction) return 0;
        if (ok || !BeginBands(it->second)) Finish(window);
        return 0;
    }
    if (m == WM_ERASEBKGND) { RECT r{}; GetClientRect(h, &r); FillRect(reinterpret_cast<HDC>(w), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); return 1; }
    if (m == WM_PAINT) { PAINTSTRUCT ps{}; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    return DefWindowProcW(h, m, w, l);
}

std::unordered_map<HWND, ULONGLONG> g_stageMinimizes;  // 0 while parked, then a restore deadline

// A minimized window gets its Dock tile (`animate`: play the Genie once the tile has a place).
void Add(HWND w, bool animate) {
    if (StageOwnsMinimize(w) || g_entries.count(w) || !Dockable(w)) return;
    if (!g_covered&&!g_gameMode&&!IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_SHOWNOACTIVATE);  // hidden in game mode
    Entry e; e.window = w; e.normal = NormalRect(w);e.addedAt=GetTickCount64();
    if(SUCCEEDED(DwmRegisterThumbnail(g_overlay,w,&e.tile)))DwmQueryThumbnailSourceSize(e.tile,&e.source);
    else DLOG(L"minimize: thumbnail unavailable, retaining Dock tile");
    BOOL animations = FALSE; ANIMATIONINFO ai{sizeof(ai)};
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0)) animations = ai.iMinAnimate != 0;
    e.pendingGenie = animate && e.tile && !g_covered && !g_gameMode && g_genie && !animations;
    g_entries[w] = e;
    wchar_t cls[64]{}; GetClassNameW(w, cls, 64);
    DLOG(L"minimize: window to the Dock class=%ls genie=%d%ls", cls, e.pendingGenie ? 1 : 0, animate ? L"" : L" (already minimized at start)");
    if (g_changed) g_changed(w, true);
    FitOverlay(false);
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG object, LONG child, DWORD, DWORD) {
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !g_overlay) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        Add(w, true);
    } else if (event == EVENT_SYSTEM_MINIMIZEEND) {
        g_stageMinimizes.erase(w);
        auto it = g_entries.find(w);
        if (it != g_entries.end() && it->second.direction >= 0) { DLOG(L"minimize: restored elsewhere"); Remove(w, true); }  // taskbar, Alt+Tab
    }
}

}  // namespace

bool Init(HWND dock, std::function<void(HWND, bool)> changed) {
    g_dock = dock; g_changed = std::move(changed);
    WNDCLASSW wc{}; wc.lpfnWndProc = OverlayProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    g_overlayRect = IdleOverlayRect();
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP,
                                g_overlayRect.left, g_overlayRect.top, g_overlayRect.right - g_overlayRect.left, g_overlayRect.bottom - g_overlayRect.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_overlay) return false;
    SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);  // black = transparent and click-through
    if (!GetEnvironmentVariableW(L"LIQUID_MINIMIZE_CAPTURABLE", nullptr, 0))  // test runs: keep screenshots possible
        SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);     // never inside a glass backdrop
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    gpu::Start(g_overlay, kGpuDone);
    g_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    docky::capture_visible::Watch(); docky::capture_visible::Trigger::Install();
    // Windows minimized before the Dock started (a Dock restart lost their tiles; with the taskbar hidden they were gone).
    EnumWindows([](HWND w, LPARAM) -> BOOL { if (IsIconic(w) && !Cloaked(w)) Add(w, false); return TRUE; }, 0);
    return g_hook != nullptr;
}

void Shutdown() {
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    if (g_foregroundHook) { UnhookWinEvent(g_foregroundHook); g_foregroundHook = nullptr; }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    docky::capture_visible::Trigger::Remove();
    gpu::Stop();
    std::vector<HWND> all; for (auto& [w, e] : g_entries) all.push_back(w);
    for (HWND w : all) Remove(w, false);
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = nullptr; }
}

void SetTileRect(HWND window, const RECT& screen) {
    auto it = g_entries.find(window); if (it == g_entries.end()) return;
    Entry& e = it->second;
    const bool changed = !EqualRect(&e.tileRect, &screen);
    e.tileRect = screen;
    if (e.direction && e.gpu) gpu::SetTile(window, screen);
    if (e.pendingGenie && !Empty(screen)) {
        e.pendingGenie = false;
        if (Begin(e, 1)) return;
    }
    if (!e.direction && (changed || !e.thumbnailVisible)) {
        e.thumbnailVisible=e.source.cx>0&&e.source.cy>0&&!Empty(screen)&&Show(e.tile,FitTile(e),nullptr,255,true);
        if(e.thumbnailVisible)Raise();
    }
    FitOverlay(false);  // follows the Dock window when it is laid out again
}

bool SuppressFallbackTile(HWND window){
    const auto it=g_entries.find(window);
    // The overlay draws the tile only while it is shown (hidden in game mode and under Launchpad): otherwise the Dock
    // draws its own plate, so the tile never turns into an empty gap.
    return it!=g_entries.end()&&g_overlay&&IsWindowVisible(g_overlay)&&(it->second.thumbnailVisible||it->second.direction!=0);
}

void Restore(HWND window) {
    auto it = g_entries.find(window); if (it == g_entries.end()) { ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); return; }
    Entry& e = it->second;
    if (e.direction) return;
    BOOL animations = FALSE; ANIMATIONINFO ai{sizeof(ai)};
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0)) animations = ai.iMinAnimate != 0;
    if (g_genie && !animations && !Empty(e.tileRect) && Begin(e, -1)) return;
    ShowWindow(window, SW_RESTORE); SetForegroundWindow(window);
    Remove(window, true);
}

void StagePark(HWND window) {
    g_stageMinimizes[window] = 0;
    Remove(window, true);
}

void StageRestore(HWND window) {
    if (auto it = g_stageMinimizes.find(window); it != g_stageMinimizes.end())
        it->second = GetTickCount64() + 1200;
}

bool StageOwnsMinimize(HWND window) {
    return g_stageMinimizes.count(window) != 0;
}

void Prune() {
    const auto now = GetTickCount64();
    std::erase_if(g_stageMinimizes, [&](const auto& entry) {
        return !IsWindow(entry.first) || (entry.second && now >= entry.second);
    });
    std::vector<HWND> gone;
    for (auto& [w, e] : g_entries) {
        if (e.direction) continue;
        const wchar_t* why = !IsWindow(w) ? L"closed" : (!IsIconic(w) && GetTickCount64() - e.addedAt > 1200) ? L"no longer minimized"
                           : !IsWindowVisible(w) ? L"hidden by its app (tray)" : nullptr;
        if (why) { DLOG(L"minimize: tile removed, window %ls", why); gone.push_back(w); }
    }
    for (HWND w : gone) Remove(w, true);
    KeepAbove();
}

// The overlay must stay right above the Dock. Activating the Dock (a click) or laying it out again (SetWindowPos
// HWND_TOPMOST) puts the Dock on top of every always-on-top window, over the overlay: the minimized windows' tiles then
// vanished behind the Dock until something moved them. Placed directly above the Dock, not on top of everything, so
// menus and popups opened from the Dock stay above the tiles.
void KeepAbove() {
    if (!g_overlay || !g_dock || !IsWindowVisible(g_overlay)) return;
    bool any = false;
    for (auto& [w, e] : g_entries) if (e.thumbnailVisible || e.direction) { any = true; break; }
    if (!any) return;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 100) return;
    last = now;
    for (HWND h = GetWindow(g_dock, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) if (h == g_overlay) return;
    const HWND above = GetWindow(g_dock, GW_HWNDPREV);
    SetWindowPos(g_overlay, above ? above : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DLOG(L"minimize: tiles were under the Dock; overlay placed above it again");
}

void SetGenie(bool enabled) { g_genie = enabled; }
void SetCovered(bool covered) {
    if (covered == g_covered || !g_overlay) return;
    g_covered = covered;
    if (covered) ShowWindow(g_overlay, SW_HIDE);
    else if (!g_gameMode) { ShowWindow(g_overlay, SW_SHOWNOACTIVATE); Raise(); }
    DLOG(L"minimize: previews %ls", covered ? L"hidden under Launchpad" : L"shown");
}
void SetGameMode(bool on) {
    if (on == g_gameMode || !g_overlay) return;
    g_gameMode = on;
    if (on) {
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        docky::capture_visible::Trigger::Remove();  // no keyboard hook while a game runs
        g_active = nullptr; KillTimer(g_overlay, kSnapshotTimer);
        if (!Animating()) ShowWindow(g_overlay, SW_HIDE);
    } else {
        if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        docky::capture_visible::Trigger::Install();
        if (!g_covered) ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    }
    DLOG(L"minimize: game mode %ls", on ? L"on (mouse hook, snapshots and overlay off)" : L"off");
}
bool Animating() { for (auto& [w, e] : g_entries) if (e.direction) return true; return false; }

}  // namespace dock::minimize
