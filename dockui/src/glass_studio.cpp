#include "glass_studio.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "glass_params.h"
#include "json_lite.h"
#include "log.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace dock {
namespace {

enum : int {
    kIdMaterial = 100, kIdBackdrop, kIdDebug, kIdBlurQ, kIdContinuous, kIdPresetCombo, kIdPresetName, kIdLoad, kIdSave,
    kIdRename, kIdDelete, kIdReset, kIdSaveDefault, kIdPaste, kIdCopy, kIdPickTint, kIdStatus, kIdStats, kIdPanel, kIdSurface,
    kIdParamBase = 1000,  // + index * 4 + {0 label, 1 trackbar, 2 edit}
    kIdFlagBase = 3000,
};
constexpr UINT_PTR kTimerStats = 1;
constexpr UINT_PTR kTimerSave = 2;
constexpr int kTrackMax = 1000;
constexpr wchar_t kStudioClass[] = L"LiquidDockGlassStudio";
constexpr wchar_t kPanelClass[] = L"LiquidDockGlassStudioPanel";

struct Row {
    const wchar_t* header = nullptr;
    int param = -1;
    int flag = -1;
    HWND label = nullptr, track = nullptr, edit = nullptr, check = nullptr;
};

std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<HWND> g_hwnd{nullptr};

class Studio {
public:
    bool Create(HWND dock);
    bool PreTranslate(MSG& msg);
    HWND hwnd = nullptr;

private:
    static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnMain(UINT m, WPARAM w, LPARAM l);
    LRESULT OnPanel(UINT m, WPARAM w, LPARAM l);

    int Px(float dip) const { return static_cast<int>(std::lround(dip * dpi_ / 96.0f)); }
    HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0);
    void BuildControls();
    void Layout();
    void LayoutPanel();
    void UpdateFont();
    void RefreshAll();
    void RefreshRow(const Row& r);
    void RefreshPresets(const std::wstring& select = L"");
    void UpdateStats();
    void Push();
    void CommitEdit(HWND edit);
    void OnTrack(HWND track);
    void OnCommand(int id, int code, HWND ctl);
    std::wstring PresetSelection(int* builtin) const;
    std::wstring ReadText(HWND h) const;
    static std::wstring Format(float v, const FloatParam& d);
    static void ApplyLook(GlassParameters& dst, const GlassParameters& src);

    HWND dock_ = nullptr;
    HWND panel_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;
    UINT dpi_ = 96;
    GlassParameters p_;
    std::vector<Row> rows_;
    std::vector<HWND> headerCtls_;
    HWND material_ = nullptr, backdrop_ = nullptr, debug_ = nullptr, blurQ_ = nullptr, continuous_ = nullptr;
    HWND presetCombo_ = nullptr, presetName_ = nullptr, status_ = nullptr, stats_ = nullptr;
    HWND btn_[9] = {};
    HWND lblMaterial_ = nullptr, lblBackdrop_ = nullptr, lblDebug_ = nullptr, lblBlur_ = nullptr, lblPreset_ = nullptr;
    HWND lblSurface_ = nullptr, surface_ = nullptr;  // which glass surface the sliders edit
    GlassSurface editing_ = GlassSurface::Dock;
    int scroll_ = 0, content_ = 0;
    bool updating_ = false;
};

HWND Studio::Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (h && font_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    return h;
}

bool Studio::Create(HWND dock) {
    dock_ = dock;
    p_ = GlassSettingsStore::Get().Current();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kStudioClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = PanelProc;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPanelClass;
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    const UINT sysDpi = GetDpiForSystem();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int w = MulDiv(640, sysDpi, 96), h = MulDiv(880, sysDpi, 96);
    hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kStudioClass, L"Glass Studio - DockOver", WS_OVERLAPPEDWINDOW,
                           mi.rcWork.right - w - 40, mi.rcWork.top + 40, w, std::min<int>(h, mi.rcWork.bottom - mi.rcWork.top - 80),
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    dpi_ = GetDpiForWindow(hwnd);
    UpdateFont();
    BuildControls();
    RefreshAll();
    Layout();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, kTimerStats, 250, nullptr);  // refreshes the stats text only; never drives the Dock
    DLOG(L"studio: opened");
    return true;
}

void Studio::UpdateFont() {
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                        L"Segoe UI");
    bold_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                        L"Segoe UI");
}

void Studio::BuildControls() {
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    lblSurface_ = Make(hwnd, WC_STATICW, L"Category", SS_LEFT, 0);
    surface_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdSurface);
    for (int i = 0; i < kGlassSurfaceCount; ++i) ComboBox_AddString(surface_, GlassSurfaceName(static_cast<GlassSurface>(i)));
    ComboBox_SetCurSel(surface_, 0);
    lblMaterial_ = Make(hwnd, WC_STATICW, L"Material", SS_LEFT, 0);
    material_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdMaterial);
    ComboBox_AddString(material_, L"Liquid Glass (GPU)");
    ComboBox_AddString(material_, L"Smoked Glass (V0.2 fallback)");
    lblBackdrop_ = Make(hwnd, WC_STATICW, L"Backdrop", SS_LEFT, 0);
    backdrop_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdBackdrop);
    for (int i = 0; i < 6; ++i) ComboBox_AddString(backdrop_, BackdropName(static_cast<BackdropKind>(i)));
    status_ = Make(hwnd, WC_STATICW, L"", SS_LEFT, kIdStatus);
    stats_ = Make(hwnd, WC_STATICW, L"", SS_LEFT, kIdStats);
    lblPreset_ = Make(hwnd, WC_STATICW, L"Preset", SS_LEFT, 0);
    presetCombo_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdPresetCombo);
    presetName_ = Make(hwnd, WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kIdPresetName);
    SendMessageW(presetName_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"new / rename-to name"));
    const wchar_t* labels[9] = {L"Load", L"Save", L"Rename", L"Delete", L"Reset to default", L"Save as default",
                                L"Paste Glass Lab JSON", L"Copy JSON", L"Pick tint..."};
    const int ids[9] = {kIdLoad, kIdSave, kIdRename, kIdDelete, kIdReset, kIdSaveDefault, kIdPaste, kIdCopy, kIdPickTint};
    for (int i = 0; i < 9; ++i) btn_[i] = Make(hwnd, WC_BUTTONW, labels[i], BS_PUSHBUTTON | WS_TABSTOP, ids[i]);
    lblDebug_ = Make(hwnd, WC_STATICW, L"Debug view [Dock]", SS_LEFT, 0);
    debug_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdDebug);
    for (const wchar_t* s : {L"Off", L"SDF distance bands", L"Normals", L"Refraction offset", L"Blur texture",
                             L"Sharp backdrop", L"Frost mask"})
        ComboBox_AddString(debug_, s);
    lblBlur_ = Make(hwnd, WC_STATICW, L"Blur quality [Dock]", SS_LEFT, 0);
    blurQ_ = Make(hwnd, WC_COMBOBOXW, L"", combo, kIdBlurQ);
    ComboBox_AddString(blurQ_, L"Half resolution (as Glass Lab)");
    ComboBox_AddString(blurQ_, L"Quarter resolution");
    continuous_ = Make(hwnd, WC_BUTTONW, L"Continuous render (benchmark)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdContinuous);

    panel_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                             0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPanel)),
                             GetModuleHandleW(nullptr), this);
    const wchar_t* groups[] = {L"Geometry", L"Optics", L"Lighting", L"Material", L"Interaction"};
    for (const wchar_t* g : groups) {
        Row header;
        header.header = g;
        header.label = Make(panel_, WC_STATICW, g, SS_LEFT, 0);
        SendMessageW(header.label, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), FALSE);
        rows_.push_back(header);
        const auto& flags = FlagParams();
        for (int i = 0; i < static_cast<int>(flags.size()); ++i) {
            if (wcscmp(flags[i].group, g) != 0) continue;
            Row r;
            r.flag = i;
            r.check = Make(panel_, WC_BUTTONW, flags[i].label, BS_AUTOCHECKBOX | WS_TABSTOP, kIdFlagBase + i);
            rows_.push_back(r);
        }
        const auto& params = FloatParams();
        for (int i = 0; i < static_cast<int>(params.size()); ++i) {
            if (wcscmp(params[i].group, g) != 0) continue;
            Row r;
            r.param = i;
            std::wstring text = params[i].label;
            if (!params[i].labKey) text += L" [Dock]";
            if (params[i].note) text += std::wstring(L"  (") + params[i].note + L")";
            r.label = Make(panel_, WC_STATICW, text.c_str(), SS_LEFT | SS_ENDELLIPSIS | SS_NOTIFY, kIdParamBase + i * 4);
            r.track = Make(panel_, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kIdParamBase + i * 4 + 1);
            SendMessageW(r.track, TBM_SETRANGE, FALSE, MAKELPARAM(0, kTrackMax));
            SendMessageW(r.track, TBM_SETPAGESIZE, 0, kTrackMax / 20);
            r.edit = Make(panel_, WC_EDITW, L"", ES_RIGHT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kIdParamBase + i * 4 + 2);
            rows_.push_back(r);
        }
    }
}

void Studio::Layout() {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, m = Px(12), h = Px(24), gap = Px(6);
    int y = m;
    const int lw = Px(116);
    auto place = [](HWND c, int x, int yy, int w, int hh) { MoveWindow(c, x, yy, std::max(w, 1), hh, TRUE); };
    place(lblSurface_, m, y + Px(4), lw, h);
    place(surface_, m + lw, y, W - 2 * m - lw, Px(200));  // category names are long
    y += h + gap;
    place(lblMaterial_, m, y + Px(4), lw, h);
    place(material_, m + lw, y, Px(220), Px(200));
    y += h + gap;
    place(lblBackdrop_, m, y + Px(4), lw, h);
    place(backdrop_, m + lw, y, W - 2 * m - lw, Px(200));
    y += h + gap;
    place(status_, m, y, W - 2 * m, Px(34));
    y += Px(36);
    place(stats_, m, y, W - 2 * m, Px(34));
    y += Px(38);
    place(lblPreset_, m, y + Px(4), lw, h);
    place(presetCombo_, m + lw, y, Px(230), Px(240));
    place(presetName_, m + lw + Px(236), y, W - 2 * m - lw - Px(236), h);
    y += h + gap;
    const int bw = (W - 2 * m - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) place(btn_[i], m + i * (bw + gap), y, bw, h + Px(2));
    y += h + gap + Px(2);
    for (int i = 4; i < 8; ++i) place(btn_[i], m + (i - 4) * (bw + gap), y, bw, h + Px(2));
    y += h + gap + Px(2);
    place(lblDebug_, m, y + Px(4), lw, h);
    place(debug_, m + lw, y, Px(200), Px(240));
    place(btn_[8], W - m - bw, y, bw, h + Px(2));
    y += h + gap;
    place(lblBlur_, m, y + Px(4), lw, h);
    place(blurQ_, m + lw, y, Px(200), Px(200));
    place(continuous_, m + lw + Px(210), y, W - 2 * m - lw - Px(210), h);
    y += h + Px(10);
    MoveWindow(panel_, 0, y, W, std::max<int>(1, rc.bottom - y), TRUE);
    LayoutPanel();
}

void Studio::LayoutPanel() {
    RECT rc;
    GetClientRect(panel_, &rc);
    const int W = rc.right, m = Px(12), editW = Px(70), labelW = std::max(Px(200), (W - 2 * m) * 45 / 100);
    int y = Px(6);
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(rows_.size()) * 3);
    auto put = [&](HWND c, int x, int yy, int w, int hh) {
        if (c) dwp = DeferWindowPos(dwp, c, nullptr, x, yy - scroll_, std::max(w, 1), hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (const Row& r : rows_) {
        if (r.header) {
            y += Px(8);
            put(r.label, m, y, W - 2 * m, Px(22));
            y += Px(26);
        } else if (r.flag >= 0) {
            put(r.check, m + Px(4), y, W - 2 * m, Px(22));
            y += Px(26);
        } else {
            put(r.label, m + Px(4), y + Px(4), labelW - Px(8), Px(20));
            put(r.track, m + labelW, y, W - 2 * m - labelW - editW - Px(6), Px(26));
            put(r.edit, W - m - editW, y + Px(2), editW, Px(22));
            y += Px(30);
        }
    }
    EndDeferWindowPos(dwp);
    content_ = y + Px(12);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_ - 1),
                  static_cast<UINT>(std::max<LONG>(1, rc.bottom)), scroll_};
    SetScrollInfo(panel_, SB_VERT, &si, TRUE);
}

std::wstring Studio::Format(float v, const FloatParam& d) {
    wchar_t buf[32];
    const float span = d.maxValue - d.minValue;
    swprintf_s(buf, span >= 20.0f ? L"%.1f" : span >= 2.0f ? L"%.2f" : L"%.3f", v);
    return buf;
}

void Studio::RefreshRow(const Row& r) {
    if (r.param >= 0) {
        const FloatParam& d = FloatParams()[r.param];
        const float v = p_.*d.field;
        const int pos = static_cast<int>(std::lround((v - d.minValue) / (d.maxValue - d.minValue) * kTrackMax));
        SendMessageW(r.track, TBM_SETPOS, TRUE, pos);
        SetWindowTextW(r.edit, Format(v, d).c_str());
    } else if (r.flag >= 0) {
        Button_SetCheck(r.check, (p_.*FlagParams()[r.flag].field) ? BST_CHECKED : BST_UNCHECKED);
    }
}

void Studio::RefreshAll() {
    updating_ = true;
    for (const Row& r : rows_) RefreshRow(r);
    ComboBox_SetCurSel(material_, static_cast<int>(p_.material));
    ComboBox_SetCurSel(backdrop_, static_cast<int>(p_.backdrop));
    ComboBox_SetCurSel(debug_, p_.debugView);
    ComboBox_SetCurSel(blurQ_, p_.blurDownsample >= 4 ? 1 : 0);
    Button_SetCheck(continuous_, p_.continuousRender ? BST_CHECKED : BST_UNCHECKED);
    updating_ = false;
    int builtin = 0;
    RefreshPresets(PresetSelection(&builtin));  // keep the current preset selected
    UpdateStats();
}

void Studio::RefreshPresets(const std::wstring& select) {
    ComboBox_ResetContent(presetCombo_);
    ComboBox_AddString(presetCombo_, L"[Built-in] Glass Lab defaults");
    ComboBox_AddString(presetCombo_, L"[Built-in] Launcher tuned");
    int sel = 0;
    for (const std::wstring& n : GlassSettingsStore::Get().PresetNames()) {
        const int i = ComboBox_AddString(presetCombo_, n.c_str());
        if (n == select) sel = i;
    }
    ComboBox_SetCurSel(presetCombo_, sel);
}

std::wstring Studio::ReadText(HWND h) const {
    wchar_t buf[256];
    GetWindowTextW(h, buf, ARRAYSIZE(buf));
    std::wstring s = buf;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}

std::wstring Studio::PresetSelection(int* builtin) const {
    const int sel = ComboBox_GetCurSel(presetCombo_);
    *builtin = sel == 0 ? 1 : sel == 1 ? 2 : 0;
    if (sel < 2) return L"";
    wchar_t buf[256] = {};
    ComboBox_GetLBText(presetCombo_, sel, buf);
    return buf;
}

void Studio::ApplyLook(GlassParameters& dst, const GlassParameters& src) {
    // Presets describe the look; backdrop source, debug view and benchmark mode stay session settings.
    const BackdropKind b = dst.backdrop;
    const int dv = dst.debugView;
    const bool cont = dst.continuousRender;
    dst = src;
    dst.backdrop = b;
    dst.debugView = dv;
    dst.continuousRender = cont;
}

void Studio::UpdateStats() {
    GlassStats& st = GlassSettingsStore::Get().stats;
    const int bs = st.backdropState.load();
    const wchar_t* state = bs == 1 ? L"loading" : bs == 2 ? L"ready" : bs == 3 ? L"FAILED (dark fallback)" : L"off";
    std::wstring status = L"Backdrop: ";
    if (p_.backdrop == BackdropKind::None) status += L"none - Liquid Glass falls back to Smoked Glass";
    else if (p_.backdrop == BackdropKind::Live) status += std::wstring(L"Live WGC (") + state + L")";
    else status += std::wstring(L"SIMULATED, not live capture (") + state + L")";
    SetWindowTextW(status_, status.c_str());
    wchar_t buf[256];
    const bool anim = st.animating.load();
    std::wstring gpu = st.gpuValid.load() ? L"" : L"GPU: not yet measured";
    if (st.gpuValid.load()) {
        wchar_t g[96];
        swprintf_s(g, L"GPU %.3f ms (glass %.3f)", st.gpuFrameMs.load(), st.gpuGlassMs.load());
        gpu = g;
    }
    swprintf_s(buf, L"Drawing: %ls   |   %ls %.1f fps   |   CPU %.3f ms   |   %ls",
               st.activeMaterial.load() == 0 ? L"Liquid Glass" : L"Smoked Glass",
               anim ? L"animating" : L"idle (on demand), last", st.fps.load(), st.cpuMs.load(), gpu.c_str());
    SetWindowTextW(stats_, buf);
}

void Studio::Push() {
    // Dock edits apply live; other surfaces apply in-process at once (Dock menus) and in the Menu Bar process
    // when it next checks glass.json (saved below).
    GlassSettingsStore::Get().SetSurface(editing_, p_);
    SetTimer(hwnd, kTimerSave, 600, nullptr);  // debounced persistence on this thread
}

void Studio::OnTrack(HWND track) {
    for (const Row& r : rows_) {
        if (r.track != track) continue;
        const FloatParam& d = FloatParams()[r.param];
        const int pos = static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
        p_.*d.field = d.minValue + (d.maxValue - d.minValue) * pos / kTrackMax;
        updating_ = true;
        SetWindowTextW(r.edit, Format(p_.*d.field, d).c_str());
        updating_ = false;
        Push();
        return;
    }
}

void Studio::CommitEdit(HWND edit) {
    for (const Row& r : rows_) {
        if (r.edit != edit) continue;
        const FloatParam& d = FloatParams()[r.param];
        const std::wstring text = ReadText(edit);
        wchar_t* end = nullptr;
        const float v = wcstof(text.c_str(), &end);
        if (text.empty() || !end || *end != L'\0' || !std::isfinite(v)) {
            RefreshRow(r);  // invalid input: restore the current value
            return;
        }
        p_.*d.field = std::clamp(v, d.minValue, d.maxValue);
        RefreshRow(r);
        Push();
        return;
    }
}

void Studio::OnCommand(int id, int code, HWND ctl) {
    GlassSettingsStore& store = GlassSettingsStore::Get();
    if (updating_) return;
    if (id >= kIdFlagBase && id < kIdFlagBase + 100 && code == BN_CLICKED) {
        const FlagParam& f = FlagParams()[id - kIdFlagBase];
        p_.*f.field = Button_GetCheck(ctl) == BST_CHECKED;
        Push();
        return;
    }
    if (id >= kIdParamBase && id < kIdFlagBase && (id - kIdParamBase) % 4 == 2 && code == EN_KILLFOCUS) {
        CommitEdit(ctl);
        return;
    }
    switch (id) {
    case kIdMaterial:
        if (code == CBN_SELCHANGE) { p_.material = static_cast<MaterialKind>(ComboBox_GetCurSel(ctl)); Push(); }
        break;
    case kIdBackdrop:
        if (code == CBN_SELCHANGE) { p_.backdrop = static_cast<BackdropKind>(ComboBox_GetCurSel(ctl)); Push(); UpdateStats(); }
        break;
    case kIdDebug:
        if (code == CBN_SELCHANGE) { p_.debugView = ComboBox_GetCurSel(ctl); Push(); }
        break;
    case kIdBlurQ:
        // Needs new blur textures: the material re-creates them lazily on its next frame.
        if (code == CBN_SELCHANGE) { p_.blurDownsample = ComboBox_GetCurSel(ctl) == 1 ? 4 : 2; Push(); }
        break;
    case kIdContinuous:
        if (code == BN_CLICKED) { p_.continuousRender = Button_GetCheck(ctl) == BST_CHECKED; Push(); }
        break;
    case kIdLoad: {
        int builtin = 0;
        const std::wstring name = PresetSelection(&builtin);
        GlassParameters src;
        if (builtin == 1) src = GlassLabDefaults();
        else if (builtin == 2) src = LauncherTunedPreset();
        else if (!store.GetPreset(name, src)) break;
        ApplyLook(p_, src);
        RefreshAll();
        Push();
        DLOG(L"studio: loaded preset %ls", builtin ? (builtin == 1 ? L"[Glass Lab defaults]" : L"[Launcher tuned]") : name.c_str());
        break;
    }
    case kIdSave: {
        int builtin = 0;
        std::wstring name = ReadText(presetName_);
        if (name.empty()) name = PresetSelection(&builtin);
        if (name.empty()) {
            MessageBoxW(hwnd, L"Type a preset name in the box next to the list.", L"Glass Studio", MB_ICONINFORMATION);
            break;
        }
        store.SavePreset(name, p_);
        store.Save();
        SetWindowTextW(presetName_, L"");
        RefreshPresets(name);
        DLOG(L"studio: saved preset (%zu chars)", name.size());
        break;
    }
    case kIdRename: {
        int builtin = 0;
        const std::wstring from = PresetSelection(&builtin);
        const std::wstring to = ReadText(presetName_);
        if (builtin || from.empty() || to.empty() || !store.RenamePreset(from, to)) {
            MessageBoxW(hwnd, L"Select a saved (not built-in) preset and type a new, unused name.", L"Glass Studio",
                        MB_ICONINFORMATION);
            break;
        }
        store.Save();
        SetWindowTextW(presetName_, L"");
        RefreshPresets(to);
        break;
    }
    case kIdDelete: {
        int builtin = 0;
        const std::wstring name = PresetSelection(&builtin);
        if (builtin || name.empty()) break;
        if (MessageBoxW(hwnd, (L"Delete preset \"" + name + L"\"?").c_str(), L"Glass Studio", MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        store.DeletePreset(name);
        store.Save();
        RefreshPresets();
        break;
    }
    case kIdSurface:
        if (code == CBN_SELCHANGE) {
            editing_ = static_cast<GlassSurface>(std::clamp(ComboBox_GetCurSel(ctl), 0, kGlassSurfaceCount - 1));
            p_ = store.Surface(editing_);
            RefreshAll();
            DLOG(L"studio: editing surface %ls", GlassSurfaceName(editing_));
        }
        break;
    case kIdReset:
        ApplyLook(p_, store.UserDefault(editing_));
        RefreshAll();
        Push();
        break;
    case kIdSaveDefault:
        store.SetUserDefault(editing_, p_);
        store.Save();
        MessageBoxW(hwnd, (std::wstring(L"Current parameters saved as the default for ") + GlassSurfaceName(editing_) + L".").c_str(),
                    L"Glass Studio", MB_ICONINFORMATION);
        break;
    case kIdPaste: {
        std::wstring text;
        if (OpenClipboard(hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                    text = t;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        GlassParameters next;
        std::wstring err;
        if (text.empty() || !ParametersFromJsonText(json::ToUtf8(text), p_, next, err, true)) {
            MessageBoxW(hwnd, (L"Clipboard does not hold a usable Glass Lab JSON.\n" + err).c_str(), L"Glass Studio",
                        MB_ICONWARNING);
            break;
        }
        p_ = next;
        RefreshAll();
        Push();
        DLOG(L"studio: imported Glass Lab JSON from clipboard");
        break;
    }
    case kIdCopy: {
        const std::wstring text = json::FromUtf8(ParametersToJson(p_));
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                memcpy(GlobalLock(g), text.c_str(), bytes);
                GlobalUnlock(g);
                if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
            }
            CloseClipboard();
        }
        break;
    }
    case kIdPickTint: {
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = hwnd;
        cc.rgbResult = RGB(std::lround(p_.tintR * 255), std::lround(p_.tintG * 255), std::lround(p_.tintB * 255));
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (ChooseColorW(&cc)) {
            p_.tintR = GetRValue(cc.rgbResult) / 255.0f;
            p_.tintG = GetGValue(cc.rgbResult) / 255.0f;
            p_.tintB = GetBValue(cc.rgbResult) / 255.0f;
            RefreshAll();
            Push();
        }
        break;
    }
    default: break;
    }
}

bool Studio::PreTranslate(MSG& msg) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
        HWND focus = GetFocus();
        for (const Row& r : rows_) {
            if (r.edit == focus) {
                CommitEdit(focus);
                SendMessageW(focus, EM_SETSEL, 0, -1);
                return true;
            }
        }
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    if (msg.message == WM_MOUSEWHEEL) {
        // Scroll the panel unless the wheel is over a trackbar (which would change its value).
        POINT pt{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
        HWND under = WindowFromPoint(pt);
        wchar_t cls[32] = {};
        if (under) GetClassNameW(under, cls, ARRAYSIZE(cls));
        if (_wcsicmp(cls, TRACKBAR_CLASSW) != 0 && _wcsicmp(cls, WC_COMBOBOXW) != 0) {
            SendMessageW(panel_, WM_MOUSEWHEEL, msg.wParam, msg.lParam);
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK Studio::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* s = static_cast<Studio*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = h;
    }
    auto* s = reinterpret_cast<Studio*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return s ? s->OnMain(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Studio::PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* s = static_cast<Studio*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->panel_ = h;  // must be set before OnPanel forwards anything to DefWindowProc
    }
    auto* s = reinterpret_cast<Studio*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return s ? s->OnPanel(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Studio::OnMain(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (panel_) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        mm->ptMinTrackSize = {Px(520), Px(420)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        UpdateFont();
        auto apply = [this](HWND c) { SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return TRUE; };
        EnumChildWindows(hwnd, [](HWND c, LPARAM p) -> BOOL { return (*reinterpret_cast<decltype(apply)*>(p))(c); },
                         reinterpret_cast<LPARAM>(&apply));
        for (const Row& r : rows_)
            if (r.header) SendMessageW(r.label, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
        const RECT* rc = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w), reinterpret_cast<HWND>(l));
        return 0;
    case WM_TIMER:
        if (w == kTimerStats) UpdateStats();
        if (w == kTimerSave) {
            KillTimer(hwnd, kTimerSave);
            GlassSettingsStore::Get().Save();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerStats);
        KillTimer(hwnd, kTimerSave);
        GlassSettingsStore::Get().Save();
        if (font_) DeleteObject(font_);
        if (bold_) DeleteObject(bold_);
        font_ = bold_ = nullptr;
        DLOG(L"studio: closed; parameters kept and saved");
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

LRESULT Studio::OnPanel(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        LayoutPanel();
        return 0;
    case WM_HSCROLL:
        if (l && !updating_) OnTrack(reinterpret_cast<HWND>(l));
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w), reinterpret_cast<HWND>(l));
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_VSCROLL:
    case WM_MOUSEWHEEL: {
        RECT rc;
        GetClientRect(panel_, &rc);
        int pos = scroll_;
        if (m == WM_MOUSEWHEEL) {
            pos -= GET_WHEEL_DELTA_WPARAM(w) * Px(60) / WHEEL_DELTA;
        } else {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(panel_, SB_VERT, &si);
            switch (LOWORD(w)) {
            case SB_LINEUP: pos -= Px(30); break;
            case SB_LINEDOWN: pos += Px(30); break;
            case SB_PAGEUP: pos -= rc.bottom; break;
            case SB_PAGEDOWN: pos += rc.bottom; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            default: break;
            }
        }
        pos = std::clamp(pos, 0, std::max(0, content_ - static_cast<int>(rc.bottom)));
        if (pos != scroll_) {
            scroll_ = pos;
            LayoutPanel();
        }
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(panel_, m, w, l);
}

void StudioThread(HWND dock) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    {
        Studio studio;
        if (studio.Create(dock)) {
            g_hwnd.store(studio.hwnd);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (studio.PreTranslate(msg)) continue;
                if (IsDialogMessageW(studio.hwnd, &msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else {
            DLOG(L"studio: window creation failed %lu", GetLastError());
        }
    }
    g_hwnd.store(nullptr);
    if (dock) PostMessageW(dock, kMsgStudioClosed, 0, 0);
    g_running.store(false);
}

}  // namespace

void OpenGlassStudio(HWND dock) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HWND h = g_hwnd.load()) {
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return;
    }
    if (g_running.load()) return;  // still starting up or shutting down
    if (g_thread.joinable()) g_thread.join();
    g_running.store(true);
    g_thread = std::thread(StudioThread, dock);
}

void ShutdownGlassStudio() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HWND h = g_hwnd.load()) PostMessageW(h, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}

HWND GlassStudioWindow() { return g_hwnd.load(); }

}  // namespace dock
