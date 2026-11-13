#include "glass_params.h"

#include <algorithm>
#include <cmath>

#include "design_tokens.h"
#include "json_lite.h"
#include "log.h"

namespace dock {
namespace {

using json::Value;

#define GP(f) &GlassParameters::f

}  // namespace

const std::vector<FloatParam>& FloatParams() {
    // Ranges follow Glass Lab (GlassTuning.qml ranges) where the parameter exists there.
    static const std::vector<FloatParam> params = {
        {"radius", L"Corner radius (px)", L"Geometry", 0.0f, 38.0f, GP(radius), L"Dock plate; Lab value is per control", true},
        {"bevelWidth", L"Edge width / bevel (px)", L"Geometry", 1.0f, 40.0f, GP(bevelWidth), L"also sets lens depth", true},
        {"blurSpread", L"Blur radius (pre-blur)", L"Optics", 0.0f, 1.0f, GP(blurSpread), L"x 40 px blur texture", true},
        {"frost", L"Frost (sharp/blur mix)", L"Optics", 0.0f, 1.0f, GP(frost), nullptr, true},
        {"refraction", L"Refraction strength", L"Optics", -2.0f, 1.0f, GP(refraction), L"negative = inverted lens (Sliders & switches)", true},
        {"refractiveIndex", L"Refractive index", L"Optics", 1.0f, 2.5f, GP(refractiveIndex), nullptr, true},
        {"dispersion", L"Chromatic dispersion", L"Optics", 0.0f, 0.3f, GP(dispersion), nullptr, true},
        {"magnification", L"Backdrop magnification", L"Optics", 0.5f, 2.0f, GP(magnification), nullptr, true},
        {"glare", L"Highlight (glare)", L"Lighting", 0.0f, 1.0f, GP(glare), L"scales all highlights", true},
        {"edgeAmount", L"Edge highlight", L"Lighting", 0.0f, 3.0f, GP(edgeAmount), nullptr, true},
        {"staticTopLight", L"Top light", L"Lighting", 0.0f, 2.0f, GP(staticTopLight), L"fixed reflection only", true},
        {"staticBottomLight", L"Bottom light", L"Lighting", 0.0f, 2.0f, GP(staticBottomLight), L"fixed reflection only", true},
        {"staticBottomGlow", L"Bottom glow", L"Lighting", 0.0f, 3.0f, GP(staticBottomGlow), L"fixed reflection only", true},
        {"lightAngle", L"Light direction (deg)", L"Lighting", -180.0f, 180.0f, GP(lightAngle), L"dynamic reflection only", true},
        {"specularAmount", L"Specular strength", L"Lighting", 0.0f, 3.0f, GP(specularAmount), L"dynamic reflection only", true},
        {"fresnelAmount", L"Fresnel rim", L"Lighting", 0.0f, 3.0f, GP(fresnelAmount), L"dynamic reflection only", true},
        {"shadowAmount", L"Shadow", L"Lighting", 0.0f, 3.0f, GP(shadowAmount), nullptr, true},
        {"opacity", L"Opacity", L"Material", 0.0f, 1.0f, GP(opacity), L"surface opacity (qt_Opacity)", false},
        {"density", L"Material density", L"Material", 0.0f, 1.0f, GP(density), nullptr, true},
        {"glassLift", L"Dark glass lift", L"Material", 0.0f, 3.0f, GP(glassLift), nullptr, true},
        {"highlightDim", L"Highlight dimming", L"Material", 0.0f, 1.0f, GP(highlightDim), L"how much dark glass pulls bright backdrop colours toward grey; 0 = off (rim/glare stay softened)", false},
        {"backdropBlur", L"Background blur (behind popup)", L"Material", 0.0f, 1.5f, GP(backdropBlur), L"Control Center and Menu bar popups surfaces: desktop blur behind the whole panel", false},
        {"saturation", L"Backdrop saturation", L"Material", 0.0f, 2.5f, GP(saturation), nullptr, true},
        {"brightness", L"Backdrop brightness", L"Material", 0.0f, 2.0f, GP(brightness), nullptr, true},
        {"tintStrength", L"Tint strength", L"Material", 0.0f, 1.0f, GP(tintStrength), L"LiquidGlass.qml tint", false},
        {"tintR", L"Tint red", L"Material", 0.0f, 1.0f, GP(tintR), nullptr, false},
        {"tintG", L"Tint green", L"Material", 0.0f, 1.0f, GP(tintG), nullptr, false},
        {"tintB", L"Tint blue", L"Material", 0.0f, 1.0f, GP(tintB), nullptr, false},
        {"strokeWidth", L"Stroke width (px)", L"Material", 0.0f, 3.0f, GP(strokeWidth), nullptr, true},
        {"strokeOpacity", L"Stroke opacity", L"Material", 0.0f, 1.0f, GP(strokeOpacity), nullptr, true},
        {"switchPressWidth", L"Pressed switch thumb width (x)", L"Pressed control", 1.0f, 2.0f, GP(switchPressWidth), L"Sliders & switches category only", false},
        {"switchPressHeight", L"Pressed switch thumb height (x)", L"Pressed control", 1.0f, 2.5f, GP(switchPressHeight), L"Sliders & switches category only", false},
        {"sliderPressWidth", L"Pressed slider thumb width (x)", L"Pressed control", 1.0f, 2.0f, GP(sliderPressWidth), L"Sliders & switches category only", false},
        {"sliderPressHeight", L"Pressed slider thumb height (x)", L"Pressed control", 1.0f, 2.5f, GP(sliderPressHeight), L"Sliders & switches category only", false},
        {"interactionStrength", L"Pointer / press strength", L"Interaction", 0.0f, 3.0f, GP(interactionStrength), nullptr, true},
        {"interactionRadius", L"Interaction radius (px)", L"Interaction", 10.0f, 200.0f, GP(interactionRadius), nullptr, true},
        {"responseTime", L"Pointer response (ms)", L"Interaction", 0.0f, 500.0f, GP(responseTime), nullptr, true},
    };
    return params;
}

const std::vector<FlagParam>& FlagParams() {
    static const std::vector<FlagParam> flags = {
        {"staticReflection", L"Fixed reflection (macOS rim)", L"Lighting", GP(staticReflection), true},
        {"dark", L"Dark glass", L"Material", GP(dark), true},
        {"reducedTransparency", L"Reduce transparency", L"Material", GP(reducedTransparency), true},
    };
    return flags;
}

GlassParameters GlassLabDefaults() { return GlassParameters{}; }

GlassParameters LauncherTunedPreset() {
    GlassParameters p;
    p.frost = 0.3f;
    p.refraction = 1.0f;
    p.glare = 0.35f;
    p.density = 0.2f;
    p.strokeWidth = 0.5f;
    p.refractiveIndex = 1.3f;
    p.bevelWidth = 16.0f;
    p.dispersion = 0.1f;
    p.magnification = 1.0f;
    p.blurSpread = 0.2f;
    p.saturation = 1.1f;
    p.brightness = 0.81f;
    p.staticTopLight = 0.9f;
    p.staticBottomLight = 0.7f;
    p.staticBottomGlow = 1.0f;
    p.glassLift = 1.0f;
    p.strokeOpacity = 0.5f;
    return p;
}

GlassParameters RoleGlass(GlassRole role, const GlassParameters& base) {
    GlassParameters p = base;
    if (role == GlassRole::Dock) return p;
    if (role == GlassRole::DockLabel) {
        p.radius = 12.5f;
        return p;
    }
    p.material = MaterialKind::Liquid;
    p.opacity = 1.0f;
    p.debugView = 0;
    p.continuousRender = false;
    switch (role) {
    case GlassRole::Menu:
        p.radius = liquid::design::radius::Menu;
        // A 6 px bevel at 0.6 refraction left the menu edge almost unlensed, so it read as plain
        // frosted plastic. A wider, stronger lens band makes the liquid-glass edge visible.
        p.bevelWidth = 10.0f;
        p.refraction = 0.85f;
        p.dispersion = 0.03f;
        p.frost = 0.55f;
        p.blurSpread = 0.7f;
        p.shadowAmount = 0.0f;
        p.dark = true;  // these surfaces draw light ink; light appearance uses the opaque path
        break;
    case GlassRole::ControlModule:
    case GlassRole::PopupModule:
        p.bevelWidth = 12.0f;
        p.refraction = 0.7f;
        p.dispersion = 0.03f;
        p.frost = 0.62f;
        p.blurSpread = 0.75f;
        p.density = 0.32f;
        p.glare = 0.42f;
        p.shadowAmount = 0.8f;
        p.dark = true;
        break;
    case GlassRole::FanCapsule:
        // Reference fan capsule (Launcher GlassPane), sized for a 28 DIP capsule.
        p.bevelWidth = 9.0f;
        p.refraction = 0.55f;
        p.refractiveIndex = 1.45f;
        p.dispersion = 0.035f;
        p.frost = 0.72f;
        p.blurSpread = 0.8f;
        p.density = 0.35f;
        p.glare = 0.55f;
        p.saturation = 1.1f;
        p.shadowAmount = 0.6f;
        p.dark = true;
        break;
    case GlassRole::BackdropBlur:
        // Plain blur of the desktop behind a whole panel (macOS Control Center): no lens, highlights,
        // stroke, shadow, body tone or pointer response; only the blurred backdrop.
        p.bevelWidth = 1.0f;
        p.refraction = 0.0f;
        p.dispersion = 0.0f;
        p.magnification = 1.0f;
        p.frost = 1.0f;
        p.blurSpread = 0.9f;
        p.saturation = 1.0f;
        p.brightness = 1.0f;
        p.strokeWidth = 0.0f;
        p.glare = 0.0f;
        p.edgeAmount = 0.0f;
        p.shadowAmount = 0.0f;
        p.tintStrength = 0.0f;
        p.interactionStrength = 0.0f;
        p.bodyTone = 0.0f;
        p.dark = true;
        break;
    default:  // Panel: the base material as tuned
        break;
    }
    return p;
}

const wchar_t* GlassSurfaceName(GlassSurface s) {
    switch (s) {
    case GlassSurface::Menus: return L"Menus (all right-click menus, menu bar menus)";
    case GlassSurface::MenuBarPanels: return L"Menu bar popups & widget panels (Wi-Fi, Bluetooth, sound, battery; Edit Widgets, Widgets & Stacks settings, Stacks panel)";
    case GlassSurface::ControlCenter: return L"Control Center";
    case GlassSurface::Controls: return L"Sliders & switches (pressed thumb lens: Control Center, Settings, widget settings)";
    default: return L"Dock";
    }
}

const char* GlassSurfaceKey(GlassSurface s) {
    switch (s) {
    case GlassSurface::Menus: return "menus";
    case GlassSurface::MenuBarPanels: return "menuBarPanels";
    case GlassSurface::ControlCenter: return "controlCenter";
    case GlassSurface::Controls: return "controls";
    default: return "dock";
    }
}

GlassParameters DockDefault() {
    GlassParameters p;  // the user's Dock tuning, 2026-11-12
    p.radius = 23.37f; p.bevelWidth = 40.0f;
    p.blurSpread = 0.0f; p.frost = 0.0f; p.refraction = 1.0f; p.refractiveIndex = 1.5f; p.dispersion = 0.035f; p.magnification = 1.0f;
    p.glare = 0.6f; p.shadowAmount = 0.0f;
    p.density = 0.0f; p.glassLift = 0.0f; p.highlightDim = 0.33f; p.backdropBlur = 0.0f;
    p.saturation = 1.0f; p.brightness = 0.872f;
    p.strokeWidth = 0.303f; p.strokeOpacity = 1.0f;
    p.interactionRadius = 81.44f;
    p.backdrop = BackdropKind::Live;
    return p;
}

namespace {
// Values shared by the user's menu and panel tunings (2026-11-12).
GlassParameters TunedBase() {
    GlassParameters p;
    p.refractiveIndex = 1.45f; p.dispersion = 0.03f; p.glassLift = 1.0f; p.saturation = 1.1f; p.brightness = 0.798f;
    p.strokeWidth = 0.3f; p.strokeOpacity = 0.8f; p.backdrop = BackdropKind::Live; p.dark = true;
    return p;
}
}  // namespace

GlassParameters GlassSurfaceDefault(GlassSurface s) {
    switch (s) {
    case GlassSurface::Menus: {  // the former "Dock right-click menus" tuning
        GlassParameters p = TunedBase();
        p.radius = liquid::design::radius::Menu; p.bevelWidth = 10.0f;
        p.blurSpread = 0.7f; p.frost = 1.0f; p.refraction = 0.85f; p.glare = 0.4f; p.shadowAmount = 0.0f;
        p.density = 0.0f; p.highlightDim = 0.376f; p.backdropBlur = 0.489f;
        return p;
    }
    case GlassSurface::MenuBarPanels:  // Wi-Fi / Bluetooth popups drew with the Control Center set; keep that look
    case GlassSurface::ControlCenter: {
        GlassParameters p = TunedBase();
        p.radius = 22.0f; p.bevelWidth = 12.0f;
        p.blurSpread = 0.75f; p.frost = 0.62f; p.refraction = 0.7f; p.glare = 0.42f; p.shadowAmount = 0.8f;
        p.density = 0.32f; p.highlightDim = 0.0f; p.backdropBlur = 0.0f;
        return p;
    }
    case GlassSurface::Controls: {  // the Control Center switch lens (Glass Lab refractionDirection -1, references 2026-11-10)
        GlassParameters p;
        p.radius = 14.0f; p.bevelWidth = 9.0f; p.refraction = -1.7f; p.refractiveIndex = 1.45f; p.magnification = 1.0f;
        p.frost = 0.0f; p.blurSpread = 0.0f; p.dispersion = 0.06f; p.density = 0.0f; p.glassLift = 1.0f; p.highlightDim = 0.0f; p.backdropBlur = 0.0f;
        p.saturation = 1.05f; p.brightness = 1.0f; p.staticReflection = false; p.glare = 0.6f; p.edgeAmount = 1.0f; p.fresnelAmount = 1.0f;
        p.specularAmount = 0.6f; p.lightAngle = -120.0f; p.strokeWidth = 0.0f; p.strokeOpacity = 0.0f; p.shadowAmount = 1.0f;
        p.tintStrength = 0.0f; p.interactionStrength = 3.0f;  // Lab interactionGain 3: the lens follows the finger
        p.backdrop = BackdropKind::Live; p.dark = true;
        return p;
    }
    default: return DockDefault();
    }
}

GlassParameters RoleGlass(GlassRole role) {
    GlassSettingsStore& store = GlassSettingsStore::Get();
    if (role == GlassRole::ControlModule || role == GlassRole::PopupModule || role == GlassRole::Menu) {
        store.ReloadIfChanged();  // no-op in the process that owns the file
        const GlassSurface s = role == GlassRole::ControlModule ? GlassSurface::ControlCenter
                             : role == GlassRole::PopupModule   ? GlassSurface::MenuBarPanels
                                                                : GlassSurface::Menus;
        GlassParameters p = store.Surface(s);
        // Invariants the surfaces rely on, whatever was tuned: always the GPU material, light ink on dark glass,
        // no debug view, menus keep their window corner radius and draw no shadow inside the popup window.
        p.material = MaterialKind::Liquid; p.dark = true; p.debugView = 0; p.continuousRender = false; p.opacity = 1.0f;
        if (role == GlassRole::Menu) { p.radius = liquid::design::radius::Menu; p.shadowAmount = 0.0f; }
        return p;
    }
    return RoleGlass(role, store.Current());
}

GlassParameters ControlLensGlass(float thumbHeight) {
    GlassSettingsStore& store = GlassSettingsStore::Get();
    store.ReloadIfChanged();  // no-op in the process that owns the file
    GlassParameters p = store.Surface(GlassSurface::Controls);
    const float h = (std::max)(1.0f, thumbHeight);
    p.material = MaterialKind::Liquid; p.debugView = 0; p.continuousRender = false; p.opacity = 1.0f;
    p.radius = h * 0.5f; p.bevelWidth = (std::min)(p.bevelWidth, h * 0.45f);
    p.bodyTone = 0.0f;  // clear glass: no body tone (the Lab lens)
    return p;
}

GlassParameters GlassSettingsStore::Surface(GlassSurface s) const {
    if (s == GlassSurface::Dock) return Current();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(static_cast<int>(s));
    return it != surfaces_.end() ? it->second : GlassSurfaceDefault(s);
}

void GlassSettingsStore::SetSurface(GlassSurface s, const GlassParameters& p) {
    if (s == GlassSurface::Dock) { SetCurrent(p); return; }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        GlassParameters q = p;
        ClampParameters(q);
        surfaces_[static_cast<int>(s)] = q;
    }
    version_.fetch_add(1, std::memory_order_acq_rel);
}

namespace {
unsigned long long FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}
}  // namespace

bool GlassSettingsStore::LoadReadOnly(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        readOnly_ = true;
    }
    fileStamp_ = FileStamp(path);
    lastCheck_ = GetTickCount64();
    return ParseFile(path, false);
}

void GlassSettingsStore::ReloadIfChanged() {
    if (!readOnly_) return;
    const unsigned long long now = GetTickCount64();
    if (now - lastCheck_ < 500) return;
    lastCheck_ = now;
    std::wstring path;
    { std::lock_guard<std::mutex> lock(mutex_); path = path_; }
    const unsigned long long stamp = FileStamp(path);
    if (!stamp || stamp == fileStamp_) return;
    fileStamp_ = stamp;
    if (ParseFile(path, false)) DLOG(L"glass: settings reloaded (read-only)");
}

float MaterializeAlpha(float t) {
    const float x = std::clamp(t / 0.45f, 0.0f, 1.0f);
    return x * (2.0f - x);
}

void Materialize(GlassParameters& p, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t >= 1.0f) return;
    // Only constant-buffer values change, so the cached blur passes are not re-run while animating.
    p.opacity *= MaterializeAlpha(t);
    p.frost *= t;
    p.refraction *= t;
    p.dispersion *= t;
    p.density *= t;
    p.glare *= t;
    p.shadowAmount *= t;
    p.tintStrength *= t;
}

void ClampParameters(GlassParameters& p) {
    for (const FloatParam& d : FloatParams()) {
        float& v = p.*d.field;
        if (!std::isfinite(v)) v = GlassLabDefaults().*d.field;
        v = std::clamp(v, d.minValue, d.maxValue);
    }
    p.blurDownsample = p.blurDownsample >= 4 ? 4 : 2;
    p.debugView = std::clamp(p.debugView, 0, 6);
    if (static_cast<int>(p.material) < 0 || static_cast<int>(p.material) > 1) p.material = MaterialKind::Liquid;
    if (static_cast<int>(p.backdrop) < 0 || static_cast<int>(p.backdrop) > 5) p.backdrop = BackdropKind::Wallpaper;
}

const wchar_t* MaterialName(MaterialKind k) { return k == MaterialKind::Liquid ? L"Liquid Glass" : L"Smoked Glass"; }

const wchar_t* BackdropName(BackdropKind k) {
    switch (k) {
    case BackdropKind::Wallpaper: return L"Simulated: static wallpaper";
    case BackdropKind::Grid: return L"Simulated: test grid";
    case BackdropKind::Gradient: return L"Simulated: colour gradient";
    case BackdropKind::Dark: return L"Simulated: dark";
    case BackdropKind::Live: return L"Live desktop (WGC)";
    default: return L"None (Smoked fallback)";
    }
}

namespace {

const char* kMaterialKeys[] = {"liquid", "smoked"};
const char* kBackdropKeys[] = {"wallpaper", "grid", "gradient", "dark", "none", "live"};

Value ToValue(const GlassParameters& p) {
    Value v = Value::MakeObject();
    v.Set("version", Value::MakeNumber(3));
    for (const FloatParam& d : FloatParams()) v.Set(d.key, Value::MakeNumber(p.*d.field));
    for (const FlagParam& f : FlagParams()) v.Set(f.key, Value::MakeBool(p.*f.field));
    v.Set("dockMaterial", Value::MakeString(kMaterialKeys[static_cast<int>(p.material)]));
    v.Set("dockBackdrop", Value::MakeString(kBackdropKeys[static_cast<int>(p.backdrop)]));
    v.Set("dockBlurDownsample", Value::MakeNumber(p.blurDownsample));
    v.Set("dockDebugView", Value::MakeNumber(p.debugView));
    v.Set("dockContinuousRender", Value::MakeBool(p.continuousRender));
    return v;
}

// Atomic: validates every present field first, then applies to a copy of base.
bool FromValue(const Value& v, const GlassParameters& base, GlassParameters& out, std::wstring& error, bool labImport) {
    if (!v.IsObject()) {
        error = L"expected a JSON object";
        return false;
    }
    GlassParameters next = base;
    for (const FloatParam& d : FloatParams()) {
        if (labImport && (!d.labKey || std::string(d.key) == "radius")) continue;  // Lab radius is per control
        const Value* f = v.Find(d.key);
        if (!f) continue;
        if (f->type != Value::Type::Number) {
            error = json::FromUtf8(d.key) + L" must be a finite number";
            return false;
        }
        next.*d.field = std::clamp(static_cast<float>(f->number), d.minValue, d.maxValue);
    }
    for (const FlagParam& fl : FlagParams()) {
        const Value* f = v.Find(fl.key);
        if (!f) continue;
        if (f->type != Value::Type::Bool) {
            error = json::FromUtf8(fl.key) + L" must be a boolean";
            return false;
        }
        next.*fl.field = f->boolean;
    }
    if (!labImport) {
        if (const Value* m = v.Find("dockMaterial"); m && m->type == Value::Type::String)
            next.material = m->string == "smoked" ? MaterialKind::Smoked : MaterialKind::Liquid;
        if (const Value* b = v.Find("dockBackdrop"); b && b->type == Value::Type::String) {
            for (int i = 0; i < 6; ++i)
                if (b->string == kBackdropKeys[i]) next.backdrop = static_cast<BackdropKind>(i);
        }
        if (const Value* q = v.Find("dockBlurDownsample"); q && q->type == Value::Type::Number)
            next.blurDownsample = static_cast<int>(q->number);
        if (const Value* d = v.Find("dockDebugView"); d && d->type == Value::Type::Number)
            next.debugView = static_cast<int>(d->number);
        if (const Value* c = v.Find("dockContinuousRender"); c && c->type == Value::Type::Bool)
            next.continuousRender = c->boolean;
    }
    ClampParameters(next);
    out = next;
    return true;
}

}  // namespace

std::string ParametersToJson(const GlassParameters& p) { return json::Write(ToValue(p)); }

bool ParametersFromJsonText(const std::string& text, const GlassParameters& base, GlassParameters& out,
                            std::wstring& error, bool labImport) {
    Value v;
    std::string perr;
    if (!json::Parse(text, v, perr)) {
        error = L"JSON parse error: " + json::FromUtf8(perr);
        return false;
    }
    return FromValue(v, base, out, error, labImport);
}

GlassSettingsStore& GlassSettingsStore::Get() {
    static GlassSettingsStore store;
    return store;
}

void GlassSettingsStore::Load(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        readOnly_ = false;
        current_ = DockDefault();
        userDefaults_.clear();
        presets_.clear();
        surfaces_.clear();
    }
    ParseFile(path, true);
}

// Parses glass.json into the store. On a missing file the defaults stay. On an invalid file the owning process
// (backupOnError) moves it aside and keeps defaults; a read-only process leaves file and values untouched.
bool GlassSettingsStore::ParseFile(const std::wstring& path, bool backupOnError) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadMessage_ = L"no saved glass settings; using Glass Lab defaults";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string text(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 4 << 20)), '\0');
    DWORD read = 0;
    ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    text.resize(read);

    Value root;
    std::string perr;
    std::wstring err;
    bool ok = json::Parse(text, root, perr) && root.IsObject();
    GlassParameters cur = DockDefault();
    std::map<std::wstring, GlassParameters> presets;
    std::map<int, GlassParameters> surfaces, defaults;
    if (ok) {
        const Value* c = root.Find("current");
        ok = c && FromValue(*c, DockDefault(), cur, err, false);
        // The old single "userDefault" is ignored: Studio stored whichever surface was being edited in it.
        if (ok) {
            if (const Value* ps = root.Find("presets"); ps && ps->IsObject()) {
                for (const auto& kv : ps->object) {
                    GlassParameters pp;
                    if (FromValue(kv.second, GlassLabDefaults(), pp, err, false)) presets[json::FromUtf8(kv.first)] = pp;
                }
            }
            if (const Value* ss = root.Find("surfaces"); ss && ss->IsObject()) {
                for (int i = 1; i < kGlassSurfaceCount; ++i) {
                    const GlassSurface gs = static_cast<GlassSurface>(i);
                    const Value* sv = ss->Find(GlassSurfaceKey(gs));
                    // Before the four categories (2026-11-12) menus were "contextMenu" (Dock only). The old
                    // "menuBarPopup" / "widgetPanels" sets are dropped: those surfaces now follow the new categories.
                    if (!sv && gs == GlassSurface::Menus) sv = ss->Find("contextMenu");
                    GlassParameters sp;
                    if (sv && FromValue(*sv, GlassSurfaceDefault(gs), sp, err, false)) surfaces[i] = sp;
                }
            }
            if (const Value* ds = root.Find("defaults"); ds && ds->IsObject()) {
                for (int i = 0; i < kGlassSurfaceCount; ++i) {
                    const GlassSurface gs = static_cast<GlassSurface>(i);
                    GlassParameters dp;
                    if (const Value* dv = ds->Find(GlassSurfaceKey(gs)); dv && FromValue(*dv, GlassSurfaceDefault(gs), dp, err, false))
                        defaults[i] = dp;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
        if (!backupOnError) { loadMessage_ = L"glass settings unreadable; previous values kept"; return false; }
        const std::wstring backup = path + L".corrupt";
        MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
        loadMessage_ = L"glass settings were invalid (" + (err.empty() ? json::FromUtf8(perr) : err) +
                       L"); backed up as .corrupt and reset to defaults";
        return false;
    }
    current_ = cur;
    userDefaults_ = std::move(defaults);
    presets_ = std::move(presets);
    surfaces_ = std::move(surfaces);
    loadMessage_ = L"glass settings loaded";
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool GlassSettingsStore::Save() {
    std::string text;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Value root = Value::MakeObject();
        root.Set("format", Value::MakeString("LiquidDockGlass"));
        root.Set("version", Value::MakeNumber(1));
        root.Set("current", ToValue(current_));
        Value ds = Value::MakeObject();
        for (const auto& kv : userDefaults_) ds.Set(GlassSurfaceKey(static_cast<GlassSurface>(kv.first)), ToValue(kv.second));
        root.Set("defaults", ds);
        Value ps = Value::MakeObject();
        for (const auto& kv : presets_) ps.Set(json::ToUtf8(kv.first), ToValue(kv.second));
        root.Set("presets", ps);
        Value ss = Value::MakeObject();
        for (const auto& kv : surfaces_)
            ss.Set(GlassSurfaceKey(static_cast<GlassSurface>(kv.first)), ToValue(kv.second));
        root.Set("surfaces", ss);
        text = json::Write(root);
        path = path_;
    }
    if (path.empty() || readOnly_) return false;  // read-only processes never write glass.json
    const std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != text.size()) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::wstring GlassSettingsStore::LoadMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadMessage_;
}

GlassParameters GlassSettingsStore::Current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void GlassSettingsStore::SetCurrent(const GlassParameters& p) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = p;
        ClampParameters(current_);
    }
    version_.fetch_add(1, std::memory_order_acq_rel);
    NotifyDock();
}

GlassParameters GlassSettingsStore::UserDefault(GlassSurface s) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = userDefaults_.find(static_cast<int>(s));
    return it != userDefaults_.end() ? it->second : GlassSurfaceDefault(s);
}

void GlassSettingsStore::SetUserDefault(GlassSurface s, const GlassParameters& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    GlassParameters q = p;
    ClampParameters(q);
    userDefaults_[static_cast<int>(s)] = q;
}

std::vector<std::wstring> GlassSettingsStore::PresetNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::wstring> names;
    for (const auto& kv : presets_) names.push_back(kv.first);
    return names;
}

bool GlassSettingsStore::GetPreset(const std::wstring& name, GlassParameters& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = presets_.find(name);
    if (it == presets_.end()) return false;
    out = it->second;
    return true;
}

void GlassSettingsStore::SavePreset(const std::wstring& name, const GlassParameters& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    presets_[name] = p;
}

bool GlassSettingsStore::RenamePreset(const std::wstring& from, const std::wstring& to) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = presets_.find(from);
    if (it == presets_.end() || to.empty() || presets_.count(to)) return false;
    presets_[to] = it->second;
    presets_.erase(from);
    return true;
}

bool GlassSettingsStore::DeletePreset(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return presets_.erase(name) > 0;
}

void GlassSettingsStore::NotifyDock() {
    if (HWND h = dock_.load()) PostMessageW(h, kMsgGlassChanged, 0, 0);
}

}  // namespace dock
