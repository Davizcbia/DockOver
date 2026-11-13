#pragma once
// GlassParameters: the single parameter model shared by Glass Studio (writer), the render thread
// (reader, one consistent snapshot per frame) and the GPU (constant buffer). Keys of parameters that
// exist in Glass Lab use the Lab's JSON (version 3) names so Lab exports can be pasted directly.
// See dockui/docs/p0.3-glass-lab-port.md for the mapping table.
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dock {

enum class MaterialKind : int { Liquid = 0, Smoked = 1 };
enum class BackdropKind : int { Wallpaper = 0, Grid = 1, Gradient = 2, Dark = 3, None = 4, Live = 5 };

struct GlassParameters {
    // --- Glass Lab parameters (Lab key in comments where it differs) ---
    float radius = 22.0f;           // Lab "radius"/"corners"; Dock geometry default (macOS-measured)
    float bevelWidth = 16.0f;
    float refraction = 1.0f;        // Lab ini "lens"
    float refractiveIndex = 1.45f;
    float dispersion = 0.035f;
    float magnification = 1.0f;
    float frost = 0.45f;
    float blurSpread = 0.85f;
    float density = 0.3f;
    float glassLift = 1.0f;
    float saturation = 1.1f;
    float brightness = 1.0f;
    float strokeWidth = 0.3f;       // Lab ini "stroke"
    float strokeOpacity = 0.8f;
    float glare = 0.4f;             // Lab ini "reflection"
    float staticTopLight = 1.0f;
    float staticBottomLight = 1.0f;
    float staticBottomGlow = 1.0f;
    float edgeAmount = 1.0f;
    float fresnelAmount = 1.0f;
    float specularAmount = 1.0f;
    float lightAngle = -120.0f;
    float shadowAmount = 1.0f;
    float interactionStrength = 1.0f;
    float interactionRadius = 70.0f;
    float responseTime = 80.0f;
    // docky additions (Glass Studio sliders, persisted):
    float highlightDim = 1.0f;    // dark glass: how strongly bright backdrop areas are pulled toward grey and compressed (0 = off)
    float backdropBlur = 0.9f;    // blur of the desktop behind a whole popup (menu bar popups surface); 0 = none
    // "Sliders & switches" category: a pressed thumb grows to these multiples of its resting size (Glass Lab: switch
    // 74x44 -> 104x72, slider 40x30 -> 50x36) while it turns into the lens.
    float switchPressWidth = 1.405f, switchPressHeight = 1.636f;
    float sliderPressWidth = 1.25f, sliderPressHeight = 1.2f;
    // LiquidGlass.qml per-surface properties (not part of the Lab tuning JSON)
    float opacity = 1.0f;
    float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
    float tintStrength = 0.0f;
    bool staticReflection = true;
    bool dark = true;
    bool reducedTransparency = false;
    // Per-draw shaping, set by code, never persisted: 1 / 0 reproduce the original material exactly.
    float bodyTone = 1.0f;       // 0 = pure blurred backdrop without the glass body tone
    float edgeSoftness = 0.0f;   // DIP; > 0 defocuses the plate outline (open transitions)
    // --- Dock-only settings (not in Glass Lab) ---
    MaterialKind material = MaterialKind::Liquid;
    BackdropKind backdrop = BackdropKind::Wallpaper;  // simulated by default
    int blurDownsample = 2;                            // 2 = Lab's half resolution, 4 = quarter
    int debugView = 0;                                 // 0 off, 1 SDF, 2 normals, 3 refraction, 4 blur, 5 sharp, 6 frost
    bool continuousRender = false;                     // benchmark: render every vblank
};

// Surface roles. Every glass surface is the shared base material (the Glass Studio tuning in a
// process that loads it, Glass Lab defaults elsewhere) plus the few values its size and job need.
// Callers still set per-instance geometry (radius) themselves.
enum class GlassRole { Dock, DockLabel, Panel, Menu, ControlModule, PopupModule, FanCapsule, BackdropBlur };
GlassParameters RoleGlass(GlassRole role, const GlassParameters& base);
// Base = GlassSettingsStore::Get().Current(); Menu, ControlModule and PopupModule use their user-tunable categories.
GlassParameters RoleGlass(GlassRole role);

// The four separately tunable glass categories (Glass Studio "Category"). Every surface of a category reads the same set:
//   Dock          - the Dock plate, labels, Downloads fan / grid, Launchpad (the store's current set)
//   Menus         - every unified menu in every process: Dock right-click, widget / Stacks right-click, docky Desktop tray,
//                   menu bar dropdowns (docky menu, Hidden Icons)
//   MenuBarPanels - popups opened from menu bar items (Wi-Fi, Bluetooth, sound, battery) and the widget gallery / settings
//   ControlCenter - the Control Center panel and its modules
// The non-Dock sets live under "surfaces" in glass.json and fall back to GlassSurfaceDefault until edited.
enum class GlassSurface : int { Dock = 0, Menus = 1, MenuBarPanels = 2, ControlCenter = 3, Controls = 4 };
constexpr int kGlassSurfaceCount = 5;
const wchar_t* GlassSurfaceName(GlassSurface s);
const char* GlassSurfaceKey(GlassSurface s);  // glass.json key
// Built-in defaults: the tuning the user settled on (glass.json, 2026-11-12).
GlassParameters GlassSurfaceDefault(GlassSurface s);
GlassParameters DockDefault();  // == GlassSurfaceDefault(GlassSurface::Dock)

// Glass appears by materializing, not by a plain alpha fade: for t in 0..1 the blur, refraction,
// body tone and highlights grow with t while coverage arrives early (MaterializeAlpha).
float MaterializeAlpha(float t);
void Materialize(GlassParameters& p, float t);

struct FloatParam {
    const char* key;
    const wchar_t* label;
    const wchar_t* group;
    float minValue, maxValue;
    float GlassParameters::*field;
    const wchar_t* note;  // shown in Studio (e.g. "dynamic reflection only")
    bool labKey;          // key exists in Glass Lab JSON v3
};

struct FlagParam {
    const char* key;
    const wchar_t* label;
    const wchar_t* group;
    bool GlassParameters::*field;
    bool labKey;
};

const std::vector<FloatParam>& FloatParams();
const std::vector<FlagParam>& FlagParams();

GlassParameters GlassLabDefaults();        // Lab's current saved tuning (lab-settings.ini), Dock radius
GlassParameters LauncherTunedPreset();     // GlassTuning.qml "tuned defaults"
// The lens of a pressed slider or switch thumb (Glass Studio category "Sliders & switches", re-read when glass.json
// changes): every switch and slider of DockOver (Control Center, Settings, widget settings) uses it. Radius = half the
// thumb height; the bevel is kept within the thumb.
GlassParameters ControlLensGlass(float thumbHeight);
void ClampParameters(GlassParameters& p);

const wchar_t* MaterialName(MaterialKind k);
const wchar_t* BackdropName(BackdropKind k);

// JSON <-> parameters. FromJson is atomic: on any invalid field nothing is applied.
std::string ParametersToJson(const GlassParameters& p);
bool ParametersFromJsonText(const std::string& text, const GlassParameters& base, GlassParameters& out,
                            std::wstring& error, bool labImport);

struct GlassStats {
    std::atomic<float> fps{0};
    std::atomic<float> cpuMs{0};
    std::atomic<float> gpuFrameMs{0};
    std::atomic<float> gpuGlassMs{0};
    std::atomic<bool> gpuValid{false};
    std::atomic<bool> animating{false};
    std::atomic<int> backdropState{0};  // 0 none, 1 loading, 2 ready, 3 failed
    std::atomic<int> activeMaterial{1}; // MaterialKind actually drawn last frame
};

class GlassSettingsStore {
public:
    static GlassSettingsStore& Get();

    void Load(const std::wstring& path);      // safe: corrupt file -> backup + defaults
    // For processes that only read (Menu Bar): never renames, resets or writes the file. Returns false on a
    // missing or unreadable file (previous values are kept).
    bool LoadReadOnly(const std::wstring& path);
    // Read-only processes: reload when the file's write time changed (checked at most every 500 ms).
    void ReloadIfChanged();
    GlassParameters Surface(GlassSurface s) const;   // Dock -> Current()
    void SetSurface(GlassSurface s, const GlassParameters& p);  // Dock -> SetCurrent()
    bool Save();                               // atomic write (temp + replace)
    std::wstring Path() const { return path_; }
    std::wstring LoadMessage() const;

    GlassParameters Current() const;
    unsigned long long Version() const { return version_.load(std::memory_order_acquire); }
    void SetCurrent(const GlassParameters& p);  // clamps, bumps version, notifies the Dock
    // "Save as default" per category; Reset returns to it, else to the built-in GlassSurfaceDefault.
    GlassParameters UserDefault(GlassSurface s) const;
    void SetUserDefault(GlassSurface s, const GlassParameters& p);

    std::vector<std::wstring> PresetNames() const;  // user presets, sorted
    bool GetPreset(const std::wstring& name, GlassParameters& out) const;
    void SavePreset(const std::wstring& name, const GlassParameters& p);
    bool RenamePreset(const std::wstring& from, const std::wstring& to);
    bool DeletePreset(const std::wstring& name);

    void SetDockWindow(HWND hwnd) { dock_.store(hwnd); }
    HWND DockWindow() const { return dock_.load(); }

    GlassStats stats;

private:
    GlassSettingsStore() = default;
    void NotifyDock();

    mutable std::mutex mutex_;
    std::wstring path_;
    std::wstring loadMessage_;
    GlassParameters current_;
    std::map<int, GlassParameters> userDefaults_;  // per category, absent = built-in default
    std::map<std::wstring, GlassParameters> presets_;
    std::map<int, GlassParameters> surfaces_;  // edited surface sets (absent = role default)
    bool readOnly_ = false;
    unsigned long long fileStamp_ = 0, lastCheck_ = 0;
    bool ParseFile(const std::wstring& path, bool backupOnError);
    std::atomic<unsigned long long> version_{1};
    std::atomic<HWND> dock_{nullptr};
};

constexpr UINT kMsgGlassChanged = WM_APP + 40;  // posted to the Dock window: render one frame
constexpr UINT kMsgStudioClosed = WM_APP + 41;

}  // namespace dock
