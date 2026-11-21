#include "finder_fit.h"
#include "liquid_build_id.h"
static volatile const char liquidBuildMarker[] = LIQUID_BUILD_MARKER;
// Liquid Dock V0.2 - a Win32 / D3D11 / DirectComposition / Direct2D dock with real Windows icons,
// magnification, launching and pinning. Capture-based glass is intentionally out of scope (see backdrop.h).
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "backdrop.h"
#include "dock_model.h"
#include "frame_ready.h"
#include "display_geometry.h"
#include "drag_reorder.h"
#include "icon_mask.h"
#include "minimize_dock.h"
#include "downloads_stack.h"
#include "fan_layout.h"
#include "downloads_preview.h"
#include <thread>
#include <mutex>
#include <memory>
#include "fan_gpu.h"
#include "unified_menu.h"
#include "menu_surface.h"
#include "jump_list.h"
#include "log.h"
#include "renderer.h"
#include "shell_items.h"
#include "custom_icons.h"
#include "workshop.h"
#include "icon_editor.h"
#include <shobjidl.h>
#include "backdrop_sim.h"
#include "backdrop_wgc.h"
#include "desktop/duplication.h"
#include "game_compat.h"
#include <roapi.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "glass_params.h"
#include "glass_studio.h"
#include "launchpad.h"
#include "app_registry.h"
#include "applications_win_key.h"
#include "vblank.h"
#include "desktop_settings.h"
#include "theme_state.h"
#include "status_group.h"
#include "status_popups.h"
#include "status_hosts.h"
#include "control_center.h"
#include "system_status.h"
#include "system_controls.h"

using namespace dock;

namespace {

constexpr wchar_t kClassName[] = L"LiquidDockV01";
constexpr UINT_PTR kTimerRunning = 1;
constexpr UINT_PTR kTimerExit = 2;
constexpr UINT_PTR kTimerLaunchpadIcon = 3;  // a Launchpad icon command, run once Launchpad has faded out
constexpr UINT_PTR kTimerLaunchpadPrewarm = 4;  // app discovery and icon cache for Launchpad, a few seconds after start
constexpr UINT_PTR kTimerApplicationsPrewarm = 58;  // the resident Applications window, started hidden after the Dock
static HWND ApplicationsWindow(HANDLE process);  // the resident Applications window of that process, if any
constexpr UINT kIconPx = 256;
constexpr UINT kDownloadsPreviewReady = WM_APP + 210;
struct DownloadsPreviewShared {std::mutex mutex;HWND window=nullptr;bool busy=false;std::wstring signature;std::vector<std::uint32_t> pixels;};

static int RecycleBinFull();  // defined below; read-only Recycle Bin state query
static HWND g_lastExternalForeground = nullptr;  // V0.16: updated by the EVENT_SYSTEM_FOREGROUND hook
// V0.16: the popup window is kept (hidden) after it closes, so only a visible popup of this process counts as open.
static bool OwnPopupVisible() {
    HWND popup = FindWindowW(L"LiquidUnifiedPopup98d2", nullptr);
    DWORD owner = 0;
    if (popup) GetWindowThreadProcessId(popup, &owner);
    return popup && owner == GetCurrentProcessId() && IsWindowVisible(popup);
}
constexpr UINT kMenuApplications=74;
constexpr UINT kMenuTaskManager=79;  // Dock menu > Task Manager (taskmgr.exe; Windows asks for elevation when its policy wants it)
constexpr UINT kMenuShowBase=1100;  // Dock menu > Show in Dock: +0 divider, +1 Downloads, +2 Recycle Bin, +10.. status cells
// The status items a user can put in the Dock, in menu order: whole-icon tiles first, then the half-size cells.
// Whole-icon tiles first (kStatusTileCount), then the half-size cells of the status group.
static const wchar_t* const kStatusMenuIds[] = {L"combo", L"analog", L"date", L"digital", L"tray", L"control", L"wifi", L"bluetooth", L"sound", L"battery", L"input", L"clock"};
static constexpr UINT kStatusTileCount = 5;
constexpr UINT kMenuNewFinder = 70, kMenuNewWindow = 71, kMenuDockySettings = 72, kMenuQuitApp = 73, kMenuJumpBase = 700;
constexpr size_t kJumpMax = 12;
enum MenuId : UINT { kMenuRename = 63, kMenuEditIcon = 62, kMenuInsertSep = 60, kMenuRemoveSep = 61, kMenuKeep = 50, kMenuOpen = 1, kMenuUnpin, kMenuLocation, kMenuReload, kMenuEditConfig, kMenuQuit, kMenuGlass, kMenuChangeIcon, kMenuResetIcon, kMenuSnapshot };

double Now() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

// A per-tile offset spring (reorder slide, post-drop settle): DIPs, rests under 0.05 DIP.
docky::motion::Spring MakeOffsetSpring() {
    docky::motion::Spring s;
    s.SetSpec(dock::dockmotion::Reorder);
    s.SetRest(0.05, 2.0);
    return s;
}

struct Options {
    std::wstring config;
    std::wstring log;
    bool diag = false;
    bool explicitDiag = false, devIconEditor = false;
    bool iconMask = true;
    bool autoHide = false;
    int iconMaskOverride = -1;
    std::wstring desktopSettingsPath;
    HANDLE firstFrameReady=nullptr;  // optional handle inherited only from the Manager
    bool wgcCompare = false;
    double exitAfter = 0.0;
    // Session overrides of the glass settings (not saved unless Glass Studio saves).
    std::wstring snapshot;     // optional one-shot diagnostic BMP
    std::wstring glassConfig;  // alternative glass.json (tests)
    std::wstring workshopRoot; // isolated workshop root for testing
    std::wstring iconConfig;   // independent custom icon mapping
    std::wstring downloadsPath, downloadsSettings;  // isolated preview/test overrides
    std::wstring material;  // liquid | smoked
    std::wstring backdrop;  // wallpaper | grid | gradient | dark | none
    int debugView = -1;
    bool continuous = false;
    bool lowLatency = false;  // --lowlat or DOCKY_LOWLAT=1: one-frame DXGI glass path (opt-in, see Frame)
    bool studio = false;
    bool applications = false;
    std::wstring launchpadVisual;
    bool launchpad = false;        // open Launchpad at start-up (tests)
    std::wstring launchpadConfig;  // alternative launchpad.json (tests)
};

Options ParseArgs() {
    Options o;
    o.desktopSettingsPath = liquid::settings::DefaultPath();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) o.config = argv[++i];
        else if (a == L"--log" && i + 1 < argc) o.log = argv[++i];
        else if (a == L"--diag") {o.diag = true;o.explicitDiag=true;}
        else if (a == L"--dev-icon-editor") o.devIconEditor = true;
        else if (a == L"--desktop-settings" && i+1<argc) o.desktopSettingsPath=argv[++i];
        else if (a == L"--mac-icon-mask" && i+1<argc) {const std::wstring v=argv[++i];if(v==L"on")o.iconMaskOverride=1;else if(v==L"off")o.iconMaskOverride=0;}
        else if (a == L"--manager-ready-handle" && i+1<argc){wchar_t* end=nullptr;const auto value=_wcstoui64(argv[++i],&end,10);
            if(end&&!*end&&value && value!=~0ull)o.firstFrameReady=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));}
        else if (a == L"--wgc-compare") o.wgcCompare = true;
        else if (a == L"--exit-after" && i + 1 < argc) o.exitAfter = _wtof(argv[++i]);
        else if (a == L"--snapshot" && i + 1 < argc) o.snapshot = argv[++i];
        else if (a == L"--glass-config" && i + 1 < argc) o.glassConfig = argv[++i];
        else if (a == L"--icon-config" && i + 1 < argc) o.iconConfig = argv[++i];
        else if (a == L"--workshop-root" && i + 1 < argc) o.workshopRoot = argv[++i];
        else if (a == L"--downloads-path" && i + 1 < argc) o.downloadsPath = argv[++i];
        else if (a == L"--downloads-settings" && i + 1 < argc) o.downloadsSettings = argv[++i];
        else if (a == L"--material" && i + 1 < argc) o.material = argv[++i];
        else if (a == L"--backdrop" && i + 1 < argc) o.backdrop = argv[++i];
        else if (a == L"--debug-view" && i + 1 < argc) o.debugView = _wtoi(argv[++i]);
        else if (a == L"--continuous") o.continuous = true;
        else if (a == L"--lowlat") o.lowLatency = true;
        else if (a == L"--studio") o.studio = true;
        else if (a == L"--applications") o.applications = true;
        else if (a == L"--launchpad") o.launchpad = true;
        else if (a == L"--launchpad-snapshot"&&i+1<argc){o.launchpadVisual=argv[++i];o.launchpad=true;}
        else if (a == L"--launchpad-config" && i + 1 < argc) o.launchpadConfig = argv[++i];
    }
    if (argv) LocalFree(argv);
    // Never use a saved diagnostics preference alone to enable test actions.
    if(o.devIconEditor&&(!o.explicitDiag||o.config.empty()||o.iconConfig.empty()))o.devIconEditor=false;
    if(!o.explicitDiag||o.launchpadConfig.empty()||o.desktopSettingsPath==liquid::settings::DefaultPath()||!wcsstr(GetCommandLineW(),L"--dev-isolated"))o.launchpadVisual.clear();
    const auto settings=liquid::settings::Load(o.desktopSettingsPath);
    o.diag=o.diag||settings.value.dock.diagnostics;
    o.iconMask=o.iconMaskOverride<0?settings.value.dock.useMacIconMask:o.iconMaskOverride!=0;
    o.autoHide=settings.value.dock.autoHide;
    return o;
}

class App {
public:
    ~App(){if(opt_.firstFrameReady && opt_.firstFrameReady!=INVALID_HANDLE_VALUE)CloseHandle(opt_.firstFrameReady);}
    int Run(HINSTANCE instance, const Options& options);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void LoadItems();
    void ResolveItem(PinnedItem& item);
    void ChangeIcon(int index, bool reset);
    void OpenIconEditor(int index);
    void ChangeIconFor(const PinnedItem& target, bool reset);
    void OpenIconEditorFor(const PinnedItem& target);
    // Launchpad right-click (kMsgLaunchpadAppMenu): the app menu, shown by the Dock over Launchpad.
    void RenameItem(const PinnedItem& item);
    void ShowLaunchpadAppMenu(const dock::launchpad::AppMenuRequest& request);
    void OpenApplications();
    bool StartApplications(bool background);
    dock::applications::WinKeyListener winKey_;
    HANDLE applicationsProcess_=nullptr;
    void RunLaunchpadIconCommand();
    PinnedItem launchpadIconItem_;  // the Launchpad app whose icon command waits for Launchpad to close
    UINT launchpadIconCommand_ = 0;
    void CheckCustomIconFiles();
    void LoadDockWorkshop();
    bool ApplyDockWorkshop(PinnedItem& item,const char* key);
    void BuildIcons();
    void RequestDownloadsPreview();
    void RefreshThemeIcons();
    bool ApplyPersonalDarkIcon(PinnedItem& item,const wchar_t* name);
    HRESULT CreateDockIcon(const std::vector<uint32_t>& pixels,UINT w,UINT h,ComPtr<ID2D1Bitmap1>& icon,ComPtr<ID2D1Bitmap1>& shadow,float& pad,int maskOverride=-1);
    void ApplyWindowLayout(const wchar_t* reason);
    void SyncTiles();  // hands the model each slot's separator flag and width (status groups are wider or narrower)
    bool SyncRunningApps();
    void NormalizeSections();
    int recyclePoll_ = 0;
    int downloadsPoll_ = 0;
    std::shared_ptr<DownloadsPreviewShared> downloadsPreview_=std::make_shared<DownloadsPreviewShared>();
    std::vector<std::uint32_t> downloadsPixels_;
    void SetFullRegion(bool full, bool force = false);
    void Frame();
    void StartAnimating();
    bool WantsFrame() const { return animating_ || needsFrame_ || backdropDirty_ || glass_.continuousRender || BackdropStreaming(); }
    // One-frame path: the desktop behind the Dock has been changing (video wallpaper, a moving window): render paced by
    // the swap chain like an animation, so each frame takes the newest desktop frame right after DWM hands it out. The
    // idle timer path only catches the first frame of a change (it landed a refresh late ~30-50% of the time, measured
    // 2026-11-20). Changes elsewhere on the screen do not count (ProcessCapture publishes only the Dock's area).
    double lastDockBackdrop_ = 0;
    // Only a source that changes every refresh streams: presenting unchanged frames between the frames of a slower one
    // (Wallpaper Engine at 30-60 fps) used the swap chain's slot, and its next frame then showed a refresh late (2.8
    // instead of ~1.9 refreshes, measured 2026-11-20). Slower sources take the idle path, which draws at once.
    bool BackdropStreaming() const {
        return lowLat_ && lastDockBackdrop_ > 0 && Now() - lastDockBackdrop_ < 1.5 * docky::vblank::FramePeriod().load();
    }
    // ---- One-frame DXGI glass (opt-in: --lowlat / DOCKY_LOWLAT=1) ------------------------------------------------
    // The Dock's render device owns its monitor's desktop duplication (SharedDesktop owner mode) and takes each desktop
    // frame on this thread right before drawing; the UI layer is drawn before that (Renderer::PrepareUi) so only the GPU
    // glass lies between the frame and Present. Measured basis: experiments/hostbackdrop-lab/REPORT-dxgi-latency.md.
    bool lowLat_ = false;
    HMONITOR ownedMonitor_ = nullptr;
    bool backdropDirty_ = false;     // a new desktop image only: the cached UI layer is still valid
    HANDLE ownerTimer_ = nullptr;    // wakes the idle loop just after DWM hands out the next frame
    double lastOwnedFrame_ = 0;      // Now() when PumpOwned last took a frame
    double lastFrameEnd_ = 0;        // Now() after the previous frame's Present (long-frame log)
    double housekeepingDeferredSince_ = 0;  // status / Recycle Bin checks held back while animating (at most 5 s)
    double loopWaitSec_ = 0, loopMsgSec_ = 0, loopSlowMsgSec_ = 0; UINT loopSlowMsg_ = 0; WPARAM loopSlowMsgWp_ = 0; wchar_t loopSlowClass_[64]{};  // between frames
    bool LowLatActive() const;
    bool lowLatForced_ = false, latencyLogForced_ = false;  // --lowlat / DOCKY_LOWLAT, DOCKY_LATENCY_LOG
    void SetLowLatency(bool on, const wchar_t* why);         // Settings > General > Low latency glass (live)
    // Game compatibility: the Menu Bar's full-screen game detection and the games list (game_compat.h) both hide the
    // Dock and stop capture (SetGameSuppressed).
    bool gameCompatSetting_ = false, menuBarGame_ = false, listedGame_ = false;
    std::wstring listedGameName_;
    docky::game::Watcher gameWatch_;
    // The monitor duplication this Dock owns (low latency), whether or not the Dock's own glass currently reads it: Control
    // Center, menus and the Fan depend on this thread taking its frames even while the Dock is hidden or smoked.
    docky::desk::SharedDesktop* OwnedDesktop() const {
        if (!lowLat_ || !ownedMonitor_) return nullptr;
        auto* d = &docky::desk::SharedDesktop::ForMonitor(ownedMonitor_);
        return d->Owned() ? d : nullptr;
    }
    double latencyLogEvery_ = 0;     // seconds between latency log lines; 0 = counter off (standard builds)
    void RequestBackdrop(bool enabled, HMONITOR monitor);
    void ReleaseCaptureOwner();
    double NextOwnedFrameDelay() const;
    // Latency counter (both paths, Info log every 10 s and at exit): from the LastPresentTime of the desktop image a
    // frame showed to the moment that frame reached the screen (DXGI frame statistics).
    std::map<UINT, int64_t> latencyPending_;
    int64_t latencyLastLp_ = 0;
    double latencySumMs_ = 0, latencyMaxMs_ = 0, latencyLogAt_ = 0; uint64_t latencyN_ = 0; std::map<int, uint64_t> latencyHist_;
    double llUiMs_ = 0, llWaitMs_ = 0, llComposeCpu_ = 0; uint64_t llPrepared_ = 0, llComposed_ = 0, llFull_ = 0, llWaits_ = 0, llMisses_ = 0;
    uint64_t llAcq0_ = 0, llAccum0_ = 0;
    void TrackLatency(bool force);
    void SyncGlassParameters();
    bool RefreshAccessibility();
    bool UpdateGlassPointer(double dt);
    void OnMouseMove(float x, float y);
    void OnMouseLeave();
    int SeparatorAt(float x, float y) const;
    void FinishResize();
    void OnLButtonDown(float x, float y);
    void OnLButtonUp(float x, float y);
    void UpdateDragTarget(float x, float y);
    void FinishDrag(bool commit);
    void OnRButtonUp(float x, float y);
    void UpdateDockAppBar();  // reserve / release the Dock band as a bottom AppBar (MyDockFinder "window boundary")
    static constexpr UINT kDockAppBarMsg = WM_APP + 78;
    // macOS: an app in true full screen (covering the whole monitor, e.g. F11 / its full-screen button) hides the Dock;
    // a maximized window keeps it. Told by the shell (ABN_FULLSCREENAPP, while the AppBar is registered) and by the Menu
    // Bar (always registered) with kDockFullscreenMsg.
    static constexpr UINT kDockFullscreenMsg = WM_APP + 79;
    bool fullscreenApp_ = false;
    bool gameSuppressed_ = false;
    // A full-screen app makes the Dock auto-hide (macOS): hidden, revealed over the app by moving to the bottom edge,
    // without changing the work area or the app's size.
    bool AutoHiding() const { return autoHide_ || fullscreenApp_; }
    // Game mode: a full-screen app owns the monitor and the Dock has slid away. Nothing of the Dock is visible, so the
    // desktop capture stops (it copied every game frame) and background polling slows to every 5 s.
    bool GameMode() const { return gameSuppressed_ || (fullscreenApp_ && hiddenRegion_); }
    // Launchpad covers the whole monitor (and the Dock): the Dock's own desktop capture is invisible meanwhile, and every
    // Launchpad frame made Windows deliver (and on HDR convert) a full-screen capture frame to it, ~7 ms of GPU per frame.
    bool launchpadCovers_ = false;
    bool CaptureWanted() const { return !opaqueFallback_ && glass_.backdrop == BackdropKind::Live && !GameMode() && !launchpadCovers_; }
    int gameTick_ = 0;
    // Minimized windows as Dock tiles (minimize_dock.h). The Genie needs Windows' own minimize animation off: with the
    // setting on it is paused for this session only (SPI_SETANIMATION without SPIF_UPDATEINIFILE) and restored on exit.
    static constexpr UINT kMinimizeMsg = WM_APP + 81;  // wParam 1 added / 0 removed, lParam HWND
    std::vector<HWND> minimized_;
    bool genie_ = false, pausedWindowsAnimation_ = false;
    void ApplyGenie(bool on);
    void MinimizedChanged(HWND w, bool added);
    void PlaceMinimizedTiles();
    void SetFullscreenApp(bool full);
    void SetGameSuppressed(bool suppress);
    void ApplyGameMode();
    bool dockAppBar_ = false, dockAppBarEnabled_ = false;
    RECT dockAppBarRect_{};
    void OnKey(WPARAM key);
    void OnDrop(HDROP drop);
    void PinPaths(const std::vector<std::wstring>& paths, float x);
    void AddToLaunchpad(const std::vector<std::wstring>& paths);
public:
    // OLE drop target (macOS): the Dock magnifies under a dragged file; a drop on the Trash recycles it, on an app
    // opens it with that app, on Downloads moves it there, and between icons pins it (as WM_DROPFILES did).
    enum class DropKind { None, Pin, Trash, OpenWith, Downloads, Launchpad };
    DWORD DragHover(POINTL screen, DWORD allowed, bool leave);
    DWORD DragDropped(IDataObject* data, POINTL screen, DWORD allowed);
private:
    DropKind DropKindAt(int hit) const;
    int dropHit_ = -1;
    static constexpr UINT kDropWork = WM_APP + 83;  // deferred file work after the drag source has returned
    struct DropJob { DropKind kind; std::vector<std::wstring> paths; int item; float x; };
    std::vector<DropJob> dropJobs_;
    void ActivateItem(int index);
    void Reload();
    void DumpState(const wchar_t* reason);
    bool RecreateDevice();
    void TrackStats(double now, double dt, bool animatingFrame);
    void FlushStats(double now, const wchar_t* reason);
    std::wstring DiagText() const;
    float Dip(int px) const { return static_cast<float>(px) / scale_; }

    HWND hwnd_ = nullptr;
    Options opt_;
    std::wstring configPath_;
    std::wstring downloadsPath_,downloadsSettingsPath_,downloadsSettingsError_;
    std::wstring launchpadPath_;
    dock::downloads::Settings downloadsSettings_;
    bool downloadsSettingsValid_=true;
    bool iconMaskEnabled_=true;
    liquid::settings::Revision settingsRevision_{};
    docky::workshop::Pack dockPack_;
    bool dockPackEnabled_=false;
    std::wstring dockPackRoot_;
    liquid::settings::Revision dockPackRevision_{};
    CustomIconStore iconStore_;
    std::wstring iconStoreError_;
    std::map<std::string, IconFileStamp> iconFileStamps_;
    float dpi_ = 96.0f;
    float scale_ = 1.0f;
    UINT pxW_ = 0, pxH_ = 0;
    RECT windowRect_{};
    DockModel model_;
    DockFrame frame_;
    bool logGeometry_ = true;
    HWINEVENTHOOK fgHook_ = nullptr;
    Renderer renderer_;
    WgcBackdropSource backdrop_;
    double timingCpu_ = 0, timingPresent_ = 0, timingPresentMax_ = 0; uint64_t timingFrames_ = 0, timingSlowPresents_ = 0;
    SimulatedBackdropSource simBackdrop_;   // stand-in until P0.2 WGC capture is integrated
    GlassParameters glass_;                 // per-frame snapshot of GlassSettingsStore
    unsigned long long glassVersion_ = 0;
    bool loggedLiveFrame_ = false;
    bool loggedLiveFallback_ = false;
    float mouseX_ = 0, mouseY_ = 0;         // last pointer position, DIP
    // Status group: the menu bar's status items as one Dock tile (status_group.h), cells from shared settings.
    std::vector<std::wstring> statusCells_;
    dock::statusgroup::State statusState_;
    liquid::status::Snapshot statusSnapshot_;
    HKL statusLayout_ = nullptr; HWND statusInputTarget_ = nullptr;
    std::atomic<bool> statusQuerying_{false};
    int statusTick_ = 0;
    bool menuBarEnabled_ = true;
    bool statusPercent_ = false;  // dock.statusBatteryPercent
    int statusTileStyle_ = 0;      // dock.statusTileStyle: Clock / Calendar plates
    std::vector<PinnedItem> MakeStatusItems() const;
    // Removable Dock items (dock.showDownloads / showTrash / showDivider) and the order of the right-hand items.
    bool showDownloads_ = true, showTrash_ = true, showDivider_ = true;
    std::vector<std::wstring> tailOrder_;
    bool removeOnDrop_ = false;   // the dragged item is held well above the Dock: letting go removes it
    int sepPress_ = -1; POINT sepPressCursor_{};  // a separator pressed: vertical motion resizes, horizontal moves it
    void RebuildTail(const wchar_t* why);
    void RemoveDockItem(int index);
    bool ToggleStatusCell(const std::wstring& id);
    bool SaveTailOrder();
    size_t PinnedEnd() const;
    std::string DragKey(size_t index) const;
    void BeginSeparatorResize();
    bool statusLight_ = false;    // the backdrop behind the status group is bright: black glyphs (menu bar rule)
    int statusLightVotes_ = 0;    // consecutive samples asking to switch (black needs a bright backdrop for 3 s)
    static constexpr UINT kStatusReadyMsg = WM_APP + 85, kStatusPanelMsg = WM_APP + 86, kStatusTrayMsg = WM_APP + 87, kStatusVolumeMsg = WM_APP + 88;
    liquid::controls::VolumeWatcher statusVolume_;   // volume changes reach the combined cell at once
    double statusInkAt_ = 0;                           // last backdrop sample for black / white glyphs
    void ApplyStatusCells(const std::vector<std::wstring>& cells, bool menuBarEnabled);
    void RefreshStatus(bool query);
    void OpenStatusCell(int index, int cell);
    void ShowStatusGroupMenu(int index);
    void UpdateTrayCells();                                    // live icons for tray icons kept in the Dock
    bool ccPrewarmed_ = false;                                 // Control Center's device made on first hover
    std::wstring StatusCellLabel(const std::wstring& id) const;  // CellLabel, with a kept tray icon's app name
    RECT StatusCellScreenRect(int index, int cell) const;        // a status cell on screen (px)
    float ptrX_ = 0, ptrY_ = 0, ptrHover_ = 0, ptrPress_ = 0;  // smoothed, window px
    float cpuEma_ = 0;
    std::vector<PinnedItem> items_;
    std::vector<ComPtr<ID2D1Bitmap1>> iconBmp_, shadowBmp_;
    ComPtr<ID2D1Bitmap1> downloadsOpenBmp_;  // open-state Downloads icon (V0.9.3)
    std::vector<float> shadowPad_;
    bool animating_ = false;
    // Auto-hide (V0.15, docky only): 0 shown .. 1 hidden. The DComp visual slides down; while hidden the
    // window region is a thin strip on the screen edge so the pointer reaching the edge reveals the Dock.
    bool autoHide_ = false, hiddenRegion_ = false;
    bool reducedMotion_ = false, opaqueFallback_ = true, highContrast_ = false;
    int appearanceMode_=0; bool systemDark_=false;
    winrt::Windows::UI::ViewManagement::UISettings systemUi_{nullptr};
    docky::motion::Spring hide_;  // 0 shown .. 1 hidden; velocity survives interruptions
    bool hideInit_ = false;
    float appliedOffset_ = -1.0f;
    double leftAt_ = -1.0;
    int autoHideBusy_ = -1;
    bool UpdateAutoHide(double dt, double now);
    bool needsFrame_ = true;
    bool tracking_ = false;
    bool regionFull_ = false;
    bool regionInit_ = false;
    bool diag_ = false;
    bool bounds_ = false;
    int pressed_ = -1;
    float pressX_=0,pressY_=0,grabX_=0,grabY_=0;
    bool resizing_=false; int resizeStartY_=0, resizeStartSize_=46, resizeMaxSize_=80;
    bool dragging_=false,dropAllowed_=false;
    int dragFrom_=-1,dragTarget_=-1;
    DragRegion dragRegion_;
    std::string dragId_;
    std::vector<docky::motion::Spring> visualOffsets_,visualYOffsets_;  // reorder slide (x) and post-drop settle (y), DIP
    double lastT_ = 0.0;
    double startT_ = 0.0;
    bool firstFrameLogged_ = false;
    // Animation timing statistics (only while animating; idle frames are on demand).
    double statStart_ = 0.0, statLast_ = 0.0, statMaxDt_ = 0.0;
    int statFrames_ = 0;
    UINT statPresent0_ = 0, statSync0_ = 0;
    bool statDxgi_ = false;
    double lastFps_ = 0.0, lastMaxMs_ = 0.0;
    double refreshHz_ = 0.0;
    unsigned long long totalFrames_ = 0;
    unsigned long long animationFrames_ = 0;
    unsigned long long animationVersion_ = 0;
};

class DockDropTarget final : public IDropTarget {
public:
    explicit DockDropTarget(App* app) : app_(app) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files_ = data && SUCCEEDED(data->QueryGetData(&f));
        *effect = files_ ? app_->DragHover(pt, *effect, false) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override { *effect = files_ ? app_->DragHover(pt, *effect, false) : DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { app_->DragHover(POINTL{}, 0, true); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        *effect = files_ ? app_->DragDropped(data, pt, *effect) : DROPEFFECT_NONE;
        app_->DragHover(POINTL{}, 0, true);
        return S_OK;
    }
private:
    ~DockDropTarget() = default;
    App* app_; LONG refs_ = 1; bool files_ = false;
};

App::DropKind App::DropKindAt(int hit) const {
    if (hit < 0 || static_cast<size_t>(hit) >= items_.size()) return DropKind::Pin;
    const auto& it = items_[hit];
    if (it.isRecycleBin) return DropKind::Trash;
    if (it.isDownloads) return DropKind::Downloads;
    if(it.isLaunchpad)return DropKind::Launchpad;
    if (it.isSeparator || it.isMinimized || it.isStatusGroup) return DropKind::Pin;
    const std::wstring& t = !it.exePath.empty() ? it.exePath : it.target;
    if (t.empty() || t.rfind(L"shell:", 0) == 0) return DropKind::Pin;  // packaged apps take no file arguments here
    return DropKind::OpenWith;
}

DWORD App::DragHover(POINTL screen, DWORD allowed, bool leave) {
    if (leave) { dropHit_ = -1; OnMouseLeave(); return DROPEFFECT_NONE; }
    POINT pt{screen.x, screen.y}; ScreenToClient(hwnd_, &pt);
    OnMouseMove(Dip(pt.x), Dip(pt.y));  // same magnification as the pointer
    const int hit = model_.HitTest(frame_, Dip(pt.x), Dip(pt.y));
    if (hit != dropHit_) { dropHit_ = hit; needsFrame_ = true; }
    switch (DropKindAt(hit)) {
    case DropKind::Trash: case DropKind::Downloads: return allowed & DROPEFFECT_MOVE ? DROPEFFECT_MOVE : allowed & DROPEFFECT_COPY;
    case DropKind::OpenWith: return allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : allowed & DROPEFFECT_LINK;
    default: return allowed & DROPEFFECT_LINK ? DROPEFFECT_LINK : allowed & DROPEFFECT_COPY;
    }
}

DWORD App::DragDropped(IDataObject* data, POINTL screen, DWORD allowed) {
    FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
    if (!data || FAILED(data->GetData(&f, &m))) return DROPEFFECT_NONE;
    std::vector<std::wstring> paths;
    if (auto drop = static_cast<HDROP>(m.hGlobal)) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && i < 500; ++i) { wchar_t path[MAX_PATH * 2]; if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) paths.push_back(path); }
    }
    ReleaseStgMedium(&m);
    if (paths.empty()) return DROPEFFECT_NONE;
    POINT pt{screen.x, screen.y}; ScreenToClient(hwnd_, &pt);
    const int hit = model_.HitTest(frame_, Dip(pt.x), Dip(pt.y));
    const DropKind kind = DropKindAt(hit);
    const DWORD effect = DragHover(screen, allowed, false);
    if(effect==DROPEFFECT_NONE)return DROPEFFECT_NONE;
    // The file work runs after the drag source's DoDragDrop has returned, so the source app never waits on it.
    dropJobs_.push_back({kind, std::move(paths), hit, Dip(pt.x)});
    PostMessageW(hwnd_, kDropWork, 0, 0);
    DLOG(L"drop: kind=%d item=%d", static_cast<int>(kind), hit);
    return effect;
}

int App::Run(HINSTANCE instance, const Options& options) {
    opt_ = options;
    liquid::cc::SetSettingsPath(opt_.desktopSettingsPath);
    iconMaskEnabled_=options.iconMask;
    autoHide_=options.autoHide;
    try {systemUi_=winrt::Windows::UI::ViewManagement::UISettings();} catch(...) {systemUi_=nullptr;}
    RefreshAccessibility();
    appearanceMode_=liquid::settings::Load(opt_.desktopSettingsPath).value.appearance.mode;
    try {if(systemUi_){const auto c=systemUi_.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);systemDark_=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000;}} catch(...) {}
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) reducedMotion_ = !animations;
    model_.SetReducedMotion(reducedMotion_);
    diag_ = options.diag;
    startT_ = Now();
    const std::wstring dataDir = DefaultDataDir();
    configPath_ = opt_.config.empty() ? dataDir + L"\\pinned.txt" : opt_.config;
    launchpadPath_ = opt_.launchpadConfig.empty() ? dataDir + L"\\launchpad.json" : opt_.launchpadConfig;
    { const auto perf = liquid::settings::Load(opt_.desktopSettingsPath).value.performance; docky::vblank::SetCaps(perf.animationFps, perf.captureFps);
      DLOG(L"frame rate caps: animation %d, capture %d (0 = display rate)", perf.animationFps, perf.captureFps); }
    downloadsPath_=opt_.downloadsPath.empty()?dock::downloads::KnownDownloadsPath():opt_.downloadsPath;
    downloadsSettingsPath_=opt_.downloadsSettings.empty()?dataDir+L"\\downloads-stack-v1.txt":opt_.downloadsSettings;
    downloadsSettingsValid_=dock::downloads::LoadSettings(downloadsSettingsPath_,downloadsSettings_,downloadsSettingsError_);
    const std::wstring iconPath = opt_.iconConfig.empty() ? dataDir + L"\\custom-icons.json" : opt_.iconConfig;
    Log::Get().Open(opt_.log.empty() ? dataDir + L"\\dock.log" : opt_.log);
    if (!iconStore_.Load(iconPath, iconStoreError_)) DLOG(L"custom icons: settings unavailable: %ls", iconStoreError_.c_str());
    dock::launchpad::SetCustomIconPath(iconPath);  // Launchpad shows the same custom icons and edits
    DLOG(L"==== docky start; config=%ls", configPath_.c_str());

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, kClassName,
                            L"DockOver", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.bottom - 10, 10, 10, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        DLOG(L"CreateWindowEx failed %lu", GetLastError());
        return 1;
    }
    backdrop_.SetExcludedWindow(hwnd_);
    if (SUCCEEDED(OleInitialize(nullptr))) {
        auto* target = new DockDropTarget(this);
        if (SUCCEEDED(RegisterDragDrop(hwnd_, target))) DLOG(L"drop: OLE drop target registered (hover magnification, Trash, open with)");
        target->Release();
    }
    {
        GlassSettingsStore& store = GlassSettingsStore::Get();
        store.Load(opt_.glassConfig.empty() ? dataDir + std::wstring(L"\\glass.json") : opt_.glassConfig);
        store.SetDockWindow(hwnd_);
        DLOG(L"glass: %ls (%ls)", store.LoadMessage().c_str(), store.Path().c_str());
        GlassParameters p = store.Current();
        bool overridden = false;
        if (!opt_.material.empty()) {
            p.material = opt_.material == L"smoked" ? MaterialKind::Smoked : MaterialKind::Liquid;
            overridden = true;
        }
        if (!opt_.backdrop.empty()) {
            const wchar_t* names[] = {L"wallpaper", L"grid", L"gradient", L"dark", L"none", L"live"};
            for (int i = 0; i < 6; ++i)
                if (opt_.backdrop == names[i]) p.backdrop = static_cast<BackdropKind>(i);
            overridden = true;
        }
        if (opt_.debugView >= 0) {
            p.debugView = opt_.debugView;
            overridden = true;
        }
        if (opt_.continuous) {
            p.continuousRender = true;
            overridden = true;
        }
        if (overridden) store.SetCurrent(p);
    }
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));

    DWM_TIMING_INFO timing{sizeof(timing)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiDenominator)
        refreshHz_ = static_cast<double>(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator;

    dockPackRoot_=opt_.workshopRoot.empty()?docky::workshop::DefaultRoot():opt_.workshopRoot;
    dockPackRevision_=liquid::settings::ReadRevision(dockPackRoot_+L"\\active.txt");
    LoadDockWorkshop();
    { const auto initial = liquid::settings::Load(opt_.desktopSettingsPath).value; statusCells_ = initial.dock.statusCells; menuBarEnabled_ = initial.menuBar.enabled; statusPercent_ = initial.dock.statusBatteryPercent; statusTileStyle_ = initial.dock.statusTileStyle;
      showDownloads_ = initial.dock.showDownloads; showTrash_ = initial.dock.showTrash; showDivider_ = initial.dock.showDivider; tailOrder_ = initial.dock.tailOrder;
      if (!menuBarEnabled_ && statusCells_.empty()) statusCells_ = liquid::settings::Dock{}.statusCells; }  // the menu bar is off: status items need a home
    LoadItems();
    if (!statusCells_.empty()) RefreshStatus(true);  // time, input layout now; Wi-Fi / battery / volume when the first query returns
    { const auto dockPrefs = liquid::settings::Load(opt_.desktopSettingsPath).value.dock;
      model_.m.SetIconSize(static_cast<float>(dockPrefs.iconSize));
      model_.m.maxScale = 1.0f + dockPrefs.magnification / 100.0f; }  // Settings > Dock > Magnification
    RefreshRunningState(items_, hwnd_);
    ApplyWindowLayout(L"startup");
    if (!renderer_.Ready()) return 2;
    BuildIcons();
    {std::lock_guard<std::mutex> lock(downloadsPreview_->mutex);downloadsPreview_->window=hwnd_;}
    RequestDownloadsPreview();
    simBackdrop_.SetDevice(renderer_.Device());
    backdrop_.SetDevice(renderer_.Device());
    backdrop_.SetDiagnostics(opt_.wgcCompare);
    { wchar_t v[8]{}; lowLatForced_ = opt_.lowLatency || (GetEnvironmentVariableW(L"DOCKY_LOWLAT", v, 8) && v[0] == L'1'); }
    if (wchar_t v[8]{}; GetEnvironmentVariableW(L"DOCKY_LATENCY_LOG", v, 8) && v[0] == L'1') latencyLogForced_ = true;  // measurements
    {   const auto initial = liquid::settings::Load(opt_.desktopSettingsPath).value.dock;
        gameCompatSetting_ = initial.gameCompatibility;
        SetLowLatency(lowLatForced_ || initial.lowLatencyGlass, lowLatForced_ ? L"command line / DOCKY_LOWLAT" : L"settings"); }
    if (latencyLogForced_) latencyLogEvery_ = 10.0;
    if (!opt_.snapshot.empty()) renderer_.RequestSnapshot(opt_.snapshot);
    SyncGlassParameters();
    DLOG(L"glass: liquid material %ls; current %ls, backdrop %ls", renderer_.LiquidAvailable() ? L"available" : L"UNAVAILABLE",
         MaterialName(glass_.material), BackdropName(glass_.backdrop));
    DLOG(L"renderer ready: feature level 0x%X, refresh %.2f Hz, waitable %ls, backdrop %ls, material %ls",
         renderer_.FeatureLevel(), refreshHz_, renderer_.Waitable() ? L"yes" : L"no", backdrop_.Name(),
         renderer_.MaterialName());
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (opt_.studio) OpenGlassStudio(hwnd_);
    if(!opt_.launchpadVisual.empty())dock::launchpad::ConfigureVisualTest(opt_.launchpadVisual);
    // Launchpad's app discovery and icon cache (566 apps here: ~8 s of CPU) wait until the Dock has settled after start;
    // opening Launchpad before that discovers on demand.
    if (opt_.launchpad || !opt_.launchpadVisual.empty()) dock::launchpad::Prewarm(); else SetTimer(hwnd_, kTimerLaunchpadPrewarm, 6000, nullptr);
    dock::downloads::SetFanRendererFactory(dock::downloads::MakeGpuFanRendererFactory());  // Fan V0.9: GPU Liquid Glass labels  // background app discovery + icon cache for the first opening
    winKey_.SetEnabled(!fullscreenApp_ && !GameMode());
    if (!wcsstr(GetCommandLineW(),L"--dev-isolated") && !winKey_.Start(hwnd_))
        DLOG(L"Applications: Windows key listener unavailable");
    if (opt_.applications) OpenApplications();
    else if (!wcsstr(GetCommandLineW(),L"--dev-isolated")) SetTimer(hwnd_, kTimerApplicationsPrewarm, 2500, nullptr);
    if (opt_.launchpad) dock::launchpad::Toggle(hwnd_, launchpadPath_, opt_.desktopSettingsPath);
    if(opt_.devIconEditor){
        for(size_t i=0;i<items_.size();++i){const auto& item=items_[i];
            if(!item.isSeparator&&!item.isDownloads&&!item.isRecycleBin&&!item.isLaunchpad&&!item.stableId.empty()){
                OpenIconEditor(static_cast<int>(i));DLOG(L"dev: icon editor requested index=%zu",i);break;
            }}
    }
    settingsRevision_=liquid::settings::ReadRevision(opt_.desktopSettingsPath);
    // V0.16: remember the last foreground window outside this process (clicking the Dock makes the Dock foreground).
    fgHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        [](HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) {
            DWORD pid = 0; if (h) GetWindowThreadProcessId(h, &pid);
            if (h && pid != GetCurrentProcessId()) g_lastExternalForeground = h;
        }, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(hwnd_, kTimerRunning, 1000, nullptr);  // state polling only; never drives animation
    if (!statusVolume_.Start(hwnd_, kStatusVolumeMsg)) DLOG(L"status group: volume watcher unavailable (volume follows the 5 s query)");
    if (opt_.exitAfter <= 0) {  // timed test runs never hook other windows or change Windows animation settings
        HWND self = hwnd_;
        if (dock::minimize::Init(hwnd_, [self](HWND w, bool added) { PostMessageW(self, kMinimizeMsg, added ? 1 : 0, reinterpret_cast<LPARAM>(w)); }))
            ApplyGenie(liquid::settings::Load(opt_.desktopSettingsPath).value.dock.genieMinimize);
        else DLOG(L"minimize: overlay unavailable");
        { const auto d = liquid::settings::Load(opt_.desktopSettingsPath).value.dock; dock::downloads::SetFanOptions(d.downloadsCount, d.downloadsVertical, d.downloadsPreview); }
    }
    dockAppBarEnabled_ = opt_.exitAfter <= 0;  // timed test runs never change the user's work area
    UpdateDockAppBar();
    if (opt_.exitAfter > 0) SetTimer(hwnd_, kTimerExit, static_cast<UINT>(opt_.exitAfter * 1000.0), nullptr);
    needsFrame_ = true;

    // The capture completion event and animation waitable are independent. Under a frame-rate cap (Settings > General)
    // a frame that is due too early waits on a high-resolution timer instead (docky::vblank::Gate).
    docky::vblank::Gate gate;
    HANDLE paceTimer = docky::vblank::CreatePaceTimer();
    bool paceArmed = false;
    auto render = [&] {
        if (const double left = gate.Remaining(); left > 0 && paceTimer) { docky::vblank::Arm(paceTimer, left); paceArmed = true; return; }
        Frame(); gate.Mark();
    };
    MSG msg{};
    for (;;) {
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            const double m0 = latencyLogEvery_ > 0 ? Now() : 0;
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (m0 > 0) { const double d = Now() - m0; loopMsgSec_ += d; if (d > loopSlowMsgSec_) { loopSlowMsgSec_ = d; loopSlowMsg_ = msg.message; loopSlowMsgWp_ = msg.wParam; loopSlowClass_[0] = 0; if (msg.hwnd) GetClassNameW(msg.hwnd, loopSlowClass_, 64); } }
        }
        if (quit) break;
        HANDLE handles[4]{}; DWORD count = 0; int gpuIndex = -1, renderIndex = -1, paceIndex = -1, ownerIndex = -1;
        if (HANDLE h = backdrop_.CompletionEvent()) { gpuIndex = static_cast<int>(count); handles[count++] = h; }
        if (paceArmed) { paceIndex = static_cast<int>(count); handles[count++] = paceTimer; }
        else if (WantsFrame() && renderer_.Waitable()) { renderIndex = static_cast<int>(count); handles[count++] = renderer_.Waitable(); }
        if (WantsFrame() && renderIndex < 0 && !paceArmed) { render(); continue; }
        // Low latency, nothing to draw: this thread still takes the desktop frames (for the Dock and every other glass
        // surface of this process), just after DWM hands each one out.
        if (!WantsFrame() && ownerTimer_ && OwnedDesktop() && OwnedDesktop()->OwnerNeedsPump()) {
            docky::vblank::Arm(ownerTimer_, NextOwnedFrameDelay()); ownerIndex = static_cast<int>(count); handles[count++] = ownerTimer_;
        }
        const double wait0 = latencyLogEvery_ > 0 ? Now() : 0;
        DWORD w = MsgWaitForMultipleObjectsEx(count, handles, WantsFrame() ? 100 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait0 > 0) loopWaitSec_ += Now() - wait0;
        if (ownerIndex >= 0 && w == WAIT_OBJECT_0 + ownerIndex) {
            const bool recent = lastOwnedFrame_ > 0 && Now() - lastOwnedFrame_ < 2.5 * docky::vblank::FramePeriod().load();
            auto* owned = OwnedDesktop();
            bool got = owned && owned->PumpOwned(0);  // non-blocking looks (see Frame), at most 1 ms when frames are flowing
            for (const double t0w = Now(); owned && !got && recent && Now() - t0w < 0.001;) { docky::vblank::Pause(0.00025); got = owned->PumpOwned(0); }
            if (got) {
                lastOwnedFrame_ = Now();
                if (LowLatActive() && glass_.backdrop == BackdropKind::Live && backdrop_.ProcessCapture(windowRect_)) {
                    backdropDirty_ = true; lastDockBackdrop_ = Now();
                    // Draw it now when the swap chain can take a frame: another trip round the loop could miss DWM's next
                    // composition (~1 ms after it hands out the frame).
                    if (!paceArmed && renderer_.Waitable() && WaitForSingleObject(renderer_.Waitable(), 0) == WAIT_OBJECT_0) render();
                }
            }
        } else if (gpuIndex >= 0 && w == WAIT_OBJECT_0 + gpuIndex) {
            if (backdrop_.CompleteCompare(windowRect_)) { if (LowLatActive()) backdropDirty_ = true; else needsFrame_ = true; }
        } else if (paceIndex >= 0 && w == WAIT_OBJECT_0 + paceIndex) {
            paceArmed = false;
            if (WantsFrame()) { Frame(); gate.Mark(); }
        } else if ((renderIndex >= 0 && w == WAIT_OBJECT_0 + renderIndex) || (WantsFrame() && w == WAIT_TIMEOUT && !paceArmed)) {
            render();
        }
    }
    winKey_.Stop();
    if (paceTimer) CloseHandle(paceTimer);
    TrackLatency(true);
    backdrop_.Request(false, nullptr);  // stop reading first, so handing the duplication back starts no thread at exit
    ReleaseCaptureOwner();
    if (ownerTimer_) { CloseHandle(ownerTimer_); ownerTimer_ = nullptr; }
    if(applicationsProcess_){  // the resident window only hides on WM_CLOSE: ask it to quit
        if(HWND win=ApplicationsWindow(applicationsProcess_))PostMessageW(win,RegisterWindowMessageW(L"DockOverApplicationsQuit"),0,0);
        if(WaitForSingleObject(applicationsProcess_,1500)==WAIT_TIMEOUT)TerminateProcess(applicationsProcess_,0);
        CloseHandle(applicationsProcess_);
    }
    dock::launchpad::Shutdown();
    liquid::cc::Shutdown();  // joins the Control Center thread; a still-joinable std::thread at exit aborts the process
    ShutdownGlassStudio();
    FlushStats(Now(), L"exit");
    DLOG(L"==== docky exit code %d after %.2f s, %llu frames", static_cast<int>(msg.wParam), Now() - startT_,
         totalFrames_);
    DLOG(L"render timing: %llu frames, cpu %.2f ms avg, present %.2f ms avg / %.2f ms max, %llu presents over 2 ms",
         static_cast<unsigned long long>(timingFrames_), timingFrames_ ? timingCpu_ / timingFrames_ : 0.0, timingFrames_ ? timingPresent_ / timingFrames_ : 0.0,
         timingPresentMax_, static_cast<unsigned long long>(timingSlowPresents_));
    DLOG(L"diagnostics: render_frames=%llu animation_frames=%llu animationVersion=%llu materialVersion=%llu present_calls=%llu; actual displayed frames not measured",
         totalFrames_, animationFrames_, animationVersion_, glassVersion_, static_cast<unsigned long long>(renderer_.PresentCalls()));
    backdrop_.SetDevice(nullptr);
    renderer_.Shutdown();
    Log::Get().Close();
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<App*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case dock::applications::OpenFromWinKey:
        if (!fullscreenApp_ && !GameMode()) OpenApplications();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp)==HTCLIENT) {
            POINT p{}; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
            if (resizing_ || SeparatorAt(Dip(p.x), Dip(p.y)) >= 0) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
        }
        break;
    case WM_MOUSEMOVE:
        OnMouseMove(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
        return 0;
    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;
    case WM_MOUSEACTIVATE:
        // Over a full-screen app (game) a click on the revealed Dock must not take the focus: an exclusive full-screen
        // game minimizes as soon as it loses it. Launching / switching still hands the foreground to the chosen app.
        if (fullscreenApp_) return MA_NOACTIVATE;
        break;
    case WM_LBUTTONDOWN:
        OnLButtonDown(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
        return 0;
    case WM_LBUTTONUP:
        OnLButtonUp(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
        return 0;
    case WM_RBUTTONUP:
        OnRButtonUp(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
        return 0;
    case WM_CAPTURECHANGED:
        if (resizing_) FinishResize();
        if (dragging_) FinishDrag(false);
        if (pressed_ >= 0) {
            pressed_ = -1;
            needsFrame_ = true;
        }
        return 0;
    case WM_KEYDOWN:
        OnKey(wp);
        return 0;
    case WM_DROPFILES:
        OnDrop(reinterpret_cast<HDROP>(wp));
        return 0;
    case kDropWork: {
        auto jobs = std::move(dropJobs_); dropJobs_.clear();
        for (auto& job : jobs) {
            if (job.kind == DropKind::Pin) { PinPaths(job.paths, job.x); continue; }
            if(job.kind==DropKind::Launchpad){AddToLaunchpad(job.paths);continue;}
            if (job.kind == DropKind::OpenWith && job.item >= 0 && static_cast<size_t>(job.item) < items_.size()) {
                const auto& app = items_[job.item];
                std::wstring args = app.args;
                for (const auto& p : job.paths) args += (args.empty() ? L"" : L" ") + std::wstring(L"\"") + p + L"\"";
                const std::wstring exe = !app.exePath.empty() ? app.exePath : app.target;
                const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, nullptr, exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL));
                DLOG(L"drop: opened %zu file(s) with '%ls' result=%lld", job.paths.size(), app.label.c_str(), static_cast<long long>(r));
                continue;
            }
            // Trash: recycle (undoable; Windows warns if an item is too large for the Recycle Bin). Downloads: move.
            ComPtr<IFileOperation> op;
            if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) continue;
            op->SetOwnerWindow(hwnd_);
            ComPtr<IShellItem> downloads;
            if (job.kind == DropKind::Trash) op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE);
            else if (FAILED(SHGetKnownFolderItem(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&downloads)))) continue;
            else op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR);
            for (const auto& p : job.paths) {
                ComPtr<IShellItem> item;
                if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item)))) continue;
                if (job.kind == DropKind::Trash) op->DeleteItem(item.Get(), nullptr); else op->MoveItem(item.Get(), downloads.Get(), nullptr, nullptr);
            }
            const HRESULT hr = op->PerformOperations();
            DLOG(L"drop: %ls %zu item(s) hr=0x%08X", job.kind == DropKind::Trash ? L"recycled" : L"moved to Downloads", job.paths.size(), static_cast<unsigned>(hr));
            if (job.kind == DropKind::Trash) for (auto& it : items_) if (it.isRecycleBin) { ResolveItem(it); BuildIcons(); needsFrame_ = true; break; }
        }
        return 0;
    }
    case kMsgCaptureAvailable:
        if (glass_.backdrop == BackdropKind::Live && backdrop_.ProcessCapture(windowRect_)) {
            if (LowLatActive()) { backdropDirty_ = true; lastDockBackdrop_ = Now(); } else needsFrame_ = true;  // low latency: the UI layer stays cached
        }
        return 0;
    case kMsgGlassChanged:
        needsFrame_ = true;
        return 0;
    case kMsgStudioClosed:
        DLOG(L"Glass Studio closed; Dock keeps running");
        needsFrame_ = true;
        return 0;
    case dock::downloads::kProgressMessage:  // Fan opening/closing: cross-fade the Downloads icon
        needsFrame_ = true;
        {   // Test-only: LIQUID_DOCK_OPEN_SNAPSHOT=<file.bmp> captures the Dock once the Fan is fully open.
            static bool taken = false; wchar_t path[MAX_PATH]{};
            if (!taken && dock::downloads::OpenProgress() >= 0.999f && GetEnvironmentVariableW(L"LIQUID_DOCK_OPEN_SNAPSHOT", path, MAX_PATH)) { taken = true; renderer_.RequestSnapshot(path); }
        }
        return 0;
    case kDownloadsPreviewReady: {
        std::vector<std::uint32_t> next;{std::lock_guard<std::mutex> lock(downloadsPreview_->mutex);next=downloadsPreview_->pixels;}
        if(next!=downloadsPixels_){downloadsPixels_=std::move(next);for(auto& item:items_)if(item.isDownloads){ResolveItem(item);break;}BuildIcons();}
        return 0;}
    case WM_TIMER:
        if (wp == kTimerLaunchpadPrewarm) { KillTimer(hwnd_, kTimerLaunchpadPrewarm); dock::launchpad::Prewarm(); return 0; }
        if (wp == kTimerApplicationsPrewarm) { KillTimer(hwnd_, kTimerApplicationsPrewarm); if (!ApplicationsWindow(applicationsProcess_)) StartApplications(true); return 0; }
        if (wp == kTimerLaunchpadIcon) {
            if (dock::launchpad::IsOpen()) return 0;  // still fading out: try again on the next tick
            KillTimer(hwnd_, kTimerLaunchpadIcon);
            RunLaunchpadIconCommand();
            return 0;
        }
        if (wp == kTimerRunning) {
            {   // Game compatibility, games list: a listed program runs -> hidden, capture stopped (checked every 2 s, also
                // while suppressed, so the Dock returns within ~2 s after the program exits). Names only, no process opened.
                const std::wstring& game = gameWatch_.Poll(opt_.desktopSettingsPath, gameCompatSetting_);
                if (game.empty() == listedGame_ || game != listedGameName_) {
                    listedGame_ = !game.empty(); listedGameName_ = game;
                    DLOG(L"game compatibility: games list %ls%ls", listedGame_ ? L"program running: " : L"no listed program running", game.c_str());
                    SetGameSuppressed(menuBarGame_ || listedGame_);
                }
            }
            if (GameMode() && (++gameTick_ % 5) != 0) return 0;
            // Debug log only: where the once-a-second tick spends its time (averaged over 30 ticks).
            static double tickMs[9]{}; static int tickCount = 0; double tickAt = Now();
            auto tickMark = [&](int i) { const double n = Now(); tickMs[i] += (n - tickAt) * 1000.0; tickAt = n; };
            dock::minimize::Prune();
            if (RefreshAccessibility()) needsFrame_ = true;
            const bool darkBefore=liquid::theme::IsDark(appearanceMode_,systemDark_);
            if(++downloadsPoll_>=5){downloadsPoll_=0;RequestDownloadsPreview();}
            try {if(systemUi_){const auto c=systemUi_.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);const bool dark=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000;if(dark!=systemDark_){systemDark_=dark;if(appearanceMode_==0&&opaqueFallback_)needsFrame_=true;}}} catch(...) {}
            tickMark(0);
            if (SyncRunningApps()) needsFrame_ = true;
            tickMark(1);
            // Wi-Fi and battery every 5 s (a query takes ~25 ms of Wi-Fi and audio service calls); volume changes arrive at
            // once through the volume watcher (kStatusVolumeMsg).
            // While an animation runs, the status refresh (2-6 ms) and the Recycle Bin check (~1 ms) on this thread made
            // the animation miss a display frame every couple of seconds (measured 2026-11-20 at 240 Hz): they wait until
            // the animation settles, at most 5 s.
            const bool animatingNow = animating_ || glass_.continuousRender;
            if (!animatingNow) housekeepingDeferredSince_ = 0; else if (!housekeepingDeferredSince_) housekeepingDeferredSince_ = Now();
            const bool deferHousekeeping = animatingNow && Now() - housekeepingDeferredSince_ < 5.0;
            if (!deferHousekeeping) housekeepingDeferredSince_ = animatingNow ? Now() : 0;
            if (!statusCells_.empty() && !deferHousekeeping) RefreshStatus(++statusTick_ % 5 == 0);
            tickMark(2);
            docky::statushost::Maintain(hwnd_, kStatusTrayMsg, !menuBarEnabled_);  // tray / extension hosts while the menu bar is off
            tickMark(3);
            if (!deferHousekeeping && ++recyclePoll_ >= 3) { recyclePoll_ = 0; const int full = RecycleBinFull();
                for (auto& it : items_) if (it.isRecycleBin && full >= 0 && full != it.recycleFull) { ResolveItem(it); BuildIcons(); needsFrame_ = true; break; } }
            tickMark(4);
            const auto revision=liquid::settings::ReadRevision(opt_.desktopSettingsPath);
            if(revision!=settingsRevision_){settingsRevision_=revision;const auto current=liquid::settings::Load(opt_.desktopSettingsPath);
                docky::vblank::SetCaps(current.value.performance.animationFps,current.value.performance.captureFps);
                if(appearanceMode_!=current.value.appearance.mode){appearanceMode_=current.value.appearance.mode;if(opaqueFallback_)needsFrame_=true;}
                const bool mask=opt_.iconMaskOverride<0?current.value.dock.useMacIconMask:opt_.iconMaskOverride!=0;
                if(mask!=iconMaskEnabled_){iconMaskEnabled_=mask;BuildIcons();needsFrame_=true;DLOG(L"icon mask: %ls",mask?L"on":L"off");}
                if(current.value.dock.genieMinimize!=genie_)ApplyGenie(current.value.dock.genieMinimize);
                if(const float mag=1.0f+current.value.dock.magnification/100.0f;std::fabs(mag-model_.m.maxScale)>0.001f){
                    model_.m.maxScale=mag;ApplyWindowLayout(L"magnification");needsFrame_=true;StartAnimating();DLOG(L"magnification: %.2fx",mag);}
                gameCompatSetting_=current.value.dock.gameCompatibility;
                if(!lowLatForced_)SetLowLatency(current.value.dock.lowLatencyGlass,L"settings changed");
                if(current.value.dock.statusBatteryPercent!=statusPercent_){statusPercent_=current.value.dock.statusBatteryPercent;RefreshStatus(false);}
                if(current.value.dock.statusTileStyle!=statusTileStyle_){statusTileStyle_=current.value.dock.statusTileStyle;RefreshStatus(false);}
                if(current.value.dock.statusCells!=statusCells_||current.value.menuBar.enabled!=menuBarEnabled_)ApplyStatusCells(current.value.dock.statusCells,current.value.menuBar.enabled);
                if(current.value.dock.showDownloads!=showDownloads_||current.value.dock.showTrash!=showTrash_||current.value.dock.showDivider!=showDivider_||current.value.dock.tailOrder!=tailOrder_){
                    showDownloads_=current.value.dock.showDownloads;showTrash_=current.value.dock.showTrash;showDivider_=current.value.dock.showDivider;tailOrder_=current.value.dock.tailOrder;
                    if(!dragging_)RebuildTail(L"settings");}
                dock::downloads::SetFanOptions(current.value.dock.downloadsCount,current.value.dock.downloadsVertical,current.value.dock.downloadsPreview);  // used the next time the fan opens
                if(current.value.dock.autoHide!=autoHide_){autoHide_=current.value.dock.autoHide;leftAt_=model_.PointerActive()?-1.0:Now();StartAnimating();UpdateDockAppBar();DLOG(L"auto-hide: %ls",autoHide_?L"on":L"off");}
                DLOG(L"shared settings changed; status=%d",static_cast<int>(current.status));}
            tickMark(5);
            if(darkBefore!=liquid::theme::IsDark(appearanceMode_,systemDark_))RefreshThemeIcons();
            const auto packRevision=liquid::settings::ReadRevision(dockPackRoot_+L"\\active.txt");
            if(packRevision!=dockPackRevision_){dockPackRevision_=packRevision;LoadDockWorkshop();for(auto& item:items_)ResolveItem(item);BuildIcons();needsFrame_=true;}
            tickMark(6);
            CheckCustomIconFiles();
            tickMark(7);
            if (glass_.backdrop == BackdropKind::Live && backdrop_.CheckHealth()) needsFrame_ = true;
            tickMark(8);
            {   // Measurement mode: a single slow tick, step by step (it blocks this thread, so animation frames wait).
                static double prev[9]{}; double step[9]; double total = 0;
                for (int i = 0; i < 9; ++i) { step[i] = tickMs[i] - prev[i]; prev[i] = tickMs[i]; total += step[i]; }
                if (tickCount == 29) for (double& p : prev) p = 0;  // the sums restart after the 30-tick debug line
                if (latencyLogEvery_ > 0 && total > 3.0)
                    DLOG(L"slow tick %.2f ms: misc %.2f running %.2f status %.2f hosts %.2f recycle %.2f settings %.2f pack %.2f icons %.2f health %.2f",
                         total, step[0], step[1], step[2], step[3], step[4], step[5], step[6], step[7], step[8]);
            }
            if (++tickCount == 30) {
                DLOG_DEBUG(L"tick ms/30: misc %.1f running %.1f status %.1f hosts %.1f recycle %.1f settings %.1f pack %.1f icons %.1f health %.1f",
                           tickMs[0], tickMs[1], tickMs[2], tickMs[3], tickMs[4], tickMs[5], tickMs[6], tickMs[7], tickMs[8]);
                tickCount = 0; for (double& v : tickMs) v = 0;
            }
        } else if (wp == kTimerExit) {
            DLOG(L"--exit-after elapsed; closing");
            DestroyWindow(hwnd_);
        }
        return 0;
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED || !renderer_.Ready()) return 0;
        RECT client{};
        if (!GetClientRect(hwnd_, &client)) return 0;
        const UINT width = static_cast<UINT>(client.right - client.left);
        const UINT height = static_cast<UINT>(client.bottom - client.top);
        if (!width || !height) return 0;
        GetWindowRect(hwnd_, &windowRect_);
        if (width == pxW_ && height == pxH_) return 0;
        pxW_ = width; pxH_ = height;
        model_.SetWindowSize(pxW_ / scale_, pxH_ / scale_);
        const HRESULT hr = renderer_.Resize(pxW_, pxH_, dpi_);
        DLOG(L"window resized: %ux%u rect=(%ld,%ld)-(%ld,%ld) swapchain=0x%08X",
             width, height, windowRect_.left, windowRect_.top, windowRect_.right, windowRect_.bottom,
             static_cast<unsigned>(hr));
        if (FAILED(hr)) {
            DLOG(L"resize failed; recreating device");
            RecreateDevice();
        }
        SetFullRegion(false, true);
        needsFrame_ = true;
        return 0;
    }
    case WM_MOVE: {
        RECT moved{};
        if (GetWindowRect(hwnd_, &moved) && !EqualRect(&moved, &windowRect_)) {
            windowRect_ = moved;
            needsFrame_ = true;
            DLOG(L"window moved: (%ld,%ld)-(%ld,%ld)", moved.left, moved.top, moved.right, moved.bottom);
        }
        return 0;
    }
    case WM_DPICHANGED:
        dpi_ = static_cast<float>(HIWORD(wp));
        ApplyWindowLayout(L"WM_DPICHANGED");
        return 0;
    case WM_DISPLAYCHANGE:
        ApplyWindowLayout(L"WM_DISPLAYCHANGE");
        return 0;
    case kStatusReadyMsg: {  // a status query finished on its worker
        std::unique_ptr<liquid::status::Snapshot> snapshot(reinterpret_cast<liquid::status::Snapshot*>(lp));
        statusQuerying_ = false;
        if (snapshot) { statusSnapshot_ = *snapshot; RefreshStatus(false); }
        return 0;
    }
    case kStatusPanelMsg: RefreshStatus(true); StartAnimating(); return 0;  // a status panel changed something or closed
    case kStatusTrayMsg: UpdateTrayCells(); return 0;
    case kStatusVolumeMsg: if (!statusCells_.empty()) RefreshStatus(true); return 0;  // the system volume changed: show it now  // tray icons changed (the Hidden Icons grid is built when it opens)
    case kMinimizeMsg:
        MinimizedChanged(reinterpret_cast<HWND>(lp), wp != 0);
        return 0;
    case WM_APP + 80:  // Settings imported a new pinned.txt (MyDockFinder import)
        Reload();
        return 0;
    case kDockFullscreenMsg:
        SetFullscreenApp(wp != 0);
        menuBarGame_ = lp != 0;  // the Menu Bar's full-screen game detection
        SetGameSuppressed(menuBarGame_ || listedGame_);
        return 0;
    case dock::launchpad::kMsgLaunchpadAppMenu: {
        std::unique_ptr<dock::launchpad::AppMenuRequest> request(reinterpret_cast<dock::launchpad::AppMenuRequest*>(lp));
        if (request) ShowLaunchpadAppMenu(*request);
        return 0;
    }
    case dock::launchpad::kMsgLaunchpadCovers:
        dock::minimize::SetCovered(wp != 0);
        launchpadCovers_ = wp != 0;
        ShowWindow(hwnd_,(launchpadCovers_||gameSuppressed_)?SW_HIDE:SW_SHOWNOACTIVATE);
        RequestBackdrop(CaptureWanted(), MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY));
        if (!launchpadCovers_) needsFrame_ = true;  // a fresh backdrop for the Dock as Launchpad closes
        DLOG(L"dock: desktop capture %ls", launchpadCovers_ ? L"paused under Launchpad" : L"resumed");
        return 0;
    case kDockAppBarMsg:
        // ABN_FULLSCREENAPP is not trusted: Windows also sends it for the virtual-desktop switch animation (measured
        // 2026-11-10), which made the Dock slide away on every switch. The Menu Bar's checked decision (kDockFullscreenMsg) rules.
        if (wp == ABN_POSCHANGED) {  // another appbar / the taskbar changed: recompute (bounded, never a notify loop)
            static ULONGLONG last = 0; const ULONGLONG t = GetTickCount64();
            if (t - last > 500) { last = t; dockAppBarRect_ = {}; UpdateDockAppBar(); }
        }
        return 0;
    case WM_SETTINGCHANGE: {
        if (wp == SPI_SETWORKAREA) ApplyWindowLayout(L"work area changed");
        if (RefreshAccessibility()) needsFrame_ = true;
        BOOL animations = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && reducedMotion_ != !animations) {
            reducedMotion_ = !animations; model_.SetReducedMotion(reducedMotion_); StartAnimating(); needsFrame_ = true;
        }
        break;
    }
    case WM_SYSCOLORCHANGE:
        if (highContrast_) needsFrame_ = true;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        needsFrame_ = true;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        docky::statushost::Shutdown();
        RevokeDragDrop(hwnd_);
        dock::minimize::Shutdown(); ApplyGenie(false);  // restores Windows' minimize animation if docky paused it
        dockAppBarEnabled_ = false; UpdateDockAppBar();  // give the reserved band back while the window still exists
        liquid::popup::Close();
        dock::downloads::Close();
        {std::lock_guard<std::mutex> lock(downloadsPreview_->mutex);downloadsPreview_->window=nullptr;}
        DLOG(L"window destroyed");
        if (fgHook_) { UnhookWinEvent(fgHook_); fgHook_ = nullptr; }
        KillTimer(hwnd_, kTimerRunning);
        hwnd_ = nullptr;
        animating_ = needsFrame_ = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

static bool IsFinderSlot(const PinnedItem& item);
// The Dock's right-hand items (after the apps): status tiles, Downloads, minimized windows, Recycle Bin.
static bool IsTailItem(const PinnedItem& p) { return p.isDownloads || p.isRecycleBin || p.isMinimized || p.isStatusGroup; }
// Their key in dock.tailOrder (empty for minimized windows, which are placed by rule).
static std::wstring TailKey(const PinnedItem& p) {
    if (p.isDownloads) return L"downloads";
    if (p.isRecycleBin) return L"trash";
    if (p.isStatusGroup) return p.statusCells.size() == 1 && dock::statusgroup::StandaloneCell(p.statusCells[0]) ? L"status:" + p.statusCells[0] : std::wstring(L"status");
    return {};
}
void App::LoadItems() {
    items_.clear();
    if (!LoadPinnedConfig(configPath_, items_) || items_.empty()) {
        items_ = DefaultPinnedItems();
        if (SavePinnedConfig(configPath_, items_)) DLOG(L"config: seeded %zu default items", items_.size());
        else DLOG(L"config: failed to write %ls", configPath_.c_str());
    }
    // Sections (V0.4): [Launchpad + pinned + user separators] | [running, not pinned] | [Downloads].
    // Older configs saved the right-hand boundary as a trailing "---"; boundaries are synthesized now,
    // so trailing separators are dropped here (user separators between apps are kept as they are).
    while (!items_.empty() && items_.back().isSeparator) items_.pop_back();
    auto lp=std::find_if(items_.begin(),items_.end(),[](const PinnedItem& item){return item.isLaunchpad;});
    if(lp==items_.end()){  // Finder stays leftmost (the macOS Dock order): Launchpad goes right after it
        PinnedItem launchpad;launchpad.isLaunchpad=true;launchpad.target=L"launchpad";
        launchpad.labelOverride=L"Launchpad";
        const bool finderFirst=!items_.empty()&&!items_.front().isSeparator&&IsFinderSlot(items_.front());
        items_.insert(items_.begin()+(finderFirst?1:0),std::move(launchpad));
    }  // Launchpad keeps its saved place anywhere in the pinned section.
    for(auto& tile:MakeStatusItems())items_.push_back(std::move(tile));  // combined status, Clock, Calendar, then the half-size group
    if(showDownloads_){PinnedItem downloads;downloads.isDownloads=true;downloads.target=downloadsPath_;
        downloads.labelOverride=L"Downloads";items_.push_back(std::move(downloads));}
    if(showTrash_){PinnedItem bin;bin.isRecycleBin=true;bin.target=L"shell:RecycleBinFolder";bin.labelOverride=L"Recycle Bin";items_.push_back(std::move(bin));}
    for (HWND w : minimized_) { PinnedItem m; m.isMinimized = true; m.window = w; wchar_t title[256]{}; GetWindowTextW(w, title, 256); m.label = title;
        char id[40]{}; sprintf_s(id, "min:%p", static_cast<void*>(w)); m.stableId = id; items_.push_back(std::move(m)); }
    NormalizeSections();
    const double t0 = Now();
    for (PinnedItem& item : items_) ResolveItem(item);
    int withIcon = 0;
    for (const PinnedItem& item : items_) withIcon += item.pixels.empty() ? 0 : 1;
    DLOG(L"config: %zu items, %d with real icons, resolved in %.0f ms", items_.size(), withIcon, (Now() - t0) * 1000.0);
    for (const PinnedItem& item : items_)
        DLOG(L"  item '%ls' icon %ux%u exe '%ls'", item.label.c_str(), item.iconW, item.iconH, item.exeBase.c_str());
}

static bool IsFinderSlot(const PinnedItem& item){return item.isExplorer || _wcsicmp(item.target.c_str(),L"shell:MyComputerFolder")==0 || _wcsicmp(item.target.c_str(),L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")==0;}
void App::LoadDockWorkshop(){std::wstring error;docky::workshop::Pack next;bool enabled=false;
    if(!docky::workshop::Active(dockPackRoot_,next,enabled,error)){dockPackEnabled_=false;dockPack_={};return;}
    dockPackEnabled_=enabled;dockPack_=std::move(next);}
bool App::ApplyDockWorkshop(PinnedItem& item,const char* key){
    if(!dockPackEnabled_)return false;const auto it=dockPack_.icons.find(key);if(it==dockPack_.icons.end())return false;
    std::vector<uint32_t> px;UINT w=0,h=0;std::wstring error;
    if(!LoadCustomIconPixels(it->second,px,w,h,error))return false;item.pixels=std::move(px);item.iconW=w;item.iconH=h;DLOG(L"workshop: dock icon applied %hs",key);return true;}

bool App::ApplyPersonalDarkIcon(PinnedItem& item,const wchar_t* name){
    if(!liquid::theme::IsDark(appearanceMode_,systemDark_))return false;
    std::vector<uint32_t> pixels;UINT w=0,h=0;std::wstring error;
    if(!LoadCustomIconPixels(DefaultDataDir()+L"\\personal-icons\\"+name,pixels,w,h,error))return false;
    item.pixels=std::move(pixels);item.iconW=w;item.iconH=h;DLOG(L"personal dark icon: %ls",name);return true;
}
void App::RefreshThemeIcons(){for(auto& item:items_)if(item.isLaunchpad||IsFinderSlot(item))ResolveItem(item);BuildIcons();needsFrame_=true;}
void App::RequestDownloadsPreview(){
    auto shared=downloadsPreview_;const auto directory=downloadsPath_;
    {std::lock_guard<std::mutex> lock(shared->mutex);if(shared->busy||!shared->window)return;shared->busy=true;}
    std::thread([shared,directory]{HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);DWORD error=0;auto entries=dock::downloads::Scan(directory,error);dock::downloads::SortEntries(entries,dock::downloads::Sort::Date);
        std::wstring signature=std::to_wstring(error);for(size_t i=0;i<(std::min)(size_t(3),entries.size());++i){const auto& e=entries[i];signature+=L"|"+e.path+L":"+std::to_wstring(e.modified.dwHighDateTime)+L":"+std::to_wstring(e.modified.dwLowDateTime);}
        bool changed=false;{std::lock_guard<std::mutex> lock(shared->mutex);changed=signature!=shared->signature;}
        std::vector<dock::preview::Image> images;
        if(changed&&error==ERROR_SUCCESS)for(size_t i=0;i<(std::min)(size_t(3),entries.size());++i){dock::preview::Image image;std::wstring why;
            if(!LoadCustomIconPixels(entries[i].path,image.pixels,image.w,image.h,why))LoadShellIconPixels(entries[i].path,kIconPx,image.pixels,image.w,image.h);
            if(!image.pixels.empty())images.push_back(std::move(image));}
        auto preview=changed?dock::preview::Stack(images,kIconPx).pixels:std::vector<uint32_t>{};
        {std::lock_guard<std::mutex> lock(shared->mutex);if(changed){shared->signature=std::move(signature);shared->pixels=std::move(preview);if(shared->window)PostMessageW(shared->window,kDownloadsPreviewReady,0,0);}shared->busy=false;}
        if(SUCCEEDED(co))CoUninitialize();
    }).detach();
}
void App::ResolveItem(PinnedItem& item) {
    if (item.isMinimized) return;  // label and id are set when the tile is created
    if (item.isLaunchpad) { item.stableId="system:launchpad:v1";item.label=iconStore_.Label(item.stableId);if(item.label.empty())item.label=L"Launchpad";
        item.pixels=dock::launchpad::MakeDockIcon(kIconPx);item.iconW=item.iconH=kIconPx;ApplyDockWorkshop(item,"dock.launchpad");if(liquid::theme::IsDark(appearanceMode_,systemDark_)&&!ApplyDockWorkshop(item,"dock.launchpad.dark"))ApplyPersonalDarkIcon(item,L"launchpad-dark.png");return; }
    if (item.isRecycleBin) { item.label=L"Recycle Bin";item.stableId="system:recyclebin:v1";if(auto alias=iconStore_.Label(item.stableId);!alias.empty())item.label=std::move(alias);item.recycleFull=RecycleBinFull();
        item.pixels.clear();if(!LoadShellIconPixels(L"::{645FF040-5081-101B-9F08-00AA002F954E}",kIconPx,item.pixels,item.iconW,item.iconH))DLOG(L"recycle bin: icon unavailable");
        if(item.recycleFull>=0)ApplyDockWorkshop(item,item.recycleFull?"dock.trash.full":"dock.trash.empty");
        {dock::preview::Image source{item.pixels,item.iconW,item.iconH};auto fitted=dock::preview::Fit(source,kIconPx,1.0f);if(!fitted.pixels.empty()){item.pixels=std::move(fitted.pixels);item.iconW=fitted.w;item.iconH=fitted.h;}}
        DLOG(L"recycle bin: %ls",item.recycleFull==1?L"full":item.recycleFull==0?L"empty":L"unknown");return; }
    ResolvePinnedItem(item, kIconPx);
    if (item.isSeparator) return;
    if (item.isStatusGroup) { item.label=item.labelOverride.empty()?L"Status":item.labelOverride;item.pixels.clear();  // drawn live by the renderer
        statusState_.wifiIcon.reset(); statusState_.wifiW = statusState_.wifiH = 0;  // the Workshop pack's Wi-Fi glyph for the combined cell
        if (dockPackEnabled_) if (const auto it = dockPack_.icons.find("wifi.connected"); it != dockPack_.icons.end()) {
            std::vector<uint32_t> px; UINT w = 0, h = 0; std::wstring error;
            if (LoadCustomIconPixels(it->second, px, w, h, error)) { statusState_.wifiIcon = std::make_shared<const std::vector<uint32_t>>(std::move(px)); statusState_.wifiW = w; statusState_.wifiH = h; }
        }
        needsFrame_ = true; return; }
    if (item.isDownloads) { item.label=L"Downloads";item.stableId="system:downloads:v1";if(auto alias=iconStore_.Label(item.stableId);!alias.empty())item.label=std::move(alias);
        if(downloadsSettings_.display==dock::downloads::Display::Stack){item.pixels=downloadsPixels_;if(item.pixels.empty()){dock::preview::Image fallback{dock::downloads::MakeStackIcon(kIconPx),kIconPx,kIconPx};item.pixels=dock::preview::Fit(fallback,kIconPx,0.95f).pixels;}item.iconW=item.iconH=kIconPx;}
        return;
    }
    item.stableId = ApplicationItemId(item.target, item.args);
    if(auto alias=iconStore_.Label(item.stableId);!alias.empty())item.label=std::move(alias);
    const auto* path = iconStore_.Find(item.stableId);
    if (!path) {if(IsFinderSlot(item)){ApplyDockWorkshop(item,"dock.finder");if(liquid::theme::IsDark(appearanceMode_,systemDark_)&&!ApplyDockWorkshop(item,"dock.finder.dark"))ApplyPersonalDarkIcon(item,L"finder-dark.png");}return;}
    const IconFileStamp stamp=ReadIconFileStamp(*path);
    iconFileStamps_[item.stableId] = stamp;
    std::vector<uint32_t> pixels; UINT w=0,h=0; std::wstring error;
    if (stamp.exists && LoadCustomIconPixels(*path,pixels,w,h,error) && ReadIconFileStamp(*path)==stamp) {
        item.pixels=std::move(pixels); item.iconW=w; item.iconH=h;
        DLOG(L"custom icon: loaded %ux%u",w,h);
    } else {
        DLOG(L"custom icon: unavailable; using default (%ls)",error.c_str());
        if(IsFinderSlot(item)){ApplyDockWorkshop(item,"dock.finder");if(liquid::theme::IsDark(appearanceMode_,systemDark_)&&!ApplyDockWorkshop(item,"dock.finder.dark"))ApplyPersonalDarkIcon(item,L"finder-dark.png");}
    }
}

// Icon editor V0.2: edits a temporary state; Apply persists through CustomIconStore (image override +
// per-icon edit) and rebuilds every Dock item with that stable id (pinned and running alike).
static std::wstring PickIconImage(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    const COMDLG_FILTERSPEC filters[] = {{L"Images (*.png;*.ico;*.svg)", L"*.png;*.ico;*.svg"}, {L"PNG (*.png)", L"*.png"}, {L"Icon (*.ico)", L"*.ico"}, {L"SVG (*.svg)", L"*.svg"}};
    FILEOPENDIALOGOPTIONS opts{};
    if (FAILED(dialog->GetOptions(&opts))) return {};
    dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> selected;
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(dialog->GetResult(&selected)) && SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}
void App::OpenIconEditor(int index) {
    if(index<0 || static_cast<size_t>(index)>=items_.size() || items_[index].isSeparator || items_[index].isDownloads || items_[index].isRecycleBin || items_[index].isLaunchpad || items_[index].isStatusGroup)return;
    OpenIconEditorFor(items_[index]);
}
// Also used for Launchpad apps that are not in the Dock: the edit is stored under the same stable id.
void App::OpenIconEditorFor(const PinnedItem& target) {
    const std::string id=target.stableId;
    if(!iconStore_.Available() || id.empty()) {
        const std::wstring detail=iconStoreError_.empty()?L"Custom icon settings are unavailable.":iconStoreError_;
        MessageBoxW(hwnd_,detail.c_str(),L"Edit Icon",MB_OK|MB_ICONERROR);return;
    }
    dock::iconedit::State st;
    st.source=target.pixels;st.w=target.iconW;st.h=target.iconH;st.globalMask=iconMaskEnabled_;
    if(const IconEdit* e=iconStore_.FindEdit(id))st.edit=*e;
    const PinnedItem originalItem=target;
    dock::iconedit::Callbacks cb;
    cb.render=[dark=liquid::theme::IsDark(appearanceMode_,systemDark_)](const dock::iconedit::State& s,UINT& side){
        auto px=ApplyIconEdit(s.source,s.w,s.h,s.edit);side=(std::max)(s.w,s.h);
        if(!(s.edit.mask<0?s.globalMask:s.edit.mask!=0))return px;
        unsigned ms=0;auto masked=MakeMacStyleIcon(px,s.w,s.h,true,&ms,dark);if(ms)side=ms;return masked;};
    cb.changeImage=[this](dock::iconedit::State& s){
        const std::wstring path=PickIconImage(hwnd_);if(path.empty())return false;
        std::vector<uint32_t> px;UINT w=0,h=0;std::wstring error;
        if(!LoadCustomIconPixels(path,px,w,h,error)){MessageBoxW(hwnd_,error.c_str(),L"Edit Icon",MB_OK|MB_ICONERROR);return false;}
        s.source=std::move(px);s.w=w;s.h=h;s.newImage=path;return true;};
    cb.resetToOriginal=[this,originalItem](dock::iconedit::State& s){
        PinnedItem original=originalItem;ResolvePinnedItem(original,kIconPx);if(IsFinderSlot(original))ApplyDockWorkshop(original,"dock.finder");
        if(!original.pixels.empty()){s.source=std::move(original.pixels);s.w=original.iconW;s.h=original.iconH;}};
    cb.apply=[this,id](const dock::iconedit::State& s){
        std::wstring error;bool ok;
        if(s.resetAll&&s.newImage.empty()&&s.edit.IsDefault())ok=iconStore_.Remove(id,error);  // full reset: image override + all edits
        else{const std::wstring none;const std::wstring* image=!s.newImage.empty()?&s.newImage:(s.resetAll?&none:nullptr);
            ok=iconStore_.SetEdit(id,s.edit,image,error);}
        if(!ok){MessageBoxW(hwnd_,error.c_str(),L"Edit Icon",MB_OK|MB_ICONERROR);return false;}
        for(auto& item:items_)if(!item.isSeparator&&item.stableId==id)ResolveItem(item);
        BuildIcons();needsFrame_=true;StartAnimating();dock::launchpad::InvalidateIcon(id);
        DLOG(L"icon editor: applied scale=%.2f offset=%.2f,%.2f mask=%d shadow=%d image=%d reset=%d",s.edit.scale,s.edit.offsetX,s.edit.offsetY,s.edit.mask,s.edit.shadow,s.newImage.empty()?0:1,s.resetAll?1:0);
        return true;};
    if(!dock::iconedit::Open(hwnd_,target.label,std::move(st),std::move(cb)))MessageBoxW(hwnd_,L"Could not open the icon editor.",L"Edit Icon",MB_OK|MB_ICONERROR);
}

void App::ChangeIcon(int index, bool reset) {
    if(index<0 || static_cast<size_t>(index)>=items_.size() || items_[index].isSeparator || items_[index].isDownloads || items_[index].isRecycleBin || items_[index].isLaunchpad || items_[index].isStatusGroup)return;
    ChangeIconFor(items_[index], reset);
}
// target may be a Dock item or a Launchpad app outside the Dock (the same stable id either way). Copied: the
// file dialog runs a message loop, during which items_ may change.
void App::ChangeIconFor(const PinnedItem& targetRef, bool reset) {
    const PinnedItem target=targetRef;
    const auto id=target.stableId;
    if(!iconStore_.Available() || id.empty()) {
        const std::wstring detail=iconStoreError_.empty()?L"Custom icon settings are unavailable.":iconStoreError_;
        MessageBoxW(hwnd_,detail.c_str(),L"Custom icon",MB_OK|MB_ICONERROR);return;
    }
    std::vector<uint32_t> pixels; UINT w=0,h=0; std::wstring path,error; IconFileStamp selectedStamp;
    if(reset) {
        PinnedItem original=target; ResolvePinnedItem(original,kIconPx);if(IsFinderSlot(original))ApplyDockWorkshop(original,"dock.finder");
        pixels=std::move(original.pixels);w=original.iconW;h=original.iconH;
    } else {
        ComPtr<IFileOpenDialog> dialog;HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
        if(SUCCEEDED(hr)) {
            const COMDLG_FILTERSPEC filters[]={{L"Images (*.png;*.ico;*.svg)",L"*.png;*.ico;*.svg"},{L"PNG (*.png)",L"*.png"},{L"Icon (*.ico)",L"*.ico"},{L"SVG (*.svg)",L"*.svg"}};
            FILEOPENDIALOGOPTIONS opts{};hr=dialog->GetOptions(&opts);
            if(SUCCEEDED(hr))hr=dialog->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
            if(SUCCEEDED(hr))hr=dialog->SetFileTypes(ARRAYSIZE(filters),filters);
            if(SUCCEEDED(hr))hr=dialog->Show(hwnd_);
            if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;
            ComPtr<IShellItem> selected;if(SUCCEEDED(hr))hr=dialog->GetResult(&selected);
            PWSTR raw=nullptr;if(SUCCEEDED(hr))hr=selected->GetDisplayName(SIGDN_FILESYSPATH,&raw);
            if(SUCCEEDED(hr)&&raw){path=raw;CoTaskMemFree(raw);}else if(raw)CoTaskMemFree(raw);
        }
        if(FAILED(hr)||path.empty()){MessageBoxW(hwnd_,L"Could not open the image chooser.",L"Custom icon",MB_OK|MB_ICONERROR);return;}
        selectedStamp=ReadIconFileStamp(path);
        if(!selectedStamp.exists){MessageBoxW(hwnd_,L"The image is no longer available.",L"Custom icon",MB_OK|MB_ICONERROR);return;}
        if(!LoadCustomIconPixels(path,pixels,w,h,error)){MessageBoxW(hwnd_,error.c_str(),L"Custom icon",MB_OK|MB_ICONERROR);return;}
        if(!(ReadIconFileStamp(path)==selectedStamp)){MessageBoxW(hwnd_,L"The image changed while loading. Please choose it again.",L"Custom icon",MB_OK|MB_ICONERROR);return;}
    }
    ComPtr<ID2D1Bitmap1> icon,shadow;float shadowPad=0;
    if(!pixels.empty()) {
        const HRESULT hr=CreateDockIcon(pixels,w,h,icon,shadow,shadowPad);
        if(FAILED(hr)){MessageBoxW(hwnd_,L"Could not create the icon texture.",L"Custom icon",MB_OK|MB_ICONERROR);return;}
    }
    const bool saved=reset?iconStore_.Remove(id,error):iconStore_.Set(id,path,error);
    if(!saved){MessageBoxW(hwnd_,error.c_str(),L"Custom icon",MB_OK|MB_ICONERROR);return;}
    for(size_t i=0;i<items_.size();++i)if(!items_[i].isSeparator && items_[i].stableId==id){
        items_[i].pixels=pixels;items_[i].iconW=w;items_[i].iconH=h;
        if(i<iconBmp_.size()){iconBmp_[i]=icon;shadowBmp_[i]=shadow;shadowPad_[i]=shadowPad;}
    }
    if(reset)iconFileStamps_.erase(id);else iconFileStamps_[id]=selectedStamp;
    dock::launchpad::InvalidateIcon(id);
    DLOG(L"custom icon: %ls",reset?L"reset to default":L"updated");needsFrame_=true;
}

void App::CheckCustomIconFiles() {
    std::set<std::string> checked;
    for(size_t index=0;index<items_.size();++index) {
        auto& item=items_[index];if(item.isSeparator||item.isDownloads||item.isRecycleBin||item.isLaunchpad||item.stableId.empty()||!checked.insert(item.stableId).second)continue;
        const auto* path=iconStore_.Find(item.stableId);if(!path)continue;
        const IconFileStamp now=ReadIconFileStamp(*path);auto prev=iconFileStamps_.find(item.stableId);
        if(prev==iconFileStamps_.end()){iconFileStamps_[item.stableId]=now;continue;}
        if(prev->second==now)continue;
        std::vector<uint32_t> pixels;UINT w=0,h=0;std::wstring error;
        const bool custom=now.exists && LoadCustomIconPixels(*path,pixels,w,h,error);
        if(!custom){PinnedItem original=item;ResolvePinnedItem(original,kIconPx);if(IsFinderSlot(original))ApplyDockWorkshop(original,"dock.finder");pixels=std::move(original.pixels);w=original.iconW;h=original.iconH;}
        if(!(ReadIconFileStamp(*path)==now))continue; // Retry if the file changed during decoding.
        ComPtr<ID2D1Bitmap1> icon,shadow;float shadowPad=0;
        if(!pixels.empty() && FAILED(CreateDockIcon(pixels,w,h,icon,shadow,shadowPad))) {DLOG(L"custom icon: refresh texture failed");continue;}
        const auto id=item.stableId;
        for(size_t i=0;i<items_.size();++i)if(!items_[i].isSeparator&&items_[i].stableId==id){items_[i].pixels=pixels;items_[i].iconW=w;items_[i].iconH=h;if(i<iconBmp_.size()){iconBmp_[i]=icon;shadowBmp_[i]=shadow;shadowPad_[i]=shadowPad;}}
        prev->second=now;DLOG(L"custom icon: file %ls",custom?L"reloaded":L"unavailable; default restored");needsFrame_=true;
    }
}

HRESULT App::CreateDockIcon(const std::vector<uint32_t>& pixels,UINT w,UINT h,ComPtr<ID2D1Bitmap1>& icon,ComPtr<ID2D1Bitmap1>& shadow,float& pad,int maskOverride) {
    if(maskOverride<0?!iconMaskEnabled_:maskOverride==0)return renderer_.CreateIcon(pixels,w,h,icon,shadow,pad);
    unsigned maskedSide=0;
    auto masked=MakeMacStyleIcon(pixels,w,h,true,&maskedSide,liquid::theme::IsDark(appearanceMode_,systemDark_));
    return renderer_.CreateIcon(masked,maskedSide?maskedSide:w,maskedSide?maskedSide:h,icon,shadow,pad);
}
void App::BuildIcons() {
    iconBmp_.assign(items_.size(), nullptr);
    shadowBmp_.assign(items_.size(), nullptr);
    shadowPad_.assign(items_.size(), 0.0f);
    if (!renderer_.Ready()) return;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].pixels.empty()) continue;
        const IconEdit* edit = items_[i].stableId.empty() ? nullptr : iconStore_.FindEdit(items_[i].stableId);
        // Same visible body size for every icon (Finder, Workshop and shell icons alike).
        const auto normalized=NormalizeIconBody(items_[i].pixels,items_[i].iconW,items_[i].iconH);
        const auto& input=normalized.empty()?items_[i].pixels:normalized;
        HRESULT hr;
        if (edit) {  // Saved user edits continue to apply after body normalization.
            hr = CreateDockIcon(ApplyIconEdit(input, items_[i].iconW, items_[i].iconH, *edit), items_[i].iconW, items_[i].iconH, iconBmp_[i],
                                shadowBmp_[i], shadowPad_[i], edit->mask);
            if (edit->shadow == 0) shadowBmp_[i].Reset();
        } else hr = CreateDockIcon(input, items_[i].iconW, items_[i].iconH, iconBmp_[i],
                                                shadowBmp_[i], shadowPad_[i], (items_[i].isRecycleBin || items_[i].isDownloads) ? 0 : -1);
        if (FAILED(hr)) DLOG(L"icon upload failed 0x%08X for %ls", hr, items_[i].label.c_str());
    }
    {   ComPtr<ID2D1Bitmap1> unusedShadow; float unusedPad = 0;
        downloadsOpenBmp_.Reset();
        const bool mask = iconMaskEnabled_; iconMaskEnabled_ = false;  // keep it translucent: no neutral backing tile
        if (FAILED(CreateDockIcon(dock::downloads::MakeOpenStackIcon(kIconPx), kIconPx, kIconPx, downloadsOpenBmp_, unusedShadow, unusedPad))) downloadsOpenBmp_.Reset();
        iconMaskEnabled_ = mask; }
    needsFrame_ = true;
}

// ---- Status group (status_group.h) ----------------------------------------------------------------------------
// Cells come from shared settings (dock.statusCells). Turning the menu bar off with no cells gives the group the default
// cells, so Control Center, Wi-Fi and the rest always keep a way in.
// The Dock tiles for the status cells, in their order: each whole-icon cell (combined status, Clock, Calendar) is a tile
// of its own; the half-size cells share one group tile placed where the first of them is.
std::vector<PinnedItem> App::MakeStatusItems() const {
    std::vector<PinnedItem> out; std::vector<std::wstring> rest; int groupAt = -1;
    for (const auto& id : statusCells_) {
        if (dock::statusgroup::StandaloneCell(id)) {
            PinnedItem tile; tile.isStatusGroup = true; tile.statusCells = {id}; tile.target = L"status:" + id; tile.labelOverride = dock::statusgroup::CellLabel(id);
            out.push_back(std::move(tile));
        } else { if (groupAt < 0) groupAt = static_cast<int>(out.size()); rest.push_back(id); }
    }
    if (!rest.empty()) {
        PinnedItem group; group.isStatusGroup = true; group.statusCells = std::move(rest); group.target = L"status"; group.labelOverride = L"Status";
        out.insert(out.begin() + groupAt, std::move(group));
    }
    return out;
}
void App::ApplyStatusCells(const std::vector<std::wstring>& cells, bool menuBarEnabled) {
    std::vector<std::wstring> next = cells;
    if (!menuBarEnabled && next.empty()) {
        next = liquid::settings::Dock{}.statusCells;
        liquid::settings::WriteStatusCells(opt_.desktopSettingsPath, next);
        DLOG(L"status group: menu bar off and no cells; default cells restored");
    }
    menuBarEnabled_ = menuBarEnabled;
    statusCells_ = std::move(next);
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const PinnedItem& p) { return p.isStatusGroup; }), items_.end());
    for (auto& tile : MakeStatusItems()) { ResolveItem(tile); items_.push_back(std::move(tile)); }
    NormalizeSections();
    pressed_ = -1;
    ApplyWindowLayout(L"status group");
    BuildIcons(); UpdateTrayCells(); RefreshStatus(true); StartAnimating(); needsFrame_ = true;
    DLOG(L"status group: %zu cells, menu bar %ls", statusCells_.size(), menuBarEnabled_ ? L"on" : L"off");
}

// Tray icons kept in the Dock: their live pictures and names from the tray host (only while the Dock hosts the tray, i.e.
// the menu bar is off). A buffer is reused while the app's icon is unchanged, so the cell's bitmap is not rebuilt.
void App::UpdateTrayCells() {
    using Image = dock::statusgroup::State::TrayImage;
    std::map<std::wstring, Image> next; std::vector<Image> preview;
    const bool kept = std::any_of(statusCells_.begin(), statusCells_.end(), [](const std::wstring& id) { return dock::statusgroup::TrayCell(id); });
    const bool folder = std::find(statusCells_.begin(), statusCells_.end(), std::wstring(L"tray")) != statusCells_.end();
    // Reuse the previous buffer while an icon is unchanged (its bitmap stays cached).
    auto reuse = [&](const docky::tray::Icon& icon, Image& image) {
        auto same = [&](const Image& old) { return old.pixels && old.w == icon.w && old.h == icon.h && *old.pixels == icon.pixels; };
        if (const auto old = statusState_.trayIcons.find(icon.key); old != statusState_.trayIcons.end() && same(old->second)) { image.pixels = old->second.pixels; return; }
        for (const auto& old : statusState_.trayPreview) if (same(old)) { image.pixels = old.pixels; return; }
        if (!icon.pixels.empty()) image.pixels = std::make_shared<const std::vector<uint32_t>>(icon.pixels);
    };
    if (kept || folder) for (const auto& icon : docky::statushost::TrayIcons()) {
        if (icon.hidden) continue;
        const bool isKept = std::find(statusCells_.begin(), statusCells_.end(), icon.key) != statusCells_.end();
        if (!isKept && (!folder || preview.size() >= 9)) continue;
        if (isKept && next.count(icon.key)) continue;
        Image image; image.name = docky::tray::DisplayName(icon); image.w = icon.w; image.h = icon.h; reuse(icon, image);
        if (isKept) next.emplace(icon.key, std::move(image)); else preview.push_back(std::move(image));  // the folder previews the hidden ones
    }
    if (next != statusState_.trayIcons || preview != statusState_.trayPreview) {
        statusState_.trayIcons = std::move(next); statusState_.trayPreview = std::move(preview); needsFrame_ = true; StartAnimating();
    }
}
std::wstring App::StatusCellLabel(const std::wstring& id) const {
    if (dock::statusgroup::TrayCell(id)) if (const auto it = statusState_.trayIcons.find(id); it != statusState_.trayIcons.end() && !it->second.name.empty()) return it->second.name;
    return dock::statusgroup::CellLabel(id);
}
RECT App::StatusCellScreenRect(int index, int cell) const {
    if (index < 0 || static_cast<size_t>(index) >= frame_.items.size() || static_cast<size_t>(index) >= items_.size()) return RECT{};
    const auto& g = frame_.items[static_cast<size_t>(index)];
    const auto& cells = items_[static_cast<size_t>(index)].statusCells;
    if (cell < 0 || static_cast<size_t>(cell) >= cells.size()) return RECT{};
    const D2D1_RECT_F r = dock::statusgroup::CellRect(D2D1::RectF(g.left, g.top, g.left + g.width, g.top + g.size), cells, static_cast<size_t>(cell));
    return RECT{windowRect_.left + static_cast<LONG>(r.left * scale_), windowRect_.top + static_cast<LONG>(r.top * scale_),
                windowRect_.left + static_cast<LONG>(r.right * scale_), windowRect_.top + static_cast<LONG>(r.bottom * scale_)};
}

// Time and input layout every second; system status (Wi-Fi, battery, volume) from a worker every other second.
void App::RefreshStatus(bool query) {
    dock::statusgroup::State next = statusState_;
    SYSTEMTIME now{}; GetLocalTime(&now);
    // 24-hour or 12-hour as the user's Windows time format says (no AM/PM: the digits alone, as on the Lock Screen).
    wchar_t pattern[80]{}; GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, pattern, 80);
    const bool h24 = wcschr(pattern, L'H') != nullptr;
    wchar_t time[32]{}; GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &now, h24 ? L"H:mm" : L"h:mm", time, 32);
    next.time = time;
    next.hour = now.wHour; next.minute = now.wMinute; next.second = now.wSecond; next.day = now.wDay;  // Clock and Calendar tiles (live)
    wchar_t weekday[32]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"ddd", weekday, 32, nullptr); next.weekday = weekday;
    next.tileStyle = statusTileStyle_;
    statusLayout_ = docky::statuspopup::ForegroundLayout(statusLayout_, statusInputTarget_);
    next.inputBadge = docky::statuspopup::InputBadge(statusLayout_);
    const auto& s = statusSnapshot_;
    next.batteryPresent = s.batteryPresent; next.charging = s.charging; next.battery = s.batteryPercent;
    next.plugged = s.batteryPresent && (s.charging || s.powerSource == liquid::status::PowerSource::AC);  // no bolt on a desktop PC
    next.showPercent = statusPercent_;
    { SYSTEM_POWER_STATUS power{}; next.saver = GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1; }  // Battery Saver
    // Black or white glyphs by what is actually behind the group, like the menu bar (SampleBarBackdrop): the median
    // luminance of the screen under the group's body (sampled every 2 s) turns black only when it stays at 205 or brighter
    // for two samples in a row (about 4 s), and back to white once it drops to 175 or below (no flicker in between). The Dock window keeps itself out of captures while it uses the live backdrop, so the capture shows
    // what is behind it; otherwise (or in high contrast) the Dock's light / dark theme decides as before.
    {
        HIGHCONTRASTW hc{sizeof(hc)};
        const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
        DWORD affinity = 0;
        const bool excluded = hwnd_ && GetWindowDisplayAffinity(hwnd_, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE;
        // Sample under the first tile with glyphs on the Dock glass (the Clock and Calendar carry their own plates).
        int group = -1; for (size_t i = 0; i < items_.size() && i < frame_.items.size(); ++i)
            if (items_[i].isStatusGroup && !(items_[i].statusCells.size() == 1 && dock::statusgroup::PlateCell(items_[i].statusCells[0]))) { group = static_cast<int>(i); break; }
        { static int logged = -1; const int state = (contrast ? 1 : 0) | (excluded ? 2 : 0) | (group >= 0 ? 4 : 0);
          if (state != logged) { logged = state; DLOG(L"status group: adaptive ink contrast=%d excluded-from-capture=%d (affinity %lu) group=%d", contrast ? 1 : 0, excluded ? 1 : 0, affinity, group); } }
        if (contrast || !excluded || group < 0) { statusLight_ = false; statusLightVotes_ = 0; }
        else if (Now() - statusInkAt_ >= 1.9) {  // a screen read every 2 s (each one makes the compositor copy pixels back)
            statusInkAt_ = Now();
            const auto& g = frame_.items[static_cast<size_t>(group)];
            const RECT r{windowRect_.left + static_cast<LONG>(g.left * scale_), windowRect_.top + static_cast<LONG>(g.top * scale_),
                         windowRect_.left + static_cast<LONG>((g.left + g.width) * scale_), windowRect_.top + static_cast<LONG>((g.top + g.size) * scale_)};
            const int w = r.right - r.left, h = r.bottom - r.top;
            if (w > 0 && h > 0 && w < 4096 && h < 4096) if (HDC screen = GetDC(nullptr)) {
                HDC mem = CreateCompatibleDC(screen);
                BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr; HBITMAP bmp = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
                unsigned hist[256]{}; size_t n = 0;
                if (bmp && bits) {
                    HGDIOBJ old = SelectObject(mem, bmp);
                    if (BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY)) {
                        const auto* px = static_cast<const uint32_t*>(bits);
                        for (int y = 0; y < h; y += 2) for (int x = 0; x < w; x += 2) {
                            const uint32_t v = px[static_cast<size_t>(y) * w + x];
                            ++hist[(std::min)(255u, (((v >> 16) & 255u) * 54u + ((v >> 8) & 255u) * 183u + (v & 255u) * 19u) >> 8)]; ++n;
                        }
                    }
                    SelectObject(mem, old);
                }
                if (bmp) DeleteObject(bmp); if (mem) DeleteDC(mem); ReleaseDC(nullptr, screen);
                if (n) {
                    size_t acc = 0; int median = 0; for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc * 2 >= n) { median = i; break; } }
                    const bool wants = statusLight_ ? median <= 175 : median >= 205;  // a change is asked for
                    statusLightVotes_ = wants ? statusLightVotes_ + 1 : 0;
                    if (statusLightVotes_ >= (statusLight_ ? 1 : 2)) {  // samples 2 s apart: black after ~4 s, white within ~2 s
                        statusLight_ = !statusLight_; statusLightVotes_ = 0;
                        DLOG(L"status group: backdrop median luminance %d -> %ls glyphs", median, statusLight_ ? L"black" : L"white");
                    }
                }
            }
        }
    }
    next.dark = !statusLight_ && liquid::theme::IsDark(appearanceMode_, systemDark_);  // black glyphs on a bright backdrop or a light Dock
    next.wifi = s.wifi == liquid::status::Wifi::Connected ? 2 : s.wifi == liquid::status::Wifi::Disconnected ? 1 : 0;
    next.audio = s.audioAvailable; next.muted = s.muted; next.volume = s.volumePercent;
    if (!(next == statusState_)) { statusState_ = next; needsFrame_ = true; }
    if (query && !statusQuerying_.exchange(true)) {
        const HWND target = hwnd_;
        std::thread([target] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const double w0 = Now();
            auto* snapshot = new liquid::status::Snapshot(liquid::status::Query(false));  // no nearby-network list: the tiles do not show it
            DLOG_DEBUG(L"status query: %.1f ms", (Now() - w0) * 1000.0);
            if (SUCCEEDED(co)) CoUninitialize();
            if (!PostMessageW(target, kStatusReadyMsg, 0, reinterpret_cast<LPARAM>(snapshot))) delete snapshot;
        }).detach();
    }
}

// A click on a cell opens its panel above that cell (panels share the menu bar popups' Liquid Glass settings).
void App::OpenStatusCell(int index, int cell) {
    if (index < 0 || static_cast<size_t>(index) >= frame_.items.size() || static_cast<size_t>(index) >= items_.size()) return;
    const std::vector<std::wstring> cells = items_[static_cast<size_t>(index)].statusCells;
    if (cell < 0 || static_cast<size_t>(cell) >= cells.size()) return;
    const auto& g = frame_.items[static_cast<size_t>(index)];
    const D2D1_RECT_F r = dock::statusgroup::CellRect(D2D1::RectF(g.left, g.top, g.left + g.width, g.top + g.size), cells, static_cast<size_t>(cell));
    // The anchor spans the cell horizontally and starts at the top of the plate (panels open above the whole Dock).
    const RECT anchor{windowRect_.left + static_cast<LONG>(r.left * scale_), windowRect_.top + static_cast<LONG>(std::min(r.top, frame_.dockTop) * scale_),
                      windowRect_.left + static_cast<LONG>(r.right * scale_), windowRect_.top + static_cast<LONG>(r.bottom * scale_)};
    const std::wstring id = cells[static_cast<size_t>(cell)];
    DLOG(L"status group: open %ls", id.c_str());
    liquid::cc::SetOpenAbove(true);
    const POINT above{anchor.left, anchor.top - static_cast<LONG>(8 * scale_)};
    auto closed = [this] { needsFrame_ = true; };
    if (id == L"control" || id == L"combo") liquid::cc::Toggle(anchor, hwnd_, kStatusPanelMsg);
    else if (id == L"wifi") liquid::cc::ToggleWifi(anchor, hwnd_, kStatusPanelMsg);
    else if (id == L"bluetooth") liquid::cc::ToggleBluetooth(anchor, hwnd_, kStatusPanelMsg);
    else if (id == L"sound") liquid::cc::ToggleSound(anchor, hwnd_, kStatusPanelMsg);
    else if (id == L"battery") docky::statuspopup::ShowBatteryMenu(hwnd_, above, true, statusSnapshot_, [this, anchor] { liquid::cc::SetOpenAbove(true); liquid::cc::Toggle(anchor, hwnd_, kStatusPanelMsg); }, closed);
    else if (id == L"input") docky::statuspopup::ShowInputMenu(hwnd_, above, true, statusLayout_, statusInputTarget_, closed);
    else if (id == L"clock" || id == L"analog" || id == L"date" || id == L"digital") {
        SYSTEMTIME now{}; GetLocalTime(&now); wchar_t date[128]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &now, nullptr, date, 128, nullptr);
        std::vector<liquid::menu::MenuItem> items{{date, liquid::menu::ItemKind::Command, false, false, {}},
                                                 {L"", liquid::menu::ItemKind::Separator, false, false, {}},
                                                 {L"Open Clock", liquid::menu::ItemKind::Command, true, false, {}, 1},
                                                 {L"Date & Time Settings…", liquid::menu::ItemKind::Command, true, false, {}, 2}};
        liquid::popup::ShowAbove(hwnd_, above, std::move(items), [](const liquid::menu::MenuItem& item) {
            ShellExecuteW(nullptr, L"open", item.commandId == 1 ? L"ms-clock:" : L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL); }, closed);
    } else if (id == L"tray") {  // hosted here only while the menu bar is off
        docky::statushost::ShowHiddenIcons(hwnd_, above, statusCells_, [this](const std::wstring& cellId) {
            DLOG(L"status group: %ls kept in the Dock toggled", cellId.c_str()); ToggleStatusCell(cellId); });
    } else if (dock::statusgroup::TrayCell(id)) {  // a tray icon kept in the Dock: click it like the real tray icon
        if (!docky::statushost::ClickTray(id, false, StatusCellScreenRect(index, cell))) {
            const std::wstring note = docky::statushost::Active() ? StatusCellLabel(id) + L" is not running" : std::wstring(L"Shown in the menu bar while it is on");
            liquid::popup::ShowAbove(hwnd_, above, {{note, liquid::menu::ItemKind::Command, false, false, {}}, {L"", liquid::menu::ItemKind::Separator, false, false, {}},
                                                    {L"Remove from Dock", liquid::menu::ItemKind::Command, true, false, {}, 1}},
                                     [this, id](const liquid::menu::MenuItem& row) { if (row.commandId == 1) ToggleStatusCell(id); }, closed);
        }
    } else if (id.rfind(L"ext:", 0) == 0) {
        if (docky::statushost::Active()) docky::statushost::ActivateExtension(hwnd_, id.substr(4), above);
        else liquid::popup::ShowAbove(hwnd_, above, {{L"Shown in the menu bar while it is on", liquid::menu::ItemKind::Command, false, false, {}}}, [](const liquid::menu::MenuItem&) {}, closed);
    }
    StartAnimating();
}

// Right-click: which cells the group shows, whether the menu bar runs, and DockOver Settings.
void App::ShowStatusGroupMenu(int index) {
    using liquid::menu::ItemKind;
    const auto& kIds = kStatusMenuIds;
    std::vector<liquid::menu::MenuItem> items;
    for (UINT i = 0; i < ARRAYSIZE(kIds); ++i) {
        const bool on = std::find(statusCells_.begin(), statusCells_.end(), kIds[i]) != statusCells_.end();
        items.push_back({i == 0 ? std::wstring(L"Status (Battery, Wi-Fi, Volume)") : dock::statusgroup::CellLabel(kIds[i]), ItemKind::Command, true, on, {}, 900 + i});
        if (i + 1 == kStatusTileCount) items.push_back({L"", ItemKind::Separator, false, false, {}});  // whole-icon tiles above, half-size cells below
    }
    std::vector<std::wstring> extIds;
    for (const auto& item : docky::statushost::Items()) if (extIds.size() < 40) extIds.push_back(L"ext:" + item.id);
    for (const auto& id : statusCells_) if (id.rfind(L"ext:", 0) == 0 && std::find(extIds.begin(), extIds.end(), id) == extIds.end() && extIds.size() < 40) extIds.push_back(id);
    for (const auto& id : statusCells_) if (dock::statusgroup::TrayCell(id) && extIds.size() < 40) extIds.push_back(id);  // kept tray icons (uncheck to remove)
    if (!extIds.empty()) items.push_back({L"", ItemKind::Separator, false, false, {}});
    for (UINT i = 0; i < extIds.size(); ++i)
        items.push_back({StatusCellLabel(extIds[i]), ItemKind::Command, true, std::find(statusCells_.begin(), statusCells_.end(), extIds[i]) != statusCells_.end(), {}, 1000 + i});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({L"Show Battery Percentage", ItemKind::Command, true, statusPercent_, {}, 952});
    {   liquid::menu::MenuItem style{L"Clock & Calendar Style", ItemKind::Submenu, true, false, {}};
        const wchar_t* names[3] = {L"Light", L"Dark", L"Liquid Glass"};
        for (UINT k = 0; k < 3; ++k) style.children.push_back({names[k], ItemKind::Command, true, statusTileStyle_ == static_cast<int>(k), {}, 960 + k});
        items.push_back(std::move(style)); }
    items.push_back({L"Show Menu Bar", ItemKind::Command, true, menuBarEnabled_, {}, 950});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({L"Remove from Dock", ItemKind::Command, true, false, {}, 953});
    items.push_back({L"DockOver Settings…", ItemKind::Command, true, false, {}, 951});
    POINT pt{}; GetCursorPos(&pt);
    const std::wstring key = index >= 0 && static_cast<size_t>(index) < items_.size() ? TailKey(items_[static_cast<size_t>(index)]) : std::wstring();
    liquid::popup::Show(hwnd_, pt, std::move(items), [this, extIds, key](const liquid::menu::MenuItem& chosen) {
        const UINT cmd = chosen.commandId;
        if ((cmd >= 900 && cmd < 900 + ARRAYSIZE(kStatusMenuIds)) || (cmd >= 1000 && cmd < 1000 + extIds.size())) {
            ToggleStatusCell(cmd >= 1000 ? extIds[cmd - 1000] : std::wstring(kStatusMenuIds[cmd - 900]));
        } else if (cmd == 953) {  // the tile that was right-clicked (found again: the Dock may have changed meanwhile)
            for (size_t i = 0; i < items_.size(); ++i) if (items_[i].isStatusGroup && TailKey(items_[i]) == key) { RemoveDockItem(static_cast<int>(i)); break; }
        } else if (cmd == 950) {
            if (liquid::settings::WriteBool(opt_.desktopSettingsPath, "menuBar", "enabled", !menuBarEnabled_) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save settings.", L"DockOver", MB_OK | MB_ICONWARNING); return; }
            DLOG(L"status group: menu bar %ls requested", menuBarEnabled_ ? L"off" : L"on");  // the Manager starts or stops it
        } else if (cmd >= 960 && cmd <= 962) {
            if (liquid::settings::WriteDockInt(opt_.desktopSettingsPath, "statusTileStyle", static_cast<int>(cmd - 960)) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save settings.", L"DockOver", MB_OK | MB_ICONWARNING); return; }
            statusTileStyle_ = static_cast<int>(cmd - 960); RefreshStatus(false); settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath);
        } else if (cmd == 952) {
            if (liquid::settings::WriteBool(opt_.desktopSettingsPath, "dock", "statusBatteryPercent", !statusPercent_) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save settings.", L"DockOver", MB_OK | MB_ICONWARNING); return; }
            statusPercent_ = !statusPercent_; RefreshStatus(false); settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath);
        } else if (cmd == 951) {
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\LiquidDesktopSettingsOpenV1");
            if (ev) { SetEvent(ev); CloseHandle(ev); } else MessageBoxW(hwnd_, L"Start DockOver to open Settings.", L"DockOver", MB_OK | MB_ICONINFORMATION);
        }
    });
}

// Running apps that are not pinned appear as transient items at the end of the app area (before the
// trailing separator / Downloads) and disappear when they exit. Skipped while a drag is in progress.
// Rebuilds the three sections, keeping the relative order inside each. System boundaries are recreated
// here (only one before Downloads when nothing unpinned is running) and are never saved or removable.
void App::NormalizeSections() {
    std::vector<PinnedItem> pinned, running, tail;
    for (auto& it : items_) {
        if (it.isSeparator && it.isSystemSeparator) continue;
        if (it.isDownloads || it.isRecycleBin || it.isMinimized || it.isStatusGroup) tail.push_back(std::move(it));
        else if (it.isTransient) running.push_back(std::move(it));
        else pinned.push_back(std::move(it));
    }
    auto boundary = [] { PinnedItem s; s.isSeparator = true; s.isSystemSeparator = true; s.target = L"---"; return s; };
    items_.clear();
    for (auto& it : pinned) items_.push_back(std::move(it));
    if (!running.empty()) { if (showDivider_) items_.push_back(boundary()); for (auto& it : running) items_.push_back(std::move(it)); }
    if (showDivider_) items_.push_back(boundary());  // the divider can be removed (and added back) by the user
    // Right-hand items in the user's order (dock.tailOrder); items it does not list keep the default order after it:
    // status tiles, Downloads, Recycle Bin. Minimized windows always sit just left of the Recycle Bin (or at the end).
    std::vector<PinnedItem> minimized, rest;
    for (auto& it : tail) (it.isMinimized ? minimized : rest).push_back(std::move(it));
    auto rank = [&](const PinnedItem& p) {
        const std::wstring key = TailKey(p);
        auto at = std::find(tailOrder_.begin(), tailOrder_.end(), key);
        // A tile the saved order does not know yet (a new standalone tile such as Hidden Icons, which used to be a cell of
        // the status group) takes the status group's place.
        if (at == tailOrder_.end() && key.rfind(L"status:", 0) == 0) at = std::find(tailOrder_.begin(), tailOrder_.end(), std::wstring(L"status"));
        if (at != tailOrder_.end()) return static_cast<int>(at - tailOrder_.begin());
        return 100 + (p.isStatusGroup ? 0 : p.isDownloads ? 1 : 3);
    };
    std::stable_sort(rest.begin(), rest.end(), [&](const PinnedItem& a, const PinnedItem& b) { return rank(a) < rank(b); });
    auto bin = std::find_if(rest.begin(), rest.end(), [](const PinnedItem& p) { return p.isRecycleBin; });
    const size_t binAt = static_cast<size_t>(bin - rest.begin());
    DLOG(L"sections: pinned=%zu running=%zu tail=%zu", pinned.size(), running.size(), rest.size() + minimized.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        if (i == binAt) for (auto& m : minimized) items_.push_back(std::move(m));
        items_.push_back(std::move(rest[i]));
    }
    if (binAt >= rest.size()) for (auto& m : minimized) items_.push_back(std::move(m));
}

// The first index after the pinned section (Launchpad, pinned apps, the user's separators).
size_t App::PinnedEnd() const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].isSystemSeparator || items_[i].isTransient || IsTailItem(items_[i])) return i;
    return items_.size();
}
// An identity for the item under a drag: the stable id of an app, the tail key of a system item, or the slot of a
// separator (separators have no id; the slot cannot change until the drag ends).
std::string App::DragKey(size_t index) const {
    if (index >= items_.size()) return {};
    const auto& it = items_[index];
    if (it.isSeparator) return it.isSystemSeparator ? std::string() : "separator:" + std::to_string(index);
    if (it.isMinimized) return {};
    if (IsTailItem(it)) { std::string k = "tail:"; for (wchar_t c : TailKey(it)) k += static_cast<char>(c); return k; }  // ASCII keys
    return it.stableId;
}

// Recreates the right-hand items from the settings (status tiles, Downloads, Recycle Bin), keeping minimized windows.
void App::RebuildTail(const wchar_t* why) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const PinnedItem& p) { return p.isStatusGroup || p.isDownloads || p.isRecycleBin; }), items_.end());
    std::vector<PinnedItem> added = MakeStatusItems();
    if (showDownloads_) { PinnedItem d; d.isDownloads = true; d.target = downloadsPath_; d.labelOverride = L"Downloads"; added.push_back(std::move(d)); }
    if (showTrash_) { PinnedItem b; b.isRecycleBin = true; b.target = L"shell:RecycleBinFolder"; b.labelOverride = L"Recycle Bin"; added.push_back(std::move(b)); }
    for (auto& it : added) { ResolveItem(it); items_.push_back(std::move(it)); }
    NormalizeSections();
    pressed_ = -1;
    RefreshRunningState(items_, hwnd_);
    ApplyWindowLayout(why);
    BuildIcons(); if (!statusCells_.empty()) RefreshStatus(true); StartAnimating(); needsFrame_ = true;
    DLOG(L"dock items: rebuilt (%ls): downloads=%d trash=%d divider=%d status cells=%zu", why, showDownloads_ ? 1 : 0, showTrash_ ? 1 : 0, showDivider_ ? 1 : 0, statusCells_.size());
}
// The right-hand items' current order (dock.tailOrder) and the status cells in the matching order (dock.statusCells).
bool App::SaveTailOrder() {
    std::vector<std::wstring> keys, cells;
    for (const auto& it : items_) {
        if (!IsTailItem(it) || it.isMinimized) continue;
        const std::wstring k = TailKey(it);
        if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        if (it.isStatusGroup) for (const auto& c : it.statusCells) cells.push_back(c);
    }
    for (const auto& c : statusCells_) if (std::find(cells.begin(), cells.end(), c) == cells.end()) cells.push_back(c);  // never drop a cell
    const bool ok = liquid::settings::WriteTailOrder(opt_.desktopSettingsPath, keys) == liquid::settings::SaveStatus::Saved &&
                    (cells == statusCells_ || liquid::settings::WriteStatusCells(opt_.desktopSettingsPath, cells) == liquid::settings::SaveStatus::Saved);
    if (ok) { tailOrder_ = std::move(keys); statusCells_ = std::move(cells); }
    settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath);
    DLOG(L"dock items: order saved=%d", ok ? 1 : 0);
    return ok;
}
// Adds or removes one status cell (the status menu, Show in Dock, Remove from Dock).
bool App::ToggleStatusCell(const std::wstring& id) {
    std::vector<std::wstring> next = statusCells_;
    auto it = std::find(next.begin(), next.end(), id);
    if (it != next.end()) next.erase(it);
    else {  // kept tray icons and extension items go just before the Hidden Icons chevron, which stays last
        const auto chevron = std::find(next.begin(), next.end(), std::wstring(L"tray"));
        if ((dock::statusgroup::TrayCell(id) || id.rfind(L"ext:", 0) == 0) && chevron != next.end()) next.insert(chevron, id); else next.push_back(id);
    }
    if (next.size() > 16) { MessageBoxW(hwnd_, L"The Dock's status items are full (16 at most). Remove one first.", L"DockOver", MB_OK | MB_ICONINFORMATION); return false; }
    if (next.empty() && !menuBarEnabled_) { MessageBoxW(hwnd_, L"Keep at least one status item while the menu bar is off.", L"DockOver", MB_OK | MB_ICONINFORMATION); return false; }
    if (liquid::settings::WriteStatusCells(opt_.desktopSettingsPath, next) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save the status items.", L"DockOver", MB_OK | MB_ICONWARNING); return false; }
    ApplyStatusCells(next, menuBarEnabled_);
    settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath);
    return true;
}
// Remove from Dock for any item: an app or a separator leaves the pinned list; Downloads, the Recycle Bin, the divider
// and the status tiles are hidden through the shared settings (Dock menu > Show in Dock brings them back).
void App::RemoveDockItem(int index) {
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) return;
    const PinnedItem& it = items_[static_cast<size_t>(index)];
    auto saveBool = [&](const char* key, bool& field) {
        if (liquid::settings::WriteBool(opt_.desktopSettingsPath, "dock", key, false) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save settings.", L"DockOver", MB_OK | MB_ICONWARNING); return false; }
        field = false; settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath); return true;
    };
    if (it.isLaunchpad || it.isMinimized || it.isTransient) return;  // Launchpad always exists; running windows are not Dock items
    if (it.isSeparator && it.isSystemSeparator) { if (saveBool("showDivider", showDivider_)) RebuildTail(L"divider removed"); return; }
    if (it.isDownloads) { if (saveBool("showDownloads", showDownloads_)) { dock::downloads::Close(); RebuildTail(L"downloads removed"); } return; }
    if (it.isRecycleBin) { if (saveBool("showTrash", showTrash_)) RebuildTail(L"trash removed"); return; }
    if (it.isStatusGroup) {
        std::vector<std::wstring> next = statusCells_;
        for (const auto& c : it.statusCells) next.erase(std::remove(next.begin(), next.end(), c), next.end());
        if (next.empty() && !menuBarEnabled_) { MessageBoxW(hwnd_, L"Keep at least one status item while the menu bar is off.", L"DockOver", MB_OK | MB_ICONINFORMATION); return; }
        if (liquid::settings::WriteStatusCells(opt_.desktopSettingsPath, next) != liquid::settings::SaveStatus::Saved) { MessageBoxW(hwnd_, L"Could not save the status items.", L"DockOver", MB_OK | MB_ICONWARNING); return; }
        ApplyStatusCells(next, menuBarEnabled_); settingsRevision_ = liquid::settings::ReadRevision(opt_.desktopSettingsPath);
        return;
    }
    DLOG(L"remove from dock '%ls'", it.isSeparator ? L"separator" : it.label.c_str());
    items_.erase(items_.begin() + index);
    if (!SavePinnedConfig(configPath_, items_)) DLOG(L"remove: config save FAILED");
    pressed_ = -1; RefreshRunningState(items_, hwnd_); ApplyWindowLayout(L"remove from dock"); BuildIcons(); needsFrame_ = true; StartAnimating();
}

// 1 = items present, 0 = empty, -1 = query failed. Read-only (SHQueryRecycleBinW over all drives).
static int RecycleBinFull() { SHQUERYRBINFO info{sizeof(info)}; if (FAILED(SHQueryRecycleBinW(nullptr, &info))) return -1; return info.i64NumItems > 0 ? 1 : 0; }

bool App::SyncRunningApps() {
    std::vector<std::wstring> unpinned;
    bool scanned = false;
    bool changed = RefreshRunningState(items_, hwnd_, &scanned, dragging_ ? nullptr : &unpinned);
    if (!scanned || dragging_) return changed;
    bool structure = false;
    for (size_t i = items_.size(); i-- > 0;)
        if (items_[i].isTransient && !items_[i].running) {
            DLOG(L"running: transient '%ls' exited; removed", items_[i].label.c_str());
            items_.erase(items_.begin() + i);
            structure = true;
        }
    for (const auto& path : unpinned) {
        PinnedItem item;
        item.target = path;
        item.isTransient = true;
        ResolveItem(item);
        if (item.stableId.empty() || std::any_of(items_.begin(), items_.end(), [&](const PinnedItem& e) { return !e.isSeparator && e.stableId == item.stableId; }))
            continue;
        DLOG(L"running: transient '%ls' added", item.label.c_str());
        items_.push_back(std::move(item));
        structure = true;
    }
    if (structure) {
        NormalizeSections();
        pressed_ = -1;
        RefreshRunningState(items_, hwnd_);
        ApplyWindowLayout(L"running apps");
        BuildIcons();
    }
    return changed || structure;
}

void App::MinimizedChanged(HWND w, bool added) {
    const auto it = std::find(minimized_.begin(), minimized_.end(), w);
    if (added) {
        if (it != minimized_.end()) return;
        minimized_.push_back(w);
        PinnedItem m; m.isMinimized = true; m.window = w; wchar_t title[256]{}; GetWindowTextW(w, title, 256); m.label = title;
        char id[40]{}; sprintf_s(id, "min:%p", static_cast<void*>(w)); m.stableId = id;
        items_.push_back(std::move(m));
    } else {
        if (it == minimized_.end()) return;
        minimized_.erase(it);
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const PinnedItem& p) { return p.isMinimized && p.window == w; }), items_.end());
    }
    NormalizeSections(); pressed_ = -1;
    ApplyWindowLayout(added ? L"window minimized" : L"window restored");
    BuildIcons(); StartAnimating(); needsFrame_ = true;
}
// Each frame: tell the minimize overlay where every minimized tile's icon is on screen (it moves with magnification,
// reordering and auto-hide).
void App::PlaceMinimizedTiles() {
    if (minimized_.empty() || frame_.items.size() != items_.size()) return;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].isMinimized) continue;
        const auto& g = frame_.items[i];
        RECT r{windowRect_.left + static_cast<LONG>(std::lround(g.left * scale_)), windowRect_.top + static_cast<LONG>(std::lround(g.top * scale_)) + static_cast<LONG>(appliedOffset_),
               0, 0};
        r.right = r.left + static_cast<LONG>(std::lround(g.size * scale_)); r.bottom = r.top + static_cast<LONG>(std::lround(g.size * scale_));
        if (hiddenRegion_ || r.top >= windowRect_.bottom) r = RECT{};
        dock::minimize::SetTileRect(items_[i].window, r);
    }
    dock::minimize::KeepAbove();
}
void App::ApplyGenie(bool on) {
    ANIMATIONINFO ai{sizeof(ai)};
    if (on && !pausedWindowsAnimation_ && SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0) && ai.iMinAnimate) {
        ai.iMinAnimate = 0;
        if (SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0)) { pausedWindowsAnimation_ = true; DLOG(L"minimize: Windows minimize animation paused for this session"); }
    } else if (!on && pausedWindowsAnimation_) {
        ai.iMinAnimate = 1; SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0); pausedWindowsAnimation_ = false;
        DLOG(L"minimize: Windows minimize animation restored");
    }
    genie_ = on; dock::minimize::SetGenie(on);
}

void App::SyncTiles() {
    std::vector<bool> separators(items_.size());
    std::vector<float> widths(items_.size(), 1.0f);
    for (size_t i = 0; i < items_.size(); ++i) { separators[i] = items_[i].isSeparator; if (items_[i].isStatusGroup) widths[i] = dock::statusgroup::WidthFactor(items_[i].statusCells); }
    model_.SetTiles(separators, widths);
}
void App::ApplyWindowLayout(const wchar_t* reason) {
    if (!hwnd_) return;
    scale_ = dpi_ / 96.0f;
    SyncTiles();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT monitor = mi.rcMonitor;
    const int monitorWidth = monitor.right - monitor.left, monitorHeight = monitor.bottom - monitor.top;
    resizeMaxSize_ = model_.MaxIconSizeForWidth(monitorWidth / scale_);
    if (model_.m.icon > resizeMaxSize_) model_.SetIconSize(static_cast<float>(resizeMaxSize_));
    // Keep one stable, screen-sized-or-smaller canvas throughout the drag.
    const float iconSize = model_.m.icon;
    if (resizing_) model_.SetIconSize(static_cast<float>(resizeMaxSize_));
    pxW_ = static_cast<UINT>(std::min<double>(monitorWidth, std::ceil(model_.DesiredWindowWidth() * scale_)));
    pxH_ = static_cast<UINT>(std::min<double>(monitorHeight, std::ceil(model_.DesiredWindowHeight() * scale_)));
    if (resizing_) model_.SetIconSize(iconSize);
    model_.SetWindowSize(pxW_ / scale_, pxH_ / scale_);

    const RECT position = docky::geometry::DockOnMonitor(monitor,static_cast<int>(pxW_),static_cast<int>(pxH_));
    const int x = position.left, y = position.top;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, static_cast<int>(pxW_), static_cast<int>(pxH_), SWP_NOACTIVATE);
    GetWindowRect(hwnd_, &windowRect_);

    HRESULT hr = renderer_.Ready() ? renderer_.Resize(pxW_, pxH_, dpi_) : renderer_.Init(hwnd_, pxW_, pxH_, dpi_);
    if (FAILED(hr)) DLOG(L"renderer init/resize failed 0x%08X", hr);
    // Popups opened from the Dock (Hidden Icons, battery, input, clock, menus) run on this thread: they draw with the
    // Dock's device. A second device reading the shared desktop image adds a cross-device GPU sync that held the Dock
    // to every other refresh while a popup was open (240 -> ~125 fps, measured 2026-11-18; docky Desktop: glass_window.h).
    liquid::popup::SetRenderDevice(renderer_.Device());
    dock::downloads::SetFanDevice(renderer_.Device());  // the Downloads Fan too (no device of its own: ~37 driver threads less)
    SetFullRegion(false, true);
    UpdateDockAppBar();
    needsFrame_ = true;
    logGeometry_ = true;
    DLOG(L"layout (%ls): dpi %.0f, %d items, window %dx%d at (%d,%d), monitor (%ld,%ld)-(%ld,%ld), base dock %.1f DIP, "
         L"max extra %.1f DIP",
         reason, dpi_, model_.Count(), pxW_, pxH_, x, y, monitor.left, monitor.top, monitor.right, monitor.bottom,
         model_.BaseWidth(), model_.MaxExtra());
}

// Idle: the window region is just the resting plate (+ shadow) so the rest of the window never
// blocks clicks to the desktop. While hovered/animating the region covers the whole window.
void App::SetFullRegion(bool full, bool force) {
    if (hiddenRegion_ && hwnd_) {  // hidden by auto-hide: only the reveal strip on the screen edge
        const RectF r = model_.BaseDockRect();
        const int strip = (std::max)(2, static_cast<int>(std::lround(4.0f * scale_)));
        SetWindowRgn(hwnd_, CreateRectRgn(static_cast<int>(std::floor(r.left * scale_)), static_cast<int>(pxH_) - strip,
                                         static_cast<int>(std::ceil(r.right * scale_)), static_cast<int>(pxH_)), TRUE);
        regionFull_ = full; regionInit_ = true;
        return;
    }
    if (!hwnd_ || (!force && regionInit_ && full == regionFull_)) return;
    regionFull_ = full;
    regionInit_ = true;
    if (full) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    const RectF r = model_.BaseDockRect();
    const float pad = model_.m.shadowPad;
    HRGN rgn = CreateRectRgn(static_cast<int>(std::floor((r.left - pad) * scale_)),
                             static_cast<int>(std::floor((r.top - pad) * scale_)),
                             static_cast<int>(std::ceil((r.right + pad) * scale_)), static_cast<int>(pxH_));
    SetWindowRgn(hwnd_, rgn, TRUE);
}

bool App::UpdateAutoHide(double dt, double now) {
    // Never hide while the user is working with the Dock or anything it opened.
    const bool busy = !AutoHiding() || resizing_ || dragging_ || pressed_ >= 0 || model_.PointerActive() || dock::downloads::OpenProgress() > 0.0f ||
                      dock::launchpad::IsOpen() || dock::iconedit::IsOpen() || OwnPopupVisible();
    if (AutoHiding()) {  // V0.16 diagnostics: log which condition keeps the Dock shown, only when it changes
        const int reasons = (dragging_ ? 1 : 0) | (pressed_ >= 0 ? 2 : 0) | (model_.PointerActive() ? 4 : 0) | (dock::downloads::OpenProgress() > 0.0f ? 8 : 0) |
                            (dock::launchpad::IsOpen() ? 16 : 0) | (dock::iconedit::IsOpen() ? 32 : 0) | (OwnPopupVisible() ? 64 : 0);
        if (reasons != autoHideBusy_) { autoHideBusy_ = reasons; DLOG(L"auto-hide: busy mask 0x%02X", reasons); }
    }
    if (!hideInit_) { hideInit_ = true; hide_.SetRest(0.002, 0.02); }  // normalised 0..1 (about 0.2 px at 100 px travel)
    double target = hide_.Target();
    if (busy) { target = 0.0; if (model_.PointerActive() || !AutoHiding()) leftAt_ = -1.0; }
    else if (leftAt_ < 0) leftAt_ = now;
    else if (now - leftAt_ >= 0.4) target = 1.0;  // short grace period, like the macOS Dock
    if (reducedMotion_) hide_.SnapTo(target);
    else {
        if (target != hide_.Target()) hide_.SetSpec(target > hide_.Target() ? dock::dockmotion::AutoHideHide : dock::dockmotion::AutoHideShow);
        hide_.SetTarget(target);  // mid-slide reversal keeps value and velocity
        hide_.Step(dt);
    }
    const RectF r = model_.BaseDockRect();
    const float distance = static_cast<float>(pxH_) - std::floor((r.top - model_.m.shadowPad) * scale_);
    const float offset = std::round(hide_.F() * distance);
    if (offset != appliedOffset_) { appliedOffset_ = offset; renderer_.SetVisualOffsetY(offset); }
    const bool hiddenNow = hide_.Value() >= 0.999;
    if (hiddenNow != hiddenRegion_) {
        hiddenRegion_ = hiddenNow; SetFullRegion(regionFull_, true); DLOG(L"auto-hide: %ls", hiddenNow ? L"hidden" : L"revealing");
        ApplyGameMode();
    }
    // Keep frames running while sliding or while the grace period is pending.
    return hide_.Animating() || (AutoHiding() && !busy && hide_.Target() == 0.0);
}

void App::StartAnimating() {
    if (!animating_) {
        lastT_ = Now();
        animating_ = true;
    }
}

// Window boundary, as MyDockFinder does it ("Enable window boundaries to prevent windows from covering the Dock",
// config [normal] border): reverse engineering of Dock_64.exe (FUN_140004060 / FUN_140004180 / FUN_140120c80 /
// FUN_140120f80, 2026-11-10) shows a plain shell AppBar on the Dock's edge: ABM_NEW, ABM_QUERYPOS, ABM_SETPOS with
// thickness = Dock size + distance from the screen edge, minus a taskbar already reserved on that edge; no
// drag-time window constraint was found. Windows then keeps maximized / snapped windows out of the band. Like
// MyDockFinder this is not combined with auto-hide (a hidden Dock must not keep reserving space).
void App::SetFullscreenApp(bool full) {
    if (!hwnd_ || full == fullscreenApp_) return;
    fullscreenApp_ = full;
    if (full) { liquid::popup::Close(); leftAt_ = Now() - 1.0; }  // slide away now; the bottom edge reveals it
    ApplyGameMode();
    needsFrame_ = true; StartAnimating();
    DLOG(L"dock: full-screen app %ls", full ? L"on: Dock auto-hides" : L"off: Dock shown");
}

void App::SetGameSuppressed(bool suppress) {
    if (!hwnd_ || gameSuppressed_ == suppress) return;
    gameSuppressed_ = suppress;
    if (suppress) {
        liquid::popup::Close();
        dock::downloads::Close();
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        if(!launchpadCovers_)ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        needsFrame_ = true;
        StartAnimating();
    }
    ApplyGameMode();
    DLOG(L"dock: game compatibility %ls", suppress ? L"hidden" : L"restored");
}

void App::ApplyGameMode() {
    const bool game = GameMode();
    winKey_.SetEnabled(!fullscreenApp_ && !game);
    dock::minimize::SetGameMode(game);
    if (hwnd_) RequestBackdrop(CaptureWanted(), MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY));
    if (!game && fullscreenApp_ == false) gameTick_ = 0;
    DLOG(L"dock: game mode %ls", game ? L"on: capture stopped, polling every 5 s" : L"off");
}

void App::UpdateDockAppBar() {
    const bool want = hwnd_ && !autoHide_ && dockAppBarEnabled_;
    if (!want) {
        if (dockAppBar_) { APPBARDATA a{sizeof(a)}; a.hWnd = hwnd_; SHAppBarMessage(ABM_REMOVE, &a); dockAppBar_ = false; dockAppBarRect_ = {}; DLOG(L"dock: appbar removed"); }
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    const RectF base = model_.BaseDockRect();
    const LONG plateTop = windowRect_.top + static_cast<LONG>(std::lround(base.top * scale_));
    const LONG band = mi.rcMonitor.bottom - plateTop;  // plate + its gap to the screen edge
    if (band <= 0) return;
    RECT want_rc{mi.rcMonitor.left, mi.rcMonitor.bottom - band, mi.rcMonitor.right, mi.rcMonitor.bottom};
    if (dockAppBar_ && EqualRect(&want_rc, &dockAppBarRect_)) return;  // SETPOS changes the work area: only on real change
    if (!dockAppBar_) {
        APPBARDATA a{sizeof(a)}; a.hWnd = hwnd_; a.uCallbackMessage = kDockAppBarMsg;
        dockAppBar_ = SHAppBarMessage(ABM_NEW, &a) != FALSE;
        if (!dockAppBar_) { DLOG(L"dock: appbar registration failed"); return; }
    }
    APPBARDATA a{sizeof(a)}; a.hWnd = hwnd_; a.uEdge = ABE_BOTTOM; a.rc = want_rc;
    SHAppBarMessage(ABM_QUERYPOS, &a);
    // A taskbar already reserving the bottom edge pushes our proposal up; keep the band anchored to the screen
    // edge so the total reserved height is the Dock's (MyDockFinder subtracts the taskbar the same way).
    a.rc.top = want_rc.top;
    if (a.rc.bottom <= a.rc.top) {  // the taskbar alone already covers the Dock band
        SHAppBarMessage(ABM_REMOVE, &a); dockAppBar_ = false; dockAppBarRect_ = want_rc; DLOG(L"dock: appbar not needed (edge already reserved)"); return;
    }
    SHAppBarMessage(ABM_SETPOS, &a);
    dockAppBarRect_ = want_rc;
    DLOG(L"dock: appbar reserves %ld px (%ld,%ld)-(%ld,%ld)", a.rc.bottom - a.rc.top, a.rc.left, a.rc.top, a.rc.right, a.rc.bottom);
}

bool App::RefreshAccessibility() {
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool known=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)!=FALSE;
    const bool contrast=known && (hc.dwFlags&HCF_HIGHCONTRASTON)!=0;
    bool opaque=true;
    try {opaque=!systemUi_ || !systemUi_.AdvancedEffectsEnabled() || !known || contrast;} catch(...) {opaque=true;}
    const bool changed=opaqueFallback_!=opaque || highContrast_!=contrast;
    opaqueFallback_=opaque;highContrast_=contrast;
    if(changed && hwnd_){const HMONITOR monitor=MonitorFromWindow(hwnd_,MONITOR_DEFAULTTOPRIMARY);
        RequestBackdrop(CaptureWanted(),monitor);
        simBackdrop_.Request(opaqueFallback_ || glass_.backdrop==BackdropKind::Live ? BackdropKind::None : glass_.backdrop,monitor);}
    return changed;
}
void App::SyncGlassParameters() {
    // One consistent snapshot per frame; the lock is only held for a struct copy inside the store.
    GlassSettingsStore& store = GlassSettingsStore::Get();
    const unsigned long long v = store.Version();
    if (v == glassVersion_) return;
    const GlassParameters previous = glass_;
    glass_ = store.Current();
    glassVersion_ = v;
    if (hwnd_) {
        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        simBackdrop_.Request(opaqueFallback_ || glass_.backdrop == BackdropKind::Live ? BackdropKind::None : glass_.backdrop, monitor);
        RequestBackdrop(CaptureWanted(), monitor);
    }
    if (glass_.continuousRender && !previous.continuousRender) StartAnimating();
}

// Pointer data for the glass interaction, eased in real time (responseTime) like the Lab's pointer
// smoothing. Returns true while still moving so the frame loop keeps running.
bool App::UpdateGlassPointer(double dt) {
    const float tx = mouseX_ * scale_, ty = mouseY_ * scale_;
    const float hoverT = model_.PointerActive() ? 1.0f : 0.0f;
    const float pressT = pressed_ >= 0 ? 1.0f : 0.0f;
    const auto ease = [dt, this](float& v, float target, double seconds) {
        if (reducedMotion_) { v = target; return; }
        const double tau = std::max(seconds / 3.0, 1e-4);
        v += (target - v) * static_cast<float>(1.0 - std::exp(-dt / tau));
        if (std::fabs(target - v) < 1e-3f) v = target;
    };
    ease(ptrX_, tx, glass_.responseTime / 1000.0);
    ease(ptrY_, ty, glass_.responseTime / 1000.0);
    ease(ptrHover_, hoverT, 0.16);
    ease(ptrPress_, pressT, 0.23);
    const bool moving = std::fabs(ptrX_ - tx) > 0.25f || std::fabs(ptrY_ - ty) > 0.25f || ptrHover_ != hoverT || ptrPress_ != pressT;
    return moving && !opaqueFallback_ && glass_.material == MaterialKind::Liquid;
}

void App::Frame() {
    const double now = Now();
    double dt = now - lastT_;
    dt = std::clamp(dt, 0.0, 0.05);
    lastT_ = now;
    const bool wasAnimating = animating_ || glass_.continuousRender;
    SyncGlassParameters();
    bool anim = model_.Update(dt, now) | UpdateGlassPointer(dt);
    anim = UpdateAutoHide(dt, now) || anim;
    model_.Compute(frame_, now);
    DockFrame baseFrame=frame_;
    if(visualOffsets_.size()!=frame_.items.size())visualOffsets_.assign(frame_.items.size(),MakeOffsetSpring());
    if(visualYOffsets_.size()!=frame_.items.size())visualYOffsets_.assign(frame_.items.size(),MakeOffsetSpring());
    for(size_t i=0;i<frame_.items.size();++i){
        float target=0.0f;
        if(dragging_ && dropAllowed_ && static_cast<int>(i)!=dragFrom_){
            if(dragTarget_>dragFrom_ && static_cast<int>(i)>dragFrom_ && static_cast<int>(i)<=dragTarget_)target=-model_.m.pitch;
            if(dragTarget_<dragFrom_ && static_cast<int>(i)>=dragTarget_ && static_cast<int>(i)<dragFrom_)target=model_.m.pitch;
        }
        // Springs keep velocity when the drop target changes mid-slide (fast drags across several tiles).
        auto& ox=visualOffsets_[i];auto& oy=visualYOffsets_[i];
        if(reducedMotion_){ox.SnapTo(target);oy.SnapTo(0.0);}
        else{ox.SetTarget(target);oy.SetTarget(0.0);if(ox.Step(dt))anim=true;if(oy.Step(dt))anim=true;}
        const float offset=ox.F(),offsetY=oy.F();
        auto& g=frame_.items[i];g.cx+=offset;g.left+=offset;g.tileLeft+=offset;g.tileRight+=offset;g.top+=offsetY;g.restTop+=offsetY;
    }
    if(dragging_ && dragFrom_>=0 && static_cast<size_t>(dragFrom_)<frame_.items.size()){
        auto& g=frame_.items[dragFrom_];const float center=mouseX_-grabX_;g.left+=center-g.cx;g.cx=center;g.top=mouseY_-grabY_;g.restTop=g.top;
    }
    // V0.16 diagnostics: once per layout, log each icon's rest centre in screen pixels (used by real-mouse tests).
    if(logGeometry_ && !anim && model_.Hover()<0.01f && frame_.items.size()==items_.size()){
        logGeometry_=false;
        for(size_t i=0;i<frame_.items.size();++i){const auto& g=frame_.items[i];if(items_[i].isSeparator)continue;
            DLOG(L"geometry: item %zu '%ls' centre %ld,%ld size %ld",i,items_[i].label.c_str(),windowRect_.left+static_cast<LONG>(g.cx*scale_),
                 windowRect_.top+static_cast<LONG>((g.restTop+g.size/2)*scale_),static_cast<LONG>(g.size*scale_));}
    }

    PlaceMinimizedTiles();
    // Pull (never wait for) the latest backdrop frame: the simulated source until P0.2's capture.
    BackdropFrame backdrop;
    const RECT wanted = windowRect_;
    const HMONITOR currentMonitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    RequestBackdrop(CaptureWanted(), currentMonitor);

    Scene scene;
    scene.frame = &frame_;
    scene.metrics = &model_.m;
    if(dragging_){scene.dragIndex=dragFrom_;scene.showInsertion=dropAllowed_;
        if(dropAllowed_ && dragTarget_>=0 && static_cast<size_t>(dragTarget_)<baseFrame.items.size())scene.insertionX=baseFrame.items[dragTarget_].cx;}
    scene.windowW = model_.WindowWidth();
    scene.windowH = model_.WindowHeight();
    scene.icons.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        IconVisual& v = scene.icons[i];
        v.icon = i < iconBmp_.size() ? iconBmp_[i].Get() : nullptr;
        v.shadow = i < shadowBmp_.size() ? shadowBmp_[i].Get() : nullptr;
        v.shadowPadRatio = i < shadowPad_.size() ? shadowPad_[i] : 0.0f;
        v.running = items_[i].running || IsFinderSlot(items_[i]);  // macOS: Finder is always running, so its dot always shows
        v.separator = items_[i].isSeparator;
        v.external = items_[i].isMinimized && dock::minimize::SuppressFallbackTile(items_[i].window);
        v.opacity = !dragging_ && static_cast<int>(i) == pressed_ ? 0.6f : (dragging_ && removeOnDrop_ && static_cast<int>(i) == dragFrom_ ? 0.45f : 1.0f);  // dimmed: letting go removes it
        if (items_[i].isDownloads && downloadsOpenBmp_) { v.alt = downloadsOpenBmp_.Get(); v.altBlend = dock::downloads::OpenProgress(); }
        if (items_[i].isStatusGroup && i < frame_.items.size()) {
            v.statusCells = &items_[i].statusCells; v.statusState = &statusState_; v.running = false;
            const auto& g = frame_.items[i];
            if (model_.HoveredIndex(frame_) == static_cast<int>(i) && !dragging_)
                v.statusHover = items_[i].statusCells.size() > 1 ? dock::statusgroup::CellAt(D2D1::RectF(g.left, g.top, g.left + g.width, g.top + g.size), items_[i].statusCells, D2D1::Point2F(mouseX_, mouseY_)) : -1;
        }
    }
    const int hovered = model_.HoveredIndex(frame_);
    if (hovered >= 0 && !dragging_) {
        scene.labelIndex = hovered;
        scene.label = items_[hovered].label;
        if (items_[hovered].isStatusGroup) {  // the label names the hovered cell
            const auto& g = frame_.items[hovered];
            const auto& cells = items_[hovered].statusCells;
            const int cell = dock::statusgroup::CellAt(D2D1::RectF(g.left, g.top, g.left + g.width, g.top + g.size), cells, D2D1::Point2F(mouseX_, mouseY_));
            if (cell >= 0) scene.label = StatusCellLabel(cells[static_cast<size_t>(cell)]);
            // Control Center's device takes ~170 ms to create: make it while the pointer is on a tile that opens it, so
            // the first opening is as quick as the later ones (it is kept afterwards anyway).
            if (!ccPrewarmed_ && std::any_of(cells.begin(), cells.end(), [](const std::wstring& id) {
                    return id == L"combo" || id == L"control" || id == L"wifi" || id == L"bluetooth" || id == L"sound" || id == L"battery"; })) {
                ccPrewarmed_ = true; liquid::cc::Prewarm();
            }
        }
        scene.labelAlpha = std::clamp((model_.Hover() - 0.35f) / 0.65f, 0.0f, 1.0f);
    }
    scene.opaqueFallback=opaqueFallback_;
    if(opaqueFallback_){const auto palette=liquid::theme::Palette(appearanceMode_,systemDark_,highContrast_);
        scene.opaqueBackground=liquid::theme::ColorRef(palette.surface);scene.opaqueForeground=liquid::theme::ColorRef(palette.primary);}
    GlassParameters dockGlass = glass_;
    dockGlass.radius *= model_.m.icon / 46.0f;
    scene.glass = &dockGlass;
    scene.windowScreenRect = windowRect_;
    scene.pointerX = ptrX_;
    scene.pointerY = ptrY_;
    scene.hover = ptrHover_;
    scene.press = ptrPress_;
    scene.showDiag = diag_;
    scene.showBounds = bounds_;
    if (diag_) scene.diagText = DiagText();

    // Low latency (opt-in): draw the UI layer first (only when something besides the desktop changed), then take the
    // newest desktop frame on this thread (owner device: no cross-device hand-off) and draw only the glass under the
    // cached layer, so Present follows the frame within ~0.3 ms (DWM composes the next frame ~1 ms after handing it out).
    const bool ll = LowLatActive() && renderer_.CanCompose(scene);
    const double tUi0 = Now();
    if (ll) {
        const bool uiDirty = !renderer_.UiValid() || needsFrame_ || wasAnimating || anim || diag_;
        if (uiDirty && SUCCEEDED(renderer_.PrepareUi(scene))) { ++llPrepared_; llUiMs_ += renderer_.LastUiMs(); }
    }
    const double tWait0 = Now();
    if (auto* owned = OwnedDesktop()) {  // owner: this thread takes the desktop frames for every glass surface
        const bool dockReads = LowLatActive();
        // Frames have been arriving (video wallpaper, a moving window): the next one is due within ~1 ms, wait for it so
        // this frame shows it. After a quiet spell only look: the first frame of a change then shows one refresh later.
        // A frame the idle loop just took (backdropDirty_) is drawn now; waiting would skip it for the next one.
        // Only the one-frame path waits; otherwise just collect what is there.
        const bool expect = ll && !backdropDirty_ && lastOwnedFrame_ > 0 && now - lastOwnedFrame_ < 1.5 * docky::vblank::FramePeriod().load();
        const double w0 = Now();
        // Never block in AcquireNextFrame with a timeout: kernel waits round to the system timer (up to ~15 ms), which
        // was the source of the occasional 10-12 ms frame (measured 2026-11-20). Look without waiting, then look again
        // every 0.25 ms on a high-resolution timer for at most 2 ms.
        bool got = owned->PumpOwned(0);
        while (!got && expect && Now() - w0 < 0.002) { docky::vblank::Pause(0.00025); got = owned->PumpOwned(0); }
        if (got) lastOwnedFrame_ = Now();
        if (expect) { llWaitMs_ += (Now() - w0) * 1000.0; ++llWaits_; if (!got) ++llMisses_; }
        if (dockReads && glass_.backdrop == BackdropKind::Live && backdrop_.ProcessCapture(windowRect_)) lastDockBackdrop_ = Now();
    }
    // Pull (never wait for) the latest backdrop frame.
    bool haveBackdrop = !opaqueFallback_ && glass_.backdrop == BackdropKind::Live && backdrop_.Latest(wanted, backdrop);
    if (!opaqueFallback_ && !haveBackdrop && glass_.backdrop != BackdropKind::None && glass_.backdrop != BackdropKind::Live)
        haveBackdrop = simBackdrop_.Latest(wanted, backdrop);
    scene.backdrop = haveBackdrop ? &backdrop : nullptr;
    const double tDraw0 = Now();
    HRESULT hr = S_FALSE;
    if (ll && renderer_.UiValid()) {
        hr = renderer_.Compose(scene);
        if (hr != S_FALSE) { ++llComposed_; llComposeCpu_ += renderer_.LastCpuMs(); }
    }
    if (hr == S_FALSE) { hr = renderer_.Render(scene); if (ll) ++llFull_; }
    if (glass_.backdrop == BackdropKind::Live && haveBackdrop) TrackLatency(false);
    {   // Measurement mode (latency counter on): where a long animation frame went (at most 40 lines per run).
        const double period = docky::vblank::FramePeriod().load(), end = Now();
        static int longLogged = 0;
        if (latencyLogEvery_ > 0 && (anim || wasAnimating) && lastFrameEnd_ > 0 && end - lastFrameEnd_ > 1.6 * period && longLogged < 40) {
            ++longLogged;
            DLOG(L"long frame: %.2f ms since the previous one; before frame %.2f (loop waiting %.2f, messages %.2f, slowest 0x%04X/%llu %ls %.2f), ui layer %.2f, desktop frame %.2f, draw+present %.2f ms (present %.2f)",
                 (end - lastFrameEnd_) * 1000.0, (now - lastFrameEnd_) * 1000.0, loopWaitSec_ * 1000.0, loopMsgSec_ * 1000.0, loopSlowMsg_,
                 static_cast<unsigned long long>(loopSlowMsgWp_), loopSlowClass_, loopSlowMsgSec_ * 1000.0, (tWait0 - tUi0) * 1000.0, (tDraw0 - tWait0) * 1000.0,
                 (end - tDraw0) * 1000.0, renderer_.LastPresentMs());
        }
        lastFrameEnd_ = end; loopWaitSec_ = loopMsgSec_ = loopSlowMsgSec_ = 0; loopSlowMsg_ = 0; loopSlowMsgWp_ = 0;
    }
    // Dev fault injection (--dev-isolated + DOCKY_TEST_DEVICE_LOST_AFTER=n): after n frames, behave as if the GPU device
    // had been removed (driver update / reset), once, to exercise RecreateDevice and the capture owner hand-over.
    {   static const unsigned long long lostAfter = [] { wchar_t v[16]{}; return wcsstr(GetCommandLineW(), L"--dev-isolated") && GetEnvironmentVariableW(L"DOCKY_TEST_DEVICE_LOST_AFTER", v, 16) ? _wtoi64(v) : 0; }();
        static bool injected = false;
        if (lostAfter && !injected && totalFrames_ >= lostAfter && SUCCEEDED(hr)) { injected = true; hr = DXGI_ERROR_DEVICE_REMOVED; DLOG(L"render: device removed [test fault]"); } }
    {   // render timing (Info at exit): CPU work and time spent in Present per frame
        const double c = renderer_.LastCpuMs(), p = renderer_.LastPresentMs();
        timingCpu_ += c; timingPresent_ += p; timingPresentMax_ = (std::max)(timingPresentMax_, p); ++timingFrames_;
        if (p > 2.0) ++timingSlowPresents_;
    }
    if(docky::SignalFirstPresent(hr,opt_.firstFrameReady))DLOG(L"manager: first successful Present acknowledged");
    if (glass_.backdrop == BackdropKind::Live && haveBackdrop && !loggedLiveFrame_ && SUCCEEDED(hr)) {
        DLOG(L"wgc: live frame rendered id=%llu material=%ls", backdrop.frameId,
            renderer_.LastUsedGpu() ? L"Liquid Glass" : L"Smoked fallback");
        loggedLiveFrame_ = true;
    }
    if (glass_.backdrop == BackdropKind::Live && !haveBackdrop && !loggedLiveFallback_ && SUCCEEDED(hr)) {
        DLOG(L"wgc: background unavailable material=%ls", renderer_.LastUsedGpu() ? L"Liquid" : L"Smoked fallback");
        loggedLiveFallback_ = true;
    }
    if (haveBackdrop) loggedLiveFallback_ = false;
    if (glass_.backdrop != BackdropKind::Live) { loggedLiveFrame_ = false; loggedLiveFallback_ = false; }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET) {
        DLOG(L"render: device lost 0x%08X; recreating", hr);
        RecreateDevice();
        needsFrame_ = true;
        return;
    }
    if (FAILED(hr)) DLOG(L"render: failed 0x%08X", hr);
    ++totalFrames_;
    if (anim || wasAnimating) { ++animationFrames_; ++animationVersion_; }
    if (!firstFrameLogged_) {
        firstFrameLogged_ = true;
        DLOG(L"first frame presented %.0f ms after start", (now - startT_) * 1000.0);
    }
    needsFrame_ = false;
    backdropDirty_ = false;
    {
        GlassStats& st = GlassSettingsStore::Get().stats;
        const float cpu = renderer_.LastCpuMs();
        cpuEma_ = cpuEma_ <= 0.0f ? cpu : cpuEma_ * 0.9f + cpu * 0.1f;
        st.cpuMs.store(cpuEma_);
        float gpuFrame = 0, gpuGlass = 0;
        if (renderer_.PollGpuTimes(gpuFrame, gpuGlass)) {
            st.gpuFrameMs.store(gpuFrame);
            st.gpuGlassMs.store(gpuGlass);
            st.gpuValid.store(true);
        }
        st.activeMaterial.store(renderer_.LastUsedGpu() ? 0 : 1);
        st.backdropState.store(glass_.backdrop == BackdropKind::None ? 0 :
            glass_.backdrop == BackdropKind::Live ? backdrop_.State() : simBackdrop_.State());
        st.animating.store(anim || glass_.continuousRender);
    }
    TrackStats(now, dt, wasAnimating);
    animating_ = anim;
    if (!anim && !glass_.continuousRender) {
        FlushStats(now, L"animation settled");
        if (!tracking_ && !dragging_ && model_.Hover() <= 0.0f) SetFullRegion(false);
    }
}

void App::TrackStats(double now, double dt, bool animatingFrame) {
    if (!animatingFrame) return;
    if (statFrames_ == 0) {
        statStart_ = now;
        statMaxDt_ = 0.0;
        statDxgi_ = false;
    } else {
        statMaxDt_ = std::max(statMaxDt_, dt);
    }
    if (statFrames_ == 2) {
        // DXGI statistics describe the last displayed present; after an idle gap the first values are
        // stale, so the baseline is taken from the third frame of each measurement window.
        UINT presentRefresh = 0;
        statDxgi_ = SUCCEEDED(renderer_.QueryFrameStats(statPresent0_, presentRefresh, statSync0_));
    }
    ++statFrames_;
    statLast_ = now;
    if (now - statStart_ >= 1.0) FlushStats(now, L"1 s window");
}

void App::FlushStats(double now, const wchar_t* reason) {
    (void)now;
    if (statFrames_ < 2) {
        statFrames_ = 0;
        return;
    }
    const double span = statLast_ - statStart_;
    lastFps_ = span > 0 ? (statFrames_ - 1) / span : 0.0;
    GlassSettingsStore::Get().stats.fps.store(static_cast<float>(lastFps_));
    lastMaxMs_ = statMaxDt_ * 1000.0;
    std::wstring dxgi = L"n/a";
    UINT present = 0, presentRefresh = 0, sync = 0;
    const HRESULT hr = renderer_.QueryFrameStats(present, presentRefresh, sync);
    wchar_t buf[160];
    if (statDxgi_ && SUCCEEDED(hr)) {
        swprintf_s(buf, L"DXGI presents %u over %u vblanks (from frame 3)", present - statPresent0_, sync - statSync0_);
        dxgi = buf;
    } else {
        swprintf_s(buf, L"GetFrameStatistics 0x%08X", static_cast<unsigned>(hr));
        dxgi = buf;
    }
    const GlassStats& gs = GlassSettingsStore::Get().stats;
    DLOG(L"anim stats (%ls): %d frames in %.3f s = %.1f fps, max frame interval %.2f ms, refresh %.1f Hz, %ls; "
         L"material %ls, CPU %.3f ms, GPU frame %.3f ms (glass+blur %.3f ms)%ls",
         reason, statFrames_, span, lastFps_, lastMaxMs_, refreshHz_, dxgi.c_str(),
         renderer_.LastUsedGpu() ? L"Liquid" : L"Smoked", cpuEma_, gs.gpuFrameMs.load(), gs.gpuGlassMs.load(),
         gs.gpuValid.load() ? L"" : L" [GPU timing not yet valid]");
    statFrames_ = 0;
}

std::wstring App::DiagText() const {
    wchar_t buf[512];
    swprintf_s(buf,
               L"DockOver  tiles %d  dpi %.0f  refresh %.1f Hz  frames %llu\n"
               L"last anim %.1f fps  max interval %.2f ms  hover %.2f  region %ls\n"
               L"material: %ls | backdrop: %ls | GPU %.3f ms (glass %.3f) CPU %.2f ms",
               model_.Count(), dpi_, refreshHz_, totalFrames_, lastFps_, lastMaxMs_, model_.Hover(),
               regionFull_ ? L"full" : L"plate", renderer_.LastUsedGpu() ? L"Liquid Glass" : L"Smoked Glass",
               glass_.backdrop == BackdropKind::None ? L"none" : BackdropName(glass_.backdrop),
               GlassSettingsStore::Get().stats.gpuFrameMs.load(), GlassSettingsStore::Get().stats.gpuGlassMs.load(),
               cpuEma_);
    return buf;
}

void App::OnMouseMove(float x, float y) {
    if (resizing_) {
        POINT cursor{}; GetCursorPos(&cursor);
        const int size = std::clamp(resizeStartSize_ + static_cast<int>(std::lround((resizeStartY_ - cursor.y) / scale_)), (std::min)(24, resizeMaxSize_), resizeMaxSize_);
        if (size != static_cast<int>(model_.m.icon)) {
            model_.SetIconSize(static_cast<float>(size));
            mouseX_=x; mouseY_=y;
            needsFrame_=true; StartAnimating();
        }
        return;
    }
    mouseX_ = x;
    mouseY_ = y;
    if (sepPress_ >= 0 && !dragging_) {
        const float dx = x - pressX_, dy = y - pressY_;
        const float threshold = static_cast<float>(std::max(GetSystemMetricsForDpi(SM_CXDRAG, static_cast<UINT>(dpi_)), GetSystemMetricsForDpi(SM_CYDRAG, static_cast<UINT>(dpi_)))) / scale_;
        if (dx * dx + dy * dy >= threshold * threshold) {
            const int sep = sepPress_;
            if (std::fabs(dy) >= std::fabs(dx) || static_cast<size_t>(sep) >= items_.size() || items_[static_cast<size_t>(sep)].isSystemSeparator) { BeginSeparatorResize(); OnMouseMove(x, y); return; }
            sepPress_ = -1; pressed_ = sep;  // a sideways drag of the user's separator: the ordinary item drag below
        } else return;
    }
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
    }
    SetFullRegion(true);
    if(pressed_>=0 && !dragging_){
        const float dx=x-pressX_,dy=y-pressY_;
        const float threshold=static_cast<float>(std::max(GetSystemMetricsForDpi(SM_CXDRAG,static_cast<UINT>(dpi_)),GetSystemMetricsForDpi(SM_CYDRAG,static_cast<UINT>(dpi_))))/scale_;
        if(dx*dx+dy*dy>=threshold*threshold && static_cast<size_t>(pressed_)<items_.size() && !DragKey(static_cast<size_t>(pressed_)).empty()){
            // Every Dock item moves within its own section: the pinned apps with Launchpad and the user's separators,
            // the running apps, or the right-hand items (status tiles, Downloads, Recycle Bin; minimized windows stay put).
            auto section=[&](size_t i){const auto& it=items_[i];if(it.isSystemSeparator)return -1;if(IsTailItem(it))return 2;return it.isTransient?1:0;};
            std::vector<bool> bounds;for(size_t i=0;i<items_.size();++i)bounds.push_back(section(i)!=section(static_cast<size_t>(pressed_)));
            dragRegion_=FindDragRegion(bounds,pressed_);
            if(dragRegion_.Valid()){
                dragging_=true;removeOnDrop_=false;dragFrom_=dragTarget_=pressed_;dragId_=DragKey(static_cast<size_t>(pressed_));
                grabX_=pressX_-frame_.items[pressed_].cx;grabY_=pressY_-frame_.items[pressed_].top;
                model_.SetPointer(false,x,y);DLOG(L"drag: started index=%d region=%d..%d",dragFrom_,dragRegion_.first,dragRegion_.last);
            }
        }
    }
    if(dragging_){UpdateDragTarget(x,y);needsFrame_=true;StartAnimating();return;}
    const bool wasActive = model_.PointerActive();
    model_.SetPointer(true, x, y);
    if (model_.PointerActive() != wasActive) DLOG(L"hover %ls at (%.0f,%.0f)", wasActive ? L"end" : L"start", x, y);
    StartAnimating();
}

void App::OnMouseLeave() {
    tracking_ = false;
    if (AutoHiding()) leftAt_ = Now();
    if (model_.PointerActive()) DLOG(L"hover end (left window)");
    model_.SetPointer(false, 0, 0);
    StartAnimating();
}

int App::SeparatorAt(float x, float y) const {
    for (size_t i=0; i<frame_.items.size(); ++i) {
        const auto& g=frame_.items[i];
        if (g.separator && x>=g.tileLeft && x<g.tileRight && y>=frame_.dockTop && y<=frame_.dockBottom) return static_cast<int>(i);
    }
    return -1;
}
void App::FinishResize() {
    if (!resizing_) return;
    resizing_=false;
    ApplyWindowLayout(L"separator resize complete");
    const auto result=liquid::settings::WriteDockInt(opt_.desktopSettingsPath, "iconSizeV2", static_cast<int>(model_.m.icon));
    if (GetCapture()==hwnd_) ReleaseCapture();
    model_.SetPointer(false, 0, 0); needsFrame_=true; StartAnimating();
    if (result!=liquid::settings::SaveStatus::Saved)
        MessageBoxW(hwnd_, L"The Dock size could not be saved. It will remain in this session only.", L"DockOver", MB_OK|MB_ICONWARNING);
}
void App::OnLButtonDown(float x, float y) {
    if (const int sep = SeparatorAt(x,y); sep >= 0) {
        // Decided by the first movement: up / down resizes the Dock (macOS), sideways moves the user's separator.
        sepPress_ = sep; GetCursorPos(&sepPressCursor_); pressX_ = x; pressY_ = y; pressed_ = -1; SetCapture(hwnd_);
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return;
    }
    pressed_ = model_.HitTest(frame_, x, y);pressX_=x;pressY_=y;
    if(pressed_>=0)SetCapture(hwnd_);
    needsFrame_ = true;
}
void App::BeginSeparatorResize() {
    resizeStartY_=sepPressCursor_.y; resizeStartSize_=static_cast<int>(model_.m.icon);
    resizing_=true; pressed_=-1; sepPress_=-1; if (GetCapture()!=hwnd_) SetCapture(hwnd_);
    model_.ResetHover();
    ApplyWindowLayout(L"separator resize begin");
    SetFullRegion(true); needsFrame_=true; StartAnimating();
    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
}
void App::UpdateDragTarget(float x,float y){
    if(!dragging_ || !dragRegion_.Valid())return;
    DockFrame base;model_.Compute(base,Now());
    if(static_cast<size_t>(dragRegion_.last)>=base.items.size()){dropAllowed_=false;return;}
    const float left=dragRegion_.first>0?base.items[dragRegion_.first-1].cx:base.dockLeft;
    const float right=static_cast<size_t>(dragRegion_.last+1)<base.items.size()?base.items[dragRegion_.last+1].cx:base.dockRight;
    dropAllowed_=x>=left && x<right && y>=base.dockTop-model_.m.shadowPad && y<=base.dockBottom+model_.m.shadowPad;
    // macOS: an item held a good way above the Dock is removed on release (Launchpad and running apps stay).
    {   const auto& it=items_[static_cast<size_t>(dragFrom_)];
        const bool removable=!it.isLaunchpad&&!it.isTransient&&!it.isMinimized;
        const bool wasRemoving=removeOnDrop_;removeOnDrop_=removable&&y<base.dockTop-model_.m.icon*1.1f;
        if(removeOnDrop_!=wasRemoving)needsFrame_=true;}
    float nearest=1e30f;int target=dragFrom_;
    for(int i=dragRegion_.first;i<=dragRegion_.last;++i){float d=std::fabs(x-base.items[i].cx);if(d<nearest){nearest=d;target=i;}}
    dragTarget_=target;
}
void App::FinishDrag(bool commit){
    if(!dragging_)return;
    const int from=dragFrom_,to=dragTarget_;const bool valid=commit&&dropAllowed_&&from>=0&&to>=0&&
        static_cast<size_t>(from)<items_.size()&&DragKey(static_cast<size_t>(from))==dragId_;
    const bool remove=commit&&removeOnDrop_&&!dropAllowed_&&from>=0&&static_cast<size_t>(from)<items_.size()&&DragKey(static_cast<size_t>(from))==dragId_;
    const bool tail=from>=0&&static_cast<size_t>(from)<items_.size()&&IsTailItem(items_[static_cast<size_t>(from)]);
    removeOnDrop_=false;
    std::vector<float> positions,positionsY;positions.reserve(frame_.items.size());positionsY.reserve(frame_.items.size());
    for(const auto& g:frame_.items){positions.push_back(g.cx);positionsY.push_back(g.top);}
    if(from>=0&&static_cast<size_t>(from)<positions.size()){positions[from]=mouseX_-grabX_;positionsY[from]=mouseY_-grabY_;}
    bool saveFailed=false;
    if(valid&&from!=to){
        ReorderWithinRegion(items_,from,to,dragRegion_);ReorderWithinRegion(iconBmp_,from,to,dragRegion_);
        ReorderWithinRegion(shadowBmp_,from,to,dragRegion_);ReorderWithinRegion(shadowPad_,from,to,dragRegion_);
        ReorderWithinRegion(positions,from,to,dragRegion_);ReorderWithinRegion(positionsY,from,to,dragRegion_);
        SyncTiles();  // widths travel with their items: a half-width status group must not leave its width behind
        saveFailed=tail?!SaveTailOrder():!SavePinnedConfig(configPath_,items_);
        if(saveFailed)DLOG(L"drag: order save failed; in-memory order retained");
        else DLOG(L"drag: order saved %d -> %d",from,to);
    }else DLOG(L"drag: %ls",valid?L"unchanged":L"cancelled");
    dragging_=false;dropAllowed_=false;dragFrom_=dragTarget_=-1;dragId_.clear();pressed_=-1;
    DockFrame base;model_.Compute(base,Now());visualOffsets_.assign(base.items.size(),MakeOffsetSpring());visualYOffsets_.assign(base.items.size(),MakeOffsetSpring());
    for(size_t i=0;i<positions.size()&&i<base.items.size();++i){visualOffsets_[i].SetValue(positions[i]-base.items[i].cx);visualYOffsets_[i].SetValue(positionsY[i]-base.items[i].top);}
    if(GetCapture()==hwnd_)ReleaseCapture();
    model_.SetPointer(tracking_,mouseX_,mouseY_);needsFrame_=true;StartAnimating();
    if(saveFailed)MessageBoxW(hwnd_,L"The new order could not be saved. It will remain in this session only.",L"DockOver",MB_OK|MB_ICONWARNING);
    if(remove){DLOG(L"drag: removed from the Dock (dropped above it)");RemoveDockItem(from);}
}
void App::OnLButtonUp(float x, float y) {
    if (resizing_) { FinishResize(); return; }
    if (sepPress_ >= 0) { sepPress_ = -1; if (GetCapture() == hwnd_) ReleaseCapture(); return; }
    mouseX_=x;mouseY_=y;
    if(dragging_){UpdateDragTarget(x,y);FinishDrag(true);return;}
    const int hit = model_.HitTest(frame_, x, y);
    const int pressed = pressed_;
    pressed_ = -1;
    if (GetCapture() == hwnd_) ReleaseCapture();
    needsFrame_ = true;
    if (hit >= 0 && hit == pressed) ActivateItem(hit);
}

void App::ActivateItem(int index) {
    if (index < 0 || static_cast<size_t>(index) >= items_.size() || items_[index].isSeparator) return;
    if(items_[index].isMinimized){DLOG(L"click: minimized window");dock::minimize::Restore(items_[index].window);return;}
    if(items_[index].isLaunchpad){
        DLOG(L"click: Launchpad");dock::launchpad::Toggle(hwnd_,launchpadPath_,opt_.desktopSettingsPath);return;
    }
    if(items_[index].isRecycleBin){
        DLOG(L"click: Recycle Bin");
        ShellExecuteW(hwnd_,L"open",L"explorer.exe",L"shell:RecycleBinFolder",nullptr,SW_SHOWNORMAL);return;
    }
    if(items_[index].isStatusGroup){
        const auto& g=frame_.items[index];
        const int cell=dock::statusgroup::CellAt(D2D1::RectF(g.left,g.top,g.left+g.width,g.top+g.size),items_[index].statusCells,D2D1::Point2F(mouseX_,mouseY_));
        OpenStatusCell(index,cell<0?0:cell);return;
    }
    if(items_[index].isDownloads){
        const auto& g=frame_.items[index];RECT icon{windowRect_.left+static_cast<LONG>(g.left*scale_),windowRect_.top+static_cast<LONG>(g.top*scale_),windowRect_.left+static_cast<LONG>((g.left+g.size)*scale_),windowRect_.top+static_cast<LONG>((g.top+g.size)*scale_)};
        if(!dock::downloads::Toggle(hwnd_,icon,downloadsPath_,downloadsSettings_))MessageBoxW(hwnd_,L"Could not open Downloads.",L"DockOver",MB_OK|MB_ICONERROR);
        return;
    }
    PinnedItem& item = items_[index];
    // Non-executable files and document shortcuts use the Shell association directly.
    if(item.exePath.empty() && item.exeBase.empty() && item.aumid.empty() && !item.isTransient){  // V0.18: AUMID items are apps
        DLOG(L"click: opening associated item '%ls'",item.label.c_str());
        LaunchPinnedItemAsync(item);model_.StartBounce(index,Now());StartAnimating();return;
    }
    bool scanSucceeded = false;
    if (RefreshRunningState(items_, hwnd_, &scanSucceeded)) needsFrame_ = true;
    if (!scanSucceeded) {
        DLOG(L"click: window scan failed; no launch or activation for '%ls'", item.label.c_str());
        return;
    }
    if (item.running) {
        // V0.16: when the app is already in front, a click cycles to its bottom-most window (visits every window in turn);
        // otherwise the most recently used window is activated (restored first if minimized).
        PinnedItem target = item;
        const auto windows = AppWindows(item, hwnd_);
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0; if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == GetCurrentProcessId()) fg = g_lastExternalForeground;  // the click activated the Dock itself
        const bool appInFront = std::find(windows.begin(), windows.end(), fg) != windows.end();
        if (!windows.empty()) target.window = appInFront && windows.size() > 1 ? windows.back() : windows.front();
        const bool activated = ActivateAppWindow(target);
        DLOG(L"click: activation %ls for '%ls' (%zu windows%ls)", activated ? L"succeeded" : L"failed or denied", item.label.c_str(),
             windows.size(), appInFront && windows.size() > 1 ? L", cycled" : L"");
        return; // Never create a duplicate merely because Windows refused focus.
    }
    DLOG(L"click: launching '%ls'", item.label.c_str());
    LaunchPinnedItemAsync(item);
    model_.StartBounce(index, Now());
    StartAnimating();
}

void App::OnRButtonUp(float x, float y) {
    DLOG(L"rclick: up at %.1f,%.1f dragging=%d",x,y,dragging_?1:0);
    if(dragging_)return;
    int hit = model_.HitTest(frame_, x, y);
    if(hit>=0 && items_[hit].isLaunchpad) hit=-1;  // system item: show the Dock menu
    if(hit>=0 && items_[hit].isMinimized) return;  // a minimized window: click restores it
    if(hit>=0 && items_[hit].isRecycleBin){
        HMENU menu=CreatePopupMenu();const int full=RecycleBinFull();
        AppendMenuW(menu,MF_STRING,401,L"Open Recycle Bin");AppendMenuW(menu,MF_STRING|(full==0?MF_GRAYED:0),402,L"Empty Recycle Bin\u2026");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,403,L"Remove from Dock");
        POINT pt{};GetCursorPos(&pt);SetForegroundWindow(hwnd_);auto popupItems=liquid::popup::FromNative(menu);DestroyMenu(menu);
        liquid::popup::Show(hwnd_,pt,std::move(popupItems),[this](const liquid::menu::MenuItem& chosen){
            if(chosen.commandId==403){for(size_t i=0;i<items_.size();++i)if(items_[i].isRecycleBin){RemoveDockItem(static_cast<int>(i));break;}return;}
            if(chosen.commandId==401){ShellExecuteW(hwnd_,L"open",L"explorer.exe",L"shell:RecycleBinFolder",nullptr,SW_SHOWNORMAL);return;}
            if(chosen.commandId==402){const HRESULT hr=SHEmptyRecycleBinW(hwnd_,nullptr,0);  // flags 0: Windows shows its own confirmation
                DLOG(L"recycle bin: empty requested hr=0x%08X",static_cast<unsigned>(hr));recyclePoll_=3;}});
        return;
    }
    if(hit>=0 && items_[hit].isStatusGroup){
        // A tray icon kept in the Dock gets its app's own menu, like the real tray icon; other cells get the group menu.
        if(static_cast<size_t>(hit)<frame_.items.size()){const auto& g=frame_.items[hit];const auto& cells=items_[hit].statusCells;
            const int cell=dock::statusgroup::CellAt(D2D1::RectF(g.left,g.top,g.left+g.width,g.top+g.size),cells,D2D1::Point2F(x,y));
            if(cell>=0&&dock::statusgroup::TrayCell(cells[static_cast<size_t>(cell)])&&docky::statushost::ClickTray(cells[static_cast<size_t>(cell)],true,StatusCellScreenRect(hit,cell)))return;}
        ShowStatusGroupMenu(hit);return;}
    if(hit>=0 && items_[hit].isDownloads){
        HMENU menu=CreatePopupMenu(),display=CreatePopupMenu(),view=CreatePopupMenu(),sort=CreatePopupMenu();
        auto append=[](HMENU m,UINT id,const wchar_t* text,bool checked){AppendMenuW(m,MF_STRING|(checked?MF_CHECKED:0),id,text);};
        append(display,301,L"Stack",downloadsSettings_.display==dock::downloads::Display::Stack);append(display,302,L"Folder",downloadsSettings_.display==dock::downloads::Display::Folder);
        append(view,311,L"Fan",downloadsSettings_.view==dock::downloads::View::Fan);append(view,312,L"Grid",downloadsSettings_.view==dock::downloads::View::Grid);append(view,313,L"List",downloadsSettings_.view==dock::downloads::View::List);
        append(sort,321,L"Date",downloadsSettings_.sort==dock::downloads::Sort::Date);append(sort,322,L"Name",downloadsSettings_.sort==dock::downloads::Sort::Name);append(sort,323,L"Type",downloadsSettings_.sort==dock::downloads::Sort::Type);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(display),L"Display as");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"View content as");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(sort),L"Sort by");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,330,L"Remove from Dock");
        POINT pt{};GetCursorPos(&pt);SetForegroundWindow(hwnd_);auto popupItems=liquid::popup::FromNative(menu);DestroyMenu(menu);
        liquid::popup::Show(hwnd_,pt,std::move(popupItems),[this](const liquid::menu::MenuItem& chosen){
        const UINT cmd=chosen.commandId;auto next=downloadsSettings_;bool changed=true;
        if(cmd==330){for(size_t i=0;i<items_.size();++i)if(items_[i].isDownloads){RemoveDockItem(static_cast<int>(i));break;}return;}
        switch(cmd){case 301:next.display=dock::downloads::Display::Stack;break;case 302:next.display=dock::downloads::Display::Folder;break;
        case 311:next.view=dock::downloads::View::Fan;break;case 312:next.view=dock::downloads::View::Grid;break;case 313:next.view=dock::downloads::View::List;break;
        case 321:next.sort=dock::downloads::Sort::Date;break;case 322:next.sort=dock::downloads::Sort::Name;break;case 323:next.sort=dock::downloads::Sort::Type;break;default:changed=false;break;}
        if(changed){std::wstring error;if(!downloadsSettingsValid_||!dock::downloads::SaveSettings(downloadsSettingsPath_,next,error)){
            if(error.empty())error=downloadsSettingsError_;MessageBoxW(hwnd_,error.c_str(),L"Downloads settings",MB_OK|MB_ICONWARNING);
        }else{bool displayChanged=downloadsSettings_.display!=next.display;downloadsSettings_=next;dock::downloads::Close();
            if(displayChanged){for(auto& item:items_)if(item.isDownloads)ResolveItem(item);BuildIcons();}needsFrame_=true;}} });
        return;
    }
    // Separators (V0.4): the insert slot and the separator under the pointer come from the current
    // Dock geometry (tile centres), not a fixed index. Only the pinned section accepts new separators.
    int sepHit=-1,insertAt=-1;
    if(frame_.items.size()==items_.size()){
        for(size_t i=0;i<items_.size();++i)if(items_[i].isSeparator&&std::fabs(x-frame_.items[i].cx)<=12.0f){sepHit=static_cast<int>(i);break;}
        const size_t pinnedEnd=PinnedEnd();
        if(sepHit<0&&pinnedEnd<items_.size()&&x<frame_.items[pinnedEnd].cx){
            size_t at=pinnedEnd;for(size_t i=0;i<pinnedEnd;++i)if(frame_.items[i].cx>x){at=i;break;}
            if(at>0&&at<pinnedEnd&&!items_[at-1].isSeparator&&!items_[at].isSeparator)insertAt=static_cast<int>(at);
        }
    }
    DLOG(L"rclick: x=%.1f y=%.1f hit=%d sepHit=%d insertAt=%d frameItems=%zu items=%zu",x,y,hit,sepHit,insertAt,frame_.items.size(),items_.size());
    const size_t layoutCount=items_.size();
    HMENU menu = CreatePopupMenu();
    // Application section first, like the macOS Dock and the Windows taskbar jump list: Finder gets a new window
    // and Quick access folders; other apps get the recent documents Windows recorded for their AppUserModelID.
    std::vector<dock::jump::Entry> jumpEntries;
    if (hit >= 0 && sepHit < 0 && !items_[hit].isSeparator) {
        const bool finder = IsFinderSlot(items_[hit]);
        if (finder) {
            AppendMenuW(menu, MF_STRING, kMenuNewFinder, L"New Finder Window");
            jumpEntries = dock::jump::QuickAccessFolders(kJumpMax);
        } else {
            jumpEntries = dock::jump::RecentForApp(dock::jump::AppIdFor(items_[hit]), kJumpMax);
            if (items_[hit].running) AppendMenuW(menu, MF_STRING, kMenuNewWindow, L"New Window");
        }
        if (!jumpEntries.empty()) {
            HMENU sub = CreatePopupMenu();
            for (size_t j = 0; j < jumpEntries.size(); ++j) AppendMenuW(sub, MF_STRING, kMenuJumpBase + static_cast<UINT>(j), jumpEntries[j].label.c_str());
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), finder ? L"Quick Access" : L"Recent");
        }
        if (GetMenuItemCount(menu) > 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (sepHit >= 0) {
        AppendMenuW(menu, MF_STRING, kMenuRemoveSep, items_[sepHit].isSystemSeparator ? L"Remove Divider" : L"Remove Separator");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else {  // blank Dock area or an icon: the pointer's side of the nearest tile centre decides the slot
        AppendMenuW(menu, MF_STRING | (insertAt >= 0 ? 0 : MF_GRAYED), kMenuInsertSep, L"Insert Separator");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (hit >= 0) {
        AppendMenuW(menu, MF_STRING, kMenuOpen, (L"Open " + items_[hit].label).c_str());
        if (items_[hit].isTransient) AppendMenuW(menu, MF_STRING, kMenuKeep, L"Keep in Dock");
        else AppendMenuW(menu, MF_STRING, kMenuUnpin, L"Remove from Dock");
        AppendMenuW(menu, MF_STRING, kMenuRename, L"Rename…");
        AppendMenuW(menu, MF_STRING, kMenuChangeIcon, L"Change Icon…");
        AppendMenuW(menu, MF_STRING, kMenuEditIcon, L"Edit Icon…");
        AppendMenuW(menu, MF_STRING | (iconStore_.Find(items_[hit].stableId) ? 0 : MF_GRAYED), kMenuResetIcon, L"Reset Icon");
        AppendMenuW(menu, MF_STRING | (items_[hit].fileLocation.empty() ? MF_GRAYED : 0), kMenuLocation,
                    L"Open file location");
        if (items_[hit].running && !IsFinderSlot(items_[hit])) {  // macOS: Quit closes every window of the app
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuQuitApp, L"Quit");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    {   // Show in Dock: every removable system item, checked while it is in the Dock; choosing one adds or removes it.
        HMENU show = CreatePopupMenu();
        AppendMenuW(show, MF_STRING | (showDivider_ ? MF_CHECKED : 0), kMenuShowBase + 0, L"Divider");
        AppendMenuW(show, MF_STRING | (showDownloads_ ? MF_CHECKED : 0), kMenuShowBase + 1, L"Downloads");
        AppendMenuW(show, MF_STRING | (showTrash_ ? MF_CHECKED : 0), kMenuShowBase + 2, L"Recycle Bin");
        AppendMenuW(show, MF_SEPARATOR, 0, nullptr);
        for (UINT i = 0; i < ARRAYSIZE(kStatusMenuIds); ++i) {
            const bool on = std::find(statusCells_.begin(), statusCells_.end(), kStatusMenuIds[i]) != statusCells_.end();
            AppendMenuW(show, MF_STRING | (on ? MF_CHECKED : 0), kMenuShowBase + 10 + i, i == 0 ? L"Status (Battery, Wi-Fi, Volume)" : dock::statusgroup::CellLabel(kStatusMenuIds[i]).c_str());
            if (i == 2) AppendMenuW(show, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(show), L"Show in Dock");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kMenuApplications, L"Applications…");
    AppendMenuW(menu, MF_STRING, kMenuTaskManager, L"Task Manager");
    AppendMenuW(menu, MF_STRING, kMenuDockySettings, L"DockOver Settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSnapshot, L"Export Dock PNG (F5)");
    AppendMenuW(menu, MF_STRING, kMenuGlass, L"Glass Settings…");
    AppendMenuW(menu, MF_STRING, kMenuEditConfig, L"Edit pinned items…");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload (F3)");
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit Dock (Esc)");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    auto popupItems=liquid::popup::FromNative(menu);DestroyMenu(menu);
    const std::string selectedId=hit>=0?items_[hit].stableId:std::string{};
    liquid::popup::Show(hwnd_,pt,std::move(popupItems),[this,hit,selectedId,sepHit,insertAt,layoutCount,jumpEntries=std::move(jumpEntries)](const liquid::menu::MenuItem& chosen){
    const UINT cmd=chosen.commandId;
    if(cmd>=kMenuJumpBase&&cmd<kMenuJumpBase+jumpEntries.size()){dock::jump::Open(jumpEntries[cmd-kMenuJumpBase].parsingName,hwnd_);return;}
    if(cmd==kMenuApplications){OpenApplications();return;}
    if(cmd==kMenuTaskManager){const auto r=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL));DLOG(L"menu: Task Manager (%ld)",static_cast<long>(r));return;}
    if(cmd>=kMenuShowBase&&cmd<kMenuShowBase+3){
        const char* keys[3]={"showDivider","showDownloads","showTrash"};bool* fields[3]={&showDivider_,&showDownloads_,&showTrash_};
        const UINT k=cmd-kMenuShowBase;const bool value=!*fields[k];
        if(liquid::settings::WriteBool(opt_.desktopSettingsPath,"dock",keys[k],value)!=liquid::settings::SaveStatus::Saved){MessageBoxW(hwnd_,L"Could not save settings.",L"DockOver",MB_OK|MB_ICONWARNING);return;}
        *fields[k]=value;settingsRevision_=liquid::settings::ReadRevision(opt_.desktopSettingsPath);
        if(k==1&&!value)dock::downloads::Close();
        RebuildTail(value?L"shown":L"hidden");return;}
    if(cmd>=kMenuShowBase+10&&cmd<kMenuShowBase+10+ARRAYSIZE(kStatusMenuIds)){ToggleStatusCell(kStatusMenuIds[cmd-kMenuShowBase-10]);return;}
    if(cmd==kMenuDockySettings){  // the Desktop Manager owns the Settings window (single instance), like the menu bar's docky menu
        HANDLE ev=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\LiquidDesktopSettingsOpenV1");
        if(ev){SetEvent(ev);CloseHandle(ev);DLOG(L"menu: docky Settings requested");}
        else MessageBoxW(hwnd_,L"Start DockOver to open Settings.",L"DockOver",MB_OK|MB_ICONINFORMATION);
        return;}
    if(cmd==kMenuNewFinder){ShellExecuteW(hwnd_,L"open",L"explorer.exe",nullptr,nullptr,SW_SHOWNORMAL);DLOG(L"jump: new Finder window");return;}
    if(cmd==kMenuQuitApp){if(hit<0||static_cast<size_t>(hit)>=items_.size()||items_[hit].stableId!=selectedId)return;
        // Asks each window to close (WM_CLOSE), as the taskbar's "Close all windows" does: the app may still ask to save.
        const auto windows=AppWindows(items_[hit],hwnd_);
        for(HWND w:windows)PostMessageW(w,WM_CLOSE,0,0);
        DLOG(L"menu: quit '%ls' (%zu windows)",items_[hit].label.c_str(),windows.size());return;}
    if(cmd==kMenuNewWindow){if(hit<0||static_cast<size_t>(hit)>=items_.size()||items_[hit].stableId!=selectedId)return;
        LaunchPinnedItemAsync(items_[hit]);model_.StartBounce(hit,Now());StartAnimating();DLOG(L"jump: new window for '%ls'",items_[hit].label.c_str());return;}
    if(cmd==kMenuInsertSep||cmd==kMenuRemoveSep){
        if(items_.size()!=layoutCount||dragging_)return;  // the Dock changed while the menu was open
        if(cmd==kMenuInsertSep&&insertAt>0&&static_cast<size_t>(insertAt)<items_.size()){
            PinnedItem s;s.isSeparator=true;s.target=L"---";items_.insert(items_.begin()+insertAt,std::move(s));
            DLOG(L"separator: inserted at %d",insertAt);
        }else if(cmd==kMenuRemoveSep&&sepHit>=0&&static_cast<size_t>(sepHit)<items_.size()&&items_[sepHit].isSystemSeparator){
            RemoveDockItem(sepHit);return;  // the divider: hidden through the settings (Show in Dock adds it back)
        }else if(cmd==kMenuRemoveSep&&sepHit>=0&&static_cast<size_t>(sepHit)<items_.size()&&items_[sepHit].isSeparator&&!items_[sepHit].isSystemSeparator){
            items_.erase(items_.begin()+sepHit);DLOG(L"separator: removed at %d",sepHit);
        }else return;
        if(!SavePinnedConfig(configPath_,items_))DLOG(L"separator: config save FAILED");
        pressed_=-1;RefreshRunningState(items_,hwnd_);ApplyWindowLayout(L"separator");BuildIcons();needsFrame_=true;StartAnimating();return;
    }
    if((cmd==kMenuRename||cmd==kMenuOpen||cmd==kMenuUnpin||cmd==kMenuKeep||cmd==kMenuChangeIcon||cmd==kMenuEditIcon||cmd==kMenuResetIcon||cmd==kMenuLocation) &&
       (hit<0||static_cast<size_t>(hit)>=items_.size()||items_[hit].stableId!=selectedId))return;
    switch (cmd) {
    case kMenuOpen:
        ActivateItem(hit);
        break;
    case kMenuUnpin:
        DLOG(L"unpin '%ls'", items_[hit].label.c_str());
        items_.erase(items_.begin() + hit);
        if (!SavePinnedConfig(configPath_, items_)) DLOG(L"unpin: config save FAILED");
        ApplyWindowLayout(L"unpin");
        BuildIcons();
        break;
    case kMenuKeep:
        items_[hit].isTransient = false;  // moves to the end of the pinned section and is persisted
        NormalizeSections();
        DLOG(L"keep in dock saved=%d", SavePinnedConfig(configPath_, items_));
        RefreshRunningState(items_, hwnd_);ApplyWindowLayout(L"keep in dock");BuildIcons();needsFrame_ = true;
        break;
    case kMenuEditIcon:
        OpenIconEditor(hit);
        break;
    case kMenuRename:
        RenameItem(items_[hit]);
        break;
    case kMenuChangeIcon:
        ChangeIcon(hit, false);
        break;
    case kMenuResetIcon:
        ChangeIcon(hit, true);
        break;
    case kMenuLocation:
        OpenFileLocation(items_[hit]);
        break;
    case kMenuEditConfig:
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + configPath_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case kMenuReload:
        Reload();
        break;
    case kMenuSnapshot:
        renderer_.RequestPngSnapshot(); needsFrame_=true;
        break;
    case kMenuGlass:
        DLOG(L"opening Glass Studio from context menu");
        OpenGlassStudio(hwnd_);
        break;
    case kMenuQuit:
        DLOG(L"quit from context menu");
        DestroyWindow(hwnd_);
        break;
    default:
        break;
    }
    });
}

void App::OnKey(WPARAM key) {
    if(dragging_ && key!=VK_ESCAPE)return;
    switch (key) {
    case VK_ESCAPE:
        if(dragging_){FinishDrag(false);return;}
        // V0.16: Esc dismisses this Dock's context menu; it must never also quit the Dock.
        if(OwnPopupVisible()){DLOG(L"Escape: closing context menu");liquid::popup::Close();return;}
        // Never quit on Esc: the Dock takes keyboard focus whenever another DockOver window (Applications, a popup)
        // closes, so an Esc meant for that window quit the whole of DockOver (the Manager stops every component when
        // the Dock exits normally). Quitting is the "Quit" menu item.
        DLOG(L"Escape pressed; ignored (Esc never quits the Dock)");
        break;
    case VK_F1:
        diag_ = !diag_;
        DLOG(L"F1 diagnostics overlay %ls", diag_ ? L"on" : L"off");
        needsFrame_ = true;
        break;
    case VK_F2:
        bounds_ = !bounds_;
        DLOG(L"F2 bounds %ls", bounds_ ? L"on" : L"off");
        needsFrame_ = true;
        break;
    case VK_F3:
        Reload();
        break;
    case VK_F4:
        DumpState(L"F4");
        break;
    case VK_F5:
        renderer_.RequestPngSnapshot(); needsFrame_=true;
        break;
    default:
        break;
    }
}

void App::OnDrop(HDROP drop) {
    POINT pt{};
    DragQueryPoint(drop, &pt);
    std::vector<std::wstring> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) { wchar_t path[MAX_PATH * 2]; if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) paths.push_back(path); }
    DragFinish(drop);
    const int hit=model_.HitTest(frame_,Dip(pt.x),Dip(pt.y));
    if(DropKindAt(hit)==DropKind::Launchpad)AddToLaunchpad(paths);else PinPaths(paths, Dip(pt.x));
}

namespace {
enum LaunchpadMenuCommand : UINT { kLpOpen = 1, kLpDock, kLpChangeIcon, kLpEditIcon, kLpResetIcon, kLpLocation, kLpRename };
}

// Right-click in Launchpad. The menu is the Dock's popup (shown without activation, so Launchpad stays open);
// commands that open a dialog or Explorer first close Launchpad, which covers the whole screen.
namespace {
INT_PTR CALLBACK RenameProc(HWND dialog,UINT message,WPARAM w,LPARAM l){
    if(message==WM_INITDIALOG){
        auto* value=reinterpret_cast<std::wstring*>(l);
        SetWindowLongPtrW(dialog,DWLP_USER,l);SetWindowTextW(dialog,L"Rename icon");
        RECT box{};GetClientRect(dialog,&box);const int width=box.right,height=box.bottom;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        auto control=[&](const wchar_t* cls,const wchar_t* title,DWORD style,int x,int y,int cx,int cy,int id){
            const HWND h=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,x,y,cx,cy,dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            return h;
        };
        auto field=control(L"EDIT",value->c_str(),WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,12,12,width-24,25,100);
        control(L"BUTTON",L"Save",BS_DEFPUSHBUTTON|WS_TABSTOP,width-166,height-38,70,26,IDOK);
        control(L"BUTTON",L"Cancel",WS_TABSTOP,width-86,height-38,74,26,IDCANCEL);
        SendMessageW(field,EM_SETLIMITTEXT,128,0);SetFocus(field);SendMessageW(field,EM_SETSEL,0,-1);
        return FALSE;
    }
    if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(w)==IDCANCEL)){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(w)==IDOK){
        wchar_t text[129]{};GetDlgItemTextW(dialog,100,text,ARRAYSIZE(text));std::wstring value=text;
        const auto first=value.find_first_not_of(L" \t");if(first==std::wstring::npos){MessageBeep(MB_ICONWARNING);return TRUE;}
        *reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog,DWLP_USER))=value.substr(first,value.find_last_not_of(L" \t")-first+1);
        EndDialog(dialog,IDOK);return TRUE;
    }
    return FALSE;
}
}
void App::RenameItem(const PinnedItem& item){
    const std::string id=item.stableId;
    auto name=item.label;
    alignas(DWORD) struct { DLGTEMPLATE dialog; WORD menu,windowClass,title; } spec{};
    spec.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;
    spec.dialog.cx=216;spec.dialog.cy=60;
    if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&spec.dialog,hwnd_,RenameProc,reinterpret_cast<LPARAM>(&name))!=IDOK)return;
    std::wstring error;
    if(!iconStore_.SetLabel(id,name,error)){MessageBoxW(hwnd_,error.c_str(),L"Rename icon",MB_OK|MB_ICONWARNING);return;}
    for(auto& tile:items_)if(tile.stableId==id)tile.label=name;
    dock::launchpad::InvalidateIcon(id);
    needsFrame_=true;StartAnimating();
}

void App::ShowLaunchpadAppMenu(const dock::launchpad::AppMenuRequest& request) {
    if(!IsWindow(request.owner)){dock::launchpad::MenuClosed();return;}
    if(request.folder>=0){
        HMENU menu=CreatePopupMenu();
        if(!menu){dock::launchpad::MenuClosed();return;}
        AppendMenuW(menu,MF_STRING,kLpRename,L"Rename…");
        auto items=liquid::popup::FromNative(menu);DestroyMenu(menu);
        if(!liquid::popup::Show(request.owner,request.screen,std::move(items),
            [owner=request.owner,folder=request.folder](const liquid::menu::MenuItem&){dock::launchpad::RenameFolderFromMenu(owner,folder);},
            []{dock::launchpad::MenuClosed();}))dock::launchpad::MenuClosed();
        return;
    }
    PinnedItem app;
    app.target = request.target;
    ResolveItem(app);
    if (!request.name.empty()) app.label = request.name;
    if (app.stableId.empty()) { dock::launchpad::MenuClosed(); return; }
    int inDock = -1;
    for (size_t i = 0; i < items_.size(); ++i)
        if (!items_[i].isSeparator && items_[i].stableId == app.stableId) { inDock = static_cast<int>(i); break; }
    const bool pinned = inDock >= 0 && !items_[inDock].isTransient;
    const bool custom = iconStore_.Find(app.stableId) || iconStore_.FindEdit(app.stableId);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kLpOpen, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLpDock, pinned ? L"Remove from Dock" : L"Keep in Dock");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLpRename, L"Rename…");
    AppendMenuW(menu, MF_STRING, kLpChangeIcon, L"Change Icon…");
    AppendMenuW(menu, MF_STRING, kLpEditIcon, L"Edit Icon…");
    AppendMenuW(menu, MF_STRING | (custom ? 0 : MF_GRAYED), kLpResetIcon, L"Reset Icon");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (app.fileLocation.empty() ? MF_GRAYED : 0), kLpLocation, L"Show in File Explorer");
    auto popupItems = liquid::popup::FromNative(menu);
    DestroyMenu(menu);
    DLOG(L"launchpad menu: '%ls' pinned=%d custom=%d", app.label.c_str(), pinned ? 1 : 0, custom ? 1 : 0);
    const std::string appId = request.appId;
    const HWND shown = liquid::popup::Show(request.owner, request.screen, std::move(popupItems),
        [this, app, appId](const liquid::menu::MenuItem& chosen) {
            switch (chosen.commandId) {
            case kLpOpen:
                dock::launchpad::OpenApp(appId);
                break;
            case kLpDock: {
                int at = -1;  // looked up again: the Dock may have changed while the menu was open
                for (size_t i = 0; i < items_.size(); ++i)
                    if (!items_[i].isSeparator && items_[i].stableId == app.stableId) { at = static_cast<int>(i); break; }
                if (at >= 0 && !items_[at].isTransient) {
                    DLOG(L"launchpad menu: remove '%ls' from the Dock", items_[at].label.c_str());
                    items_.erase(items_.begin() + at);
                } else if (at >= 0) {
                    items_[at].isTransient = false;  // a running app: moves to the end of the pinned section
                    NormalizeSections();
                } else {
                    // End of the pinned section: before the system separator (Downloads, Recycle Bin), as dropped items.
                    auto end = items_.begin() + static_cast<std::ptrdiff_t>(PinnedEnd());
                    DLOG(L"launchpad menu: keep '%ls' in the Dock", app.label.c_str());
                    items_.insert(end, app);
                }
                if (!SavePinnedConfig(configPath_, items_)) DLOG(L"launchpad menu: config save FAILED");
                RefreshRunningState(items_, hwnd_);
                ApplyWindowLayout(L"launchpad menu");
                BuildIcons();
                needsFrame_ = true;
                StartAnimating();
                break;
            }
            case kLpRename: case kLpChangeIcon: case kLpEditIcon: case kLpResetIcon: case kLpLocation:
                launchpadIconItem_ = app;
                launchpadIconCommand_ = chosen.commandId;
                dock::launchpad::Dismiss();
                SetTimer(hwnd_, kTimerLaunchpadIcon, 100, nullptr);
                break;
            default: break;
            }
        },
        [] { dock::launchpad::MenuClosed(); });
    if (!shown) dock::launchpad::MenuClosed();
}

// The Applications window (Windows key) is resident: started hidden a few seconds after the Dock (--resident
// --background) and then only toggled with a message, so it appears at once instead of after a process start.
static HWND ApplicationsWindow(HANDLE process){
    if(!process || WaitForSingleObject(process,0)!=WAIT_TIMEOUT)return nullptr;
    struct Find{DWORD pid;HWND found;}find{GetProcessId(process),nullptr};
    EnumWindows([](HWND win,LPARAM data)->BOOL{auto& f=*reinterpret_cast<Find*>(data);DWORD owner=0;GetWindowThreadProcessId(win,&owner);
        wchar_t title[64]{};GetWindowTextW(win,title,64);
        if(owner==f.pid&&GetWindow(win,GW_OWNER)==nullptr&&!wcscmp(title,L"DockOver Applications")){f.found=win;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&find));
    return find.found;
}
bool App::StartApplications(bool background){
    if(applicationsProcess_){CloseHandle(applicationsProcess_);applicationsProcess_=nullptr;}
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    std::wstring exe(path);exe.resize(exe.find_last_of(L"\\/")+1);exe+=L"applications-menu\\DockOverApplications.exe";
    std::wstring command=L"\""+exe+L"\" --resident";if(background)command+=L" --background";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION proc{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&proc))return false;
    applicationsProcess_=proc.hProcess;CloseHandle(proc.hThread);
    DLOG(L"Applications: started %ls",background?L"in the background":L"visible");
    return true;
}
void App::OpenApplications(){
    if(applicationsProcess_ && WaitForSingleObject(applicationsProcess_,0)==WAIT_TIMEOUT){
        HWND win=ApplicationsWindow(applicationsProcess_);
        for(int i=0;!win&&i<25;++i){Sleep(20);win=ApplicationsWindow(applicationsProcess_);}  // still starting
        if(win){
            AllowSetForegroundWindow(GetProcessId(applicationsProcess_));
            PostMessageW(win,RegisterWindowMessageW(L"DockOverApplicationsToggle"),0,0);
            return;
        }
    }
    if(!StartApplications(false))MessageBoxW(hwnd_,L"Could not open the DockOver Applications menu.",L"DockOver",MB_ICONERROR);
}

void App::RunLaunchpadIconCommand() {
    const UINT command = launchpadIconCommand_;
    launchpadIconCommand_ = 0;
    const PinnedItem item = launchpadIconItem_;
    switch (command) {
    case kLpRename: RenameItem(item); break;
    case kLpChangeIcon: ChangeIconFor(item, false); break;
    case kLpEditIcon: OpenIconEditorFor(item); break;
    case kLpResetIcon: ChangeIconFor(item, true); break;
    case kLpLocation: OpenFileLocation(item); break;
    default: break;
    }
}

void App::AddToLaunchpad(const std::vector<std::wstring>& paths){
    std::wstring error;const int added=dock::apps::AddFilesToLaunchpad(paths,dock::apps::ExtraAppsDir(),error);
    if(added)dock::launchpad::RefreshApps();
    DLOG(L"launchpad drop: added %d of %zu",added,paths.size());
    if(!error.empty())MessageBoxW(hwnd_,error.c_str(),L"Add to Launchpad",MB_OK|MB_ICONWARNING);
}

void App::PinPaths(const std::vector<std::wstring>& paths, float x) {
    int insert = 0;
    for (const ItemGeom& g : frame_.items)
        if (g.cx < x) ++insert;
    // The last separator reserves the Downloads system area; all dropped items stay to its left.
    insert=std::min(insert,static_cast<int>(PinnedEnd()));
    int added = 0;
    for (const auto& pathString : paths) {
        const wchar_t* path = pathString.c_str();
        const DWORD attributes=GetFileAttributesW(path);
        if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)){
            DLOG(L"pin: ignored missing item or directory '%ls'",path);continue;
        }
        PinnedItem item;
        item.target = path;
        ResolveItem(item);
        if (std::any_of(items_.begin(), items_.end(), [&](const PinnedItem& e) { return !e.isSeparator && e.stableId == item.stableId; })) {
            DLOG(L"pin: '%ls' already in Dock", item.label.c_str());
            continue;
        }
        DLOG(L"pin '%ls' at %d", item.label.c_str(), insert);
        ++added;
        items_.insert(items_.begin() + std::min<size_t>(insert, items_.size()), std::move(item));
        ++insert;
    }
    if (!added) return;
    if (!SavePinnedConfig(configPath_, items_)) DLOG(L"pin: config save FAILED");
    RefreshRunningState(items_, hwnd_);
    ApplyWindowLayout(L"pin");
    BuildIcons();
}

void App::Reload() {
    DLOG(L"reload requested");
    LoadItems();
    RefreshRunningState(items_, hwnd_);
    ApplyWindowLayout(L"reload");
    BuildIcons();
}

void App::DumpState(const wchar_t* reason) {
    DLOG(L"state (%ls): window (%ld,%ld)-(%ld,%ld) dpi %.0f region %ls hover %.3f animating %d frames %llu", reason,
         windowRect_.left, windowRect_.top, windowRect_.right, windowRect_.bottom, dpi_,
         regionFull_ ? L"full" : L"plate", model_.Hover(), animating_ ? 1 : 0, totalFrames_);
    DLOG(L"  dock plate DIP (%.1f,%.1f)-(%.1f,%.1f)", frame_.dockLeft, frame_.dockTop, frame_.dockRight,
         frame_.dockBottom);
    for (size_t i = 0; i < items_.size() && i < frame_.items.size(); ++i) {
        const ItemGeom& g = frame_.items[i];
        if (g.separator) {
            DLOG(L"  [%zu] separator cx %.2f tile %.2f-%.2f line %.1f-%.1f", i, g.cx, g.tileLeft, g.tileRight, g.lineTop,
                 g.lineBottom);
            continue;
        }
        DLOG(L"  [%zu] '%ls' cx %.2f size %.2f scale %.3f top %.2f bottom %.2f tile %.2f-%.2f running %d icon %ls", i,
             items_[i].label.c_str(), g.cx, g.size, g.size / model_.m.icon, g.top, g.top + g.size, g.tileLeft,
             g.tileRight, items_[i].running ? 1 : 0, iconBmp_[i] ? L"yes" : L"placeholder");
    }
}

bool App::LowLatActive() const {
    const auto* d = backdrop_.Duplication();
    return lowLat_ && ownedMonitor_ && d && d->Owned() && d == &docky::desk::SharedDesktop::ForMonitor(ownedMonitor_);
}

// Every capture request goes through here: with --lowlat the Dock's render device becomes the owner of that monitor's
// shared duplication before the backdrop starts reading it (moving to another monitor hands the old one back).
void App::RequestBackdrop(bool enabled, HMONITOR monitor) {
    if (lowLat_ && enabled && monitor && renderer_.Device() && monitor != ownedMonitor_) {
        ReleaseCaptureOwner();
        docky::desk::SharedDesktop::ForMonitor(monitor).SetOwner(renderer_.Device());
        ownedMonitor_ = monitor;
    }
    backdrop_.Request(enabled, monitor);
}

// Switches the one-frame path on or off while running. Off hands the duplication back to SharedDesktop's background
// thread (every other glass surface keeps its frames) and the next frame draws through the standard path.
void App::SetLowLatency(bool on, const wchar_t* why) {
    if (on == lowLat_) return;
    if (!on) {
        ReleaseCaptureOwner();
        lowLat_ = false; backdropDirty_ = false; renderer_.InvalidateUi();
        latencyLogEvery_ = latencyLogForced_ ? 10.0 : 0.0;
    } else {
        lowLat_ = true;
        if (!ownerTimer_) ownerTimer_ = docky::vblank::CreatePaceTimer();
        if (latencyLogEvery_ <= 0) latencyLogEvery_ = 60.0;
        if (hwnd_ && renderer_.Device()) RequestBackdrop(CaptureWanted(), MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY));
    }
    needsFrame_ = true;
    DLOG(L"lowlat: one-frame DXGI glass path %ls (%ls)", on ? L"on" : L"off", why);
}

void App::ReleaseCaptureOwner() {
    if (!ownedMonitor_) return;
    docky::desk::SharedDesktop::ForMonitor(ownedMonitor_).SetOwner(nullptr);  // its background thread takes over
    ownedMonitor_ = nullptr;
}

// Idle loop: wake 0.4 ms after the next display refresh, when DWM has just handed out the frame it composed (frames
// come at refresh boundaries, aligned to the last frame's LastPresentTime).
double App::NextOwnedFrameDelay() const {
    const double period = docky::vblank::FramePeriod().load();
    const auto* d = backdrop_.Duplication();
    const int64_t lp = d ? d->LastPresentQpc() : 0;
    if (!lp || period <= 0) return period > 0 ? period : 0.004;
    LARGE_INTEGER f{}, n{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&n);
    const double since = static_cast<double>(n.QuadPart - lp) / f.QuadPart;
    double next = std::ceil((since - 0.0004) / period) * period + 0.0004 - since;
    if (next < 0.0002) next += period;
    return next;
}

// Desktop-to-Dock latency: for the first frame that showed each new desktop image, the time from that image's
// LastPresentTime (DWM put it on screen) to this frame reaching the screen (DXGI frame statistics). 1 refresh is the
// floor for any capture; the earlier pipeline measured ~2.8 refreshes, the one-frame path ~1.8 (lab, 240 Hz).
void App::TrackLatency(bool force) {
    if (latencyLogEvery_ <= 0) return;  // standard builds: no counter, no log lines
    const int64_t lp = backdrop_.PublishedLastPresentQpc();
    UINT last = 0, shown = 0; LONGLONG shownQpc = 0;
    const bool ok = renderer_.PresentTiming(last, shown, shownQpc);
    if (!force && ok && lp && lp != latencyLastLp_) { latencyPending_[last] = lp; latencyLastLp_ = lp; }
    if (ok) {
        const auto it = latencyPending_.find(shown);
        if (it != latencyPending_.end() && shownQpc > it->second) {
            LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
            const double ms = static_cast<double>(shownQpc - it->second) * 1000.0 / f.QuadPart;
            const double periodMs = docky::vblank::FramePeriod().load() * 1000.0;
            if (ms < 500) { latencySumMs_ += ms; latencyMaxMs_ = (std::max)(latencyMaxMs_, ms); ++latencyN_;
                            ++latencyHist_[periodMs > 0 ? static_cast<int>(std::floor(ms / periodMs + 0.5)) : 0]; }
        }
        latencyPending_.erase(latencyPending_.begin(), latencyPending_.upper_bound(shown));
    }
    while (latencyPending_.size() > 64) latencyPending_.erase(latencyPending_.begin());
    const double t = Now();
    if (!latencyLogAt_) latencyLogAt_ = t;
    if (latencyN_ && (force || t - latencyLogAt_ >= latencyLogEvery_)) {
        const double periodMs = docky::vblank::FramePeriod().load() * 1000.0;
        std::wstring hist; for (const auto& [k, n] : latencyHist_) { wchar_t b[32]; swprintf_s(b, L"%d:%llu ", k, static_cast<unsigned long long>(n)); hist += b; }
        DLOG(L"glass latency (%ls): %llu new desktop images, desktop shown -> Dock shown %.2f ms avg (%.2f refreshes), %.2f ms max; refreshes histogram %ls",
             LowLatActive() ? L"one-frame path" : L"standard path", static_cast<unsigned long long>(latencyN_), latencySumMs_ / latencyN_,
             periodMs > 0 ? latencySumMs_ / latencyN_ / periodMs : 0.0, latencyMaxMs_, hist.c_str());
        if (lowLat_) DLOG(L"lowlat: ui layer drawn %llu (%.3f ms avg), composed %llu (%.3f ms cpu avg), full renders %llu, waited for a frame %llu (%.3f ms avg)",
             static_cast<unsigned long long>(llPrepared_), llPrepared_ ? llUiMs_ / llPrepared_ : 0.0, static_cast<unsigned long long>(llComposed_),
             llComposed_ ? llComposeCpu_ / llComposed_ : 0.0, static_cast<unsigned long long>(llFull_), static_cast<unsigned long long>(llWaits_),
             llWaits_ ? llWaitMs_ / llWaits_ : 0.0);
        if (const auto* d = backdrop_.Duplication()) {
            const uint64_t acq = d->acquired_, acc = d->accumulated_;
            DLOG(L"glass latency: desktop frames taken %llu, composed by Windows meanwhile %llu, waits that found no frame %llu",
                 static_cast<unsigned long long>(acq - llAcq0_), static_cast<unsigned long long>(acc - llAccum0_), static_cast<unsigned long long>(llMisses_));
            llAcq0_ = acq; llAccum0_ = acc;
        }
        latencySumMs_ = latencyMaxMs_ = 0; latencyN_ = 0; latencyHist_.clear(); latencyLogAt_ = t;
        llUiMs_ = llWaitMs_ = llComposeCpu_ = 0; llPrepared_ = llComposed_ = llFull_ = llWaits_ = llMisses_ = 0;
    }
}

bool App::RecreateDevice() {
    ReleaseCaptureOwner();  // the duplication lived on the lost device; the next RequestBackdrop moves it to the new one
    backdrop_.SetDevice(nullptr);
    renderer_.Shutdown();
    const HRESULT hr = renderer_.Init(hwnd_, pxW_, pxH_, dpi_);
    DLOG(L"device recreate 0x%08X", hr);
    if (FAILED(hr)) return false;
    simBackdrop_.SetDevice(renderer_.Device());
    backdrop_.SetDevice(renderer_.Device());
    liquid::popup::SetRenderDevice(renderer_.Device());  // popups opened from now on use the new device
    dock::downloads::SetFanDevice(renderer_.Device());
    BuildIcons();
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    (void)liquidBuildMarker[0];
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // --dev-isolated (tests only): a second Dock beside the user's copy, with its own instance lock.
    const bool isolated = wcsstr(GetCommandLineW(), L"--dev-isolated") != nullptr;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, isolated ? L"Local\\LiquidDockV01DevIsolated" : L"Local\\LiquidDockV01SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 3;
    }
    // Tests only (--dev-isolated): DOCKY_POPUP_CAPTURE_TEST=1 leaves popups visible to screen captures (they then show the
    // fallback material instead of live glass), so a test can screenshot Hidden Icons and other Dock popups.
    if (wchar_t v[4]{}; isolated && GetEnvironmentVariableW(L"DOCKY_POPUP_CAPTURE_TEST", v, 4) && v[0] == L'1') liquid::popup::SetVisualCaptureTest(true);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HRESULT ro = RoInitialize(RO_INIT_SINGLETHREADED);
    dock::PrewarmBorderlessCapture();  // answer ready before the first capture (no yellow border flash)
    int code = 0;
    {
        App app;
        code = app.Run(instance, ParseArgs());
    }
    if (SUCCEEDED(ro)) RoUninitialize();
    if (SUCCEEDED(co)) CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
