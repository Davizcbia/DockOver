#include "control_center.h"
#include "color_output.h"
#include "ui_language.h"
#include "audio_output_icons.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cwctype>
#include "vblank.h"
#include "status_glyphs.h"
#include "battery_indicator.h"
#include "custom_icons.h"
#include "user_sound_icons.h"
#include "workshop.h"
#include "desktop_settings.h"
#include "theme_state.h"
#include "design_tokens.h"
#include "glass_params.h"
#include "motion.h"
#include "popup_dismiss_watch.h"
#include <winrt/Windows.UI.ViewManagement.h>

#include <windowsx.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "backdrop_wgc.h"
#include "desktop/duplication.h"
#include "glass_params.h"
#include "log.h"
#include "material.h"
#include "system_controls.h"

using Microsoft::WRL::ComPtr;
namespace ctl = liquid::controls;

namespace liquid::cc {
namespace {

constexpr wchar_t kClass[] = L"LiquidControlCenter";
constexpr UINT kMsgState = WM_APP + 120, kMsgVolume = WM_APP + 121, kMsgClose = WM_APP + 122, kMsgAnchor = WM_APP + 123, kMsgActionFailed = WM_APP + 124;
constexpr UINT_PTR kTimerRefresh = 1, kTimerHealth = 2, kTimerSnapshot = 3, kTimerWgcStart = 4, kTimerStatus = 5;
constexpr float kMargin = 18.0f, kGap = 12.0f, kCol = 140.0f, kWidth = 292.0f, kPill = 64.0f;
// Window padding around the panel. The blur behind the whole Control Center fades out over the sides and the bottom
// (macOS: a soft gradient, no visible edge), so the window extends 64 DIP there; the top touches the menu bar.
// The panel itself keeps its place: 11 DIP below the bar, 18 DIP from the screen edge (main Control Center).
constexpr float kPadX = 64.0f, kPadTop = 11.0f, kPadBottom = 64.0f, kBlurFeather = 34.0f, kBlurReach = 28.0f;
// Wi-Fi / Bluetooth menu, measured from the macOS 27 Wi-Fi menu (2x screenshot, 2026-11-10; values in pt = DIP):
// 308 wide, corner 15; title row 42 (text centre 21) with a 54x24 switch inset 14 from the right; hairline separators
// inset 14; section header centre 17 below a separator; 32-high rows inset 7 (hover radius 10) with a 26 circle at
// x 27 and the name at x 48.5; 5 below a section's last row comes the next separator; action rows 33 high.
constexpr float kMenuWidth = 308.0f, kMenuRadius = 15.0f, kMenuTitleRow = 42.0f, kMenuInset = 14.0f, kMenuRowInset = 7.0f;
constexpr float kMenuRow = 32.0f, kMenuAction = 33.0f, kMenuHeaderGap = 28.0f, kMenuAfterRows = 5.0f;
constexpr float kMenuSwitchW = 54.0f, kMenuSwitchH = 24.0f;
constexpr float kSliderThumbInset = 12.0f;  // half the resting slider thumb (24 DIP): its centre's distance from the track ends
std::mutex g_anchorMutex;
RECT g_anchor{};
bool g_openAbove = false;  // guarded by g_anchorMutex

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / f.QuadPart;
}

// ---------------------------------------------------------------------------------------------
// System worker: one MTA thread running blocking system calls. Volume/brightness writes coalesce
// (only the newest pending value is applied), so slider drags never queue up stale work.
struct OutputDevice {
    std::wstring id, name, alias;
    std::string icon;
    bool selected = false, hidden = false;
    const std::wstring& Label() const { return alias.empty() ? name : alias; }
};

constexpr int kOutputNameLimit = 80;

INT_PTR CALLBACK RenameOutputDialog(HWND window, UINT message, WPARAM w, LPARAM l) {
    constexpr int editId = 100;
    if (message == WM_INITDIALOG) {
        auto* name = reinterpret_cast<std::wstring*>(l);
        SetWindowLongPtrW(window, DWLP_USER, l);
        SetWindowTextW(window, L"Rename output");
        auto create = [&](DWORD ex, const wchar_t* kind, const wchar_t* label,
                          DWORD style, int id, RECT bounds) {
            MapDialogRect(window, &bounds);
            HWND control = CreateWindowExW(ex, kind, label, WS_CHILD | WS_VISIBLE | style,
                bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return control;
        };
        create(0, L"STATIC", L"Display name (leave blank to use the Windows name)", 0, -1, {8, 8, 210, 20});
        HWND edit = create(WS_EX_CLIENTEDGE, L"EDIT", name->c_str(),
            WS_TABSTOP | ES_AUTOHSCROLL, editId, {8, 24, 212, 39});
        create(0, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, {107, 49, 157, 64});
        create(0, L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, {162, 49, 212, 64});
        SendMessageW(edit, EM_SETLIMITTEXT, kOutputNameLimit, 0);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SendMessageW(window, DM_SETDEFID, IDOK, 0);
        SetFocus(edit);
        return FALSE;
    }
    if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(w)) {
    case IDOK: {
        wchar_t buffer[kOutputNameLimit + 1]{};
        GetDlgItemTextW(window, editId, buffer, ARRAYSIZE(buffer));
        std::wstring text(buffer);
        const auto first = text.find_first_not_of(L" \t");
        auto* name = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(window, DWLP_USER));
        *name = first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t") - first + 1);
        EndDialog(window, IDOK);
        return TRUE;
    }
    case IDCANCEL: EndDialog(window, IDCANCEL); return TRUE;
    }
    return FALSE;
}
std::vector<OutputDevice> QueryOutputs(){
    std::vector<OutputDevice> result;ComPtr<IMMDeviceEnumerator> e;ComPtr<IMMDeviceCollection> collection;ComPtr<IMMDevice> current;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e))) || FAILED(e->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection)))return result;
    std::wstring defaultId;LPWSTR raw=nullptr;if(SUCCEEDED(e->GetDefaultAudioEndpoint(eRender,eMultimedia,&current))&&SUCCEEDED(current->GetId(&raw))){defaultId=raw;CoTaskMemFree(raw);}
    UINT count=0;collection->GetCount(&count);
    for(UINT i=0;i<count;++i){ComPtr<IMMDevice> device;ComPtr<IPropertyStore> props;raw=nullptr;
      if(FAILED(collection->Item(i,&device))||FAILED(device->GetId(&raw)))continue;
      OutputDevice item;item.id=raw;CoTaskMemFree(raw);item.selected=item.id==defaultId;
      if(SUCCEEDED(device->OpenPropertyStore(STGM_READ,&props))){PROPVARIANT value;PropVariantInit(&value);if(SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)item.name=value.pwszVal;PropVariantClear(&value);}
      if(item.name.empty())item.name=L"Audio output";result.push_back(std::move(item));}
    return result;
}
// Windows does not expose default-endpoint selection through IMMDeviceEnumerator.
// This interface is version-dependent; report failure and offer Windows Settings if unavailable.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8") IAudioPolicyConfig : public IUnknown {
 virtual HRESULT STDMETHODCALLTYPE GetMixFormat()=0;virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat()=0;
 virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat()=0;virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat()=0;
 virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod()=0;virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod()=0;
 virtual HRESULT STDMETHODCALLTYPE GetShareMode()=0;virtual HRESULT STDMETHODCALLTYPE SetShareMode()=0;
 virtual HRESULT STDMETHODCALLTYPE GetPropertyValue()=0;virtual HRESULT STDMETHODCALLTYPE SetPropertyValue()=0;
 virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR,ERole)=0;virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility()=0;
};
bool SelectOutput(const std::wstring& id){
 const CLSID clsid={0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};ComPtr<IAudioPolicyConfig> policy;
 if(FAILED(CoCreateInstance(clsid,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&policy))))return false;
 const HRESULT a=policy->SetDefaultEndpoint(id.c_str(),eConsole),b=policy->SetDefaultEndpoint(id.c_str(),eMultimedia);
 return SUCCEEDED(a)&&SUCCEEDED(b);
}
class Worker {
public:
    explicit Worker(HWND notify, bool soundOnly) : notify_(notify), soundOnly_(soundOnly) {
        thread_ = std::thread([this] { Run(); });
    }
    ~Worker() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void RequestQuery(bool networks) {
        std::lock_guard<std::mutex> l(m_);
        queryPending_ = true;
        wantNetworks_ = wantNetworks_ || networks;
        cv_.notify_one();
    }
    void Post(std::function<void()> job) {
        std::lock_guard<std::mutex> l(m_);
        jobs_.push_back(std::move(job));
        cv_.notify_one();
    }
    void SetVolume(int v) {
        std::lock_guard<std::mutex> l(m_);
        volume_ = v;
        cv_.notify_one();
    }
    void SetBrightness(int v) {
        std::lock_guard<std::mutex> l(m_);
        brightness_ = v;
        cv_.notify_one();
    }
    ctl::State Latest() {
        std::lock_guard<std::mutex> l(m_);
        return state_;
    }
    std::vector<OutputDevice> LatestOutputs() { std::lock_guard<std::mutex> l(m_); return outputs_; }

private:
    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::function<void()> job;
            std::optional<int> vol, bri;
            bool query = false, networks = false;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !jobs_.empty() || queryPending_ || volume_ || brightness_; });
                if (stop_) break;
                vol.swap(volume_);
                bri.swap(brightness_);
                if (!jobs_.empty()) {
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                } else if (!vol && !bri && queryPending_) {
                    query = true;
                    networks = wantNetworks_;
                    queryPending_ = wantNetworks_ = false;
                }
            }
            if (vol) ctl::SetVolume(*vol);
            if (bri) ctl::SetBrightness(*bri);
            if (job) job();
            if (query) {
                ctl::State s = ctl::Query(networks, true);
                auto outputs=soundOnly_?QueryOutputs():std::vector<OutputDevice>{};
                {
                    std::lock_guard<std::mutex> l(m_);
                    if (!networks) s.networks = state_.networks;  // keep the last scan when not rescanning
                    state_ = std::move(s);
                    if(soundOnly_)outputs_=std::move(outputs);
                }
                PostMessageW(notify_, kMsgState, 0, 0);
            }
        }
        winrt::uninit_apartment();
    }
    HWND notify_;
    bool soundOnly_=false;
    std::vector<OutputDevice> outputs_;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::optional<int> volume_, brightness_;
    bool queryPending_ = false, wantNetworks_ = false, stop_ = false;
    ctl::State state_;
};

// ---------------------------------------------------------------------------------------------
// Brings the app behind the current media session forward. Its app id is a package app id ("Family!App": activated
// through the shell) or, for desktop apps, usually the program file name ("Spotify.exe"): its main window is restored
// and brought forward; a program running only in the tray is started again, which shows its window (single instance);
// otherwise the shell opens shell:AppsFolder\<id>.
bool ActivateMediaApp(const std::wstring& id) {
    if (id.find(L'!') != std::wstring::npos) {
        Microsoft::WRL::ComPtr<IApplicationActivationManager> manager; DWORD pid = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager))) &&
            SUCCEEDED(manager->ActivateApplication(id.c_str(), nullptr, AO_NONE, &pid))) return true;
    }
    std::wstring exe = id; for (auto& c : exe) c = wchar_t(towlower(c));
    if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0) {
        struct Find { std::wstring exe; HWND window = nullptr; std::wstring path; } find{exe};
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            auto* f = reinterpret_cast<Find*>(l);
            DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!process) return TRUE;
            wchar_t path[MAX_PATH]{}; DWORD size = MAX_PATH; const bool named = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE; CloseHandle(process);
            if (!named) return TRUE;
            std::wstring name = path; name = name.substr(name.find_last_of(L'\\') + 1); for (auto& c : name) c = wchar_t(towlower(c));
            if (name != f->exe) return TRUE;
            f->path = path;
            wchar_t title[8]{};
            if (IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetWindowTextW(w, title, 8) > 0 && !(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) { f->window = w; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&find));
        if (find.window) {
            if (IsIconic(find.window)) ShowWindow(find.window, SW_RESTORE);
            if (!SetForegroundWindow(find.window)) SwitchToThisWindow(find.window, TRUE);
            return true;
        }
        if (!find.path.empty() && reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", find.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32) return true;
    }
    const std::wstring target = L"shell:AppsFolder\\" + id;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", target.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

enum class Page { Main, Wifi, Bluetooth };
enum class Hit {
    None, WifiToggle, WifiBody, BtToggle, BtBody, Airplane, Focus, Battery, Stage, MediaPrev, MediaPlay, MediaNext, Media, MediaCover,
    DisplayTitle, DisplaySlider, SoundTitle, SoundSlider, Mute, Back, PageSwitch, Row, OtherNetworks, Settings, ManageOutputs
};

struct Module {
    D2D1_RECT_F r{};
    float radius = 0;
};

std::wstring g_settingsPath;
std::wstring g_workshopRoot;
bool g_visualCaptureTest=false;
class Panel {
public:
    int Run(RECT anchor, HWND notify, UINT notifyMessage, bool soundOnly, Page initialPage);
    HWND hwnd = nullptr;

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    HRESULT InitGraphics();
    HRESULT ResizeTarget();
    void Layout();
    void Place();
    void Frame();
    void Render();
    bool RefreshTheme();
    D2D1::ColorF Ink(float a=1.f) const {const unsigned c=palette_.primary;return D2D1::ColorF(((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,a);}
    D2D1::ColorF Surface(float a=1.f) const {const unsigned c=palette_.surface;return D2D1::ColorF(((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,a);}
    liquid::design::Palette palette_=liquid::design::Dark;
    bool forceOpaque_=false, contrastMode_=false;
    bool mediaArtworkBackground_=false;
    ComPtr<ID2D1Bitmap1> mediaArtwork_; uint64_t mediaArtworkHash_=0;
    int backdropBlur_=60;  // Settings > Control Center > Background blur (0..100 %)
    // Blur radius (blurSpread) behind the whole panel: the Control Center uses docky Settings; menu bar popups (Wi-Fi,
    // Bluetooth, sound) use Glass Studio > Menu bar popups > Background blur.
    bool MenuBarPopup() const { return directDetail_ || soundOnly_; }
    // Module glass: menu bar popups use their own category, the Control Center its own.
    dock::GlassRole ModuleRole() const { return MenuBarPopup() ? dock::GlassRole::PopupModule : dock::GlassRole::ControlModule; }
    float BackdropSpread() const {
        auto& store = dock::GlassSettingsStore::Get(); store.ReloadIfChanged();
        if (MenuBarPopup()) return store.Surface(dock::GlassSurface::MenuBarPanels).backdropBlur;
        // Control Center: Glass Studio's Control Center slider, scaled by docky Settings (60 % = as tuned), so either control works.
        return store.Surface(dock::GlassSurface::ControlCenter).backdropBlur * static_cast<float>(backdropBlur_) / 60.0f;
    }
    bool DrawGlass(const Module& m, float opacity, float soften = 0.0f, dock::GlassRole role = dock::GlassRole::ControlModule, float edge = 1.0f);
    bool RenderPlate(D2D1_RECT_F r, dock::GlassParameters& gp, dock::BackdropFrame& frame);
    D2D1_RECT_F SwitchThumbRect(D2D1_RECT_F track) const;
    bool SwitchStateOn() const;
    bool UpdateSwitchMotion(double dt);
    void DrawSwitchLens(D2D1_RECT_F thumb, float opacity);
    void DrawLensHighlights(D2D1_RECT_F thumb, float opacity, float k, D2D1_POINT_2F pivot);
    docky::motion::Spring switchOn_, switchPress_, switchColor_;  // thumb travel, press swell, track colour (0 off .. 1 on)
    bool switchInit_ = false, switchDragging_ = false;
    float switchPressX_ = 0, switchStartPos_ = 0; double switchDragPos_ = 0;
    int switchWant_ = -1; double switchWantUntil_ = 0;  // optimistic state while Windows applies the change
    ComPtr<ID3D11Texture2D> lensScene_; UINT lensW_ = 0, lensH_ = 0; unsigned long long lensFrame_ = 0;
    D2D1_RECT_F lensThumb_{}; float lensOpacity_ = 0;  // set by the D2D pass for the post-pass glass lens
    ComPtr<ID2D1Bitmap1> contentLayer_;  // offscreen D2D content while it is defocused during opening
    ComPtr<ID2D1Effect> defocusFx_;
    void DrawModuleBase(const Module& m, float a, bool hover, bool press);
    void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, float alpha, D2D1::ColorF c = D2D1::ColorF(1, 1, 1));
    void CenteredText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, float alpha, D2D1::ColorF c = D2D1::ColorF(1, 1, 1));
    void Glyph(wchar_t g, float size, D2D1_POINT_2F center, float alpha, D2D1::ColorF c = D2D1::ColorF(1, 1, 1), bool fixedColor = false);
    void ToggleCircle(D2D1_POINT_2F c, wchar_t g, bool on, bool enabled, float a, float r = 19.0f);
    struct IconMask {ComPtr<ID2D1Bitmap1> bitmap;D2D1_RECT_F source{};float offsetX=0,offsetY=0;
      UINT width=0;std::vector<uint32_t> pixels;mutable std::map<std::uint64_t,ComPtr<ID2D1Bitmap1>> scaled;};
    bool MakeIconMask(const std::vector<uint32_t>& pixels,UINT w,UINT h,IconMask& result);
    void LoadStatusIcons();
    void DrawMask(const IconMask& icon,D2D1_POINT_2F center,float size,D2D1::ColorF color,float alpha);
    void DrawWifi(D2D1_POINT_2F center,float size,const char* key,D2D1::ColorF color,float alpha);
    void DrawSound(D2D1_POINT_2F center,float size,D2D1::ColorF color,float alpha);
    void DrawBluetooth(D2D1_POINT_2F center,float size,D2D1::ColorF color,float alpha);
    void DrawBattery(D2D1_POINT_2F center,float alpha);
    void DetailCircle(D2D1_POINT_2F c, wchar_t g, bool active, float a);
    void Slider(D2D1_RECT_F track, float value, bool active, float a, Hit which);
    // Glass Lab slider thumb: a white 24x18 pill that, pressed, grows (Sliders & switches category) into the glass lens.
    docky::motion::Spring sliderPress_; Hit sliderPressHit_ = Hit::None; bool sliderInit_ = false;
    bool UpdateSliderMotion(double dt);
    void Switch(D2D1_RECT_F r, bool on, bool enabled, float a);
    Hit HitTest(float x, float y, int* row);
    void Click(Hit h, int row);
    void SliderAt(Hit which, float x);
    void Close(const wchar_t* why);
    void NotifyBar();
    void SaveSnapshot();
    std::wstring Describe(Hit h) const;

    RECT anchor_{};
    HWND notify_ = nullptr, previous_ = nullptr;
    UINT notifyMessage_ = 0;
    float scale_ = 1, W_ = 0, H_ = 0;
    UINT pxW_ = 0, pxH_ = 0;
    RECT windowRect_{};
    std::unique_ptr<Worker> worker_;
    ctl::State st_;
    bool haveState_ = false;
    ctl::VolumeWatcher volumeWatch_;

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIDevice> dxgi_;
    ComPtr<IDXGISwapChain1> swap_;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv_;
    docky::color::ColorOutput colors_;
    ComPtr<ID3D11RenderTargetView> outputRtv_;
    bool floatingScene_ = false, scRgb_ = false;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> title_, sub_, small_, rowFmt_, big_, menuTitle_, menuHead_, menuRow_;
    std::wstring iconFamily_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<std::string,IconMask> statusIcons_;IconMask fallbackWifi_,fallbackSound_;
    int fallbackSoundState_=-1,batteryLevel_=-999;bool batteryCharging_=false;
    ComPtr<ID2D1Bitmap1> batteryBitmap_;
    std::unique_ptr<dock::IPlateMaterial> glass_;
    bool glassOk_ = false;
    dock::WgcBackdropSource backdrop_;
    bool lastGlass_ = false;
    float glassFade_ = 0.f;
    double tBackdropReady_ = 0, tFirstGlass_ = 0, tFirstFallback_ = 0, tAnimDone_ = 0;

    bool soundOnly_=false, directDetail_=false, above_=false;  // above_: opened from a Dock cell, content sits above the anchor
    std::vector<OutputDevice> outputs_;
    std::vector<liquid::settings::AudioOutputPreference> outputPreferences_;
    std::vector<OutputDevice> activeOutputs_;
    std::wstring outputSettingsPath_;
    docky::motion::Spring outputScroll_;
    D2D1_RECT_F outputViewport_{}, manageOutputsRow_{};
    bool managingOutputs_ = false, outputEditorOpen_ = false, closeAfterOutputEditor_ = false;
    static constexpr float kOutputRowHeight = 32.0f;
    double OutputScrollMax() const;
    D2D1_RECT_F OutputRow(size_t index) const;
    void RefreshOutputs();
    void EditOutput(size_t index);
    Page page_ = Page::Main;
    std::vector<Module> modules_;  // glass plates of the current page
    // Main-page geometry
    Module wifi_, bt_, media_, airplane_, focus_, stage_, battery_, display_, sound_;
    HWND StageDock() const;
    D2D1_RECT_F displayTrack_{}, soundTrack_{}, muteBtn_{}, prevBtn_{}, playBtn_{}, nextBtn_{};
    // Detail-page geometry
    Module detail_;
    D2D1_RECT_F switch_{}, back_{}, otherNetworks_{}, settingsRow_{};
    std::vector<D2D1_RECT_F> rows_;
    std::vector<int> rowItem_;                                  // detail rows -> index into st_.networks / st_.btDevices (-1 = placeholder)
    std::vector<std::pair<float, std::wstring>> menuHeaders_;   // section header centre y + text
    std::vector<float> menuSeparators_;                         // hairline y positions
    float footerY_ = 0;

    // Interaction state
    float mx_ = -1, my_ = -1;
    Hit hover_ = Hit::None, pressed_ = Hit::None;
    // V0.15 keyboard focus: Tab / Shift+Tab cycle the interactive targets of the current page, Enter /
    // Space activate through the same Click() path as the mouse, Left / Right step a focused slider.
    Hit kbFocus_ = Hit::None;
    int kbRow_ = -1;
    std::vector<std::pair<Hit, int>> FocusOrder() const;
    bool FocusRect(Hit h, int row, D2D1_RECT_F& out) const;
    void StepSlider(Hit h, int delta);
    int hoverRow_ = -1, pressedRow_ = -1;
    bool dragging_ = false;
    int dragVolume_ = -1, dragBrightness_ = -1;
    double lastUserVolume_ = 0, lastUserBrightness_ = 0;
    std::wstring status_;  // transient message (e.g. "Connecting…")
    double statusUntil_ = 0;

    // Animation
    float openT_ = 0, openTarget_ = 1, transitionFrom_ = 0;
    docky::motion::Spring open_;  // 0 closed .. 1 open
    // Critically damped: no overshoot on a panel that just appeared. 95% settle ~0.75 x response.
    // Reference: open ~0.12 s from first visible frame to settled, close ~0.16 s.
    inline static const docky::motion::SpringSpec kOpenSpring = docky::motion::tokens::PopoverOpen;
    inline static const docky::motion::SpringSpec kCloseSpring = docky::motion::tokens::PopoverClose;
    static constexpr float kOpenScaleFrom = 0.02f;  // reference shows < 2% scale; most of the "larger" look is defocus
    static constexpr float kDefocusMax = 6.0f;      // DIP of content/outline softening at the first visible frame
    double transitionAt_ = 0;
    bool closing_ = false, animating_ = true, needsFrame_ = true, reducedMotion_ = false;
    double lastT_ = 0;
    liquid::popup::DismissWatch dismissal_;
    // Open-path timing (ms since Run start); logged once as 'cc: timing'.
    bool wgcStarted_ = false, volumeStarted_ = false;
    double t0_ = 0, tWindow_ = 0, tDevice_ = 0, tSwapD2D_ = 0, tDcomp_ = 0, tText_ = 0, tGlass_ = 0, tWgc_ = 0, tShown_ = 0, tFirstFrame_ = 0, tFirstPresent_ = 0, tFirstCapture_ = 0;
    double prevFrame_ = 0, maxGap_ = 0; int frames_ = 0; bool timingLogged_ = false;
    double Ms() const { return (Now() - t0_) * 1000.0; }
    std::wstring snapshotDir_;
    int snapshotIndex_ = 0;
    bool snapshotRequested_ = false;
};

// ---------------------------------------------------------------------------------------------
// V0.7: one D3D11 device for every Control Center opening (only one panel exists at a time). Recreated
// if the device was removed. Everything else (swap chain, D2D, DComp, glass, WGC) stays per opening.
std::mutex g_deviceMutex;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
HRESULT SharedDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if (g_device && g_device->GetDeviceRemovedReason() != S_OK) { DLOG(L"cc: shared device removed; recreating"); g_device.Reset(); g_context.Reset(); }
    if (!g_device) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                             D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
        if (FAILED(hr)) { g_device.Reset(); g_context.Reset(); return hr; }
    }
    device = g_device; context = g_context; return S_OK;
}

HRESULT Panel::InitGraphics() {
    HRESULT hr = SharedDevice(d3d_, ctx_);
    if (FAILED(hr)) return hr;
    tDevice_ = Ms();
    d3d_.As(&dxgi_);
    ComPtr<IDXGIFactory2> f;
    if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return hr;
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
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                      reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return hr;
    if (FAILED(hr = factory_->CreateDevice(dxgi_.Get(), &d2dDevice_))) return hr;
    if (FAILED(hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return hr;
    floatingScene_ = colors_.Init(d3d_.Get()) && colors_.Bind(dc_.Get(), pxW_, pxH_);
    if (floatingScene_) {
        sd.Format = docky::color::SceneFormat;
        scRgb_ = SUCCEEDED(f->CreateSwapChainForComposition(dxgi_.Get(), &sd, nullptr, &swap_)) &&
                 docky::color::EnableScRgb(swap_.Get());
    }
    if (!scRgb_) {
        swap_.Reset();
        sd.Format = docky::color::SdrFormat;
        if (FAILED(hr = f->CreateSwapChainForComposition(dxgi_.Get(), &sd, nullptr, &swap_))) return hr;
    }
    if (floatingScene_ && dc_->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT)) {
        D2D1_RENDERING_CONTROLS controls{};
        dc_->GetRenderingControls(&controls);
        controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
        dc_->SetRenderingControls(&controls);
    }
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }
    if (FAILED(hr = ResizeTarget())) return hr;
    LoadStatusIcons();
    tSwapD2D_ = Ms();
    if (FAILED(hr = DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_)))) return hr;
    if (FAILED(hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) return hr;
    dcomp_->CreateVisual(&visual_);
    visual_->SetContent(swap_.Get());
    dcompTarget_->SetRoot(visual_.Get());
    dcomp_->Commit();
    tDcomp_ = Ms();
    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return hr;
    auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
        dwrite_->CreateTextFormat(liquid::design::type::Family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"", &out);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(out.Get(), &ell))) {
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out->SetTrimming(&t, ell.Get());
        }
    };
    namespace type = liquid::design::type;
    fmt(type::Emphasis, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_);
    fmt(type::Secondary, DWRITE_FONT_WEIGHT_NORMAL, sub_);
    fmt(type::Caption, DWRITE_FONT_WEIGHT_NORMAL, small_);
    fmt(type::Label, DWRITE_FONT_WEIGHT_NORMAL, rowFmt_);
    fmt(type::Field, DWRITE_FONT_WEIGHT_SEMI_BOLD, big_);
    // macOS menu type at the measured cap heights: 13 pt bold title, 12.5 pt bold section headers, 13 pt rows.
    fmt(13.5f, DWRITE_FONT_WEIGHT_BOLD, menuTitle_);
    fmt(12.5f, DWRITE_FONT_WEIGHT_BOLD, menuHead_);
    fmt(13.5f, DWRITE_FONT_WEIGHT_NORMAL, menuRow_);
    // Icon font: Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets otherwise (same code points).
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(&fonts);
    UINT32 idx = 0;
    BOOL exists = FALSE;
    iconFamily_ = L"Segoe MDL2 Assets";
    if (fonts && SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists) iconFamily_ = L"Segoe Fluent Icons";
    dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    tText_ = Ms();
    glass_ = dock::CreateLiquidGlassMaterial();
    glassOk_ = glass_ && SUCCEEDED(glass_->CreateGpuResources(d3d_.Get()));
    tGlass_ = Ms();
    backdrop_.SetDevice(d3d_.Get());
    backdrop_.SetExcludedWindow(hwnd);
    // Capture startup may block for tens of milliseconds, so it normally starts after the first fallback frames
    // (kTimerWgcStart). When this process already duplicates the monitor (the Dock's live glass), joining it costs
    // nothing: start now, so the first visible frame is glass ~80 ms sooner (the reveal waits for it).
    if (!forceOpaque_ && !g_visualCaptureTest && docky::desk::SharedDesktop::ForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).Active()) {
        backdrop_.Request(true, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        tWgc_ = Ms(); wgcStarted_ = true;
        DLOG(L"cc: capture joined the running desktop duplication at ms=%.1f", tWgc_);
    }
    return S_OK;
}

HRESULT Panel::ResizeTarget() {
    dc_->SetTarget(nullptr);
    target_.Reset();
    rtv_.Reset();
    outputRtv_.Reset();
    HRESULT hr = S_OK;
    DXGI_SWAP_CHAIN_DESC1 d{};
    swap_->GetDesc1(&d);
    if (d.Width != pxW_ || d.Height != pxH_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
        if (FAILED(hr = swap_->ResizeBuffers(2, pxW_, pxH_, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
            return hr;
    }
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return hr;
    const float dpi = 96.f * scale_;
    if (floatingScene_) {
        if (!colors_.Bind(dc_.Get(), pxW_, pxH_)) return E_FAIL;
        if (FAILED(hr = d3d_->CreateRenderTargetView(buffer.Get(), nullptr, &outputRtv_))) return hr;
        target_ = colors_.Bitmap();
        buffer = colors_.Texture();
    } else {
        ComPtr<IDXGISurface> surface;
        if (FAILED(hr = buffer.As(&surface))) return hr;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(docky::color::SdrFormat, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return hr;
    }
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    return d3d_->CreateRenderTargetView(buffer.Get(), nullptr, &rtv_);
}

// Geometry follows the macOS Control Center module grid: 140 pt columns, 64 pt pills, 12 pt gaps,
// large modules 140x140, full-width slider modules 292x64, continuous rounded corners.
double Panel::OutputScrollMax() const {
    return outputs_.size() > 8 ? (outputs_.size() - 8) * kOutputRowHeight : 0.0;
}

D2D1_RECT_F Panel::OutputRow(size_t index) const {
    const float top = outputViewport_.top + index * kOutputRowHeight
        - static_cast<float>(std::clamp(outputScroll_.Value(), 0.0, OutputScrollMax()));
    return D2D1::RectF(outputViewport_.left, top, outputViewport_.right, top + kOutputRowHeight);
}

void Panel::RefreshOutputs() {
    auto ordered = activeOutputs_;
    auto preference = [&](const std::wstring& id) {
        return std::find_if(outputPreferences_.begin(), outputPreferences_.end(),
            [&](const auto& entry) { return entry.id == id; });
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
        return preference(a.id) < preference(b.id);
    });
    for (auto& device : ordered) {
        const auto entry = preference(device.id);
        if (entry == outputPreferences_.end()) continue;
        device.alias = entry->name;
        device.icon = entry->icon;
        device.hidden = entry->hidden;
    }
    if (!managingOutputs_)
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [](const auto& device) {
            return device.hidden;
        }), ordered.end());
    const bool sameRows = ordered.size() == outputs_.size() &&
        std::equal(ordered.begin(), ordered.end(), outputs_.begin(),
            [](const auto& a, const auto& b) { return a.id == b.id; });
    const bool sizeChanged = ordered.size() != outputs_.size();
    outputs_ = std::move(ordered);
    if (!sameRows) {
        if (pressed_ == Hit::Row) pressed_ = Hit::None;
        if (kbFocus_ == Hit::Row) { kbFocus_ = Hit::None; kbRow_ = -1; }
        if (hover_ == Hit::Row) { hover_ = Hit::None; hoverRow_ = -1; }
    }
    const double maxScroll = OutputScrollMax();
    if (outputScroll_.Value() > maxScroll) outputScroll_.SnapTo(maxScroll);
    else if (outputScroll_.Target() > maxScroll) outputScroll_.SetTarget(maxScroll);
    if (sizeChanged && haveState_) {
        Layout();
        Place();
        ResizeTarget();
    }
    needsFrame_ = true;
}

void Panel::EditOutput(size_t index) {
    if (outputEditorOpen_ || index >= outputs_.size()) return;
    const auto device = outputs_[index];
    enum : UINT { Rename = 1, Hide, MoveUp, MoveDown, FirstIcon = 100 };
    HMENU menu = CreatePopupMenu();
    HMENU icons = CreatePopupMenu();
    if (!menu || !icons) {
        if (menu) DestroyMenu(menu);
        if (icons) DestroyMenu(icons);
        return;
    }
    auto add = [](HMENU target, UINT id, const wchar_t* label, UINT flags = 0) {
        AppendMenuW(target, MF_STRING | flags, id, label);
    };
    add(menu, Rename, L"Rename…");
    add(menu, Hide, device.hidden ? L"Show in output list" : L"Hide from output list");
    add(menu, MoveUp, L"Move up", index > 0 ? MF_ENABLED : MF_GRAYED);
    add(menu, MoveDown, L"Move down", index + 1 < outputs_.size() ? MF_ENABLED : MF_GRAYED);
    const auto& presets = docky::audio_output::presets;
    for (UINT i = 0; i < ARRAYSIZE(presets); ++i)
        add(icons, FirstIcon + i, presets[i].name, device.icon == presets[i].key ? MF_CHECKED : 0);
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(icons), L"Icon")) DestroyMenu(icons);
    const auto bounds = OutputRow(index);
    POINT anchor{static_cast<LONG>((bounds.right - 8) * scale_),
                 static_cast<LONG>(std::min(bounds.bottom, outputViewport_.bottom) * scale_)};
    ClientToScreen(hwnd, &anchor);
    outputEditorOpen_ = true;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
        anchor.x, anchor.y, hwnd, nullptr);
    DestroyMenu(menu);

    auto next = outputPreferences_;
    auto ensure = [&](const std::wstring& id) {
        auto found = std::find_if(next.begin(), next.end(), [&](const auto& entry) { return entry.id == id; });
        if (found != next.end()) return static_cast<size_t>(found - next.begin());
        next.push_back({id, {}, {}, false});
        return next.size() - 1;
    };
    if (command) {
        // Preserve the current order when adding preferences for a previously unconfigured endpoint.
        for (const auto& output : outputs_) ensure(output.id);
    }
    bool edited = false;
    if (command == Rename && !closeAfterOutputEditor_) {
        struct DialogTemplate {
            DLGTEMPLATE dialog;
            WORD menu, windowClass, title;
        };
        alignas(DWORD) DialogTemplate layout{};
        layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
        layout.dialog.cx = 220;
        layout.dialog.cy = 73;
        auto alias = device.alias;
        if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, hwnd, RenameOutputDialog,
                                    reinterpret_cast<LPARAM>(&alias)) == IDOK) {
            next[ensure(device.id)].name = std::move(alias);
            edited = true;
        }
    } else if (command == Hide) {
        next[ensure(device.id)].hidden = !device.hidden;
        edited = true;
    } else if (command == MoveUp || command == MoveDown) {
        const auto current = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& row) { return row.id == device.id; });
        if (current != outputs_.end()) {
            const auto row = static_cast<size_t>(current - outputs_.begin());
            if ((command == MoveUp && row > 0) || (command == MoveDown && row + 1 < outputs_.size())) {
                const size_t from = ensure(device.id);
                const size_t to = ensure(outputs_[command == MoveUp ? row - 1 : row + 1].id);
                std::iter_swap(next.begin() + from, next.begin() + to);
                edited = true;
            }
        }
    } else if (command >= FirstIcon && command < FirstIcon + ARRAYSIZE(presets)) {
        next[ensure(device.id)].icon = presets[command - FirstIcon].key;
        edited = true;
    }
    outputEditorOpen_ = false;
    if (edited) {
        if (liquid::settings::WriteAudioOutputs(outputSettingsPath_, next) == liquid::settings::SaveStatus::Saved) {
            outputPreferences_ = std::move(next);
            RefreshOutputs();
        } else {
            status_ = L"Could not save output preferences.";
            statusUntil_ = Now() + 4.0;
            SetTimer(hwnd, kTimerStatus, 4000, nullptr);
        }
    }
    needsFrame_ = true;
    if (closeAfterOutputEditor_) Close(L"toggle");
}

HWND Panel::StageDock() const {
    wchar_t name[48]{};
    if (GetClassNameW(notify_, name, ARRAYSIZE(name)) && !wcscmp(name, kStageDockWindow))
        return notify_;
    return FindWindowW(kStageDockWindow, nullptr);
}

void Panel::Layout() {
    modules_.clear();
    rows_.clear();
    const float x0 = kPadX, x1 = kPadX + kCol + kGap;
    float y = kPadTop;
    if (soundOnly_) {
        const size_t n = std::clamp<size_t>(outputs_.size(), 1, 8);
        const float listTop = y + 92, footer = listTop + n * kOutputRowHeight;
        outputViewport_ = D2D1::RectF(x0 + 8, listTop, x0 + kWidth - 8, footer);
        manageOutputsRow_ = D2D1::RectF(x0 + kWidth - 84, y + 66, x0 + kWidth - 8, y + 91);
        sound_ = {D2D1::RectF(x0,y,x0+kWidth,footer+40),16.0f};
        soundTrack_=D2D1::RectF(x0+36,y+43,x0+kWidth-40,y+49);
        muteBtn_=D2D1::RectF(x0+kWidth-38,y+32,x0+kWidth-8,y+60);
        settingsRow_=D2D1::RectF(x0+8,footer+4,x0+kWidth-8,footer+36);
        modules_.push_back(sound_);y=sound_.r.bottom;
    } else if (page_ == Page::Main) {
        wifi_ = {D2D1::RectF(x0, y, x0 + kCol, y + kPill), kPill * 0.5f};
        media_ = {D2D1::RectF(x1, y, x1 + kCol, y + 2 * kPill + kGap), 30.0f};
        y += kPill + kGap;
        bt_ = {D2D1::RectF(x0, y, x0 + kCol, y + kPill), kPill * 0.5f};
        y += kPill + kGap;
        constexpr float quickGap = 7.0f, diameter = (kCol - 2 * quickGap) / 3;
        const auto quick = [&](int column) {
            const float left = x0 + column * (diameter + quickGap), top = y + (kPill - diameter) * .5f;
            return Module{D2D1::RectF(left, top, left + diameter, top + diameter), diameter * .5f};
        };
        airplane_ = quick(0);
        focus_ = quick(1);
        stage_ = quick(2);
        battery_ = {D2D1::RectF(x1, y, x1 + kCol, y + kPill), kPill * 0.5f};
        y += kPill + kGap;
        modules_ = {wifi_, media_, bt_, airplane_, focus_, stage_, battery_};
        if (st_.brightness) {
            display_ = {D2D1::RectF(x0, y, x0 + kWidth, y + kPill), 26.0f};
            displayTrack_ = D2D1::RectF(x0 + 40, y + 42, x0 + kWidth - 40, y + 46);
            modules_.push_back(display_);
            y += kPill + kGap;
        } else {
            display_ = {};
        }
        sound_ = {D2D1::RectF(x0, y, x0 + kWidth, y + kPill), 26.0f};
        soundTrack_ = D2D1::RectF(x0 + 40, y + 42, x0 + kWidth - 76, y + 46);
        muteBtn_ = D2D1::RectF(x0 + kWidth - 52, y + 18, x0 + kWidth - 16, y + 54);
        modules_.push_back(sound_);
        y += kPill;
        const float mb = media_.r.bottom - 26;
        prevBtn_ = D2D1::RectF(x1 + 14, mb - 16, x1 + 50, mb + 16);
        playBtn_ = D2D1::RectF(x1 + 52, mb - 18, x1 + 88, mb + 18);
        nextBtn_ = D2D1::RectF(x1 + 90, mb - 16, x1 + 126, mb + 16);
    } else {
        const bool wifi = page_ == Page::Wifi;
        const float W = kMenuWidth, y0 = y;
        menuHeaders_.clear(); menuSeparators_.clear(); rowItem_.clear();
        back_ = D2D1::RectF(x0 + 6, y0 + 7, x0 + 34, y0 + 35);
        switch_ = D2D1::RectF(x0 + W - kMenuInset - kMenuSwitchW, y0 + (kMenuTitleRow - kMenuSwitchH) * 0.5f - 1.0f,
                              x0 + W - kMenuInset, y0 + (kMenuTitleRow + kMenuSwitchH) * 0.5f - 1.0f);
        float sep = y0 + kMenuTitleRow;
        menuSeparators_.push_back(sep);
        auto section = [&](const std::wstring& header, const std::vector<int>& items) {
            menuHeaders_.push_back({sep + 17.0f, header});
            float ry = sep + kMenuHeaderGap;
            for (int item : items) { rows_.push_back(D2D1::RectF(x0 + kMenuRowInset, ry, x0 + W - kMenuRowInset, ry + kMenuRow)); rowItem_.push_back(item); ry += kMenuRow; }
            sep = ry + kMenuAfterRows;
            menuSeparators_.push_back(sep);
        };
        const bool on = wifi ? st_.wifiRadio == ctl::Radio::On : st_.btRadio == ctl::Radio::On;
        if (wifi) {
            if (on) {
                std::vector<int> known, other;
                for (int i = 0; i < static_cast<int>(st_.networks.size()); ++i)
                    (st_.networks[i].connected || !st_.networks[i].profile.empty() ? known : other).push_back(i);
                // Windows lists a saved network twice (with and without its profile); macOS shows it once, as known.
                other.erase(std::remove_if(other.begin(), other.end(), [&](int o) {
                    return std::any_of(known.begin(), known.end(), [&](int kn) { return st_.networks[kn].ssid == st_.networks[o].ssid; });
                }), other.end());
                size_t budget = 8;
                if (known.size() > budget) known.resize(budget); budget -= known.size();
                if (other.size() > budget) other.resize(budget);
                if (!known.empty()) section(known.size() == 1 ? L"Known Network" : L"Known Networks", known);
                if (!other.empty()) section(L"Other Networks", other);
                if (known.empty() && other.empty()) section(L"Networks", {-1});
            }
        } else if (on) {
            // Every paired device (macOS Bluetooth menu): connected ones highlighted, the rest dimmed; no header.
            std::vector<int> devices;
            for (int i = 0; i < static_cast<int>(std::min<size_t>(st_.btPaired.size(), 12)); ++i) devices.push_back(i);
            if (devices.empty()) devices.push_back(-1);
            float ry = sep + 6.0f;
            for (int item : devices) { rows_.push_back(D2D1::RectF(x0 + kMenuRowInset, ry, x0 + W - kMenuRowInset, ry + kMenuRow)); rowItem_.push_back(item); ry += kMenuRow; }
            sep = ry + kMenuAfterRows;
            menuSeparators_.push_back(sep);
        }
        if (wifi && on) {
            otherNetworks_ = D2D1::RectF(x0 + kMenuRowInset, sep + 1.0f, x0 + W - kMenuRowInset, sep + kMenuAction);
            sep += kMenuAction;
            menuSeparators_.push_back(sep);
        } else otherNetworks_ = D2D1::RectF(0, 0, 0, 0);
        settingsRow_ = D2D1::RectF(x0 + kMenuRowInset, sep + 1.0f, x0 + W - kMenuRowInset, sep + kMenuAction + 1.0f);
        detail_ = {D2D1::RectF(x0, y0, x0 + W, sep + kMenuAction + 2.0f), kMenuRadius};
        modules_.push_back(detail_);
        y = detail_.r.bottom;
    }
    footerY_ = y + 6;
    W_ = kPadX * 2 + ((page_ != Page::Main && !soundOnly_) ? kMenuWidth : kWidth);
    H_ = y + kPadBottom;  // blur fade room (no footer caption)
}

void Panel::Place() {
    pxW_ = static_cast<UINT>(std::ceil(W_ * scale_));
    pxH_ = static_cast<UINT>(std::ceil(H_ * scale_));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &mi);
    // Align the panel window with the right edge of the monitor containing its anchor.
    // Panel right edge: the anchor's right (popups) or 18 DIP from the screen edge (Control Center); the side padding may
    // extend past the screen edge (only blur fade lives there).
    int x = (directDetail_||soundOnly_) ? static_cast<int>(anchor_.right) - static_cast<int>(pxW_) + static_cast<int>(kPadX*scale_)
                                        : static_cast<int>(mi.rcMonitor.right) - static_cast<int>(pxW_) + static_cast<int>((kPadX - kMargin)*scale_);
    int y = anchor_.bottom;
    if (above_) {  // Dock cell: the content's bottom 8 DIP above the anchor, centred on it, kept on the monitor
        const int contentW = static_cast<int>((W_ - 2 * kPadX) * scale_), padX = static_cast<int>(kPadX * scale_), margin = static_cast<int>(kMargin * scale_);
        int left = (anchor_.left + anchor_.right) / 2 - contentW / 2;
        left = std::clamp(left, static_cast<int>(mi.rcMonitor.left) + margin, std::max(static_cast<int>(mi.rcMonitor.left) + margin, static_cast<int>(mi.rcMonitor.right) - margin - contentW));
        x = left - padX;
        y = anchor_.top - static_cast<int>(8.0f * scale_) - static_cast<int>((H_ - kPadBottom) * scale_);
        y = std::max(y, static_cast<int>(mi.rcMonitor.top) - static_cast<int>(kPadTop * scale_));
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, static_cast<int>(pxW_), static_cast<int>(pxH_), SWP_NOACTIVATE);
        GetWindowRect(hwnd, &windowRect_);
        return;
    }
    x = std::max(x, static_cast<int>(mi.rcMonitor.left) - static_cast<int>((kPadX - kMargin)*scale_));
    y = std::clamp(y, static_cast<int>(mi.rcMonitor.top), std::max(static_cast<int>(mi.rcMonitor.top), static_cast<int>(mi.rcMonitor.bottom - pxH_)));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, static_cast<int>(pxW_), static_cast<int>(pxH_), SWP_NOACTIVATE);
    GetWindowRect(hwnd, &windowRect_);
    // Hit-target centres in screen pixels for the acceptance script (verify_control_center.ps1).
    auto sx = [&](float v) { return static_cast<int>(windowRect_.left + v * scale_); };
    auto sy = [&](float v) { return static_cast<int>(windowRect_.top + v * scale_); };
    if (page_ == Page::Main)
        DLOG(L"cc: targets page=main sound=%d,%d,%d mute=%d,%d wifi=%d,%d display=%d %d,%d,%d", sx(soundTrack_.left), sx(soundTrack_.right),
             sy((soundTrack_.top + soundTrack_.bottom) * 0.5f), sx((muteBtn_.left + muteBtn_.right) * 0.5f), sy((muteBtn_.top + muteBtn_.bottom) * 0.5f),
             sx(wifi_.r.left + 100), sy(wifi_.r.top + 32), st_.brightness ? 1 : 0, sx(displayTrack_.left), sx(displayTrack_.right),
             sy((displayTrack_.top + displayTrack_.bottom) * 0.5f));
    else
        DLOG(L"cc: targets page=%ls back=%d,%d rows=%zu", page_ == Page::Wifi ? L"wifi" : L"bluetooth", sx((back_.left + back_.right) * 0.5f),
             sy((back_.top + back_.bottom) * 0.5f), rows_.size());
}

// ---------------------------------------------------------------------------------------------
bool Panel::DrawGlass(const Module& m, float opacity, float soften, dock::GlassRole role, float edge) {
    if (!glassOk_) return false;
    dock::BackdropFrame frame;
    if (!backdrop_.Latest(windowRect_, frame)) return false;
    if (!tBackdropReady_) { tBackdropReady_ = Ms(); DLOG(L"cc: backdrop ready ms=%.1f", tBackdropReady_); }
    if (role == dock::GlassRole::ControlModule) role = ModuleRole();
    dock::GlassParameters gp = dock::RoleGlass(role);
    if (role == dock::GlassRole::BackdropBlur && gp.reducedTransparency) return true;  // no blur layer when transparency is reduced
    if (role == dock::GlassRole::BackdropBlur) gp.blurSpread = BackdropSpread();
    gp.radius = m.radius;
    gp.opacity = std::clamp(opacity, 0.0f, 1.0f);  // formed glass from the first visible frame; fade only
    gp.edgeSoftness = soften;
    edge = std::clamp(edge, 0.0f, 1.0f);  // grows the lens band and highlights; frost and body stay formed
    gp.refraction *= edge; gp.dispersion *= edge; gp.glare *= edge; gp.edgeAmount *= edge;
    gp.strokeOpacity *= edge; gp.fresnelAmount *= edge; gp.specularAmount *= edge;
    return RenderPlate(m.r, gp, frame);
}

bool Panel::RenderPlate(D2D1_RECT_F r, dock::GlassParameters& gp, dock::BackdropFrame& frame) {
    dock::GpuPaint g;
    g.context = ctx_.Get();
    g.target = rtv_.Get();
    g.targetWidth = pxW_;
    g.targetHeight = pxH_;
    g.windowScreenRect = windowRect_;
    g.dpiScale = scale_;
    g.plateDip = r;
    g.backdrop = &frame;
    g.params = &gp;
    g.pointerX = mx_ * scale_;
    g.pointerY = my_ * scale_;
    return glass_->RenderGpu(g);
}

// Liquid-glass switch, following the Glass Lab switch design (Lab.qml "labGlassSwitch"; that file is
// GPL-3.0-only, so this is an independent implementation of its proportions and behaviour, not a port):
//  * track 116x50 units, solid colour (off #DADADA, on: system blue #0A84FF instead of the Lab's green), 220 ms;
//  * thumb 74x44 white pill inset 3; while pressed it grows to 104x72 (overflowing the track, centre fixed) over
//    150 ms, the white fades out and a glass lens that refracts the track itself fades in;
//  * travel eases with an overshoot (Lab: 420 ms OutBack 0.65); drag follows the pointer 1:1, release past the
//    middle switches.
// Geometry of the thumb for the current animation state, in panel DIPs (before the open-animation transform).
D2D1_RECT_F Panel::SwitchThumbRect(D2D1_RECT_F r) const {
    // macOS 27 proportions (measured): track 54x24, thumb 32x20 inset 2. Pressed it swells like the Lab thumb
    // (x1.41 wide, x1.64 high) and overflows the track.
    const float q = (r.bottom - r.top) / 24.0f, p = static_cast<float>(std::clamp(switchPress_.Value(), 0.0, 1.0));
    const float pos = static_cast<float>(std::clamp(switchOn_.Value(), -0.08, 1.08));
    const dock::GlassParameters lp = dock::ControlLensGlass(20.0f * q);  // pressed size: Sliders & switches category (Lab 104x72 from 74x44)
    const float restW = 32.0f * q, w = 32.0f * (1.0f + (lp.switchPressWidth - 1.0f) * 1.265f * p) * q, h = 20.0f * (1.0f + (lp.switchPressHeight - 1.0f) * 1.148f * p) * q, inset = 2.0f * q;
    const float cx = r.left + inset + restW * 0.5f + pos * ((r.right - r.left) - restW - 2.0f * inset), cy = (r.top + r.bottom) * 0.5f;
    return D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
}

bool Panel::SwitchStateOn() const {
    const bool wifi = page_ == Page::Wifi;
    const bool actual = wifi ? st_.wifiRadio == ctl::Radio::On : st_.btRadio == ctl::Radio::On;
    // Show the state the user asked for until Windows confirms it (or 4 s pass), so the thumb never bounces back.
    if (switchWant_ >= 0 && Now() < switchWantUntil_ && (switchWant_ == 1) != actual) return switchWant_ == 1;
    return actual;
}

bool Panel::UpdateSwitchMotion(double dt) {
    const bool on = SwitchStateOn();
    if (!switchInit_) { switchInit_ = true;
        switchOn_.SetSpec(docky::motion::tokens::Toggle); switchOn_.SetRest(0.001, 0.01); switchOn_.SnapTo(on ? 1.0 : 0.0);
        switchPress_.SetSpec(docky::motion::tokens::Press); switchPress_.SetRest(0.002, 0.02);
        switchColor_.SetSpec(docky::motion::tokens::Tint); switchColor_.SetRest(0.002, 0.02); switchColor_.SnapTo(on ? 1.0 : 0.0); }
    if (switchDragging_) switchOn_.SnapTo(switchDragPos_);  // 1:1 with the pointer
    else switchOn_.SetTarget(on ? 1.0 : 0.0);
    switchColor_.SetTarget(switchDragging_ ? (switchDragPos_ >= 0.5 ? 1.0 : 0.0) : (on ? 1.0 : 0.0));
    switchPress_.SetTarget(pressed_ == Hit::PageSwitch ? 1.0 : 0.0);
    if (reducedMotion_) { switchOn_.SnapTo(switchOn_.Target()); switchPress_.SnapTo(switchPress_.Target()); switchColor_.SnapTo(switchColor_.Target()); return false; }
    const bool a = switchOn_.Step(dt), b = switchPress_.Step(dt), c = switchColor_.Step(dt);
    return a || b || c;
}

// Glass lens thumb (pressed state): copy the already drawn switch area of the back buffer and refract it, so the
// lens shows the blue/grey track underneath like the Lab's control backdrop. Runs after the D2D pass.
void Panel::DrawSwitchLens(D2D1_RECT_F thumb, float opacity) {
    if (!glassOk_ || opacity <= 0.002f || forceOpaque_) return;
    const float m = 16.0f;  // refraction / bevel margin (DIP)
    LONG x0 = static_cast<LONG>(std::floor((thumb.left - m) * scale_)), y0 = static_cast<LONG>(std::floor((thumb.top - m) * scale_));
    LONG x1 = static_cast<LONG>(std::ceil((thumb.right + m) * scale_)), y1 = static_cast<LONG>(std::ceil((thumb.bottom + m) * scale_));
    x0 = std::clamp(x0, 0L, static_cast<LONG>(pxW_)); x1 = std::clamp(x1, 0L, static_cast<LONG>(pxW_));
    y0 = std::clamp(y0, 0L, static_cast<LONG>(pxH_)); y1 = std::clamp(y1, 0L, static_cast<LONG>(pxH_));
    if (x1 - x0 < 4 || y1 - y0 < 4) return;
    ComPtr<ID3D11Texture2D> back;
    if (floatingScene_) back = colors_.Texture();
    else if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC bd{}; back->GetDesc(&bd);
    const UINT w = static_cast<UINT>(x1 - x0), h = static_cast<UINT>(y1 - y0);
    if (!lensScene_ || lensW_ != w || lensH_ != h) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = bd.Format; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        lensScene_.Reset(); if (FAILED(d3d_->CreateTexture2D(&d, nullptr, &lensScene_))) return;
        lensW_ = w; lensH_ = h;
    }
    const D3D11_BOX box{static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x1), static_cast<UINT>(y1), 1};
    ctx_->CopySubresourceRegion(lensScene_.Get(), 0, 0, 0, 0, back.Get(), 0, &box);
    dock::BackdropFrame frame;
    frame.texture = lensScene_.Get();
    frame.screenRect = RECT{windowRect_.left + x0, windowRect_.top + y0, windowRect_.left + x1, windowRect_.top + y1};
    frame.frameId = ++lensFrame_;
    // Glass Studio "Sliders & switches" (defaults: the Lab lens, refractionDirection -1).
    dock::GlassParameters gp = dock::ControlLensGlass(thumb.bottom - thumb.top);
    gp.opacity = opacity;
    // Lab lens (refractionDirection -1): inverted refraction, the rim samples outward, so the track shows slightly smaller
    // inside the drop with a dark band around it and coloured fringes at the edge (references 2026-11-10). Clear glass:
    // no frost, no body tone; the outline rings are drawn by DrawLensHighlights.
    RenderPlate(thumb, gp, frame);
}

// Glass edge of the pressed thumb, drawn over the GPU lens: a bright hairline rim that is strongest at the top, a
// specular crescent near the top edge and a faint inner glow at the bottom (liquid-glass drop over the track).
// Lens outline (references): a dark outer ring with a thin light rim just inside it.
void Panel::DrawLensHighlights(D2D1_RECT_F t, float op, float k, D2D1_POINT_2F pivot) {
    if (!glassOk_ || op <= 0.002f || forceOpaque_) return;
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, pivot));
    const float r = (t.bottom - t.top) * 0.5f;
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.55f * op));
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left + 0.7f, t.top + 0.7f, t.right - 0.7f, t.bottom - 0.7f), r - 0.7f, r - 0.7f), brush_.Get(), 1.4f);
    const D2D1_GRADIENT_STOP rim[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0.45f * op)}, {0.5f, D2D1::ColorF(1, 1, 1, 0.18f * op)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.35f * op)}};
    ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> rimBrush;
    if (SUCCEEDED(dc_->CreateGradientStopCollection(rim, 3, &stops)) &&
        SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(t.left, t.top), D2D1::Point2F(t.right, t.bottom)), stops.Get(), &rimBrush)))
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left + 2.0f, t.top + 2.0f, t.right - 2.0f, t.bottom - 2.0f), r - 2.0f, r - 2.0f), rimBrush.Get(), 0.9f);
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->EndDraw();
}

bool Panel::RefreshTheme(){
 const auto loaded=liquid::settings::Load(g_settingsPath.empty()?liquid::settings::DefaultPath():g_settingsPath).value;
 const bool mediaChanged=mediaArtworkBackground_!=loaded.controlCenter.mediaArtworkBackground;
 const int mode=loaded.appearance.mode;backdropBlur_=loaded.controlCenter.backdropBlur;mediaArtworkBackground_=loaded.controlCenter.mediaArtworkBackground;
 bool systemDark=true;try{auto ui=winrt::Windows::UI::ViewManagement::UISettings();auto c=ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);systemDark=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000;}catch(...){}
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 const bool opaque=contrast||!liquid::theme::IsDark(mode,systemDark);const auto next=liquid::theme::Palette(mode,systemDark,contrast);
 const bool changed=forceOpaque_!=opaque||contrastMode_!=contrast||palette_.surface!=next.surface||palette_.primary!=next.primary;
 forceOpaque_=opaque;contrastMode_=contrast;palette_=next;if(changed)DLOG(L"cc: appearance=%d opaque=%d contrast=%d",mode,opaque,contrast);return changed||mediaChanged;
}
void Panel::Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, float alpha, D2D1::ColorF c) {
    if(forceOpaque_&&c.r==1.f&&c.g==1.f&&c.b==1.f)c=Ink(c.a);
    brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a * alpha));
    const auto text = docky::ui::Text(s.c_str());
    dc_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Panel::CenteredText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, float alpha, D2D1::ColorF c) {
    if(!f)return;const auto old=f->GetTextAlignment();f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);Text(s,f,r,alpha,c);f->SetTextAlignment(old);
}

void Panel::Glyph(wchar_t g, float size, D2D1_POINT_2F c, float alpha, D2D1::ColorF color, bool fixedColor) {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dwrite_->CreateTextFormat(iconFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &f)))
        return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const wchar_t s[2] = {g, 0};
    if(!fixedColor&&forceOpaque_&&color.r==1.f&&color.g==1.f&&color.b==1.f)color=Ink(color.a);
    brush_->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a * alpha));
    dc_->DrawTextW(s, 1, f.Get(), D2D1::RectF(c.x - size, c.y - size, c.x + size, c.y + size), brush_.Get());
}

bool Panel::MakeIconMask(const std::vector<uint32_t>& pixels,UINT w,UINT h,IconMask& result){
    result={};if(!w||!h||pixels.size()!=static_cast<size_t>(w)*h)return false;
    UINT x0=w,y0=h,x1=0,y1=0;double total=0,sx=0,sy=0;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){const unsigned a=pixels[static_cast<size_t>(y)*w+x]>>24;if(a>8){x0=(std::min)(x0,x);y0=(std::min)(y0,y);x1=(std::max)(x1,x);y1=(std::max)(y1,y);total+=a;sx+=(x+.5)*a;sy+=(y+.5)*a;}}
    if(!total)return false;result.source=D2D1::RectF(static_cast<float>(x0),static_cast<float>(y0),static_cast<float>(x1+1),static_cast<float>(y1+1));
    result.width=w;result.pixels=pixels;
    result.offsetX=static_cast<float>((x0+x1+1)*.5-sx/total);result.offsetY=static_cast<float>((y0+y1+1)*.5-sy/total);
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    return SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(w,h),pixels.data(),w*4,&props,result.bitmap.GetAddressOf()));
}
// Decoded status icon pixels, kept between openings: decoding the PNGs and drawing the fallback Wi-Fi glyph with GDI+
// took a good part of each opening. Reused while the active pack's files are unchanged (path and write time).
struct DecodedStatusIcons { std::wstring signature; std::vector<std::pair<std::string,std::vector<uint32_t>>> icons; std::vector<std::pair<UINT,UINT>> sizes; std::vector<uint32_t> wifi; };
std::mutex g_decodedMutex;
DecodedStatusIcons g_decoded;
void Panel::LoadStatusIcons(){
    statusIcons_.clear();docky::workshop::Pack pack;bool enabled=false;std::wstring error;
    const auto root=g_workshopRoot.empty()?docky::workshop::DefaultRoot():g_workshopRoot;
    std::vector<std::pair<std::string,std::wstring>> files;std::wstring signature=root;
    if(docky::workshop::Active(root,pack,enabled,error)&&enabled)
      for(const auto& [key,path]:pack.icons){if(key.rfind("wifi.",0)!=0&&key.rfind("sound.",0)!=0&&key.rfind("battery.",0)!=0)continue;
        files.emplace_back(key,path);WIN32_FILE_ATTRIBUTE_DATA a{};GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a);
        signature+=L"|"+path+L"@"+std::to_wstring(a.ftLastWriteTime.dwLowDateTime)+L"."+std::to_wstring(a.ftLastWriteTime.dwHighDateTime);}
    DecodedStatusIcons decoded;bool reused=false;
    {   std::lock_guard<std::mutex> lock(g_decodedMutex);
        if(!g_decoded.wifi.empty()&&g_decoded.signature==signature){decoded=g_decoded;reused=true;}
    }
    if(!reused){
        decoded.signature=signature;
        for(const auto& [key,path]:files){std::vector<uint32_t> pixels;UINT w=0,h=0;std::wstring why;
          if(dock::LoadCustomIconPixels(path,pixels,w,h,why)){decoded.icons.emplace_back(key,std::move(pixels));decoded.sizes.emplace_back(w,h);}}
        for(const auto& entry:docky::usersound::entries){std::vector<uint32_t> pixels;UINT w=0,h=0;
          if(docky::usersound::Load(entry.file,pixels,w,h)){decoded.icons.emplace_back(entry.key,std::move(pixels));decoded.sizes.emplace_back(w,h);}}
        decoded.wifi.assign(128*128,0);ULONG_PTR token=0;Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok){
          {Gdiplus::Bitmap bmp(128,128,128*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(decoded.wifi.data()));Gdiplus::Graphics g(&bmp);docky::statusart::Wifi(g,64,64,4,Gdiplus::Color(255,255,255,255));}
          Gdiplus::GdiplusShutdown(token);
        }
        std::lock_guard<std::mutex> lock(g_decodedMutex);g_decoded=decoded;
    }
    for(size_t i=0;i<decoded.icons.size();++i){IconMask icon;  // later entries win (user sound icons over the pack's)
      if(MakeIconMask(decoded.icons[i].second,decoded.sizes[i].first,decoded.sizes[i].second,icon))statusIcons_[decoded.icons[i].first]=std::move(icon);}
    MakeIconMask(decoded.wifi,128,128,fallbackWifi_);
    DLOG(L"cc: %ls %zu shared Workshop status icons",reused?L"reused":L"loaded",statusIcons_.size());
}
void Panel::DrawMask(const IconMask& icon,D2D1_POINT_2F c,float size,D2D1::ColorF color,float alpha){
    if(!icon.bitmap)return;const float w=icon.source.right-icon.source.left,h=icon.source.bottom-icon.source.top;if(w<=0||h<=0)return;
    float k=size/h;if(w*k>size*2)k=size*2/w;const float limit=size*(1.25f/14.f);
    const float ox=std::clamp(icon.offsetX*k*.8f,-limit,limit),oy=std::clamp(icon.offsetY*k*.8f,-limit,limit);
    const auto dest=D2D1::RectF(c.x-w*k*.5f+ox,c.y-h*k*.5f+oy,c.x+w*k*.5f+ox,c.y+h*k*.5f+oy);
    // FillOpacityMask performs poor minification on large source images. Area-average
    // their alpha at the actual physical output size first, and cache each DPI/size.
    const UINT tw=std::max(1u,static_cast<UINT>(std::lround(w*k*scale_))),th=std::max(1u,static_cast<UINT>(std::lround(h*k*scale_)));
    const std::uint64_t key=(static_cast<std::uint64_t>(tw)<<32)|th;auto found=icon.scaled.find(key);
    if(found==icon.scaled.end()&&!icon.pixels.empty()&&icon.width){
      std::vector<uint32_t> samples(static_cast<size_t>(tw)*th);const UINT left=static_cast<UINT>(icon.source.left),top=static_cast<UINT>(icon.source.top),sw=static_cast<UINT>(w),sh=static_cast<UINT>(h);
      for(UINT y=0;y<th;++y){const UINT y0=top+y*sh/th,y1=top+std::max(y*sh/th+1,(y+1)*sh/th);
       for(UINT x=0;x<tw;++x){const UINT x0=left+x*sw/tw,x1=left+std::max(x*sw/tw+1,(x+1)*sw/tw);std::uint64_t sum=0;UINT count=0;
        for(UINT sy=y0;sy<y1;++sy)for(UINT sx=x0;sx<x1;++sx){sum+=icon.pixels[static_cast<size_t>(sy)*icon.width+sx]>>24;++count;}
        const unsigned value=count?static_cast<unsigned>((sum+count/2)/count):0;samples[static_cast<size_t>(y)*tw+x]=value*0x01010101u;
       }}
      const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);ComPtr<ID2D1Bitmap1> scaled;
      if(SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(tw,th),samples.data(),tw*4,&props,scaled.GetAddressOf())))found=icon.scaled.emplace(key,std::move(scaled)).first;
    }
    color.a*=alpha;brush_->SetColor(color);const auto mode=dc_->GetAntialiasMode();dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if(found!=icon.scaled.end()){const auto source=D2D1::RectF(0,0,static_cast<float>(tw),static_cast<float>(th));dc_->FillOpacityMask(found->second.Get(),brush_.Get(),D2D1_OPACITY_MASK_CONTENT_GRAPHICS,&dest,&source);}
    else dc_->FillOpacityMask(icon.bitmap.Get(),brush_.Get(),D2D1_OPACITY_MASK_CONTENT_GRAPHICS,&dest,&icon.source);
    dc_->SetAntialiasMode(mode);
}
void Panel::DrawWifi(D2D1_POINT_2F c,float size,const char* key,D2D1::ColorF color,float alpha){
    auto it=statusIcons_.find(key);if(it==statusIcons_.end())it=statusIcons_.find("wifi.connected");DrawMask(it==statusIcons_.end()?fallbackWifi_:it->second,c,size,color,alpha);
}
void Panel::DrawBluetooth(D2D1_POINT_2F c,float size,D2D1::ColorF color,float alpha){
    color.a*=alpha;brush_->SetColor(color);const float s=size/18.f,w=2.15f*s;
    // The rune as one polyline (left wing, up-right, down the stem, right arrowheads, right wing) with round caps and
    // round joins, so every corner is rounded; slightly heavier than before (2.15 vs 1.8 / 18), matching the Dock and menu bar.
    const D2D1_POINT_2F p[]={{c.x-5*s,c.y-4*s},{c.x+5*s,c.y+4*s},{c.x,c.y+8*s},{c.x,c.y-8*s},{c.x+5*s,c.y-4*s},{c.x-5*s,c.y+4*s}};
    ComPtr<ID2D1Factory> factory;dc_->GetFactory(&factory);ComPtr<ID2D1StrokeStyle> round;ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
    if(!factory||FAILED(factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND),nullptr,0,&round))||
       FAILED(factory->CreatePathGeometry(&path))||FAILED(path->Open(&sink)))return;
    sink->BeginFigure(p[0],D2D1_FIGURE_BEGIN_HOLLOW);sink->AddLines(p+1,5);sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->Close();
    dc_->DrawGeometry(path.Get(),brush_.Get(),w,round.Get());
}
void Panel::DrawSound(D2D1_POINT_2F c,float size,D2D1::ColorF color,float alpha){
    const char* key=!st_.audio?"sound.unavailable":st_.muted?"sound.muted":st_.volume<34?"sound.low":st_.volume<67?"sound.medium":"sound.high";
    const auto it=statusIcons_.find(key);if(it!=statusIcons_.end()){DrawMask(it->second,c,size,color,alpha);return;}
    const int state=(!st_.audio?0:st_.muted?1:st_.volume<=0?2:st_.volume<34?3:st_.volume<67?4:5);
    if(state!=fallbackSoundState_){fallbackSoundState_=state;std::vector<uint32_t> pixels(128*128,0);ULONG_PTR token=0;Gdiplus::GdiplusStartupInput input;
      if(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok){
       {Gdiplus::Bitmap bmp(128,128,128*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(pixels.data()));Gdiplus::Graphics g(&bmp);docky::statusart::Speaker(g,64,64,4,Gdiplus::Color(255,255,255,255),st_.audio,st_.muted,st_.volume);}
       Gdiplus::GdiplusShutdown(token);for(auto& px:pixels){const unsigned a=(std::max)({px&255u,(px>>8)&255u,(px>>16)&255u});px=a|(a<<8)|(a<<16)|(a<<24);}MakeIconMask(pixels,128,128,fallbackSound_);
      }
    }DrawMask(fallbackSound_,c,size,color,alpha);
}
void Panel::DrawBattery(D2D1_POINT_2F c,float alpha){
    const auto key=(st_.charging||st_.powerSource==ctl::PowerSource::External)?"battery.charging":"battery.discharging";const auto it=statusIcons_.find(key);
    if(it!=statusIcons_.end()){DrawMask(it->second,c,16,Ink(),alpha);return;}
    const bool bolt=st_.charging||st_.powerSource==ctl::PowerSource::External;
    if(!batteryBitmap_||batteryLevel_!=st_.batteryPercent||batteryCharging_!=bolt){
      batteryLevel_=st_.batteryPercent;batteryCharging_=bolt;std::vector<uint32_t> pixels(128*128,0);ULONG_PTR token=0;Gdiplus::GdiplusStartupInput input;
      if(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok){docky::battery::DrawSmooth(pixels.data(),128,128,64,64,2.5f,batteryLevel_,batteryCharging_);Gdiplus::GdiplusShutdown(token);
       const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);batteryBitmap_.Reset();dc_->CreateBitmap(D2D1::SizeU(128,128),pixels.data(),128*4,&props,batteryBitmap_.GetAddressOf());}
    }
    if(batteryBitmap_)dc_->DrawBitmap(batteryBitmap_.Get(),D2D1::RectF(c.x-19,c.y-19,c.x+19,c.y+19),alpha,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}
// One accent for every selected / connected / on state in these popups: the macOS dark-appearance system blue #0A84FF.
// (The pale #678EF6 circles and #607DF7 switch track read whitish next to it, and the sound glyph used a third blue.)
static D2D1::ColorF Accent(float a = 1.0f) { return D2D1::ColorF(0x0A / 255.f, 0x84 / 255.f, 1.0f, a); }

void Panel::ToggleCircle(D2D1_POINT_2F c, wchar_t g, bool on, bool enabled, float a, float r) {
    // macOS: active = white disc with tinted glyph; inactive = translucent disc with white glyph.
    brush_->SetColor(on ? D2D1::ColorF(1, 1, 1, 0.97f * a) : (forceOpaque_?Ink((enabled ? 0.20f : 0.10f) * a):D2D1::ColorF(1, 1, 1, (enabled ? 0.20f : 0.10f) * a)));
    dc_->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
    const auto color=on?Accent():(forceOpaque_?Ink():D2D1::ColorF(1,1,1));
    if(g==0xE701){const char* key=!st_.wifiAdapter||st_.wifiRadio==ctl::Radio::Unavailable?"wifi.unavailable":st_.wifiConnected?"wifi.connected":"wifi.disconnected";DrawWifi(c,r*.9f,key,color,a*(enabled?1.f:.45f));}
    else if(g==0xE702)DrawBluetooth(c,r*.9f,color,a*(enabled?1.f:.45f));
    else Glyph(g,r*.9f,c,a*(enabled?1.f:.45f),color,true);
}

void Panel::DetailCircle(D2D1_POINT_2F c,wchar_t g,bool active,float a){
    const unsigned bg=contrastMode_?palette_.accent:0x0A84FF,fg=contrastMode_?palette_.onAccent:0xFFFFFF;
    brush_->SetColor(active?D2D1::ColorF(((bg>>16)&255)/255.f,((bg>>8)&255)/255.f,(bg&255)/255.f,a):Ink(0.17f*a));
    dc_->FillEllipse(D2D1::Ellipse(c,13,13),brush_.Get());
    const auto color=active?D2D1::ColorF(((fg>>16)&255)/255.f,((fg>>8)&255)/255.f,(fg&255)/255.f,1.f):Ink(0.72f);
    if(g==0xE701)DrawWifi(c,12.f,active?"wifi.connected":"wifi.disconnected",color,a);
    else if(g==0xE702)DrawBluetooth(c,12.f,color,a);
    else Glyph(g,12.f,c,a,color,true);
}

bool Panel::UpdateSliderMotion(double dt) {
    if (!sliderInit_) { sliderInit_ = true; sliderPress_.SetSpec(docky::motion::tokens::Press); sliderPress_.SetRest(0.002, 0.02); }
    const bool pressing = pressed_ == Hit::DisplaySlider || pressed_ == Hit::SoundSlider;
    if (pressing) sliderPressHit_ = pressed_;
    sliderPress_.SetTarget(pressing ? 1.0 : 0.0);
    if (reducedMotion_) { sliderPress_.SnapTo(sliderPress_.Target()); return false; }
    const bool moving = sliderPress_.Step(dt);
    if (!pressing && sliderPress_.Value() <= 0.002) sliderPressHit_ = Hit::None;
    return moving;
}

void Panel::Slider(D2D1_RECT_F t, float v, bool active, float a, Hit which) {
    // Glass Lab slider: the track and its white fill as before (macOS Control Center), the thumb a white pill
    // (Lab 40x30 at 60 %) that grows into a glass lens refracting the fill while it is pressed.
    v = std::clamp(v, 0.0f, 1.0f);
    const float r = (t.bottom - t.top) * 0.5f;
    brush_->SetColor(Ink(0.22f * a));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(t, r, r), brush_.Get());
    // The thumb's centre stays half a thumb inside both ends (Lab: track inset 20 for the 40 px thumb), so at 0 % and 100 %
    // the thumb ends with the track instead of covering the icons beside it.
    const float x = t.left + kSliderThumbInset + (t.right - t.left - 2 * kSliderThumbInset) * v;
    brush_->SetColor(active ? Ink(0.95f * a) : Ink(0.45f * a));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left, t.top, std::max(t.left + 2 * r, x), t.bottom), r, r), brush_.Get());
    const float cy = (t.top + t.bottom) * 0.5f;
    const float p = which == sliderPressHit_ ? static_cast<float>(std::clamp(sliderPress_.Value(), 0.0, 1.0)) : 0.0f;
    const dock::GlassParameters lensParams = dock::ControlLensGlass(18.0f);
    const float kw = 24.0f * (1.0f + (lensParams.sliderPressWidth - 1.0f) * p), kh = 18.0f * (1.0f + (lensParams.sliderPressHeight - 1.0f) * p);
    const D2D1_RECT_F k = D2D1::RectF(x - kw * 0.5f, cy - kh * 0.5f, x + kw * 0.5f, cy + kh * 0.5f);
    const float kr = kh * 0.5f;
    const bool lens = !forceOpaque_ && glassOk_ && !reducedMotion_;
    const float white = lens ? 1.0f - p : 1.0f;
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.18f * a * white));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left, k.top + 1.0f, k.right, k.bottom + 1.0f), kr, kr), brush_.Get());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, a * white));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(k, kr, kr), brush_.Get());
    if (lens && p > 0.002f) { lensThumb_ = k; lensOpacity_ = p * a; }
}

void Panel::Switch(D2D1_RECT_F r, bool /*on*/, bool enabled, float a) {
    // Solid track: off grey -> macOS 27 menu blue #607DF7 (measured) on, cross-faded by the colour spring.
    const float c = static_cast<float>(std::clamp(switchColor_.Value(), 0.0, 1.0)), ea = (enabled ? 1.0f : 0.5f) * a;
    const float rad = (r.bottom - r.top) * 0.5f, off = forceOpaque_ ? 0.855f : 0.27f;
    const auto on = Accent();
    brush_->SetColor(D2D1::ColorF(off + (on.r - off) * c, off + (on.g - off) * c, off + (on.b - off) * c, ea));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.08f * ea));  // the slightly darker rim of the reference track
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), brush_.Get(), 1.0f);
    // White thumb; fades out while pressed as the glass lens (post pass) takes over.
    const float p = static_cast<float>(std::clamp(switchPress_.Value(), 0.0, 1.0));
    const D2D1_RECT_F t = SwitchThumbRect(r);
    const float tr = (t.bottom - t.top) * 0.5f;
    const bool lens = !forceOpaque_ && glassOk_ && !reducedMotion_;
    const float white = lens ? 1.0f - p : 1.0f;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, ea * white));  // plain white thumb, no shadow (reference)
    dc_->FillRoundedRectangle(D2D1::RoundedRect(t, tr, tr), brush_.Get());
    lensThumb_ = t; lensOpacity_ = lens ? p * ea : 0.0f;
}

void Panel::DrawModuleBase(const Module& m, float a, bool hover, bool press) {
    if (!lastGlass_ || glassFade_ < 1.f) {  // Fade the stable fallback over newly available glass.
        const float fallback = lastGlass_ ? 1.f - glassFade_ : 1.f;
        brush_->SetColor(forceOpaque_?Surface(a*fallback):D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.80f * a * fallback));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(m.r, m.radius, m.radius), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a * fallback));
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(m.r, m.radius, m.radius), brush_.Get(), 1.0f / scale_);
    }
    if (hover || press) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, (press ? 0.14f : 0.07f) * a));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(m.r, m.radius, m.radius), brush_.Get());
    }
}

std::wstring Panel::Describe(Hit h) const {
    switch (h) {
    case Hit::WifiToggle: return ctl::CanToggleRadio(st_.wifiSwitchable,st_.wifiRadio) ? L"Turn Wi-Fi on or off" : L"Wi-Fi switching unavailable — opens Settings";
    case Hit::WifiBody: return L"Show nearby Wi-Fi networks";
    case Hit::BtToggle: return ctl::CanToggleRadio(st_.btSwitchable,st_.btRadio) ? L"Turn Bluetooth on or off" : L"Bluetooth switching unavailable — opens Settings";
    case Hit::BtBody: return L"Show connected Bluetooth devices";
    case Hit::Airplane: return L"Airplane Mode — opens Windows Settings";
    case Hit::Focus: return L"Do Not Disturb — opens Windows Settings";
    case Hit::Stage: return docky::ui::Text(GetPropW(StageDock(), kStageEnabledProperty) ?
        L"Turn Stage Manager off" : L"Turn Stage Manager on");
    case Hit::Battery: return L"Battery — opens Windows Settings";
    case Hit::DisplayTitle: return L"Display settings";
    case Hit::SoundTitle: return L"Sound settings";
    case Hit::Mute: return st_.muted ? L"Unmute" : L"Mute";
    case Hit::OtherNetworks: return L"Open Windows network list";
    case Hit::Settings: return soundOnly_ ? L"Open sound settings" : page_ == Page::Wifi ? L"Open Wi-Fi settings" : L"Open Bluetooth settings";
    default: return L"";
    }
}

void Panel::Render() {
    // Open/close: fade combined with a scale from slightly larger down to 1 (macOS-style Control Center),
    // pivoting on the menu-bar button so the panel reads as coming from it. `s` is the spring value;
    // opacity reaches 1 early (s >= 0.6) while the scale keeps settling. Glass is fully formed throughout.
    // Measured from the macOS reference recording (2026-11-10, frames 120-128 and 150-160): the desktop
    // behind the whole panel blurs within 1-2 frames, then modules go from blurred + faint to sharp + opaque
    // in ~0.12 s with no measurable scale (< 2%); on close the backdrop blur drops at once and modules fade
    // out over ~0.16 s while staying sharp.
    const float s = std::clamp(openT_, 0.0f, 1.0f);
    const float a = closing_ ? s : std::clamp(s / 0.85f, 0.0f, 1.0f);
    const float defocus = (closing_ || reducedMotion_) ? 0.0f : kDefocusMax * (1.0f - s) * (1.0f - s);  // DIP
    const float blurLayer = closing_ ? 0.0f : std::clamp(s / 0.12f, 0.0f, 1.0f);
    const float k = 1.0f + kOpenScaleFrom * (1.0f - s);
    const float pivotX = std::clamp((static_cast<float>(anchor_.left + anchor_.right) * 0.5f - static_cast<float>(windowRect_.left)) / scale_, 0.0f, W_);
    const float pivotY = 0.0f;  // the menu bar edge
    auto scaled = [&](D2D1_RECT_F r) {
        return D2D1::RectF(pivotX + (r.left - pivotX) * k, pivotY + (r.top - pivotY) * k, pivotX + (r.right - pivotX) * k, pivotY + (r.bottom - pivotY) * k);
    };
    const float clear[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);
    // Glass plates (live WGC backdrop + shared Liquid Glass material), then content.
    bool allGlass = !forceOpaque_;
    if (!forceOpaque_ && blurLayer > 0.0f && !modules_.empty()) {
        // One blur behind the whole panel (not per module), so the gaps between modules are blurred too.
        // The feather lies entirely inside the window: the blur reaches 0 exactly at the window edge, so
        // the window boundary can never show as a hard line. smoothstep over 2*f gives a long, soft ramp.
        // The blurred area reaches 28 DIP past the panel and fades over a further 34 DIP on the sides and bottom (the
        // window padding is 64), so there is no visible edge; it runs straight up to the menu bar.
        Module back;
        back.r = D2D1::RectF(kPadX - kBlurReach, -200.0f, W_ - kPadX + kBlurReach, H_ - kPadBottom + kBlurReach);
        back.radius = 48.0f;
        if (BackdropSpread() > 0.001f) DrawGlass(back, blurLayer, kBlurFeather, dock::GlassRole::BackdropBlur);
    }
    // Liquid-glass reveal (reference frames 124-132): plates start as faint frosted bodies with no rim; the
    // refraction band, edge highlights and stroke grow in more slowly than the opacity, completing ~0.17 s.
    const float edgeReveal = closing_ ? 1.0f : s * s * (3.0f - 2.0f * s);
    for (Module m : modules_) {
        m.r = scaled(m.r);
        m.radius *= k;
        if(!forceOpaque_)allGlass = DrawGlass(m, a, defocus, dock::GlassRole::ControlModule, edgeReveal) && allGlass;
    }
    lensOpacity_ = 0.0f;  // set by Switch() when the page switch is drawn this frame
    lastGlass_ = allGlass;
    if (!lastGlass_) glassFade_ = 0.f;
    else if (s < 0.02f) glassFade_ = 1.f;  // glass ready before the reveal: no fallback cross-fade on top of it
    if (lastGlass_ && !tFirstGlass_) { tFirstGlass_ = Ms(); DLOG(L"cc: first glass submitted ms=%.1f", tFirstGlass_); }
    // While defocused, D2D content (icons, text, toggles) is drawn into an offscreen layer and composited
    // through a Gaussian blur whose radius shrinks to 0 as the panel settles.
    bool layered = false;
    if (defocus > 0.3f && target_) {
        const auto px = target_->GetPixelSize();
        if (!contentLayer_ || contentLayer_->GetPixelSize().width != px.width || contentLayer_->GetPixelSize().height != px.height) {
            contentLayer_.Reset();
            float dx = 96.f, dy = 96.f; dc_->GetDpi(&dx, &dy);
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dx, dy);
            dc_->CreateBitmap(px, nullptr, 0, &props, &contentLayer_);
        }
        if (!defocusFx_) dc_->CreateEffect(CLSID_D2D1GaussianBlur, &defocusFx_);
        layered = contentLayer_ && defocusFx_;
    }
    if (layered) dc_->SetTarget(contentLayer_.Get());
    dc_->BeginDraw();
    if (layered) dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(pivotX, pivotY)));
    auto hovered = [&](Hit h) { return hover_ == h && pressed_ == Hit::None; };
    auto pressedOn = [&](Hit h) { return pressed_ == h && hover_ == h; };
    if (page_ == Page::Main) {
        if (!soundOnly_) {
        // Wi-Fi
        DrawModuleBase(wifi_, a, hovered(Hit::WifiBody) || hovered(Hit::WifiToggle), pressedOn(Hit::WifiBody));
        const bool wifiOn = st_.wifiRadio == ctl::Radio::On;
        ToggleCircle(D2D1::Point2F(wifi_.r.left + 32, wifi_.r.top + 32), 0xE701, wifiOn, st_.wifiAdapter, a);
        CenteredText(L"Wi-Fi", title_.Get(), D2D1::RectF(wifi_.r.left + 58, wifi_.r.top + 13, wifi_.r.right - 10, wifi_.r.top + 32), a);
        const std::wstring wsub = !st_.wifiAdapter || st_.wifiRadio==ctl::Radio::Unavailable ? L"Unavailable" : st_.wifiRadio==ctl::Radio::Unknown ? L"Status Unknown" : !wifiOn ? L"Off" : st_.wifiConnected ? st_.ssid : L"Not Connected";
        CenteredText(wsub, sub_.Get(), D2D1::RectF(wifi_.r.left + 58, wifi_.r.top + 32, wifi_.r.right - 10, wifi_.r.top + 50), a, Ink( 0.62f));
        // Bluetooth
        DrawModuleBase(bt_, a, hovered(Hit::BtBody) || hovered(Hit::BtToggle), pressedOn(Hit::BtBody));
        const bool btOn = st_.btRadio == ctl::Radio::On;
        ToggleCircle(D2D1::Point2F(bt_.r.left + 32, bt_.r.top + 32), 0xE702, btOn, st_.btRadio != ctl::Radio::Unavailable, a);
        CenteredText(L"Bluetooth", title_.Get(), D2D1::RectF(bt_.r.left + 58, bt_.r.top + 13, bt_.r.right - 10, bt_.r.top + 32), a);
        std::wstring bsub = st_.btRadio == ctl::Radio::Unavailable ? L"Unavailable" : st_.btRadio==ctl::Radio::Unknown ? L"Status Unknown" : !btOn ? L"Off"
                            : st_.btDevices.empty() ? L"On" : (st_.btDevices.size() == 1 ? st_.btDevices[0] : std::to_wstring(st_.btDevices.size()) + L" devices");
        CenteredText(bsub, sub_.Get(), D2D1::RectF(bt_.r.left + 58, bt_.r.top + 32, bt_.r.right - 10, bt_.r.top + 50), a, Ink( 0.62f));
        // Media
        DrawModuleBase(media_, a, false, false);
        if (!st_.artwork) {mediaArtwork_.Reset();mediaArtworkHash_=0;}
        else if (!mediaArtwork_ || mediaArtworkHash_!=st_.artworkHash) {
            const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            mediaArtwork_.Reset();
            if(SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(st_.artworkWidth,st_.artworkHeight),st_.artwork->data(),st_.artworkWidth*4,&props,&mediaArtwork_)))mediaArtworkHash_=st_.artworkHash;
        }
        auto artwork=[&](D2D1_RECT_F rect,float radius,float alpha) {
            if(!mediaArtwork_)return;
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect,radius,radius),&clip);
            if(!clip)return;
            dc_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),clip.Get()),nullptr);
            const auto size=mediaArtwork_->GetSize();const float aspect=(rect.right-rect.left)/(rect.bottom-rect.top);
            float w=size.width,h=size.height;if(w/h>aspect)w=h*aspect;else h=w/aspect;
            const auto src=D2D1::RectF((size.width-w)*.5f,(size.height-h)*.5f,(size.width+w)*.5f,(size.height+h)*.5f);
            dc_->DrawBitmap(mediaArtwork_.Get(),rect,alpha,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,src);
            dc_->PopLayer();
        };
        if(mediaArtworkBackground_ && mediaArtwork_ && !contrastMode_) {
            artwork(media_.r,media_.radius,a*.7f);
            brush_->SetColor(D2D1::ColorF(0,0,0,a*.48f));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(media_.r,media_.radius,media_.radius),brush_.Get());
        }
        const bool artworkBackground=mediaArtworkBackground_ && mediaArtwork_ && !contrastMode_;
        auto mediaInk=[&](float opacity){return artworkBackground?D2D1::ColorF(1,1,1,opacity):Ink(opacity);};
        const float ml=media_.r.left,mt=media_.r.top;
        const auto cover=D2D1::RectF(ml+14,mt+14,ml+54,mt+54);
        if(mediaArtwork_)artwork(cover,10,a);
        else {brush_->SetColor(Ink(a*.1f));dc_->FillRoundedRectangle(D2D1::RoundedRect(cover,10,10),brush_.Get());Glyph(0xE8D6,20,D2D1::Point2F(ml+34,mt+34),a*.5f);}
        Text(st_.media&&!st_.title.empty()?st_.title:L"Not Playing",title_.Get(),D2D1::RectF(ml+14,mt+63,media_.r.right-12,mt+82),a,mediaInk(1.f));
        Text(st_.media?(st_.artist.empty()?st_.source:st_.artist):L"",sub_.Get(),D2D1::RectF(ml+14,mt+82,media_.r.right-12,mt+99),a,mediaInk(.55f));
        auto mediaButton = [&](Hit h, D2D1_RECT_F r, bool enabled) {
            const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            if (enabled && (hovered(h) || pressedOn(h))) {
                brush_->SetColor(Ink( (pressedOn(h) ? 0.22f : 0.12f) * a));
                dc_->FillEllipse(D2D1::Ellipse(c, (r.right - r.left) * 0.5f, (r.right - r.left) * 0.5f), brush_.Get());
            }
            brush_->SetColor(mediaInk(a * (enabled ? .85f : .32f)));
            auto triangle=[&](float x,float y,float w,float height,bool reverse){
                ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
                if(FAILED(factory_->CreatePathGeometry(&path))||FAILED(path->Open(&sink)))return;
                sink->BeginFigure(D2D1::Point2F(x+(reverse?w:0),y-height*.5f),D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x+(reverse?0:w),y));
                sink->AddLine(D2D1::Point2F(x+(reverse?w:0),y+height*.5f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);if(SUCCEEDED(sink->Close()))dc_->FillGeometry(path.Get(),brush_.Get());
            };
            if(h==Hit::MediaPlay) {
                if(st_.playing){dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x-7,c.y-9,c.x-2,c.y+9),1,1),brush_.Get());dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x+2,c.y-9,c.x+7,c.y+9),1,1),brush_.Get());}
                else triangle(c.x-7,c.y,17,20,false);
            } else {triangle(c.x-11,c.y,11,13,h==Hit::MediaPrev);triangle(c.x,c.y,11,13,h==Hit::MediaPrev);}
        };
        mediaButton(Hit::MediaPrev, prevBtn_, st_.media && st_.canPrevious);
        mediaButton(Hit::MediaPlay, playBtn_, st_.media && st_.canPlayPause);
        mediaButton(Hit::MediaNext, nextBtn_, st_.media && st_.canNext);
        // Airplane / Focus (links to Settings: Windows offers no supported switch for apps)
        for (auto [m, g, h] : {std::tuple{airplane_, wchar_t(0xE709), Hit::Airplane}, std::tuple{focus_, wchar_t(0xE708), Hit::Focus}}) {
            DrawModuleBase(m, a, hovered(h), pressedOn(h));
            Glyph(g, 20.0f, D2D1::Point2F((m.r.left + m.r.right) * 0.5f, (m.r.top + m.r.bottom) * 0.5f), a);
        }
        DrawModuleBase(stage_, a, hovered(Hit::Stage), pressedOn(Hit::Stage));
        {
            const auto dock = StageDock();
            const bool on = dock && GetPropW(dock, kStageEnabledProperty);
            const float x = (stage_.r.left + stage_.r.right) * .5f, y = (stage_.r.top + stage_.r.bottom) * .5f;
            if (on) {
                brush_->SetColor(D2D1::ColorF(0.96f, 0.99f, 1.f, a));
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), stage_.radius - 2, stage_.radius - 2), brush_.Get());
            }
            brush_->SetColor(on ? Accent(a) : Ink(dock ? a : .40f * a));
            for (int i = -1; i <= 1; ++i) {
                const float top = y + i * 5.5f - 1.5f;
                dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 10, top, x - 7, top + 3), .7f, .7f), brush_.Get());
            }
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 3, y - 7, x + 10, y + 7), 2.5f, 2.5f),
                                      brush_.Get(), 1.8f);
        }
        // Battery (read-only)
        DrawModuleBase(battery_, a, hovered(Hit::Battery), pressedOn(Hit::Battery));
        if (st_.battery) {
            DrawBattery(D2D1::Point2F(battery_.r.left + 32,battery_.r.top + 32),a);
            CenteredText(st_.batteryPercent >= 0 ? std::to_wstring(st_.batteryPercent) + L"%" : L"Battery", title_.Get(),
                 D2D1::RectF(battery_.r.left + 58, battery_.r.top + 13, battery_.r.right - 10, battery_.r.top + 32), a);
            CenteredText(ctl::BatterySourceLabel(st_.charging, st_.powerSource), sub_.Get(),
                 D2D1::RectF(battery_.r.left + 58, battery_.r.top + 32, battery_.r.right - 10, battery_.r.top + 50), a, Ink( 0.62f));
        } else {
            Glyph(0xE83F, 18.0f, D2D1::Point2F(battery_.r.left + 32, battery_.r.top + 32), a * 0.5f);
            Text(st_.batteryKnown ? L"No Battery" : L"Battery Unknown", title_.Get(), D2D1::RectF(battery_.r.left + 58, battery_.r.top + 22, battery_.r.right - 10, battery_.r.top + 42), a,
                 Ink( 0.6f));
        }
        // Display (only when brightness is really adjustable)
        if (st_.brightness) {
            DrawModuleBase(display_, a, false, false);
            Text(L"Display", title_.Get(), D2D1::RectF(display_.r.left + 16, display_.r.top + 9, display_.r.right - 16, display_.r.top + 29), a);
            const float v = (dragBrightness_ >= 0 ? dragBrightness_ : st_.brightnessPercent) / 100.0f;
            Glyph(0xE706, 11.0f, D2D1::Point2F(display_.r.left + 24, displayTrack_.top + 2), a * 0.8f);
            Glyph(0xE706, 15.0f, D2D1::Point2F(display_.r.right - 24, displayTrack_.top + 2), a * 0.9f);
            Slider(displayTrack_, v, true, a, Hit::DisplaySlider);
        }
        } // ordinary Control Center modules
        // The standalone sound panel follows the menu-bar layout; the Control Center keeps its compact card.
        DrawModuleBase(sound_, a, false, false);
        Text(L"Sound", title_.Get(), D2D1::RectF(sound_.r.left + 16, sound_.r.top + 9, sound_.r.right - 16, sound_.r.top + 29), a);
        if (st_.audio) {
            const float v = (dragVolume_ >= 0 ? dragVolume_ : st_.volume) / 100.0f;
            const auto minSound=D2D1::Point2F(sound_.r.left+24,(soundTrack_.top+soundTrack_.bottom)*.5f);
            if(soundOnly_)Glyph(0xE992,11.f,minSound,a*.65f,Ink(),true);
            else DrawSound(minSound,11.f,Ink(),a*.8f);
            Slider(soundTrack_, v, !st_.muted, a, Hit::SoundSlider);
            const D2D1_POINT_2F mc = D2D1::Point2F((muteBtn_.left + muteBtn_.right) * 0.5f, (muteBtn_.top + muteBtn_.bottom) * 0.5f);
            if(!soundOnly_){brush_->SetColor(st_.muted ? Ink( 0.95f * a) : Ink( (hovered(Hit::Mute) ? 0.28f : 0.18f) * a));dc_->FillEllipse(D2D1::Ellipse(mc, 16, 16), brush_.Get());}
            const auto maxSound=statusIcons_.find("sound.high");
            if(soundOnly_&&maxSound!=statusIcons_.end())DrawMask(maxSound->second,mc,14.f,Ink(.7f),a);
            else DrawSound(mc,14.f,soundOnly_?Ink(0.7f):st_.muted?Accent():(forceOpaque_?Ink():D2D1::ColorF(1,1,1)),a);
        } else {
            Text(L"No audio output", sub_.Get(), D2D1::RectF(sound_.r.left + 16, sound_.r.top + 34, sound_.r.right - 16, sound_.r.top + 54), a,
                 Ink( 0.6f));
        }
        if(soundOnly_){
            brush_->SetColor(Ink(0.16f*a));dc_->DrawLine(D2D1::Point2F(sound_.r.left+14,sound_.r.top+64),D2D1::Point2F(sound_.r.right-14,sound_.r.top+64),brush_.Get(),1.f);
            Text(L"Output",title_.Get(),D2D1::RectF(sound_.r.left+14,sound_.r.top+68,sound_.r.right-14,sound_.r.top+91),a,Ink(0.55f));
            if (hovered(Hit::ManageOutputs)) {
                brush_->SetColor(Ink(0.10f * a));
                dc_->FillRoundedRectangle(D2D1::RoundedRect(manageOutputsRow_, 6, 6), brush_.Get());
            }
            CenteredText(managingOutputs_ ? L"Done" : L"Manage", rowFmt_.Get(), manageOutputsRow_, a);
            dc_->PushAxisAlignedClip(outputViewport_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (outputs_.empty()) {
                auto r = OutputRow(0);
                r.left += 8;
                Text(activeOutputs_.empty() ? L"No audio output" : L"All outputs are hidden",
                     sub_.Get(), r, a, Ink(0.6f));
            }
            for (size_t i = 0; i < outputs_.size(); ++i) {
                const auto r = OutputRow(i);
                if (r.bottom <= outputViewport_.top) continue;
                if (r.top >= outputViewport_.bottom) break;
                const auto& device = outputs_[i];
                if (hover_ == Hit::Row && hoverRow_ == static_cast<int>(i)) {
                    brush_->SetColor(Ink(0.10f * a));
                    dc_->FillRoundedRectangle(D2D1::RoundedRect(r, 7, 7), brush_.Get());
                }
                const auto center = D2D1::Point2F(r.left + 18, (r.top + r.bottom) * .5f);
                const float opacity = device.hidden ? a * .55f : a;
                // Selected output: blue disc, white glyph; the others a faint disc, like the Wi-Fi and Bluetooth rows.
                brush_->SetColor(device.selected ? Accent(opacity) : Ink(0.10f * opacity));
                dc_->FillEllipse(D2D1::Ellipse(center, 13, 13), brush_.Get());
                auto color = device.selected ? D2D1::ColorF(1, 1, 1) : Ink(0.72f);
                auto shape = docky::audio_output::FindShape(device.icon);
                if (shape == docky::audio_output::Shape::None) {
                    std::wstring lower = device.name;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (lower.find(L"display") != std::wstring::npos || lower.find(L"hdmi") != std::wstring::npos ||
                        lower.find(L"nvidia") != std::wstring::npos)
                        shape = docky::audio_output::Shape::Monitor;
                }
                if (shape == docky::audio_output::Shape::None) DrawSound(center, 12.f, color, opacity);
                else {
                    color.a *= opacity;
                    brush_->SetColor(color);
                    docky::audio_output::Draw(dc_.Get(), brush_.Get(), center, shape);
                }
                const float tail = (managingOutputs_ ? (device.hidden ? 65.f : 25.f) : 6.f)
                    + (OutputScrollMax() > 0.0 ? 4.f : 0.f);
                Text(device.Label(), rowFmt_.Get(), D2D1::RectF(r.left + 39, r.top, r.right - tail, r.bottom), opacity);
                if (managingOutputs_) {
                    if (device.hidden)
                        Text(L"Hidden", sub_.Get(), D2D1::RectF(r.right - 62, r.top, r.right - 22, r.bottom), opacity);
                    Glyph(0xE712, 12.f, D2D1::Point2F(r.right - 12, center.y), a, Ink(), true);
                }
            }
            if (const double overflow = OutputScrollMax(); overflow > 0.0) {
                const float height = outputViewport_.bottom - outputViewport_.top - 8.f;
                const float thumb = std::max(16.f, height * 8.f / static_cast<float>(outputs_.size()));
                const float fraction = static_cast<float>(std::clamp(outputScroll_.Value() / overflow, 0.0, 1.0));
                const float top = outputViewport_.top + 4.f + (height - thumb) * fraction;
                const float x = outputViewport_.right - 2.f;
                brush_->SetColor(Ink(0.26f * a));
                dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, top, x + 2.f, top + thumb), 1, 1), brush_.Get());
            }
            dc_->PopAxisAlignedClip();
            brush_->SetColor(Ink(0.16f*a));dc_->DrawLine(D2D1::Point2F(sound_.r.left+8,settingsRow_.top-4),D2D1::Point2F(sound_.r.right-8,settingsRow_.top-4),brush_.Get(),1.f);
            if(hovered(Hit::Settings)){brush_->SetColor(Ink(0.10f*a));dc_->FillRoundedRectangle(D2D1::RoundedRect(settingsRow_,8,8),brush_.Get());}
            const auto footerLabel = Now() < statusUntil_ ? status_ : L"Sound Settings…";
            Text(footerLabel, rowFmt_.Get(),
                 D2D1::RectF(settingsRow_.left + 4, settingsRow_.top, settingsRow_.right - 4, settingsRow_.bottom), a);
        }
    } else {
        const bool wifi = page_ == Page::Wifi;
        DrawModuleBase(detail_, a, false, false);
        const float L = detail_.r.left, R = detail_.r.right;
        const auto ink = [&](float v) { return Ink(v); };
        if (!directDetail_) {
            if (hovered(Hit::Back) || pressedOn(Hit::Back)) {brush_->SetColor(Ink(0.12f*a));dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((back_.left+back_.right)*0.5f,(back_.top+back_.bottom)*0.5f),14,14),brush_.Get());}
            Glyph(0xE76B,12.0f,D2D1::Point2F((back_.left+back_.right)*0.5f,(back_.top+back_.bottom)*0.5f),a);
        }
        const float titleLeft = L + (directDetail_ ? 14.5f : 38.0f);
        Text(wifi ? L"Wi-Fi" : L"Bluetooth", menuTitle_.Get(), D2D1::RectF(titleLeft, detail_.r.top + 10, switch_.left - 8, detail_.r.top + 32), a, ink(0.92f));
        const bool on = wifi ? st_.wifiRadio == ctl::Radio::On : st_.btRadio == ctl::Radio::On;
        Switch(switch_, on, wifi ? ctl::CanToggleRadio(st_.wifiSwitchable,st_.wifiRadio) : ctl::CanToggleRadio(st_.btSwitchable,st_.btRadio), a);
        brush_->SetColor(Ink(0.10f * a));
        for (float y : menuSeparators_) dc_->FillRectangle(D2D1::RectF(L + kMenuInset, y, R - kMenuInset, y + 1.0f), brush_.Get());
        for (const auto& [cy, text] : menuHeaders_) Text(text, menuHead_.Get(), D2D1::RectF(L + kMenuInset + 0.5f, cy - 11, R - kMenuInset, cy + 11), a, ink(0.35f));
        auto hoverFill = [&](D2D1_RECT_F r) { brush_->SetColor(Ink(0.09f * a)); dc_->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), brush_.Get()); };
        for (size_t i = 0; i < rows_.size(); ++i) {
            const D2D1_RECT_F r = rows_[i];
            const int item = i < rowItem_.size() ? rowItem_[i] : -1;
            const float cy = (r.top + r.bottom) * 0.5f;
            const auto circle = D2D1::Point2F(L + 27.0f, cy);
            const D2D1_RECT_F label = D2D1::RectF(L + 48.5f, r.top, R - 34, r.bottom);
            if (item < 0) {
                const wchar_t* empty = wifi ? L"Searching for networks…" : L"No paired devices";
                Text(empty, menuRow_.Get(), D2D1::RectF(L + kMenuInset + 0.5f, r.top, R - kMenuInset, r.bottom), a, ink(0.45f));
                continue;
            }
            if (hover_ == Hit::Row && hoverRow_ == static_cast<int>(i)) hoverFill(r);
            const bool active = wifi ? st_.networks[item].connected : st_.btPaired[item].connected;
            // Circle: connected = system blue with a white glyph; otherwise a faint disc with a grey glyph.
            brush_->SetColor(active ? Accent(a) : Ink(0.10f * a));
            dc_->FillEllipse(D2D1::Ellipse(circle, 13, 13), brush_.Get());
            const auto glyph = active ? D2D1::ColorF(1, 1, 1, 1) : Ink(0.58f);
            if (wifi) {
                const auto& n = st_.networks[item];
                DrawWifi(circle, 10.0f, n.connected ? "wifi.connected" : "wifi.disconnected", glyph, a);
                const std::wstring name = !n.profile.empty() && n.profile != n.ssid ? n.ssid + L" (" + n.profile + L")" : n.ssid;
                Text(name, menuRow_.Get(), label, a, ink(0.92f));
                if (n.secured) Glyph(0xE72E, 12.0f, D2D1::Point2F(R - 20.0f, cy), a, ink(0.45f));
            } else {
                const auto& d = st_.btPaired[item];
                // Device kind glyphs (Segoe Fluent Icons); unknown kinds keep the Bluetooth rune.
                const wchar_t g = d.kind == ctl::BtKind::Keyboard ? 0xE765 : d.kind == ctl::BtKind::Mouse ? 0xE962 : d.kind == ctl::BtKind::Trackpad ? 0xEFA5
                                : d.kind == ctl::BtKind::Headphones ? 0xE7F6 : d.kind == ctl::BtKind::Speaker ? 0xE7F5 : d.kind == ctl::BtKind::Gamepad ? 0xE7FC
                                : d.kind == ctl::BtKind::Phone ? 0xE8EA : 0;
                if (g) Glyph(g, 13.0f, circle, a, glyph, active); else DrawBluetooth(circle, 13.0f, glyph, a);
                Text(d.name, menuRow_.Get(), D2D1::RectF(label.left, label.top, R - (d.battery >= 0 ? 86.0f : 14.0f), label.bottom), a, ink(active ? 0.92f : 0.38f));
                if (d.battery >= 0) {  // "55%" and a small battery, as macOS shows for accessories that report a level
                    const D2D1_RECT_F body = D2D1::RectF(R - 41.0f, cy - 5.0f, R - 20.0f, cy + 5.0f);
                    brush_->SetColor(Ink(0.40f * a)); dc_->FillRoundedRectangle(D2D1::RoundedRect(body, 3.0f, 3.0f), brush_.Get());
                    dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right + 1.0f, cy - 2.0f, body.right + 2.5f, cy + 2.0f), 0.7f, 0.7f), brush_.Get());
                    brush_->SetColor(Ink(0.85f * a));
                    dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left, body.top, body.left + (body.right - body.left) * d.battery / 100.0f, body.bottom), 3.0f, 3.0f), brush_.Get());
                    const auto old = menuRow_->GetTextAlignment(); menuRow_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    Text(std::to_wstring(d.battery) + L"%", menuRow_.Get(), D2D1::RectF(R - 96.0f, r.top, R - 45.0f, r.bottom), a, ink(0.55f));
                    menuRow_->SetTextAlignment(old);
                }
            }
        }
        if (otherNetworks_.bottom > otherNetworks_.top) {
            if (hover_ == Hit::OtherNetworks) hoverFill(otherNetworks_);
            Text(L"Other…", menuRow_.Get(), D2D1::RectF(L + kMenuInset + 0.5f, otherNetworks_.top, R - kMenuInset, otherNetworks_.bottom), a, ink(0.92f));
        }
        if (hover_ == Hit::Settings) hoverFill(settingsRow_);
        Text(wifi ? L"Wi-Fi Settings…" : L"Bluetooth Settings…", menuRow_.Get(),
             D2D1::RectF(L + kMenuInset + 0.5f, settingsRow_.top, R - kMenuInset, settingsRow_.bottom), a, ink(0.92f));
    }
    if (kbFocus_ != Hit::None) {  // keyboard focus ring (accent blue, macOS style)
        D2D1_RECT_F fr{};
        if (FocusRect(kbFocus_, kbRow_, fr)) {
            const float rr = (std::min)(12.0f, (fr.bottom - fr.top) * 0.5f);
            brush_->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.95f * a));
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(fr, rr, rr), brush_.Get(), 2.5f);
        }
    }
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (layered) {
        dc_->SetTarget(target_.Get());
        defocusFx_->SetInput(0, contentLayer_.Get());
        defocusFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, defocus * 0.5f);
        defocusFx_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        dc_->DrawImage(defocusFx_.Get());
    }
    dc_->EndDraw();
    // Pressed switch: glass lens over the freshly drawn track (lensThumb_ is in pre-transform DIPs; map it through
    // the same open-animation scale the D2D pass used).
    if (lensOpacity_ > 0.002f) { DrawSwitchLens(scaled(lensThumb_), lensOpacity_); DrawLensHighlights(lensThumb_, lensOpacity_, k, D2D1::Point2F(pivotX, pivotY)); }
    if (snapshotRequested_) SaveSnapshot();
    if (floatingScene_ && !colors_.Resolve(outputRtv_.Get(), scRgb_ ? colors_.WhiteForWindow(hwnd) : 0.f)) return;
    swap_->Present(1, 0);
    if (!tFirstPresent_) tFirstPresent_ = Ms();
    if (!lastGlass_ && !tFirstFallback_) { tFirstFallback_ = Ms(); DLOG(L"cc: first fallback presented ms=%.1f", tFirstFallback_); }
}

void Panel::SaveSnapshot() {
    // Test-only readback (LIQUID_CC_SNAPSHOT_DIR): the window is excluded from screen capture.
    snapshotRequested_ = false;
    if (snapshotDir_.empty()) return;
    ComPtr<ID3D11Texture2D> back, staging;
    if (floatingScene_) back = colors_.SdrSnapshot();
    else swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(d3d_->CreateTexture2D(&d, nullptr, &staging))) return;
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const std::wstring path = snapshotDir_ + L"\\cc-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++snapshotIndex_) + L".bmp";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = static_cast<LONG>(d.Width);
        ih.biHeight = -static_cast<LONG>(d.Height);
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
        DWORD wr = 0;
        WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
        WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
        for (UINT y = 0; y < d.Height; ++y) WriteFile(f, static_cast<BYTE*>(m.pData) + y * m.RowPitch, d.Width * 4, &wr, nullptr);
        CloseHandle(f);
        DLOG(L"cc: snapshot %ls (glass %d)", path.c_str(), lastGlass_);
    }
    ctx_->Unmap(staging.Get(), 0);
}

// ---------------------------------------------------------------------------------------------
Hit Panel::HitTest(float x, float y, int* row) {
    if (row) *row = -1;
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    if(soundOnly_){
        if(in(settingsRow_))return Hit::Settings;
        if (in(manageOutputsRow_)) return Hit::ManageOutputs;
        if (in(outputViewport_)) {
            const float offset = static_cast<float>(std::clamp(outputScroll_.Value(), 0.0, OutputScrollMax()));
            const auto index = static_cast<size_t>((y - outputViewport_.top + offset) / kOutputRowHeight);
            if (y < outputViewport_.bottom && index < outputs_.size()) {
                if (row) *row = static_cast<int>(index);
                return Hit::Row;
            }
        }
        if(st_.audio && in(muteBtn_))return Hit::Mute;
        if(st_.audio && in(sound_.r) && y>=soundTrack_.top-12 && y<=soundTrack_.bottom+12)return Hit::SoundSlider;
        return Hit::None;
    }
    if (page_ == Page::Main) {
        if (in(wifi_.r)) return x < wifi_.r.left + 54 ? Hit::WifiToggle : Hit::WifiBody;
        if (in(bt_.r)) return x < bt_.r.left + 54 ? Hit::BtToggle : Hit::BtBody;
        if (in(prevBtn_)) return Hit::MediaPrev;
        if (in(playBtn_)) return Hit::MediaPlay;
        if (in(nextBtn_)) return Hit::MediaNext;
        if (in(D2D1::RectF(media_.r.left + 10, media_.r.top + 10, media_.r.left + 58, media_.r.top + 58))) return Hit::MediaCover;
        if (in(media_.r)) return Hit::Media;
        if (in(airplane_.r)) return Hit::Airplane;
        if (in(focus_.r)) return Hit::Focus;
        if (in(stage_.r) && StageDock()) return Hit::Stage;
        if (in(battery_.r)) return Hit::Battery;
        if (st_.brightness && in(display_.r)) return y > display_.r.top + 30 ? Hit::DisplaySlider : Hit::DisplayTitle;
        if (in(muteBtn_)) return Hit::Mute;
        if (in(sound_.r)) return y > sound_.r.top + 30 ? Hit::SoundSlider : Hit::SoundTitle;
        return Hit::None;
    }
    if (!directDetail_ && in(back_)) return Hit::Back;
    if (in(D2D1::RectF(switch_.left - 6, switch_.top - 6, switch_.right + 6, switch_.bottom + 6))) return Hit::PageSwitch;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (in(rows_[i]) && i < rowItem_.size() && rowItem_[i] >= 0) {
            if (row) *row = static_cast<int>(i);
            return Hit::Row;
        }
    if (page_==Page::Wifi && otherNetworks_.bottom > otherNetworks_.top && in(otherNetworks_)) return Hit::OtherNetworks;
    if (in(settingsRow_)) return Hit::Settings;
    return Hit::None;
}

void Panel::NotifyBar() {
    if (notify_) PostMessageW(notify_, notifyMessage_, 0, 0);
}

std::vector<std::pair<Hit, int>> Panel::FocusOrder() const {
    std::vector<std::pair<Hit, int>> o;
    if (soundOnly_) {
        if (st_.audio) { o.push_back({Hit::SoundSlider, -1}); o.push_back({Hit::Mute, -1}); }
        o.push_back({Hit::ManageOutputs, -1});
        for (size_t i = 0; i < outputs_.size(); ++i) o.push_back({Hit::Row, static_cast<int>(i)});
        o.push_back({Hit::Settings, -1});
        return o;
    }
    if (page_ == Page::Main) {
        for (Hit h : {Hit::WifiToggle, Hit::WifiBody, Hit::BtToggle, Hit::BtBody, Hit::MediaPrev, Hit::MediaPlay, Hit::MediaNext,
                      Hit::Airplane, Hit::Focus, Hit::Battery})
            o.push_back({h, -1});
        if (StageDock()) o.push_back({Hit::Stage, -1});
        if (st_.brightness) o.push_back({Hit::DisplaySlider, -1});
        if (st_.audio) { o.push_back({Hit::SoundSlider, -1}); o.push_back({Hit::Mute, -1}); }
        return o;
    }
    if(!directDetail_)o.push_back({Hit::Back, -1});
    o.push_back({Hit::PageSwitch, -1});
    for (size_t i = 0; i < rows_.size(); ++i) if (i < rowItem_.size() && rowItem_[i] >= 0) o.push_back({Hit::Row, static_cast<int>(i)});
    if(page_==Page::Wifi && otherNetworks_.bottom > otherNetworks_.top)o.push_back({Hit::OtherNetworks,-1});
    o.push_back({Hit::Settings, -1});
    return o;
}

bool Panel::FocusRect(Hit h, int row, D2D1_RECT_F& r) const {
    auto grow = [](D2D1_RECT_F x, float d) { return D2D1::RectF(x.left - d, x.top - d, x.right + d, x.bottom + d); };
    auto toggle = [](const D2D1_RECT_F& m) { const float c = (m.top + m.bottom) * 0.5f; return D2D1::RectF(m.left + 7, c - 22, m.left + 51, c + 22); };
    auto body = [](const D2D1_RECT_F& m) { return D2D1::RectF(m.left + 54, m.top + 6, m.right - 6, m.bottom - 6); };
    switch (h) {
    case Hit::WifiToggle: r = toggle(wifi_.r); return true;
    case Hit::WifiBody: r = body(wifi_.r); return true;
    case Hit::BtToggle: r = toggle(bt_.r); return true;
    case Hit::BtBody: r = body(bt_.r); return true;
    case Hit::MediaPrev: r = grow(prevBtn_, 2); return true;
    case Hit::MediaPlay: r = grow(playBtn_, 2); return true;
    case Hit::MediaNext: r = grow(nextBtn_, 2); return true;
    case Hit::Airplane: r = grow(airplane_.r, -3); return true;
    case Hit::Focus: r = grow(focus_.r, -3); return true;
    case Hit::Stage: r = grow(stage_.r, -3); return true;
    case Hit::Battery: r = grow(battery_.r, -3); return true;
    case Hit::DisplaySlider: r = grow(displayTrack_, 9); return true;
    case Hit::SoundSlider: r = grow(soundTrack_, 9); return true;
    case Hit::Mute: r = grow(muteBtn_, 2); return true;
    case Hit::Back: r = grow(back_, 2); return true;
    case Hit::PageSwitch: r = grow(switch_, 4); return true;
    case Hit::OtherNetworks: r = otherNetworks_; return true;
    case Hit::Settings: r = settingsRow_; return true;
    case Hit::ManageOutputs: r = manageOutputsRow_; return true;
    case Hit::Row:
        if (soundOnly_) {
            if (row < 0 || static_cast<size_t>(row) >= outputs_.size()) return false;
            r = OutputRow(static_cast<size_t>(row));
            r.top = std::max(r.top, outputViewport_.top);
            r.bottom = std::min(r.bottom, outputViewport_.bottom);
            return r.top < r.bottom;
        }
        if (row >= 0 && static_cast<size_t>(row) < rows_.size()) { r = rows_[row]; return true; } return false;
    default: return false;
    }
}

void Panel::StepSlider(Hit h, int delta) {
    if (h == Hit::SoundSlider && st_.audio) {
        dragVolume_ = std::clamp((dragVolume_ >= 0 ? dragVolume_ : st_.volume) + delta, 0, 100);
        lastUserVolume_ = Now();
        worker_->SetVolume(dragVolume_);
        worker_->RequestQuery(false);
        NotifyBar();
    } else if (h == Hit::DisplaySlider && st_.brightness) {
        dragBrightness_ = std::clamp((dragBrightness_ >= 0 ? dragBrightness_ : st_.brightnessPercent) + delta, 0, 100);
        lastUserBrightness_ = Now();
        worker_->SetBrightness(dragBrightness_);
        worker_->RequestQuery(false);
    }
    needsFrame_ = true;
}

void Panel::SliderAt(Hit which, float x) {
    const D2D1_RECT_F& t = which == Hit::DisplaySlider ? displayTrack_ : soundTrack_;
    const float in = kSliderThumbInset;  // the thumb centre travels inside the track (see Slider)
    const int v = static_cast<int>(std::lround(std::clamp((x - t.left - in) / (t.right - t.left - 2 * in), 0.0f, 1.0f) * 100.0f));
    if (which == Hit::DisplaySlider) {
        if (v != dragBrightness_) {
            dragBrightness_ = v;
            worker_->SetBrightness(v);
            lastUserBrightness_ = Now();
        }
    } else if (v != dragVolume_) {
        dragVolume_ = v;
        worker_->SetVolume(v);
        if (st_.muted) worker_->Post([] { ctl::SetMute(false); });  // moving the slider unmutes, as on macOS
        lastUserVolume_ = Now();
    }
    needsFrame_ = true;
}

void Panel::Click(Hit h, int row) {
    auto settings = [&](const wchar_t* uri, const wchar_t* label) {
        DLOG(L"cc: open settings %ls", label);
        ctl::OpenUri(uri);
        Close(L"settings link");
    };
    auto radio = [&](bool wifi) {
        const bool switchable = wifi ? ctl::CanToggleRadio(st_.wifiSwitchable,st_.wifiRadio) : ctl::CanToggleRadio(st_.btSwitchable,st_.btRadio);
        if (!switchable) {
            settings(wifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth", wifi ? L"wifi (no switch)" : L"bluetooth (no switch)");
            return;
        }
        const bool on = (wifi ? st_.wifiRadio : st_.btRadio) == ctl::Radio::On;
        DLOG(L"cc: %ls radio -> %ls", wifi ? L"Wi-Fi" : L"Bluetooth", on ? L"off" : L"on");
        HWND w = hwnd;
        worker_->Post([wifi, on, w] {
            std::wstring err;
            const bool ok = ctl::SetRadio(wifi, !on, err);
            DLOG(L"cc: radio change %ls %ls", ok ? L"ok" : L"FAILED", err.c_str());
            if(!ok)PostMessageW(w,kMsgActionFailed,wifi?1:2,0);
        });
        status_ = std::wstring(wifi ? L"Wi-Fi " : L"Bluetooth ") + (on ? L"turning off…" : L"turning on…");
        statusUntil_ = Now() + 2.0;
        SetTimer(hwnd,kTimerStatus,2000,nullptr);
        worker_->RequestQuery(page_ == Page::Wifi);
        NotifyBar();
    };
    switch (h) {
    case Hit::WifiToggle: radio(true); break;
    case Hit::BtToggle: radio(false); break;
    case Hit::WifiBody:
        page_ = Page::Wifi;
        worker_->Post([] { ctl::ScanNetworks(); });
        worker_->RequestQuery(true);
        DLOG(L"cc: page wifi");
        break;
    case Hit::BtBody:
        page_ = Page::Bluetooth;
        worker_->RequestQuery(false);
        DLOG(L"cc: page bluetooth");
        break;
    case Hit::Back:
        page_ = Page::Main;
        DLOG(L"cc: page main");
        break;
    case Hit::PageSwitch: {
        const bool wifi = page_ == Page::Wifi;
        if (wifi ? ctl::CanToggleRadio(st_.wifiSwitchable, st_.wifiRadio) : ctl::CanToggleRadio(st_.btSwitchable, st_.btRadio)) {
            switchWant_ = SwitchStateOn() ? 0 : 1; switchWantUntil_ = Now() + 4.0; animating_ = true;
        }
        radio(wifi);
        break;
    }
    case Hit::Airplane: settings(L"ms-settings:network-airplanemode", L"airplane"); return;
    case Hit::Focus: settings(L"ms-settings:notifications", L"focus"); return;
    case Hit::Stage:
        if (const HWND dock = StageDock())
            PostMessageW(dock, kToggleStage, 0, reinterpret_cast<LPARAM>(hwnd));
        return;
    case Hit::Battery: settings(L"ms-settings:batterysaver", L"battery"); return;
    case Hit::DisplayTitle: settings(L"ms-settings:display", L"display"); return;
    case Hit::ManageOutputs:
        managingOutputs_ = !managingOutputs_;
        outputScroll_.SnapTo(0.0);
        RefreshOutputs();
        return;
    case Hit::SoundTitle: settings(L"ms-settings:sound", L"sound"); return;
    case Hit::OtherNetworks: ctl::OpenUri(L"ms-availablenetworks:"); Close(L"windows network flow"); return;
    case Hit::Settings: settings(soundOnly_ ? L"ms-settings:sound" : page_ == Page::Wifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth", soundOnly_ ? L"sound" : L"detail"); return;
    case Hit::Mute: {
        const bool m = !st_.muted;
        st_.muted = m;  // optimistic; the watcher/query confirms
        worker_->Post([m] { ctl::SetMute(m); });
        DLOG(L"cc: mute -> %d", m);
        NotifyBar();
        break;
    }
    case Hit::Media: {
        // The media card brings the playing app forward (as on macOS); its artwork opens full-screen Now Playing.
        const std::wstring app = st_.source;
        const bool ok = !app.empty() && ActivateMediaApp(app);
        DLOG(L"cc: open media app %ls %ls", app.c_str(), ok ? L"ok" : L"failed");
        Close(L"media app");
        return;
    }
    case Hit::MediaCover: {
        // The artwork opens the full-screen Now Playing view (docky-now-playing.exe beside this program).
        wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);std::wstring exe=self;exe=exe.substr(0,exe.find_last_of(L'\\'))+L"\\docky-now-playing.exe";
        const auto r=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",exe.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
        DLOG(L"cc: now playing %ls",r>32?L"opened":L"failed");
        Close(L"now playing");
        return;
    }
    case Hit::MediaPrev:
    case Hit::MediaPlay:
    case Hit::MediaNext: {
        if (!st_.media) break;
        const auto cmd = h == Hit::MediaPlay ? ctl::MediaCommand::PlayPause : h == Hit::MediaNext ? ctl::MediaCommand::Next : ctl::MediaCommand::Previous;
        worker_->Post([cmd] {
            const bool ok = ctl::SendMedia(cmd);
            DLOG(L"cc: media command %d %ls", static_cast<int>(cmd), ok ? L"ok" : L"refused");
        });
        worker_->RequestQuery(false);
        break;
    }
    case Hit::Row:
        if(soundOnly_ && row >= 0 && static_cast<size_t>(row) < outputs_.size()){
            if (managingOutputs_) { EditOutput(static_cast<size_t>(row)); return; }
            const auto device=outputs_[row];if(device.selected)break;HWND target=hwnd;
            worker_->Post([device,target]{if(!SelectOutput(device.id))PostMessageW(target,kMsgActionFailed,4,0);});
            worker_->RequestQuery(false);
            break;
        }
        if (page_ == Page::Bluetooth && row >= 0 && row < static_cast<int>(rowItem_.size()) && rowItem_[row] >= 0) {
            ctl::OpenUri(L"ms-settings:bluetooth");
            Close(L"bluetooth device settings");
            return;
        }
        if (page_ == Page::Wifi && row >= 0 && row < static_cast<int>(rowItem_.size()) && rowItem_[row] >= 0 &&
            rowItem_[row] < static_cast<int>(st_.networks.size())) {
            const auto n = st_.networks[rowItem_[row]];
            if (n.connected) break;
            if (n.profile.empty()) {
                // Unknown network: credentials are entered in Windows' own secure flow, never here.
                DLOG(L"cc: network without saved profile -> Windows network flyout");
                ctl::OpenUri(L"ms-availablenetworks:");
                Close(L"windows network flow");
                return;
            }
            DLOG(L"cc: connecting with saved profile");
            HWND target=hwnd;
            worker_->Post([n,target] {
                std::wstring err;
                const bool ok = ctl::ConnectNetwork(n, err);
                DLOG(L"cc: connect %ls %ls", ok ? L"requested" : L"FAILED", err.c_str());
                if(!ok)PostMessageW(target,kMsgActionFailed,3,0);
            });
            status_ = L"Connecting to " + n.ssid + L"…";
            statusUntil_ = Now() + 4.0;
            SetTimer(hwnd,kTimerStatus,4000,nullptr);
            worker_->RequestQuery(true);
        }
        break;
    default: break;
    }
    Layout();
    Place();
    ResizeTarget();
    needsFrame_ = animating_ = true;
}

void Panel::Close(const wchar_t* why) {
    if (closing_) return;
    closing_ = true;
    transitionFrom_ = openT_;
    transitionAt_ = Now();
    openTarget_ = 0;
    animating_ = true;
    DLOG(L"cc: closing (%ls)", why);
}

// ---------------------------------------------------------------------------------------------
LRESULT CALLBACK Panel::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* p = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        p->hwnd = h;
    }
    auto* p = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return p ? p->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Panel::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE: {
        mx_ = GET_X_LPARAM(l) / scale_;
        my_ = GET_Y_LPARAM(l) / scale_;
        if (pressed_ == Hit::PageSwitch) {  // drag the switch thumb 1:1 (Lab behaviour)
            if (!switchDragging_ && std::fabs(mx_ - switchPressX_) > 4.0f) switchDragging_ = true;
            if (switchDragging_) {
                const float q = (switch_.bottom - switch_.top) / 50.0f, travel = (switch_.right - switch_.left) - 80.0f * q;
                switchDragPos_ = std::clamp(static_cast<double>(switchStartPos_ + (mx_ - switchPressX_) / (std::max)(1.0f, travel)), 0.0, 1.0);
            }
            needsFrame_ = animating_ = true;
            return 0;
        }
        if (dragging_) {
            SliderAt(pressed_, mx_);
            return 0;
        }
        int row = -1;
        const Hit h = HitTest(mx_, my_, &row);
        if (h != hover_ || row != hoverRow_) {
            hover_ = h;
            hoverRow_ = row;
            needsFrame_ = true;
        }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!dragging_ && hover_ != Hit::None) {
            hover_ = Hit::None;
            needsFrame_ = true;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        kbFocus_ = Hit::None; kbRow_ = -1;
        SetCapture(hwnd);
        mx_ = GET_X_LPARAM(l) / scale_;
        my_ = GET_Y_LPARAM(l) / scale_;
        pressed_ = HitTest(mx_, my_, &pressedRow_);
        hover_ = pressed_;
        if (pressed_ == Hit::None) {  // the window's blur padding is outside the panel: a click there closes, like any outside click
            bool inside = false;
            for (const auto& m : modules_) if (mx_ >= m.r.left && mx_ <= m.r.right && my_ >= m.r.top && my_ <= m.r.bottom) inside = true;
            if (!inside) { ReleaseCapture(); Close(L"clicked outside (padding)"); return 0; }
        }
        if (pressed_ == Hit::PageSwitch) {
            switchPressX_ = mx_; switchDragging_ = false;
            switchStartPos_ = static_cast<float>(std::clamp(switchOn_.Value(), 0.0, 1.0)); switchDragPos_ = switchStartPos_;
            animating_ = true;
        }
        if (pressed_ == Hit::SoundSlider || pressed_ == Hit::DisplaySlider) {
            if ((pressed_ == Hit::SoundSlider && st_.audio) || pressed_ == Hit::DisplaySlider) {
                dragging_ = true;
                SliderAt(pressed_, mx_);
            }
        }
        needsFrame_ = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        const Hit p = pressed_;
        const int prow = pressedRow_;
        const bool wasDragging = dragging_;
        if (p == Hit::PageSwitch && switchDragging_) {  // released after dragging: past the middle switches
            switchDragging_ = false; pressed_ = Hit::None; animating_ = needsFrame_ = true;
            if (GetCapture() == hwnd) ReleaseCapture();
            if ((switchDragPos_ >= 0.5) != SwitchStateOn()) Click(Hit::PageSwitch, -1);
            return 0;
        }
        pressed_ = Hit::None;
        dragging_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (wasDragging) {
            DLOG(L"cc: %ls set to %d%%", p == Hit::DisplaySlider ? L"brightness" : L"volume", p == Hit::DisplaySlider ? dragBrightness_ : dragVolume_);
            worker_->RequestQuery(false);
            NotifyBar();
            needsFrame_ = true;
            return 0;
        }
        int row = -1;
        const Hit h = HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_, &row);
        if (h == p && h != Hit::None && h != Hit::SoundSlider && h != Hit::DisplaySlider && (h != Hit::Row || row == prow)) Click(h, row);
        needsFrame_ = true;
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) {
            dragging_ = false;
            pressed_ = Hit::None;
            worker_->RequestQuery(false);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (soundOnly_ && OutputScrollMax() > 0.0) {
            POINT cursor{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &cursor);
            const float x = cursor.x / scale_, y = cursor.y / scale_;
            if (x < outputViewport_.left || x >= outputViewport_.right ||
                y < outputViewport_.top || y >= outputViewport_.bottom) break;
            const double delta = GET_WHEEL_DELTA_WPARAM(w) / static_cast<double>(WHEEL_DELTA);
            const double target = std::clamp(outputScroll_.Target() - delta * kOutputRowHeight, 0.0, OutputScrollMax());
            if (reducedMotion_) outputScroll_.SnapTo(target);
            else outputScroll_.SetTarget(target);
            if (pressed_ == Hit::Row) pressed_ = Hit::None;
            hover_ = Hit::None; hoverRow_ = -1;
            kbFocus_ = Hit::None; kbRow_ = -1;
            needsFrame_ = true;
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            const auto order = FocusOrder();
            if (!order.empty()) {
                int at = -1;
                for (size_t i = 0; i < order.size(); ++i) if (order[i].first == kbFocus_ && order[i].second == kbRow_) at = static_cast<int>(i);
                const int n = static_cast<int>(order.size()), step = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                at = at < 0 ? (step > 0 ? 0 : n - 1) : (at + step + n) % n;
                kbFocus_ = order[at].first; kbRow_ = order[at].second;
                if (soundOnly_ && kbFocus_ == Hit::Row) {
                    const double top = kbRow_ * kOutputRowHeight;
                    const double height = outputViewport_.bottom - outputViewport_.top;
                    const double offset = std::clamp(outputScroll_.Value(), 0.0, OutputScrollMax());
                    outputScroll_.SnapTo(std::clamp(offset, std::max(0.0, top + kOutputRowHeight - height),
                                                  std::min(top, OutputScrollMax())));
                }
                DLOG(L"cc: keyboard focus %d row %d", static_cast<int>(kbFocus_), kbRow_);
                needsFrame_ = true;
            }
            return 0;
        }
        if ((w == VK_RETURN || w == VK_SPACE) && kbFocus_ != Hit::None && kbFocus_ != Hit::SoundSlider && kbFocus_ != Hit::DisplaySlider) {
            const Hit h = kbFocus_; const int row = kbRow_;
            DLOG(L"cc: keyboard activate %d row %d", static_cast<int>(h), row);
            if (h != Hit::Mute) { kbFocus_ = Hit::None; kbRow_ = -1; }  // page changes rebuild the focus order
            Click(h, row);
            needsFrame_ = true;
            return 0;
        }
        if ((w == VK_LEFT || w == VK_RIGHT) && (kbFocus_ == Hit::SoundSlider || kbFocus_ == Hit::DisplaySlider)) {
            StepSlider(kbFocus_, w == VK_RIGHT ? 5 : -5);
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (page_ != Page::Main && !directDetail_) Click(Hit::Back, -1);
            else Close(L"Escape");
        } else if(soundOnly_ && st_.audio && (w==VK_LEFT||w==VK_RIGHT)) {
            dragVolume_=std::clamp((dragVolume_>=0?dragVolume_:st_.volume)+(w==VK_RIGHT?5:-5),0,100);
            lastUserVolume_=Now();worker_->SetVolume(dragVolume_);worker_->RequestQuery(false);needsFrame_=true;
        } else if(soundOnly_ && st_.audio && w==VK_SPACE) {
            Click(Hit::Mute,-1);
        } else if (w == VK_F12 && !snapshotDir_.empty()) {
            snapshotRequested_ = true;
            needsFrame_ = true;
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            PostMessageW(hwnd, liquid::popup::DismissWatch::FocusMessage, 0, 0);
        return 0;
    case liquid::popup::DismissWatch::FocusMessage:
        if (!outputEditorOpen_ && dismissal_.LostFocus()) Close(L"lost focus");
        return 0;
    case liquid::popup::DismissWatch::ClickMessage: {
        RECT rect{};
        const POINT point{static_cast<LONG>(w), static_cast<LONG>(l)};
        if (!outputEditorOpen_ && GetWindowRect(hwnd, &rect) && !PtInRect(&rect, point)) Close(L"clicked outside");
        return 0;
    }
    case kStageChanged: needsFrame_ = true; return 0;
    case kMsgState: {
        ctl::State s = worker_->Latest();
        // While (or just after) the user drags a slider, keep the local value to avoid jitter.
        if (dragging_ || Now() - lastUserVolume_ < 0.8) { s.volume = dragVolume_ >= 0 ? dragVolume_ : s.volume; }
        else dragVolume_ = -1;
        if (dragging_ || Now() - lastUserBrightness_ < 1.5) { s.brightnessPercent = dragBrightness_ >= 0 ? dragBrightness_ : s.brightnessPercent; }
        else dragBrightness_ = -1;
        const bool layoutChange = !haveState_ || s.brightness != st_.brightness || s.networks.size() != st_.networks.size() ||
                                  s.btDevices.size() != st_.btDevices.size();
        if (!haveState_)
            DLOG(L"cc: state wifi=%d/%d bt=%d/%d audio=%d vol=%d muted=%d brightness=%d/%d battery=%d/%d media=%d",
                 static_cast<int>(s.wifiRadio), s.wifiSwitchable, static_cast<int>(s.btRadio), s.btSwitchable, s.audio, s.volume, s.muted,
                 s.brightness, s.brightnessPercent, s.battery, s.batteryPercent, s.media);
        if (haveState_ && (s.volume != st_.volume || s.muted != st_.muted || s.wifiRadio != st_.wifiRadio || s.btRadio != st_.btRadio))
            DLOG(L"cc: state sync wifi=%d bt=%d vol=%d muted=%d", static_cast<int>(s.wifiRadio), static_cast<int>(s.btRadio), s.volume, s.muted);
        // Explicit isolated visual tests only: exercise artwork and both transport states.
        wchar_t mediaFixture[16]{};
        if(g_visualCaptureTest && !snapshotDir_.empty() && GetEnvironmentVariableW(L"LIQUID_CC_DEV_MEDIA",mediaFixture,16)) {
            s.media=true;s.playing=!_wcsicmp(mediaFixture,L"playing");s.canPlayPause=s.canNext=s.canPrevious=true;
            s.title=L"Your Fault";s.artist=L"Excision, SLANDER";
            auto pixels=std::make_shared<std::vector<uint32_t>>(256*256);
            for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)(*pixels)[y*256+x]=0xff000000u|((20+x/3)<<16)|((32+y/2)<<8)|(80+x/2);
            s.artwork=pixels;s.artworkWidth=s.artworkHeight=256;s.artworkHash=1;
        }
        st_ = std::move(s);
        if (soundOnly_) {
            activeOutputs_ = worker_->LatestOutputs();
            RefreshOutputs();
        }
        haveState_ = true;
        if (layoutChange) {
            Layout();
            Place();
            ResizeTarget();
        }
        needsFrame_ = true;
        return 0;
    }
    case kMsgActionFailed:
        DLOG(L"cc: action failure feedback type=%llu",static_cast<unsigned long long>(w));
        status_=w==1?L"Could not change Wi-Fi.":w==2?L"Could not change Bluetooth.":w==4?L"Could not switch output. Open Sound Settings.":L"Could not request a connection.";
        statusUntil_=Now()+4.0;SetTimer(hwnd,kTimerStatus,4000,nullptr);needsFrame_=true;
        worker_->RequestQuery(page_==Page::Wifi);return 0;
    case kMsgVolume:
        worker_->RequestQuery(false);  // the system volume changed (any app): resync
        return 0;
    case kMsgAnchor:
        {std::lock_guard<std::mutex> lock(g_anchorMutex);anchor_=g_anchor;}
        Place();needsFrame_=true;return 0;
    case WM_CLOSE:
    case kMsgClose:
        if (outputEditorOpen_) {
            closeAfterOutputEditor_ = true;
            const HWND dialog = GetLastActivePopup(hwnd);
            if (dialog && dialog != hwnd && GetWindow(dialog, GW_OWNER) == hwnd)
                PostMessageW(dialog, WM_CLOSE, 0, 0);
            EndMenu();
            return 0;
        }
        if (m == WM_CLOSE) break;
        Close(L"toggle");
        return 0;
    case dock::kMsgCaptureAvailable:
        if (backdrop_.ProcessCapture(windowRect_)) needsFrame_ = true;
        return 0;
    case WM_TIMER:
        if (w == kTimerStatus) {
            const double remaining=statusUntil_-Now();
            if(remaining>0) SetTimer(hwnd,kTimerStatus,static_cast<UINT>(std::max(1.0,remaining*1000.0+1.0)),nullptr);
            else {KillTimer(hwnd,kTimerStatus);needsFrame_=true;DLOG(L"cc: transient status expired");}
        }
        else if (w == kTimerRefresh) worker_->RequestQuery(page_ == Page::Wifi);
        else if (w == kTimerHealth) {if(RefreshTheme()){backdrop_.Request(!forceOpaque_&&!g_visualCaptureTest,MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST));needsFrame_=true;}if(!forceOpaque_&&!g_visualCaptureTest&&backdrop_.CheckHealth())needsFrame_=true;}
        else if (w == kTimerWgcStart) {
            KillTimer(hwnd, kTimerWgcStart);
            const double begin = Ms();
            backdrop_.Request(!forceOpaque_&&!g_visualCaptureTest, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            tWgc_ = Ms();
            DLOG(L"cc: deferred wgc start ms=%.1f end=%.1f duration=%.1f", begin, tWgc_, tWgc_ - begin);
            needsFrame_ = true;
        }
        else if (w == kTimerSnapshot) {
            KillTimer(hwnd, kTimerSnapshot);
            snapshotRequested_ = true;
            needsFrame_ = true;
        }
        return 0;
    case WM_SETTINGCHANGE: {
        BOOL animations=TRUE;
        if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0) && reducedMotion_ != !animations){reducedMotion_=!animations;animating_=needsFrame_=true;}
        break;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
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
        dismissal_.Stop();
        PostQuitMessage(0);
        hwnd = nullptr;
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

void Panel::Frame() {
    const double now = Now();
    if (!tFirstFrame_) tFirstFrame_ = Ms();
    if (prevFrame_ > 0 && frames_ < 20) maxGap_ = (std::max)(maxGap_, (now - prevFrame_) * 1000.0);
    prevFrame_ = now; ++frames_;
    if (!timingLogged_ && frames_ == 20) { timingLogged_ = true;
        DLOG(L"cc: timing ms window=%.1f device=%.1f swap+d2d=%.1f dcomp=%.1f text=%.1f glass=%.1f wgc_request=%.1f shown=%.1f first_frame=%.1f first_present=%.1f first_capture=%.1f max_gap_first20=%.1f",
             tWindow_, tDevice_, tSwapD2D_, tDcomp_, tText_, tGlass_, tWgc_, tShown_, tFirstFrame_, tFirstPresent_, tFirstCapture_, maxGap_); }
    const double dt = std::clamp(now - lastT_, 0.0, 0.05);
    lastT_ = now;
    // Spring-driven open/close (exact for any dt; closing mid-open continues from the live value).
    // Hold the reveal until the first captured backdrop frame exists (or 250 ms passed), so the first
    // visible frame is already liquid glass instead of the translucent fallback plate.
    const bool waitingForGlass = !closing_ && !forceOpaque_ && !tBackdropReady_ && (now - t0_) < 0.25;
    open_.SetSpec(closing_ ? kCloseSpring : kOpenSpring);
    open_.SetTarget(openTarget_);
    if (reducedMotion_) open_.SnapTo(waitingForGlass ? 0.0 : openTarget_);
    else if (!waitingForGlass) open_.Step(dt);
    openT_ = open_.F();
    if (open_.AtRest() && !waitingForGlass) openT_ = openTarget_;
    if (lastGlass_) glassFade_ = reducedMotion_ ? 1.f : (std::min)(1.f, glassFade_ + static_cast<float>(dt / 0.16));
    if (!closing_ && openT_ == 1.f && !tAnimDone_) { tAnimDone_ = Ms(); DLOG(L"cc: opening animation complete ms=%.1f", tAnimDone_); }
    const bool switchMoving = page_ != Page::Main && UpdateSwitchMotion(dt);
    const bool sliderMoving = page_ == Page::Main && UpdateSliderMotion(dt);
    if (reducedMotion_) outputScroll_.SnapTo(outputScroll_.Target());
    else outputScroll_.Step(dt);
    animating_ = !outputScroll_.AtRest() || waitingForGlass || switchMoving || sliderMoving || openT_ != openTarget_ || (lastGlass_ && glassFade_ < 1.f);
    if (closing_ && openT_ <= 0.02f) {
        const bool restore=GetForegroundWindow()==hwnd;
        DestroyWindow(hwnd);
        if(restore && previous_ && IsWindow(previous_))SetForegroundWindow(previous_);
        return;
    }
    Render();
    needsFrame_ = false;
    if (!volumeStarted_) { volumeStarted_ = true; volumeWatch_.Start(hwnd, kMsgVolume); }  // after the first frame (see Run)
}

int Panel::Run(RECT anchor, HWND notify, UINT notifyMessage, bool soundOnly, Page initialPage) {
    soundOnly_=soundOnly;page_=initialPage;directDetail_=initialPage!=Page::Main;RefreshTheme();
    if (soundOnly_) {
        outputSettingsPath_ = g_settingsPath.empty() ? liquid::settings::DefaultPath() : g_settingsPath;
        outputPreferences_ = liquid::settings::Load(outputSettingsPath_).value.controlCenter.audioOutputs;
        outputScroll_.SetSpec(docky::motion::tokens::Scroll);
        outputScroll_.SetRest(0.12, 1.0);
    }
    BOOL animations=TRUE;if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0))reducedMotion_=!animations;
    {std::lock_guard<std::mutex> lock(g_anchorMutex);anchor_=g_anchor;above_=g_openAbove;}
    notify_ = notify;
    notifyMessage_ = notifyMessage;
    previous_ = GetForegroundWindow();
    t0_ = Now();
    wchar_t dir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LIQUID_CC_SNAPSHOT_DIR", dir, MAX_PATH)) snapshotDir_ = dir;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass, L"DockOver Control Center", WS_POPUP, anchor.left,
                    anchor.bottom, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return 1;
    tWindow_ = Ms();
    scale_ = GetDpiForWindow(hwnd) / 96.0f;
    worker_ = std::make_unique<Worker>(hwnd,soundOnly_);
    if(page_==Page::Wifi)worker_->Post([] { ctl::ScanNetworks(); });
    worker_->RequestQuery(page_==Page::Wifi);
    Layout();
    Place();
    if (FAILED(InitGraphics())) {
        DLOG(L"cc: graphics init failed");
        DestroyWindow(hwnd);
        return 1;
    }
    // The volume watcher (an audio endpoint callback, ~20 ms to register) starts after the first frame (Frame); the
    // worker's first status query already brings the current volume.
    if(g_visualCaptureTest && !SetWindowDisplayAffinity(hwnd,WDA_NONE)){DestroyWindow(hwnd);return 2;}
    ShowWindow(hwnd, SW_SHOW);
    tShown_ = Ms();
    SetForegroundWindow(hwnd);  // keyboard (Esc) and outside-click via deactivation; restored on close
    dismissal_.Start(hwnd);
    SetTimer(hwnd, kTimerRefresh, 1500, nullptr);  // state polling only; animation uses the frame clock
    SetTimer(hwnd, kTimerHealth, 1000, nullptr);
    if (!wgcStarted_) SetTimer(hwnd, kTimerWgcStart, 80, nullptr);
    if (!snapshotDir_.empty()) SetTimer(hwnd, kTimerSnapshot, 900, nullptr);
    {   // Development: LIQUID_CC_DEV_PRESS=sound|display shows that slider pressed (no value is changed) for the snapshot.
        wchar_t press[16] = {};
        if (GetEnvironmentVariableW(L"LIQUID_CC_DEV_PRESS", press, 16)) {
            if (!_wcsicmp(press, L"sound")) pressed_ = Hit::SoundSlider; else if (!_wcsicmp(press, L"display")) pressed_ = Hit::DisplaySlider;
            needsFrame_ = true;
        }
    }
    lastT_ = transitionAt_ = Now();
    transitionFrom_ = 0.f;
    DLOG(L"cc: opening animation start ms=%.1f", Ms());
    DLOG(L"cc: open at (%ld,%ld) %ux%u px, icon font %ls, glass %ls", windowRect_.left, windowRect_.top, pxW_, pxH_, iconFamily_.c_str(),
         glassOk_ ? L"liquid" : L"unavailable");
    MSG msg{};
    docky::vblank::Gate gate;  // Settings > General > Frame rate
    HANDLE paceTimer = docky::vblank::CreatePaceTimer();
    bool paceArmed = false;
    for (;;) {
        if (!(animating_ || needsFrame_)) {
            HANDLE ev = backdrop_.CompletionEvent();
            const DWORD r = MsgWaitForMultipleObjectsEx(ev ? 1 : 0, ev ? &ev : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (ev && r == WAIT_OBJECT_0) {
                if (backdrop_.CompleteCompare(windowRect_)) { needsFrame_ = true; if (!tFirstCapture_) tFirstCapture_ = Ms(); }
                continue;
            }
        }
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit || !hwnd) break;
        if (!(animating_ || needsFrame_)) continue;
        HANDLE handles[2];
        DWORD count = 0;
        if (paceArmed) handles[count++] = paceTimer;  // a frame that came too early under the frame-rate cap
        else if (waitable_) handles[count++] = waitable_;
        HANDLE ev = backdrop_.CompletionEvent();
        if (ev) handles[count++] = ev;
        const DWORD r = MsgWaitForMultipleObjectsEx(count, handles, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ev && r == WAIT_OBJECT_0 + count - 1) {
            if (backdrop_.CompleteCompare(windowRect_)) { needsFrame_ = true; if (!tFirstCapture_) tFirstCapture_ = Ms(); }
            continue;
        }
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) {
            if (paceArmed && r == WAIT_OBJECT_0) { paceArmed = false; Frame(); gate.Mark(); continue; }
            if (paceArmed) continue;
            if (const double left = gate.Remaining(); left > 0 && paceTimer) { docky::vblank::Arm(paceTimer, left); paceArmed = true; continue; }
            Frame(); gate.Mark();
        }
    }
    if (paceTimer) CloseHandle(paceTimer);
    volumeWatch_.Stop();
    backdrop_.Request(false, nullptr);
    backdrop_.SetDevice(nullptr);
    // A Wi-Fi scan or Bluetooth lookup in progress can take tens of seconds (measured 36 s, 2026-11-21). Joining it here
    // kept the panel thread alive after its window was gone, and every click on Wi-Fi, sound or Control Center was
    // ignored meanwhile (g_running). The worker owns everything it touches, so it finishes on its own thread.
    if (worker_) std::thread([w = std::move(worker_)]() mutable { w.reset(); }).detach();
    if (glass_) glass_->ReleaseGpuResources();
    if (ctx_) { ctx_->ClearState(); ctx_->Flush(); }  // shared device: leave no bindings for the next opening
    if (waitable_) CloseHandle(waitable_);
    if (previous_ && IsWindow(previous_) && GetForegroundWindow() == nullptr) SetForegroundWindow(previous_);
    DLOG(L"cc: closed");
    return 0;
}

// ---------------------------------------------------------------------------------------------
std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<HWND> g_notify{nullptr};
UINT g_notifyMessage = 0;

}  // namespace

void SetSettingsPath(const std::wstring& path){g_settingsPath=path;}
void SetWorkshopRoot(const std::wstring& path){g_workshopRoot=path;}
void SetVisualCaptureTest(bool enabled){g_visualCaptureTest=enabled;}

void TogglePanel(const RECT& anchor, HWND notify, UINT notifyMessage, bool soundOnly, Page initialPage) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HWND h = FindWindowW(kClass, L"DockOver Control Center")) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) {
            PostMessageW(h, kMsgClose, 0, 0);
            return;
        }
    }
    if (g_running.load()) return;
    if (g_thread.joinable()) g_thread.join();
    {std::lock_guard<std::mutex> anchorLock(g_anchorMutex);g_anchor=anchor;}
    g_running.store(true);
    g_notify.store(notify);
    g_notifyMessage = notifyMessage;
    g_thread = std::thread([anchor, notify, notifyMessage, soundOnly, initialPage] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            Panel p;
            p.Run(anchor, notify, notifyMessage, soundOnly, initialPage);
        }
        if (SUCCEEDED(co)) CoUninitialize();
        g_running.store(false);
        if (notify) PostMessageW(notify, notifyMessage, 1, 0);  // closed
    });
}

void Toggle(const RECT& anchor, HWND notify, UINT notifyMessage) { TogglePanel(anchor,notify,notifyMessage,false,Page::Main); }
void ToggleSound(const RECT& anchor, HWND notify, UINT notifyMessage) { TogglePanel(anchor,notify,notifyMessage,true,Page::Main); }
void ToggleWifi(const RECT& anchor, HWND notify, UINT notifyMessage) { TogglePanel(anchor,notify,notifyMessage,false,Page::Wifi); }
void ToggleBluetooth(const RECT& anchor, HWND notify, UINT notifyMessage) { TogglePanel(anchor,notify,notifyMessage,false,Page::Bluetooth); }
void SetOpenAbove(bool above) { std::lock_guard<std::mutex> lock(g_anchorMutex); g_openAbove = above; }
void UpdateAnchor(const RECT& anchor) {
    {std::lock_guard<std::mutex> lock(g_anchorMutex);if(EqualRect(&g_anchor,&anchor))return;g_anchor=anchor;}
    if(HWND h=FindWindowW(kClass,L"DockOver Control Center")){DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())PostMessageW(h,kMsgAnchor,0,0);}
}
bool IsOpen() { return g_running.load(); }

void Prewarm() {
    std::thread([] { ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c; const double t = Now();
        const HRESULT hr = SharedDevice(d, c); DLOG(L"cc: prewarm device hr=0x%08X in %.1f ms", static_cast<unsigned>(hr), (Now() - t) * 1000.0); }).detach();
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (HWND h = FindWindowW(kClass, L"DockOver Control Center")) PostMessageW(h, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace liquid::cc
