#include "notification_center.h"
#include "ui_language.h"
#include "notification_card_metrics.h"

#include "backdrop_wgc.h"
#include "design_tokens.h"
#include "desktop_settings.h"
#include "glass_params.h"
#include "log.h"
#include "material.h"
#include "motion.h"
#include "popup_dismiss_watch.h"
#include "theme_state.h"
#include "custom_icons.h"
#include "icon_mask.h"
#include "shell_items.h"

#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <shcore.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propkey.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace docky::notify {
namespace {

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---------------------------------------------------------------------------------------------------------------
// Data
struct IconImage { std::vector<std::uint32_t> px; UINT side = 0; };  // premultiplied BGRA, square
struct Item {
    std::uint32_t id = 0;
    std::wstring aumid, app, title, body;
    std::int64_t created = 0;  // FILETIME ticks (UTC)
    std::shared_ptr<const IconImage> icon;  // the app's icon as the Dock shows it; null = letter placeholder
    bool synthetic = false;    // DebugBanner: not a Windows notification
};
constexpr UINT kIconPx = 96;

enum class Access { Off, Pending, Allowed, Denied, Unavailable };
std::wstring& IconSettingsPath() { static std::wstring path; return path; }  // set once by Start(), before the reader runs

constexpr UINT kMsgItems = WM_APP + 301, kMsgToggle = WM_APP + 302, kMsgCapture = WM_APP + 303, kMsgDebug = WM_APP + 304,
               kMsgCloseCenter = WM_APP + 305, kMsgHideSystem = WM_APP + 306, kMsgBelowBar = WM_APP + 307;

// ---------------------------------------------------------------------------------------------------------------
// Reader: one MTA thread polling Windows' notification list (the change event needs a packaged background task).
class Reader {
public:
    explicit Reader(HWND host) : host_(host) { thread_ = std::thread([this] { Run(); }); }
    ~Reader() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void SetEnabled(bool on) { { std::lock_guard<std::mutex> l(m_); enabled_ = on; } cv_.notify_all(); }
    void Remove(std::uint32_t id) { { std::lock_guard<std::mutex> l(m_); removals_.push_back(id); } cv_.notify_all(); }
    void ClearAll() { { std::lock_guard<std::mutex> l(m_); clearAll_ = true; } cv_.notify_all(); }
    void Poll() { { std::lock_guard<std::mutex> l(m_); poke_ = true; } cv_.notify_all(); }
    void Latest(std::vector<Item>& items, Access& access) { std::lock_guard<std::mutex> l(m_); items = items_; access = access_; }

private:
    void Run();
    void Publish(std::vector<Item> items, Access access) {
        { std::lock_guard<std::mutex> l(m_); items_ = std::move(items); access_ = access; }
        PostMessageW(host_, kMsgItems, 0, 0);
    }
    std::shared_ptr<const std::vector<std::uint32_t>> Logo(const winrt::Windows::ApplicationModel::AppInfo& info);
    std::shared_ptr<const IconImage> Icon(const winrt::Windows::ApplicationModel::AppInfo& info, const std::wstring& aumid);
    void RefreshIconSources();

    HWND host_;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false, enabled_ = false, clearAll_ = false, poke_ = false;
    std::vector<std::uint32_t> removals_;
    // Notifications Windows would not remove for DockOver (it refuses ClearNotifications / RemoveNotification for
    // apps without package identity): hidden from DockOver's list until Windows drops them itself.
    std::set<std::uint32_t> dismissed_;
    std::set<std::uint32_t> lastIds_;  // ids in the last Windows list, so a Clear All can dismiss what stays
    std::vector<Item> items_;
    Access access_ = Access::Off;
    std::map<std::wstring, std::shared_ptr<const IconImage>> logos_;  // by AUMID
    ComPtr<IWICImagingFactory> wic_;
    // The Dock's icons: its pinned items (resolved once per pinned.txt change), custom-icons.json and the mask setting.
    std::vector<dock::PinnedItem> pinned_;
    dock::CustomIconStore store_;
    bool mask_ = true, dark_ = true;
    std::string sourcesKey_;
};

std::shared_ptr<const std::vector<std::uint32_t>> Reader::Logo(const winrt::Windows::ApplicationModel::AppInfo& info) {
    std::shared_ptr<const std::vector<std::uint32_t>> result;
    try {
        if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
        // Null projections are not exceptions in C++/WinRT: calling through one crashes, so check each step.
        auto display = info ? info.DisplayInfo() : nullptr;
        auto ref = display ? display.GetLogo(winrt::Windows::Foundation::Size{static_cast<float>(kIconPx), static_cast<float>(kIconPx)}) : nullptr;
        auto stream = ref ? ref.OpenReadAsync().get() : nullptr;
        if (!stream) return nullptr;
        ComPtr<IStream> is;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> conv;
        if (wic_ && SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&is))) &&
            SUCCEEDED(wic_->CreateDecoderFromStream(is.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), kIconPx, kIconPx, WICBitmapInterpolationModeHighQualityCubic)) &&
            SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            auto px = std::make_shared<std::vector<std::uint32_t>>(static_cast<size_t>(kIconPx) * kIconPx);
            if (SUCCEEDED(conv->CopyPixels(nullptr, kIconPx * 4, kIconPx * kIconPx * 4, reinterpret_cast<BYTE*>(px->data())))) result = px;
        }
    } catch (...) {
    }
    return result;
}

namespace {
std::wstring Expand(const std::wstring& s) {
    wchar_t out[32768]{};
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), out, ARRAYSIZE(out));
    return n && n < ARRAYSIZE(out) ? std::wstring(out) : s;
}
std::wstring FullPath(const std::wstring& p) {
    if (p.empty()) return {};
    wchar_t out[MAX_PATH * 4]{};
    const DWORD n = GetFullPathNameW(Expand(p).c_str(), ARRAYSIZE(out), out, nullptr);
    return n && n < ARRAYSIZE(out) ? std::wstring(out) : p;
}
bool EndsWithExe(const std::wstring& s) { return s.size() > 4 && !_wcsicmp(s.c_str() + s.size() - 4, L".exe"); }
// The executable behind an AUMID: the AppsFolder entry's shortcut target, or a known-folder relative id such as
// "{1AC14E77-...}\WindowsPowerShell\v1.0\powershell.exe". Empty for packaged apps and system toasts.
std::wstring ExeForAumid(const std::wstring& aumid) {
    if (aumid.empty()) return {};
    ComPtr<IShellItem2> item;
    if (SUCCEEDED(SHCreateItemFromParsingName((L"shell:AppsFolder\\" + aumid).c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        PWSTR target = nullptr;
        if (SUCCEEDED(item->GetString(PKEY_Link_TargetParsingPath, &target)) && target) {
            std::wstring t = target;
            CoTaskMemFree(target);
            if (EndsWithExe(t)) return FullPath(t);
        }
    }
    if (aumid[0] == L'{') {
        const size_t close = aumid.find(L'}');
        GUID id{};
        if (close != std::wstring::npos && SUCCEEDED(CLSIDFromString(aumid.substr(0, close + 1).c_str(), &id))) {
            PWSTR base = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base)) && base) {
                std::wstring t = std::wstring(base) + aumid.substr(close + 1);
                CoTaskMemFree(base);
                if (EndsWithExe(t)) return FullPath(t);
            }
        }
    }
    return {};
}
// The icon an app registered for its toasts (HKCU, then HKCR \AppUserModelId\<AUMID>\IconUri), when it is a file.
std::wstring RegisteredToastIcon(const std::wstring& aumid) {
    for (const auto& [root, base] : {std::pair{HKEY_CURRENT_USER, L"Software\\Classes\\AppUserModelId\\"}, std::pair{HKEY_CLASSES_ROOT, L"AppUserModelId\\"}}) {
        wchar_t value[MAX_PATH * 2]{};
        DWORD bytes = sizeof(value);
        if (RegGetValueW(root, (std::wstring(base) + aumid).c_str(), L"IconUri", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) continue;
        const std::wstring path = Expand(value);
        if (!path.empty() && path.find(L"://") == std::wstring::npos && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}
std::wstring StampKey(const std::wstring& path) {
    const auto st = dock::ReadIconFileStamp(path);
    return std::to_wstring(st.exists) + L":" + std::to_wstring(st.modified) + L":" + std::to_wstring(st.size) + L";";
}
}  // namespace

// Re-reads the Dock's icon sources when pinned.txt, custom-icons.json or the settings changed (then icons are rebuilt).
void Reader::RefreshIconSources() {
    const std::wstring dir = dock::DefaultDataDir(), settingsPath = IconSettingsPath().empty() ? liquid::settings::DefaultPath() : IconSettingsPath();
    const auto settings = liquid::settings::Load(settingsPath).value;
    const bool mask = settings.dock.useMacIconMask, dark = settings.appearance.mode != 1;
    const std::wstring key = StampKey(dir + L"\\pinned.txt") + StampKey(dir + L"\\custom-icons.json") + (mask ? L"m" : L"-") + (dark ? L"d" : L"l");
    const std::string k8 = dock::json::ToUtf8(key);
    if (k8 == sourcesKey_) return;
    sourcesKey_ = k8;
    mask_ = mask; dark_ = dark;
    pinned_.clear();
    if (dock::LoadPinnedConfig(dir + L"\\pinned.txt", pinned_))
        for (auto& p : pinned_) if (!p.isSeparator && !p.isLaunchpad && !p.isDownloads && !p.isRecycleBin) dock::ResolvePinnedItem(p, 16);
    std::wstring error;
    store_.Load(dir + L"\\custom-icons.json", error);
    logos_.clear();
    DLOG(L"notify: icon sources reloaded pinned=%zu mask=%d", pinned_.size(), mask ? 1 : 0);
}

// The app's icon as the Dock shows it: the Dock's custom image and edits when the app is pinned (or has an override
// under its exe / AppsFolder id), else the shell icon, the icon the app registered for toasts, or the WinRT logo; then
// the Dock's macOS mask. Cached per AUMID (failures too) until the sources change.
std::shared_ptr<const IconImage> Reader::Icon(const winrt::Windows::ApplicationModel::AppInfo& info, const std::wstring& aumid) {
    if (auto it = logos_.find(aumid); it != logos_.end()) return it->second;
    const std::wstring exe = ExeForAumid(aumid), lower = [&] { std::wstring l = aumid; for (auto& c : l) c = static_cast<wchar_t>(towlower(c)); return l; }();
    const dock::PinnedItem* pin = nullptr;
    for (const auto& p : pinned_) {
        if ((!p.aumid.empty() && p.aumid == lower) || (!exe.empty() && !p.exePath.empty() && !_wcsicmp(FullPath(p.exePath).c_str(), exe.c_str()))) { pin = &p; break; }
    }
    std::vector<std::string> ids;
    if (pin) ids.push_back(dock::ApplicationItemId(pin->target, pin->args));
    if (!exe.empty()) ids.push_back(dock::ApplicationItemId(exe, L""));
    ids.push_back(dock::ApplicationItemId(L"shell:AppsFolder\\" + aumid, L""));
    std::vector<std::uint32_t> px;
    UINT w = 0, h = 0;
    std::string stable;
    const char* source = "none";
    for (const auto& id : ids) {
        const std::wstring* custom = id.empty() ? nullptr : store_.Find(id);
        std::wstring error;
        if (custom && dock::LoadCustomIconPixels(*custom, px, w, h, error)) { stable = id; source = "custom"; break; }
    }
    if (stable.empty()) for (const auto& id : ids) if (!id.empty() && store_.FindEdit(id)) { stable = id; break; }
    if (px.empty()) {
        std::vector<std::wstring> targets;
        if (pin) targets.push_back(Expand(pin->target));
        if (!exe.empty()) targets.push_back(exe);
        if (aumid.find(L"SystemToast") == std::wstring::npos) targets.push_back(L"shell:AppsFolder\\" + aumid);
        for (const auto& t : targets) if (dock::LoadShellIconPixels(t, 256, px, w, h)) { source = "shell"; break; }
    }
    if (px.empty()) {
        const std::wstring file = RegisteredToastIcon(aumid);
        std::wstring error;
        if (!file.empty() && dock::LoadCustomIconPixels(file, px, w, h, error)) source = "registered";
    }
    if (px.empty()) {
        if (auto logo = Logo(info)) { px = *logo; w = h = kIconPx; source = "winrt"; }
    }
    std::shared_ptr<const IconImage> result;
    if (!px.empty() && w == h && px.size() == static_cast<size_t>(w) * h) {
        const dock::IconEdit* edit = stable.empty() ? nullptr : store_.FindEdit(stable);
        if (edit) px = dock::ApplyIconEdit(px, w, h, *edit);
        const bool mask = edit && edit->mask >= 0 ? edit->mask != 0 : mask_;
        auto img = std::make_shared<IconImage>();
        img->side = w;
        if (mask) {
            unsigned side = 0;
            auto masked = dock::MakeMacStyleIcon(px, w, h, true, &side, dark_);
            if (side) { img->px = std::move(masked); img->side = side; }
            else img->px = std::move(px);
        } else img->px = std::move(px);
        result = img;
    }
    DLOG(L"notify: icon for %ls source=%hs pinned=%d exe=%ls", aumid.c_str(), source, pin ? 1 : 0, exe.c_str());
    logos_[aumid] = result;
    return result;
}

void Reader::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::UI::Notifications;
    using namespace winrt::Windows::UI::Notifications::Management;
    UserNotificationListener listener{nullptr};
    bool requested = false, loggedAccess = false;
    Access lastAccess = Access::Off;
    for (;;) {
        bool enabled, clear;
        std::vector<std::uint32_t> removals;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::milliseconds(enabled_ ? 1500 : 60000),
                         [&] { return stop_ || poke_ || clearAll_ || !removals_.empty() || (enabled_ && lastAccess == Access::Off); });
            if (stop_) break;
            poke_ = false;
            enabled = enabled_;
            clear = clearAll_;
            clearAll_ = false;
            removals.swap(removals_);
        }
        if (!enabled) {
            if (lastAccess != Access::Off) { lastAccess = Access::Off; Publish({}, Access::Off); }
            continue;
        }
        Access access = Access::Unavailable;
        std::vector<Item> items;
        try {
            if (!listener) listener = UserNotificationListener::Current();
            auto status = listener.GetAccessStatus();
            if (status == UserNotificationListenerAccessStatus::Unspecified && !requested) {
                requested = true;
                try { status = listener.RequestAccessAsync().get(); }
                catch (const winrt::hresult_error& e) { DLOG(L"notify: access request failed 0x%08X", static_cast<unsigned>(e.code())); }
            }
            access = status == UserNotificationListenerAccessStatus::Allowed ? Access::Allowed
                   : status == UserNotificationListenerAccessStatus::Denied ? Access::Denied : Access::Pending;
            if (access == Access::Allowed) {
                RefreshIconSources();
                for (auto id : removals) {
                    try { listener.RemoveNotification(id); }
                    catch (const winrt::hresult_error& e) { DLOG(L"notify: remove %u refused 0x%08X; hidden in DockOver", id, static_cast<unsigned>(e.code())); }
                    dismissed_.insert(id);  // gone from DockOver either way (removed, or Windows keeps it in its own list)
                }
                if (clear) {
                    bool cleared = false;
                    try { listener.ClearNotifications(); cleared = true; }
                    catch (const winrt::hresult_error& e) { DLOG(L"notify: ClearNotifications refused 0x%08X; clearing per app", static_cast<unsigned>(e.code())); }
                    if (!cleared) {  // per app, through the toast history (allowed for unpackaged callers)
                        std::set<std::wstring> apps;
                        try { for (const auto& n : listener.GetNotificationsAsync(NotificationKinds::Toast).get()) if (n) try { if (auto info = n.AppInfo()) apps.insert(info.AppUserModelId().c_str()); } catch (...) {} } catch (...) {}
                        int ok = 0;
                        for (const auto& app : apps) { try { ToastNotificationManager::History().Clear(app); ++ok; } catch (...) {} }
                        DLOG(L"notify: cleared %d of %zu apps' notifications", ok, apps.size());
                    }
                    dismissed_.insert(lastIds_.begin(), lastIds_.end());  // whatever Windows still keeps is hidden here
                }
                auto list = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                std::set<std::uint32_t> ids;
                for (const auto& n : list) {
                    if (!n) continue;
                    Item it;
                    it.id = n.Id();
                    ids.insert(it.id);
                    if (dismissed_.count(it.id)) continue;
                    it.created = static_cast<std::int64_t>(winrt::clock::to_file_time(n.CreationTime()).value);
                    try {
                        auto info = n.AppInfo();
                        if (info) {
                            it.aumid = info.AppUserModelId().c_str();
                            if (auto display = info.DisplayInfo()) it.app = display.DisplayName().c_str();
                            it.icon = Icon(info, it.aumid);
                        }
                    } catch (...) {}
                    try {
                        auto toast = n.Notification();
                        auto visual = toast ? toast.Visual() : nullptr;
                        auto binding = visual ? visual.GetBinding(KnownNotificationBindings::ToastGeneric()) : nullptr;
                        if (binding) {
                            auto texts = binding.GetTextElements();
                            for (uint32_t i = 0; i < texts.Size(); ++i) {
                                std::wstring t = texts.GetAt(i).Text().c_str();
                                if (t.empty()) continue;
                                if (it.title.empty()) it.title = t;
                                else { if (!it.body.empty()) it.body += L"\n"; it.body += t; }
                            }
                        }
                    } catch (...) {}
                    if (it.title.empty()) it.title = it.app;
                    items.push_back(std::move(it));
                }
                std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.created > b.created; });
                for (auto d = dismissed_.begin(); d != dismissed_.end();) d = ids.count(*d) ? std::next(d) : dismissed_.erase(d);  // Windows dropped it
                lastIds_ = std::move(ids);
            }
        } catch (const winrt::hresult_error& e) {
            DLOG(L"notify: listener unavailable 0x%08X %ls", static_cast<unsigned>(e.code()), e.message().c_str());
            access = Access::Unavailable;
        } catch (...) {
            access = Access::Unavailable;
        }
        if (!loggedAccess || access != lastAccess) { loggedAccess = true; DLOG(L"notify: access=%d items=%zu", static_cast<int>(access), items.size()); }
        lastAccess = access;
        Publish(std::move(items), access);
    }
    listener = nullptr;
    logos_.clear();
    wic_.Reset();
    winrt::uninit_apartment();
}

// ---------------------------------------------------------------------------------------------------------------
// Shared module resources (UI thread)
struct Shared {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> header, title, body, time, info, button, groupTitle;
    float timeCap = 8.1f, timeX = 5.9f;  // cap and x height of the time text (DIP), from the font's own metrics
    std::wstring settingsPath;
    liquid::design::Palette palette = liquid::design::Dark;
    int ncBlur = 20, ncFade = 100, ncShade = 30;  // Settings > Menu Bar > Notification Center blur (%), fade width (DIP), shade (%)
    bool opaque = false, reducedMotion = false;
};
Shared g;

HRESULT EnsureDevice() {
    if (g.d3d && g.d3d->GetDeviceRemovedReason() != S_OK) { g.d3d.Reset(); g.ctx.Reset(); }
    if (g.d3d) return S_OK;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g.d3d,
                             nullptr, &g.ctx);
}

void EnsureFonts() {
    if (g.dwrite) return;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()))))
        return;
    auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, bool wrap, ComPtr<IDWriteTextFormat>& out) {
        g.dwrite->CreateTextFormat(liquid::design::type::Family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
        if (!out) return;
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(g.dwrite->CreateEllipsisTrimmingSign(out.Get(), &ell))) {
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out->SetTrimming(&t, ell.Get());
        }
    };
    // macOS 26 Notification Center type (reference 2026-11-19): panel title 18 bold, notification title 14 semibold, body
    // 13.5 on a 15.6 pitch, time 11.5. Fixed (uniform) lines so the card rhythm matches the reference exactly.
    fmt(18.0f, DWRITE_FONT_WEIGHT_BOLD, false, g.header);
    fmt(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, g.title);
    fmt(13.5f, DWRITE_FONT_WEIGHT_NORMAL, true, g.body);
    fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, g.time);
    fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, true, g.info);
    fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, g.button);
    fmt(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, g.groupTitle);
    if (g.header) g.header->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 24.0f, 19.0f);
    if (g.title) g.title->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f, 14.0f);
    if (g.time) g.time->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.0f, 14.0f);
    if (g.body) g.body->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 15.6f, 12.5f);
    {   // the time's cap height, so its top gap can equal its right gap exactly
        ComPtr<IDWriteFontCollection> fonts; ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteFont> font;
        UINT32 index = 0; BOOL exists = FALSE;
        if (SUCCEEDED(g.dwrite->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(liquid::design::type::Family, &index, &exists)) && exists &&
            SUCCEEDED(fonts->GetFontFamily(index, &family)) && SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
            DWRITE_FONT_METRICS m{};
            font->GetMetrics(&m);
            if (m.designUnitsPerEm) { g.timeCap = 11.5f * static_cast<float>(m.capHeight) / static_cast<float>(m.designUnitsPerEm);
                                      g.timeX = 11.5f * static_cast<float>(m.xHeight) / static_cast<float>(m.designUnitsPerEm); }
        }
    }
    if (g.time) g.time->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (g.button) { g.button->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g.button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
}

bool RefreshTheme() {
    const auto loaded = liquid::settings::Load(g.settingsPath.empty() ? liquid::settings::DefaultPath() : g.settingsPath).value;
    bool systemDark = true;
    try {
        auto ui = winrt::Windows::UI::ViewManagement::UISettings();
        auto c = ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);
        systemDark = (int(c.R) * 299 + int(c.G) * 587 + int(c.B) * 114) < 128000;
    } catch (...) {}
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    const bool opaque = contrast || !liquid::theme::IsDark(loaded.appearance.mode, systemDark);
    const auto next = liquid::theme::Palette(loaded.appearance.mode, systemDark, contrast);
    const bool changed = opaque != g.opaque || next.primary != g.palette.primary || next.surface != g.palette.surface;
    g.opaque = opaque;
    g.palette = next;
    const bool blurChanged = g.ncBlur != loaded.menuBar.notificationBlur || g.ncShade != loaded.menuBar.notificationShade;  // redraw an open panel
    g.ncBlur = loaded.menuBar.notificationBlur;
    g.ncFade = loaded.menuBar.notificationBlurFade;
    g.ncShade = loaded.menuBar.notificationShade;
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) g.reducedMotion = !animations;
    return changed || blurChanged;
}

D2D1::ColorF Rgb(unsigned c, float a) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
D2D1::ColorF Ink(float a) { return Rgb(g.palette.primary, a); }
D2D1::ColorF Secondary(float a) { return Rgb(g.palette.secondary, a); }

// "now", "13m ago", "4:01 PM", "Yesterday, 10:34 PM", "2 days ago" (macOS Notification Center).
std::wstring TimeLabel(std::int64_t created) {
    if (created <= 0) return L"";
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    const std::int64_t now = (static_cast<std::int64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
    const std::int64_t minutes = (now - created) / 600000000ll;
    if (minutes < 1) return L"now";
    if (minutes < 60) return std::to_wstring(minutes) + L"m ago";
    FILETIME utc{static_cast<DWORD>(created & 0xffffffff), static_cast<DWORD>(created >> 32)}, local{};
    SYSTEMTIME st{}, today{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    GetLocalTime(&today);
    wchar_t clock[64]{};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"h:mm tt", clock, 64);  // "4:20 PM" (macOS), not "04:20 PM"
    auto dayIndex = [](const SYSTEMTIME& s) {
        FILETIME f{};
        SYSTEMTIME d = s;
        d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
        SystemTimeToFileTime(&d, &f);
        return ((static_cast<std::int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 864000000000ll;
    };
    const std::int64_t days = dayIndex(today) - dayIndex(st);
    if (days <= 0) return clock;
    if (days == 1) return std::wstring(L"Yesterday, ") + clock;
    return std::to_wstring(days) + L" days ago";
}

// ---------------------------------------------------------------------------------------------------------------
// GlassView: a top-most composition window drawn with the shared Liquid Glass material over a live backdrop.
class GlassView {
public:
    virtual ~GlassView() { Destroy(); }
    HWND hwnd = nullptr;
    float scale = 1;
    UINT pxW = 0, pxH = 0;
    RECT rect{};
    bool needsFrame = false;

    bool Create(const wchar_t* cls, const wchar_t* title, DWORD exStyle) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | exStyle, cls, title, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                        GetModuleHandleW(nullptr), this);
        return hwnd != nullptr;
    }
    // Moves/resizes the window to `screen` (physical px) and (re)creates the graphics on first use.
    bool Place(const RECT& screen) {
        if (!hwnd) return false;
        scale = GetDpiForWindow(hwnd) / 96.0f;
        const UINT w = static_cast<UINT>((std::max)(1L, screen.right - screen.left)), h = static_cast<UINT>((std::max)(1L, screen.bottom - screen.top));
        SetWindowPos(hwnd, HWND_TOPMOST, screen.left, screen.top, static_cast<int>(w), static_cast<int>(h), SWP_NOACTIVATE);
        const float dpiNow = GetDpiForWindow(hwnd) / 96.0f;
        if (dpiNow != scale) scale = dpiNow;
        GetWindowRect(hwnd, &rect);
        const bool resized = w != pxW || h != pxH;
        pxW = w; pxH = h;
        if (!swap_) { const HRESULT hr = InitGraphics(); if (FAILED(hr)) DLOG(L"notify: graphics init failed 0x%08X", static_cast<unsigned>(hr)); return SUCCEEDED(hr); }
        if (resized) return SUCCEEDED(ResizeTarget());
        return true;
    }
    void StartCapture() {
        if (!glassOk_ || g.opaque) return;
        backdrop_.Request(true, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        captureAt_ = Now();
        // Development screenshots only: let GDI/WGC screen grabs see the window (it then also sees itself).
        if (GetEnvironmentVariableW(L"LIQUID_NOTIFY_CAPTURABLE", nullptr, 0)) SetWindowDisplayAffinity(hwnd, WDA_NONE);
    }
    void StopCapture() { backdrop_.Request(false, nullptr); }
    HANDLE CompletionEvent() const { return backdrop_.CompletionEvent(); }
    void OnCompletion() { if (backdrop_.CompleteCompare(rect)) needsFrame = true; }
    bool BackdropReady() { dock::BackdropFrame f; return !g.opaque && glassOk_ && backdrop_.Latest(rect, f); }
    bool WaitingForGlass() { return !g.opaque && glassOk_ && !BackdropReady() && Now() - captureAt_ < 0.25; }
    void Destroy() {
        backdrop_.Request(false, nullptr);
        backdrop_.SetDevice(nullptr);
        if (glass_) glass_->ReleaseGpuResources();
        glass_.reset();
        if (dc_) dc_->SetTarget(nullptr);
        target_.Reset(); rtv_.Reset(); visual_.Reset(); dcompTarget_.Reset(); dcomp_.Reset(); dc_.Reset(); swap_.Reset();
        if (waitable_) { CloseHandle(waitable_); waitable_ = nullptr; }
        if (hwnd) { HWND h = hwnd; hwnd = nullptr; SetWindowLongPtrW(h, GWLP_USERDATA, 0); DestroyWindow(h); }
        icons_.clear();
        slotSource_.clear();
    }
    virtual LRESULT Handle(UINT m, WPARAM w, LPARAM l) = 0;
    virtual void Frame() = 0;
    virtual bool Animating() const = 0;

protected:
    // Glass pass: clears the target and draws plates; then Begin()/End() for the D2D content.
    void Clear() { const float c[4] = {0, 0, 0, 0}; g.ctx->ClearRenderTargetView(rtv_.Get(), c); lastGlass_ = true; }
    // Returns false when the material or a backdrop frame is missing (the caller draws a fallback body in D2D).
    bool Plate(D2D1_RECT_F r, float radius, float opacity, float edge = 1.0f, dock::GlassRole role = dock::GlassRole::Notification, float feather = 0.0f,
               float spread = -1.0f, float brightness = 1.0f) {
        if (g.opaque || !glassOk_) { lastGlass_ = false; return false; }
        dock::BackdropFrame frame;
        if (!backdrop_.Latest(rect, frame)) { lastGlass_ = false; return false; }
        dock::GlassParameters gp = dock::RoleGlass(role);
        if (role == dock::GlassRole::BackdropBlur) {
            if (gp.reducedTransparency) return true;  // no blur layer when transparency is reduced
            gp.blurSpread = (std::max)(0.0f, spread);
            gp.brightness *= brightness;  // the background shade
            if (gp.blurSpread <= 0.001f && brightness >= 0.999f) return true;
        } else if (spread > 0.0f) {
            // A card over the blurred column: never less blurred than what surrounds it, or it reads as a window
            // through the blur to the sharp desktop.
            gp.blurSpread = (std::max)(gp.blurSpread, (std::min)(1.0f, spread * 1.15f));
            // Frost comes from Glass Studio > Notifications (default 1: cards show only the blurred sample).
        }
        gp.edgeSoftness = feather;
        gp.radius = radius;
        gp.opacity = std::clamp(opacity, 0.0f, 1.0f);
        edge = std::clamp(edge, 0.0f, 1.0f);
        gp.refraction *= edge; gp.dispersion *= edge; gp.glare *= edge; gp.edgeAmount *= edge;
        gp.strokeOpacity *= edge; gp.fresnelAmount *= edge; gp.specularAmount *= edge;
        dock::GpuPaint p;
        p.context = g.ctx.Get();
        p.target = rtv_.Get();
        p.targetWidth = pxW;
        p.targetHeight = pxH;
        p.windowScreenRect = rect;
        p.dpiScale = scale;
        p.plateDip = r;
        p.backdrop = &frame;
        p.params = &gp;
        p.pointerX = mx_ * scale;
        p.pointerY = my_ * scale;
        const bool ok = glass_->RenderGpu(p);
        if (!ok) lastGlass_ = false;
        return ok;
    }
    void Begin() { dc_->BeginDraw(); dc_->SetTransform(D2D1::Matrix3x2F::Identity()); }
    void End() {
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        dc_->EndDraw();
        if (!snapshotPath_.empty()) { SaveSnapshot(snapshotPath_); snapshotPath_.clear(); }
        swap_->Present(1, 0);
    }
    // Development (LIQUID_NOTIFY_SNAPSHOT): the next frame's own pixels (premultiplied BGRA) as a PNG, so the panel can
    // be checked over a normal screenshot without letting the window capture itself.
    std::wstring snapshotPath_;
    void SaveSnapshot(const std::wstring& path) {
        ComPtr<ID3D11Texture2D> back, staging;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(g.d3d->CreateTexture2D(&d, nullptr, &staging))) return;
        g.ctx->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICStream> stream;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                  SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                  SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                  SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(d.Width, d.Height));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;  // stored as is: the pixels are premultiplied
        ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(frame->WritePixels(d.Height, m.RowPitch, m.RowPitch * d.Height, static_cast<BYTE*>(m.pData))) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        g.ctx->Unmap(staging.Get(), 0);
        DLOG(L"notify: snapshot %ls ok=%d rect=%ld,%ld,%ld,%ld", path.c_str(), ok ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom);
    }
    // Fallback / opaque card body (light appearance, high contrast, or before the first captured frame).
    void Body(D2D1_RECT_F r, float radius, float a, bool glassDrawn) {
        if (glassDrawn) return;
        brush_->SetColor(g.opaque ? Rgb(g.palette.surface, 0.97f * a) : D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.86f * a));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
        brush_->SetColor(g.opaque ? Rgb(g.palette.divider, 0.6f * a) : D2D1::ColorF(1, 1, 1, 0.14f * a));
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), 1.0f / scale);
    }
    void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1::ColorF c) {
        if (!f || s.empty()) return;
        brush_->SetColor(c);
        dc_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    // Wrapped text clipped to `maxLines`; returns the height it takes.
    float Measure(const std::wstring& s, IDWriteTextFormat* f, float width, int maxLines, float lineHeight) {
        if (!f || s.empty()) return 0;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(g.dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, width, 1000.0f, &layout))) return lineHeight;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const UINT lines = (std::max)(1u, (std::min)(m.lineCount, static_cast<UINT>(maxLines)));
        return lines * lineHeight;
    }
    void Wrapped(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1::ColorF c) {
        if (!f || s.empty()) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(g.dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, r.right - r.left, r.bottom - r.top + 1.0f, &layout))) return;
        brush_->SetColor(c);
        dc_->PushAxisAlignedClip(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2), D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        dc_->PopAxisAlignedClip();
    }
    void AppIcon(const Item& it, D2D1_RECT_F r, float a) {
        ID2D1Bitmap1* bmp = nullptr;
        if (it.icon && !it.aumid.empty()) {
            auto& slot = icons_[it.aumid];
            if (!slot || slotSource_[it.aumid] != it.icon.get()) {  // new image for this app (sources changed)
                slot.Reset();
                const UINT side = it.icon->side;
                if (side && it.icon->px.size() == static_cast<size_t>(side) * side) {
                    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    dc_->CreateBitmap(D2D1::SizeU(side, side), it.icon->px.data(), side * 4, &props, &slot);
                }
                slotSource_[it.aumid] = it.icon.get();
            }
            bmp = slot.Get();
        }
        if (bmp) { dc_->DrawBitmap(bmp, r, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); return; }
        const float rad = (r.right - r.left) * 0.225f;
        brush_->SetColor(D2D1::ColorF(0.35f, 0.38f, 0.45f, a));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
        const std::wstring letter = it.app.empty() ? L"?" : it.app.substr(0, 1);
        ComPtr<IDWriteTextFormat> f;
        g.dwrite->CreateTextFormat(liquid::design::type::Family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   (r.bottom - r.top) * 0.5f, L"", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); Text(letter, f.Get(), r, D2D1::ColorF(1, 1, 1, a)); }
    }
    void PaintCard(const Item& it, D2D1_RECT_F r, float a, int lines, bool hover, bool pressed, bool closeHot, const std::wstring& when, bool glass);
    // A small round close button (card corners, panel header).
    void CloseButton(D2D1_POINT_2F c, float radius, float a, bool hot) {
        brush_->SetColor(g.opaque ? Rgb(g.palette.surface, a) : D2D1::ColorF(0.22f, 0.22f, 0.25f, (hot ? 0.98f : 0.9f) * a));
        dc_->FillEllipse(D2D1::Ellipse(c, radius, radius), brush_.Get());
        brush_->SetColor(g.opaque ? Rgb(g.palette.divider, a) : D2D1::ColorF(1, 1, 1, 0.18f * a));
        dc_->DrawEllipse(D2D1::Ellipse(c, radius, radius), brush_.Get(), 1.0f / scale);
        const float k = radius * 0.36f;
        brush_->SetColor(Ink(0.85f * a));
        dc_->DrawLine(D2D1::Point2F(c.x - k, c.y - k), D2D1::Point2F(c.x + k, c.y + k), brush_.Get(), 1.5f);
        dc_->DrawLine(D2D1::Point2F(c.x + k, c.y - k), D2D1::Point2F(c.x - k, c.y + k), brush_.Get(), 1.5f);
    }

    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    float mx_ = -1, my_ = -1;
    bool lastGlass_ = false;
    dock::WgcBackdropSource backdrop_;

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto* v = static_cast<GlassView*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
            v->hwnd = h;
        }
        auto* v = reinterpret_cast<GlassView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (v && m == dock::kMsgCaptureAvailable) { if (v->backdrop_.ProcessCapture(v->rect)) v->needsFrame = true; return 0; }
        if (v && m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); v->needsFrame = true; return 0; }
        if (m == WM_ERASEBKGND) return 1;
        return v ? v->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
    }
    HRESULT InitGraphics() {
        HRESULT hr = EnsureDevice();
        if (FAILED(hr)) return hr;
        ComPtr<IDXGIDevice> dxgi;
        g.d3d.As(&dxgi);
        ComPtr<IDXGIFactory2> f;
        if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return hr;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = pxW; sd.Height = pxH;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(hr = f->CreateSwapChainForComposition(dxgi.Get(), &sd, nullptr, &swap_))) return hr;
        ComPtr<ID2D1Factory1> factory;
        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(factory.GetAddressOf())))) return hr;
        ComPtr<ID2D1Device> d2d;
        if (FAILED(hr = factory->CreateDevice(dxgi.Get(), &d2d))) return hr;
        if (FAILED(hr = d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return hr;
        if (FAILED(hr = ResizeTarget())) return hr;
        if (FAILED(hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return hr;
        if (FAILED(hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) return hr;
        dcomp_->CreateVisual(&visual_);
        visual_->SetContent(swap_.Get());
        dcompTarget_->SetRoot(visual_.Get());
        dcomp_->Commit();
        dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush_);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        glass_ = dock::CreateLiquidGlassMaterial();
        glassOk_ = glass_ && SUCCEEDED(glass_->CreateGpuResources(g.d3d.Get()));
        backdrop_.SetDevice(g.d3d.Get());
        backdrop_.SetExcludedWindow(hwnd);
        return S_OK;
    }
    HRESULT ResizeTarget() {
        dc_->SetTarget(nullptr);
        target_.Reset();
        rtv_.Reset();
        HRESULT hr = S_OK;
        DXGI_SWAP_CHAIN_DESC1 d{};
        swap_->GetDesc1(&d);
        if (d.Width != pxW || d.Height != pxH) {
            g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
            g.ctx->Flush();
            if (FAILED(hr = swap_->ResizeBuffers(2, pxW, pxH, DXGI_FORMAT_UNKNOWN, 0))) return hr;
        }
        ComPtr<IDXGISurface> s;
        if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&s)))) return hr;
        const float dpi = 96.0f * scale;
        const auto p = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(s.Get(), &p, &target_))) return hr;
        dc_->SetTarget(target_.Get());
        dc_->SetDpi(dpi, dpi);
        ComPtr<ID3D11Texture2D> t;
        swap_->GetBuffer(0, IID_PPV_ARGS(&t));
        return g.d3d->CreateRenderTargetView(t.Get(), nullptr, &rtv_);
    }

    ComPtr<IDXGISwapChain1> swap_;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    std::unique_ptr<dock::IPlateMaterial> glass_;
    bool glassOk_ = false;
    double captureAt_ = 0;
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> icons_;
    std::map<std::wstring, const IconImage*> slotSource_;  // which IconImage each cached bitmap was made from
};

// Card geometry (DIP), measured on the macOS 26 Notification Center screenshot (2026-11-19, 1.41 px per pt): 343 wide,
// corner 17; a 34 icon 13 from the left, centred vertically; text from x 57.5 to 13 from the right; the title (14
// semibold, 18 line, baseline 14) starts 10 below the card top, the body (13.5, 15.6 pitch) 28.85 below it, and 12.35
// stay below the last line (title + one line = 56.8, + two = 72.4). Cards are 8 apart, plus 8 per stacked plate; the
// plates are inset 16 and 33 on both sides.
constexpr float kCardW = docky::notification_card::Width, kCardRadius = docky::notification_card::Radius,
    kIcon = docky::notification_card::Icon, kIconX = docky::notification_card::IconInset,
    kTextX = docky::notification_card::TextInset, kTextR = docky::notification_card::IconInset;
constexpr float kTitleTop = 10.0f, kTitleLine = 18.0f, kBodyTop = 28.85f, kLineH = 15.6f, kBottomPad = 12.35f;
constexpr float kCardGap = 8.0f, kPlateStep = 8.0f, kPlateInset1 = 16.0f, kPlateInset2 = 33.0f;
constexpr int kMaxLines = 4, kBannerLines = 3;
constexpr float kTitlePitch = 17.0f;  // a wrapped title's second line (reference: 16.5-17)
constexpr int kTitleMaxLines = 2;
float CardHeight(int lines, int titleLines = 1) {
    const float extra = static_cast<float>((std::max)(1, titleLines) - 1) * kTitlePitch;
    return lines > 0 ? kBodyTop + extra + static_cast<float>(lines) * kLineH + kBottomPad : kTitleTop + kTitleLine + extra + 12.0f;
}
float WhenWidth(const std::wstring& when) {
    if (when.empty() || !g.dwrite || !g.time) return 0.0f;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(g.dwrite->CreateTextLayout(when.c_str(), static_cast<UINT32>(when.size()), g.time.Get(), 200, kTitleLine, &l))) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace + 8.0f;
}
int TitleLines(const std::wstring& title, const std::wstring& when) {
    if (title.empty() || !g.dwrite || !g.title) return 1;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g.dwrite->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), g.title.Get(), kCardW - kTextX - kTextR - WhenWidth(when), 4000.0f, &layout))) return 1;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return std::clamp(static_cast<int>(m.lineCount), 1, kTitleMaxLines);
}
int BodyLines(const std::wstring& body, int maxLines) {
    if (body.empty() || !g.dwrite || !g.body) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g.dwrite->CreateTextLayout(body.c_str(), static_cast<UINT32>(body.size()), g.body.Get(), kCardW - kTextX - kTextR, 4000.0f, &layout))) return 1;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return std::clamp(static_cast<int>(m.lineCount), 1, maxLines);
}

// One row of the list: a notification card (a collapsed app group shows its newest notification with up to two plates
// under it), or the header of an expanded group (app name and "Show Less").
struct Row {
    enum class Kind { Card, Header } kind = Kind::Card;
    std::wstring key, group;
    Item item;
    int count = 1, plates = 0, lines = 0, titleLines = 1;
    float h = 0;
    docky::motion::Spring y, appear;
    bool placed = false;
    // Folding a stack: the cards below its top card stay as ghosts that slide up under it and are then removed.
    bool ghost = false;
    std::wstring follow;  // ghost: the key of the card it slides under
};

class Module;
Module* g_module = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// Notification Center panel: a column at the right edge, from the menu bar to the bottom of the monitor.
class Center : public GlassView {
public:
    explicit Center(Module* owner) : owner_(owner) {}
    void Open(const RECT& anchor, HWND notify, UINT msg);
    void Close(const wchar_t* why);
    bool IsOpen() const { return hwnd != nullptr && !closing_; }
    bool Exists() const { return hwnd != nullptr; }
    void SetItems(const std::vector<Item>& items, Access access);
    bool Animating() const override { return animating_; }
    void Frame() override;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l) override;

private:
    enum class Hit { None, Clear, Card, CardClose, ShowLess, Info, EditWidgets, BottomClose, Empty };
    Hit HitTest(float x, float y, int* index);
    void Rebuild();
    void Layout(bool snap);
    // First card 54 below the window top (the menu bar's bottom): 84 from the top of the screen, as on the reference.
    float ListTop() const { return topInset_ + 54.0f; }
    D2D1_RECT_F RowRect(size_t i) const;
    float Left() const { return padL_; }
    float Right() const { return padL_ + kCardW; }
    float Slide() const { return kCardW + 26.0f + kMarginR; }  // travel of the open/close slide
    // Bottom controls (reference): an "Edit Widgets" pill 85 x 24 and a 23 close circle 9 apart, centred under the
    // column, 115 above the bottom of the screen; the list fades out as it reaches them.
    float PillTop() const { return bottomY_ - 12.0f; }
    D2D1_RECT_F EditPill() const { const float cx = (Left() + Right()) * 0.5f - 16.0f; return D2D1::RectF(cx - 42.5f, bottomY_ - 12.0f, cx + 42.5f, bottomY_ + 12.0f); }
    D2D1_POINT_2F BottomClose() const { const auto p = EditPill(); return D2D1::Point2F(p.right + 9.0f + 11.5f, bottomY_); }
    D2D1_POINT_2F HeaderClose() const { return D2D1::Point2F(Right() - 10.0f, ListTop() - 18.5f); }
    float RowFade(float top) const {  // 1 above the controls, 0.3 under them (reference)
        const float t = std::clamp((top - (PillTop() - 40.0f)) / 40.0f, 0.0f, 1.0f);
        return 1.0f - 0.7f * t;
    }

    // The window reaches the blur's fade width (Settings, 40..400 DIP) further left than the cards, so the background
    // blur can fade out toward the left inside it.
    static constexpr float kMarginR = 17.0f;  // reference: 17 from the screen edge
    float padL_ = 214.0f, bottomY_ = 1000.0f;
    float BlurSpread() const { return 0.8f * static_cast<float>(std::clamp(g.ncBlur, 0, 100)) / 100.0f; }  // 100 % = 0.8
    Module* owner_;
    RECT anchor_{};
    HWND notify_ = nullptr;
    UINT notifyMsg_ = 0;
    HWND previous_ = nullptr;
    std::vector<Item> items_;
    std::vector<Row> rows_;
    std::wstring expanded_;  // the app group shown expanded ("" = all collapsed)
    std::wstring builtExpanded_;  // expanded_ at the last Rebuild (to tell an unfold or a fold from other changes)
    float topInset_ = 30.0f;  // the menu bar: the window starts at the top of the screen so the blur runs under it
    Access access_ = Access::Off;
    docky::motion::Spring open_, scroll_;
    bool closing_ = false, animating_ = false, snapshotTaken_ = false;
    double lastT_ = 0, openedAt_ = 0;
    float contentH_ = 0, viewH_ = 0;
    Hit hover_ = Hit::None;
    int hoverIndex_ = -1;
    Hit pressed_ = Hit::None;
    int pressedIndex_ = -1;
    liquid::popup::DismissWatch dismissal_;
};

// ---------------------------------------------------------------------------------------------------------------
// Banner: one card at the top right that slides in from the right edge and leaves the same way.
class Banner : public GlassView {
public:
    explicit Banner(Module* owner) : owner_(owner) {}
    void Show(const Item& item);
    void Dismiss(bool toRight = true);
    bool Active() const { return hwnd != nullptr; }
    std::uint32_t Id() const { return item_.id; }
    bool Animating() const override { return animating_; }
    void Frame() override;
    LRESULT Handle(UINT m, WPARAM w, LPARAM l) override;

private:
    static constexpr float kPad = 14.0f;  // window margin around the card (close button, shadow)
    Module* owner_;
    Item item_;
    int lines_ = 0;
    float cardH_ = 0;
    docky::motion::Spring slide_;
    bool leaving_ = false, animating_ = false, hover_ = false, closeHot_ = false, pressed_ = false;
    double lastT_ = 0, shownAt_ = 0, hideAt_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------
// SystemBannerHider: keeps Windows' own notification popup off screen while DockOver shows its banners.
// Windows 11 draws the popup in one persistent CoreWindow of ShellExperienceHost ("New notification"); it is zero high
// while idle and grows when a toast arrives. Each time Windows moves or sizes it to show a toast, it is moved to
// -32000,-32000 at once (the WinEvent arrives before the next composition frame). Windows puts it back by itself for
// the next toast, so nothing has to be restored, and the notification still goes to Windows' notification list.
// Only ShellExperienceHost's events are hooked (location changes are very frequent system-wide).
class SystemBannerHider {
public:
    ~SystemBannerHider() { Stop(); }
    // Called when Windows starts showing a toast: the reader polls at once, so DockOver's banner is not up to a poll late.
    std::function<void()> onToast;
    void SetActive(bool on) {
        if (on == active_) return;
        active_ = on;
        if (on) Hook(); else Stop();
        DLOG(L"notify: hide Windows banners %ls", on ? L"on" : L"off");
    }
    // Every few seconds: ShellExperienceHost may have restarted (new process id).
    void Maintain() {
        if (!active_) return;
        DWORD pid = 0;
        if (HWND h = FindToast(&pid); h && pid != pid_) { Stop(); active_ = true; Hook(); }
        else if (!hook_) Hook();
    }

private:
    static bool IsToastTitle(const wchar_t* t) {
        static const wchar_t* titles[] = {L"New notification", L"新通知", L"新しい通知", L"새 알림",
                                          L"Neue Benachrichtigung", L"Nouvelle notification", L"Nueva notificación", L"Nuova notifica",
                                          L"Nova notificação", L"Новое уведомление"};
        for (const wchar_t* k : titles) if (!wcscmp(t, k)) return true;
        return false;
    }
    static bool IsShellHost(DWORD pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return false;
        wchar_t path[MAX_PATH]{};
        DWORD n = MAX_PATH;
        const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != FALSE;
        CloseHandle(p);
        const wchar_t* name = wcsrchr(path, L'\\');
        return ok && name && !_wcsicmp(name + 1, L"ShellExperienceHost.exe");
    }
    // The popup window: a ShellExperienceHost CoreWindow with the (localized) "New notification" title.
    static HWND FindToast(DWORD* pidOut) {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, L"Windows.UI.Core.CoreWindow", nullptr)) != nullptr) {
            wchar_t title[128]{};
            GetWindowTextW(h, title, 128);
            if (!IsToastTitle(title)) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (!IsShellHost(pid)) continue;
            if (pidOut) *pidOut = pid;
            return h;
        }
        return nullptr;
    }
    void Hook() {
        DWORD pid = 0;
        toast_ = FindToast(&pid);
        if (!toast_) { DLOG(L"notify: Windows notification popup window not found (yet)"); return; }
        pid_ = pid;
        self_ = this;
        hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEvent, pid_, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        Hide(toast_);  // a popup already on screen
    }
    void Stop() {
        if (hook_) UnhookWinEvent(hook_);
        hook_ = nullptr;
        toast_ = nullptr;
        pid_ = 0;
        if (self_ == this) self_ = nullptr;
        active_ = false;
    }
    static void Hide(HWND h) {
        RECT r{};
        if (!GetWindowRect(h, &r) || r.bottom - r.top <= 0 || r.left <= -30000) return;  // idle (zero high) or already away
        SetWindowPos(h, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        ++self_->hidden_;
        if (self_->onToast) self_->onToast();
        if (self_->hidden_ <= 3 || self_->hidden_ % 50 == 0) DLOG(L"notify: Windows popup moved off screen (%ld,%ld %ldx%ld) count=%u", r.left, r.top, r.right - r.left, r.bottom - r.top, self_->hidden_);
    }
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND h, LONG object, LONG child, DWORD, DWORD) {
        if (!self_ || object != OBJID_WINDOW || child != CHILDID_SELF || h != self_->toast_) return;
        Hide(h);
    }

    inline static SystemBannerHider* self_ = nullptr;
    bool active_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    HWND toast_ = nullptr;
    DWORD pid_ = 0;
    unsigned hidden_ = 0;
};

class Module {
public:
    int Run();
    SystemBannerHider hider;
    bool hideSystem = true;
    // Windows' popup is hidden only while DockOver really receives notifications (capture on, access granted).
    void SyncHider() { hider.SetActive(capture && hideSystem && access == Access::Allowed); }
    HWND host = nullptr;
    std::unique_ptr<Reader> reader;
    std::vector<Item> items;
    Access access = Access::Off;
    std::set<std::uint32_t> seen;
    bool primed = false, capture = false;
    std::vector<Item> synthetic;
    Center center{this};
    Banner banner{this};
    std::vector<Item> queue;  // banners waiting for the current one

    void Merge();
    void Launch(const Item& it) {
        if (!it.aumid.empty()) {
            const std::wstring target = L"shell:AppsFolder\\" + it.aumid;
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    void Remove(std::uint32_t id) {
        synthetic.erase(std::remove_if(synthetic.begin(), synthetic.end(), [&](const Item& i) { return i.id == id; }), synthetic.end());
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.id == id; }), items.end());
        if (reader && !(id & 0x80000000u)) reader->Remove(id);
        center.SetItems(items, access);
    }
    void ClearAll() {
        DLOG(L"notify: clear all (%zu items)", items.size());
        synthetic.clear();
        items.clear();
        if (reader) reader->ClearAll();
        center.SetItems(items, access);
    }
    void NextBanner() {
        if (banner.Active() || queue.empty() || center.IsOpen()) return;
        Item next = queue.front();
        queue.erase(queue.begin());
        banner.Show(next);
    }

private:
    static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l);
};

std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false}, g_centerOpen{false};
std::atomic<HWND> g_host{nullptr};
std::wstring g_settingsPath;
bool g_capture = false, g_hideSystem = true;  // guarded by g_mutex
struct ToggleRequest { RECT anchor{}; HWND notify = nullptr; UINT msg = 0; };
ToggleRequest g_toggle;  // guarded by g_mutex
struct DebugRequest { std::wstring app, title, body; };
std::vector<DebugRequest> g_debug;  // guarded by g_mutex

// The card shared by the panel and the banner (macOS 26 layout, see kCardW).
void GlassView::PaintCard(const Item& it, D2D1_RECT_F r, float a, int lines, bool hover, bool pressed, bool closeHot, const std::wstring& when, bool glass) {
    Body(r, kCardRadius, a, glass);
    if (glass && !g.opaque) {  // the reference cards read a little lighter than what is around them
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.035f * a));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, kCardRadius, kCardRadius), brush_.Get());
    }
    if (hover) {
        brush_->SetColor(g.opaque ? Rgb(g.palette.hover, 0.5f * a) : D2D1::ColorF(1, 1, 1, (pressed ? 0.10f : 0.045f) * a));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, kCardRadius, kCardRadius), brush_.Get());
    }
    const float iy = r.top + (r.bottom - r.top - kIcon) * 0.5f;
    AppIcon(it, D2D1::RectF(r.left + kIconX, iy, r.left + kIconX + kIcon, iy + kIcon), a);
    const float tx = r.left + kTextX, right = r.right - kTextR, ty = r.top + kTitleTop;
    const float whenW = WhenWidth(when);
    if (!when.empty())
    {   // "1m ago" sits as far from the card's top as from its right edge (its cap top 13 below the top; baseline 14 in its line)
        // the label's own top: digits and capitals reach the cap height, "now" only the x-height
        const bool tall = std::any_of(when.begin(), when.end(), [](wchar_t c) { return iswdigit(c) || iswupper(c); });
        const float timeTop = r.top + kTextR - (14.0f - (tall ? g.timeCap : g.timeX));
        Text(when, g.time.Get(), D2D1::RectF(right - 200.0f, timeTop, right, timeTop + kTitleLine), g.opaque ? Secondary(0.9f * a) : D2D1::ColorF(1, 1, 1, 0.5f * a));
    }
    const int titleLines = TitleLines(it.title, when);
    Wrapped(it.title, g.title.Get(), D2D1::RectF(tx, ty, right - whenW, ty + static_cast<float>(titleLines) * kTitlePitch + 1.0f),
            g.opaque ? Ink(a) : D2D1::ColorF(1, 1, 1, 0.96f * a));
    const float bodyTop = r.top + kBodyTop + static_cast<float>(titleLines - 1) * kTitlePitch;
    if (lines > 0)
        Wrapped(it.body, g.body.Get(), D2D1::RectF(tx, bodyTop, right, bodyTop + static_cast<float>(lines) * kLineH),
                g.opaque ? Ink(0.92f * a) : D2D1::ColorF(1, 1, 1, 0.88f * a));
    if (hover) CloseButton(D2D1::Point2F(r.left + 4.0f, r.top + 4.0f), 10.0f, a, closeHot);
}

// ---------------------------------------------------------------------------------------------------------------
// Center
void Center::Open(const RECT& anchor, HWND notify, UINT msg) {
    anchor_ = anchor; notify_ = notify; notifyMsg_ = msg;
    RefreshTheme();
    EnsureFonts();
    if (!hwnd && !Create(L"DockOverNotificationCenter", L"DockOver Notification Center", 0)) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const float s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    padL_ = static_cast<float>(std::clamp(g.ncFade, 40, 400)) + 14.0f;
    const LONG width = static_cast<LONG>(std::lround((padL_ + kCardW + kMarginR) * s));
    // From the very top of the monitor (the blur and shade run under the menu bar), kept just below the bar in z-order.
    RECT r{mi.rcMonitor.right - width, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
    topInset_ = static_cast<float>((std::max)(anchor.bottom, mi.rcMonitor.top) - mi.rcMonitor.top) / s;
    if (!Place(r)) { DLOG(L"notify: center graphics init failed"); Destroy(); return; }
    viewH_ = (r.bottom - r.top) / scale;
    bottomY_ = viewH_ - 115.0f;
    previous_ = GetForegroundWindow();
    closing_ = false;
    snapshotTaken_ = false;
    expanded_.clear();
    builtExpanded_.clear();
    open_.SetSpec(docky::motion::tokens::PanelOpen);
    open_.SnapTo(0.0);
    open_.SetTarget(1.0);
    scroll_.SetSpec(docky::motion::tokens::Scroll);
    scroll_.SnapTo(0.0);
    hover_ = Hit::None; hoverIndex_ = -1; pressed_ = Hit::None;
    Rebuild();
    Layout(true);
    StartCapture();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);  // Esc and the scroll wheel; outside clicks close (DismissWatch)
    PostMessageW(hwnd, kMsgBelowBar, 0, 0);  // under the menu bar
    dismissal_.Start(hwnd);
    lastT_ = openedAt_ = Now();
    animating_ = needsFrame = true;
    g_centerOpen.store(true);
    if (notify_) PostMessageW(notify_, notifyMsg_, 1, 0);
    DLOG(L"notify: center open %ldx%ld rows=%zu items=%zu access=%d", r.right - r.left, r.bottom - r.top, rows_.size(), items_.size(), static_cast<int>(access_));
}

void Center::Close(const wchar_t* why) {
    if (!hwnd || closing_) return;
    closing_ = true;
    open_.SetSpec(docky::motion::tokens::PanelClose);
    open_.SetTarget(0.0);
    animating_ = needsFrame = true;
    dismissal_.Stop();
    g_centerOpen.store(false);
    if (notify_) PostMessageW(notify_, notifyMsg_, 0, 0);
    DLOG(L"notify: center closing (%ls)", why);
}

void Center::SetItems(const std::vector<Item>& items, Access access) {
    items_ = items;
    access_ = access;
    if (hwnd) { Rebuild(); Layout(false); animating_ = needsFrame = true; }
}

// Groups the notifications by app (newest first, as Windows lists them): a collapsed group is one card with up to two
// plates under it; the expanded group is a header row and all of its cards. Rows keep their springs by key.
void Center::Rebuild() {
    std::vector<std::pair<std::wstring, std::vector<const Item*>>> groups;
    for (const auto& it : items_) {
        const std::wstring key = !it.aumid.empty() ? it.aumid : L"app:" + it.app;
        auto g2 = std::find_if(groups.begin(), groups.end(), [&](const auto& p) { return p.first == key; });
        if (g2 == groups.end()) groups.push_back({key, {&it}});
        else g2->second.push_back(&it);
    }
    if (!expanded_.empty() && std::none_of(groups.begin(), groups.end(), [&](const auto& p) { return p.first == expanded_ && p.second.size() > 1; })) expanded_.clear();
    std::vector<Row> next;
    auto oldRow = [&](const std::wstring& k) -> const Row* {
        for (const auto& r : rows_) if (r.key == k && !r.ghost) return &r;
        return nullptr;
    };
    auto take = [&](Row row) {
        if (const Row* old = oldRow(row.key)) { row.y = old->y; row.appear = old->appear; row.placed = old->placed; }
        else { row.appear.SetSpec(docky::motion::tokens::Appear); row.appear.SnapTo(0.0); row.appear.SetTarget(1.0); }
        next.push_back(std::move(row));
    };
    for (const auto& [key, list] : groups) {
        const std::wstring topKey = L"i:" + std::to_wstring(list.front()->id);
        if (key == expanded_) {
            Row h; h.kind = Row::Kind::Header; h.key = L"h:" + key; h.group = key; h.item = *list.front(); h.count = static_cast<int>(list.size());
            take(std::move(h));
            // Unfolding (macOS): the stack's other cards start under its top card, fully drawn, and slide down to their
            // places (Layout's springs); the earlier card is drawn over the later ones (Frame draws back to front).
            const Row* top = builtExpanded_ != key ? oldRow(topKey) : nullptr;
            for (const Item* it : list) {
                Row c; c.key = L"i:" + std::to_wstring(it->id); c.group = key; c.item = *it;
                if (top && c.key != topKey && !oldRow(c.key)) { c.y.SnapTo(top->y.Value()); c.placed = true; c.appear.SnapTo(1.0); }
                take(std::move(c));
            }
        } else {
            Row c; c.key = topKey; c.group = key; c.item = *list.front();
            c.count = static_cast<int>(list.size()); c.plates = (std::min)(c.count - 1, 2);
            take(std::move(c));
            if (builtExpanded_ == key) {  // folding: the other cards slide back up under the top card, then go
                for (const auto& r : rows_)
                    if (r.kind == Row::Kind::Card && !r.ghost && r.group == key && r.key != topKey) { Row ghost = r; ghost.ghost = true; ghost.follow = topKey; ghost.plates = 0; next.push_back(std::move(ghost)); }
            }
        }
    }
    for (const auto& r : rows_) if (r.ghost && std::any_of(next.begin(), next.end(), [&](const Row& n) { return n.key == r.follow && !n.ghost; })
                                     && std::none_of(next.begin(), next.end(), [&](const Row& n) { return n.ghost && n.key == r.key; })) next.push_back(r);  // folds still running
    rows_ = std::move(next);
    builtExpanded_ = expanded_;
}

void Center::Layout(bool snap) {
    if (!g.dwrite) return;
    float y = ListTop();
    for (auto& r : rows_) {
        if (r.ghost) continue;  // placed below, under the card it folds into
        if (r.kind == Row::Kind::Header) r.h = 30.0f;
        else { r.lines = BodyLines(r.item.body, kMaxLines); r.titleLines = TitleLines(r.item.title, TimeLabel(r.item.created)); r.h = CardHeight(r.lines, r.titleLines); }
        r.y.SetSpec(docky::motion::tokens::Reflow);
        if (snap || !r.placed) { r.y.SnapTo(y); r.placed = true; if (snap) r.appear.SnapTo(1.0); }
        else r.y.SetTarget(y);
        y += r.h + (r.kind == Row::Kind::Header ? 6.0f : kCardGap + kPlateStep * static_cast<float>(r.plates));
    }
    for (auto& r : rows_) {
        if (!r.ghost) continue;
        for (const auto& t : rows_) if (!t.ghost && t.key == r.follow) { r.y.SetSpec(docky::motion::tokens::Reflow); r.y.SetTarget(t.y.Target()); break; }
    }
    contentH_ = y;
    const double maxScroll = (std::max)(0.0f, contentH_ - (PillTop() - 12.0f));
    if (scroll_.Target() > maxScroll) scroll_.SetTarget(maxScroll);
}

D2D1_RECT_F Center::RowRect(size_t i) const {
    const auto& r = rows_[i];
    const float top = r.y.F() - scroll_.F();
    return D2D1::RectF(Left(), top, Right(), top + r.h);
}

Center::Hit Center::HitTest(float x, float y, int* index) {
    *index = -1;
    const float dx = static_cast<float>((1.0 - open_.Value()) * Slide());
    x -= dx;
    auto inCircle = [&](D2D1_POINT_2F c, float r) { return (x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) <= r * r; };
    if (!items_.empty()) {
        const auto c = HeaderClose();
        const bool wide = hover_ == Hit::Clear;
        if (wide ? (x >= Right() - 84.0f && x <= Right() + 2.0f && y >= c.y - 14.0f && y <= c.y + 14.0f) : inCircle(c, 14.0f)) return Hit::Clear;
    }
    const auto pill = EditPill();
    if (x >= pill.left && x <= pill.right && y >= pill.top && y <= pill.bottom) return Hit::EditWidgets;
    if (inCircle(BottomClose(), 14.0f)) return Hit::BottomClose;
    if (y < ListTop() - 8.0f || y > PillTop() + 30.0f) return Hit::Empty;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].ghost) continue;
        const auto r = RowRect(i);
        if (rows_[i].kind == Row::Kind::Header) {
            if (x >= Right() - 84.0f && x <= Right() && y >= r.top + 3.0f && y <= r.bottom - 3.0f) { *index = static_cast<int>(i); return Hit::ShowLess; }
            continue;
        }
        if (hoverIndex_ == static_cast<int>(i) && inCircle(D2D1::Point2F(r.left + 4.0f, r.top + 4.0f), 12.0f)) { *index = static_cast<int>(i); return Hit::CardClose; }
        const float bottom = r.bottom + kPlateStep * static_cast<float>(rows_[i].plates);
        if (x >= r.left && x <= r.right && y >= r.top && y <= bottom) { *index = static_cast<int>(i); return Hit::Card; }
    }
    if (items_.empty() && x >= Left() && x <= Right() && y >= ListTop() && y <= ListTop() + 84.0f) return Hit::Info;
    return Hit::Empty;
}

void Center::Frame() {
    if (!hwnd) return;
    const double now = Now();
    const double dt = std::clamp(now - lastT_, 0.0, 0.05);
    lastT_ = now;
    const bool waiting = !closing_ && WaitingForGlass();
    if (g.reducedMotion) open_.SnapTo(waiting ? 0.0 : open_.Target());
    else if (!waiting) open_.Step(dt);
    bool moving = !open_.AtRest() || waiting;
    moving |= scroll_.Step(dt);
    for (auto& r : rows_) { moving |= r.y.Step(dt); moving |= r.appear.Step(dt); }
    const size_t before = rows_.size();
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const Row& r) { return r.ghost && r.y.AtRest(); }), rows_.end());
    if (rows_.size() != before && hoverIndex_ >= 0) { hover_ = Hit::None; hoverIndex_ = -1; }  // indices moved
    animating_ = moving;
    if (closing_ && open_.AtRest()) {
        StopCapture();
        ShowWindow(hwnd, SW_HIDE);
        const bool restore = GetForegroundWindow() == hwnd;
        Destroy();
        if (restore && previous_ && IsWindow(previous_)) SetForegroundWindow(previous_);
        animating_ = false;
        if (g_module) g_module->NextBanner();
        return;
    }
    if (!closing_ && open_.AtRest() && !snapshotTaken_ && now - openedAt_ > 1.5) {
        wchar_t path[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LIQUID_NOTIFY_SNAPSHOT", path, MAX_PATH)) snapshotPath_ = path;
        snapshotTaken_ = true;
        animating_ = true;  // one more frame carries the snapshot
    }
    const float s = std::clamp(open_.F(), 0.0f, 1.2f);
    const float a = std::clamp(open_.F() / 0.55f, 0.0f, 1.0f);
    const float dx = (1.0f - s) * Slide();
    const float edge = std::clamp(open_.F(), 0.0f, 1.0f);
    const float clipTop = ListTop() - 8.0f, clipBottom = PillTop() + 30.0f;
    Clear();
    // Background blur under the whole column: it runs past the top, right and bottom of the window and fades out toward
    // the left in layers of increasing blur (Settings > Menu Bar). It fades with the panel but does not slide.
    // Each layer also darkens by its share of the shade (Settings), so the shade fades out to the left with the blur.
    const float spread = BlurSpread(), shade = 0.6f * static_cast<float>(std::clamp(g.ncShade, 0, 100)) / 100.0f;  // 100 % = 60 % darker
    if (!g.opaque && (spread > 0.001f || shade > 0.001f)) {
        constexpr int kLayers = 4;
        const float fade = padL_ - 14.0f, step = fade / kLayers;
        for (int i = 1; i <= kLayers; ++i) {
            const float soft = step * 0.75f, start = 4.0f + step * static_cast<float>(i - 1);
            const float k = static_cast<float>(i) / kLayers;
            Plate(D2D1::RectF(start + soft, -200.0f, Right() + kMarginR + 200.0f, viewH_ + 200.0f), 0.0f, a, 1.0f,
                  dock::GlassRole::BackdropBlur, soft, spread * k, 1.0f - shade * k);
        }
    }
    const float cardSpread = g.opaque ? -1.0f : spread;
    auto visible = [&](const D2D1_RECT_F& r, float extra) { return r.bottom + extra > clipTop && r.top < clipBottom; };
    std::vector<std::array<bool, 3>> glass(rows_.size(), {false, false, false});  // card, plate 1, plate 2
    for (size_t n = rows_.size(); n-- > 0;) {  // back to front (see Rebuild)
        const size_t i = n;
        const auto& row = rows_[i];
        if (row.kind != Row::Kind::Card) continue;
        auto r = RowRect(i);
        if (!visible(r, kPlateStep * 2.0f)) continue;
        r.left += dx; r.right += dx;
        const float ra = a * row.appear.F() * RowFade(r.top);
        for (int k = row.plates; k >= 1; --k) {  // stacked plates under the card, the farther one first
            const float inset = k == 1 ? kPlateInset1 : kPlateInset2;
            const auto p = D2D1::RectF(r.left + inset, r.bottom - 24.0f, r.right - inset, r.bottom + kPlateStep * static_cast<float>(k));
            glass[i][k] = Plate(p, 17.0f, ra, edge, dock::GlassRole::Notification, 0.0f, cardSpread);
        }
        glass[i][0] = Plate(r, kCardRadius, ra, edge, dock::GlassRole::Notification, 0.0f, cardSpread);
    }
    const D2D1_RECT_F infoRect = D2D1::RectF(Left() + dx, ListTop(), Right() + dx, ListTop() + 84.0f);
    bool infoGlass = false;
    if (items_.empty()) infoGlass = Plate(infoRect, kCardRadius, a, edge, dock::GlassRole::Notification, 0.0f, cardSpread);
    // Bottom controls are glass too (they do not slide: like the blur they belong to the column).
    const auto pill = EditPill();
    const bool pillGlass = Plate(D2D1::RectF(pill.left + dx, pill.top, pill.right + dx, pill.bottom), 12.0f, a, edge, dock::GlassRole::Notification, 0.0f, cardSpread);
    const auto bc = BottomClose();
    const bool closeGlass = Plate(D2D1::RectF(bc.x - 11.5f + dx, bc.y - 11.5f, bc.x + 11.5f + dx, bc.y + 11.5f), 11.5f, a, edge, dock::GlassRole::Notification, 0.0f, cardSpread);
    Begin();
    dc_->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0));
    const auto white = [&](float k) { return g.opaque ? Ink(k * a) : D2D1::ColorF(1, 1, 1, k * a); };
    // Header: "Notification Center" (18 bold, baseline 12 above the first card) and the clear button at the right.
    Text(docky::ui::Text(L"Notification Center"), g.header.Get(), D2D1::RectF(Left() + 2.0f, ListTop() - 31.0f, Right() - 90.0f, ListTop() - 7.0f), white(0.96f));
    if (!items_.empty()) {
        const auto c = HeaderClose();
        if (hover_ == Hit::Clear) {
            const auto p = D2D1::RectF(Right() - 82.0f, c.y - 12.0f, Right(), c.y + 12.0f);
            brush_->SetColor(g.opaque ? Rgb(g.palette.surface, a) : D2D1::ColorF(0.24f, 0.24f, 0.26f, 0.96f * a));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(p, 12.0f, 12.0f), brush_.Get());
            brush_->SetColor(g.opaque ? Rgb(g.palette.divider, a) : D2D1::ColorF(1, 1, 1, 0.16f * a));
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(p, 12.0f, 12.0f), brush_.Get(), 1.0f / scale);
            Text(docky::ui::Text(L"Clear All"), g.button.Get(), p, white(0.9f));
        } else CloseButton(c, 10.0f, a, false);
    }
    dc_->PushAxisAlignedClip(D2D1::RectF(-dx - 20.0f, clipTop, Right() + kMarginR, clipBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (items_.empty()) {
        Body(D2D1::RectF(Left(), ListTop(), Right(), ListTop() + 84.0f), kCardRadius, a, infoGlass);
        std::wstring head = L"No Notifications", detail;
        if (access_ == Access::Off) { head = L"Notifications are not captured"; detail = L"Turn on Capture notifications in DockOver Settings › Menu Bar."; }
        else if (access_ == Access::Denied) { head = L"Notification access is off"; detail = L"Allow DockOver in Windows Settings › Privacy & security › Notifications. Click to open."; }
        else if (access_ == Access::Unavailable) { head = L"Notification access unavailable"; detail = L"Windows did not give DockOver access to notifications on this build."; }
        else if (access_ == Access::Pending) { head = L"Waiting for notification access"; detail = L"Windows has not answered the access request yet."; }
        const float textTop = detail.empty() ? ListTop() + 33.0f : ListTop() + 16.0f;
        Text(head, g.title.Get(), D2D1::RectF(Left() + 18.0f, textTop, Right() - 18.0f, textTop + 20.0f), white(0.95f));
        if (!detail.empty()) Wrapped(detail, g.info.Get(), D2D1::RectF(Left() + 18.0f, textTop + 22.0f, Right() - 18.0f, textTop + 60.0f), white(0.6f));
    }
    for (size_t n = rows_.size(); n-- > 0;) {  // back to front, as the glass pass
        const size_t i = n;
        const auto& row = rows_[i];
        const auto r = RowRect(i);
        if (!visible(r, kPlateStep * 2.0f)) continue;
        const float ra = a * row.appear.F() * RowFade(r.top);
        if (row.kind == Row::Kind::Header) {  // expanded group: app name and "Show Less" (only above a card sliding past it)
            float coverFrom = 1e9f;
            for (size_t j = i + 1; j < rows_.size(); ++j)
                if (rows_[j].kind == Row::Kind::Card) { const auto o = RowRect(j); if (o.top < r.bottom && o.bottom > r.top) coverFrom = (std::min)(coverFrom, o.top); }
            if (coverFrom <= r.top) continue;
            const bool clipped = coverFrom < r.bottom;
            if (clipped) dc_->PushAxisAlignedClip(D2D1::RectF(-dx - 20.0f, clipTop, Right() + kMarginR, coverFrom), D2D1_ANTIALIAS_MODE_ALIASED);
            Text(row.item.app.empty() ? L"Notifications" : row.item.app, g.groupTitle.Get(), D2D1::RectF(r.left + 4.0f, r.top + 4.0f, r.right - 96.0f, r.top + 26.0f), white(0.95f * row.appear.F()));
            const auto p = D2D1::RectF(r.right - 84.0f, r.top + 3.0f, r.right, r.bottom - 3.0f);
            brush_->SetColor(g.opaque ? Rgb(g.palette.surface, ra) : D2D1::ColorF(0.24f, 0.24f, 0.26f, (hover_ == Hit::ShowLess ? 0.98f : 0.85f) * ra));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(p, 12.0f, 12.0f), brush_.Get());
            Text(docky::ui::Text(L"Show Less"), g.button.Get(), p, white(0.88f * row.appear.F()));
            if (clipped) dc_->PopAxisAlignedClip();
            continue;
        }
        for (int k = row.plates; k >= 1; --k) {  // fallback bodies, then the plates darkened like the reference's
            const float inset = k == 1 ? kPlateInset1 : kPlateInset2;
            const auto p = D2D1::RectF(r.left + inset, r.bottom - 24.0f, r.right - inset, r.bottom + kPlateStep * static_cast<float>(k));
            Body(p, 17.0f, ra, glass[i][k]);
            brush_->SetColor(D2D1::ColorF(0, 0, 0, (k == 1 ? 0.28f : 0.42f) * ra));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(p, 17.0f, 17.0f), brush_.Get());
        }
        const bool hot = hoverIndex_ == static_cast<int>(i) && (hover_ == Hit::Card || hover_ == Hit::CardClose);
        // A card sliding out from under an earlier one (unfold / fold): its text and icon only show below that card,
        // since all text is drawn after the glass pass and would otherwise show through the card on top.
        float coveredTo = -1e9f;
        for (size_t j = 0; j < i; ++j)
            if (rows_[j].kind == Row::Kind::Card) { const auto o = RowRect(j); if (o.bottom > r.top && o.top < r.bottom) coveredTo = (std::max)(coveredTo, o.bottom); }
        if (coveredTo > r.top) dc_->PushAxisAlignedClip(D2D1::RectF(-dx - 20.0f, coveredTo, Right() + kMarginR, clipBottom), D2D1_ANTIALIAS_MODE_ALIASED);
        PaintCard(row.item, r, ra, row.lines, hot, hot && pressed_ == Hit::Card, hover_ == Hit::CardClose, TimeLabel(row.item.created), glass[i][0]);
        if (coveredTo > r.top) dc_->PopAxisAlignedClip();
    }
    dc_->PopAxisAlignedClip();
    // Bottom controls.
    Body(pill, 12.0f, a, pillGlass);
    if (hover_ == Hit::EditWidgets) { brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a)); dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, 12.0f, 12.0f), brush_.Get()); }
    Text(docky::ui::Text(L"Edit Widgets"), g.button.Get(), pill, white(0.62f));
    {
        const auto c = BottomClose();
        const auto cr = D2D1::RectF(c.x - 11.5f, c.y - 11.5f, c.x + 11.5f, c.y + 11.5f);
        Body(cr, 11.5f, a, closeGlass);
        if (hover_ == Hit::BottomClose) { brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a)); dc_->FillEllipse(D2D1::Ellipse(c, 11.5f, 11.5f), brush_.Get()); }
        const float k = 4.0f;
        brush_->SetColor(white(0.62f));
        dc_->DrawLine(D2D1::Point2F(c.x - k, c.y - k), D2D1::Point2F(c.x + k, c.y + k), brush_.Get(), 1.5f);
        dc_->DrawLine(D2D1::Point2F(c.x + k, c.y - k), D2D1::Point2F(c.x - k, c.y + k), brush_.Get(), 1.5f);
    }
    End();
    needsFrame = false;
}

LRESULT Center::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE: {
        mx_ = GET_X_LPARAM(l) / scale; my_ = GET_Y_LPARAM(l) / scale;
        int idx = -1;
        const Hit h = HitTest(mx_, my_, &idx);
        const int rowIdx = (h == Hit::Card || h == Hit::CardClose) ? idx : -1;
        if (h != hover_ || rowIdx != hoverIndex_) { hover_ = h; hoverIndex_ = rowIdx; needsFrame = true; }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover_ != Hit::None) { hover_ = Hit::None; hoverIndex_ = -1; needsFrame = true; }
        return 0;
    case WM_LBUTTONDOWN: {
        mx_ = GET_X_LPARAM(l) / scale; my_ = GET_Y_LPARAM(l) / scale;
        pressed_ = HitTest(mx_, my_, &pressedIndex_);
        if (pressed_ == Hit::Empty || pressed_ == Hit::None) { Close(L"clicked empty area"); pressed_ = Hit::None; return 0; }
        SetCapture(hwnd);
        needsFrame = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == hwnd) ReleaseCapture();
        int idx = -1;
        const Hit h = HitTest(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale, &idx);
        const Hit was = pressed_;
        const int wasIdx = pressedIndex_;
        pressed_ = Hit::None;
        needsFrame = true;
        if (h != was || idx != wasIdx) return 0;
        const bool validRow = idx >= 0 && static_cast<size_t>(idx) < rows_.size();
        if (h == Hit::Clear) { owner_->ClearAll(); hover_ = Hit::None; }
        else if (h == Hit::EditWidgets) { if (notify_) PostMessageW(notify_, notifyMsg_, 2, 0); Close(L"edit widgets"); }
        else if (h == Hit::BottomClose) Close(L"close button");
        else if (h == Hit::ShowLess) { expanded_.clear(); Rebuild(); Layout(false); animating_ = true; }
        else if (h == Hit::CardClose && validRow) {
            // A collapsed stack's close button clears the whole app group (macOS); a single card only itself.
            const Row row = rows_[idx];
            hoverIndex_ = -1;
            if (row.count > 1 && row.group != expanded_) { for (const auto& it : std::vector<Item>(items_)) if ((!it.aumid.empty() ? it.aumid : L"app:" + it.app) == row.group) owner_->Remove(it.id); }
            else owner_->Remove(row.item.id);
        } else if (h == Hit::Card && validRow) {
            const Row row = rows_[idx];
            if (row.count > 1 && row.group != expanded_) { expanded_ = row.group; Rebuild(); Layout(false); animating_ = true; }  // open the stack
            else { Close(L"opened notification"); owner_->Launch(row.item); owner_->Remove(row.item.id); }
        } else if (h == Hit::Info && access_ == Access::Denied) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-notifications", nullptr, nullptr, SW_SHOWNORMAL);
            Close(L"opened settings");
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const double maxScroll = (std::max)(0.0f, contentH_ - (PillTop() - 12.0f));
        const double next = std::clamp(scroll_.Target() - GET_WHEEL_DELTA_WPARAM(w) / 120.0 * 72.0, 0.0, maxScroll);
        scroll_.SetTarget(next);
        animating_ = needsFrame = true;
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Close(L"escape");
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) PostMessageW(hwnd, liquid::popup::DismissWatch::FocusMessage, 0, 0);
        else PostMessageW(hwnd, kMsgBelowBar, 0, 0);
        return 0;
    case liquid::popup::DismissWatch::FocusMessage:
        if (dismissal_.LostFocus()) Close(L"lost focus");
        return 0;
    case liquid::popup::DismissWatch::ClickMessage: {
        const POINT pt{static_cast<LONG>(w), static_cast<LONG>(l)};
        RECT r{};
        if (PtInRect(&anchor_, pt)) return 0;  // the clock: the menu bar toggles
        if (GetWindowRect(hwnd, &r) && !PtInRect(&r, pt)) Close(L"clicked outside");
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case kMsgBelowBar:  // after an activation lifted the panel: back under the menu bar (it reaches under the bar)
        if (notify_ && IsWindow(notify_)) SetWindowPos(hwnd, notify_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ---------------------------------------------------------------------------------------------------------------
// Banner
void Banner::Show(const Item& item) {
    item_ = item;
    RefreshTheme();
    EnsureFonts();
    if (!hwnd && !Create(L"DockOverNotificationBanner", L"DockOver Notification", WS_EX_NOACTIVATE)) { DLOG(L"notify: banner window failed %lu", GetLastError()); return; }
    lines_ = BodyLines(item_.body, kBannerLines);
    cardH_ = CardHeight(lines_, TitleLines(item_.title, L"now"));
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const float s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    // 10 DIP below the menu bar (the work area's top when the bar reserves it), 17 DIP from the screen edge (the panel's margin).
    const LONG top = (std::max)(mi.rcWork.top, mi.rcMonitor.top + static_cast<LONG>(std::lround(24 * s))) + static_cast<LONG>(std::lround((10 - kPad) * s));
    const LONG w = static_cast<LONG>(std::lround((kCardW + 2 * kPad) * s)), h = static_cast<LONG>(std::lround((cardH_ + 2 * kPad) * s));
    const LONG right = mi.rcMonitor.right - static_cast<LONG>(std::lround((docky::notification_card::RightMargin - kPad) * s));
    if (!Place(RECT{right - w, top, right, top + h})) { DLOG(L"notify: banner placement failed"); Destroy(); return; }
    leaving_ = false; hover_ = closeHot_ = pressed_ = false;
    // macOS: the banner slides in from beyond the right edge with a short, lightly damped spring.
    slide_.SetSpec(docky::motion::tokens::BannerIn);
    slide_.SnapTo(0.0);
    slide_.SetTarget(1.0);
    StartCapture();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    lastT_ = shownAt_ = Now();
    hideAt_ = shownAt_ + 5.5;
    animating_ = needsFrame = true;
    SetTimer(hwnd, 1, 250, nullptr);
    DLOG(L"notify: banner id=%u app=%ls", item_.id, item_.app.c_str());
}

void Banner::Dismiss(bool toRight) {
    if (!hwnd || leaving_) return;
    leaving_ = true;
    (void)toRight;
    slide_.SetSpec(docky::motion::tokens::BannerOut);
    slide_.SetTarget(0.0);
    animating_ = needsFrame = true;
}

void Banner::Frame() {
    if (!hwnd) return;
    const double now = Now();
    const double dt = std::clamp(now - lastT_, 0.0, 0.05);
    lastT_ = now;
    const bool waiting = !leaving_ && WaitingForGlass();
    if (g.reducedMotion) slide_.SnapTo(waiting ? 0.0 : slide_.Target());
    else if (!waiting) slide_.Step(dt);
    animating_ = !slide_.AtRest() || waiting;
    if (leaving_ && slide_.AtRest()) {
        StopCapture();
        ShowWindow(hwnd, SW_HIDE);
        Destroy();
        animating_ = false;
        if (g_module) g_module->NextBanner();
        return;
    }
    const float s = slide_.F();
    const float dx = (1.0f - s) * (kCardW + 2 * kPad + 12.0f);  // fully beyond the screen edge at 0
    const float a = g.reducedMotion ? std::clamp(s, 0.0f, 1.0f) : 1.0f;
    const auto r = D2D1::RectF(kPad + dx, kPad, kPad + kCardW + dx, kPad + cardH_);
    Clear();
    const bool glass = Plate(r, kCardRadius, a, 1.0f);
    Begin();
    PaintCard(item_, r, a, lines_, hover_, pressed_, closeHot_, L"now", glass);
    End();
    needsFrame = false;
}

LRESULT Banner::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE: {
        mx_ = GET_X_LPARAM(l) / scale; my_ = GET_Y_LPARAM(l) / scale;
        const float cx = kPad + 4, cy = kPad + 4;
        const bool hot = (mx_ - cx) * (mx_ - cx) + (my_ - cy) * (my_ - cy) <= 144.0f;
        if (!hover_ || hot != closeHot_) { hover_ = true; closeHot_ = hot; needsFrame = true; }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = closeHot_ = pressed_ = false;
        hideAt_ = (std::max)(hideAt_, Now() + 2.0);
        needsFrame = true;
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = true; needsFrame = true; SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        if (GetCapture() == hwnd) ReleaseCapture();
        const bool was = pressed_;
        pressed_ = false; needsFrame = true;
        if (!was || leaving_) return 0;
        RECT c{}; GetClientRect(hwnd, &c);
        const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!PtInRect(&c, pt)) return 0;
        const Item it = item_;
        Dismiss();
        if (closeHot_) { owner_->Remove(it.id); return 0; }
        owner_->Launch(it);
        if (!it.aumid.empty()) owner_->Remove(it.id);
        return 0;
    }
    case WM_RBUTTONUP:
        Dismiss();
        return 0;
    case WM_TIMER:
        if (!hover_ && !leaving_ && Now() >= hideAt_) { KillTimer(hwnd, 1); Dismiss(); }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ---------------------------------------------------------------------------------------------------------------
// Module
void Module::Merge() {
    std::vector<Item> next;
    Access acc = Access::Off;
    if (reader) reader->Latest(next, acc);
    if (!capture) { next.clear(); acc = Access::Off; }
    // New arrivals become banners; whatever exists when capture starts (or access is granted) is history.
    if (acc == Access::Allowed) {
        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        const std::int64_t now = (static_cast<std::int64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
        for (const auto& it : next) {
            if (seen.insert(it.id).second && primed && now - it.created < 600000000ll * 2 && !center.IsOpen()) {
                if (banner.Active()) queue.push_back(it);
                else banner.Show(it);
            }
        }
        primed = true;
    } else primed = false;
    for (const auto& s : synthetic) next.insert(next.begin(), s);
    items = std::move(next);
    access = acc;
    SyncHider();
    center.SetItems(items, access);
}

LRESULT CALLBACK Module::HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Module* self = g_module;
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case kMsgItems:
        self->Merge();
        return 0;
    case kMsgCapture: {
        bool on;
        { std::lock_guard<std::mutex> lock(g_mutex); on = g_capture; }
        if (on != self->capture) {
            self->capture = on;
            self->primed = false;
            self->seen.clear();
            if (self->reader) self->reader->SetEnabled(on);
            DLOG(L"notify: capture %ls", on ? L"on" : L"off");
            self->Merge();
        }
        return 0;
    }
    case kMsgToggle: {
        ToggleRequest req;
        { std::lock_guard<std::mutex> lock(g_mutex); req = g_toggle; }
        if (self->center.IsOpen()) self->center.Close(L"toggle");
        else {
            if (self->banner.Active()) self->banner.Dismiss();
            if (self->center.Exists()) self->center.Destroy();
            self->center.Open(req.anchor, req.notify, req.msg);
            if (self->reader) self->reader->Poll();
        }
        return 0;
    }
    case kMsgHideSystem: {
        bool on;
        { std::lock_guard<std::mutex> lock(g_mutex); on = g_hideSystem; }
        self->hideSystem = on;
        self->SyncHider();
        return 0;
    }
    case kMsgCloseCenter:
        self->center.Close(L"request");
        return 0;
    case kMsgDebug: {
        DLOG(L"notify: debug notifications requested");
        std::vector<DebugRequest> list;
        { std::lock_guard<std::mutex> lock(g_mutex); list.swap(g_debug); }
        static std::uint32_t next = 0x80000001u;
        for (auto& d : list) {
            Item it;
            it.id = next++;
            it.app = d.app; it.title = d.title; it.body = d.body; it.synthetic = true;
            FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
            it.created = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            self->synthetic.insert(self->synthetic.begin(), it);
            if (!self->center.IsOpen()) { if (self->banner.Active()) self->queue.push_back(it); else self->banner.Show(it); }
        }
        self->Merge();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (RefreshTheme()) { self->center.needsFrame = self->banner.needsFrame = true; }
        return 0;
    case WM_TIMER:
        if (w == 2) { KillTimer(h, 2); if (self->reader) self->reader->Poll(); return 0; }
        if (w == 1) { self->hider.Maintain(); if (RefreshTheme()) self->center.needsFrame = self->banner.needsFrame = true; }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

int Module::Run() {
    g_module = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DockOverNotificationHost";
    RegisterClassExW(&wc);
    host = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!host) { g_module = nullptr; return 1; }
    g_host.store(host);
    RefreshTheme();
    reader = std::make_unique<Reader>(host);
    // Poll now and once more shortly after: Windows may list the toast a moment after it starts showing it.
    hider.onToast = [this] { if (reader) reader->Poll(); SetTimer(host, 2, 350, nullptr); };
    SetTimer(host, 1, 3000, nullptr);
    PostMessageW(host, kMsgCapture, 0, 0);
    PostMessageW(host, kMsgHideSystem, 0, 0);
    MSG msg{};
    for (;;) {
        HANDLE handles[2];
        GlassView* owners[2];
        DWORD n = 0;
        for (GlassView* v : {static_cast<GlassView*>(&center), static_cast<GlassView*>(&banner)}) {
            if (!v->hwnd) continue;
            if (HANDLE e = v->CompletionEvent()) { handles[n] = e; owners[n] = v; ++n; }
        }
        const bool anim = (center.hwnd && (center.Animating() || center.needsFrame)) || (banner.hwnd && (banner.Animating() || banner.needsFrame));
        const DWORD r = MsgWaitForMultipleObjectsEx(n, n ? handles : nullptr, anim ? 0 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r < WAIT_OBJECT_0 + n) owners[r - WAIT_OBJECT_0]->OnCompletion();
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        bool drew = false;
        if (center.hwnd && (center.Animating() || center.needsFrame)) { center.Frame(); drew = true; }
        if (banner.hwnd && (banner.Animating() || banner.needsFrame)) { banner.Frame(); drew = true; }
        if (drew) DwmFlush();  // one frame per composition
    }
    hider.SetActive(false);
    center.Destroy();
    banner.Destroy();
    reader.reset();
    g_host.store(nullptr);
    DestroyWindow(host);
    host = nullptr;
    g_module = nullptr;
    g.ctx.Reset(); g.d3d.Reset();
    return 0;
}

}  // namespace

void Start(const std::wstring& settingsPath) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running.load()) return;
    g_settingsPath = settingsPath;
    g.settingsPath = settingsPath;
    IconSettingsPath() = settingsPath;
    g_running.store(true);
    g_thread = std::thread([] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        { Module m; m.Run(); }
        if (SUCCEEDED(co)) CoUninitialize();
        g_running.store(false);
    });
}

void SetCapture(bool enabled) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_capture = enabled; }
    if (HWND h = g_host.load()) PostMessageW(h, kMsgCapture, 0, 0);
}

void SetHideSystemBanners(bool hide) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_hideSystem = hide; }
    if (HWND h = g_host.load()) PostMessageW(h, kMsgHideSystem, 0, 0);
}

void ToggleCenter(const RECT& anchor, HWND notify, UINT notifyMessage) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_toggle = ToggleRequest{anchor, notify, notifyMessage}; }
    if (HWND h = g_host.load()) PostMessageW(h, kMsgToggle, 0, 0);
}

void CloseCenter() { if (HWND h = g_host.load()) PostMessageW(h, kMsgCloseCenter, 0, 0); }

bool IsCenterOpen() { return g_centerOpen.load(); }

void DebugBanner(const std::wstring& app, const std::wstring& title, const std::wstring& body) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_debug.push_back({app, title, body}); }
    if (HWND h = g_host.load()) PostMessageW(h, kMsgDebug, 0, 0);
}

void Shutdown() {
    if (HWND h = g_host.load()) PostMessageW(h, WM_QUIT, 0, 0);
    std::thread t;
    { std::lock_guard<std::mutex> lock(g_mutex); t.swap(g_thread); }
    if (t.joinable()) t.join();
}

}  // namespace docky::notify
