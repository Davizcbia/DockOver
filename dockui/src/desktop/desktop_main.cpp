// docky Desktop (DockyDesktop.exe): desktop widgets and file Stacks.
// A separate program from docky (Dock + Menu Bar): installed, started and quit on its own. The two only meet through
// the extension protocol windows (ext_protocol.h): the Menu Bar's docky menu can open this app's widget gallery,
// this app's tray icon shows in the Menu Bar's Hidden Icons, and both follow docky's Appearance setting when present.
#include "../game_compat.h"
#include "../ui_language.h"
#include "../capture_visible.h"
#include "canvas.h"
#include "widgets.h"
#include "stacks.h"
#include "glass_window.h"
#include "calendar_feed.h"
#include "calendar_config.h"
#include "devices.h"
#include "weather.h"
#include "../ext_protocol.h"
#include "../unified_menu.h"
#include "../json_lite.h"
#include "../desktop_settings.h"
#include "../theme_state.h"
#include "../menu_surface.h"
#include "../glass_params.h"
#include "../lab_slider.h"
#include "../log.h"
#include "../vblank.h"
#include "../motion.h"
#include "../music_wallpaper_frame.h"
#include "../pairing/widget_queue.h"
#include <shellapi.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

using namespace docky::desk;
using dock::json::Value;

namespace {
constexpr wchar_t kHostClass[] = L"DockyDesktopHostV1", kWidgetClass[] = L"DockyDesktopWidget", kGalleryClass[] = L"DockyDesktopGallery";
constexpr UINT kTrayMsg = WM_APP + 1, kDataReady = WM_APP + 3, kCalendarReady = WM_APP + 4, kDevicesReady = WM_APP + 5, kWeatherReady = WM_APP + 6, kMusicFrame = WM_APP + 20;
std::atomic<bool> musicFramePending{false};
constexpr UINT_PTR kTickTimer = 1, kPollTimer = 2, kExitTimer = 3, kDevMenuTimer = 4, kDevMenuShot = 5;
using docky::pairing::kDropWidgetKind;
using docky::pairing::WidgetDraft;
using docky::pairing::WidgetQueue;
// Game mode: a full-screen app (game, video, F11 browser) in front covers the widgets, so clocks, system graphs and data
// polling pause until it leaves (they repaint on the next tick).
// Games list (Settings > Menu Bar > Game compatibility): a listed program running pauses the widgets too.
bool gameCompatDesktop = false;
docky::game::Watcher desktopGameWatch;
bool FullscreenAppInFront() {
    if (!desktopGameWatch.Poll(liquid::settings::DefaultPath(), gameCompatDesktop).empty()) return true;
    HWND f = GetForegroundWindow();
    if (!f || !IsWindowVisible(f) || IsIconic(f)) return false;
    DWORD pid = 0, shellPid = 0; GetWindowThreadProcessId(f, &pid); GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    if (pid == GetCurrentProcessId() || pid == shellPid) return false;
    MONITORINFO mi{sizeof(mi)}; RECT r{};
    if (!GetMonitorInfoW(MonitorFromWindow(f, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(f, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
bool devMenu = false;

struct WidgetWin {
    std::wstring kind;  // clock | calendar | battery | system | ext:<id>
    Size size = Size::Small;
    POINT pos{};        // top-left of the widget shape, physical px
    HWND hwnd = nullptr;
    std::unique_ptr<Canvas> canvas = std::make_unique<Canvas>();
    ULONGLONG lastPaint = 0;
    // Wheel scrolling (Calendar, Weather): a spring toward the wheel's target; the painter reports the range.
    docky::motion::Spring scroll;
    float scrollMin = 0, scrollMax = 0;
    ULONGLONG lastScroll = 0;  // after 20 s without scrolling the widget glides back to its normal view
    double scrollStepAt = 0;
    bool dropHover = false;
    bool dropRegistered = false;
    int dropPage = 0;
    int dropPreview = -1;
    bool dropTracking = false;
    DropWidgetNotice dropNotice = DropWidgetNotice::None;
    ULONGLONG dropNoticeUntil = 0;
};
struct Config {
    std::vector<WidgetWin> widgets;
    // Stacks replace Explorer's desktop icons (macOS); otherwise every file would show twice.
    bool stacks = true, hideIcons = true; GroupBy by = GroupBy::Kind; StackStyle style = StackStyle::InPlace;
    bool freeStacks = false; std::map<std::wstring, POINT> stackCells;  // Stacks > Place stacks anywhere (grid cells by stack name)
    std::vector<std::wstring> seenExt;  // third-party widgets already offered once (auto-placed on first registration)
    std::map<std::wstring, std::map<std::string, int>> extValues;  // third-party widget settings chosen by the user
};

Config config;
std::vector<ExtWidget> extWidgets;
HWND host = nullptr, gallery = nullptr;
bool desktopRaised = false, trayAdded = false;
std::wstring devPng;
liquid::settings::Revision themeRevision;
Theme theme;
WidgetDraft dropDraft;
std::vector<DropWidgetEntry> dropEntries;
ULONGLONG dropCheckedAt = 0;

DropWidgetEntry FileDropEntry(const std::wstring& path) {
    DropWidgetEntry entry;
    const auto slash = path.find_last_of(L"\\/");
    entry.preview = slash == std::wstring::npos ? path : path.substr(slash + 1);
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) return entry;
    ComPtr<IWICBitmap> source;
    if (Wic()) Wic()->CreateBitmapFromHICON(info.hIcon, &source);
    DestroyIcon(info.hIcon);
    ComPtr<IWICFormatConverter> converter;
    if (!source || FAILED(Wic()->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&entry.iconW, &entry.iconH)) || !entry.iconW || !entry.iconH) return entry;
    entry.icon.resize(static_cast<size_t>(entry.iconW) * entry.iconH);
    if (FAILED(converter->CopyPixels(nullptr, entry.iconW * 4, entry.iconW * entry.iconH * 4,
                                    reinterpret_cast<BYTE*>(entry.icon.data())))) entry.icon.clear();
    return entry;
}

void RefreshDropDraft() {
    WidgetDraft draft;
    if (WidgetQueue().Read(draft) && (draft.files != dropDraft.files || draft.texts != dropDraft.texts)) {
        std::vector<DropWidgetEntry> entries;
        entries.reserve(draft.files.size() + draft.texts.size());
        for (const auto& file : draft.files) {
            const auto existing = std::find(dropDraft.files.begin(), dropDraft.files.end(), file);
            if (existing == dropDraft.files.end()) entries.push_back(FileDropEntry(file));
            else entries.push_back(std::move(dropEntries[existing - dropDraft.files.begin()]));
        }
        for (const auto& text : draft.texts) {
            DropWidgetEntry entry;
            // Bound the preview work. All of the original text stays in the draft.
            entry.preview = text.size() <= 360 ? text : text.substr(0, 360) + L"…";
            if (text.size() > 360 && IS_HIGH_SURROGATE(entry.preview[359])) entry.preview.erase(359, 1);
            entry.clipboard = true;
            entries.push_back(std::move(entry));
        }
        dropDraft = std::move(draft);
        dropEntries = std::move(entries);
        for (auto& w : config.widgets) w.dropPreview = -1;
    }
    dropCheckedAt = GetTickCount64();
}
std::wstring ConfigPath() { return DataDir() + L"\\desktop.json"; }
bool EditMode();
extern RECT galleryRect;

UINT DpiAt(POINT p) { UINT x = 96, y = 96; GetDpiForMonitor(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &x, &y); return x; }
int Px(float dip, UINT dpi) { return static_cast<int>(std::lround(dip * dpi / 96.f)); }
RECT PrimaryWork() { MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi); return mi.rcWork; }
std::string U(const std::wstring& s) { return dock::json::ToUtf8(s); }
std::wstring W(const std::string& s) { return dock::json::FromUtf8(s); }

ExtWidget* FindExt(const std::wstring& kind) {
    if (kind.rfind(L"ext:", 0) != 0) return nullptr;
    for (auto& e : extWidgets) if (L"ext:" + e.id == kind) return &e;
    return nullptr;
}

// ---- Config -------------------------------------------------------------------------------------------------
void SaveConfig() {
    Value root = Value::MakeObject(); root.Set("version", Value::MakeNumber(1));
    Value list; list.type = Value::Type::Array;
    for (const auto& w : config.widgets) {
        Value o = Value::MakeObject();
        o.Set("kind", Value::MakeString(U(w.kind))); o.Set("size", Value::MakeString(SizeName(w.size)));
        o.Set("x", Value::MakeNumber(w.pos.x)); o.Set("y", Value::MakeNumber(w.pos.y));
        list.array.push_back(std::move(o));
    }
    root.Set("widgets", std::move(list));
    Value st = Value::MakeObject();
    st.Set("enabled", Value::MakeBool(config.stacks)); st.Set("groupBy", Value::MakeString(config.by == GroupBy::Date ? "date" : "kind"));
    st.Set("hideDesktopIcons", Value::MakeBool(config.hideIcons));
    st.Set("open", Value::MakeString(config.style == StackStyle::Panel ? "panel" : "inPlace"));
    st.Set("freePlacement", Value::MakeBool(config.freeStacks));
    { Value cells = Value::MakeObject();
      for (const auto& [name, c] : config.stackCells) { Value xy; xy.type = Value::Type::Array; xy.array.push_back(Value::MakeNumber(c.x)); xy.array.push_back(Value::MakeNumber(c.y)); cells.Set(U(name), std::move(xy)); }
      st.Set("cells", std::move(cells)); }
    root.Set("stacks", std::move(st));
    Value seen; seen.type = Value::Type::Array; for (const auto& s : config.seenExt) seen.array.push_back(Value::MakeString(U(s)));
    root.Set("seenExtensions", std::move(seen));
    Value ev = Value::MakeObject();
    for (const auto& [id, values] : config.extValues) { Value o = Value::MakeObject(); for (const auto& [k, v] : values) o.Set(k, Value::MakeNumber(v)); ev.Set(U(id), std::move(o)); }
    root.Set("extensionSettings", std::move(ev));
    const auto& pr = CurrentPrefs(); Value pf = Value::MakeObject();
    pf.Set("style", Value::MakeString(pr.style == 1 ? "glass" : "solid")); pf.Set("gap", Value::MakeNumber(pr.gap)); pf.Set("margin", Value::MakeNumber(pr.margin));
    pf.Set("snap", Value::MakeNumber(pr.snap)); pf.Set("glass", Value::MakeNumber(pr.glass)); pf.Set("menuBattery", Value::MakeBool(pr.menuBattery)); pf.Set("fahrenheit", Value::MakeBool(pr.fahrenheit));
    pf.Set("clockDark", Value::MakeBool(pr.clockDark)); pf.Set("batteryGlass", Value::MakeBool(pr.batteryGlass));
    pf.Set("glassRim", Value::MakeNumber(pr.glassRim)); pf.Set("glassRefraction", Value::MakeNumber(pr.glassRefraction));
    pf.Set("glassFrost", Value::MakeNumber(pr.glassFrost)); pf.Set("glassTint", Value::MakeNumber(pr.glassTint));
    auto names = [](const std::vector<std::wstring>& list) { Value a; a.type = Value::Type::Array; for (const auto& n : list) a.array.push_back(Value::MakeString(U(n))); return a; };
    pf.Set("batteryWidgetOff", names(pr.batteryWidgetOff)); pf.Set("batteryMenuOff", names(pr.batteryMenuOff));
    root.Set("widgetPrefs", std::move(pf));
    CreateDirectoryW(DataDir().c_str(), nullptr);
    const std::string text = dock::json::Write(root);
    const std::wstring temp = ConfigPath() + L".tmp";
    { std::ofstream f(temp, std::ios::binary | std::ios::trunc); if (!f) return; f.write(text.data(), static_cast<std::streamsize>(text.size())); }
    MoveFileExW(temp.c_str(), ConfigPath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool LoadConfig() {
    std::ifstream f(ConfigPath(), std::ios::binary); if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); std::string error; Value root;
    if (!dock::json::Parse(ss.str(), root, error) || !root.IsObject()) return false;
    if (const Value* list = root.Find("widgets"); list && list->type == Value::Type::Array)
        for (const Value& o : list->array) {
            if (!o.IsObject() || config.widgets.size() >= 40) continue;
            WidgetWin w; const Value* k = o.Find("kind"); if (!k || k->type != Value::Type::String) continue;
            w.kind = W(k->string);
            if (const Value* s = o.Find("size"); s && s->type == Value::Type::String) w.size = ParseSize(s->string);
            if (const Value* x = o.Find("x"); x && x->type == Value::Type::Number) w.pos.x = static_cast<LONG>(x->number);
            if (const Value* y = o.Find("y"); y && y->type == Value::Type::Number) w.pos.y = static_cast<LONG>(y->number);
            config.widgets.push_back(std::move(w));
        }
    if (const Value* st = root.Find("stacks"); st && st->IsObject()) {
        if (const Value* v = st->Find("enabled"); v && v->type == Value::Type::Bool) config.stacks = v->boolean;
        if (const Value* v = st->Find("hideDesktopIcons"); v && v->type == Value::Type::Bool) config.hideIcons = v->boolean;
        if (const Value* v = st->Find("groupBy"); v && v->type == Value::Type::String) config.by = v->string == "date" ? GroupBy::Date : GroupBy::Kind;
        if (const Value* v = st->Find("open"); v && v->type == Value::Type::String) config.style = v->string == "panel" ? StackStyle::Panel : StackStyle::InPlace;
        if (const Value* v = st->Find("freePlacement"); v && v->type == Value::Type::Bool) config.freeStacks = v->boolean;
        if (const Value* v = st->Find("cells"); v && v->IsObject())
            for (const auto& [name, xy] : v->object)
                if (xy.type == Value::Type::Array && xy.array.size() == 2 && xy.array[0].type == Value::Type::Number && xy.array[1].type == Value::Type::Number)
                    config.stackCells[W(name)] = POINT{static_cast<LONG>(xy.array[0].number), static_cast<LONG>(xy.array[1].number)};
    }
    if (const Value* pf = root.Find("widgetPrefs"); pf && pf->IsObject()) {
        auto& pr = CurrentPrefs();
        if (const Value* v = pf->Find("style"); v && v->type == Value::Type::String) pr.style = v->string == "glass" ? 1 : 0;
        auto num = [&](const char* k, float& out, float lo, float hi) { if (const Value* v = pf->Find(k); v && v->type == Value::Type::Number) out = std::clamp(static_cast<float>(v->number), lo, hi); };
        if (const Value* v = pf->Find("menuBattery"); v && v->type == Value::Type::Bool) pr.menuBattery = v->boolean;
        if (const Value* v = pf->Find("fahrenheit"); v && v->type == Value::Type::Bool) pr.fahrenheit = v->boolean;
        if (const Value* v = pf->Find("clockDark"); v && v->type == Value::Type::Bool) pr.clockDark = v->boolean;
        if (const Value* v = pf->Find("batteryGlass"); v && v->type == Value::Type::Bool) pr.batteryGlass = v->boolean;
        auto names = [&](const char* k, std::vector<std::wstring>& out) {
            if (const Value* v = pf->Find(k); v && v->type == Value::Type::Array)
                for (const Value& n : v->array) if (n.type == Value::Type::String && out.size() < 64) out.push_back(W(n.string));
        };
        names("batteryWidgetOff", pr.batteryWidgetOff); names("batteryMenuOff", pr.batteryMenuOff);
        num("gap", pr.gap, 8, 32); num("margin", pr.margin, 8, 48); num("snap", pr.snap, 0, 32); num("glass", pr.glass, 0, 1);
        num("glassRim", pr.glassRim, 0, 1.5f); num("glassRefraction", pr.glassRefraction, 0, 1); num("glassFrost", pr.glassFrost, 0, 1); num("glassTint", pr.glassTint, 0, 1);
    }
    if (const Value* ev = root.Find("extensionSettings"); ev && ev->IsObject())
        for (const auto& [id, o] : ev->object) {
            if (!o.IsObject() || config.extValues.size() >= 64) continue;
            auto& values = config.extValues[W(id)];
            for (const auto& [k, v] : o.object) if (v.type == Value::Type::Number && values.size() < 32) values[k] = static_cast<int>(v.number);
        }
    if (const Value* seen = root.Find("seenExtensions"); seen && seen->type == Value::Type::Array)
        for (const Value& v : seen->array) if (v.type == Value::Type::String) config.seenExt.push_back(W(v.string));
    return true;
}

// ---- Widget windows -------------------------------------------------------------------------------------------
RECT WindowRectFor(const WidgetWin& w) {
    const UINT dpi = DpiAt(w.pos);
    const auto s = SizeDip(w.size);
    return RECT{w.pos.x - Px(kShadow, dpi), w.pos.y - Px(kShadow, dpi), w.pos.x + Px(s.width + kShadow, dpi), w.pos.y + Px(s.height + kShadow, dpi)};
}

void PaintWidgetWin(WidgetWin& w) {
    if (!w.hwnd) return;
    const RECT r = WindowRectFor(w);
    const UINT dpi = DpiAt(w.pos);
    const int pw = r.right - r.left, ph = r.bottom - r.top;
    ID2D1RenderTarget* rt = w.canvas->Begin(pw, ph, dpi);
    if (!rt) return;
    const auto s = SizeDip(w.size);
    const D2D1_RECT_F shape = D2D1::RectF(kShadow, kShadow, kShadow + s.width, kShadow + s.height);
    const RECT screen{w.pos.x, w.pos.y, w.pos.x + Px(s.width, dpi), w.pos.y + Px(s.height, dpi)};
    ExtWidget* ext = FindExt(w.kind);
    if (ext && ext->templ == L"image") {  // decode the image once per path
        std::wstring path; if (const Value* p = ext->data.Find("path"); p && p->type == Value::Type::String) path = W(p->string);
        if (!path.empty() && ext->folder.size() && path.find(L':') == std::wstring::npos) path = ext->folder + L"\\" + path;
        if (path != ext->imagePath) { ext->imagePath = path; ext->image.clear(); if (!path.empty()) LoadImageFile(path, 900, ext->image, ext->imageW, ext->imageH); }
    }
    const Theme content = PaintPlate(rt, shape, screen, theme, PlateToneOf(w.kind));
    rt->PushAxisAlignedClip(shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    PaintScroll() = WidgetScroll{Scrollable(w.kind) ? w.scroll.F() : 0.f, w.scrollMin, w.scrollMax};
    DropWidgetState drop;
    if (w.kind == kDropWidgetKind) {
        const auto now = GetTickCount64();
        if (now - dropCheckedAt >= 600 || !dropCheckedAt) RefreshDropDraft();
        const int capacity = LayoutDropWidget(shape).Capacity();
        const int pages = (static_cast<int>(dropEntries.size()) + capacity - 1) / capacity;
        w.dropPage = std::clamp(w.dropPage, 0, std::max(0, pages - 1));
        drop.entries = dropEntries;
        drop.page = w.dropPage;
        drop.hovered = w.dropHover ? -1 : w.dropPreview;
        drop.hovering = w.dropHover;
        drop.focused = GetFocus() == w.hwnd && !EditMode();
        if (now < w.dropNoticeUntil) drop.notice = w.dropNotice;
    }
    PaintWidget(rt, w.kind, w.size, shape, content, ext, &drop);
    if (Scrollable(w.kind)) { w.scrollMin = PaintScroll().min; w.scrollMax = PaintScroll().max; }
    PaintScroll() = WidgetScroll{};  // other paths (gallery, previews) draw the normal view
    rt->PopAxisAlignedClip();
    if (EditMode()) {  // remove badge (macOS edit mode), near the top-left corner
        ComPtr<ID2D1SolidColorBrush> b; rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.96f), &b);
        const D2D1_POINT_2F c = D2D1::Point2F(shape.left + 3, shape.top + 3);
        rt->FillEllipse(D2D1::Ellipse(c, 10, 10), b.Get());
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.22f)); rt->DrawEllipse(D2D1::Ellipse(c, 10, 10), b.Get(), 1.f);
        b->SetColor(D2D1::ColorF(1, 1, 1, 1)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - 5, c.y - 1, c.x + 5, c.y + 1), 1, 1), b.Get());
    }
    w.canvas->End(w.hwnd, POINT{r.left, r.top});
    w.lastPaint = GetTickCount64();
    if (!devPng.empty()) { std::wstring name = w.kind; for (auto& c : name) if (c == L':') c = L'_'; w.canvas->SavePng(devPng + L"\\widget-" + name + L"-" + W(SizeName(w.size)) + L".png"); }
}

void PlaceWidget(WidgetWin& w, bool show) {
    const RECT r = WindowRectFor(w);
    SetWindowPos(w.hwnd, desktopRaised ? HWND_TOP : HWND_BOTTOM, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}

WidgetWin* WidgetOf(HWND h) { for (auto& w : config.widgets) if (w.hwnd == h) return &w; return nullptr; }

bool OpenDockDrop(HWND owner) {
    wchar_t module[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    if (!len || len >= ARRAYSIZE(module)) return false;
    const std::wstring path(module, len);
    const auto dir = path.substr(0, path.find_last_of(L"\\/") + 1);
    for (const auto* executable : {L"runtime\\docky-bubble.exe", L"docky-bubble.exe", L"DockDrop.exe"}) {
        const auto candidate = dir + executable;
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", candidate.c_str(), L"--pair-phone", nullptr, SW_SHOWNORMAL)) > 32)
            return true;
    }
    return false;
}

void OpenDropWidget(WidgetWin& widget) {
    if (OpenDockDrop(widget.hwnd)) return;
    widget.dropNotice = DropWidgetNotice::Unavailable;
    widget.dropNoticeUntil = GetTickCount64() + 2400;
    PaintWidgetWin(widget);
}

DropWidgetLayout DropLayoutOf(const WidgetWin& widget) {
    const auto size = SizeDip(widget.size);
    return LayoutDropWidget(D2D1::RectF(kShadow, kShadow, kShadow + size.width, kShadow + size.height));
}

D2D1_POINT_2F DropPoint(const WidgetWin& widget, LPARAM point) {
    const float scale = 96.f / DpiAt(widget.pos);
    return D2D1::Point2F(GET_X_LPARAM(point) * scale, GET_Y_LPARAM(point) * scale);
}

bool DropContains(const D2D1_RECT_F& rect, D2D1_POINT_2F point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

bool ChangeDropPage(WidgetWin& widget, int delta) {
    const int capacity = DropLayoutOf(widget).Capacity();
    const int page = std::clamp(widget.dropPage + delta, 0, std::max(0, (static_cast<int>(dropEntries.size()) - 1) / capacity));
    if (page == widget.dropPage) return false;
    widget.dropPage = page;
    widget.dropPreview = -1;
    PaintWidgetWin(widget);
    return true;
}

bool StageDropWidget(WidgetWin& widget, const WidgetDraft& drop) {
    const bool saved = WidgetQueue().Append(drop);
    RefreshDropDraft();
    widget.dropNotice = DropWidgetNotice::InvalidDrop;
    widget.dropNoticeUntil = saved ? 0 : GetTickCount64() + 2400;
    if (saved && !dropEntries.empty()) {
        const auto last = drop.texts.empty() ? dropDraft.files.size() : dropEntries.size();
        widget.dropPage = static_cast<int>(last - 1) / DropLayoutOf(widget).Capacity();
    }
    PaintWidgetWin(widget);
    return saved;
}

std::wstring ReadDropText(HGLOBAL memory, CLIPFORMAT format) {
    const auto* bytes = static_cast<const char*>(GlobalLock(memory));
    if (!bytes) return {};
    const SIZE_T size = GlobalSize(memory);
    const auto limit = docky::pairing::kWidgetMaxText + 1;
    std::wstring text;
    if (format == CF_UNICODETEXT) {
        const auto* first = reinterpret_cast<const wchar_t*>(bytes);
        const auto* bound = first + std::min(size / sizeof(wchar_t), limit);
        const auto* end = std::find(first, bound, L'\0');
        if (end < bound) text.assign(first, end);
    } else if (const auto* end = static_cast<const char*>(std::memchr(bytes, 0, std::min(size, limit)))) {
        const int length = static_cast<int>(end - bytes);
        const int wide = MultiByteToWideChar(CP_ACP, 0, bytes, length, nullptr, 0);
        if (wide > 0) {
            text.resize(static_cast<size_t>(wide));
            MultiByteToWideChar(CP_ACP, 0, bytes, length, text.data(), wide);
        }
    }
    GlobalUnlock(memory);
    return text;
}

void PasteDropWidget(WidgetWin& widget) {
    std::wstring text;
    if (OpenClipboard(widget.hwnd)) {
        if (const auto value = GetClipboardData(CF_UNICODETEXT)) text = ReadDropText(value, CF_UNICODETEXT);
        CloseClipboard();
    }
    if (text.empty()) {
        widget.dropNotice = DropWidgetNotice::EmptyClipboard;
        widget.dropNoticeUntil = GetTickCount64() + 2400;
        PaintWidgetWin(widget);
    } else StageDropWidget(widget, {{}, {std::move(text)}});
}

class WidgetDropTarget final : public IDropTarget {
public:
    explicit WidgetDropTarget(HWND widget) : widget_(widget) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDropTarget) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = InterlockedDecrement(&refs_);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override {
        format_ = 0;
        if (data) {
            for (const CLIPFORMAT candidate : {CLIPFORMAT(CF_HDROP), CLIPFORMAT(CF_UNICODETEXT), CLIPFORMAT(CF_TEXT)}) {
                FORMATETC format{candidate, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                if (SUCCEEDED(data->QueryGetData(&format))) { format_ = candidate; break; }
            }
        }
        return DragOver(keys, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        const auto* widget = WidgetOf(widget_);
        bool accept = false;
        if (format_ && widget && !EditMode() && (*effect & DROPEFFECT_COPY)) {
            const auto size = SizeDip(widget->size);
            const UINT dpi = DpiAt(widget->pos);
            const RECT body{widget->pos.x, widget->pos.y, widget->pos.x + Px(size.width, dpi), widget->pos.y + Px(size.height, dpi)};
            accept = PtInRect(&body, POINT{point.x, point.y}) != FALSE;
        }
        *effect = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        Highlight(accept);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { format_ = 0; Highlight(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override {
        DragOver(keys, point, effect);
        if (*effect == DROPEFFECT_NONE) return DragLeave();
        FORMATETC format{format_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        WidgetDraft drop;
        if (data && SUCCEEDED(data->GetData(&format, &medium))) {
            if (format_ == CF_HDROP) {
                const auto files = static_cast<HDROP>(medium.hGlobal);
                const UINT count = DragQueryFileW(files, 0xFFFFFFFF, nullptr, 0);
                if (count <= docky::pairing::kWidgetMaxFiles) {
                    for (UINT i = 0; i < count; ++i) {
                        const UINT length = DragQueryFileW(files, i, nullptr, 0);
                        std::wstring path(length + 1, L'\0');
                        if (!length || !DragQueryFileW(files, i, path.data(), length + 1)) { drop.files.clear(); break; }
                        path.resize(length);
                        drop.files.push_back(std::move(path));
                    }
                }
            } else {
                auto text = ReadDropText(medium.hGlobal, format_);
                if (!text.empty()) drop.texts.push_back(std::move(text));
            }
            ReleaseStgMedium(&medium);
        }
        auto* widget = WidgetOf(widget_);
        const bool saved = widget && StageDropWidget(*widget, drop);
        *effect = saved ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return DragLeave();
    }

private:
    void Highlight(bool value) {
        if (auto* widget = WidgetOf(widget_); widget && widget->dropHover != value) {
            widget->dropHover = value;
            PaintWidgetWin(*widget);
        }
    }
    HWND widget_;
    LONG refs_ = 1;
    CLIPFORMAT format_ = 0;
};
void ShowWidgetMenu(WidgetWin& w, POINT at);
void OpenGallery();
void RefreshGallery();
void OpenSettings();
void RemoveWidget(HWND h);
void CloseGallery();
void UpdateBatteryItem();

// ---- Placement (macOS-like) ------------------------------------------------------------------------------------
// Widgets sit on a grid whose unit is half a small widget plus half a gap ((170 + 16) / 2 DIP), measured from the
// work area's margin, so small, medium and large widgets all line up. While dragging, a widget is drawn towards
// the edges of its neighbours (aligned or one gap apart) when it comes within 12 DIP; on release it settles on
// the nearest grid or neighbour position that is free, with a short ease-out glide.
RECT ShapeAt(const WidgetWin& w, POINT p) {
    const UINT dpi = DpiAt(p); const auto s = SizeDip(w.size);
    return RECT{p.x, p.y, p.x + Px(s.width, dpi), p.y + Px(s.height, dpi)};
}
RECT WorkAt(POINT p) { MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi); return mi.rcWork; }
// Neighbour-edge candidates on one axis for a widget of extent `size` (px).
void NeighbourLines(const WidgetWin& self, bool x, LONG size, UINT dpi, std::vector<LONG>& out) {
    const LONG gap = Px(kGap, dpi);
    for (const auto& o : config.widgets) {
        if (&o == &self || !o.hwnd) continue;
        const RECT r = ShapeAt(o, o.pos);
        const LONG lo = x ? r.left : r.top, hi = x ? r.right : r.bottom;
        out.insert(out.end(), {lo, hi - size, hi + gap, lo - gap - size});
    }
}
void GridLines(bool x, LONG size, UINT dpi, const RECT& work, std::vector<LONG>& out) {
    const LONG unit = Px((kSmall + kGap) / 2, dpi), origin = (x ? work.left : work.top) + Px(kMargin, dpi);
    const LONG last = (x ? work.right : work.bottom) - Px(kMargin, dpi) - size;
    for (LONG v = origin; v <= last; v += unit) out.push_back(v);
}
LONG Nearest(LONG v, const std::vector<LONG>& c, LONG limit) {
    LONG best = v, dist = limit + 1;
    for (LONG x : c) if (std::abs(x - v) < dist) { dist = std::abs(x - v); best = x; }
    return dist <= limit ? best : v;
}
POINT Magnet(const WidgetWin& self, POINT raw) {
    const UINT dpi = DpiAt(raw); const RECT r = ShapeAt(self, raw);
    std::vector<LONG> xs, ys;
    NeighbourLines(self, true, r.right - r.left, dpi, xs); NeighbourLines(self, false, r.bottom - r.top, dpi, ys);
    return POINT{Nearest(raw.x, xs, Px(CurrentPrefs().snap, dpi)), Nearest(raw.y, ys, Px(CurrentPrefs().snap, dpi))};
}
bool Free(const WidgetWin& self, POINT p) {
    const UINT dpi = DpiAt(p); RECT want = ShapeAt(self, p); InflateRect(&want, Px(kGap, dpi) - 1, Px(kGap, dpi) - 1);  // neighbours keep a full gap
    for (const auto& o : config.widgets) { if (&o == &self || !o.hwnd) continue; RECT r = ShapeAt(o, o.pos), x{}; if (IntersectRect(&x, &want, &r)) return false; }
    return true;
}
POINT Settle(const WidgetWin& self, POINT p) {
    const UINT dpi = DpiAt(p); const RECT work = WorkAt(p); const RECT r = ShapeAt(self, p);
    const LONG w = r.right - r.left, h = r.bottom - r.top;
    std::vector<LONG> xs, ys;
    GridLines(true, w, dpi, work, xs); GridLines(false, h, dpi, work, ys);
    NeighbourLines(self, true, w, dpi, xs); NeighbourLines(self, false, h, dpi, ys);
    auto inside = [&](LONG x, LONG y) { return x >= work.left && y >= work.top && x + w <= work.right && y + h <= work.bottom; };
    // Every candidate position, nearest first; neighbour alignment wins ties (it was listed after the grid).
    POINT best{}; double bestDist = 1e18;
    for (LONG x : xs) for (LONG y : ys) {
        if (!inside(x, y)) continue;
        const double d = std::hypot(static_cast<double>(x - p.x), static_cast<double>(y - p.y));
        if (d <= bestDist && Free(self, POINT{x, y})) { bestDist = d; best = POINT{x, y}; }
    }
    if (bestDist < 1e18) return best;
    return POINT{std::clamp(p.x, work.left, (std::max)(work.left, work.right - w)), std::clamp(p.y, work.top, (std::max)(work.top, work.bottom - h))};
}

struct Drag { HWND h = nullptr; POINT start{}, origin{}; bool moving = false; ULONGLONG lastPaint = 0; } drag;
// Display-paced ticks (one per display frame: 240 Hz on a 240 Hz monitor) for the widget glide, the gallery and the
// settings panel; they were 8 / 16 ms WM_TIMERs, which Windows runs at 64 Hz.
docky::vblank::Ticker glideTicker, galleryTicker, settingsTicker;
struct Glide { HWND h = nullptr; POINT from{}, to{}; LONGLONG start = 0; };
std::vector<Glide> glides;
constexpr UINT_PTR kGlideTimer = 7, kScrollTimer = 8;
double Seconds();
// One frame of a widget's scroll spring; keeps the timer while it moves.
void StepScroll(HWND h) {
    WidgetWin* w = WidgetOf(h);
    if (!w) { KillTimer(h, kScrollTimer); return; }
    const double now = Seconds(), dt = std::clamp(now - w->scrollStepAt, 0.0, 0.05);
    w->scrollStepAt = now;
    const bool moving = w->scroll.Step(dt);
    PaintWidgetWin(*w);
    if (!moving) KillTimer(h, kScrollTimer);
}
void ScrollTo(WidgetWin& w, float target) {
    target = std::clamp(target, w.scrollMin, w.scrollMax);
    w.scroll.SetSpec(docky::motion::tokens::Scroll);
    w.scroll.SetTarget(target);
    w.scrollStepAt = Seconds();
    SetTimer(w.hwnd, kScrollTimer, 8, nullptr);
}
constexpr double kGlideSeconds = 0.24;
double Seconds() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return static_cast<double>(c.QuadPart) / f.QuadPart; }
void MoveWidget(WidgetWin& w, POINT p, bool repaint) {
    w.pos = p;
    const RECT r = WindowRectFor(w);
    SetWindowPos(w.hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (repaint) PaintWidgetWin(w);  // the glass shows the wallpaper under the new position
}
void StartGlide(WidgetWin& w, POINT to) {
    glides.erase(std::remove_if(glides.begin(), glides.end(), [&](const Glide& g) { return g.h == w.hwnd; }), glides.end());
    if (to.x == w.pos.x && to.y == w.pos.y) { PaintWidgetWin(w); return; }
    glides.push_back({w.hwnd, w.pos, to, static_cast<LONGLONG>(Seconds() * 1e6)});
    glideTicker.Start(host, docky::vblank::kMsgVBlank);
}
void StepGlide(HWND h) {
    auto it = std::find_if(glides.begin(), glides.end(), [&](const Glide& g) { return g.h == h; });
    WidgetWin* w = WidgetOf(h);
    if (it == glides.end() || !w) { KillTimer(h, kGlideTimer); return; }
    const double t = std::clamp((Seconds() * 1e6 - it->start) / (kGlideSeconds * 1e6), 0.0, 1.0);
    const double e = 1 - std::pow(1 - t, 3);  // ease-out: quick start, gentle landing
    MoveWidget(*w, POINT{it->from.x + static_cast<LONG>(std::lround((it->to.x - it->from.x) * e)), it->from.y + static_cast<LONG>(std::lround((it->to.y - it->from.y) * e))}, true);
    if (t >= 1) { glides.erase(it); KillTimer(h, kGlideTimer); SaveConfig(); }
}

LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE:
        if (const auto* w = WidgetOf(h); w && w->kind == kDropWidgetKind && !EditMode()) return MA_ACTIVATE;
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: if (!desktopRaised && drag.h != h) { auto* p = reinterpret_cast<WINDOWPOS*>(lp); if (!(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM; } break;
    case WM_LBUTTONDOWN: {
        WidgetWin* w = WidgetOf(h); if (!w) return 0;
        if (EditMode()) {  // the remove badge
            const UINT dpi = DpiAt(w->pos); const float x = GET_X_LPARAM(lp) * 96.f / dpi - (kShadow + 3), y = GET_Y_LPARAM(lp) * 96.f / dpi - (kShadow + 3);
            if (x * x + y * y <= 12.f * 12.f) { RemoveWidget(h); RefreshGallery(); return 0; }
        }
        if (w->kind == kDropWidgetKind && !EditMode()) SetFocus(h);
        glides.erase(std::remove_if(glides.begin(), glides.end(), [&](const Glide& g) { return g.h == h; }), glides.end()); KillTimer(h, kGlideTimer);
        drag = {h, {}, w->pos, false, 0}; GetCursorPos(&drag.start); SetCapture(h); return 0;
    }
    case WM_MOUSEMOVE:
        if (drag.h == h && (wp & MK_LBUTTON)) {
            POINT pt{}; GetCursorPos(&pt);
            if (!drag.moving && (std::abs(pt.x - drag.start.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - drag.start.y) > GetSystemMetrics(SM_CYDRAG))) {
                drag.moving = true;  // above other windows while it is carried, like macOS
                SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            if (drag.moving) if (WidgetWin* w = WidgetOf(h)) {
                const ULONGLONG now = GetTickCount64();
                const bool repaint = now - drag.lastPaint >= 16;  // glass follows at display rate, not per mouse event
                if (repaint) drag.lastPaint = now;
                MoveWidget(*w, Magnet(*w, POINT{drag.origin.x + pt.x - drag.start.x, drag.origin.y + pt.y - drag.start.y}), repaint);
            }
        } else if (auto* w = WidgetOf(h); w && w->kind == kDropWidgetKind && !EditMode()) {
            if (!w->dropTracking) {
                TRACKMOUSEEVENT leave{sizeof(leave), TME_LEAVE, h, 0};
                w->dropTracking = TrackMouseEvent(&leave) != FALSE;
            }
            const auto layout = DropLayoutOf(*w);
            const auto point = DropPoint(*w, lp);
            const int start = w->dropPage * layout.Capacity();
            int hovered = -1;
            for (int i = start; i < std::min(static_cast<int>(dropEntries.size()), start + layout.Capacity()); ++i) {
                if (DropContains(layout.Tile(i - start), point)) { hovered = i; break; }
            }
            if (hovered != w->dropPreview) { w->dropPreview = hovered; PaintWidgetWin(*w); }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (auto* w = WidgetOf(h); w && w->kind == kDropWidgetKind) {
            w->dropTracking = false;
            if (w->dropPreview >= 0) { w->dropPreview = -1; PaintWidgetWin(*w); }
        }
        return 0;
    case WM_LBUTTONUP: {
        const bool moved = drag.h == h && drag.moving; drag = {}; ReleaseCapture();
        WidgetWin* w = WidgetOf(h); if (!w) return 0;
        if (moved) {
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            PlaceWidget(*w, true);
            POINT pt{}; GetCursorPos(&pt); RECT gr = galleryRect;
            if (EditMode() && PtInRect(&gr, pt)) { RemoveWidget(h); RefreshGallery(); return 0; }  // dropped back on the gallery
            StartGlide(*w, Settle(*w, w->pos)); SaveConfig(); return 0;
        }
        if (w->kind == kDropWidgetKind && !EditMode()) {
            const auto layout = DropLayoutOf(*w);
            const auto point = DropPoint(*w, lp);
            if (DropContains(layout.open, point)) OpenDropWidget(*w);
            else if (DropContains(layout.previous, point)) ChangeDropPage(*w, -1);
            else if (DropContains(layout.next, point)) ChangeDropPage(*w, 1);
        } else if (ExtWidget* e = FindExt(w->kind)) {
            if (e->notify && IsWindow(e->notify)) {
                const UINT dpi = DpiAt(w->pos); POINT pt{}; GetCursorPos(&pt);
                const auto s = SizeDip(w->size);
                const int row = ListRowAt(*e, w->size, D2D1::RectF(0, 0, s.width, s.height), D2D1::Point2F((pt.x - w->pos.x) * 96.f / dpi, (pt.y - w->pos.y) * 96.f / dpi));
                DWORD pid = 0; GetWindowThreadProcessId(e->notify, &pid); if (pid) AllowSetForegroundWindow(pid);
                docky::ext::SendJson(e->notify, host, "{\"v\":1,\"type\":\"widget.clicked\",\"id\":" + dock::json::Write(Value::MakeString(U(e->id)), 0) + ",\"item\":\"" + std::to_string(row) + "\"}", 300);
            }
        } else OpenBuiltin(w->kind);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (auto* w = WidgetOf(h); w && w->kind == kDropWidgetKind) PaintWidgetWin(*w);
        break;
    case WM_PASTE:
    case WM_KEYDOWN:
        if (auto* w = WidgetOf(h); w && w->kind == kDropWidgetKind && !EditMode()) {
            if (m == WM_PASTE || (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))) {
                if (m == WM_PASTE || !(lp & 0x40000000)) PasteDropWidget(*w);
                return 0;
            }
            if (wp == VK_RETURN && !(lp & 0x40000000)) { OpenDropWidget(*w); return 0; }
            if (wp == VK_LEFT || wp == VK_RIGHT) { ChangeDropPage(*w, wp == VK_LEFT ? -1 : 1); return 0; }
        }
        break;
    case WM_TIMER: if (wp == kGlideTimer) StepGlide(h); else if (wp == kScrollTimer) StepScroll(h); return 0;
    case WM_MOUSEWHEEL: {  // Calendar and Weather scroll (one notch = 48 DIP; touchpads send finer steps)
        WidgetWin* w = WidgetOf(h);
        if (!w || EditMode() || drag.h) return 0;
        if (w->kind == kDropWidgetKind) {
            ChangeDropPage(*w, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        }
        if (!Scrollable(w->kind)) return 0;
        if (w->scrollMax <= w->scrollMin) { PaintWidgetWin(*w); if (w->scrollMax <= w->scrollMin) return 0; }  // range known after a paint
        const float delta = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 48.f;
        w->lastScroll = GetTickCount64();
        ScrollTo(*w, static_cast<float>(w->scroll.Target()) + delta);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (drag.h == h && drag.moving) {  // capture lost mid-drag: drop where it is
            drag = {};
            if (WidgetWin* w = WidgetOf(h)) { SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); PlaceWidget(*w, true); StartGlide(*w, Settle(*w, w->pos)); SaveConfig(); }
        }
        return 0;
    case WM_RBUTTONUP: { WidgetWin* w = WidgetOf(h); if (w) { POINT pt{}; GetCursorPos(&pt); ShowWidgetMenu(*w, pt); } return 0; }
    case WM_DPICHANGED: if (WidgetWin* w = WidgetOf(h)) { PlaceWidget(*w, true); PaintWidgetWin(*w); } return 0;
    case WM_DESTROY:
        if (auto* widget = WidgetOf(h); widget && widget->dropRegistered) RevokeDragDrop(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool CreateWidgetWindow(WidgetWin& w) {
    const DWORD style = WS_EX_LAYERED | WS_EX_TOOLWINDOW | (w.kind == kDropWidgetKind ? 0 : WS_EX_NOACTIVATE);
    w.hwnd = CreateWindowExW(style, kWidgetClass, L"DockOver widget", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w.hwnd) return false;
    if (w.kind == kDropWidgetKind) {
        auto* target = new WidgetDropTarget(w.hwnd);
        w.dropRegistered = SUCCEEDED(RegisterDragDrop(w.hwnd, target));
        target->Release();
    }
    RegisterLayer(w.hwnd, w.canvas.get());  // liquid glass menus and panels refract the widgets under them
    PlaceWidget(w, false); PaintWidgetWin(w); ShowWindow(w.hwnd, SW_SHOWNOACTIVATE); PlaceWidget(w, true);
    return true;
}

// First free grid slot from the top-left of the primary work area, column by column (macOS fills from the top-left).
POINT FreeSlot(Size size) {
    const RECT work = PrimaryWork(); const UINT dpi = DpiAt(POINT{work.left, work.top});
    const auto s = SizeDip(size);
    const int step = Px(kSmall + kGap, dpi) / 2;  // half-cell steps so medium/large fit next to small ones
    for (int x = work.left + Px(kMargin, dpi); x + Px(s.width, dpi) <= work.right - Px(kMargin + 120, dpi); x += step)
        for (int y = work.top + Px(kMargin, dpi); y + Px(s.height, dpi) <= work.bottom - Px(kMargin, dpi); y += step) {
            RECT want{x - Px(kGap / 2, dpi), y - Px(kGap / 2, dpi), x + Px(s.width + kGap / 2, dpi), y + Px(s.height + kGap / 2, dpi)};
            bool free = true;
            for (const auto& o : config.widgets) { const auto os = SizeDip(o.size); RECT r{o.pos.x, o.pos.y, o.pos.x + Px(os.width, dpi), o.pos.y + Px(os.height, dpi)}; RECT x2{}; if (IntersectRect(&x2, &want, &r)) { free = false; break; } }
            if (free) return POINT{x, y};
        }
    return POINT{work.left + Px(kMargin, dpi), work.top + Px(kMargin, dpi)};
}

void AddWidget(const std::wstring& kind, Size size) {
    if (config.widgets.size() >= 40) return;
    WidgetWin w; w.kind = kind; w.size = size; w.pos = FreeSlot(size);
    config.widgets.push_back(std::move(w));
    CreateWidgetWindow(config.widgets.back());
    SaveConfig();
}
void RemoveWidget(HWND h) {
    for (auto it = config.widgets.begin(); it != config.widgets.end(); ++it)
        if (it->hwnd == h) { UnregisterLayer(h); DestroyWindow(h); config.widgets.erase(it); SaveConfig(); return; }
}
void RepaintAll() { for (auto& w : config.widgets) PaintWidgetWin(w); RefreshGallery(); }

void SetRaised(bool raised) {
    if (raised == desktopRaised) return;
    desktopRaised = raised;
    for (auto& w : config.widgets) if (w.hwnd) SetWindowPos(w.hwnd, raised ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    GetStacks().Raise(raised);
}
// "Show desktop" (Win+D) puts the desktop window above all applications; widgets then come up with it, and go back to
// the bottom when another app becomes active.
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND w, LONG, LONG, DWORD, DWORD) {
    wchar_t cls[64]{}; if (w) GetClassNameW(w, cls, 64);
    const bool desktop = !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman");
    DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return;
    SetRaised(desktop);
}

// ---- Menus (docky's unified popup, opaque theme: this app has no screen capture) --------------------------------
using liquid::menu::ItemKind; using liquid::menu::MenuItem;
constexpr UINT kCmdAdd = 0x100, kCmdSize = 0x200, kCmdRemove = 0x300, kCmdGallery = 0x301, kCmdStacks = 0x302, kCmdGroupKind = 0x303,
               kCmdGroupDate = 0x304, kCmdHideIcons = 0x305, kCmdQuit = 0x306, kCmdInPlace = 0x307, kCmdPanel = 0x308, kCmdSettings = 0x309, kCmdEdit = 0x30A,
               kCmdClearDockDrop = 0x30B, kCmdPasteDockDrop = 0x30C, kCmdAddExt = 0x400;
std::vector<MenuItem> AddItems() {
    std::vector<MenuItem> items;
    const auto kinds = BuiltinKinds();
    for (size_t i = 0; i < kinds.size(); ++i) items.push_back({kinds[i].name, ItemKind::Command, true, false, {}, kCmdAdd + static_cast<UINT>(i)});
    if (!extWidgets.empty()) items.push_back({L"", ItemKind::Separator, false, false, {}});
    for (size_t i = 0; i < extWidgets.size() && i < 100; ++i) items.push_back({extWidgets[i].title.empty() ? extWidgets[i].id : extWidgets[i].title, ItemKind::Command, true, false, {}, kCmdAddExt + static_cast<UINT>(i)});
    return items;
}
std::vector<MenuItem> AppItems() {
    std::vector<MenuItem> items;
    items.push_back({docky::ui::Text(L"Edit Widgets…"), ItemKind::Command, true, false, {}, kCmdGallery});
    items.push_back({docky::ui::Text(L"Widgets & Stacks Settings\u2026"), ItemKind::Command, true, false, {}, kCmdSettings});
    items.push_back({docky::ui::Text(L"Add Widget"), ItemKind::Submenu, true, false, AddItems()});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({docky::ui::Text(L"Use Stacks"), ItemKind::Command, true, config.stacks, {}, kCmdStacks});
    items.push_back({docky::ui::Text(L"Group Stacks By"), ItemKind::Submenu, config.stacks, false,
                     {{docky::ui::Text(L"Kind"), ItemKind::Command, true, config.by == GroupBy::Kind, {}, kCmdGroupKind}, {docky::ui::Text(L"Date Modified"), ItemKind::Command, true, config.by == GroupBy::Date, {}, kCmdGroupDate}}});
    items.push_back({docky::ui::Text(L"Open Stacks"), ItemKind::Submenu, config.stacks, false,
                     {{docky::ui::Text(L"On the Desktop"), ItemKind::Command, true, config.style == StackStyle::InPlace, {}, kCmdInPlace}, {docky::ui::Text(L"In a Glass Panel"), ItemKind::Command, true, config.style == StackStyle::Panel, {}, kCmdPanel}}});
    items.push_back({docky::ui::Text(L"Hide Windows Desktop Icons"), ItemKind::Command, config.stacks, config.hideIcons, {}, kCmdHideIcons});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({docky::ui::Text(L"Quit DockOver Desktop"), ItemKind::Command, true, false, {}, kCmdQuit});
    return items;
}
void ApplyStacks() {
    GetStacks().SetCells(config.stackCells);
    GetStacks().SetFreePlacement(config.freeStacks);
    GetStacks().OnCellsChanged([](const std::map<std::wstring, POINT>& cells) { config.stackCells = cells; SaveConfig(); });
    if (config.stacks) { GetStacks().Start(host, config.by, config.style); GetStacks().SetGroupBy(config.by); } else GetStacks().Stop();
    HideDesktopIcons(config.stacks && config.hideIcons);
}
void EditWidget(HWND widget);
void Command(UINT id, HWND widget) {
    const auto kinds = BuiltinKinds();
    if (id >= kCmdAdd && id < kCmdAdd + kinds.size()) AddWidget(kinds[id - kCmdAdd].id, kinds[id - kCmdAdd].defaultSize);
    else if (id >= kCmdAddExt && id < kCmdAddExt + extWidgets.size()) AddWidget(L"ext:" + extWidgets[id - kCmdAddExt].id, extWidgets[id - kCmdAddExt].size);
    else if (id >= kCmdSize && id < kCmdSize + 3) { if (WidgetWin* w = WidgetOf(widget)) { w->size = static_cast<Size>(id - kCmdSize); w->pos = Settle(*w, w->pos); PlaceWidget(*w, true); PaintWidgetWin(*w); SaveConfig(); } }
    else if (id == kCmdRemove) RemoveWidget(widget);
    else if (id == kCmdClearDockDrop) {
        if (WidgetQueue().Write({})) { RefreshDropDraft(); RepaintAll(); }
    }
    else if (id == kCmdPasteDockDrop) {
        if (auto* w = WidgetOf(widget); w && w->kind == kDropWidgetKind) PasteDropWidget(*w);
    }
    else if (id == kCmdGallery) { if (gallery) CloseGallery(); else OpenGallery(); }  // Edit Widgets toggles
    else if (id == kCmdSettings) OpenSettings();
    else if (id == kCmdEdit) EditWidget(widget);
    else if (id == kCmdStacks) { config.stacks = !config.stacks; ApplyStacks(); SaveConfig(); }
    else if (id == kCmdGroupKind || id == kCmdGroupDate) { config.by = id == kCmdGroupDate ? GroupBy::Date : GroupBy::Kind; GetStacks().SetGroupBy(config.by); SaveConfig(); }
    else if (id == kCmdInPlace || id == kCmdPanel) { config.style = id == kCmdPanel ? StackStyle::Panel : StackStyle::InPlace; GetStacks().SetStyle(config.style); SaveConfig(); }
    else if (id == kCmdHideIcons) { config.hideIcons = !config.hideIcons; HideDesktopIcons(config.stacks && config.hideIcons); SaveConfig(); }
    else if (id == kCmdQuit) PostMessageW(host, WM_CLOSE, 0, 0);
}
void ShowMenu(std::vector<MenuItem> items, POINT at, HWND widget) {
    liquid::popup::Show(host, at, std::move(items), [widget](const MenuItem& item) { Command(item.commandId, widget); });
}
bool WidgetHasPage(const WidgetWin& w);
std::wstring WidgetName(const WidgetWin& w) {
    if (w.kind.rfind(L"ext:", 0) == 0) {
        for (const auto& e : extWidgets) if (L"ext:" + e.id == w.kind) return e.title.empty() ? e.id : e.title;
        return w.kind.substr(4);
    }
    for (const auto& k : BuiltinKinds()) if (k.id == w.kind) return k.name;
    return w.kind;
}
void ShowWidgetMenu(WidgetWin& w, POINT at) {
    std::vector<MenuItem> items;
    const bool ext = w.kind.rfind(L"ext:", 0) == 0;
    const wchar_t* names[3] = {L"Small", L"Medium", L"Large"};
    for (UINT i = 0; i < 3; ++i) items.push_back({names[i], ItemKind::Command, true, static_cast<UINT>(w.size) == i, {}, kCmdSize + i});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    // macOS: "Edit “Weather”…" opens the widget's own settings beside it; general settings stay separate
    if (WidgetHasPage(w)) items.push_back({L"Edit \u201C" + WidgetName(w) + L"\u201D\u2026", ItemKind::Command, true, false, {}, kCmdEdit});
    if (w.kind == kDropWidgetKind) {
        items.push_back({docky::ui::Text(L"Paste Text"), ItemKind::Command, IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE, false, {}, kCmdPasteDockDrop});
        items.push_back({docky::ui::Text(L"Clear Staged Items"), ItemKind::Command, !dropEntries.empty(), false, {}, kCmdClearDockDrop});
    }
    items.push_back({docky::ui::Text(L"Remove Widget"), ItemKind::Command, true, false, {}, kCmdRemove});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({docky::ui::Text(L"Edit Widgets…"), ItemKind::Command, true, false, {}, kCmdGallery});
    items.push_back({docky::ui::Text(L"Widgets & Stacks Settings\u2026"), ItemKind::Command, true, false, {}, kCmdSettings});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({docky::ui::Text(L"Quit DockOver Desktop"), ItemKind::Command, true, false, {}, kCmdQuit});
    (void)ext;
    ShowMenu(std::move(items), at, w.hwnd);
}

// Editing never moves or minimizes other windows: the gallery and Settings glass sample what is really behind them
// (GlassWindow::SetLiveBackdrop). An earlier "Show desktop" approach was removed at the user's request (2026-11-11).
bool SettingsVisible();

// ---- Widget gallery (macOS "Edit Widgets"): a Liquid Glass panel at the bottom of the screen ----------------------
// Sidebar with search and one row per widget source; previews of every widget in its sizes; drag a preview onto the
// desktop (or click it) to add it; while the gallery is open every widget shows a remove badge. Done / Esc closes.
void OpenSettings();
void RemoveWidget(HWND h);
void RefreshSettings();
using Microsoft::WRL::ComPtr;
GlassWindow galleryWin;
struct GalleryTile { std::wstring kind, name; Size size; D2D1_RECT_F rect{}; };
struct GalleryRow { std::wstring id, name, glyph; unsigned color = 0x0A84FF; D2D1_RECT_F rect{}; };
std::vector<GalleryTile> galleryTiles; std::vector<GalleryRow> galleryRows;
RECT galleryRect{}; int galleryHover = -1, galleryRowSel = 0, galleryScroll = 0; bool galleryMouseWasDown = false;
std::wstring gallerySearch; D2D1_RECT_F galleryDone{}, gallerySearchBox{}, gallerySettingsLink{};
struct GalleryPress { bool down = false; int tile = -1; POINT at{}; } galleryPress;
constexpr float kGalleryW = 820.f, kGalleryH = 392.f, kSidebar = 196.f, kPreviewScale = 0.5f;
bool EditMode() { return gallery != nullptr; }

D2D1_COLOR_F Rgb(unsigned c, float a = 1.f) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
std::vector<GalleryRow> GalleryRows() {
    std::vector<GalleryRow> rows{{L"", docky::ui::Text(L"All Widgets"), L"", 0x8E8E93}};
    const struct { const wchar_t* id; const wchar_t* glyph; unsigned color; } builtins[] = {
        {L"Clock", L"", 0x1C1C1E}, {L"Calendar", L"", 0xFF3B30}, {L"Weather", L"\uE753", 0x2F7AD6}, {L"Batteries", L"", 0x30D158}, {L"System", L"", 0x0A84FF}};
    for (const auto& b : builtins) rows.push_back({b.id, b.id, b.glyph, b.color});
    for (const auto& e : extWidgets) {
        const std::wstring name = e.title.empty() ? e.id : e.title;
        if (std::none_of(rows.begin(), rows.end(), [&](const GalleryRow& r) { return r.id == L"ext:" + e.id; })) rows.push_back({L"ext:" + e.id, name, L"", e.hasAccent ? 0xFF9F0Au : 0x5E5CE6u});
    }
    return rows;
}
bool Matches(const std::wstring& text) {
    if (gallerySearch.empty()) return true;
    std::wstring a = text, b = gallerySearch;
    for (auto& c : a) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : b) c = static_cast<wchar_t>(towlower(c));
    return a.find(b) != std::wstring::npos;
}
void LayoutGallery() {
    const RECT work = PrimaryWork(); const UINT dpi = DpiAt(POINT{work.left, work.top});
    const int pw = Px(kGalleryW, dpi), ph = Px(kGalleryH, dpi);
    galleryRect = RECT{(work.left + work.right - pw) / 2, work.bottom - ph - Px(12, dpi), (work.left + work.right + pw) / 2, work.bottom - Px(12, dpi)};
    galleryRows = GalleryRows();
    galleryRowSel = std::clamp(galleryRowSel, 0, static_cast<int>(galleryRows.size()) - 1);
    float y = 54;
    for (auto& r : galleryRows) { r.rect = D2D1::RectF(12, y, kSidebar - 8, y + 30); y += 32; }
    gallerySearchBox = D2D1::RectF(12, 14, kSidebar - 8, 42);
    galleryDone = D2D1::RectF(kGalleryW - 96, kGalleryH - 40, kGalleryW - 18, kGalleryH - 12);
    // Tiles: the selected source in all three sizes, or every widget at its usual size ("All Widgets").
    galleryTiles.clear();
    const std::wstring group = galleryRows[static_cast<size_t>(galleryRowSel)].id;
    auto add = [&](const std::wstring& kind, const std::wstring& name, const std::wstring& g, Size defaultSize) {
        if (!Matches(name) && !Matches(g)) return;
        if (group.empty()) { galleryTiles.push_back({kind, name, defaultSize}); if (kind == L"calendar") galleryTiles.push_back({kind, name, Size::Small}); return; }
        if (group != g) return;
        for (Size s : {Size::Small, Size::Medium, Size::Large}) galleryTiles.push_back({kind, name, s});
    };
    for (const auto& k : BuiltinKinds()) add(k.id, k.name, k.group, k.defaultSize);
    for (const auto& e : extWidgets) add(L"ext:" + e.id, e.title.empty() ? e.id : e.title, L"ext:" + e.id, e.size);
    const float left = kSidebar + 20, right = kGalleryW - 18;
    float x = left, top = group.empty() && gallerySearch.empty() ? 150.f : 46.f, rowH = 0;
    for (auto& t : galleryTiles) {
        const auto s = SizeDip(t.size);
        const float w = s.width * kPreviewScale, h = s.height * kPreviewScale;
        if (x + w > right) { x = left; top += rowH + 14; rowH = 0; }
        t.rect = D2D1::RectF(x, top, x + w, top + h);
        x += w + 12; rowH = (std::max)(rowH, h);
    }
}
float GalleryOffset() { return -galleryScroll * 1.f; }
void PaintGallery() {
    if (!gallery) return;
    const float W = kGalleryW, H = kGalleryH;
    galleryWin.Render(galleryRect, HWND_TOPMOST, D2D1::RectF(0, 0, W, H), 26.f, [&](ID2D1DeviceContext* dc, bool gpu) {
        // Text on the glass: white with soft secondary greys, as docky's Control Center.
        const D2D1_COLOR_F ink = gpu ? D2D1::ColorF(1, 1, 1, 0.96f) : theme.ink, dim = gpu ? D2D1::ColorF(1, 1, 1, 0.62f) : theme.secondary;
        ComPtr<ID2D1SolidColorBrush> b; dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.12f), &b);
        if (!gpu) { b->SetColor(theme.dark ? D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.97f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.98f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 26, 26), b.Get()); }
        // Search field
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.13f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(gallerySearchBox, 14, 14), b.Get());
        b->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.f, 0.9f));
        if (GetForegroundWindow() == gallery) dc->DrawRoundedRectangle(D2D1::RoundedRect(gallerySearchBox, 14, 14), b.Get(), 1.5f);
        Text(dc, L"", IconFont(12.f), D2D1::RectF(gallerySearchBox.left + 8, gallerySearchBox.top, gallerySearchBox.left + 28, gallerySearchBox.bottom), dim);
        Text(dc, gallerySearch.empty() ? docky::ui::Text(L"Search Widgets") : gallerySearch, Font(12.5f), D2D1::RectF(gallerySearchBox.left + 30, gallerySearchBox.top, gallerySearchBox.right - 8, gallerySearchBox.bottom), gallerySearch.empty() ? dim : ink);
        // Sidebar rows
        for (size_t i = 0; i < galleryRows.size(); ++i) {
            const auto& r = galleryRows[i];
            if (r.rect.bottom > H - 8) break;
            if (static_cast<int>(i) == galleryRowSel) { b->SetColor(D2D1::ColorF(1, 1, 1, 0.16f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r.rect, 8, 8), b.Get()); }
            const D2D1_RECT_F icon = D2D1::RectF(r.rect.left + 6, r.rect.top + 5, r.rect.left + 26, r.rect.top + 25);
            b->SetColor(Rgb(r.color)); dc->FillRoundedRectangle(D2D1::RoundedRect(icon, 5, 5), b.Get());
            Text(dc, r.glyph, IconFont(11.f), icon, D2D1::ColorF(1, 1, 1, 1));
            Text(dc, r.name, Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(r.rect.left + 34, r.rect.top, r.rect.right - 6, r.rect.bottom), ink);
        }
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
        dc->FillRectangle(D2D1::RectF(kSidebar + 4, 14, kSidebar + 5, H - 54), b.Get());
        const bool all = galleryRows[static_cast<size_t>(galleryRowSel)].id.empty() && gallerySearch.empty();
        const float cl = kSidebar + 20, cr = W - 18;
        if (all) {  // header card
            const D2D1_RECT_F card = D2D1::RectF(cl, 14, cr, 112);
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(card, 14, 14), b.Get());
            Text(dc, docky::ui::Text(L"Widgets on Desktop"), Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(card.left + 18, card.top + 14, card.right - 196, card.top + 36), ink);
            ComPtr<IDWriteTextFormat> wrap;
            if (SUCCEEDED(DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f, L"", &wrap)))
                Text(dc, docky::ui::Text(L"Place widgets directly on the desktop by dragging them from the gallery, or click one to add it. Widgets snap to each other and to the grid."), wrap.Get(),
                     D2D1::RectF(card.left + 18, card.top + 40, card.right - 196, card.bottom - 10), dim);
            gallerySettingsLink = D2D1::RectF(card.right - 168, card.top + 35, card.right - 22, card.top + 63);
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.14f)); dc->FillRoundedRectangle(D2D1::RoundedRect(gallerySettingsLink, 14, 14), b.Get());
            Text(dc, docky::ui::Text(L"Widget Settings…"), Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), gallerySettingsLink, ink);
            Text(dc, docky::ui::Text(L"Suggestions"), Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(cl + 2, 124, cr, 142), dim);
        } else {
            gallerySettingsLink = D2D1::RectF();
            Text(dc, gallerySearch.empty() ? galleryRows[static_cast<size_t>(galleryRowSel)].name : docky::ui::Text(L"Results"), Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(cl + 2, 14, cr, 38), ink);
        }
        // Previews (scrolled), clipped to the content area
        const float clipTop = all ? 146.f : 42.f;
        dc->PushAxisAlignedClip(D2D1::RectF(cl - 4, clipTop, cr + 4, H - 52), D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
        for (size_t i = 0; i < galleryTiles.size(); ++i) {
            const auto& t = galleryTiles[i];
            const D2D1_RECT_F r = D2D1::RectF(t.rect.left, t.rect.top + GalleryOffset(), t.rect.right, t.rect.bottom + GalleryOffset());
            if (r.bottom < clipTop || r.top > H - 52) continue;
            const auto s = SizeDip(t.size);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(kPreviewScale, kPreviewScale) * D2D1::Matrix3x2F::Translation(r.left, r.top) * old);
            const D2D1_RECT_F inner = D2D1::RectF(0, 0, s.width, s.height);
            const RECT screen{galleryRect.left + static_cast<LONG>(r.left), galleryRect.top + static_cast<LONG>(r.top), galleryRect.left + static_cast<LONG>(r.right), galleryRect.top + static_cast<LONG>(r.bottom)};
            const Theme content = PaintPlate(dc, inner, screen, theme, PlateToneOf(t.kind));
            dc->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            PaintWidget(dc, t.kind, t.size, inner, content, FindExt(t.kind));
            dc->PopAxisAlignedClip();
            dc->SetTransform(old);
            if (static_cast<int>(i) == galleryHover) { b->SetColor(D2D1::ColorF(1, 1, 1, 0.55f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), 14, 14), b.Get(), 2.f); }
        }
        dc->PopAxisAlignedClip();
        if (galleryTiles.empty()) Text(dc, docky::ui::Text(L"No widgets match"), Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cl, 150, cr, 180), dim);
        // Bottom bar
        Text(dc, docky::ui::Text(L"Drag a widget to place it on the desktop…"), Font(12.5f), D2D1::RectF(16, H - 40, W - 120, H - 12), dim);
        b->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.f, 1.f)); dc->FillRoundedRectangle(D2D1::RoundedRect(galleryDone, 14, 14), b.Get());
        Text(dc, docky::ui::Text(L"Done"), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), galleryDone, D2D1::ColorF(1, 1, 1, 1));
    });
}
void RefreshGallery() { if (gallery) { LayoutGallery(); PaintGallery(); } }
void CloseGallery() {
    if (!gallery) return;
    HWND h = gallery;
    gallery = nullptr;  // first: hiding the active gallery sends WM_ACTIVATE, which must not repaint (and re-show) it
    KillTimer(h, 1); galleryTicker.Stop(); galleryWin.Hide();
    if (GetCapture() == h) ReleaseCapture();
    galleryPress = {};
    for (auto& w : config.widgets) PaintWidgetWin(w);  // remove badges disappear
}
int GalleryTileAt(D2D1_POINT_2F p) {
    const float clipTop = galleryRows[static_cast<size_t>(galleryRowSel)].id.empty() && gallerySearch.empty() ? 146.f : 42.f;
    if (p.y < clipTop || p.y > kGalleryH - 52) return -1;
    for (size_t i = 0; i < galleryTiles.size(); ++i) { const auto& r = galleryTiles[i].rect; if (p.x >= r.left && p.x < r.right && p.y >= r.top + GalleryOffset() && p.y < r.bottom + GalleryOffset()) return static_cast<int>(i); }
    return -1;
}
bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
// Dragging a preview out of the gallery creates the widget under the pointer and hands the drag to it: it then moves,
// snaps and glides like any widget. Dropped back on the gallery it is removed again.
void BeginDragFromGallery(int tile, POINT cursor) {
    const auto t = galleryTiles[static_cast<size_t>(tile)];
    const UINT dpi = DpiAt(cursor); const auto s = SizeDip(t.size);
    WidgetWin w; w.kind = t.kind; w.size = t.size;
    w.pos = POINT{cursor.x - Px(s.width / 2, dpi), cursor.y - Px(s.height / 2, dpi)};
    config.widgets.push_back(std::move(w));
    WidgetWin& added = config.widgets.back();
    if (!CreateWidgetWindow(added)) { config.widgets.pop_back(); return; }
    ReleaseCapture();
    drag = {added.hwnd, cursor, added.pos, true, 0};
    SetWindowPos(added.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetCapture(added.hwnd);
}
LRESULT CALLBACK GalleryProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == docky::vblank::kMsgVBlank) { galleryTicker.Ack(); m = WM_TIMER; w = 1; }  // the per-frame tick
    if (!gallery) return DefWindowProcW(h, m, w, l);
    const UINT dpi = DpiAt(POINT{galleryRect.left, galleryRect.top}); const float k = dpi / 96.f;
    const D2D1_POINT_2F p{GET_X_LPARAM(l) / k, GET_Y_LPARAM(l) / k};
    switch (m) {
    case WM_MOUSEMOVE: {
        if (galleryPress.down && galleryPress.tile >= 0 && (w & MK_LBUTTON)) {
            POINT pt{}; GetCursorPos(&pt);
            if (std::abs(pt.x - galleryPress.at.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - galleryPress.at.y) > GetSystemMetrics(SM_CYDRAG)) {
                galleryPress.down = false; BeginDragFromGallery(galleryPress.tile, pt); return 0;
            }
        }
        const int next = GalleryTileAt(p);
        if (next != galleryHover) { galleryHover = next; PaintGallery(); }
        TRACKMOUSEEVENT ev{sizeof(ev), TME_LEAVE, h, 0}; TrackMouseEvent(&ev); return 0;
    }
    case WM_MOUSELEAVE: if (galleryHover != -1) { galleryHover = -1; PaintGallery(); } return 0;
    case WM_LBUTTONDOWN:
        if (In(galleryDone, p)) { CloseGallery(); return 0; }  // on press: nothing later (capture, drags) can swallow it
        galleryPress = {true, GalleryTileAt(p), {}}; GetCursorPos(&galleryPress.at); if (galleryPress.tile >= 0) SetCapture(h); return 0;
    case WM_LBUTTONUP: {
        const auto press = galleryPress; galleryPress = {}; if (GetCapture() == h) ReleaseCapture();
        if (In(galleryDone, p)) { CloseGallery(); return 0; }
        if (In(gallerySettingsLink, p)) { OpenSettings(); return 0; }
        if (In(gallerySearchBox, p)) { SetForegroundWindow(h); PaintGallery(); return 0; }
        for (size_t i = 0; i < galleryRows.size(); ++i) if (In(galleryRows[i].rect, p)) { galleryRowSel = static_cast<int>(i); galleryScroll = 0; RefreshGallery(); return 0; }
        if (press.down && press.tile >= 0 && press.tile == GalleryTileAt(p)) { const auto t = galleryTiles[static_cast<size_t>(press.tile)]; AddWidget(t.kind, t.size); }
        return 0;
    }
    case WM_MOUSEWHEEL: galleryScroll = (std::max)(0, galleryScroll - GET_WHEEL_DELTA_WPARAM(w) / 3); PaintGallery(); return 0;
    case WM_CHAR:
        if (w == VK_BACK) { if (!gallerySearch.empty()) gallerySearch.pop_back(); }
        else if (w == VK_ESCAPE) { if (gallerySearch.empty()) { CloseGallery(); return 0; } gallerySearch.clear(); }
        else if (w >= 32 && gallerySearch.size() < 40) gallerySearch += static_cast<wchar_t>(w);
        galleryScroll = 0; RefreshGallery(); return 0;
    case WM_ACTIVATE: if (galleryWin.Visible()) PaintGallery(); return 0;
    case WM_TIMER: {  // a click outside (not on a widget) or Esc while not typing closes the gallery
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; POINT pt{}; GetCursorPos(&pt); RECT r = galleryRect;
        // A drag whose button-up never reached its widget (fast drags lose the capture) must not keep the gallery open:
        // with the button up, finish it here. Before this, such a drag blocked every way of closing by clicking outside.
        if (drag.h && !down) { HWND h2 = drag.h; ReleaseCapture(); SendMessageW(h2, WM_CAPTURECHANGED, 0, 0); drag = {}; }
        if (galleryPress.down && !down) galleryPress = {};
        bool onOwn = PtInRect(&r, pt) || (drag.h != nullptr && down);
        for (const auto& wdg : config.widgets) { RECT wr{}; if (GetWindowRect(wdg.hwnd, &wr) && PtInRect(&wr, pt)) onOwn = true; }
        if (HWND popup = FindWindowW(L"LiquidUnifiedPopup98d2", nullptr); popup && IsWindowVisible(popup)) onOwn = true;
        if (HWND s = FindWindowW(L"DockyDesktopSettings", nullptr); s && IsWindowVisible(s)) { RECT sr{}; GetWindowRect(s, &sr); if (PtInRect(&sr, pt)) onOwn = true; }
        const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if ((down && !galleryMouseWasDown && !onOwn) || (esc && (GetForegroundWindow() != h || gallerySearch.empty()))) { CloseGallery(); return 0; }
        galleryMouseWasDown = down;
        // Every frame (16 ms): what is behind the gallery shows through as it changes (video wallpapers included). Only the
        // glass is redrawn; the previews come from the content cache.
        if (!galleryPress.down && !drag.h) { if (galleryWin.GlassChanged()) PaintGallery(); else if (galleryWin.RefreshLiveBackdrop()) galleryWin.Redraw(); }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
void OpenGallery() {
    if (!galleryWin.Hwnd() && !galleryWin.Create(kGalleryClass, GalleryProc, WS_EX_TOPMOST, false)) return;
    galleryWin.SetFrosted(true); galleryWin.SetLiveBackdrop(true);
    gallery = galleryWin.Hwnd();
    galleryWin.Show();
    galleryHover = -1; galleryScroll = 0; gallerySearch.clear(); galleryMouseWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    LayoutGallery(); galleryWin.InvalidateBackdrop(); PaintGallery();
    galleryTicker.Start(gallery, docky::vblank::kMsgVBlank);
    for (auto& w : config.widgets) PaintWidgetWin(w);  // remove badges
    if (!devPng.empty()) galleryWin.RequestSnapshot(devPng + L"\\gallery.bmp"), PaintGallery();
}

// ---- Widget Settings: a Liquid Glass panel (style, spacing, snap distance, margin, Stacks, calendars) -------------
GlassWindow settingsWin;
RECT settingsRect{};
enum class Ctl { None, Done, Style, Gap, Snap, Margin, Glass, Stacks, Open, Group, Icons, FreeStacks, EditCalendars, RefreshCalendars, MenuBattery, Units, SetCity, RefreshWeather, BatteryWidget, BatteryMenu, ExtSetting,
                 CalColour, CalRemove, CalField, CalAdd, CalFile, CalUndo, CalWheel, CalBright, CalSwatch, CalPickerDone, ClockStyle, BatteryGlass, GlassRim, GlassRefraction, GlassFrost, GlassTint };
struct Control { Ctl id; D2D1_RECT_F rect; int index = 0; bool on = false; };
std::vector<Control> controls;
Ctl draggingSlider = Ctl::None;
constexpr float kSetW = 460.f, kDeviceRow = 34.f;
std::vector<DeviceBattery> settingsDevices;  // the rows of Settings > Batteries, as last painted
// ---- Glass Lab controls (liquid glass test/Lab.qml, the same as DockOver Settings' switches) ----
// Switch: the Lab's 116x50 control at 40 % (track 46.4x20, thumb 29.6x17.6), green when on; the thumb moves with the Lab's
// OutBack (420 ms, overshoot 0.65) and can be dragged. Slider: the Lab's 6 px track and 40x30 thumb at 60 %, blue fill.
// Pressed, a thumb grows (150 ms, OutCubic) and turns into a Liquid Glass lens refracting the track under it
// (GlassWindow::LocalGlass, Lab default material); its white fades out. Reduced motion: no growth, no lens.
constexpr float kLabSwitch = 0.4f, kLabSlider = 0.6f;
struct LabSwitchAnim { bool init = false, on = false; float from = 0, to = 0; ULONGLONG start = 0; };
std::map<long long, LabSwitchAnim> labSwitches;
Control labPressed{Ctl::None, {}};      // the switch or slider being pressed (or releasing)
bool labDown = false, labDragging = false;
float labPressFrom = 0, labPressTo = 0, labDragPos = 0, labStartX = 0, labStartPos = 0;
ULONGLONG labPressStart = 0;
D2D1_POINT_2F labPointer{};
bool LabMotion() { BOOL a = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &a, 0); return a != FALSE; }
float LabOutBack(float p) { const float x = p - 1.f; return 1.f + 2.65f * x * x * x + .65f * x * x; }
long long LabKey(Ctl id, int index) { return static_cast<long long>(id) * 100000 + index; }
float LabPress(Ctl id, int index, ULONGLONG now) {
    if (labPressed.id != id || labPressed.index != index) return 0.f;
    const float t = std::clamp((now - labPressStart) / 150.f, 0.f, 1.f), e = 1.f - std::pow(1.f - t, 3.f);
    return labPressFrom + (labPressTo - labPressFrom) * e;
}
void LabBeginPress(const Control& c, bool down) {
    const ULONGLONG now = GetTickCount64();
    labPressFrom = LabPress(c.id, c.index, now); labPressTo = down ? 1.f : 0.f; labPressStart = now;
    labPressed = c; labDown = down;
}
bool LabAnimating() {
    const ULONGLONG now = GetTickCount64();
    if (labPressed.id != Ctl::None && now - labPressStart < 170) return true;
    for (const auto& kv : labSwitches) if (kv.second.from != kv.second.to && now - kv.second.start < 440) return true;
    return false;
}
struct SliderSpec { Ctl id; float lo, hi; float* value; };
SliderSpec Slider(Ctl id) {
    auto& p = CurrentPrefs();
    switch (id) {
    case Ctl::Gap: return {id, 8, 32, &p.gap};
    case Ctl::Snap: return {id, 0, 32, &p.snap};
    case Ctl::Margin: return {id, 8, 48, &p.margin};
    case Ctl::GlassRim: return {id, 0, 1.5f, &p.glassRim};
    case Ctl::GlassRefraction: return {id, 0, 1, &p.glassRefraction};
    case Ctl::GlassFrost: return {id, 0, 1, &p.glassFrost};
    case Ctl::GlassTint: return {id, 0, 1, &p.glassTint};
    default: return {id, 0, 1, &p.glass};
    }
}
void SaveConfig();
// New spacing / margin: keep every widget on the same grid cell (the arrangement stays, the spacing changes).
void Repack(float oldGap, float oldMargin) {
    for (auto& w : config.widgets) {
        const UINT dpi = DpiAt(w.pos); const RECT work = WorkAt(w.pos);
        const float oldUnit = (kSmall + oldGap) / 2, newUnit = (kSmall + kGap) / 2;
        const float cx = ((w.pos.x - work.left) * 96.f / dpi - oldMargin) / oldUnit, cy = ((w.pos.y - work.top) * 96.f / dpi - oldMargin) / oldUnit;
        MoveWidget(w, POINT{work.left + Px(kMargin + std::round(cx) * newUnit, dpi), work.top + Px(kMargin + std::round(cy) * newUnit, dpi)}, true);
    }
}
// Settings pages: the general page (Widgets & Stacks) and one page per widget that has settings (macOS "Edit
// Widget"): Weather, Batteries, Calendar, and third-party widgets that declare "settings". A widget's right-click menu
// opens its own page next to it, so each widget only shows what belongs to it.
enum class Page { General, Weather, Batteries, Calendar, Clock, Ext };
Page settingsPage = Page::General;
std::wstring settingsExtId;   // Page::Ext: the third-party widget
HWND settingsAnchor = nullptr;  // the widget the page belongs to (the panel sits beside it)
ExtWidget* SettingsExt() {
    for (auto& e : extWidgets) if (e.id == settingsExtId) return &e;
    return nullptr;
}
// ---- Calendar page: the subscriptions in calendars.txt as a list (colour, name, status, remove) and an Add field ----
// The file stays the source of truth (it is re-read every paint, so a hand edit shows at once); every change is
// written back atomically and the feeds are fetched again. Links are shown by host only: they are secret addresses.
constexpr float kCalRow = 52.f, kCalFieldRow = 44.f;
namespace calconfig = docky::desk::calconfig;
calconfig::Config calConfig;               // as last painted (row indexes refer to it)
std::wstring calField, calError;           // the Add field's text; an inline message under it (empty: the hint)
bool calFocus = false;                     // the Add field takes the keyboard
ULONGLONG calCaretStart = 0; bool calCaretOn = true;
struct CalUndo { bool active = false; size_t index = 0; calconfig::Entry entry; std::wstring name; } calUndo;
Control settingsPressed{Ctl::None, {}};    // a button held down (drawn pressed; released outside: cancelled)
bool settingsPressedOut = false;           // the pointer moved off the held button (drawn normal until it returns)
bool SettingsPressed(Ctl id, int index = 0) { return settingsPressed.id == id && settingsPressed.index == index && !settingsPressedOut; }
std::wstring ReadWideFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB) s.erase(0, 3);
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>((std::max)(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
void LoadCalConfig() { calConfig = calconfig::Parse(ReadWideFile(CalendarFeeds::ConfigPath())); }
bool SaveCalConfig() {
    const std::string text = U(calconfig::Serialize(calConfig));
    CreateDirectoryW(DataDir().c_str(), nullptr);
    const std::wstring path = CalendarFeeds::ConfigPath(), temp = path + L".tmp";
    { std::ofstream f(temp, std::ios::binary | std::ios::trunc); if (!f) return false; f.write(text.data(), static_cast<std::streamsize>(text.size())); if (!f) return false; }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(temp.c_str()); return false; }
    Calendars().Refresh();
    return true;
}
const docky::desk::FeedStatus* CalStatus(const std::vector<docky::desk::FeedStatus>& feeds, const std::wstring& source) {
    for (const auto& f : feeds) if (f.source == source) return &f;
    return nullptr;
}
std::wstring CalName(const std::vector<docky::desk::FeedStatus>& feeds, const calconfig::Entry& e) {
    const auto* s = CalStatus(feeds, e.source);
    return s && !s->name.empty() ? s->name : calconfig::ShortName(e.source);
}
void CalFocus(bool on) { calFocus = on; calCaretStart = GetTickCount64(); calCaretOn = true; }
void CalAddText() {
    const auto r = calconfig::Add(calConfig, calField, [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; });
    calUndo.active = false;
    switch (r) {
    case calconfig::AddResult::Empty: CalFocus(true); calError.clear(); return;
    case calconfig::AddResult::NotCalendar: calError = L"Not an iCal link. It should start with https:// or webcal://"; CalFocus(true); return;
    case calconfig::AddResult::Duplicate: calError = L"This calendar is already in the list."; CalFocus(true); return;
    case calconfig::AddResult::Added:
        if (!SaveCalConfig()) { calConfig.entries.pop_back(); calError = L"Couldn't save calendars.txt."; return; }
        calField.clear(); calError.clear(); CalFocus(false); return;
    }
}
void CalChooseFile() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)}; ofn.hwndOwner = settingsWin.Hwnd(); ofn.lpstrFilter = L"iCalendar (*.ics)\0*.ics\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    calField = file; CalAddText();
}
void CalPaste() {
    if (!OpenClipboard(settingsWin.Hwnd())) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            std::wstring t = p; GlobalUnlock(h);
            for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
            calField += calconfig::Trim(t);
            if (calField.size() > 2048) calField.resize(2048);
        }
    }
    CloseClipboard();
    calError.clear();
}
// Colour picker: clicking a calendar's dot opens a colour wheel under its row (hue around, saturation outward), a
// brightness bar and the preset colours. Dragging previews the colour; it is saved to calendars.txt on release.
constexpr float kCalPickerH = 168.f, kCalWheelR = 68.f;
int calPicker = -1;                        // row whose picker is open, -1 none
Ctl calPickerDrag = Ctl::None;             // CalWheel or CalBright while dragging
float calH = 0, calS = 1, calV = 1;        // the picked colour (hue 0..360)
float calPickerTop = 0;                    // where the open picker was last drawn (DIP, settings window)
unsigned HsvToRgb(float h, float s, float v) {
    h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f) / 60.f; s = std::clamp(s, 0.f, 1.f); v = std::clamp(v, 0.f, 1.f);
    const int i = static_cast<int>(h) % 6; const float f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r = v, g = t, b = p;
    switch (i) { case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break; case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; case 5: r = v; g = p; b = q; break; default: break; }
    auto c = [](float x) { return static_cast<unsigned>(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}
void RgbToHsv(unsigned rgb, float& h, float& s, float& v) {
    const float r = ((rgb >> 16) & 255) / 255.f, g = ((rgb >> 8) & 255) / 255.f, b = (rgb & 255) / 255.f;
    const float mx = (std::max)({r, g, b}), mn = (std::min)({r, g, b}), d = mx - mn;
    v = mx; s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;  // grey: keep the previous hue
    h = mx == r ? 60.f * std::fmod((g - b) / d, 6.f) : mx == g ? 60.f * ((b - r) / d + 2) : 60.f * ((r - g) / d + 4);
    if (h < 0) h += 360.f;
}
D2D1_COLOR_F ColourF(unsigned rgb, float a = 1) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
void CalSetPicked(bool save) {
    if (calPicker < 0 || calPicker >= static_cast<int>(calConfig.entries.size())) return;
    auto& e = calConfig.entries[static_cast<size_t>(calPicker)];
    e.hasColour = true; e.colour = HsvToRgb(calH, calS, calV);
    if (save) SaveCalConfig();
}
// The wheel image at the brightness shown (premultiplied BGRA, 2 px per DIP, anti-aliased rim); rebuilt when V changes.
const std::vector<std::uint32_t>& CalWheelPixels(int size, float v) {
    static std::vector<std::uint32_t> px; static int builtSize = 0; static float builtV = -1;
    if (builtSize == size && std::fabs(builtV - v) < 0.002f) return px;
    builtSize = size; builtV = v; px.assign(static_cast<size_t>(size) * size, 0);
    const float r = size * 0.5f;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float dx = x + 0.5f - r, dy = y + 0.5f - r, d = std::sqrt(dx * dx + dy * dy);
        const float a = std::clamp(r - d, 0.f, 1.f);
        if (a <= 0) continue;
        const float hue = std::atan2(-dy, dx) * 57.29578f;
        const unsigned rgb = HsvToRgb(hue < 0 ? hue + 360.f : hue, (std::min)(1.f, d / r), v);
        auto ch = [&](int shift) { return static_cast<std::uint32_t>(std::lround(((rgb >> shift) & 255) * a)); };
        px[static_cast<size_t>(y) * size + x] = (static_cast<std::uint32_t>(std::lround(a * 255)) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
    }
    return px;
}
D2D1_POINT_2F CalWheelCentre(float pickerTop) { return D2D1::Point2F(36 + kCalWheelR, pickerTop + 16 + kCalWheelR); }
D2D1_RECT_F CalBrightBar(float pickerTop, float W) { return D2D1::RectF(200, pickerTop + 40, W - 36, pickerTop + 54); }
void CalPickAt(Ctl part, D2D1_POINT_2F p, float pickerTop, float W) {
    if (part == Ctl::CalWheel) {
        const auto c = CalWheelCentre(pickerTop); const float dx = p.x - c.x, dy = p.y - c.y;
        const float hue = std::atan2(-dy, dx) * 57.29578f;
        calH = hue < 0 ? hue + 360.f : hue; calS = (std::min)(1.f, std::sqrt(dx * dx + dy * dy) / kCalWheelR);
        if (calV < 0.15f) calV = 1;  // picking a hue on a black wheel: show it
    } else if (part == Ctl::CalBright) {
        const auto bar = CalBrightBar(pickerTop, W); calV = std::clamp((p.x - bar.left) / (bar.right - bar.left), 0.f, 1.f);
    }
    CalSetPicked(false);
}
float CalendarPageHeight() {
    const size_t rows = (std::max<size_t>)(1, calConfig.entries.size());
    return 62 + 22 + rows * kCalRow + (calPicker >= 0 ? kCalPickerH : 0.f) + 14 + 22 + kCalFieldRow + 6 + 20 + 10 + 28 + 18;
}

int ExtValue(const ExtWidget& e, const ExtSetting& s) {
    auto it = config.extValues.find(e.id);
    if (it != config.extValues.end()) { auto v = it->second.find(s.key); if (v != it->second.end()) return v->second; }
    return s.value;
}
float SettingsHeight() {
    switch (settingsPage) {
    case Page::Weather: return 62 + 22 + 2 * 40.f + 16;
    case Page::Batteries: return 62 + 22 + (CurrentPrefs().batteryGlass ? 4 : 1) * 40.f + 14 + 22 + 40.f + kDeviceRow * static_cast<float>(Devices().Snapshot().size()) + 16;
    case Page::Clock: return 62 + 22 + 40.f + 16;
    case Page::Calendar: return CalendarPageHeight();
    case Page::Ext: { const ExtWidget* e = SettingsExt(); return 62 + 22 + 40.f * (std::max)(1, e ? static_cast<int>(e->settings.size()) : 1) + 16; }
    default: return 62 + 22 + (CurrentPrefs().style == 1 ? 6 : 5) * 40.f + 14 + 22 + 5 * 40.f + 16;
    }
}
std::wstring SettingsTitle() {
    switch (settingsPage) {
    case Page::Weather: return L"Weather";
    case Page::Clock: return L"Clock";
    case Page::Batteries: return L"Batteries";
    case Page::Calendar: return L"Calendar";
    case Page::Ext: { const ExtWidget* e = SettingsExt(); return e ? (e->title.empty() ? e->id : e->title) : L"Widget"; }
    default: return L"Widgets & Stacks";
    }
}
void PaintSettings() {
    if (!settingsWin.Hwnd() || !IsWindowVisible(settingsWin.Hwnd())) return;
    if (settingsPage == Page::Calendar && calPickerDrag == Ctl::None) LoadCalConfig();  // before the height: one row per calendar (not mid-drag: the preview is unsaved)
    if (calPicker >= static_cast<int>(calConfig.entries.size())) calPicker = -1;
    const float W = kSetW, H = SettingsHeight();
    controls.clear();
    settingsWin.LocalGlass().active = false;
    const float kPx = DpiAt(POINT{settingsRect.left, settingsRect.top}) / 96.f;  // DIP -> window pixels (lens pointer)
    const bool motion = LabMotion();
    const ULONGLONG now = GetTickCount64();
    settingsDevices = Devices().Snapshot();
    {   // content decides the height (device list, glass rows)
        const UINT dpi = DpiAt(POINT{settingsRect.left, settingsRect.top});
        settingsRect.bottom = settingsRect.top + Px(H, dpi);
    }
    settingsWin.Render(settingsRect, HWND_TOPMOST, D2D1::RectF(0, 0, W, H), 26.f, [&](ID2D1DeviceContext* dc, bool gpu) {
        const D2D1_COLOR_F ink = gpu ? D2D1::ColorF(1, 1, 1, 0.96f) : theme.ink, dim = gpu ? D2D1::ColorF(1, 1, 1, 0.6f) : theme.secondary;
        ComPtr<ID2D1SolidColorBrush> b; dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.1f), &b);
        if (!gpu) { b->SetColor(theme.dark ? D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.97f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.98f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 26, 26), b.Get()); }
        Text(dc, SettingsTitle(), Font(17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(24, 18, W - 120, 46), ink);
        const D2D1_RECT_F done = D2D1::RectF(W - 96, 18, W - 22, 46);
        b->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.f, 1)); dc->FillRoundedRectangle(D2D1::RoundedRect(done, 14, 14), b.Get());
        Text(dc, docky::ui::Text(L"Done"), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), done, D2D1::ColorF(1, 1, 1, 1));
        controls.push_back({Ctl::Done, done});
        float y = 62;
        auto section = [&](const wchar_t* title, int rows) {
            Text(dc, title, Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(28, y, W - 24, y + 18), dim); y += 22;
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16, y, W - 16, y + rows * 40.f), 12, 12), b.Get());
        };
        auto label = [&](const std::wstring& text, int row, float top) { Text(dc, text, Font(13.f), D2D1::RectF(30, top + row * 40.f, 220, top + row * 40.f + 40), ink); };
        auto segmented = [&](Ctl id, int row, float top, const std::vector<std::wstring>& names, int selected, int indexBase = 0) {
            const float w = 210, x = W - 30 - w, ry = top + row * 40.f + 8;
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.10f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, ry, x + w, ry + 24), 12, 12), b.Get());
            const float seg = w / (std::max<size_t>)(1, names.size()); int i = 0;
            for (const auto& n : names) {
                const D2D1_RECT_F r = D2D1::RectF(x + seg * i + 2, ry + 2, x + seg * (i + 1) - 2, ry + 22);
                if (i == selected) { b->SetColor(D2D1::ColorF(1, 1, 1, 0.28f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), b.Get()); }
                Text(dc, n, Font(12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), r, ink);
                controls.push_back({id, r, indexBase + i}); ++i;
            }
        };
        // A pressed thumb becomes a lens over the window's own pixels; its white knob fades as it grows.
        auto lens = [&](D2D1_RECT_F knob, D2D1_RECT_F track, float scale, float press) {
            auto& lg = settingsWin.LocalGlass();
            lg.active = true; lg.plate = knob; lg.sampleBounds = track;
            lg.params = dock::ControlLensGlass(knob.bottom - knob.top);  // Glass Studio "Sliders & switches"
            lg.params.dark = true; lg.controlScale = 1.f; (void)scale;
            lg.pointerX = labPointer.x * kPx; lg.pointerY = labPointer.y * kPx;
            const float white = 1.f - press;
            auto pill = [knob](ID2D1DeviceContext* d, float alpha) {
                if (alpha <= .001f) return;
                ComPtr<ID2D1SolidColorBrush> wb;
                if (SUCCEEDED(d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, alpha), &wb))) { const float r = (knob.bottom - knob.top) * .5f; d->FillRoundedRectangle(D2D1::RoundedRect(knob, r, r), wb.Get()); }
            };
            lg.after = [pill, white](ID2D1DeviceContext* d) { pill(d, white); };
            lg.fallback = [pill](ID2D1DeviceContext* d) { pill(d, 1.f); };
        };
        auto slider = [&](Ctl id, int row, float top, const wchar_t* unit) {
            const auto s = Slider(id); const float x = W - 30 - 210, x2 = W - 30 - 44, cy = top + row * 40.f + 20;
            const float t = (*s.value - s.lo) / (s.hi - s.lo);
            const float press = motion ? LabPress(id, 0, now) : 0.f;
            const auto shape=dock::LabSlider(x,x2,cy,t,kLabSlider,press);
            dock::DrawLabSlider(dc,b.Get(),shape,gpu||theme.dark,gpu);
            if(press>.001f&&gpu)lens(shape.thumb,shape.track,kLabSlider,press);
            const bool percent = id == Ctl::Glass || id == Ctl::GlassRim || id == Ctl::GlassRefraction || id == Ctl::GlassFrost || id == Ctl::GlassTint;
            wchar_t v[16]; swprintf_s(v, percent ? L"%.0f%%" : L"%.0f%s", percent ? *s.value * 100 : *s.value, unit);
            Text(dc, v, Font(12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(x2 + 4, cy - 10, W - 28, cy + 10), dim);
            controls.push_back({id, D2D1::RectF(x - 10, cy - 14, x2 + 10, cy + 14)});
        };
        auto toggle = [&](Ctl id, int row, float top, bool on, int index = 0) {
            const float w = 116.f * kLabSwitch, h = 50.f * kLabSwitch, inset = 3.f * kLabSwitch;
            const float tw = 74.f * kLabSwitch, thh = 44.f * kLabSwitch;  // resting thumb
            const float right = W - 30, x = right - w, cy = top + row * 40.f + 20;
            const D2D1_RECT_F track = D2D1::RectF(x, cy - h * .5f, right, cy + h * .5f);
            // Thumb position: animated towards the value (Lab OutBack), or following a drag.
            auto& a = labSwitches[LabKey(id, index)];
            if (!a.init) { a.init = true; a.on = on; a.from = a.to = on ? 1.f : 0.f; a.start = now; }
            auto posAt = [&](ULONGLONG t) { const float u = std::clamp((t - a.start) / 420.f, 0.f, 1.f); return std::clamp(a.from + (a.to - a.from) * LabOutBack(u), 0.f, 1.f); };
            if (a.on != on) { a.from = posAt(now); a.to = on ? 1.f : 0.f; a.on = on; a.start = now; }
            float pos = motion ? posAt(now) : (on ? 1.f : 0.f);
            const bool mine = labPressed.id == id && labPressed.index == index;
            if (mine && labDown && labDragging) pos = labDragPos;
            b->SetColor(pos >= .5f ? D2D1::ColorF(0x60 / 255.f, 0xC7 / 255.f, 0x5F / 255.f, 1)
                                   : (gpu || theme.dark ? D2D1::ColorF(0x4B / 255.f, 0x4B / 255.f, 0x4D / 255.f, 1) : D2D1::ColorF(0xDA / 255.f, 0xDA / 255.f, 0xDA / 255.f, 1)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(track, h * .5f, h * .5f), b.Get());
            const float press = motion ? LabPress(id, index, now) : 0.f;
            const dock::GlassParameters lp = dock::ControlLensGlass(thh);
            const float kw = tw * (1.f + (lp.switchPressWidth - 1.f) * press), kh = thh * (1.f + (lp.switchPressHeight - 1.f) * press);
            const float kc = x + inset + tw * .5f + pos * (w - tw - 2 * inset);  // the resting thumb's centre stays put while it grows
            const D2D1_RECT_F knob = D2D1::RectF(kc - kw * .5f, cy - kh * .5f, kc + kw * .5f, cy + kh * .5f);
            if (press > .001f && gpu) lens(knob, track, kLabSwitch, press);
            else { b->SetColor(D2D1::ColorF(1, 1, 1, 1)); dc->FillRoundedRectangle(D2D1::RoundedRect(knob, kh * .5f, kh * .5f), b.Get());
                   b->SetColor(D2D1::ColorF(0, 0, 0, .12f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(knob, kh * .5f, kh * .5f), b.Get(), .7f); }
            Control c{id, D2D1::RectF(x - 6, cy - 16, right + 6, cy + 16), index, on};
            controls.push_back(c);
        };
        auto button = [&](Ctl id, D2D1_RECT_F r, const std::wstring& text, int index = 0) {
            b->SetColor(D2D1::ColorF(1, 1, 1, SettingsPressed(id, index) ? 0.30f : 0.16f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), b.Get());
            Text(dc, text, Font(12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), r, ink); controls.push_back({id, r, index});
        };
        const auto& p = CurrentPrefs();
        float top = y;
        switch (settingsPage) {
        case Page::General:
            section(L"WIDGETS", p.style == 1 ? 6 : 5); top = y;
            label(L"Style", 0, top); segmented(Ctl::Style, 0, top, {L"Solid", L"Glass"}, p.style);
            label(L"Spacing", 1, top); slider(Ctl::Gap, 1, top, L" pt");
            label(L"Snap distance", 2, top); slider(Ctl::Snap, 2, top, L" pt");
            label(L"Edge margin", 3, top); slider(Ctl::Margin, 3, top, L" pt");
            label(L"Edge highlight", 4, top); slider(Ctl::GlassRim, 4, top, L"");  // the liquid glass rim on every widget
            if (p.style == 1) { label(L"Glass tint", 5, top); slider(Ctl::Glass, 5, top, L""); }
            y = top + (p.style == 1 ? 6 : 5) * 40.f + 14;
            section(L"STACKS", 5); top = y;
            label(L"Use Stacks", 0, top); toggle(Ctl::Stacks, 0, top, config.stacks);
            label(L"Open stacks", 1, top); segmented(Ctl::Open, 1, top, {L"On Desktop", L"Glass Panel"}, config.style == StackStyle::Panel ? 1 : 0);
            label(L"Group by", 2, top); segmented(Ctl::Group, 2, top, {L"Kind", L"Date"}, config.by == GroupBy::Date ? 1 : 0);
            label(L"Hide Windows desktop icons", 3, top); toggle(Ctl::Icons, 3, top, config.hideIcons);
            label(L"Place stacks anywhere", 4, top); toggle(Ctl::FreeStacks, 4, top, config.freeStacks);
            break;
        case Page::Weather: {
            section(L"WEATHER", 2); top = y;
            label(L"Units", 0, top); segmented(Ctl::Units, 0, top, {L"°C", L"°F"}, p.fahrenheit ? 1 : 0);
            const auto wd = WeatherService().Data();
            const std::wstring where = wd.valid ? (wd.automatic ? L"Here: " : L"City: ") + wd.place : (wd.error.empty() ? L"Loading…" : wd.error);
            Text(dc, where, Font(13.f), D2D1::RectF(30, top + 40, 210, top + 80), ink);
            button(Ctl::SetCity, D2D1::RectF(W - 30 - 210, top + 48, W - 30 - 80, top + 72), L"Set City…");
            button(Ctl::RefreshWeather, D2D1::RectF(W - 30 - 72, top + 48, W - 30, top + 72), L"Refresh");
            break;
        }
        case Page::Clock:  // the digital Clock: black digits on white (macOS) or white digits on black
            section(L"CLOCK", 1); top = y;
            label(L"Appearance", 0, top); segmented(Ctl::ClockStyle, 0, top, {L"Light", L"Dark"}, p.clockDark ? 1 : 0);
            break;
        case Page::Batteries: {  // the menu bar switch, then one row per device with a Widget and a Menu Bar checkbox
            const float colWidget = W - 30 - 112, colMenu = W - 30 - 30;  // checkbox centres
            {   // LIQUID GLASS: the plate as liquid glass over the wallpaper, and how it looks
                const int rows = p.batteryGlass ? 4 : 1;
                section(L"LIQUID GLASS", rows); top = y;
                label(L"Glass background", 0, top); toggle(Ctl::BatteryGlass, 0, top, p.batteryGlass);
                if (p.batteryGlass) {
                    label(L"Refraction", 1, top); slider(Ctl::GlassRefraction, 1, top, L"");
                    label(L"Frost", 2, top); slider(Ctl::GlassFrost, 2, top, L"");
                    label(L"Tint", 3, top); slider(Ctl::GlassTint, 3, top, L"");
                }
                y = top + rows * 40.f + 14;
            }
            Text(dc, docky::ui::Text(L"DEVICES"), Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(28, y, W - 24, y + 18), dim);
            if (!settingsDevices.empty()) {
                Text(dc, docky::ui::Text(L"Widget"), Font(11.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(colWidget - 40, y, colWidget + 40, y + 18), dim);
                Text(dc, docky::ui::Text(L"Menu Bar"), Font(11.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(colMenu - 40, y, colMenu + 40, y + 18), dim);
            }
            y += 22;
            const float rowsH = 40.f + kDeviceRow * settingsDevices.size();
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16, y, W - 16, y + rowsH), 12, 12), b.Get());
            top = y;
            label(settingsDevices.empty() ? L"Show in menu bar (no devices found)" : L"Show in menu bar", 0, top); toggle(Ctl::MenuBattery, 0, top, p.menuBattery);
            auto check = [&](Ctl id, int index, float cx, float cy, bool on, bool enabled) {
                const D2D1_RECT_F box = D2D1::RectF(cx - 9, cy - 9, cx + 9, cy + 9);
                if (on) {
                    b->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.f, enabled ? 1.f : 0.4f)); dc->FillRoundedRectangle(D2D1::RoundedRect(box, 5, 5), b.Get());
                    b->SetColor(D2D1::ColorF(1, 1, 1, enabled ? 1.f : 0.6f));
                    dc->DrawLine(D2D1::Point2F(cx - 4.5f, cy + 0.5f), D2D1::Point2F(cx - 1.2f, cy + 3.8f), b.Get(), 2.f);
                    dc->DrawLine(D2D1::Point2F(cx - 1.2f, cy + 3.8f), D2D1::Point2F(cx + 4.8f, cy - 3.6f), b.Get(), 2.f);
                } else {
                    b->SetColor(D2D1::ColorF(1, 1, 1, enabled ? 0.38f : 0.16f));
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.75f, box.top + 0.75f, box.right - 0.75f, box.bottom - 0.75f), 4.5f, 4.5f), b.Get(), 1.5f);
                }
                controls.push_back({id, D2D1::RectF(cx - 18, cy - kDeviceRow / 2, cx + 18, cy + kDeviceRow / 2), index});
            };
            for (size_t i = 0; i < settingsDevices.size(); ++i) {
                const auto& d = settingsDevices[i];
                const float ry = top + 40.f + kDeviceRow * i, cy = ry + kDeviceRow / 2;
                b->SetColor(D2D1::ColorF(1, 1, 1, 0.06f)); dc->FillRectangle(D2D1::RectF(30, ry, W - 30, ry + 1), b.Get());
                Text(dc, DeviceGlyph(d.kind), IconFont(15.f), D2D1::RectF(30, cy - 10, 52, cy + 10), ink);
                const std::wstring level = d.percent < 0 ? L"" : std::to_wstring(d.percent) + L"%" + (d.charging ? L" ⚡" : L"");
                Text(dc, d.name, Font(13.f), D2D1::RectF(56, ry, colWidget - 96, ry + kDeviceRow), ink);
                Text(dc, level, Font(12.f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(colWidget - 92, ry, colWidget - 30, ry + kDeviceRow), dim);
                check(Ctl::BatteryWidget, static_cast<int>(i), colWidget, cy, !NameListed(p.batteryWidgetOff, d.name), true);
                if (d.kind != DeviceKind::PC)  // this PC's battery is the Menu Bar's own battery item
                    check(Ctl::BatteryMenu, static_cast<int>(i), colMenu, cy, !NameListed(p.batteryMenuOff, d.name), p.menuBattery);
            }
            break;
        }
        case Page::Calendar: {
            const auto feeds = Calendars().Feeds();
            const bool fetching = Calendars().Fetching();
            const D2D1_COLOR_F accent = D2D1::ColorF(0.04f, 0.52f, 1.f, 1), danger = D2D1::ColorF(1.f, 0.27f, 0.23f, 1);
            const size_t n = calConfig.entries.size();
            // CALENDARS: one row per subscription. The dot is its colour (click: a colour wheel opens under the row),
            // the title its own name (or the link's site), the subtitle what the last fetch found; the minus removes
            // it (with Undo).
            Text(dc, docky::ui::Text(L"CALENDARS"), Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(28, y, W - 24, y + 18), dim); y += 22;
            const float listH = (std::max<size_t>)(1, n) * kCalRow + (calPicker >= 0 ? kCalPickerH : 0.f);
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16, y, W - 16, y + listH), 12, 12), b.Get());
            if (!n) {
                Text(dc, docky::ui::Text(L"No calendars yet"), Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(30, y + 8, W - 30, y + 28), ink);
                Text(dc, L"Add one below. Its events appear in the Calendar widget.", Font(11.5f), D2D1::RectF(30, y + 27, W - 30, y + 45), dim);
            }
            for (size_t i = 0; i < n; ++i) {
                const auto& e = calConfig.entries[i];
                const float ry = y + kCalRow * i + (calPicker >= 0 && static_cast<int>(i) > calPicker ? kCalPickerH : 0.f), cy = ry + kCalRow / 2;
                if (i) { b->SetColor(D2D1::ColorF(1, 1, 1, 0.07f)); dc->FillRectangle(D2D1::RectF(58, ry, W - 16, ry + 1), b.Get()); }
                const unsigned rgb = calconfig::ColourOf(calConfig, i);
                const D2D1_COLOR_F col = D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1);
                const bool dotDown = SettingsPressed(Ctl::CalColour, static_cast<int>(i)), open = calPicker == static_cast<int>(i);
                b->SetColor(col); dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(38, cy), dotDown ? 6.f : 7.f, dotDown ? 6.f : 7.f), b.Get());
                b->SetColor(open ? D2D1::ColorF(accent.r, accent.g, accent.b, 1) : D2D1::ColorF(1, 1, 1, 0.22f)); dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(38, cy), 9.5f, 9.5f), b.Get(), open ? 2.f : 1.f);
                controls.push_back({Ctl::CalColour, D2D1::RectF(22, ry + 6, 54, ry + kCalRow - 6), static_cast<int>(i)});
                const auto* st = CalStatus(feeds, e.source);
                const bool link = calconfig::IsLink(e.source);
                const std::wstring title = st && !st->name.empty() ? st->name : calconfig::ShortName(e.source);
                const std::wstring site = link ? calconfig::ShortName(e.source) : L"File on this PC";
                std::wstring sub; bool bad = false;
                if (!st) sub = fetching ? L"Updating…" : L"Waiting to update";
                else if (!st->ok) { sub = link ? L"Couldn't load this calendar. Check the link." : L"Couldn't read this file."; bad = true; }
                else sub = (st->name.empty() && link ? L"" : site + L"  ·  ") + (st->events == 1 ? L"1 upcoming event" : std::to_wstring(st->events) + L" upcoming events");
                Text(dc, title, Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(58, ry + 8, W - 64, ry + 27), ink);
                Text(dc, sub, Font(11.5f), D2D1::RectF(58, ry + 27, W - 64, ry + 45), bad ? danger : dim);
                // Remove: a minus in a circle, red while pressed.
                const D2D1_POINT_2F rc = D2D1::Point2F(W - 30 - 11, cy);
                const bool rmDown = SettingsPressed(Ctl::CalRemove, static_cast<int>(i));
                b->SetColor(rmDown ? danger : D2D1::ColorF(1, 1, 1, 0.14f)); dc->FillEllipse(D2D1::Ellipse(rc, 11, 11), b.Get());
                b->SetColor(rmDown ? D2D1::ColorF(1, 1, 1, 1) : ink); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.x - 5.5f, rc.y - 1, rc.x + 5.5f, rc.y + 1), 1, 1), b.Get());
                controls.push_back({Ctl::CalRemove, D2D1::RectF(rc.x - 17, ry + 6, rc.x + 17, ry + kCalRow - 6), static_cast<int>(i)});
                if (!open) continue;
                // The colour picker under this row.
                const float pTop = ry + kCalRow; calPickerTop = pTop;
                b->SetColor(D2D1::ColorF(1, 1, 1, 0.07f)); dc->FillRectangle(D2D1::RectF(30, pTop, W - 30, pTop + 1), b.Get());
                const unsigned picked = HsvToRgb(calH, calS, calV);
                {   // the wheel: hue around, saturation from the centre out, at the bar's brightness
                    const auto c = CalWheelCentre(pTop); const int size = static_cast<int>(kCalWheelR * 4);
                    const auto& px = CalWheelPixels(size, calV);
                    ComPtr<ID2D1Bitmap> wheel;
                    if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(size, size), px.data(), size * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &wheel)))
                        dc->DrawBitmap(wheel.Get(), D2D1::RectF(c.x - kCalWheelR, c.y - kCalWheelR, c.x + kCalWheelR, c.y + kCalWheelR), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    const float rad = calH / 57.29578f, kx = c.x + std::cos(rad) * calS * kCalWheelR, ky = c.y - std::sin(rad) * calS * kCalWheelR;
                    b->SetColor(ColourF(picked)); dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 7, 7), b.Get());
                    b->SetColor(D2D1::ColorF(1, 1, 1, 1)); dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 7, 7), b.Get(), 2.5f);
                    b->SetColor(D2D1::ColorF(0, 0, 0, 0.25f)); dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ky), 8.5f, 8.5f), b.Get(), 1.f);
                    controls.push_back({Ctl::CalWheel, D2D1::RectF(c.x - kCalWheelR - 6, c.y - kCalWheelR - 6, c.x + kCalWheelR + 6, c.y + kCalWheelR + 6)});
                }
                Text(dc, docky::ui::Text(L"Brightness"), Font(11.5f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(200, pTop + 16, W - 36, pTop + 34), dim);
                {   // brightness: black to the full colour
                    const auto bar = CalBrightBar(pTop, W);
                    D2D1_GRADIENT_STOP stops[2] = {{0.f, D2D1::ColorF(0, 0, 0, 1)}, {1.f, ColourF(HsvToRgb(calH, calS, 1))}};
                    ComPtr<ID2D1GradientStopCollection> coll; ComPtr<ID2D1LinearGradientBrush> grad;
                    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &coll)) &&
                        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(bar.left, 0), D2D1::Point2F(bar.right, 0)), coll.Get(), &grad)))
                        dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 7, 7), grad.Get());
                    const float kx = bar.left + (bar.right - bar.left) * calV, kc = (bar.top + bar.bottom) / 2;
                    b->SetColor(D2D1::ColorF(1, 1, 1, 1)); dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, kc), 9, 9), b.Get());
                    b->SetColor(ColourF(picked)); dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, kc), 6, 6), b.Get());
                    controls.push_back({Ctl::CalBright, D2D1::RectF(bar.left - 10, bar.top - 10, bar.right + 10, bar.bottom + 10)});
                }
                for (int k = 0; k < 8; ++k) {  // the preset colours
                    const D2D1_POINT_2F sc = D2D1::Point2F(211 + k * 28.f, pTop + 86);
                    const bool same = calconfig::kPalette[k] == picked;
                    b->SetColor(ColourF(calconfig::kPalette[k])); dc->FillEllipse(D2D1::Ellipse(sc, 10, 10), b.Get());
                    if (same) { b->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); dc->DrawEllipse(D2D1::Ellipse(sc, 13, 13), b.Get(), 2.f); }
                    controls.push_back({Ctl::CalSwatch, D2D1::RectF(sc.x - 14, sc.y - 14, sc.x + 14, sc.y + 14), k});
                }
                {   // the result and Done
                    b->SetColor(ColourF(picked)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(200, pTop + 116, 228, pTop + 144), 8, 8), b.Get());
                    wchar_t hex[16]; swprintf_s(hex, L"#%06X", picked);
                    Text(dc, hex, Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(238, pTop + 116, 330, pTop + 144), ink);
                    const D2D1_RECT_F doneBtn = D2D1::RectF(W - 36 - 64, pTop + 116, W - 36, pTop + 144);
                    const bool down = SettingsPressed(Ctl::CalPickerDone);
                    b->SetColor(D2D1::ColorF(accent.r * (down ? .8f : 1.f), accent.g * (down ? .8f : 1.f), accent.b * (down ? .85f : 1.f), 1));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(doneBtn, 8, 8), b.Get());
                    Text(dc, docky::ui::Text(L"Done"), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), doneBtn, D2D1::ColorF(1, 1, 1, 1));
                    controls.push_back({Ctl::CalPickerDone, doneBtn});
                }
            }
            y += listH + 14;
            // ADD CALENDAR: a field for a pasted link (Enter adds it) and Add; the line under it is the hint, or what
            // went wrong, or Undo after a removal.
            Text(dc, docky::ui::Text(L"ADD CALENDAR"), Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(28, y, W - 24, y + 18), dim); y += 22;
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16, y, W - 16, y + kCalFieldRow), 12, 12), b.Get());
            const D2D1_RECT_F field = D2D1::RectF(26, y + 8, W - 30 - 64, y + kCalFieldRow - 8), add = D2D1::RectF(W - 30 - 56, y + 8, W - 26, y + kCalFieldRow - 8);
            b->SetColor(D2D1::ColorF(1, 1, 1, calFocus ? 0.14f : 0.09f)); dc->FillRoundedRectangle(D2D1::RoundedRect(field, 8, 8), b.Get());
            if (calFocus) { b->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.9f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(field.left - 1, field.top - 1, field.right + 1, field.bottom + 1), 9, 9), b.Get(), 2.f); }
            controls.push_back({Ctl::CalField, field});
            {
                IDWriteTextFormat* f = Font(12.5f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_LEADING, false);
                const float tx = field.left + 10, room = field.right - 10 - tx;
                std::wstring shown = calField;
                if (!shown.empty() && TextWidth(shown, f) > room) {  // the end of a long link stays in view
                    size_t lo = 0, hi = shown.size();
                    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (TextWidth(L"…" + calField.substr(mid), f) > room) lo = mid + 1; else hi = mid; }
                    shown = L"…" + calField.substr(lo);
                }
                if (shown.empty()) Text(dc, L"Paste an iCal link (https:// or webcal://)", Font(12.5f), D2D1::RectF(tx, field.top, field.right - 8, field.bottom), dim);
                else Text(dc, shown, f, D2D1::RectF(tx, field.top, field.right - 4, field.bottom), ink);
                if (calFocus && calCaretOn) {
                    const float cx = tx + (shown.empty() ? 0.f : TextWidth(shown, f)) + 1;
                    b->SetColor(accent); dc->FillRectangle(D2D1::RectF(cx, field.top + 6, cx + 1.5f, field.bottom - 6), b.Get());
                }
            }
            const bool canAdd = !calconfig::Trim(calField).empty(), addDown = SettingsPressed(Ctl::CalAdd);
            b->SetColor(canAdd ? D2D1::ColorF(accent.r * (addDown ? .8f : 1.f), accent.g * (addDown ? .8f : 1.f), accent.b * (addDown ? .85f : 1.f), 1) : D2D1::ColorF(1, 1, 1, addDown ? 0.22f : 0.12f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(add, 8, 8), b.Get());
            Text(dc, docky::ui::Text(L"Add"), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), add, canAdd ? D2D1::ColorF(1, 1, 1, 1) : dim);
            controls.push_back({Ctl::CalAdd, add});
            y += kCalFieldRow + 6;
            if (calUndo.active) {
                Text(dc, L"Removed “" + calUndo.name + L"”.", Font(11.5f), D2D1::RectF(28, y, W - 100, y + 20), dim);
                const D2D1_RECT_F undo = D2D1::RectF(W - 30 - 56, y, W - 26, y + 20);
                Text(dc, docky::ui::Text(L"Undo"), Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), undo, SettingsPressed(Ctl::CalUndo) ? dim : accent);
                controls.push_back({Ctl::CalUndo, D2D1::RectF(undo.left - 8, undo.top - 4, undo.right + 4, undo.bottom + 4)});
            } else if (!calError.empty()) Text(dc, calError, Font(11.5f), D2D1::RectF(28, y, W - 24, y + 20), danger);
            else Text(dc, L"Outlook, Google, iCloud and Canvas each offer a private iCal link.", Font(11.5f), D2D1::RectF(28, y, W - 24, y + 20), dim);
            y += 20 + 10;
            // Footer: a local .ics file, what the widget shows, and Refresh.
            button(Ctl::CalFile, D2D1::RectF(16, y, 16 + 128, y + 28), L"Choose .ics File…");
            const size_t upcoming = Calendars().Events().size();
            const std::wstring status = fetching ? L"Updating…" : n ? (upcoming == 1 ? L"1 upcoming" : std::to_wstring(upcoming) + L" upcoming") : L"";
            Text(dc, status, Font(11.5f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(150, y, W - 16 - 88, y + 28), dim);
            button(Ctl::RefreshCalendars, D2D1::RectF(W - 16 - 80, y, W - 16, y + 28), L"Refresh");
            break;
        }
        case Page::Ext: {
            const ExtWidget* e = SettingsExt();
            const int rows = e ? static_cast<int>(e->settings.size()) : 0;
            section(L"SETTINGS", (std::max)(1, rows)); top = y;
            if (!rows) { label(e ? L"This widget has no settings." : L"This widget is not running.", 0, top); break; }
            for (int i = 0; i < rows; ++i) {
                const ExtSetting& s = e->settings[static_cast<size_t>(i)];
                const int v = ExtValue(*e, s);
                label(s.title, i, top);
                if (s.type == "toggle") toggle(Ctl::ExtSetting, i, top, v != 0, i * 64);
                else if (s.type == "choice") segmented(Ctl::ExtSetting, i, top, s.options, v, i * 64);
                else button(Ctl::ExtSetting, D2D1::RectF(W - 30 - 130, top + i * 40.f + 8, W - 30, top + i * 40.f + 32), s.options.empty() ? std::wstring(L"Open") : s.options.front(), i * 64);
            }
            break;
        }
        }
    });
}
void RefreshSettings() { PaintSettings(); }
bool SettingsVisible() { return settingsWin.Hwnd() && IsWindowVisible(settingsWin.Hwnd()); }
void SetSlider(Ctl id, float x) {
    const auto s = Slider(id); const float x0 = kSetW - 30 - 210, x1 = kSetW - 30 - 44;
    const float oldGap = CurrentPrefs().gap, oldMargin = CurrentPrefs().margin;
    float v = s.lo + (s.hi - s.lo) * std::clamp((x - x0) / (x1 - x0), 0.f, 1.f);
    if (id == Ctl::Gap || id == Ctl::Snap || id == Ctl::Margin) v = std::round(v);
    if (v == *s.value) return;
    *s.value = v;
    if (id == Ctl::Gap || id == Ctl::Margin) Repack(oldGap, oldMargin);
    if (id == Ctl::Glass || id == Ctl::GlassRim) RepaintAll();
    if (id == Ctl::GlassRefraction || id == Ctl::GlassFrost || id == Ctl::GlassTint) for (auto& wdg : config.widgets) if (wdg.kind == L"battery") PaintWidgetWin(wdg);
    PaintSettings();
}
// A third-party setting changed: tell its app (widget.setting) or re-run its manifest command with the new values.
void RunDataCommand(const ExtWidget& e);
void SendExtSetting(const ExtWidget& e, const ExtSetting& s, int value) {
    if (e.fromManifest) { RunDataCommand(e); return; }
    if (!e.notify || !IsWindow(e.notify)) return;
    const std::string v = s.type == "toggle" ? (value ? "true" : "false") : s.type == "choice" ? std::to_string(value) : "null";
    docky::ext::SendJson(e.notify, host, "{\"v\":1,\"type\":\"widget.setting\",\"id\":" + dock::json::Write(Value::MakeString(U(e.id)), 0) +
                                          ",\"key\":" + dock::json::Write(Value::MakeString(s.key), 0) + ",\"value\":" + v + "}", 300);
}
bool IsLabSwitch(const Control& c) {
    switch (c.id) {
    case Ctl::Stacks: case Ctl::Icons: case Ctl::FreeStacks: case Ctl::MenuBattery: case Ctl::BatteryGlass: case Ctl::BatteryWidget: case Ctl::BatteryMenu: return true;
    case Ctl::ExtSetting: { const ExtWidget* e = SettingsExt(); const int row = c.index / 64;
        return e && row >= 0 && row < static_cast<int>(e->settings.size()) && e->settings[static_cast<size_t>(row)].type == "toggle"; }
    default: return false;
    }
}
void ActivateControl(const Control& c);
void SettingsClick(D2D1_POINT_2F p, bool down) {
    if (down && calFocus) {  // a press anywhere but the field ends typing (the field itself keeps it)
        bool onField = false; for (const auto& c : controls) if (c.id == Ctl::CalField && In(c.rect, p)) onField = true;
        if (!onField) { CalFocus(false); PaintSettings(); }
    }
    for (const auto& c : controls) {
        if (!In(c.rect, p)) continue;
        if (c.id == Ctl::Gap || c.id == Ctl::Snap || c.id == Ctl::Margin || c.id == Ctl::Glass || c.id == Ctl::GlassRim || c.id == Ctl::GlassRefraction || c.id == Ctl::GlassFrost || c.id == Ctl::GlassTint) {
            if (down) { draggingSlider = c.id; labPointer = p; LabBeginPress(Control{c.id, c.rect, 0}, true); SetCapture(settingsWin.Hwnd()); SetSlider(c.id, p.x); }
            return;
        }
        if (IsLabSwitch(c)) {  // pressed: the thumb grows into a lens; released (or dragged across) on mouse-up
            if (down) { labPointer = p; labStartX = p.x; labStartPos = c.on ? 1.f : 0.f; labDragPos = labStartPos; labDragging = false;
                        LabBeginPress(c, true); SetCapture(settingsWin.Hwnd()); PaintSettings(); }
            return;
        }
        if (c.id == Ctl::CalField) { if (down) { CalFocus(true); SetForegroundWindow(settingsWin.Hwnd()); PaintSettings(); } return; }
        if (c.id == Ctl::CalWheel || c.id == Ctl::CalBright) {  // drag the colour; saved on release
            if (down) { calPickerDrag = c.id; SetCapture(settingsWin.Hwnd()); CalPickAt(c.id, p, calPickerTop, kSetW); PaintSettings(); }
            return;
        }
        // Buttons look pressed the moment they are pressed and act on release over them (released elsewhere: nothing).
        if (down) { settingsPressed = c; settingsPressedOut = false; SetCapture(settingsWin.Hwnd()); PaintSettings(); }
        return;
    }
}
void SettingsRelease(D2D1_POINT_2F p) {
    const Control c = settingsPressed;
    settingsPressed = Control{Ctl::None, {}};
    if (GetCapture() == settingsWin.Hwnd()) ReleaseCapture();
    if (In(c.rect, p)) ActivateControl(c); else PaintSettings();
}
// Mouse-up on a pressed Lab switch: a click toggles it; a drag sets the side the thumb was dropped on.
void LabSwitchRelease(D2D1_POINT_2F p) {
    const Control c = labPressed;
    const bool dragged = labDragging; const float dropped = labDragPos;
    labDown = false; labDragging = false;  // before ReleaseCapture: its WM_CAPTURECHANGED must not cancel this release
    if (GetCapture() == settingsWin.Hwnd()) ReleaseCapture();
    const bool target = dragged ? dropped >= .5f : !c.on;
    if (dragged) {  // the thumb continues from where it was dropped
        auto& a = labSwitches[LabKey(c.id, c.index)];
        a.from = dropped; a.to = target ? 1.f : 0.f; a.on = target; a.start = GetTickCount64();
    }
    labPointer = p;
    LabBeginPress(c, false);
    if (target != c.on) ActivateControl(c); else PaintSettings();
}
void ActivateControl(const Control& c) {
    {
        switch (c.id) {
        case Ctl::Done: settingsWin.Hide(); SaveConfig(); return;
        case Ctl::Style: CurrentPrefs().style = c.index; RepaintAll(); break;
        case Ctl::Stacks: config.stacks = !config.stacks; ApplyStacks(); break;
        case Ctl::Open: config.style = c.index ? StackStyle::Panel : StackStyle::InPlace; GetStacks().SetStyle(config.style); break;
        case Ctl::Group: config.by = c.index ? GroupBy::Date : GroupBy::Kind; GetStacks().SetGroupBy(config.by); break;
        case Ctl::Icons: config.hideIcons = !config.hideIcons; HideDesktopIcons(config.stacks && config.hideIcons); break;
        case Ctl::FreeStacks: config.freeStacks = !config.freeStacks; GetStacks().SetFreePlacement(config.freeStacks); break;
        case Ctl::ClockStyle: CurrentPrefs().clockDark = c.index == 1; for (auto& wdg : config.widgets) if (wdg.kind == L"clock") PaintWidgetWin(wdg); break;
        case Ctl::BatteryGlass: CurrentPrefs().batteryGlass = !CurrentPrefs().batteryGlass; for (auto& wdg : config.widgets) if (wdg.kind == L"battery") PaintWidgetWin(wdg); break;
        case Ctl::Units: CurrentPrefs().fahrenheit = c.index == 1; WeatherService().SetFahrenheit(CurrentPrefs().fahrenheit); break;
        case Ctl::SetCity: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + Weather::CityPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return;
        case Ctl::RefreshWeather: WeatherService().Refresh(); return;
        case Ctl::MenuBattery: CurrentPrefs().menuBattery = !CurrentPrefs().menuBattery; UpdateBatteryItem(); break;
        case Ctl::BatteryWidget: case Ctl::BatteryMenu: {
            if (c.index < 0 || c.index >= static_cast<int>(settingsDevices.size())) return;
            auto& list = c.id == Ctl::BatteryWidget ? CurrentPrefs().batteryWidgetOff : CurrentPrefs().batteryMenuOff;
            const std::wstring& name = settingsDevices[static_cast<size_t>(c.index)].name;
            auto it = std::find(list.begin(), list.end(), name);
            if (it != list.end()) list.erase(it); else list.push_back(name);
            if (c.id == Ctl::BatteryWidget) { for (auto& wdg : config.widgets) if (wdg.kind == L"battery") PaintWidgetWin(wdg); }
            else UpdateBatteryItem();
            break;
        }
        case Ctl::EditCalendars: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + CalendarFeeds::ConfigPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return;
        case Ctl::RefreshCalendars: Calendars().Refresh(); PaintSettings(); return;
        case Ctl::CalAdd: CalAddText(); PaintSettings(); return;
        case Ctl::CalFile: CalChooseFile(); PaintSettings(); return;
        case Ctl::CalColour:
            if (c.index < 0 || c.index >= static_cast<int>(calConfig.entries.size())) return;
            if (calPicker == c.index) calPicker = -1;
            else { calPicker = c.index; RgbToHsv(calconfig::ColourOf(calConfig, static_cast<size_t>(c.index)), calH, calS, calV); }
            PaintSettings(); return;
        case Ctl::CalSwatch:
            if (c.index < 0 || c.index >= 8) return;
            RgbToHsv(calconfig::kPalette[c.index], calH, calS, calV); CalSetPicked(true); PaintSettings(); return;
        case Ctl::CalPickerDone: calPicker = -1; PaintSettings(); return;
        case Ctl::CalRemove: {
            if (c.index < 0 || c.index >= static_cast<int>(calConfig.entries.size())) return;
            const size_t i = static_cast<size_t>(c.index);
            const CalUndo undo{true, i, calConfig.entries[i], CalName(Calendars().Feeds(), calConfig.entries[i])};
            calconfig::Remove(calConfig, i); calPicker = -1;
            if (SaveCalConfig()) { calUndo = undo; calError.clear(); } else { calError = L"Couldn't save calendars.txt."; }
            PaintSettings(); return;
        }
        case Ctl::CalUndo:
            if (calUndo.active && calconfig::Find(calConfig, calUndo.entry.source) < 0) {
                const size_t at = (std::min)(calUndo.index, calConfig.entries.size());
                calConfig.entries.insert(calConfig.entries.begin() + static_cast<std::ptrdiff_t>(at), calUndo.entry);
                SaveCalConfig();
            }
            calUndo.active = false; PaintSettings(); return;
        case Ctl::ExtSetting: {
            ExtWidget* e = SettingsExt();
            const int row = c.index / 64, option = c.index % 64;
            if (!e || row < 0 || row >= static_cast<int>(e->settings.size())) return;
            const ExtSetting& s = e->settings[static_cast<size_t>(row)];
            int value = s.type == "toggle" ? !ExtValue(*e, s) : s.type == "choice" ? option : 0;
            if (s.type != "button") config.extValues[e->id][s.key] = value;
            SendExtSetting(*e, s, value);
            break;
        }
        default: return;
        }
        SaveConfig(); PaintSettings(); RefreshGallery(); return;
    }
}
LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == docky::vblank::kMsgVBlank) { settingsTicker.Ack(); m = WM_TIMER; w = 1; }  // the per-frame tick
    const UINT dpi = DpiAt(POINT{settingsRect.left, settingsRect.top}); const float k = dpi / 96.f;
    const D2D1_POINT_2F p{GET_X_LPARAM(l) / k, GET_Y_LPARAM(l) / k};
    switch (m) {
    case WM_LBUTTONDOWN: SettingsClick(p, true); return 0;
    case WM_MOUSEMOVE:
        if (settingsPressed.id != Ctl::None && (w & MK_LBUTTON)) {
            const bool out = !In(settingsPressed.rect, p);
            if (out != settingsPressedOut) { settingsPressedOut = out; PaintSettings(); }
        }
        else if (draggingSlider != Ctl::None && (w & MK_LBUTTON)) { labPointer = p; SetSlider(draggingSlider, p.x); }
        else if (calPickerDrag != Ctl::None && (w & MK_LBUTTON)) { CalPickAt(calPickerDrag, p, calPickerTop, kSetW); PaintSettings(); }
        else if (labDown && IsLabSwitch(labPressed) && (w & MK_LBUTTON)) {
            labPointer = p;
            const float travel = (116.f - 74.f - 6.f) * kLabSwitch;
            if (!labDragging && std::fabs(p.x - labStartX) >= 4.f) labDragging = true;
            if (labDragging) labDragPos = std::clamp(labStartPos + (p.x - labStartX) / travel, 0.f, 1.f);
            PaintSettings();
        }
        return 0;
    case WM_LBUTTONUP:
        if (calPickerDrag != Ctl::None) { const Ctl part = calPickerDrag; CalPickAt(part, p, calPickerTop, kSetW); calPickerDrag = Ctl::None; ReleaseCapture(); CalSetPicked(true); PaintSettings(); return 0; }
        if (draggingSlider != Ctl::None) { const Ctl id = draggingSlider; draggingSlider = Ctl::None; ReleaseCapture(); LabBeginPress(Control{id, {}, 0}, false); SaveConfig(); PaintSettings(); return 0; }
        if (labDown && IsLabSwitch(labPressed)) { LabSwitchRelease(p); return 0; }
        if (settingsPressed.id != Ctl::None) { SettingsRelease(p); return 0; }
        SettingsClick(p, false); return 0;
    case WM_CHAR:  // the Add Calendar field
        if (!calFocus || settingsPage != Page::Calendar) return 0;
        if (w == 0x16) CalPaste();                                   // Ctrl+V
        else if (w == L'\r') CalAddText();                            // Enter adds
        else if (w == 0x08) { if (!calField.empty()) calField.pop_back(); calError.clear(); }
        else if (w == 0x7F) { calField.clear(); calError.clear(); }  // Ctrl+Backspace clears
        else if (w >= 32 && calField.size() < 2048) { calField += static_cast<wchar_t>(w); calError.clear(); }
        else return 0;
        calCaretStart = GetTickCount64(); calCaretOn = true; PaintSettings(); return 0;
    case WM_CAPTURECHANGED:
        if (calPickerDrag != Ctl::None && reinterpret_cast<HWND>(l) != h) { calPickerDrag = Ctl::None; CalSetPicked(true); PaintSettings(); return 0; }
        if (settingsPressed.id != Ctl::None && reinterpret_cast<HWND>(l) != h) { settingsPressed = Control{Ctl::None, {}}; PaintSettings(); return 0; }
        if (labDown && IsLabSwitch(labPressed) && reinterpret_cast<HWND>(l) != h) { labDragging = false; LabBeginPress(labPressed, false); PaintSettings(); }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && calFocus && settingsPage == Page::Calendar) {  // first Esc leaves the field, the next closes
            if (!calField.empty()) calField.clear(); else CalFocus(false);
            calError.clear(); PaintSettings();
        }
        else if (w == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000) && calFocus && settingsPage == Page::Calendar) { CalPaste(); PaintSettings(); }
        else if (w == VK_ESCAPE) { settingsWin.Hide(); SaveConfig(); }
        else if (w == VK_F9 && !DevPngDir().empty()) { settingsWin.RequestSnapshot(DevPngDir() + L"\\settings-pressed.bmp"); PaintSettings(); }  // development
        (void)h; return 0;
    case WM_TIMER:  // every frame: what is behind the panel shows through; only the glass is redrawn (content cache)
        if (!settingsWin.Visible()) { KillTimer(h, 1); settingsTicker.Stop(); return 0; }
        if (LabAnimating()) { PaintSettings(); return 0; }  // a Glass Lab thumb is moving or changing size
        if (calFocus && settingsPage == Page::Calendar) {  // the field's caret blinks (on 530 ms, off 530 ms)
            const bool on = ((GetTickCount64() - calCaretStart) / 530) % 2 == 0;
            if (on != calCaretOn) { calCaretOn = on; PaintSettings(); return 0; }
        }
        if (labPressed.id != Ctl::None && !labDown) { labPressed = Control{Ctl::None, {}}; PaintSettings(); return 0; }  // release finished
        if (draggingSlider == Ctl::None && settingsWin.GlassChanged()) {
            if (!DevPngDir().empty()) settingsWin.RequestSnapshot(DevPngDir() + L"\\settings-retuned.bmp");
            PaintSettings();
        }
        else if (settingsWin.RefreshLiveBackdrop()) settingsWin.Redraw();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
// Opens a settings page. With `anchor` (the widget it belongs to) the panel sits beside that widget, like the
// macOS widget editor; the general page is centred on the primary screen.
void OpenSettingsPage(Page page, const std::wstring& extId, HWND anchor) {
    if (!settingsWin.Hwnd() && !settingsWin.Create(L"DockyDesktopSettings", SettingsProc, WS_EX_TOPMOST, false)) return;
    settingsWin.SetFrosted(true); settingsWin.SetLiveBackdrop(true);
    settingsPage = page; settingsExtId = extId; settingsAnchor = anchor;
    settingsPressed = Control{Ctl::None, {}}; calField.clear(); calError.clear(); calUndo.active = false;
    if (page == Page::Calendar) { LoadCalConfig(); CalFocus(calConfig.entries.empty()); } else CalFocus(false);  // empty: ready to paste
    RECT ar{};
    const bool anchored = anchor && IsWindow(anchor) && GetWindowRect(anchor, &ar);
    const POINT ref = anchored ? POINT{(ar.left + ar.right) / 2, (ar.top + ar.bottom) / 2} : POINT{0, 0};
    const RECT work = anchored ? WorkAt(ref) : PrimaryWork(); const UINT dpi = DpiAt(anchored ? ref : POINT{work.left, work.top});
    const int w = Px(kSetW, dpi), hgt = Px(SettingsHeight(), dpi), gap = Px(12, dpi);
    if (anchored) {
        const LONG shadow = Px(kShadow, dpi);  // widget windows carry a shadow margin around the visible plate
        ar.left += shadow; ar.right -= shadow; ar.top += shadow;
        LONG x = ar.right + gap;
        if (x + w > work.right - gap) x = ar.left - gap - w;
        x = std::clamp(x, work.left + gap, (std::max)(work.left + gap, work.right - gap - w));
        const LONG y = std::clamp(ar.top, work.top + gap, (std::max)(work.top + gap, work.bottom - gap - hgt));
        settingsRect = RECT{x, y, x + w, y + hgt};
    } else {
        settingsRect = RECT{(work.left + work.right - w) / 2, (work.top + work.bottom - hgt) / 2 - Px(60, dpi), (work.left + work.right + w) / 2, (work.top + work.bottom + hgt) / 2 - Px(60, dpi)};
        settingsRect.top = (std::max)(settingsRect.top, work.top + Px(8, dpi)); settingsRect.bottom = settingsRect.top + hgt;
    }
    settingsWin.InvalidateBackdrop();
    settingsWin.Show();
    ShowWindow(settingsWin.Hwnd(), SW_SHOW);
    PaintSettings();
    settingsTicker.Start(settingsWin.Hwnd(), docky::vblank::kMsgVBlank);
    SetForegroundWindow(settingsWin.Hwnd());
    if (!devPng.empty()) { settingsWin.RequestSnapshot(devPng + L"\\settings.bmp"); PaintSettings(); }
}
void OpenSettings() { OpenSettingsPage(Page::General, L"", nullptr); }
// The page a widget's "Edit" item opens; false when the widget has no settings of its own.
bool WidgetPage(const WidgetWin& w, Page& page, std::wstring& extId) {
    if (w.kind == L"weather") { page = Page::Weather; return true; }
    if (w.kind == L"clock") { page = Page::Clock; return true; }
    if (w.kind == L"battery") { page = Page::Batteries; return true; }
    if (w.kind == L"calendar") { page = Page::Calendar; return true; }
    if (w.kind.rfind(L"ext:", 0) == 0) {
        extId = w.kind.substr(4);
        for (const auto& e : extWidgets) if (e.id == extId && !e.settings.empty()) { page = Page::Ext; return true; }
    }
    return false;
}
bool WidgetHasPage(const WidgetWin& w) { Page page; std::wstring id; return WidgetPage(w, page, id); }
void EditWidget(HWND widget) {
    WidgetWin* w = WidgetOf(widget); if (!w) return;
    Page page = Page::General; std::wstring extId;
    if (WidgetPage(*w, page, extId)) OpenSettingsPage(page, extId, widget);
}

// ---- Third-party widgets ------------------------------------------------------------------------------------------
void RefreshExtWindows(const std::wstring& id) { for (auto& w : config.widgets) if (w.kind == L"ext:" + id) PaintWidgetWin(w); RefreshGallery(); }
void Offer(const ExtWidget& e) {  // first registration of a widget: place it once so its developer sees it at once
    if (std::find(config.seenExt.begin(), config.seenExt.end(), e.id) != config.seenExt.end()) return;
    config.seenExt.push_back(e.id);
    bool placed = false; for (const auto& w : config.widgets) if (w.kind == L"ext:" + e.id) placed = true;
    if (!placed) AddWidget(L"ext:" + e.id, e.size); else SaveConfig();
}
struct DataResult { std::wstring id; std::string output; };
void RunDataCommand(const ExtWidget& e) {
    if (e.command.empty()) return;
    const std::wstring id = e.id, command = e.command, folder = e.folder;
    std::wstring env;  // this process's environment plus DOCKY_WIDGET_SETTINGS={"key":value,...}
    {
        std::string values = "{";
        for (const auto& st : e.settings) {
            if (st.type == "button") continue;
            if (values.size() > 1) values += ",";
            const int v = ExtValue(e, st);
            values += dock::json::Write(Value::MakeString(st.key), 0) + ":" + (st.type == "toggle" ? (v ? "true" : "false") : std::to_string(v));
        }
        values += "}";
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* q = block; *q; q += wcslen(q) + 1) if (_wcsnicmp(q, L"DOCKY_WIDGET_SETTINGS=", 22) != 0) { env += q; env.push_back(L'\0'); }
            FreeEnvironmentStringsW(block);
        }
        env += L"DOCKY_WIDGET_SETTINGS=" + W(values); env.push_back(L'\0'); env.push_back(L'\0');
    }
    std::thread([id, command, folder, env] {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
        PROCESS_INFORMATION pi{}; std::wstring cmd = command;
        std::string output;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, const_cast<wchar_t*>(env.data()), folder.c_str(), &si, &pi)) {
            CloseHandle(wr); wr = nullptr;
            if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);  // our own child only
            char buffer[4096]; DWORD n = 0;
            while (output.size() < 256 * 1024 && ReadFile(rd, buffer, sizeof(buffer), &n, nullptr) && n) output.append(buffer, n);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        if (wr) CloseHandle(wr);
        CloseHandle(rd);
        auto* result = new DataResult{id, std::move(output)};
        if (!host || !PostMessageW(host, kDataReady, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}
void ReloadManifests() {
    auto next = LoadWidgetManifests(docky::ext::ExtensionsRoot());
    std::vector<std::wstring> ids; for (const auto& n : next) ids.push_back(n.id);  // `next` is moved from below
    for (auto& n : next) {
        auto it = std::find_if(extWidgets.begin(), extWidgets.end(), [&](const ExtWidget& e) { return e.id == n.id; });
        if (it != extWidgets.end()) { if (!it->fromManifest) continue; n.data = it->data; n.lastRun = it->lastRun; n.image = it->image; n.imageW = it->imageW; n.imageH = it->imageH; n.imagePath = it->imagePath; *it = std::move(n); }
        else { extWidgets.push_back(std::move(n)); Offer(extWidgets.back()); }
    }
    // A manifest that disappeared takes its widgets' content with it (placed widgets then show "waiting").
    extWidgets.erase(std::remove_if(extWidgets.begin(), extWidgets.end(), [&](const ExtWidget& e) {
        return e.fromManifest && std::find(ids.begin(), ids.end(), e.id) == ids.end(); }), extWidgets.end());
}
void TickDataCommands() {
    const ULONGLONG now = GetTickCount64();
    for (auto& e : extWidgets)
        if (e.fromManifest && !e.command.empty() && (!e.lastRun || (e.refreshSeconds && now - e.lastRun >= static_cast<ULONGLONG>(e.refreshSeconds) * 1000))) { e.lastRun = now; RunDataCommand(e); }
}

// ---- Accessory batteries in the Menu Bar (extension protocol; docky's Menu Bar shows them when it runs) ------------
// One status item per accessory, as macOS shows a Bluetooth device: its template glyph, its level as the title, and a
// menu with the details and the settings. Settings > Batteries chooses which ones appear. Re-sent when the levels
// change and when the Menu Bar (re)starts.
constexpr wchar_t kBatteryItemId[] = L"com.docky.desktop.batteries";  // v0.1 single item: removed on start
constexpr char kDeviceItemPrefix[] = "com.docky.desktop.battery.";
std::wstring BatteryIconPath() { return DataDir() + L"\\menubar-battery.png"; }
std::wstring DeviceIconPath(DeviceKind kind) { return DataDir() + L"\\menubar-device-" + std::to_wstring(static_cast<int>(kind)) + L".png"; }
std::string DeviceItemId(const std::wstring& name) {  // protocol ids allow A-Z a-z 0-9 . - _
    std::string id = kDeviceItemPrefix;
    for (wchar_t c : name) {
        if (id.size() > 120) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) id.push_back(static_cast<char>(c));
        else if (c >= L'A' && c <= L'Z') id.push_back(static_cast<char>(c - L'A' + L'a'));
        else if (c == L' ' || c == L'-' || c == L'_' || c == L'.') id.push_back('-');
        else { char hex[8]; sprintf_s(hex, "_%04x", static_cast<unsigned>(c)); id += hex; }
    }
    return id;
}
bool SavePng(IWICBitmap* bmp, const std::wstring& path) {
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    CreateDirectoryW(DataDir().c_str(), nullptr);
    DeleteFileW(path.c_str());
    if (FAILED(Wic()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    return SUCCEEDED(frame->WriteSource(bmp, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}
// The device glyph (Segoe Fluent Icons) as a black template image; the Menu Bar re-inks it.
bool RenderDeviceIcon(DeviceKind kind) {
    if (GetFileAttributesW(DeviceIconPath(kind).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    ComPtr<IWICBitmap> bmp; ComPtr<ID2D1RenderTarget> rt;
    if (!Wic() || FAILED(Wic()->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;
    ComPtr<ID2D1SolidColorBrush> b; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
    const std::wstring glyph = DeviceGlyph(kind);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(DWrite()->CreateTextLayout(glyph.c_str(), static_cast<UINT32>(glyph.size()), IconFont(46.f), 64, 64, &layout))) return false;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->BeginDraw(); rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), b.Get());
    if (FAILED(rt->EndDraw())) return false;
    return SavePng(bmp.Get(), DeviceIconPath(kind));
}
bool RenderBatteryIcon(int level) {
    ComPtr<IWICBitmap> bmp; ComPtr<ID2D1RenderTarget> rt;
    if (!Wic() || FAILED(Wic()->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;
    ComPtr<ID2D1SolidColorBrush> b; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
    rt->BeginDraw(); rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_RECT_F body = D2D1::RectF(4, 18, 54, 46);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 7, 7), b.Get(), 4.f);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(56, 26, 61, 38), 2, 2), b.Get());
    const float fill = std::clamp(level, 4, 100) / 100.f;
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 24, 10 + 38 * fill, 40), 3, 3), b.Get());
    if (FAILED(rt->EndDraw())) return false;
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    CreateDirectoryW(DataDir().c_str(), nullptr);
    DeleteFileW(BatteryIconPath().c_str());
    if (FAILED(Wic()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(BatteryIconPath().c_str(), GENERIC_WRITE))) return false;
    if (FAILED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    return SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}
std::vector<std::string> batteryItems;  // ids of the device items the Menu Bar shows now
bool batteryItemShown = false, legacyBatteryRemoved = false;
void UpdateBatteryItem() {
    HWND bar = docky::ext::FindMenuBarHost();
    if (!bar) { batteryItems.clear(); batteryItemShown = false; legacyBatteryRemoved = false; return; }
    auto remove = [&](const std::string& id) { docky::ext::SendJson(bar, host, "{\"v\":1,\"type\":\"statusItem.remove\",\"id\":\"" + id + "\"}"); };
    if (!legacyBatteryRemoved) { remove(U(kBatteryItemId)); legacyBatteryRemoved = true; }  // v0.1's single combined item
    std::vector<DeviceBattery> shown;
    if (CurrentPrefs().menuBattery)
        for (const auto& d : Devices().Snapshot())
            if (d.kind != DeviceKind::PC && d.percent >= 0 && !NameListed(CurrentPrefs().batteryMenuOff, d.name)) shown.push_back(d);
    std::vector<std::string> ids;
    auto str = [](const std::wstring& w) { return dock::json::Write(Value::MakeString(U(w)), 0); };
    for (const auto& d : shown) {
        const std::string id = DeviceItemId(d.name);
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        RenderDeviceIcon(d.kind);
        const std::wstring level = std::to_wstring(d.percent) + L"%";
        std::string j = "{\"v\":1,\"type\":\"statusItem.set\",\"id\":\"" + id + "\",\"title\":" + str(level) +
                        ",\"icon\":" + str(DeviceIconPath(d.kind)) + ",\"template\":true,\"tooltip\":" + str(d.name + L" \u2013 " + level) +
                        ",\"notifyHwnd\":" + std::to_string(reinterpret_cast<ULONG_PTR>(host)) + ",\"menu\":[" +
                        "{\"id\":\"name\",\"title\":" + str(d.name) + ",\"enabled\":false}," +
                        "{\"id\":\"level\",\"title\":" + str(L"Battery Level: " + level + (d.charging ? L" (Charging)" : L"")) + ",\"enabled\":false}," +
                        "{\"separator\":true},{\"id\":\"settings\",\"title\":\"Battery Settings\u2026\"},{\"id\":\"bt\",\"title\":\"Bluetooth & Devices Settings\u2026\"}]}";
        if (docky::ext::SendJson(bar, host, j)) ids.push_back(id);
    }
    for (const auto& old : batteryItems) if (std::find(ids.begin(), ids.end(), old) == ids.end()) remove(old);
    batteryItems = ids;
    batteryItemShown = !ids.empty();
}
void RemoveBatteryItems() {
    if (HWND bar = docky::ext::FindMenuBarHost())
        for (const auto& id : batteryItems) docky::ext::SendJson(bar, host, "{\"v\":1,\"type\":\"statusItem.remove\",\"id\":\"" + id + "\"}");
    batteryItems.clear(); batteryItemShown = false;
}

// ---- Tray icon ------------------------------------------------------------------------------------------------
HICON MakeTrayIcon() {
    // Four rounded tiles in docky's system colours (original artwork).
    const int n = 32; std::vector<std::uint32_t> px(static_cast<size_t>(n) * n, 0);
    const std::uint32_t colors[4] = {0x0A84FF, 0xFF9F0A, 0x30D158, 0xFF375F};
    for (int t = 0; t < 4; ++t) {
        const float x0 = 3.f + (t % 2) * 14.f, y0 = 3.f + (t / 2) * 14.f, s = 12.f, r = 3.5f;
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            float cover = 0;
            for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
                const float px2 = x + (sx + .5f) / 4, py2 = y + (sy + .5f) / 4;
                const float dx = (std::max)({x0 + r - px2, 0.f, px2 - (x0 + s - r)}), dy = (std::max)({y0 + r - py2, 0.f, py2 - (y0 + s - r)});
                if (px2 >= x0 && px2 <= x0 + s && py2 >= y0 && py2 <= y0 + s && dx * dx + dy * dy <= r * r) cover += 1.f / 16;
            }
            if (cover > 0) px[static_cast<size_t>(y) * n + x] = (static_cast<std::uint32_t>(cover * 255) << 24) | colors[t];
        }
    }
    BITMAPV5HEADER bi{}; bi.bV5Size = sizeof(bi); bi.bV5Width = n; bi.bV5Height = -n; bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr; HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}
void AddTray() {
    static HICON icon = MakeTrayIcon();
    NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = host; nid.uID = 1; nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg; nid.hIcon = icon; wcscpy_s(nid.szTip, L"DockOver Desktop");
    trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &nid);
}
void RemoveTray() { if (!trayAdded) return; NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = host; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); trayAdded = false; }

void ThemeChanged() {
    theme = CurrentTheme();
    const auto p = liquid::theme::Palette(theme.dark ? 2 : 1, theme.dark, theme.contrast);
    liquid::popup::SetTheme(theme.contrast || !theme.dark, liquid::theme::ColorRef(p.surface), liquid::theme::ColorRef(p.primary));  // as the Menu Bar does
    RepaintAll(); GetStacks().Relayout();
}

UINT taskbarCreated = 0, hostStarted = 0;
LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == docky::vblank::kMsgVBlank) {  // widget glides, one step per display frame
        glideTicker.Ack();
        std::vector<HWND> moving; for (const auto& g : glides) moving.push_back(g.h);
        for (HWND g : moving) StepGlide(g);
        if (glides.empty()) glideTicker.Stop();
        return 0;
    }
    LRESULT forwarded = 0;
    if (ForwardShellMenuMessage(m, w, l, forwarded)) return forwarded;
    if (taskbarCreated && m == taskbarCreated) { trayAdded = false; AddTray(); return 0; }
    if (hostStarted && m == hostStarted) { UpdateBatteryItem(); return 0; }  // the Menu Bar (re)started
    // Manager shortcut (Settings > Control Center): hide every visible window of this program except the host, or
    // show the ones hidden that way again.
    static const UINT toggleVisible = RegisterWindowMessageW(L"DockyDesktopToggleVisible");
    if (m == toggleVisible) {
        // Widgets and Stacks (the desktop's files) always go together; Windows' own desktop icons follow when
        // Settings > Music > Hide desktop icons too is on (default).
        static std::vector<HWND> hidden; static bool hiddenNow = false;
        if (hiddenNow) {
            for (HWND hw : hidden) if (IsWindow(hw)) ShowWindow(hw, SW_SHOWNA);
            hidden.clear(); hiddenNow = false; HideDesktopIconsForShortcut(false); return 0;
        }
        hiddenNow = true;
        if (liquid::settings::Load(liquid::settings::DefaultPath()).value.controlCenter.hideDesktopIconsWithWidgets) HideDesktopIconsForShortcut(true);
        struct Ctx { HWND host; std::vector<HWND>* out; } ctx{h, &hidden};
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l); DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && w != c->host && IsWindowVisible(w)) { ShowWindow(w, SW_HIDE); c->out->push_back(w); }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return 0;
    }
    switch (m) {
    case WM_COPYDATA: {
        std::string json; if (!docky::ext::ReadJson(l, json)) return 0;
        if (json.find("\"statusItem.") != std::string::npos) {  // a choice in one of our battery items' menus
            if (json.find("\"item\":\"settings\"") != std::string::npos) OpenSettingsPage(Page::Batteries, L"", nullptr);
            else if (json.find("\"item\":\"bt\"") != std::string::npos) ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            return 1;
        }
        ExtWidget e; std::string type, error;
        if (!ParseWidgetMessage(json, e, type, error)) return 0;
        if (type == "widget.gallery") { OpenGallery(); return 1; }
        if (type == "widget.settings") { OpenSettings(); return 1; }
        auto it = std::find_if(extWidgets.begin(), extWidgets.end(), [&](const ExtWidget& x) { return x.id == e.id; });
        if (type == "widget.remove") { if (it == extWidgets.end() || it->fromManifest) return 0; extWidgets.erase(it); RefreshExtWindows(e.id); return 1; }
        if (!e.notify && w) e.notify = reinterpret_cast<HWND>(w);
        const std::wstring regId = e.id;
        if (it != extWidgets.end()) { if (it->fromManifest) return 0; e.image = it->image; e.imageW = it->imageW; e.imageH = it->imageH; e.imagePath = it->imagePath; *it = std::move(e); RefreshExtWindows(it->id); }
        else { if (extWidgets.size() >= 64) return 0; extWidgets.push_back(std::move(e)); Offer(extWidgets.back()); RefreshExtWindows(extWidgets.back().id); }
        if (ExtWidget* x = [&]() -> ExtWidget* { for (auto& q : extWidgets) if (q.id == regId) return &q; return nullptr; }()) {
            for (const auto& st : x->settings) { const int v = ExtValue(*x, st); if (st.type != "button" && v != st.value) SendExtSetting(*x, st, v); }
            if (settingsPage == Page::Ext && settingsExtId == x->id) {
                PaintSettings();
                if (!devPng.empty()) { settingsWin.RequestSnapshot(devPng + L"\\settings-ext.bmp"); PaintSettings(); }
            }
        }
        return 1;
    }
    case kDevicesReady:
        for (auto& wdg : config.widgets) if (wdg.kind == L"battery") PaintWidgetWin(wdg);
        RefreshGallery(); UpdateBatteryItem(); RefreshSettings();
        if (!devPng.empty()) {  // development: what was detected
            FILE* f = nullptr;
            if (_wfopen_s(&f, (devPng + L"\\devices.txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
                for (const auto& d : Devices().Snapshot()) fwprintf(f, L"%ls | %d%% | kind %d | charging %d\n", d.name.c_str(), d.percent, static_cast<int>(d.kind), d.charging ? 1 : 0);
                fclose(f);
            }
        }
        return 0;
    case kMusicFrame:  // the music wallpaper published a frame: glass widgets follow it now, not on the next tick
        musicFramePending = false;
        if (Wallpaper::Get().RefreshMusic())
            for (auto& wdg : config.widgets)
                if (CurrentPrefs().style == 1 || PlateToneOf(wdg.kind) == PlateTone::Glass) PaintWidgetWin(wdg);
        return 0;
    case kWeatherReady:
        for (auto& wdg : config.widgets) if (wdg.kind == L"weather") PaintWidgetWin(wdg);
        RefreshGallery(); RefreshSettings(); return 0;
    case kCalendarReady:
        for (auto& wdg : config.widgets) if (wdg.kind == L"calendar") PaintWidgetWin(wdg);
        RefreshGallery(); RefreshSettings(); return 0;
    case kDataReady: {
        std::unique_ptr<DataResult> r(reinterpret_cast<DataResult*>(l));
        Value data; std::string error;
        if (dock::json::Parse(r->output, data, error) && data.IsObject()) for (auto& e : extWidgets) if (e.id == r->id) { e.data = data; RefreshExtWindows(e.id); }
        return 0;
    }
    case kTrayMsg:
        if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == NIN_SELECT || LOWORD(l) == WM_LBUTTONUP) {
            if (LOWORD(l) == WM_LBUTTONUP) return 0;  // v4 also sends NIN_SELECT for the same click
            POINT pt{GET_X_LPARAM(w), GET_Y_LPARAM(w)}; if (!pt.x && !pt.y) GetCursorPos(&pt);
            ShowMenu(AppItems(), pt, nullptr);
        }
        return 0;
    case WM_TIMER:
        if ((w == kTickTimer || w == kPollTimer) && FullscreenAppInFront()) return 0;  // game mode: widgets are covered
        if (w == kTickTimer) {
            const ULONGLONG now = GetTickCount64();
            const bool musicChanged = Wallpaper::Get().RefreshMusic();
            for (auto& wdg : config.widgets) {
                const bool glassChanged = musicChanged && (CurrentPrefs().style == 1 || PlateToneOf(wdg.kind) == PlateTone::Glass);
                if (glassChanged || now - wdg.lastPaint + 20 >= RefreshMs(wdg.kind)) PaintWidgetWin(wdg);
                // Back to the normal view (today, current weather) 20 s after the last scroll.
                if (wdg.lastScroll && now - wdg.lastScroll > 20000 && std::fabs(wdg.scroll.Target()) > 0.5) { wdg.lastScroll = 0; ScrollTo(wdg, 0); }
            }
        } else if (w == kPollTimer) {
            SampleSystem();
            if (Wallpaper::Get().Stale()) { Wallpaper::Get().Invalidate(); RepaintAll(); GetStacks().Relayout(); }  // new wallpaper, no broadcast
            const auto rev = liquid::settings::ReadRevision(liquid::settings::DefaultPath());
            if (rev != themeRevision) { themeRevision = rev; ThemeChanged(); const auto pf = liquid::settings::Load(liquid::settings::DefaultPath()).value; docky::ui::SetLanguage(pf.appearance.language); gameCompatDesktop = pf.dock.gameCompatibility; docky::vblank::SetCaps(pf.performance.animationFps, pf.performance.captureFps); }
            static int ticks = 0;
            if (++ticks % 5 == 0) {
                ReloadManifests();
                const auto before = extWidgets.size();
                extWidgets.erase(std::remove_if(extWidgets.begin(), extWidgets.end(), [](const ExtWidget& e) { return !e.fromManifest && e.notify && !IsWindow(e.notify); }), extWidgets.end());
                if (extWidgets.size() != before) RepaintAll();
            }
            TickDataCommands();
            MaintainDesktopIcons();
        } else if (w == kDevMenuTimer) {  // development: the first widget's menu, then a snapshot of it
            KillTimer(h, kDevMenuTimer);
            if (!config.widgets.empty()) { auto& wdg = config.widgets.front(); POINT pt{wdg.pos.x + 40, wdg.pos.y + 40}; ShowWidgetMenu(wdg, pt); SetTimer(h, kDevMenuShot, 1200, nullptr); }
        } else if (w == kDevMenuShot) {
            KillTimer(h, kDevMenuShot);
            if (HWND popup = FindWindowW(L"LiquidUnifiedPopup98d2", nullptr)) liquid::popup::RequestDiagnosticSnapshot(popup, devPng + L"\\menu.bmp");
        } else if (w == kExitTimer) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER) { Wallpaper::Get().Invalidate(); RepaintAll(); GetStacks().Relayout(); }
        else if (w == SPI_SETWORKAREA) GetStacks().Relayout();
        else if (l && !wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet")) ThemeChanged();
        return 0;
    case WM_DISPLAYCHANGE: Wallpaper::Get().Invalidate(); RepaintAll(); GetStacks().Relayout(); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        CloseGallery(); Calendars().Stop(); Devices().Stop(); WeatherService().Stop();
        RemoveBatteryItems(); liquid::popup::Close(); RemoveTray(); GetStacks().Stop(); HideDesktopIconsForShortcut(false); HideDesktopIcons(false);
        for (auto& wdg : config.widgets) if (wdg.hwnd) { UnregisterLayer(wdg.hwnd); DestroyWindow(wdg.hwnd); wdg.hwnd = nullptr; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
// Crash note for development: exception code and module offset in %TEMP%\docky-desktop-crash.txt (no user data).
LONG WINAPI CrashNote(EXCEPTION_POINTERS* e) {
    HideDesktopIcons(false);  // never leave Explorer's desktop icons hidden behind a crash
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    HMODULE module = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(e->ExceptionRecord->ExceptionAddress), &module);
    wchar_t name[MAX_PATH]{}; if (module) GetModuleFileNameW(module, name, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(temp) + L"docky-desktop-crash.txt").c_str(), L"w") == 0 && f) {
        fwprintf(f, L"code=0x%08lX module=%ls offset=0x%llX\n", e->ExceptionRecord->ExceptionCode, name,
                 static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(e->ExceptionRecord->ExceptionAddress) - reinterpret_cast<ULONG_PTR>(module)));
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(CrashNote);
    {   // Diagnostics without UI or single-instance check: DockyDesktop.exe --list-batteries prints what is detected.
        int n = 0; LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
        const bool list = a && n >= 2 && std::wstring(a[1]) == L"--list-batteries";
        std::wstring target = list && n >= 3 ? a[2] : L"";
        const bool a2render = a && n >= 5 && std::wstring(a[1]) == L"--render-widget";
        const std::wstring renderKind = a2render ? a[2] : L"", renderSize = a2render ? a[3] : L"", renderOut = a2render ? a[4] : L"";
        if (a2render && n >= 6) { DevCalendar() = a[5]; for (auto& c : DevCalendar()) if (c == L'|') c = L'\n'; }  // optional test feeds ("a.ics #RRGGBB|b.ics"), not calendars.txt
        if (a) LocalFree(a);
        if (a2render) {  // --render-widget <kind> <small|medium|large> <out.png>: one widget, offscreen (diagnostics)
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Devices().Start(nullptr, 0); Calendars().Start(nullptr, 0); WeatherService().SetFahrenheit(CurrentPrefs().fahrenheit); WeatherService().Start(nullptr, 0);
            SampleSystem(); Sleep(renderKind == L"weather" ? 6000 : 800); SampleSystem();
            LoadConfig();  // widget preferences (clock style, battery glass, plate style) as the running Desktop uses them
            { wchar_t local[MAX_PATH]{}; if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(local) + L"\\LiquidDock\\glass.json"); }
            theme = CurrentTheme();
            const Size size = ParseSize(U(renderSize)); const auto sz = SizeDip(size);
            Canvas canvas; ID2D1RenderTarget* rt = canvas.Begin(static_cast<int>(sz.width + 2 * kShadow), static_cast<int>(sz.height + 2 * kShadow), 96);
            if (rt) {
                const D2D1_RECT_F shape = D2D1::RectF(kShadow, kShadow, kShadow + sz.width, kShadow + sz.height);
                const Theme content = PaintPlate(rt, shape, RECT{0, 0, static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)}, theme, PlateToneOf(renderKind));
                {   // development: DOCKOVER_WIDGET_SCROLL=<DIP> renders a scrolled view (clamped by the painter's range)
                    wchar_t v[32]{};
                    if (GetEnvironmentVariableW(L"DOCKOVER_WIDGET_SCROLL", v, 32)) { PaintScroll() = WidgetScroll{0, -1e6f, 1e6f}; PaintWidget(rt, renderKind, size, shape, content, nullptr);
                        const float off = std::clamp(static_cast<float>(_wtof(v)), PaintScroll().min, PaintScroll().max); canvas.End(nullptr, POINT{}); rt = canvas.Begin(static_cast<int>(sz.width + 2 * kShadow), static_cast<int>(sz.height + 2 * kShadow), 96);
                        PaintPlate(rt, shape, RECT{0, 0, static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)}, theme, PlateToneOf(renderKind)); PaintScroll() = WidgetScroll{off, 0, 0}; }
                }
                PaintWidget(rt, renderKind, size, shape, content, nullptr);
                canvas.End(nullptr, POINT{});
                canvas.SavePng(renderOut);
            }
            Devices().Stop(); Calendars().Stop(); WeatherService().Stop();
            return 0;
        }
        if (list) {  // to the given file (UTF-8), else to the parent console
            FILE* out = nullptr;
            if (!target.empty()) _wfopen_s(&out, target.c_str(), L"w, ccs=UTF-8");
            else if (AttachConsole(ATTACH_PARENT_PROCESS)) _wfreopen_s(&out, L"CONOUT$", L"w", stdout);
            if (out) { for (const auto& d : ReadDeviceBatteriesNow()) fwprintf(out, L"%ls | %d%% | kind %d | charging %d\n", d.name.c_str(), d.percent, static_cast<int>(d.kind), d.charging ? 1 : 0); fclose(out); }
            return 0;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // --dev-isolated (tests only): runs beside the user's copy: its own instance lock, no tray icon, no broadcasts.
    const bool isolated = wcsstr(GetCommandLineW(), L"--dev-isolated") != nullptr;
    HANDLE single = CreateMutexW(nullptr, FALSE, isolated ? L"Local\\DockyDesktopV1DevIsolated" : L"Local\\DockyDesktopV1SingleInstance");
    if (!single) return 1;
    const bool already = GetLastError() == ERROR_ALREADY_EXISTS;
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring devPage;
    unsigned exitAfter = 0; bool openGallery = false, openSettings = false; int devOpenStack = -1; bool devPanel = false, devSnap = false;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--exit-after" && i + 1 < argc) exitAfter = static_cast<unsigned>(_wtoi(argv[++i]));
        else if (a == L"--gallery") openGallery = true;
        else if (a == L"--settings") openSettings = true;
        else if (a == L"--settings-page" && i + 1 < argc) { openSettings = true; devPage = argv[++i]; }  // development: weather|battery|calendar
        else if (a == L"--dev-calendar" && i + 1 < argc) DevCalendar() = argv[++i];
        else if (a == L"--dev-calendar-picker") calPicker = 0;  // development: the colour wheel open on the first calendar
        else if (a == L"--dev-png" && i + 1 < argc) { devPng = argv[++i]; DevPngDir() = devPng; }
        else if (a == L"--log" && i + 1 < argc) dock::Log::Get().Open(argv[++i]);
        else if (a == L"--dev-open-stack" && i + 1 < argc) devOpenStack = _wtoi(argv[++i]);
        else if (a == L"--dev-stacks-folder" && i + 1 < argc) DevFolder() = argv[++i];
        else if (a == L"--dev-stack-panel") devPanel = true;
        else if (a == L"--dev-menu") devMenu = true;
        else if (a == L"--dev-snap-check") devSnap = true;
    }
    if (argv) LocalFree(argv);
    if (already) {  // a second launch opens the running instance's gallery
        if (HWND running = docky::ext::FindDesktopHost()) docky::ext::SendJson(running, nullptr, openSettings ? R"({"v":1,"type":"widget.settings"})" : R"({"v":1,"type":"widget.gallery"})");
        CloseHandle(single); return 0;
    }
    if (FAILED(OleInitialize(nullptr))) { CloseHandle(single); return 1; }
    HINSTANCE inst = instance;
    WNDCLASSW wc{}; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = HostProc; wc.lpszClassName = kHostClass; RegisterClassW(&wc);
    wc.lpfnWndProc = WidgetProc; wc.lpszClassName = kWidgetClass; RegisterClassW(&wc);
    wc.lpfnWndProc = GalleryProc; wc.lpszClassName = kGalleryClass; RegisterClassW(&wc);
    host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"DockOver Desktop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    docky::capture_visible::Watch();  // screenshots show the widgets' glass (capture_visible.h)
    if (!host) { OleUninitialize(); CloseHandle(single); return 1; }
    ChangeWindowMessageFilterEx(host, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    hostStarted = RegisterWindowMessageW(L"dockyHostStarted");
    // Menus use the shared live capture pipeline, with a composed fallback when capture is unavailable.
    liquid::popup::SetStaticBackdrop(ComposeBackdrop);
    liquid::popup::SetRenderDevice(SharedUiDevice());  // menus, panels and halos share one device (see SharedUiDevice)
    { wchar_t local[MAX_PATH]{}; if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(local) + L"\\LiquidDock\\glass.json"); }
    themeRevision = liquid::settings::ReadRevision(liquid::settings::DefaultPath());
    { const auto pf = liquid::settings::Load(liquid::settings::DefaultPath()).value; docky::ui::SetLanguage(pf.appearance.language); gameCompatDesktop = pf.dock.gameCompatibility; docky::vblank::SetCaps(pf.performance.animationFps, pf.performance.captureFps); }
    theme = CurrentTheme();
    { const auto p = liquid::theme::Palette(theme.dark ? 2 : 1, theme.dark, theme.contrast); liquid::popup::SetTheme(theme.contrast || !theme.dark, liquid::theme::ColorRef(p.surface), liquid::theme::ColorRef(p.primary)); }
    SampleSystem();
    const bool firstRun = !LoadConfig();
    if (firstRun) {  // a clock and a calendar side by side at the top-left, like a fresh macOS desktop
        const RECT work = PrimaryWork(); const UINT dpi = DpiAt(POINT{work.left, work.top});
        WidgetWin clock; clock.kind = L"clock"; clock.pos = POINT{work.left + Px(kMargin, dpi), work.top + Px(kMargin, dpi)};
        WidgetWin cal; cal.kind = L"calendar"; cal.pos = POINT{clock.pos.x + Px(kSmall + kGap, dpi), clock.pos.y};
        config.widgets.push_back(std::move(clock)); config.widgets.push_back(std::move(cal));
        SaveConfig();
    }
    Calendars().Start(host, kCalendarReady);
    Devices().Start(host, kDevicesReady);
    WeatherService().SetFahrenheit(CurrentPrefs().fahrenheit);
    WeatherService().Start(host, kWeatherReady);
    for (auto& w : config.widgets) CreateWidgetWindow(w);
    if (devSnap && config.widgets.size() >= 2 && !devPng.empty()) {  // development: placement results for fixed drops
        auto& a2 = config.widgets[0]; auto& b2 = config.widgets[1];
        const POINT drops[4] = {{b2.pos.x + 5, b2.pos.y + 9}, {a2.pos.x + 100, a2.pos.y + 3}, {b2.pos.x + 400, b2.pos.y + 150}, {a2.pos.x + 7, a2.pos.y + 230}};
        FILE* f = nullptr;
        if (_wfopen_s(&f, (devPng + L"\\snap.txt").c_str(), L"w") == 0 && f) {
            fwprintf(f, L"A=%ld,%ld B=%ld,%ld magnet(B+5,+9)=", a2.pos.x, a2.pos.y, b2.pos.x, b2.pos.y);
            const POINT m = Magnet(b2, drops[0]); fwprintf(f, L"%ld,%ld\n", m.x, m.y);
            for (const auto& d : drops) { const POINT r = Settle(b2, d); fwprintf(f, L"drop %ld,%ld -> %ld,%ld\n", d.x, d.y, r.x, r.y); }
            fclose(f);
        }
    }
    ReloadManifests();
    TickDataCommands();
    if (devPanel) config.style = StackStyle::Panel;
    ApplyStacks();
    if (devOpenStack >= 0) GetStacks().DevOpen(devOpenStack);
    if (!isolated) AddTray();
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Music wallpaper frames: a waiter thread turns the publisher's ready event into one queued message at a time.
    std::thread([hostWnd = host] {
        const HANDLE ready = docky::music_wallpaper::OpenReadyEvent();
        if (!ready) return;
        while (WaitForSingleObject(ready, INFINITE) == WAIT_OBJECT_0) {
            if (!IsWindow(hostWnd)) break;
            if (!musicFramePending.exchange(true)) PostMessageW(hostWnd, kMusicFrame, 0, 0);
        }
        CloseHandle(ready);
    }).detach();
    SetTimer(host, kTickTimer, 250, nullptr);
    SetTimer(host, kPollTimer, 2000, nullptr);
    if (exitAfter) SetTimer(host, kExitTimer, exitAfter * 1000, nullptr);
    if (devMenu && !devPng.empty()) SetTimer(host, kDevMenuTimer, 1500, nullptr);
    if (!isolated) PostMessageW(HWND_BROADCAST, hostStarted, 0, 0);
    if (openGallery || firstRun) OpenGallery();
    if (openSettings && devPage.empty()) OpenSettings();
    else if (openSettings) {  // beside the first widget of that kind, as its right-click "Edit" opens it
        HWND anchor = nullptr; for (const auto& w : config.widgets) if (w.kind == devPage) { anchor = w.hwnd; break; }
        if (devPage.rfind(L"ext:", 0) == 0) OpenSettingsPage(Page::Ext, devPage.substr(4), nullptr);
        else OpenSettingsPage(devPage == L"weather" ? Page::Weather : devPage == L"battery" ? Page::Batteries : devPage == L"clock" ? Page::Clock : Page::Calendar, L"", anchor);
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (hook) UnhookWinEvent(hook);
    OleUninitialize();
    CloseHandle(single);
    return 0;
}
