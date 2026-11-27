// docky-now-playing: borderless full-screen Now Playing, modelled on the Apple Music lock screen of
// macOS / iPadOS 26. One layout for motion (video) and still artwork:
//   * the artwork is shown whole and sharp ("Extend", default) and the rest of the screen is the
//     artwork mirrored past its edges and blurred (background extension), the sharp edges feathered
//     into it so there is no visible seam; "Fill" crops the artwork to cover the screen instead (F key);
//   * lock-screen clock at the top (C key), glass transport platter at the bottom that fades away
//     with the cursor after a few idle seconds;
//   * track changes and still -> motion upgrades cross-fade.
// Media: Windows media session (any player). Artwork: session thumbnail at once, then the Apple Music
// catalog still (2000 px) and motion artwork when they exist (artwork_source.h), cached on disk.
// All composition is Direct2D on the GPU; motion artwork is decoded by Media Foundation straight into
// a D3D11 texture. Esc closes; Space play/pause; Left/Right previous/next; Up/Down volume.
//
// --wallpaper: the same scene without clock or controls, behind the desktop icons, shown only while Apple Music plays
//   (Settings > Control Center > Music wallpaper; started and stopped by the Manager). Your wallpaper shows otherwise.
//
// Diagnostics: --snapshot out.png --image art.jpg [--video art.mp4] [--size 1920x1080] [--fill] [--no-clock]
//              --fetch "Artist" "Album" ["Title"] [--thumb cover.jpg]
//              --lyrics-fetch "Artist" "Title" ["Album"] [--duration seconds]; --snapshot ... --lyrics song.lrc
// Keys: L lyrics view on/off (Apple Music style: artwork and controls left, lyrics right; click a line to seek).
#include "artwork_match.h"
#include "../ui_language.h"
#include "artwork_source.h"
#include "audio_beat.h"
#include "display_adapter.h"
#include "../suite_install.h"
#include "../music_wallpaper_frame.h"
#include "../color_output.h"
#include "lyrics_source.h"
#include "lyrics_clock.h"
#include "local_tags.h"
#include "silk_renderer.h"
#include "../desktop_settings.h"
#include "../vblank.h"
#include "../json_lite.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d2d1effects_2.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <endpointvolume.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <UIAutomation.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nowplaying;

namespace {

constexpr wchar_t kClass[] = L"DockyNowPlaying";
constexpr wchar_t kWallClass[] = L"DockyNowPlayingWallpaper", kWallCtlClass[] = L"DockyNowPlayingWallpaperCtl";
constexpr UINT kMediaMsg = WM_APP + 1, kArtMsg = WM_APP + 2, kLyricsMsg = WM_APP + 3;

// Music players whose media session drives the wallpaper (substrings of the app id, any case). Browsers are
// left out on purpose: their sessions are mostly videos.
const std::vector<std::wstring> kMusicApps = {
    L"applemusic", L"itunes", L"spotify", L"cloudmusic", L"netease", L"qqmusic", L"youtube-music", L"youtubemusic",
    L"cinhimbnkkaeohfgghhklpknlkffjgod" /* YouTube Music installed from Chrome or Edge */, L"tidal", L"deezer", L"amazonmusic",
    L"kugou", L"kuwo", L"foobar2000", L"musicbee", L"aimp", L"winamp", L"zunemusic" /* Windows Media Player */};

double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return double(c.QuadPart) / double(f.QuadPart); }
float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
float EaseInOut(float t) { t = Clamp01(t); return t < .5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }
float Approach(float v, float target, float dt, float tau) { return target + (v - target) * std::exp(-dt / tau); }

// ---------------------------------------------------------------- settings (now-playing.ini)

struct Config { bool extend = true, clock = true, online = true, lyrics = true; };

std::wstring IniPath() { return ArtworkSource::CacheDir() + L"\\..\\now-playing.ini"; }
Config LoadConfig() {
    const auto p = IniPath(); Config c;
    c.extend = GetPrivateProfileIntW(L"now-playing", L"extend", 1, p.c_str()) != 0;
    c.clock = GetPrivateProfileIntW(L"now-playing", L"clock", 1, p.c_str()) != 0;
    c.online = GetPrivateProfileIntW(L"now-playing", L"online", 1, p.c_str()) != 0;
    c.lyrics = GetPrivateProfileIntW(L"now-playing", L"lyrics", 1, p.c_str()) != 0;
    return c;
}
void SaveConfig(const Config& c) {
    const auto p = IniPath();
    WritePrivateProfileStringW(L"now-playing", L"extend", c.extend ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"now-playing", L"clock", c.clock ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"now-playing", L"online", c.online ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"now-playing", L"lyrics", c.lyrics ? L"1" : L"0", p.c_str());
}

// ---------------------------------------------------------------- media session

struct Media {
    bool has = false, playing = false, canPlayPause = false, canNext = false, canPrev = false, canSeek = false;
    std::wstring title, artist, album, source;
    std::wstring localLyrics;  // lyrics of a matched local music file (its .lrc or lyrics tag), else empty
    int64_t position = 0, end = 0, updated = 0;  // 100 ns; updated = FILETIME (UTC) of the position sample
    double anchoredAt = -1, rate = 1;
    std::shared_ptr<const std::vector<uint8_t>> thumb; uint32_t thumbW = 0, thumbH = 0; uint64_t thumbHash = 0;
    std::wstring TrackKey() const { return source + L"\x1f" + title + L"\x1f" + artist + L"\x1f" + album; }
    int64_t PositionNow() const {
        if (!playing || anchoredAt < 0) return position;
        const auto advanced = position + int64_t(std::max(0.0, Now() - anchoredAt) * rate * lyrics::kTicksPerSecond);
        return end > 0 ? std::clamp<int64_t>(advanced, 0, end) : std::max<int64_t>(0, advanced);
    }
};

enum class Cmd { PlayPause, Next, Prev, Seek };

class MediaWorker {
public:
    // apps: empty = Windows' current session; otherwise only sessions whose app id contains one of them (any case):
    // the current session when it is one and playing, else a playing one, else the current or first one of them.
    explicit MediaWorker(HWND notify, std::vector<std::wstring> apps = {}) : hwnd_(notify), apps_(std::move(apps)), thread_([this] { Loop(); }) {}
    ~MediaWorker() { { std::lock_guard l(m_); stop_ = true; } cv_.notify_all(); thread_.join(); }
    Media Snapshot() { std::lock_guard l(m_); return media_; }
    void SetLocalTags(bool on) { localTags_ = on; }
    void Command(Cmd c, int64_t arg = 0) { { std::lock_guard l(m_); cmds_.push_back({c, arg}); } cv_.notify_all(); }

private:
    void Loop() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using namespace winrt::Windows::Media::Control;
        GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
        std::wstring lastKey; int thumbTries = 0;
        for (;;) {
            std::vector<std::pair<Cmd, int64_t>> cmds;
            {
                std::unique_lock l(m_);
                if (polled_) cv_.wait_for(l, std::chrono::milliseconds(400), [&] { return stop_ || !cmds_.empty(); });
                if (stop_) break;
                cmds.swap(cmds_); polled_ = true;
            }
            try {
                if (!manager) manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                Media next;
                GlobalSystemMediaTransportControlsSession s{nullptr};
                if (apps_.empty()) s = manager.GetCurrentSession();
                else {
                    auto music = [&](const GlobalSystemMediaTransportControlsSession& c) {
                        const std::wstring id = Lower(c.SourceAppUserModelId().c_str());
                        // Local file players count too while Settings > Music > Match local music files is on.
                        static const wchar_t* const kLocalPlayers[] = {L"vlc", L"potplayer", L"dopamine", L"mpc-hc", L"mpc-be", L"musicplayer"};
                        return std::any_of(apps_.begin(), apps_.end(), [&](const std::wstring& a) { return id.find(a) != std::wstring::npos; }) ||
                               (localTags_ && std::any_of(std::begin(kLocalPlayers), std::end(kLocalPlayers), [&](const wchar_t* a) { return id.find(a) != std::wstring::npos; }));
                    };
                    auto playing = [](const GlobalSystemMediaTransportControlsSession& c) {
                        return c.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                    };
                    const auto current = manager.GetCurrentSession();
                    GlobalSystemMediaTransportControlsSession first{nullptr};
                    if (current && music(current) && playing(current)) s = current;
                    else for (const auto& c : manager.GetSessions()) {
                        if (!music(c)) continue;
                        if (!first) first = c;
                        if (playing(c)) { s = c; break; }
                    }
                    if (!s) s = current && music(current) ? current : first;
                }
                if (s) {
                    for (auto [c, arg] : cmds) {
                        try {
                            if (c == Cmd::PlayPause) s.TryTogglePlayPauseAsync().get();
                            else if (c == Cmd::Next) s.TrySkipNextAsync().get();
                            else if (c == Cmd::Prev) s.TrySkipPreviousAsync().get();
                            else if (s.GetPlaybackInfo().Controls().IsPlaybackPositionEnabled()) s.TryChangePlaybackPositionAsync(arg).get();
                            else if (Lower(s.SourceAppUserModelId().c_str()).find(L"applemusic") != std::wstring::npos) SeekAppleMusic(double(arg) / 1e7);
                        } catch (...) {}
                    }
                    next.has = true; next.source = s.SourceAppUserModelId().c_str();
                    const auto info = s.GetPlaybackInfo();
                    next.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                    if (const auto rate = info.PlaybackRate(); rate && rate.Value() > 0 && rate.Value() <= 4) next.rate = rate.Value();
                    const auto c = info.Controls();
                    next.canPlayPause = c.IsPlayPauseToggleEnabled(); next.canNext = c.IsNextEnabled(); next.canPrev = c.IsPreviousEnabled();
                    next.canSeek = c.IsPlaybackPositionEnabled() || Lower(next.source).find(L"applemusic") != std::wstring::npos;  // Apple Music: via its slider
                    const auto tl = s.GetTimelineProperties();
                    next.position = (tl.Position() - tl.StartTime()).count(); next.end = (tl.EndTime() - tl.StartTime()).count();
                    next.updated = tl.LastUpdatedTime().time_since_epoch().count();
                    if (next.updated <= 0 || next.updated > (int64_t(1) << 62)) next.updated = 0;
                    const auto props = s.TryGetMediaPropertiesAsync().get();
                    next.title = props.Title().c_str(); next.artist = props.Artist().c_str(); next.album = props.AlbumTitle().c_str();
                    localtags::Result local;
                    if (localTags_) {  // "song.mp3" or no artist: the file's own tags, cover and lyrics (local_tags.h)
                        local = resolver_.Resolve(next.title, next.artist, next.album);
                        if (local.changed) {
                            next.title = local.title; next.artist = local.artist; next.album = local.album;
                            if (next.end <= 0 && local.duration > 0) next.end = local.duration;
                            next.localLyrics = !local.lyricsBytes.empty() ? LyricsSource::DecodeText(local.lyricsBytes) : local.lyricsText;
                        }
                    }
                    const std::wstring key = next.TrackKey();
                    Media prev; { std::lock_guard l(m_); prev = media_; }
                    if (key != lastKey) { lastKey = key; thumbTries = 0; }
                    else { next.thumb = prev.thumb; next.thumbW = prev.thumbW; next.thumbH = prev.thumbH; next.thumbHash = prev.thumbHash; }
                    // Players often publish the thumbnail a moment after the title: re-read it a few times.
                    if (thumbTries < 8 && props.Thumbnail()) { ++thumbTries; DecodeThumb(props, next); }
                    if (!next.thumb && local.cover) {  // the player shows no cover: the one embedded in the file
                        next.thumb = local.cover; next.thumbW = local.coverW; next.thumbH = local.coverH;
                        next.thumbHash = 0x9E3779B97F4A7C15ull ^ (uint64_t(local.coverW) << 32) ^ local.coverH ^ std::hash<std::wstring>{}(local.path);
                    }
                }
                bool changed;
                {
                    std::lock_guard l(m_);
                    changed = next.has != media_.has || next.playing != media_.playing || next.TrackKey() != media_.TrackKey() ||
                              next.thumbHash != media_.thumbHash || next.position != media_.position || next.end != media_.end ||
                              next.updated != media_.updated || next.rate != media_.rate ||
                              next.localLyrics != media_.localLyrics || next.canPlayPause != media_.canPlayPause || next.canNext != media_.canNext || next.canPrev != media_.canPrev;
                    media_ = std::move(next);
                }
                if (changed || !posted_) { posted_ = true; PostMessageW(hwnd_, kMediaMsg, 0, 0); }
            } catch (...) { manager = nullptr; }
        }
    }

    static void DecodeThumb(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& props, Media& out) {
        try {
            using namespace winrt::Windows::Graphics::Imaging;
            auto stream = props.Thumbnail().OpenReadAsync().get();
            if (stream.Size() > 16 * 1024 * 1024) return;
            auto decoder = BitmapDecoder::CreateAsync(stream).get();
            const uint32_t w = decoder.PixelWidth(), h = decoder.PixelHeight();
            if (!w || !h) return;
            const float ratio = std::min(1.f, 1024.f / float(std::max(w, h)));
            const uint32_t aw = std::max(1u, uint32_t(w * ratio)), ah = std::max(1u, uint32_t(h * ratio));
            BitmapTransform t; t.ScaledWidth(aw); t.ScaledHeight(ah); t.InterpolationMode(BitmapInterpolationMode::Fant);
            auto bytes = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, t, ExifOrientationMode::RespectExifOrientation,
                                                   ColorManagementMode::ColorManageToSRgb).get().DetachPixelData();
            if (bytes.size() != size_t(aw) * ah * 4) return;
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t i = 0; i < bytes.size(); i += 7) { hash ^= bytes[i]; hash *= 1099511628211ull; }
            if (hash == out.thumbHash) return;
            out.thumb = std::make_shared<std::vector<uint8_t>>(bytes.begin(), bytes.end());
            out.thumbW = aw; out.thumbH = ah; out.thumbHash = hash;
        } catch (...) {}
    }

    // Apple Music for Windows reports that it cannot seek and ignores position requests from the media session. Its
    // own progress slider (UI Automation id "LCDScrubber", in seconds) is set through the accessibility interface
    // instead, as a screen reader would.
    static bool SeekAppleMusic(double seconds) {
        // Apple Music has several windows of this class (main window, hidden helpers): try each, visible ones first.
        std::vector<HWND> windows;
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            wchar_t cls[64]{}; GetClassNameW(w, cls, 64);
            if (wcscmp(cls, L"WinUIDesktopWin32WindowClass")) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!p) return TRUE;
            wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH; const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != FALSE; CloseHandle(p);
            if (ok && std::wstring(path).find(L"AppleMusic.exe") != std::wstring::npos) reinterpret_cast<std::vector<HWND>*>(l)->push_back(w);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
        std::stable_partition(windows.begin(), windows.end(), [](HWND w) { return IsWindowVisible(w) != FALSE; });
        if (windows.empty()) { Log(L"seek: Apple Music window not found"); return false; }
        ComPtr<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (FAILED(hr)) { Log(L"seek: UI Automation unavailable (0x%08lx)", static_cast<unsigned long>(hr)); return false; }
        VARIANT id; VariantInit(&id); id.vt = VT_BSTR; id.bstrVal = SysAllocString(L"LCDScrubber");
        ComPtr<IUIAutomationCondition> cond; hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond); VariantClear(&id);
        if (FAILED(hr)) return false;
        for (HWND window : windows) {
            ComPtr<IUIAutomationElement> root, slider; ComPtr<IUIAutomationRangeValuePattern> range;
            if (FAILED(uia->ElementFromHandle(window, &root)) || !root || FAILED(root->FindFirst(TreeScope_Descendants, cond.Get(), &slider)) || !slider ||
                FAILED(slider->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) || !range) continue;
            hr = range->SetValue(seconds);
            Log(L"seek: Apple Music slider set to %.1f s (0x%08lx)", seconds, static_cast<unsigned long>(hr));
            return SUCCEEDED(hr);
        }
        Log(L"seek: Apple Music slider not found in %zu window(s)", windows.size());
        return false;
    }
    static std::wstring Lower(std::wstring v) { if (!v.empty()) CharLowerBuffW(v.data(), DWORD(v.size())); return v; }
    HWND hwnd_; std::vector<std::wstring> apps_;
    std::atomic<bool> localTags_{true}; localtags::Resolver resolver_;  // resolver_: worker thread only
    std::mutex m_; std::condition_variable cv_;
    Media media_; std::vector<std::pair<Cmd, int64_t>> cmds_; bool stop_ = false, polled_ = false, posted_ = false;
    std::thread thread_;
};

// ---------------------------------------------------------------- system volume

class Volume {
public:
    Volume() {
        ComPtr<IMMDeviceEnumerator> e; ComPtr<IMMDevice> d;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e))) &&
            SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eMultimedia, &d)))
            d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep_.GetAddressOf()));
    }
    bool Ok() const { return ep_ != nullptr; }
    float Get() const { float v = 0; if (ep_) ep_->GetMasterVolumeLevelScalar(&v); return v; }
    bool Muted() const { BOOL m = FALSE; if (ep_) ep_->GetMute(&m); return m != FALSE; }
    void Set(float v) { if (ep_) { ep_->SetMasterVolumeLevelScalar(Clamp01(v), nullptr); if (v > 0) ep_->SetMute(FALSE, nullptr); } }
private:
    ComPtr<IAudioEndpointVolume> ep_;
};

// ---------------------------------------------------------------- motion artwork (Media Foundation)

class EngineNotify : public IMFMediaEngineNotify {
public:
    std::atomic<bool> error{false};
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) { *out = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override { if (event == MF_MEDIA_ENGINE_EVENT_ERROR) error = true; return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};

class VideoLayer {
public:
    ~VideoLayer() { if (engine_) engine_->Shutdown(); if (notify_) notify_->Release(); }
    bool Open(ID3D11Device* device, IMFDXGIDeviceManager* manager, ID2D1DeviceContext* dc, const std::wstring& path) {
        device_ = device; dc_ = dc;
        ComPtr<IMFMediaEngineClassFactory> factory; ComPtr<IMFAttributes> attrs;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(MFCreateAttributes(&attrs, 3))) return false;
        notify_ = new EngineNotify();
        attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager);
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_);
        attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (FAILED(factory->CreateInstance(0, attrs.Get(), &engine_))) return false;
        engine_->SetMuted(TRUE); engine_->SetVolume(0); engine_->SetLoop(TRUE); engine_->SetAutoPlay(TRUE);
        BSTR src = SysAllocString(path.c_str());
        const HRESULT hr = engine_->SetSource(src); SysFreeString(src);
        return SUCCEEDED(hr);
    }
    bool Failed() const { return !engine_ || (notify_ && notify_->error.load()); }
    void LogState(const wchar_t* where) const {
        if (!engine_) { Log(L"video %ls: no engine", where); return; }
        ComPtr<IMFMediaError> e; engine_->GetError(&e);
        Log(L"video %ls: ready=%u network=%u error=%u hr=0x%08lx paused=%d frames=%llu video=%d t=%.2f dur=%.2f tick=0x%08lx", where, engine_->GetReadyState(), engine_->GetNetworkState(),
            e ? e->GetErrorCode() : 0, e ? static_cast<unsigned long>(e->GetExtendedErrorCode()) : 0ul, engine_->IsPaused(), static_cast<unsigned long long>(frames_), engine_->HasVideo(), engine_->GetCurrentTime(), engine_->GetDuration(), static_cast<unsigned long>(lastTick_));
    }
    bool HasFrame() const { return frames_ > 0; }
    uint64_t Frames() const { return frames_; }
    ID2D1Bitmap1* Bitmap() const { return bitmap_.Get(); }
    void SetPlaying(bool play) {
        if (!engine_ || play == playing_) return;
        playing_ = play; if (play) engine_->Play(); else engine_->Pause();
    }
    // True when a new frame landed in the texture.
    bool Update() {
        if (!engine_ || Failed()) return false;
        if (engine_->IsEnded() && playing_) { engine_->SetCurrentTime(0); engine_->Play(); }  // loop, also for streams of unknown length
        LONGLONG pts = 0;
        if ((lastTick_ = engine_->OnVideoStreamTick(&pts)) != S_OK) return false;
        DWORD w = 0, h = 0;
        if (FAILED(engine_->GetNativeVideoSize(&w, &h)) || !w || !h) return false;
        if (!texture_ || w != w_ || h != h_) {
            texture_.Reset(); bitmap_.Reset();
            D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &texture_))) return false;
            ComPtr<IDXGISurface> surface; texture_.As(&surface);
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap_))) { texture_.Reset(); return false; }
            w_ = w; h_ = h;
        }
        const MFVideoNormalizedRect srcRect{0, 0, 1, 1}; const RECT dst{0, 0, LONG(w), LONG(h)}; const MFARGB border{0, 0, 0, 255};
        if (FAILED(engine_->TransferVideoFrame(texture_.Get(), &srcRect, &dst, &border))) return false;
        ++frames_; return true;
    }
private:
    ComPtr<IMFMediaEngine> engine_; EngineNotify* notify_ = nullptr;
    ComPtr<ID3D11Device> device_; ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID3D11Texture2D> texture_; ComPtr<ID2D1Bitmap1> bitmap_;
    DWORD w_ = 0, h_ = 0; uint64_t frames_ = 0; bool playing_ = true; HRESULT lastTick_ = 0;
};

// One artwork as drawn: a still, or motion artwork (with the still under it until the first frame).
struct Art {
    ComPtr<ID2D1Bitmap1> still;
    std::unique_ptr<VideoLayer> video;
    int quality = 0;  // 0 session thumbnail, 1 catalog still, 2 motion
    std::vector<D2D1_COLOR_F> palette;  // dominant colours for the flowing background
    int videoPalette = 0;  // motion: 0 palette not yet from the video, 1 from its first frame, 2 from a frame ~1.5 s in
    ID2D1Bitmap1* Image() const { return video && video->HasFrame() ? video->Bitmap() : still.Get(); }
};

// ---------------------------------------------------------------- renderer

class Scene {
public:
    bool Init(ID3D11Device* device) {
        device_ = device;
        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &fo, &factory_))) return false;
        ComPtr<IDXGIDevice> dxgi; device->QueryInterface(IID_PPV_ARGS(&dxgi));
        ComPtr<ID2D1Device2> d2d;
        if (FAILED(factory_->CreateDevice(dxgi.Get(), &d2d)) || FAILED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
        dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        if (dc_->IsBufferPrecisionSupported(D2D1_BUFFER_PRECISION_16BPC_FLOAT)) {
            D2D1_RENDERING_CONTROLS controls{};
            dc_->GetRenderingControls(&controls);
            controls.bufferPrecision = D2D1_BUFFER_PRECISION_16BPC_FLOAT;
            dc_->SetRenderingControls(&controls);
        }
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) return false;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
        dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
        for (int i = 0; i < 2; ++i) {
            auto& c = chains_[i];
            if (FAILED(dc_->CreateEffect(CLSID_D2D1Border, &c.border)) || FAILED(dc_->CreateEffect(CLSID_D2D12DAffineTransform, &c.affine)) ||
                FAILED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &c.blur)) || FAILED(dc_->CreateEffect(CLSID_D2D1Saturation, &c.saturate)) ||
                FAILED(dc_->CreateEffect(CLSID_D2D1Crop, &c.crop))) return false;
            c.border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_MIRROR);
            c.border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_MIRROR);
            c.affine->SetInputEffect(0, c.border.Get());
            c.affine->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE, D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.blur->SetInputEffect(0, c.affine.Get());
            c.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            c.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
            c.saturate->SetInputEffect(0, c.blur.Get());
            c.saturate->SetValue(D2D1_SATURATION_PROP_SATURATION, 1.25f);
            c.crop->SetInputEffect(0, c.saturate.Get());
        }
        const D2D1_GRADIENT_STOP feather[] = {{0, D2D1::ColorF(1, 1, 1, 0)}, {.075f, D2D1::ColorF(1, 1, 1, 1)}, {.925f, D2D1::ColorF(1, 1, 1, 1)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
        dc_->CreateGradientStopCollection(feather, 4, &featherStops_);
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), featherStops_.Get(), &featherBrush_);
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), featherStops_.Get(), &featherBrushV_);
        const D2D1_GRADIENT_STOP fadeIn[] = {{0, D2D1::ColorF(1, 1, 1, 0)}, {1, D2D1::ColorF(1, 1, 1, 1)}};
        ComPtr<ID2D1GradientStopCollection> fadeStops; dc_->CreateGradientStopCollection(fadeIn, 2, &fadeStops);
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), fadeStops.Get(), &silkFade_);
        if (fadeStops) dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), fadeStops.Get(), &wordSweep_);
        dc_->CreateEffect(CLSID_D2D1Border, &silkBorder_); dc_->CreateEffect(CLSID_D2D1GaussianBlur, &silkBlur_); dc_->CreateEffect(CLSID_D2D1Crop, &silkCrop_);
        if (silkBorder_ && silkBlur_ && silkCrop_) {
            silkBorder_->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP); silkBorder_->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
            silkBlur_->SetInputEffect(0, silkBorder_.Get()); silkBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            silkCrop_->SetInputEffect(0, silkBlur_.Get());
        }
        return true;
    }

    ID2D1DeviceContext2* Dc() const { return dc_.Get(); }

    void SetScale(float s) {
        if (std::abs(s - scale_) < .001f && title_) return;
        scale_ = s;
        auto fmt = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out, DWRITE_TEXT_ALIGNMENT align) {
            out.Reset();
            dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
            if (!out) return;
            out->SetTextAlignment(align); out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; ComPtr<IDWriteInlineObject> sign;
            dwrite_->CreateEllipsisTrimmingSign(out.Get(), &sign); out->SetTrimming(&trim, sign.Get());
        };
        fmt(L"Segoe UI Variable Display", 21 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_, DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt(L"Segoe UI Variable Text", 16 * s, DWRITE_FONT_WEIGHT_NORMAL, artist_, DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, timeL_, DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, timeR_, DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmt(L"Segoe UI Variable Text", 14 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, pill_, DWRITE_TEXT_ALIGNMENT_CENTER);
        lyricsKey_ = nullptr;
        clockSize_ = 0;
    }

    void SetClockSize(float screenH) {
        if (std::abs(clockSize_ - screenH) < 1) return;
        clockSize_ = screenH;
        auto mk = [&](float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
            out.Reset();
            dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
            if (out) { out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        };
        mk(screenH * .135f, DWRITE_FONT_WEIGHT_SEMI_BOLD, clock_);
        mk(screenH * .022f, DWRITE_FONT_WEIGHT_SEMI_BOLD, date_);
    }

    ComPtr<ID2D1Bitmap1> LoadFile(const std::wstring& path) {
        ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> conv; ComPtr<ID2D1Bitmap1> out;
        if (!wic_ || FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &frame))) return out;
        ComPtr<IWICBitmapSource> src = frame; UINT w = 0, h = 0; frame->GetSize(&w, &h);
        if (std::max(w, h) > 4096) {
            ComPtr<IWICBitmapScaler> scaler; const float r = 4096.f / float(std::max(w, h));
            if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(frame.Get(), UINT(w * r), UINT(h * r), WICBitmapInterpolationModeFant))) src = scaler;
        }
        WICPixelFormatGUID sourceFormat{};
        if (SUCCEEDED(src->GetPixelFormat(&sourceFormat)) &&
            (sourceFormat == GUID_WICPixelFormat16bppGray || sourceFormat == GUID_WICPixelFormat48bppRGB ||
             sourceFormat == GUID_WICPixelFormat64bppRGBA || sourceFormat == GUID_WICPixelFormat64bppPRGBA) &&
            dc_->IsDxgiFormatSupported(DXGI_FORMAT_R16G16B16A16_UNORM)) {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat64bppPRGBA, WICBitmapDitherTypeNone,
                                           nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(dc_->CreateBitmapFromWicBitmap(conv.Get(), &props, &out))) return out;
            conv.Reset();
            out.Reset();
        }
        if (FAILED(wic_->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return out;
        dc_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
        return out;
    }
    // Palette colours made vivid enough to glow (Apple's flowing backgrounds are luminous): brightness lifted to at least
    // 0.62 and saturation raised by a quarter; greys stay grey.
    // Alpha carries each colour's share of the artwork (used by the silk; 1 when unknown).
    static std::vector<D2D1_COLOR_F> ToColors(const std::vector<uint32_t>& rgb, const std::vector<float>& shares = {}) {
        std::vector<D2D1_COLOR_F> out;
        for (uint32_t c : rgb) {
            float r = ((c >> 16) & 0xFF) / 255.f, g = ((c >> 8) & 0xFF) / 255.f, b = (c & 0xFF) / 255.f;
            const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
            // Saturation from the colour's real chroma relative to its new brightness: a near-black with a faint blue cast
            // stays a dark grey instead of becoming a saturated purple.
            const float v = std::max(mx, .1f), sat = std::min(1.f, (mx - mn) * 1.25f / v);  // black stays (nearly) black
            auto channel = [&](float x) { const float rel = mx > mn ? (x - mn) / (mx - mn) : 0.f; return v * (1 - sat) + v * sat * rel; };
            out.push_back(D2D1::ColorF(channel(r), channel(g), channel(b), out.size() < shares.size() ? shares[out.size()] : 1.f));
        }
        return out;
    }
    std::vector<D2D1_COLOR_F> PaletteOfFile(const std::wstring& path) {
        ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> conv;
        if (!wic_ || FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &frame)) ||
            FAILED(wic_->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), 64, 64, WICBitmapInterpolationModeFant)) ||
            FAILED(wic_->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
        std::vector<uint8_t> px(64 * 64 * 4);
        if (FAILED(conv->CopyPixels(nullptr, 64 * 4, UINT(px.size()), px.data()))) return {};
        std::vector<float> shares; auto rgb = match::Palette(px.data(), 64, 64, 64 * 4, 6, &shares);
        return ToColors(rgb, shares);
    }
    // Palette of a bitmap on the GPU (motion artwork frames): drawn down to 64 x 64, read back. Call outside BeginDraw.
    std::vector<D2D1_COLOR_F> PaletteOfBitmap(ID2D1Bitmap1* src) {
        if (!src) return {};
        const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> target, read;
        if (FAILED(dc_->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt), &target)) ||
            FAILED(dc_->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt), &read))) return {};
        ComPtr<ID2D1Image> old; dc_->GetTarget(&old);
        D2D1_MATRIX_3X2_F oldTransform; dc_->GetTransform(&oldTransform);
        dc_->SetTarget(target.Get()); dc_->BeginDraw(); dc_->SetTransform(D2D1::IdentityMatrix()); dc_->Clear(D2D1::ColorF(0, 0, 0, 1));
        dc_->DrawBitmap(src, D2D1::RectF(0, 0, 64, 64), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        const HRESULT hr = dc_->EndDraw();
        dc_->SetTarget(old.Get()); dc_->SetTransform(oldTransform);
        const D2D1_POINT_2U origin{0, 0}; const D2D1_RECT_U all{0, 0, 64, 64};
        D2D1_MAPPED_RECT m{};
        if (FAILED(hr) || FAILED(read->CopyFromBitmap(&origin, target.Get(), &all)) || FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &m))) return {};
        std::vector<uint8_t> px(64 * 64 * 4);
        for (UINT y = 0; y < 64; ++y) std::memcpy(px.data() + y * 256, m.bits + size_t(y) * m.pitch, 256);
        read->Unmap();
        return PaletteOfPixels(px, 64, 64);
    }
    static std::vector<D2D1_COLOR_F> PaletteOfPixels(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) { std::vector<float> shares; auto rgb = match::Palette(px.data(), w, h, w * 4, 6, &shares); return ToColors(rgb, shares); }
    ComPtr<ID2D1Bitmap1> LoadPixels(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
        ComPtr<ID2D1Bitmap1> out;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc_->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, &out);
        return out;
    }

    struct Ui {
        const Media* media = nullptr;
        bool extend = true, clock = true, wallpaper = false;
        D2D1_RECT_F safe{0, 0, 0, 0};    // where the sharp artwork may go (empty: the whole screen); the extension fills the rest
        float controls = 1, appear = 1;  // 0..1
        float volume = 0; bool volumeOk = false, muted = false;
        int hover = 0, pressed = 0;      // HitId
        float seekPreview = -1;          // 0..1 while dragging the progress bar
        // Lyrics view (Apple Music style: artwork and controls left, lyrics right)
        const lyrics::Lyrics* lyrics = nullptr;
        bool lyricsView = false, lyricsAvailable = false;
        float lyricsPos = 0;             // smoothed index of the current line (fractional while scrolling)
        int lyricsActive = -1;           // line being sung
        int64_t lyricTimeMs = 0;
        float time = 0;                  // seconds, drives the slow background drift
        bool lyricsFullArt = true;       // lyrics over the borderless artwork (it moves left) instead of the card layout
        float lyricsBlend = 0;           // 0..1 transition into the full-artwork lyrics layout
        float lyricsManual = 0;          // 0..1 while the user scrolls the lyrics: no distance blur, lines evenly dimmed
        float beat = 0;                  // 0..1 beat pulse (audio loopback)
        float flowTime = 0;              // flowing background clock (runs faster with louder music)
        ID2D1Bitmap1* silk = nullptr;    // liquid silk for the lyrics side (shaders/silk.hlsl), when available
        std::wstring toast;              // short note at the top (lyrics timing)
    };
    enum HitId { None = 0, Prev, Play, Next, Progress, VolumeBar, Close, LyricsButton };

    // Hit test in window pixels against the last laid-out controls.
    int Hit(float x, float y) const {
        auto in = [&](const D2D1_RECT_F& r) { return r.right > r.left + 20 * scale_ && x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
        if (in(closeR_)) return Close;
        if (in(lyricsBtnR_)) return LyricsButton;
        if (in(prevR_)) return Prev; if (in(playR_)) return Play; if (in(nextR_)) return Next;
        if (in(Grow(progR_, 10 * scale_))) return Progress;
        if (in(Grow(volR_, 10 * scale_))) return VolumeBar;
        return None;
    }
    bool InPanel(float x, float y) const { return x >= panel_.left && x < panel_.right && y >= panel_.top && y < panel_.bottom; }
    float ProgressAt(float x) const { return Clamp01((x - progR_.left) / std::max(1.f, progR_.right - progR_.left)); }
    const std::vector<std::pair<int, D2D1_RECT_F>>& LyricRects() const { return lyricRects_; }
    D2D1_RECT_F LyricsColumnRect() const { return lyricsColumn_; }
    bool InLyrics(float x, float y) const { return lyricsColumn_.right > lyricsColumn_.left && x >= lyricsColumn_.left && x < lyricsColumn_.right && y >= lyricsColumn_.top && y < lyricsColumn_.bottom; }
    float LyricsStep() const { return std::max(20.f, lyricsStep_); }
    // Lyric line under the point (lyrics view), -1 when none.
    int LyricAt(float x, float y) const {
        for (const auto& [index, r] : lyricRects_) if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return index;
        return -1;
    }
    float VolumeAt(float x) const { return Clamp01((x - volR_.left) / std::max(1.f, volR_.right - volR_.left)); }

    // origin: where this screen-sized scene starts in the target (wallpaper: one scene per monitor).
    void Draw(float W, float H, const Art* prev, const Art* cur, float mix, const Ui& ui, D2D1_POINT_2F origin = {0, 0}) {
        W_ = W; H_ = H; SetClockSize(H);
        safe_ = ui.safe.right > ui.safe.left + 64 && ui.safe.bottom > ui.safe.top + 64 ? ui.safe : D2D1::RectF(0, 0, W, H);
        base_ = D2D1::Matrix3x2F::Translation(origin.x, origin.y);
        dc_->SetTransform(base_);
        if (ui.wallpaper) dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
        else dc_->Clear(D2D1::ColorF(0, 0, 0, ui.appear < 1 ? 0.f : 1.f));
        const bool fadeAll = ui.appear < 1;
        if (fadeAll) dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), EaseOut(ui.appear)), nullptr);
        const Art* top = cur && cur->Image() ? cur : nullptr;
        const Art* under = prev && prev->Image() && mix < 1 ? prev : nullptr;
        lyricRects_.clear(); lyricsColumn_ = {};
        fullSafe_ = safe_;
        if (ui.lyricsView && !ui.lyricsFullArt) {  // card layout: blurred backdrop, artwork card left, lyrics right
            if (!top && !ui.wallpaper) DrawEmpty();
            flowTime_ = ui.flowTime; beat_ = ui.beat;
            if (under) DrawBackdrop(*under, 1, top ? 1.f : 1 - EaseInOut(mix), ui.time);
            if (top) DrawBackdrop(*top, 0, under ? EaseInOut(mix) : 1.f, ui.time);
            if (ui.silk) DrawSilk(ui.silk, 1.f, .40f);  // the liquid flow stays on the lyrics side
            const D2D1_RECT_F art = LyricsArtRect();
            if (under) DrawCard(*under, art, top ? 1.f : 1 - EaseInOut(mix));
            if (top) DrawCard(*top, art, under ? EaseInOut(mix) : 1.f);
            const float left = W_ * .52f;
            DrawLyrics(ui, left, std::min(W_ * .40f, W_ - left - 48 * scale_), 1.f);
            if (!ui.wallpaper) DrawControls(ui, top ? 0 : under ? 1 : -1);
            else DrawCaption(ui, art);
            if (fadeAll) dc_->PopLayer();
            if (ui.wallpaper) dc_->PopAxisAlignedClip();
            return;
        }
        // Borderless artwork; with lyrics it moves into the left part of the screen and the lyrics take the right,
        // over the extension, which keeps filling everything.
        const float blend = EaseInOut(ui.lyrics && !ui.lyrics->Empty() ? ui.lyricsBlend : 0.f);
        if (blend > 0) safe_.right = safe_.right + (safe_.left + (safe_.right - safe_.left) * .56f - safe_.right) * blend;
        flowTime_ = ui.flowTime; beat_ = ui.beat;
        if (!top && !ui.wallpaper) DrawEmpty();
        const float flowInArt = ui.silk ? 0.f : blend;  // without the shader: the older colour blobs
        artRight_ = 0;
        if (under) DrawArt(*under, 1, top ? 1.f : 1 - EaseInOut(mix), ui.extend, flowInArt);
        if (top) DrawArt(*top, 0, under ? EaseInOut(mix) : 1.f, ui.extend, flowInArt);
        // The silk and the veil begin where the sharp artwork ends (its own feathered edge), never over it.
        const float sideStart = std::clamp(artRight_ > 0 ? artRight_ - W_ * .05f : W_ * .44f, W_ * .30f, W_ * .56f);  // overlaps the artwork's own feathered edge, so there is no seam
        if (ui.silk && blend > 0) DrawSilk(ui.silk, blend, sideStart / W_);
        if (blend > 0) {
            ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> g;
            const D2D1_GRADIENT_STOP st[] = {{0, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, .34f * blend)}};
            dc_->CreateGradientStopCollection(st, 2, &stops);
            dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({sideStart, 0}, {sideStart + W_ * .2f, 0}), stops.Get(), &g);
            dc_->FillRectangle(D2D1::RectF(sideStart, 0, W_, H_), g.Get());
            const float left = fullSafe_.left + (fullSafe_.right - fullSafe_.left) * .58f + 24 * scale_;
            DrawLyrics(ui, left, std::min(W_ * .40f, fullSafe_.right - left - 48 * scale_), blend);
        }
        if (!ui.wallpaper) {
            DrawScrims(ui);
            if (ui.clock && blend <= 0) DrawClock(ui);
            DrawControls(ui, top ? 0 : under ? 1 : -1);
            if (!ui.toast.empty()) {
                const D2D1_RECT_F r{W_ / 2 - 150 * scale_, 28 * scale_, W_ / 2 + 150 * scale_, 64 * scale_};
                brush_->SetColor(D2D1::ColorF(0, 0, 0, .45f)); dc_->FillRoundedRectangle(D2D1::RoundedRect(r, 18 * scale_, 18 * scale_), brush_.Get());
                Text(ui.toast, pill_.Get(), r, D2D1::ColorF(1, 1, 1, .95f));
            }
        }
        if (fadeAll) dc_->PopLayer();
        if (ui.wallpaper) dc_->PopAxisAlignedClip();
    }

private:
    struct Chain { ComPtr<ID2D1Effect> border, affine, blur, saturate, crop; };
    static D2D1_RECT_F Grow(D2D1_RECT_F r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }

    // Where the sharp artwork goes: fit inside the safe area (extend) or cover it (fill).
    D2D1_RECT_F ArtRect(D2D1_SIZE_F size, bool extend) const {
        const float sw = safe_.right - safe_.left, sh = safe_.bottom - safe_.top;
        const float s = extend ? std::min(sw / size.width, sh / size.height) : std::max(sw / size.width, sh / size.height);
        const float w = size.width * s, h = size.height * s, cx = (safe_.left + safe_.right) / 2, cy = (safe_.top + safe_.bottom) / 2;
        return {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    }

    // Background extension: artwork mirrored past its edges, scaled down 1/8, blurred, saturated.
    ID2D1Image* Extension(int slot, ID2D1Bitmap1* image, D2D1_RECT_F rect, float sigma = 9.f) {
        auto& c = chains_[slot];
        const auto size = image->GetPixelSize();
        const float k = 1.f / kDown;
        c.border->SetInput(0, image);
        c.affine->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                           D2D1::Matrix3x2F::Scale((rect.right - rect.left) / float(size.width) * k, (rect.bottom - rect.top) / float(size.height) * k) *
                               D2D1::Matrix3x2F::Translation(rect.left * k, rect.top * k));
        c.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
        c.crop->SetValue(D2D1_CROP_PROP_RECT, D2D1::Vector4F(-2, -2, std::ceil(W_ * k) + 2, std::ceil(H_ * k) + 2));  // margin: no dark rim when scaled up
        ComPtr<ID2D1Image> out; c.crop->GetOutput(&out);
        return out.Get();  // owned by the effect
    }
    void DrawBlurred(ID2D1Image* img) {
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(kDown, kDown) * base_);
        dc_->DrawImage(img, D2D1_INTERPOLATION_MODE_LINEAR);
        dc_->SetTransform(base_);
    }

    void DrawArt(const Art& art, int slot, float opacity, bool extend, float flow = 0) {
        ID2D1Bitmap1* image = art.Image();
        const auto ps = image->GetPixelSize();
        const D2D1_SIZE_F size{float(ps.width), float(ps.height)};
        const D2D1_RECT_F fit = ArtRect(size, true), rect = ArtRect(size, extend);
        if (opacity < 1) dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), opacity), nullptr);
        ID2D1Image* ext = Extension(slot, image, fit);
        DrawBlurred(ext);
        lastBlur_[slot] = ext;
        if (flow > 0 && !art.palette.empty()) DrawFlow(art, flow);  // with lyrics: the colours flow around the artwork
        // The sharp layer stays inside the safe area; every edge that does not reach the screen edge fades into
        // the extension (both axes when the artwork is kept clear of the Dock and menu bar).
        const D2D1_RECT_F clip{std::max(rect.left, safe_.left), std::max(rect.top, safe_.top), std::min(rect.right, safe_.right), std::min(rect.bottom, safe_.bottom)};
        const bool gapX = clip.left > 1 || clip.right < W_ - 1, gapY = clip.top > 1 || clip.bottom < H_ - 1;
        if (slot == 0 || artRight_ <= 0) artRight_ = clip.right;  // the lyrics side (silk, veil) starts here
        if (gapX) {
            featherBrush_->SetStartPoint(D2D1::Point2F(clip.left, 0)); featherBrush_->SetEndPoint(D2D1::Point2F(clip.right, 0));
            dc_->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1, featherBrush_.Get()), nullptr);
        }
        if (gapY) {
            featherBrushV_->SetStartPoint(D2D1::Point2F(0, clip.top)); featherBrushV_->SetEndPoint(D2D1::Point2F(0, clip.bottom));
            dc_->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1, featherBrushV_.Get()), nullptr);
        }
        if (!gapX && !gapY) dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->DrawBitmap(image, rect, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        if (!gapX && !gapY) dc_->PopAxisAlignedClip();
        if (gapY) dc_->PopLayer();
        if (gapX) dc_->PopLayer();
        if (opacity < 1) dc_->PopLayer();
    }

    // ---- lyrics view ----
    D2D1_RECT_F LyricsArtRect() const {
        const float sh = fullSafe_.bottom - fullSafe_.top;
        const float a = std::min(W_ * .30f, sh * .50f), cx = W_ * .27f, blockH = a + 210 * scale_;
        const float top = fullSafe_.top + std::max(48 * scale_, (sh - blockH) / 2);
        return {cx - a / 2, top, cx + a / 2, top + a};
    }
    // Wallpaper, card layout: title and artist under the card (no controls on the desktop).
    void DrawCaption(const Ui& ui, D2D1_RECT_F art) {
        const Media& m = *ui.media; const float s = scale_;
        const std::wstring sub = m.album.empty() ? m.artist : m.artist.empty() ? m.album : m.artist + L" \u2014 " + m.album;
        Text(m.title, title_.Get(), D2D1::RectF(art.left, art.bottom + 18 * s, art.right, art.bottom + 46 * s), D2D1::ColorF(1, 1, 1, .96f));
        Text(sub, artist_.Get(), D2D1::RectF(art.left, art.bottom + 46 * s, art.right, art.bottom + 70 * s), D2D1::ColorF(1, 1, 1, .62f));
    }
    // Liquid silk on the lyrics side: from startFraction of the width to the right edge, fading in over 16 % of the
    // width so the artwork side flows into it, with a light veil that keeps white lyrics readable.
    void DrawSilk(ID2D1Bitmap1* silk, float opacity, float startFraction) {
        const D2D1_RECT_F area{W_ * startFraction, 0, W_, H_};
        // Wide fade (a quarter of the screen) so the silk melts into the artwork side.
        silkFade_->SetStartPoint(D2D1::Point2F(area.left, 0)); silkFade_->SetEndPoint(D2D1::Point2F(area.left + W_ * .25f, 0));
        dc_->PushLayer(D2D1::LayerParameters1(area, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), opacity, silkFade_.Get()), nullptr);
        // Soft focus: clamp the edges, blur, crop back to the texture.
        const auto px = silk->GetPixelSize();
        silkBorder_->SetInput(0, silk);
        silkBlur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 4.5f);
        silkCrop_->SetValue(D2D1_CROP_PROP_RECT, D2D1::Vector4F(0, 0, float(px.width), float(px.height)));
        ComPtr<ID2D1Image> soft; silkCrop_->GetOutput(&soft);
        dc_->SetTransform(D2D1::Matrix3x2F::Scale((area.right - area.left) / float(px.width), (area.bottom - area.top) / float(px.height)) *
                          D2D1::Matrix3x2F::Translation(area.left, area.top) * base_);
        dc_->DrawImage(soft.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
        dc_->SetTransform(base_);
        brush_->SetColor(D2D1::ColorF(0, 0, 0, .3f)); dc_->FillRectangle(area, brush_.Get());
        dc_->PopLayer();
    }
    // Flowing background (Apple Music's lyrics background): the cover's dominant colours as large soft blobs that drift
    // on slow Lissajous paths. The clock runs faster with louder music and each beat swells the blobs a little.
    void DrawFlow(const Art& art, float opacity) {
        const auto& pal = art.palette;
        if (pal.empty() || opacity <= 0) return;
        if (opacity < 1) dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), opacity), nullptr);
        auto mixColor = [](D2D1_COLOR_F a, float k) { return D2D1::ColorF(a.r * k, a.g * k, a.b * k, 1); };
        D2D1_COLOR_F base{0, 0, 0, 1};
        for (const auto& c : pal) { base.r += c.r / pal.size(); base.g += c.g / pal.size(); base.b += c.b / pal.size(); }
        brush_->SetColor(mixColor(base, .7f)); dc_->FillRectangle(D2D1::RectF(0, 0, W_, H_), brush_.Get());
        static const float speedX[] = {.21f, .17f, .13f, .19f, .11f, .15f}, speedY[] = {.16f, .23f, .18f, .12f, .2f, .14f};
        static const float phaseX[] = {0, 2.1f, 4.2f, 1.3f, 3.4f, 5.5f}, phaseY[] = {1.f, 3.7f, .4f, 2.6f, 5.1f, 4.4f};
        const float t = flowTime_, big = std::max(W_, H_);
        for (int i = 0; i < 6; ++i) {
            const D2D1_COLOR_F c = pal[size_t(i) % pal.size()];
            const float cx = W_ * (.5f + .42f * std::sin(t * speedX[i] + phaseX[i]));
            const float cy = H_ * (.5f + .40f * std::cos(t * speedY[i] + phaseY[i]));
            const float r = big * (.34f + .07f * std::sin(t * .29f + i * 1.7f)) * (1 + .10f * beat_);
            const D2D1_GRADIENT_STOP st[] = {{0, D2D1::ColorF(c.r, c.g, c.b, .9f)}, {.5f, D2D1::ColorF(c.r, c.g, c.b, .5f)}, {1, D2D1::ColorF(c.r, c.g, c.b, 0)}};
            ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1RadialGradientBrush> g;
            if (FAILED(dc_->CreateGradientStopCollection(st, 3, &stops)) ||
                FAILED(dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), {}, r, r * .82f), stops.Get(), &g))) continue;
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r * .82f), g.Get());
        }
        brush_->SetColor(D2D1::ColorF(0, 0, 0, .16f)); dc_->FillRectangle(D2D1::RectF(0, 0, W_, H_), brush_.Get());  // keeps white lyrics readable
        if (opacity < 1) dc_->PopLayer();
    }
    // Whole-screen backdrop: the artwork enlarged, blurred hard and drifting slowly (Apple Music's moving colours).
    void DrawBackdrop(const Art& art, int slot, float opacity, float t) {
        ID2D1Bitmap1* image = art.Image();
        const auto ps = image->GetPixelSize();
        const float sc = std::max(W_ / float(ps.width), H_ / float(ps.height)) * 1.4f, w = ps.width * sc, h = ps.height * sc;
        const float ox = std::sin(t * .061f) * W_ * .06f, oy = std::cos(t * .047f) * H_ * .06f;
        const D2D1_RECT_F r{(W_ - w) / 2 + ox, (H_ - h) / 2 + oy, (W_ + w) / 2 + ox, (H_ + h) / 2 + oy};
        if (opacity < 1) dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), opacity), nullptr);
        ID2D1Image* ext = Extension(slot, image, r, 16.f);
        DrawBlurred(ext); lastBlur_[slot] = ext;
        brush_->SetColor(D2D1::ColorF(0, 0, 0, .30f)); dc_->FillRectangle(D2D1::RectF(0, 0, W_, H_), brush_.Get());
        if (opacity < 1) dc_->PopLayer();
    }
    // The artwork as a card: rounded corners and a soft shadow.
    void DrawCard(const Art& art, D2D1_RECT_F box, float opacity) {
        ID2D1Bitmap1* image = art.Image();
        const auto ps = image->GetPixelSize();
        const float bw = box.right - box.left, bh = box.bottom - box.top, k = std::min(bw / ps.width, bh / ps.height);
        const float w = ps.width * k, h = ps.height * k, cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
        const D2D1_RECT_F r{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
        const float radius = 14 * scale_;
        for (int i = 1; i <= 6; ++i) {  // shadow
            const float g = i * 5 * scale_;
            brush_->SetColor(D2D1::ColorF(0, 0, 0, .045f * opacity));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - g * .6f, r.top - g * .2f, r.right + g * .6f, r.bottom + g), radius + g, radius + g), brush_.Get());
        }
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &shape);
        dc_->PushLayer(D2D1::LayerParameters1(r, shape.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity), nullptr);
        dc_->DrawBitmap(image, r, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        dc_->PopLayer();
    }
    struct WordLayout {
        std::vector<DWRITE_HIT_TEST_METRICS> boxes;
        ComPtr<ID2D1CommandList> glow;
        float width = 0;
    };
    struct LyricLayout {
        ComPtr<IDWriteTextLayout> main;
        ComPtr<ID2D1CommandList> list;
        std::vector<WordLayout> words;
        float height = 0;
    };
    struct BlurFx { ComPtr<ID2D1Effect> blur, opacity; };
    // A line as an image (recorded once) so it can be blurred.
    ID2D1CommandList* LineImage(LyricLayout& l) {
        if (l.list || !l.main) return l.list.Get();
        ComPtr<ID2D1CommandList> list; if (FAILED(dc_->CreateCommandList(&list))) return nullptr;
        ComPtr<ID2D1Image> previous; dc_->GetTarget(&previous);
        D2D1_MATRIX_3X2_F transform; dc_->GetTransform(&transform);
        dc_->SetTarget(list.Get()); dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc_->DrawTextLayout(D2D1::Point2F(0, 0), l.main.Get(), brush_.Get());
        dc_->SetTarget(previous.Get()); dc_->SetTransform(transform);
        if (FAILED(list->Close())) return nullptr;
        l.list = list; return l.list.Get();
    }
    BlurFx* Fx(size_t i) {
        while (blurPool_.size() <= i) {
            BlurFx fx;
            if (FAILED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &fx.blur)) || FAILED(dc_->CreateEffect(CLSID_D2D1Opacity, &fx.opacity))) return nullptr;
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            fx.opacity->SetInputEffect(0, fx.blur.Get());
            blurPool_.push_back(fx);
        }
        return &blurPool_[i];
    }
    void BuildLyricLayouts(const lyrics::Lyrics& l, float width) {
        const float font = std::clamp(H_ * .043f, 26 * scale_, 60 * scale_);
        if (lyricsKey_ == &l && lyricsCount_ == l.lines.size() && std::abs(lyricsWidth_ - width) < 1 && std::abs(lyricsFont_ - font) < .5f) return;
        lyricsKey_ = &l; lyricsCount_ = l.lines.size(); lyricsWidth_ = width; lyricsFont_ = font;
        ComPtr<IDWriteTextFormat> mainFmt;
        dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font, L"", &mainFmt);
        layouts_.clear();
        for (const auto& line : l.lines) {
            LyricLayout out;
            const std::wstring text = line.text.empty() ? std::wstring(L"\u2022  \u2022  \u2022") : line.text;
            dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), mainFmt.Get(), width, 4000, &out.main);
            DWRITE_TEXT_METRICS m{}; if (out.main) out.main->GetMetrics(&m);
            out.height = m.height;
            if (out.main) {
                for (const auto& word : line.words) {
                    WordLayout wordLayout;
                    UINT32 count = 0;
                    const auto start = static_cast<UINT32>(word.offset), len = static_cast<UINT32>(word.length);
                    out.main->HitTestTextRange(start, len, 0, 0, nullptr, 0, &count);
                    wordLayout.boxes.resize(count);
                    if (count && SUCCEEDED(out.main->HitTestTextRange(start, len, 0, 0, wordLayout.boxes.data(), count, &count))) {
                        wordLayout.boxes.resize(count);
                        for (const auto& box : wordLayout.boxes) wordLayout.width += box.width;
                    } else wordLayout.boxes.clear();
                    out.words.push_back(std::move(wordLayout));
                }
            }
            layouts_.push_back(out);
        }
    }
    static bool AddWordClip(ID2D1GeometrySink& sink, const WordLayout& word, float part) {
        float remaining = word.width * part;
        bool filled = false;
        for (const auto& box : word.boxes) {
            const float width = std::min(box.width, remaining);
            if (width <= 0) break;
            const bool rtl = (box.bidiLevel & 1) != 0;
            const float left = box.left + (rtl ? box.width - width : 0);
            const auto rect = D2D1::RectF(left, box.top, left + width, box.top + box.height);
            sink.BeginFigure({rect.left, rect.top}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F corners[] = {{rect.right, rect.top}, {rect.right, rect.bottom}, {rect.left, rect.bottom}};
            sink.AddLines(corners, ARRAYSIZE(corners));
            sink.EndFigure(D2D1_FIGURE_END_CLOSED);
            remaining -= width;
            filled = true;
        }
        return filled;
    }

    ID2D1CommandList* GlowWordImage(const LyricLayout& layout, WordLayout& word) {
        if (word.glow || word.boxes.empty()) return word.glow.Get();
        if (!lyricGlowContext_) {
            ComPtr<ID2D1Device> device;
            dc_->GetDevice(&device);
            if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &lyricGlowContext_))) return nullptr;
        }
        ComPtr<ID2D1PathGeometry> clip;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(&clip)) || FAILED(clip->Open(&sink))) return nullptr;
        const bool filled = AddWordClip(*sink.Get(), word, 1);
        if (FAILED(sink->Close()) || !filled) return nullptr;

        ComPtr<ID2D1CommandList> image;
        ComPtr<ID2D1SolidColorBrush> white;
        auto* glow = lyricGlowContext_.Get();
        if (FAILED(glow->CreateCommandList(&image)) || FAILED(glow->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &white))) return nullptr;
        glow->SetTarget(image.Get());
        glow->BeginDraw();
        const auto bounds = D2D1::RectF(0, 0, layout.main->GetMaxWidth(), layout.height);
        glow->PushLayer(D2D1::LayerParameters1(bounds, clip.Get()), nullptr);
        glow->DrawTextLayout({0, 0}, layout.main.Get(), white.Get());
        glow->PopLayer();
        const HRESULT drawn = glow->EndDraw();
        glow->SetTarget(nullptr);
        if (FAILED(drawn) || FAILED(image->Close())) return nullptr;
        word.glow = std::move(image);
        return word.glow.Get();
    }

    void DrawSustainedGlow(ID2D1Image* image, float strength, float alpha) {
        if (!lyricGlowFx_.blur) {
            BlurFx fx;
            if (FAILED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &fx.blur)) ||
                FAILED(dc_->CreateEffect(CLSID_D2D1Opacity, &fx.opacity))) return;
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
            fx.opacity->SetInputEffect(0, fx.blur.Get());
            lyricGlowFx_ = std::move(fx);
        }
        lyricGlowFx_.blur->SetInput(0, image);
        // Diffuse light expands with the sustained vowel. Keep a smaller core close to the strokes.
        lyricGlowFx_.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, lyricsFont_ * (.09f + .13f * strength));
        lyricGlowFx_.opacity->SetValue(D2D1_OPACITY_PROP_OPACITY, alpha * strength * .72f);
        dc_->DrawImage(lyricGlowFx_.opacity.Get());
        lyricGlowFx_.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, lyricsFont_ * .052f);
        lyricGlowFx_.opacity->SetValue(D2D1_OPACITY_PROP_OPACITY, alpha * strength * .50f);
        dc_->DrawImage(lyricGlowFx_.opacity.Get());
    }

    void DrawWordSweep(const LyricLayout& layout, const WordLayout& word, float progress, float alpha) {
        const float feather = std::min(word.width * .6f, lyricsFont_ * .65f);
        if (feather <= 0) return;
        float edge = progress * (word.width + feather);
        wordSweep_->SetOpacity(alpha);
        for (const auto& box : word.boxes) {
            if (edge <= 0) break;
            const bool rtl = (box.bidiLevel & 1) != 0;
            const float front = rtl ? box.left + box.width - edge : box.left + edge;
            wordSweep_->SetStartPoint({front, 0});
            wordSweep_->SetEndPoint({front + (rtl ? feather : -feather), 0});
            dc_->PushAxisAlignedClip(D2D1::RectF(box.left, box.top, box.left + box.width, box.top + box.height),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
            dc_->DrawTextLayout({0, 0}, layout.main.Get(), wordSweep_.Get());
            dc_->PopAxisAlignedClip();
            edge -= box.width;
        }
    }

    void DrawStationaryWords(LyricLayout& layout, const lyrics::Line& line, int64_t time, float alpha) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * .42f));
        dc_->DrawTextLayout({0, 0}, layout.main.Get(), brush_.Get());
        if (time <= line.words.front().start) return;
        ComPtr<ID2D1PathGeometry> clip;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(&clip)) || FAILED(clip->Open(&sink))) return;
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        bool filled = false;
        for (size_t i = 0; i < layout.words.size(); ++i) {
            const float progress = lyrics::WordProgress(line.words[i], time);
            if (wordSweep_ && progress > 0 && progress < 1)
                DrawWordSweep(layout, layout.words[i], progress, alpha);
            else if (progress > 0 && AddWordClip(*sink.Get(), layout.words[i], progress)) filled = true;
        }
        if (FAILED(sink->Close()) || !filled) return;
        const auto bounds = D2D1::RectF(0, 0, layout.main->GetMaxWidth(), layout.height);
        dc_->PushLayer(D2D1::LayerParameters1(bounds, clip.Get()), nullptr);
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha));
        dc_->DrawTextLayout({0, 0}, layout.main.Get(), brush_.Get());
        dc_->PopLayer();
    }

    void DrawWordLyrics(LyricLayout& layout, const lyrics::Line& line, int64_t time, float alpha) {
        D2D1::Matrix3x2F original;
        dc_->GetTransform(&original);
        struct Accent { size_t index; D2D1::Matrix3x2F transform; };
        std::vector<Accent> accents;
        for (size_t i = 0; i < layout.words.size(); ++i) {
            const float strength = lyrics::SustainedWordGlow(line.words[i], time);
            if (strength < .005f) continue;
            auto& word = layout.words[i];
            auto* image = GlowWordImage(layout, word);
            if (!image) continue;
            const auto& box = word.boxes.front();
            const float grow = 1.f + .038f * strength;
            // Wrapped words share a baseline shift. Scale single-box words about their own centre.
            const auto scale = word.boxes.size() == 1 ? grow : 1.f;
            const auto transform = D2D1::Matrix3x2F::Scale(scale, scale,
                {box.left + box.width * .5f, box.top + box.height * .78f}) *
                D2D1::Matrix3x2F::Translation(0, -lyricsFont_ * .094f * strength) * original;
            dc_->SetTransform(transform);
            DrawSustainedGlow(image, strength, alpha);
            accents.push_back({i, transform});
        }
        dc_->SetTransform(original);
        if (accents.empty()) { DrawStationaryWords(layout, line, time, alpha); return; }
        if (!silentWord_) dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0), &silentWord_);
        if (!silentWord_) { DrawStationaryWords(layout, line, time, alpha); return; }
        auto range = [&](size_t i) {
            return DWRITE_TEXT_RANGE{UINT32(line.words[i].offset), UINT32(line.words[i].length)};
        };
        for (const auto& a : accents) layout.main->SetDrawingEffect(silentWord_.Get(), range(a.index));
        DrawStationaryWords(layout, line, time, alpha);
        for (const auto& a : accents) layout.main->SetDrawingEffect(nullptr, range(a.index));
        for (const auto& a : accents) {
            dc_->SetTransform(a.transform);
            const auto& word = layout.words[a.index];
            const float progress = lyrics::WordProgress(line.words[a.index], time);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * (progress >= 1 ? 1.f : .42f)));
            for (const auto& box : word.boxes) {
                dc_->PushAxisAlignedClip(D2D1::RectF(box.left, box.top, box.left + box.width, box.top + box.height),
                                         D2D1_ANTIALIAS_MODE_ALIASED);
                dc_->DrawTextLayout({0, 0}, layout.main.Get(), brush_.Get());
                dc_->PopAxisAlignedClip();
            }
            if (progress < 1 && wordSweep_) DrawWordSweep(layout, word, progress, alpha);
        }
        dc_->SetTransform(original);
    }

    void DrawLyrics(const Ui& ui, float left, float width, float fade) {
        if (!ui.lyrics || ui.lyrics->Empty() || width < 120) return;
        BuildLyricLayouts(*ui.lyrics, width);
        // Below the Lyrics/close buttons in the full-screen view.
        const float top = fullSafe_.top + (ui.wallpaper ? 0.f : 96 * scale_), bottom = fullSafe_.bottom, h = bottom - top;
        const float gap = lyricsFont_ * .62f, anchor = top + h * .38f;
        std::vector<float> ys(layouts_.size() + 1, 0.f);
        for (size_t i = 0; i < layouts_.size(); ++i) ys[i + 1] = ys[i] + layouts_[i].height + gap;
        const float pos = std::clamp(ui.lyricsPos, 0.f, float(std::max<size_t>(1, layouts_.size()) - 1));
        const size_t i0 = size_t(pos); const float f = pos - float(i0);
        const float scroll = ys[i0] + (i0 + 1 < ys.size() ? (ys[std::min(i0 + 1, layouts_.size() - 1)] - ys[i0]) * f : 0);
        lyricsColumn_ = {left - 12 * scale_, top, left + width, bottom};
        lyricsStep_ = layouts_.empty() ? 60.f : ys.back() / float(layouts_.size());
        float arrival = 1.f;
        if (ui.lyricsActive >= 0 && size_t(ui.lyricsActive) < ui.lyrics->lines.size()) {
            const float t = Clamp01(float(ui.lyricTimeMs - ui.lyrics->lines[ui.lyricsActive].ms) / 220.f);
            arrival = t * t * (3.f - 2.f * t);
        }
        size_t fxUsed = 0;
        for (size_t i = 0; i < layouts_.size(); ++i) {
            const float y = anchor + ys[i] - scroll;
            if (y > bottom + 20 || y + layouts_[i].height < top - 20) continue;
            const bool active = int(i) == ui.lyricsActive;
            // Fade toward the edges of the area; the line being sung is bright, the others dimmed - and blurred more
            // the farther they are from it (not while the user scrolls).
            const float edge = std::min(Clamp01((y + layouts_[i].height - top) / (h * .22f)), Clamp01((bottom - y) / (h * .30f)));
            const float dim = ui.lyrics->synced ? .34f : .72f;  // the sung line stays white, the others grey (also while scrolling)
            const bool departing = int(i) + 1 == ui.lyricsActive && arrival < 1;
            const float focus = active ? arrival : (departing ? 1.f - arrival : 0.f);
            const float alpha = (dim + (1.f - dim) * focus) * edge * fade;
            const float sc = .965f + .035f * focus;
            const float distance = std::abs(float(i) - pos);  // follows the eased scroll: lines sharpen as they arrive
            const float sigma = ui.lyrics->synced ? std::min(distance * 1.7f, 9.f) * scale_ * (1 - ui.lyricsManual) : 0.f;
            const auto transform = D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(left, y) * base_;
            const bool wordTiming = (active || departing) && !layouts_[i].words.empty();
            ID2D1CommandList* image = !wordTiming && sigma > .35f ? LineImage(layouts_[i]) : nullptr;
            BlurFx* fx = image ? Fx(fxUsed) : nullptr;
            if (fx) {
                ++fxUsed;
                fx->blur->SetInput(0, image);
                fx->blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
                fx->opacity->SetValue(D2D1_OPACITY_PROP_OPACITY, alpha);
                dc_->SetTransform(transform);
                dc_->DrawImage(fx->opacity.Get());
            } else if (layouts_[i].main) {
                dc_->SetTransform(transform);
                if (wordTiming) DrawWordLyrics(layouts_[i], ui.lyrics->lines[i], ui.lyricTimeMs, alpha);
                else {
                    brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha));
                    dc_->DrawTextLayout(D2D1::Point2F(0, 0), layouts_[i].main.Get(), brush_.Get());
                }
            }
            dc_->SetTransform(base_);
            if (ui.lyrics->synced && fade > .5f) lyricRects_.push_back({int(i), D2D1::RectF(left - 12 * scale_, y - gap / 2, left + width, y + layouts_[i].height + gap / 2)});
        }
    }

    void DrawEmpty() {
        ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> g;
        const D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(0.16f, 0.17f, 0.22f)}, {1, D2D1::ColorF(0.05f, 0.05f, 0.07f)}};
        dc_->CreateGradientStopCollection(s, 2, &stops);
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {W_ * .3f, H_}), stops.Get(), &g);
        dc_->FillRectangle(D2D1::RectF(0, 0, W_, H_), g.Get());
    }

    void VerticalScrim(float y0, float y1, float a0, float a1) {
        ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> g;
        const D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(0, 0, 0, a0)}, {1, D2D1::ColorF(0, 0, 0, a1)}};
        dc_->CreateGradientStopCollection(s, 2, &stops);
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y1}), stops.Get(), &g);
        dc_->FillRectangle(D2D1::RectF(0, std::min(y0, y1), W_, std::max(y0, y1)), g.Get());
    }
    void DrawScrims(const Ui& ui) {
        VerticalScrim(H_ * .52f, H_, 0, .16f + .30f * ui.controls);
        if (ui.clock) VerticalScrim(0, H_ * .32f, .26f, 0);
    }

    void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        if (!f || s.empty()) return;
        brush_->SetColor(c);
        dc_->DrawTextW(s.c_str(), UINT32(s.size()), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    // Text with a soft shadow, for lock-screen type over bright artwork.
    void ShadowText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, float alpha) {
        const float d = std::max(1.f, H_ / 900.f);
        Text(s, f, {r.left, r.top + d, r.right, r.bottom + d}, D2D1::ColorF(0, 0, 0, .22f * alpha));
        Text(s, f, r, D2D1::ColorF(1, 1, 1, .92f * alpha));
    }

    void DrawClock(const Ui& ui) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t fmt[80]{}, date[128]{}, time[32]{};
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, fmt, 80);
        const bool h24 = wcschr(fmt, L'H') != nullptr;
        swprintf_s(time, h24 ? L"%02d:%02d" : L"%d:%02d", h24 ? st.wHour : (st.wHour % 12 ? st.wHour % 12 : 12), st.wMinute);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd, MMMM d", date, 128, nullptr);
        const float top = H_ * .055f, a = 1.f; (void)ui;
        ShadowText(date, date_.Get(), D2D1::RectF(0, top, W_, top + H_ * .04f), a);
        ShadowText(time, clock_.Get(), D2D1::RectF(0, top + H_ * .02f, W_, top + H_ * .19f), a);
    }

    static std::wstring Clock(int64_t ticks) {
        const int64_t s = std::max<int64_t>(0, ticks / 10000000);
        wchar_t b[32];
        if (s >= 3600) swprintf_s(b, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60); else swprintf_s(b, L"%lld:%02lld", s / 60, s % 60);
        return b;
    }

    void Glyph(int id, D2D1_POINT_2F c, float size, float alpha, bool playing) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha));
        ComPtr<ID2D1PathGeometry> path; factory_->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink; path->Open(&sink);
        auto tri = [&](float x, float y, float w, float h, bool left) {
            sink->BeginFigure(D2D1::Point2F(left ? x + w : x, y - h / 2), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(left ? x : x + w, y)); sink->AddLine(D2D1::Point2F(left ? x + w : x, y + h / 2));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        };
        if (id == Play && playing) {
            const float w = size * .26f, h = size * .9f, gap = size * .2f;
            for (float x : {c.x - gap / 2 - w, c.x + gap / 2}) {
                sink->BeginFigure(D2D1::Point2F(x, c.y - h / 2), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x + w, c.y - h / 2)); sink->AddLine(D2D1::Point2F(x + w, c.y + h / 2)); sink->AddLine(D2D1::Point2F(x, c.y + h / 2));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
        } else if (id == Play) {
            tri(c.x - size * .36f, c.y, size * .82f, size * .92f, false);
        } else {
            const bool left = id == Prev; const float w = size * .5f, h = size * .62f;
            tri(c.x - w, c.y, w, h, left); tri(c.x, c.y, w, h, left);
        }
        sink->Close();
        ComPtr<ID2D1StrokeStyle> round;
        D2D1_STROKE_STYLE_PROPERTIES sp{}; sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        factory_->CreateStrokeStyle(sp, nullptr, 0, &round);
        dc_->FillGeometry(path.Get(), brush_.Get());
        dc_->DrawGeometry(path.Get(), brush_.Get(), size * .08f, round.Get());  // softened corners
    }

    void Speaker(D2D1_POINT_2F c, float s, int waves, float alpha) {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha));
        ComPtr<ID2D1PathGeometry> path; factory_->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink; path->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(c.x - s * .5f, c.y - s * .18f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(c.x - s * .25f, c.y - s * .18f)); sink->AddLine(D2D1::Point2F(c.x + s * .05f, c.y - s * .45f));
        sink->AddLine(D2D1::Point2F(c.x + s * .05f, c.y + s * .45f)); sink->AddLine(D2D1::Point2F(c.x - s * .25f, c.y + s * .18f));
        sink->AddLine(D2D1::Point2F(c.x - s * .5f, c.y + s * .18f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
        dc_->FillGeometry(path.Get(), brush_.Get());
        for (int i = 1; i <= waves; ++i) {
            const float r = s * (.18f + .2f * i);
            ComPtr<ID2D1PathGeometry> arc; factory_->CreatePathGeometry(&arc); ComPtr<ID2D1GeometrySink> as; arc->Open(&as);
            as->BeginFigure(D2D1::Point2F(c.x + s * .05f + r * .6f, c.y - r * .8f), D2D1_FIGURE_BEGIN_HOLLOW);
            as->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + s * .05f + r * .6f, c.y + r * .8f), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            as->EndFigure(D2D1_FIGURE_END_OPEN); as->Close();
            dc_->DrawGeometry(arc.Get(), brush_.Get(), s * .1f);
        }
    }

    void Bar(D2D1_RECT_F r, float value, float alpha, bool active) {
        const float h = active ? r.bottom - r.top : (r.bottom - r.top) * .62f, cy = (r.top + r.bottom) / 2;
        const D2D1_RECT_F track{r.left, cy - h / 2, r.right, cy + h / 2};
        brush_->SetColor(D2D1::ColorF(1, 1, 1, .26f * alpha));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(track, h / 2, h / 2), brush_.Get());
        if (value > 0) {
            dc_->PushAxisAlignedClip(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * Clamp01(value), track.bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, .9f * alpha));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(track, h / 2, h / 2), brush_.Get());
            dc_->PopAxisAlignedClip();
        }
    }

    // Glass transport platter: the blurred artwork behind it, darkened, with a hairline rim.
    void DrawControls(const Ui& ui, int blurSlot) {
        const float s = scale_, a = ui.controls;
        const bool side = ui.lyricsView && !ui.lyricsFullArt;  // card layout: under the card, no platter, title always shown
        const float pw = std::min(W_ - 48 * s, 620 * s), ph = 226 * s;
        if (side && !ui.lyricsFullArt) { const auto art = LyricsArtRect(); panel_ = {art.left - 26 * s, art.bottom + 4 * s, art.right + 26 * s, art.bottom + 4 * s + ph}; }
        else {
            // Under the artwork's column when the lyrics take the right side.
            const float cx = W_ / 2 + ((safe_.left + safe_.right) / 2 - W_ / 2);
            const float w = std::min(pw, std::max(360 * s, safe_.right - safe_.left - 48 * s));
            panel_ = {cx - w / 2, H_ - 52 * s - ph, cx + w / 2, H_ - 52 * s};
        }
        const float l = panel_.left + 26 * s, r = panel_.right - 26 * s, t = panel_.top;
        progR_ = {l, t + 92 * s, r, t + 100 * s};
        const float cy = t + 150 * s, cx = (panel_.left + panel_.right) / 2;
        playR_ = {cx - 30 * s, cy - 30 * s, cx + 30 * s, cy + 30 * s};
        prevR_ = {cx - 118 * s, cy - 26 * s, cx - 66 * s, cy + 26 * s};
        nextR_ = {cx + 66 * s, cy - 26 * s, cx + 118 * s, cy + 26 * s};
        volR_ = {l + 30 * s, t + 198 * s, r - 34 * s, t + 204 * s};
        closeR_ = {W_ - 64 * s, 24 * s, W_ - 24 * s, 64 * s};
        lyricsBtnR_ = ui.lyricsAvailable ? D2D1::RectF(W_ - 176 * s, 26 * s, W_ - 76 * s, 62 * s) : D2D1_RECT_F{};
        const Media& m = *ui.media;
        const std::wstring title = m.has && !m.title.empty() ? m.title : L"Not Playing";
        const std::wstring sub = !m.has ? L"" : m.album.empty() ? m.artist : m.artist.empty() ? m.album : m.artist + L" — " + m.album;
        if (side) {
            Text(title, title_.Get(), D2D1::RectF(l, t + 20 * s, r, t + 48 * s), D2D1::ColorF(1, 1, 1, .96f));
            Text(sub, artist_.Get(), D2D1::RectF(l, t + 48 * s, r, t + 72 * s), D2D1::ColorF(1, 1, 1, .62f));
        }
        if (a <= .002f) { closeR_ = prevR_ = playR_ = nextR_ = progR_ = volR_ = lyricsBtnR_ = {}; return; }

        if (!side) {
            ComPtr<ID2D1RoundedRectangleGeometry> shape;
            factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(panel_, 30 * s, 30 * s), &shape);
            dc_->PushLayer(D2D1::LayerParameters1(panel_, shape.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
            if (blurSlot >= 0 && lastBlur_[blurSlot]) DrawBlurred(lastBlur_[blurSlot]);
            else { brush_->SetColor(D2D1::ColorF(.2f, .2f, .24f, 1)); dc_->FillRectangle(panel_, brush_.Get()); }
            brush_->SetColor(D2D1::ColorF(0, 0, 0, .24f)); dc_->FillRectangle(panel_, brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, .07f)); dc_->FillRectangle(panel_, brush_.Get());
            dc_->PopLayer();
            brush_->SetColor(D2D1::ColorF(1, 1, 1, .20f * a));
            dc_->DrawRoundedRectangle(D2D1::RoundedRect(Grow(panel_, -.5f), 30 * s, 30 * s), brush_.Get(), 1);
            Text(title, title_.Get(), D2D1::RectF(l, t + 20 * s, r, t + 48 * s), D2D1::ColorF(1, 1, 1, .96f * a));
            Text(sub, artist_.Get(), D2D1::RectF(l, t + 48 * s, r, t + 72 * s), D2D1::ColorF(1, 1, 1, .62f * a));
        }

        const bool timeline = m.has && m.end > 0;
        const int64_t pos = ui.seekPreview >= 0 ? int64_t(ui.seekPreview * double(m.end)) : m.PositionNow();
        Bar(progR_, timeline ? float(double(pos) / double(m.end)) : 0, a * (timeline ? 1 : .45f), ui.hover == Progress || ui.pressed == Progress);
        if (timeline) {
            Text(Clock(pos), timeL_.Get(), D2D1::RectF(l, t + 106 * s, l + 120 * s, t + 122 * s), D2D1::ColorF(1, 1, 1, .55f * a));
            Text(L"-" + Clock(m.end - pos), timeR_.Get(), D2D1::RectF(r - 120 * s, t + 106 * s, r, t + 122 * s), D2D1::ColorF(1, 1, 1, .55f * a));
        }
        for (int id : {Prev, Play, Next}) {
            const D2D1_RECT_F& b = id == Prev ? prevR_ : id == Play ? playR_ : nextR_;
            const bool enabled = m.has && (id == Prev ? m.canPrev : id == Next ? m.canNext : m.canPlayPause);
            if (enabled && (ui.hover == id || ui.pressed == id)) {
                brush_->SetColor(D2D1::ColorF(1, 1, 1, (ui.pressed == id ? .2f : .11f) * a));
                const float rr = (b.right - b.left) / 2;
                dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), rr, rr), brush_.Get());
            }
            const float size = id == Play ? 30 * s : 26 * s;
            Glyph(id, D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), size, (enabled ? .95f : .35f) * a, m.playing);
        }
        if (ui.volumeOk) {
            Speaker(D2D1::Point2F(l + 8 * s, (volR_.top + volR_.bottom) / 2), 15 * s, 0, .6f * a);
            Speaker(D2D1::Point2F(r - 12 * s, (volR_.top + volR_.bottom) / 2), 15 * s, 2, .6f * a);
            Bar(volR_, ui.muted ? 0 : ui.volume, a, ui.hover == VolumeBar || ui.pressed == VolumeBar);
        } else volR_ = {};
        // Lyrics toggle: a glass pill left of the close button, filled while the lyrics view is on.
        if (ui.lyricsAvailable) {
            const float rad = (lyricsBtnR_.bottom - lyricsBtnR_.top) / 2;
            brush_->SetColor(ui.lyricsView ? D2D1::ColorF(1, 1, 1, .88f * a) : D2D1::ColorF(0, 0, 0, (ui.hover == LyricsButton ? .38f : .24f) * a));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(lyricsBtnR_, rad, rad), brush_.Get());
            Text(docky::ui::Text(L"Lyrics"), pill_.Get(), lyricsBtnR_, ui.lyricsView ? D2D1::ColorF(0, 0, 0, .85f * a) : D2D1::ColorF(1, 1, 1, .9f * a));
        }
        // Close: a small glass disc, top right.
        const D2D1_POINT_2F cc{(closeR_.left + closeR_.right) / 2, (closeR_.top + closeR_.bottom) / 2};
        brush_->SetColor(D2D1::ColorF(0, 0, 0, (ui.hover == Close ? .38f : .24f) * a));
        dc_->FillEllipse(D2D1::Ellipse(cc, 18 * s, 18 * s), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1, 1, 1, .85f * a));
        const float x = 6 * s;
        dc_->DrawLine({cc.x - x, cc.y - x}, {cc.x + x, cc.y + x}, brush_.Get(), 2 * s);
        dc_->DrawLine({cc.x - x, cc.y + x}, {cc.x + x, cc.y - x}, brush_.Get(), 2 * s);
    }

    static constexpr float kDown = 8;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID2D1Factory3> factory_; ComPtr<ID2D1DeviceContext2> dc_;
    ComPtr<IDWriteFactory> dwrite_; ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1GradientStopCollection> featherStops_; ComPtr<ID2D1LinearGradientBrush> featherBrush_, featherBrushV_;
    D2D1_RECT_F safe_{}, fullSafe_{};
    float flowTime_ = 0, beat_ = 0;
    ComPtr<ID2D1LinearGradientBrush> silkFade_; ComPtr<ID2D1Effect> silkBorder_, silkBlur_, silkCrop_;
    ComPtr<IDWriteTextFormat> title_, artist_, timeL_, timeR_, clock_, date_, pill_;
    std::vector<LyricLayout> layouts_; std::vector<BlurFx> blurPool_; D2D1_RECT_F lyricsColumn_{}; float lyricsStep_ = 60;
    ComPtr<ID2D1DeviceContext> lyricGlowContext_;
    BlurFx lyricGlowFx_;
    ComPtr<ID2D1LinearGradientBrush> wordSweep_;
    ComPtr<ID2D1SolidColorBrush> silentWord_;
    const void* lyricsKey_ = nullptr; size_t lyricsCount_ = 0; float lyricsWidth_ = 0, lyricsFont_ = 0;
    std::vector<std::pair<int, D2D1_RECT_F>> lyricRects_;
    Chain chains_[2]; ID2D1Image* lastBlur_[2]{}; float artRight_ = 0;
    float scale_ = 1, clockSize_ = 0, W_ = 0, H_ = 0;
    D2D1::Matrix3x2F base_ = D2D1::Matrix3x2F::Identity();
    D2D1_RECT_F panel_{}, progR_{}, playR_{}, prevR_{}, nextR_{}, volR_{}, closeR_{}, lyricsBtnR_{};
};

// ---------------------------------------------------------------- GPU

struct Gpu {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> mfManager; UINT mfToken = 0;
    bool Init() {
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        HRESULT hr = docky::CreateRenderDevice(flags, levels, 3, &device, nullptr, &context);
        if (FAILED(hr)) return false;
        ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&mfToken, &mfManager))) mfManager->ResetDevice(device.Get(), mfToken);
        return true;
    }
};

// ---------------------------------------------------------------- window

class App {
public:
    ~App() { if (musicFrameTimer_) CloseHandle(musicFrameTimer_); }
    int Run(HINSTANCE inst) {
        config_ = LoadConfig(); ReadSharedSettings();
        if (!gpu_.Init() || !scene_.Init(gpu_.device.Get())) return 1;
        if (!silk_.Init(gpu_.device.Get())) Log(L"silk shader unavailable; colour blobs instead");
        POINT pt; GetCursorPos(&pt);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT rc = mi.rcMonitor;
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClass, docky::ui::Text(L"Now Playing"), WS_POPUP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, inst, this);
        if (!hwnd_ || !CreateSwapChain()) return 1;
        const BOOL disable = TRUE; DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
        scene_.SetScale(float(GetDpiForWindow(hwnd_)) / 96.f);
        ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_);
        media_ = std::make_unique<MediaWorker>(hwnd_); media_->SetLocalTags(localTags_);
        if (config_.online) artwork_ = std::make_unique<ArtworkSource>([hwnd = hwnd_](const ArtworkResult& r) { PostMessageW(hwnd, kArtMsg, 0, reinterpret_cast<LPARAM>(new ArtworkResult(r))); });
        lyrics_ = std::make_unique<LyricsSource>([hwnd = hwnd_](const LyricsResult& r) { PostMessageW(hwnd, kLyricsMsg, 0, reinterpret_cast<LPARAM>(new LyricsResult(r))); });
        SetTimer(hwnd_, 1, 500, nullptr);
        opened_ = lastMove_ = Now();
        return Loop();
    }

    // --wallpaper: a child window of the desktop between the wallpaper and the icons (Windows 11 24H2+: inside Progman,
    // below SHELLDLL_DefView; earlier: in the WorkerW behind the icons). Hidden unless Apple Music is playing.
    int RunWallpaper(HINSTANCE inst, const std::wstring& testImage, const std::wstring& testLyrics = {}) {
        wallpaper_ = true; config_ = LoadConfig(); ReadSharedSettings();
        if (!gpu_.Init() || !scene_.Init(gpu_.device.Get())) return 1;
        silk_.Init(gpu_.device.Get());
        WNDCLASSEXW cc{sizeof(cc)}; cc.lpfnWndProc = Proc; cc.hInstance = inst; cc.lpszClassName = kWallCtlClass; RegisterClassExW(&cc);
        ctl_ = CreateWindowExW(0, kWallCtlClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
        if (!AttachToDesktop(inst)) { Log(L"wallpaper: desktop window not found"); return 1; }
        // The wallpaper is under the desktop icons and gets no mouse input. Raw input (passive, no hook) reports the
        // wheel and the left button; they act on the lyrics only when the pointer is on the desktop over them.
        // Registered only while lyrics are on the desktop (UpdateRawInput), so mouse movement does not wake it otherwise.
        if (!testImage.empty()) {  // diagnostics: show a fixed image as if it were playing
            auto a = std::make_shared<Art>(); a->still = scene_.LoadFile(testImage); a->quality = 1; a->palette = scene_.PaletteOfFile(testImage); cur_ = a; forceShow_ = true;
            if (!testLyrics.empty()) {  // and these lyrics, as if 80 s into a 215 s song
                HANDLE f = CreateFileW(testLyrics.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    std::string bytes(1 << 20, 0); DWORD read = 0; ReadFile(f, bytes.data(), DWORD(bytes.size()), &read, nullptr); CloseHandle(f); bytes.resize(read);
                    currentLyrics_ = std::make_shared<lyrics::Lyrics>(lyrics::Parse(LyricsSource::DecodeText(bytes)));
                }
                snapshot_.has = snapshot_.playing = true; snapshot_.title = L"Fixture Title"; snapshot_.artist = L"Fixture Artist";
                snapshot_.end = 215ll * 10000000; snapshot_.position = 80ll * 10000000; snapshot_.anchoredAt = Now();
            }
        } else {
            media_ = std::make_unique<MediaWorker>(hwnd_, kMusicApps); media_->SetLocalTags(localTags_);
            lyrics_ = std::make_unique<LyricsSource>([hwnd = hwnd_](const LyricsResult& r) { PostMessageW(hwnd, kLyricsMsg, 0, reinterpret_cast<LPARAM>(new LyricsResult(r))); });
            if (config_.online) artwork_ = std::make_unique<ArtworkSource>([hwnd = hwnd_](const ArtworkResult& r) { PostMessageW(hwnd, kArtMsg, 0, reinterpret_cast<LPARAM>(new ArtworkResult(r))); });
        }
        SetTimer(hwnd_, 1, 500, nullptr);
        Log(L"wallpaper: started %ux%u, %zu monitor(s)", W_, H_, monitors_.size());
        return Loop();
    }

    // --snapshot: render one frame offscreen to a PNG (visual checks without a live session).
    int Snapshot(const std::wstring& out, const std::wstring& image, const std::wstring& video, UINT w, UINT h, bool extend, bool clock,
                 const std::wstring& lyricsFile = {}, bool cardLayout = false, int64_t lyricTimeMs = 83000) {
        config_.extend = extend; config_.clock = clock;
        if (!gpu_.Init() || !scene_.Init(gpu_.device.Get())) return 1;
        scene_.SetScale(h / 1080.f * 1.25f);
        ComPtr<ID2D1Bitmap1> target;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(scene_.Dc()->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &target))) return 2;
        const bool floating = colors_.Init(gpu_.device.Get()) && colors_.Bind(scene_.Dc(), w, h);
        auto art = std::make_shared<Art>();
        if (!image.empty()) { art->still = scene_.LoadFile(image); art->palette = scene_.PaletteOfFile(image); }
        if (!video.empty()) {
            art->video = std::make_unique<VideoLayer>();
            if (!art->video->Open(gpu_.device.Get(), gpu_.mfManager.Get(), scene_.Dc(), video)) return 3;
            for (double t0 = Now(); Now() - t0 < 8 && !art->video->Failed();) { art->video->Update(); if (art->video->HasFrame() && Now() - t0 > 1.5) break; MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); Sleep(5); }
            if (!art->video->HasFrame()) { art->video->LogState(L"snapshot"); return 4; }
            if (auto p = scene_.PaletteOfBitmap(art->video->Bitmap()); !p.empty()) art->palette = std::move(p);
        }
        Media m; m.has = true; m.playing = true; m.canNext = m.canPrev = m.canPlayPause = true;
        m.title = L"Fixture Title"; m.artist = L"Fixture Artist"; m.album = L"Fixture Album";
        m.position = lyricTimeMs * 10000; m.end = 215ll * 10000000;
        Scene::Ui ui; ui.media = &m; ui.extend = extend; ui.clock = clock; ui.volumeOk = true; ui.volume = .6f;
        lyrics::Lyrics lyr;
        if (!lyricsFile.empty()) {
            HANDLE f = CreateFileW(lyricsFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                std::string bytes(1 << 20, '\0'); DWORD read = 0; ReadFile(f, bytes.data(), DWORD(bytes.size()), &read, nullptr); CloseHandle(f);
                bytes.resize(read); lyr = lyrics::Parse(LyricsSource::DecodeText(bytes));
            }
            ui.lyrics = &lyr; ui.lyricsAvailable = ui.lyricsView = !lyr.Empty(); ui.lyricsBlend = 1; ui.lyricsFullArt = !cardLayout;
            ui.lyricTimeMs = lyricTimeMs; ui.lyricsActive = lyr.LineAt(ui.lyricTimeMs);
            ui.lyricsPos = float(std::max(0, ui.lyricsActive)); ui.clock = false;
        }
        if (ui.lyricsView && !art->palette.empty() && silk_.Init(gpu_.device.Get()))
        {   // test hooks: DOCKY_SILK_TIME / DOCKY_SILK_TWIST pick the flow time and beat twist of the snapshot
            wchar_t v[32]{}; const float st = GetEnvironmentVariableW(L"DOCKY_SILK_TIME", v, 32) ? float(_wtof(v)) : 12.f;
            const float tw = GetEnvironmentVariableW(L"DOCKY_SILK_TWIST", v, 32) ? float(_wtof(v)) : 0.f;
            ui.silk = silk_.Render(scene_.Dc(), UINT(w * .56f / 2), h / 2, st, tw, art->palette);
        }
        scene_.Dc()->SetTarget(floating ? colors_.Bitmap() : target.Get());
        scene_.Dc()->BeginDraw();
        scene_.Draw(float(w), float(h), nullptr, art.get(), 1, ui);
        if (FAILED(scene_.Dc()->EndDraw())) return 5;
        if (floating) {
            scene_.Dc()->SetTarget(nullptr);
            ComPtr<IDXGISurface> surface;
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> output;
            if (FAILED(target->GetSurface(&surface)) || FAILED(surface.As(&texture)) ||
                FAILED(gpu_.device->CreateRenderTargetView(texture.Get(), nullptr, &output)) ||
                !colors_.Resolve(output.Get(), 0.f)) return 5;
        }
        return SavePng(scene_.Dc(), target.Get(), w, h, out) ? 0 : 6;
    }

private:
    static HWND FindWallpaperHost(HWND& insertAfter) {
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (!progman) return nullptr;
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &unused);  // asks Explorer for the wallpaper WorkerW
        if (FindWindowExW(progman, nullptr, L"WorkerW", nullptr)) { insertAfter = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr); return progman; }
        HWND found = nullptr;
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            wchar_t cls[32]{}; GetClassNameW(w, cls, 32);
            if (!wcscmp(cls, L"WorkerW") && !FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(l) = w; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        insertAfter = HWND_TOP;
        return found;
    }
    bool AttachToDesktop(HINSTANCE inst) {
        HWND after = nullptr; host_ = FindWallpaperHost(after);
        if (!host_) return false;
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = kWallClass; RegisterClassExW(&wc);
        RECT rc; GetClientRect(host_, &rc);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE, kWallClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom, host_, nullptr, inst, this);
        if (!hwnd_) return false;
        SetWindowPos(hwnd_, after ? after : HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_NOACTIVATE);
        UpdateMonitors();
        return CreateSwapChain();
    }
    void UpdateMonitors() {
        monitors_.clear();
        POINT origin{0, 0}; ClientToScreen(host_, &origin);
        struct Ctx { std::vector<Monitor>* out; POINT o; } ctx{&monitors_, origin};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l); MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(m, &mi)) {
                // rcWork leaves out the Dock band and the menu bar (both reserve their edge like a taskbar).
                Monitor x{mi.rcMonitor, mi.rcWork, m}; OffsetRect(&x.screen, -c->o.x, -c->o.y);
                OffsetRect(&x.work, -mi.rcMonitor.left, -mi.rcMonitor.top);
                c->out->push_back(x);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }
    // A full-screen or maximized app covers the wallpaper (single monitor): motion artwork pauses, as a game would want.
    bool Covered() const {
        if (monitors_.size() != 1) return false;
        HWND fg = GetForegroundWindow(); if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) return false;
        wchar_t cls[32]{}; GetClassNameW(fg, cls, 32);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
        if (IsZoomed(fg)) return true;
        RECT r; MONITORINFO mi{sizeof(mi)};
        return GetWindowRect(fg, &r) && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) &&
               r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
    }
    // Settings > Control Center > "Keep artwork clear of the Dock and menu bar" (shared settings file).
    void ReadSharedSettings() {
        const auto path = liquid::settings::DefaultPath();
        settingsRevision_ = liquid::settings::ReadRevision(path);
        const auto loaded = liquid::settings::Load(path);
        docky::ui::SetLanguage(loaded.value.appearance.language);
        if (loaded.status == liquid::settings::LoadStatus::Loaded || loaded.status == liquid::settings::LoadStatus::Missing) {
            avoidBars_ = loaded.value.controlCenter.wallpaperAvoidBars;
            lyricsFullArt_ = loaded.value.controlCenter.lyricsFullArtwork;
            lyricsAutoReturn_ = loaded.value.controlCenter.lyricsAutoReturn;
            if (lyricsDoubleClickSeek_ != loaded.value.controlCenter.lyricsDoubleClickSeek) {
                lyricsDoubleClickSeek_ = loaded.value.controlCenter.lyricsDoubleClickSeek;
                lastLyricClick_ = -1;
            }
            SetAnimatedArtwork(loaded.value.controlCenter.animatedArtwork);
            keepPaused_ = loaded.value.controlCenter.keepArtworkWhenPaused;
            localTags_ = loaded.value.controlCenter.localMusicTags;
            if (media_) media_->SetLocalTags(localTags_);
            lyricsBeat_ = loaded.value.controlCenter.lyricsBeat;
            lyricsOffsetMs_ = loaded.value.controlCenter.lyricsOffsetMs;
            const int globalCap = loaded.value.performance.animationFps;
            const int musicCap = loaded.value.controlCenter.musicFps;
            musicFps_ = musicCap > 0 ? (std::min)(globalCap,musicCap) : globalCap;
            musicResponse_ = static_cast<MusicResponse>(loaded.value.controlCenter.musicResponse);
            beat_.SetSensitivity(loaded.value.controlCenter.musicSensitivity);
            const bool lyricsBefore = wallpaperLyrics_;
            wallpaperLyrics_ = loaded.value.controlCenter.wallpaperLyrics;
            if (wallpaper_ && wallpaperLyrics_ && !lyricsBefore && !currentLyrics_ && snapshot_.has) { lyricsPending_ = true; lyricsSince_ = Now() - 10; }
        }
    }
    void SetAnimatedArtwork(bool enabled) {
        if (enabled == animatedArtwork_) return;
        animatedArtwork_ = enabled;
        if (!enabled) {
            pendingVideo_.reset();
            for (const auto& art : {cur_, prev_}) {
                if (!art || !art->video) continue;
                art->video.reset();
                art->quality = art->still ? 1 : 0;
            }
        }
        if (artwork_ && snapshot_.has && !artKey_.empty()) {
            pendingRequest_ = ArtworkRequest{snapshot_.title, snapshot_.artist, snapshot_.album, H_ > W_};
            requestSince_ = thumbSince_ = Now();
            requestThumbHash_ = snapshot_.thumbHash;
            MaybeRequestArtwork(true);
        }
        dirty_ = true;
    }
    bool VideoShouldPlay() const { return wallpaper_ ? (forceShow_ || snapshot_.playing) && shown_ && !covered_ : snapshot_.playing; }

    bool CreateSwapChain() {
        RECT rc; GetClientRect(hwnd_, &rc); W_ = UINT(rc.right - rc.left); H_ = UINT(rc.bottom - rc.top);
        ComPtr<IDXGIDevice> dxgi; gpu_.device.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
        floatingScene_ = colors_.Init(gpu_.device.Get()) && colors_.Bind(scene_.Dc(), W_, H_);
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = W_; d.Height = H_; d.Format = floatingScene_ ? docky::color::SceneFormat : docky::color::SdrFormat; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | (wallpaper_ ? DXGI_USAGE_SHADER_INPUT : 0); d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (floatingScene_) {
            scRgb_ = SUCCEEDED(factory->CreateSwapChainForComposition(gpu_.device.Get(), &d, nullptr, &swap_)) &&
                     docky::color::EnableScRgb(swap_.Get());
        }
        if (!scRgb_) {
            swap_.Reset();
            d.Format = docky::color::SdrFormat;
            if (FAILED(factory->CreateSwapChainForComposition(gpu_.device.Get(), &d, nullptr, &swap_))) return false;
        }
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_))) || FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dtarget_)) ||
            FAILED(dcomp_->CreateVisual(&visual_))) return false;
        visual_->SetContent(swap_.Get()); dtarget_->SetRoot(visual_.Get()); dcomp_->Commit();
        RefreshOutputWhite();
        Log(L"colour output: %ls; %ls scene", scRgb_ ? L"FP16 scRGB" : L"BGRA8 SDR", floatingScene_ ? L"FP16" : L"BGRA8");
        return BindTarget();
    }
    bool BindTarget() {
        scene_.Dc()->SetTarget(nullptr); target_.Reset();
        if (floatingScene_) {
            outputRtv_.Reset();
            ComPtr<ID3D11Texture2D> buffer;
            if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
                FAILED(gpu_.device->CreateRenderTargetView(buffer.Get(), nullptr, &outputRtv_))) return false;
            target_ = colors_.Bitmap();
            return target_ != nullptr;
        }
        ComPtr<IDXGISurface> surface; if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const auto options = wallpaper_ ? D2D1_BITMAP_OPTIONS_TARGET : D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        const auto props = D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        return SUCCEEDED(scene_.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_));
    }

    void RefreshOutputWhite() {
        if (!scRgb_) return;
        const float white = docky::color::DisplayWhite(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        if (white != outputWhite_) { outputWhite_ = white; dirty_ = true; }
        if (!wallpaper_) return;
        std::vector<docky::color::ColorOutput::Region> next;
        next.reserve(monitors_.size());
        for (const auto& monitor : monitors_)
            next.push_back({monitor.screen, docky::color::DisplayWhite(monitor.handle)});
        if (next.size() != outputRegions_.size() ||
            !std::equal(next.begin(), next.end(), outputRegions_.begin(), [](const auto& a, const auto& b) {
                return a.white == b.white && EqualRect(&a.rect, &b.rect);
            })) dirty_ = true;
        outputRegions_ = std::move(next);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->Handle(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    void Close() { if (closing_ < 0) { closing_ = Now(); dirty_ = true; } }

    LRESULT Handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CLOSE: Close(); return 0;
        case WM_INPUT:
            if (h == ctl_) OnDesktopMouse(reinterpret_cast<HRAWINPUT>(lp));
            break;  // DefWindowProc cleans up
        case WM_DESTROY: if (h == hwnd_) PostQuitMessage(0); return 0;  // also when Explorer restarts and takes the host with it
        case WM_TIMER:
            if (media_ && seekDeadline_ > 0 && Now() >= seekDeadline_) OnMedia();
            MaybeRequestArtwork();
            MaybeRequestLyrics();
            ReadTrackLyricOffset();
            if (wallpaper_) {
                covered_ = Covered();
                if (liquid::settings::ReadRevision(liquid::settings::DefaultPath()) != settingsRevision_) { ReadSharedSettings(); dirty_ = true; }
                RECT rc; GetClientRect(host_, &rc);
                if (UINT(rc.right) != W_ || UINT(rc.bottom) != H_) SetWindowPos(hwnd_, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
                UpdateMonitors();
                RefreshOutputWhite();
                return 0;
            }
            RefreshOutputWhite();
            if (liquid::settings::ReadRevision(liquid::settings::DefaultPath()) != settingsRevision_) { ReadSharedSettings(); dirty_ = true; }
            {
            SYSTEMTIME st; GetLocalTime(&st);
            const float v = volume_.Get(); const bool m = volume_.Muted();
            if (snapshot_.playing || st.wMinute != minute_ || v != volumeLevel_ || m != muted_) dirty_ = true;
            minute_ = st.wMinute; volumeLevel_ = v; muted_ = m;
            return 0;
            }
        case kMediaMsg: OnMedia(); return 0;
        case kArtMsg: { std::unique_ptr<ArtworkResult> r(reinterpret_cast<ArtworkResult*>(lp)); OnArtwork(*r); return 0; }
        case kLyricsMsg: {
            std::unique_ptr<LyricsResult> r(reinterpret_cast<LyricsResult*>(lp));
            if (r->tag == lyricsRequestTag_ && (!r->lyrics.Empty() || !currentLyrics_)) {
                if (!currentLyrics_) lyricsPos_ = -1;
                currentLyrics_ = std::make_shared<lyrics::Lyrics>(std::move(r->lyrics));
                dirty_ = true;
            }
            return 0;
        }
        case WM_ACTIVATE:
            SetWindowPos(h, LOWORD(wp) == WA_INACTIVE ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && controls_ < .05f && controlsTarget_ == 0) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_MOUSEWHEEL:
            if (LyricsView()) { ScrollLyrics(-float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA); lastMove_ = Now(); }
            return 0;
        case WM_MOUSEMOVE: {
            const float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
            if (x != mouseX_ || y != mouseY_) { lastMove_ = Now(); mouseX_ = x; mouseY_ = y; }
            if (lyricsPress_) {
                const float dy = y - lyricsPressY_;
                if (!lyricsDragged_ && std::abs(dy) > 6 * float(GetDpiForWindow(h)) / 96.f) lyricsDragged_ = true;
                if (lyricsDragged_) { ScrollLyrics(0); lyricsManualPos_ = ClampLine(lyricsPressPos_ - dy / scene_.LyricsStep()); }
                return 0;
            }
            const int hover = scene_.Hit(x, y);
            if (hover != hover_) { hover_ = hover; dirty_ = true; }
            if (pressed_ == Scene::Progress) { seekPreview_ = scene_.ProgressAt(x); dirty_ = true; }
            if (pressed_ == Scene::VolumeBar) { volumeLevel_ = scene_.VolumeAt(x); muted_ = false; volume_.Set(volumeLevel_); dirty_ = true; }
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            const float x = float(GET_X_LPARAM(lp)), y = float(GET_Y_LPARAM(lp));
            lastMove_ = Now();
            if (LyricsView() && scene_.InLyrics(x, y)) {  // lyrics: drag to scroll or double-click a timed line
                lyricsPress_ = true; lyricsDragged_ = false; lyricsPressX_ = x; lyricsPressY_ = y;
                lyricsPressPos_ = lyricsManual_ ? lyricsManualPos_ : std::max(0.f, lyricsPos_);
                SetCapture(h); return 0;
            }
            lastLyricClick_ = -1;
            if (controls_ < .3f) { controlsTarget_ = 1; dirty_ = true; return 0; }  // first click only reveals the controls
            pressed_ = scene_.Hit(x, y); SetCapture(h);
            if (pressed_ == Scene::Progress && snapshot_.has && snapshot_.canSeek && snapshot_.end > 0) seekPreview_ = scene_.ProgressAt(x);
            else if (pressed_ == Scene::Progress) pressed_ = Scene::None;
            if (pressed_ == Scene::VolumeBar) { volumeLevel_ = scene_.VolumeAt(x); muted_ = false; volume_.Set(volumeLevel_); }
            if (pressed_ == Scene::None && !scene_.InPanel(x, y)) hideNow_ = true;  // clicking the artwork hides the controls
            dirty_ = true; return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (lyricsPress_) {
                lyricsPress_ = false; lyricsScrolled_ = Now();
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!lyricsDragged_ && std::abs(point.x - lyricsPressX_) <= 4.f && std::abs(point.y - lyricsPressY_) <= 4.f)
                    CompleteLyricClick(scene_.LyricAt(float(point.x), float(point.y)), point);
                else lastLyricClick_ = -1;
                dirty_ = true; return 0;
            }
            const int hit = scene_.Hit(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)));
            if (pressed_ == Scene::Progress && seekPreview_ >= 0) {
                SeekPlayback(int64_t(double(seekPreview_) * double(snapshot_.end)));
            } else if (pressed_ == hit) {
                if (hit == Scene::Close) Close();
                else if (hit == Scene::LyricsButton) { config_.lyrics = !config_.lyrics; SaveConfig(config_); }
                else if (hit == Scene::Play) media_->Command(Cmd::PlayPause);
                else if (hit == Scene::Next) media_->Command(Cmd::Next);
                else if (hit == Scene::Prev) media_->Command(Cmd::Prev);
            }
            pressed_ = Scene::None; seekPreview_ = -1; dirty_ = true; return 0;
        }
        case WM_KEYDOWN:
            lastMove_ = Now(); controlsTarget_ = 1; dirty_ = true;
            switch (wp) {
            case VK_ESCAPE: Close(); break;
            case VK_SPACE: case VK_MEDIA_PLAY_PAUSE: media_->Command(Cmd::PlayPause); break;
            case VK_RIGHT: media_->Command(Cmd::Next); break;
            case VK_LEFT: media_->Command(Cmd::Prev); break;
            case VK_UP: volumeLevel_ = Clamp01(volumeLevel_ + .05f); volume_.Set(volumeLevel_); muted_ = false; break;
            case VK_DOWN: volumeLevel_ = Clamp01(volumeLevel_ - .05f); volume_.Set(volumeLevel_); break;
            case 'F': config_.extend = !config_.extend; SaveConfig(config_); break;
            case 'C': config_.clock = !config_.clock; SaveConfig(config_); break;
            case 'L': config_.lyrics = !config_.lyrics; SaveConfig(config_); break;
            case VK_OEM_4: case VK_OEM_6: {  // [ and ]: lyrics later / earlier, saved for the desktop too
                if (GetKeyState(VK_SHIFT) < 0) {
                    AdjustTrackLyricOffset(wp == VK_OEM_6 ? 50 : -50);
                    break;
                }
                lyricsOffsetMs_ = std::clamp(lyricsOffsetMs_ + (wp == VK_OEM_6 ? 100 : -100), -3000, 3000);
                liquid::settings::WriteControlCenterInt(liquid::settings::DefaultPath(), "lyricsOffsetMs", lyricsOffsetMs_);
                settingsRevision_ = liquid::settings::ReadRevision(liquid::settings::DefaultPath());
                wchar_t note[48]; swprintf_s(note, docky::ui::Text(L"Lyrics timing %+.1f s"), lyricsOffsetMs_ / 1000.0);
                toast_ = note; toastUntil_ = Now() + 1.6; break;
            }
            }
            return 0;
        case WM_DISPLAYCHANGE: case WM_DPICHANGED: {
            if (wallpaper_) {
                UpdateMonitors();
                RefreshOutputWhite();
                dirty_ = true;
                return 0;
            }
            MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowPos(h, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
            scene_.SetScale(float(GetDpiForWindow(h)) / 96.f);
            RefreshOutputWhite();
            dirty_ = true;
            return 0;
        }
        case WM_SIZE:
            if (swap_ && LOWORD(lp) && HIWORD(lp) && (LOWORD(lp) != W_ || HIWORD(lp) != H_)) {
                scene_.Dc()->SetTarget(nullptr); target_.Reset();
                outputRtv_.Reset();
                W_ = LOWORD(lp); H_ = HIWORD(lp);
                if (FAILED(swap_->ResizeBuffers(2, W_, H_, DXGI_FORMAT_UNKNOWN, 0)) ||
                    (floatingScene_ && !colors_.Bind(scene_.Dc(), W_, H_)) || !BindTarget()) {
                    Log(L"colour output: resize failed");
                    PostQuitMessage(2);
                    return 0;
                }
                dirty_ = true;
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Crossfade to new artwork (track change, or a better source for the same track).
    // Motion artwork can differ from the still it replaced (another edition, a different scene), so its colours come
    // from its own frames: the first one, then again ~1.5 s in (motion covers often open dark or with a fade).
    void VideoPalette() {
        if (!cur_ || !cur_->video || !cur_->video->HasFrame() || cur_->videoPalette >= 2) return;
        if (cur_->videoPalette == 1 && cur_->video->Frames() < 45) return;
        auto p = scene_.PaletteOfBitmap(cur_->video->Bitmap());
        if (!p.empty()) { cur_->palette = std::move(p); dirty_ = true; }
        ++cur_->videoPalette;
    }
    void Show(std::shared_ptr<Art> art) {
        if (cur_ && cur_->Image()) { prev_ = cur_; fadeStart_ = Now(); }
        cur_ = std::move(art); dirty_ = true;
        if (cur_ && cur_->video) cur_->video->SetPlaying(VideoShouldPlay());
    }

    void AnchorClock(bool track) {
        FILETIME ft{};
        GetSystemTimePreciseAsFileTime(&ft);
        const int64_t utc = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const double now = Now();
        const int64_t estimate = anchorPosition_ + (clockPlaying_
            ? int64_t(std::max(0.0, now - anchorClock_) * clockRate_ * lyrics::kTicksPerSecond) : 0);
        int64_t next = estimate;
        if (track) seekDeadline_ = 0;
        const bool apple = ContainsAppleMusic(snapshot_.source);
        const bool whole = apple && snapshot_.position % lyrics::kTicksPerSecond == 0;
        const bool positionChanged = snapshot_.position != rawPosition_;
        const bool newSample = track || !clockValid_ || positionChanged || (!whole && snapshot_.updated != rawUpdated_);
        const bool resumed = snapshot_.playing && !clockPlaying_ && !track;
        if (newSample) {
            const int64_t age = snapshot_.updated > 0 ? utc - snapshot_.updated : -1;
            int64_t observed = std::max<int64_t>(0, snapshot_.position);
            const auto maxAge = resumed ? lyrics::kTicksPerSecond / 2 : 3 * lyrics::kTicksPerSecond;
            if (snapshot_.playing && age >= 0 && age <= maxAge) observed += static_cast<int64_t>(age * snapshot_.rate);
            const bool seeking = seekDeadline_ > 0;
            const bool confirmed = seeking && positionChanged &&
                std::llabs(observed - estimate) <= (whole ? lyrics::kTicksPerSecond : lyrics::kTicksPerSecond / 2);
            if (!seeking || confirmed || now >= seekDeadline_) {
                seekDeadline_ = 0;
                rawPosition_ = snapshot_.position;
                rawUpdated_ = snapshot_.updated;
                const bool reanchor = track || !clockValid_ || (seeking && !confirmed) ||
                    (!snapshot_.playing && !clockPlaying_ && positionChanged);
                next = lyrics::CorrectPlaybackPosition(estimate, observed, whole && (snapshot_.playing || clockPlaying_), reanchor);
                if (!track && clockValid_ && clockPlaying_ && snapshot_.playing &&
                    std::llabs(next - estimate) > 3 * lyrics::kTicksPerSecond) ResetMusic();
                clockValid_ = true;
            }
        } else if (seekDeadline_ > 0 && now >= seekDeadline_) {
            // Some players ignore seek while retaining their old timeline snapshot.
            next = snapshot_.position;
            seekDeadline_ = 0;
        }
        anchorPosition_ = snapshot_.end > 0 ? std::clamp<int64_t>(next, 0, snapshot_.end) : std::max<int64_t>(0, next);
        anchorClock_ = now;
        clockRate_ = snapshot_.rate;
        clockPlaying_ = snapshot_.playing;
        snapshot_.position = anchorPosition_;
        snapshot_.anchoredAt = anchorClock_;
    }

    static bool ContainsAppleMusic(std::wstring source) {
        for (auto& ch : source) ch = static_cast<wchar_t>(std::towlower(ch));
        return source.find(L"applemusic") != std::wstring::npos;
    }

    void SeekPlayback(int64_t position) {
        media_->Command(Cmd::Seek, position);
        ResetMusic();
        anchorPosition_ = snapshot_.position = position;
        anchorClock_ = snapshot_.anchoredAt = Now();
        seekDeadline_ = anchorClock_ + 1.1;
    }



    void OnMedia() {
        snapshot_ = media_->Snapshot();
        AnchorClock(!snapshot_.has || snapshot_.TrackKey() != trackKey_);
        const std::wstring key = snapshot_.has ? snapshot_.TrackKey() : L"";
        const bool track = key != trackKey_;
        if (track) {
            ResetMusic();
            trackKey_ = key; pendingVideo_.reset(); pendingRequest_.reset(); lastLyricClick_ = -1; deskPress_ = false;
            currentLyrics_.reset(); lyricsPos_ = -1; lyricsManual_ = false;
            lyricsRequestTag_.clear(); lyricsRequestedDuration_ = 0;
            ReadTrackLyricOffset();
            lyricsPending_ = lyrics_ && snapshot_.has && !snapshot_.title.empty() && (!wallpaper_ || wallpaperLyrics_); lyricsSince_ = Now();
            ArtworkRequest req{snapshot_.title, snapshot_.artist, snapshot_.album, H_ > W_};
            const std::wstring artKey = ArtworkSource::KeyFor(req);
            // Each recording has its own validated artwork match, even when two tracks share an album.
            if (artKey.empty() || artKey != artKey_ || !cur_) {
                artKey_ = artKey; thumbHash_ = 0;
                if (snapshot_.thumb) { auto a = std::make_shared<Art>(); a->still = scene_.LoadPixels(*snapshot_.thumb, snapshot_.thumbW, snapshot_.thumbH); a->palette = Scene::PaletteOfPixels(*snapshot_.thumb, snapshot_.thumbW, snapshot_.thumbH); thumbHash_ = snapshot_.thumbHash; Show(a); }
                else if (snapshot_.has) Show(nullptr);
                if (artwork_ && !artKey.empty()) { pendingRequest_ = req; requestSince_ = thumbSince_ = Now(); requestThumbHash_ = snapshot_.thumbHash; }
            }
        } else if (snapshot_.thumb && snapshot_.thumbHash != thumbHash_ && (!cur_ || cur_->quality == 0)) {
            auto a = std::make_shared<Art>(); a->still = scene_.LoadPixels(*snapshot_.thumb, snapshot_.thumbW, snapshot_.thumbH);
            a->palette = Scene::PaletteOfPixels(*snapshot_.thumb, snapshot_.thumbW, snapshot_.thumbH);
            thumbHash_ = snapshot_.thumbHash; Show(a);
        }
        if (!track && lyrics_ && lyricsRequestedDuration_ <= 0 && snapshot_.end > 0 && !lyricsRequestTag_.empty()) {
            lyricsPending_ = true;
            ReadTrackLyricOffset();
        }
        if (cur_ && cur_->video) cur_->video->SetPlaying(VideoShouldPlay());
        MaybeRequestArtwork();
        MaybeRequestLyrics();
        dirty_ = true;
    }

    // The catalog lookup waits until the player's cover has settled (players often publish the previous track's
    // cover for a moment), so the image check compares against the right one; without a cover, after 3.5 s.
    // Lyrics are requested once the player has published the track length (it separates versions), or after 1.5 s.
    void MaybeRequestLyrics() {
        if (!lyricsPending_ || !lyrics_) return;
        if (snapshot_.end <= 0 && Now() - lyricsSince_ < 1.5) return;
        lyricsPending_ = false;
        if (wallpaper_ && !wallpaperLyrics_) return;
        lyricsRequestedDuration_ = static_cast<int>(snapshot_.end / 10000);
        lyricsRequestTag_ = trackKey_ + L"\x1f" + std::to_wstring(lyricsRequestedDuration_);
        lyrics_->Request({snapshot_.title, snapshot_.artist, snapshot_.album, lyricsRequestedDuration_, config_.online,
            lyricsRequestTag_, snapshot_.localLyrics, ContainsAppleMusic(snapshot_.source)});
    }
    // Desktop lyrics: wheel scrolls them and a double-click can seek. The pointer must be
    // on the desktop itself (icons view, Progman/WorkerW), over the lyrics, and the wallpaper is showing them.
    void OnDesktopMouse(HRAWINPUT handle) {
        if (!wallpaper_ || !shown_ || covered_ || !LyricsView()) return;
        RAWINPUT input{}; UINT size = sizeof(input);
        if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || input.header.dwType != RIM_TYPEMOUSE) return;
        const USHORT flags = input.data.mouse.usButtonFlags;
        if (!(flags & (RI_MOUSE_WHEEL | RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP))) return;
        POINT screen; GetCursorPos(&screen);
        HWND under = WindowFromPoint(screen); wchar_t cls[32]{}; GetClassNameW(GetAncestor(under, GA_ROOT), cls, 32);
        // DockOver Desktop's Stacks lays a transparent catcher over the desktop: that is the desktop too.
        if (wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW") && wcscmp(cls, L"DockyDesktopStacksCatcher")) { deskPress_ = false; lastLyricClick_ = -1; return; }
        POINT local = screen; ScreenToClient(hwnd_, &local);
        const float x = float(local.x), y = float(local.y);
        const bool overColumn = std::any_of(deskLyricsColumns_.begin(), deskLyricsColumns_.end(), [&](const D2D1_RECT_F& c) { return x >= c.left && x < c.right && y >= c.top && y < c.bottom; });
        if (flags & RI_MOUSE_WHEEL) {
            if (overColumn) ScrollLyrics(-float(short(input.data.mouse.usButtonData)) / WHEEL_DELTA);
            return;
        }
        if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            deskPress_ = overColumn && lyricsDoubleClickSeek_;
            if (!deskPress_) lastLyricClick_ = -1;
            deskPressPoint_ = screen; deskPressTime_ = Now();
            return;
        }
        if ((flags & RI_MOUSE_LEFT_BUTTON_UP) && deskPress_) {
            deskPress_ = false;
            int line = -1;
            if (std::abs(screen.x - deskPressPoint_.x) <= 4 && std::abs(screen.y - deskPressPoint_.y) <= 4 && Now() - deskPressTime_ <= .6)
                for (auto [index, r] : deskLyricRects_)
                    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) { line = index; break; }
            CompleteLyricClick(line, screen);
        }
    }
    void UpdateRawInput() {
        const bool want = wallpaper_ && shown_ && !covered_ && LyricsView();
        if (want == rawInput_) return;
        RAWINPUTDEVICE mouse{0x01, 0x02, want ? DWORD(RIDEV_INPUTSINK) : DWORD(RIDEV_REMOVE), want ? ctl_ : nullptr};
        if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) rawInput_ = want;
        else { Log(L"wallpaper: raw mouse input unavailable (%lu)", GetLastError()); rawInput_ = want; }
    }
    void CompleteLyricClick(int line, POINT point) {
        if (!lyricsDoubleClickSeek_ || line < 0) { lastLyricClick_ = -1; return; }
        const ULONGLONG now = GetTickCount64();
        const UINT dpi = GetDpiForWindow(hwnd_);
        if (lastLyricClick_ == line && now - lastLyricClickTime_ <= GetDoubleClickTime() &&
            std::abs(point.x - lastLyricClickPoint_.x) <= GetSystemMetricsForDpi(SM_CXDOUBLECLK, dpi) / 2 &&
            std::abs(point.y - lastLyricClickPoint_.y) <= GetSystemMetricsForDpi(SM_CYDOUBLECLK, dpi) / 2) {
            lastLyricClick_ = -1;
            SeekToLine(line);
            return;
        }
        lastLyricClick_ = line;
        lastLyricClickTime_ = now;
        lastLyricClickPoint_ = point;
    }
    void SeekToLine(int line) {
        if (!currentLyrics_ || line < 0 || size_t(line) >= currentLyrics_->lines.size() || currentLyrics_->lines[size_t(line)].ms < 0 || !snapshot_.canSeek) return;
        const int64_t t = std::max<int64_t>(0, int64_t(currentLyrics_->lines[size_t(line)].ms) - TotalLyricsOffset()) * 10000;
        SeekPlayback(snapshot_.end > 0 ? std::min(t, snapshot_.end) : t);
        lyricsManual_ = false; dirty_ = true;
        Log(L"lyrics: play from line %d", line);
    }
    float ClampLine(float v) const { return currentLyrics_ ? std::clamp(v, 0.f, float(std::max<size_t>(1, currentLyrics_->lines.size()) - 1)) : 0.f; }
    void ScrollLyrics(float lines) {
        lastLyricClick_ = -1;
        if (!lyricsManual_) { lyricsManual_ = true; lyricsManualPos_ = std::max(0.f, lyricsPos_); Log(L"lyrics: scrolling (%ls)", wallpaper_ ? L"desktop" : L"full screen"); }
        lyricsManualPos_ = ClampLine(lyricsManualPos_ + lines);
        lyricsScrolled_ = Now(); dirty_ = true;
    }
    // The beat pulse runs only while lyrics are on screen and music plays (and the desktop is not covered).
    // A beat pushes the flow forward a little (a visible surge), once per pulse.
    void ResetMusic() {
        beat_.Reset();
        musicResetAt_ = Now();
        musicSync_.Reset();
        musicImpulses_ = {};
        musicLevel_ = musicTwist_ = {};
    }
    void WaitForMusicFrame(double deadline) {
        const double left = deadline - Now();
        if (left <= 0) return;
        if (!musicFrameTimer_) musicFrameTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(left * 1e7));
        if (musicFrameTimer_ && SetWaitableTimer(musicFrameTimer_, &due, 0, nullptr, nullptr, FALSE))
            MsgWaitForMultipleObjects(1, &musicFrameTimer_, FALSE, INFINITE, QS_ALLINPUT);
        else
            MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(std::ceil(left * 1000)), QS_ALLINPUT);
    }
    float SampleMusic(double now) {
        const double dt = lastMusicSample_ ? std::clamp(now - lastMusicSample_, 0.0, .1) : 1.0 / 60;
        lastMusicSample_ = now;
        const auto deliver = [&](const RhythmEvent& event) {
            auto& impulse = musicImpulses_[static_cast<size_t>(event.kind)];
            impulse = event;
            if (musicTrace_ && (!musicTraceEvent_.published || event.kind == RhythmKind::Kick))
                musicTraceEvent_ = event;
        };
        MusicFrame frame;
        while (beat_.Pop(frame)) {
            if (frame.published < musicResetAt_) continue;
            if (frame.generation != musicSync_.Frame().generation) {
                musicImpulses_ = {};
                musicLevel_ = musicTwist_ = {};
            }
            musicSync_.Read(frame, now, deliver);
        }
        musicSync_.Advance(now, deliver);
        const auto& state = musicSync_.Frame();
        const bool fresh = beat_.Running() && now - state.published < .25;
        constexpr float attacks[]{.006f, .015f, .042f};
        constexpr float releases[]{.105f, .18f, .31f};
        const auto mode = static_cast<size_t>(musicResponse_);
        const float attack = attacks[mode], release = releases[mode];
        const auto pulse = [&](RhythmKind kind) {
            const auto& event = musicImpulses_[static_cast<size_t>(kind)];
            return event.strength * std::exp(-float(std::max(0.0, now - event.at)) / release);
        };
        const float strength = fresh ? std::max({pulse(RhythmKind::Beat), .8f * pulse(RhythmKind::Kick),
            .35f * pulse(RhythmKind::Snare), .12f * pulse(RhythmKind::HiHat),
            (state.period ? .15f : .55f) * pulse(RhythmKind::Onset),
            .55f * pulse(RhythmKind::Drop)}) : 0.f;
        const float targetLevel = fresh ? .65f * state.bass + .35f * state.level : 0.f;
        musicLevel_.Step(targetLevel, dt, attack, release);
        musicTwist_.Step(strength + .25f * musicLevel_.value, dt, attack, release);
        if (LyricsView() && snapshot_.playing)
            flowClock_ += float(dt) * (.12f + .3f * (fresh ? musicLevel_.value : .3f) + .42f * strength);
        return strength;
    }
    bool UpdateBeat() {
        const bool want = lyricsBeat_ && snapshot_.playing && LyricsView() && (!wallpaper_ || (shown_ && !covered_));
        if (want && !beat_.Running()) beat_.Start();
        else if (!want && beat_.Running()) { beat_.Stop(); ResetMusic(); }
        return want;
    }
    bool LyricsView() const { return (wallpaper_ ? wallpaperLyrics_ : config_.lyrics) && currentLyrics_ && !currentLyrics_->Empty(); }

    std::wstring TrackLyricOffsetKey() const {
        if (trackKey_.empty() || snapshot_.title.empty()) return {};
        wchar_t key[24];
        swprintf_s(key, L"%016llx", static_cast<unsigned long long>(
            lyrics::OffsetKey(trackKey_, static_cast<int>(snapshot_.end / 10000))));
        return key;
    }

    void ReadTrackLyricOffset() {
        const auto key = TrackLyricOffsetKey();
        wchar_t value[16]{};
        if (!key.empty()) GetPrivateProfileStringW(L"lyric-track-offsets", key.c_str(), L"0", value, ARRAYSIZE(value), IniPath().c_str());
        const int offset = std::clamp(_wtoi(value), -3000, 3000);
        if (offset != trackLyricOffsetMs_) dirty_ = true;
        trackLyricOffsetMs_ = offset;
    }

    void AdjustTrackLyricOffset(int delta) {
        const auto key = TrackLyricOffsetKey();
        if (key.empty()) return;
        ReadTrackLyricOffset();
        const int offset = std::clamp(trackLyricOffsetMs_ + delta, -3000, 3000);
        const auto value = std::to_wstring(offset);
        if (!WritePrivateProfileStringW(L"lyric-track-offsets", key.c_str(), offset ? value.c_str() : nullptr, IniPath().c_str())) return;
        trackLyricOffsetMs_ = offset;
        wchar_t note[64]; swprintf_s(note, docky::ui::Text(L"This song: lyrics %+.2f s"), offset / 1000.0);
        toast_ = note; toastUntil_ = Now() + 1.6;
    }

    int TotalLyricsOffset() const { return lyricsOffsetMs_ + trackLyricOffsetMs_; }
    int64_t LyricTimeMs() const { return snapshot_.PositionNow() / 10000 + TotalLyricsOffset(); }

    // Moves the smoothed lyric position toward the line being sung; true while it is still moving.
    bool StepLyrics(float dt) {
        const float blendTarget = LyricsView() ? 1.f : 0.f, blendBefore = lyricsBlend_;
        lyricsBlend_ = Approach(lyricsBlend_, blendTarget, dt, .2f);
        if (std::abs(lyricsBlend_ - blendTarget) < .003f) lyricsBlend_ = blendTarget;
        bool moving = lyricsBlend_ != blendBefore;
        if (!currentLyrics_ || currentLyrics_->Empty()) return moving;
        const auto& l = *currentLyrics_;
        const int64_t ms = LyricTimeMs();
        const int early = l.LineAt(ms + 150);
        const int active = early >= 0 && l.lines[early].words.empty() ? early : l.LineAt(ms);
        if (active != lyricsActive_) { lyricsActive_ = active; moving = true; }
        // Manual scrolling ends after 1.2 s without scrolling (setting), or never until a line is clicked.
        if (lyricsManual_ && !lyricsPress_ && lyricsAutoReturn_ && Now() - lyricsScrolled_ > 1.2) lyricsManual_ = false;
        const float blendBefore2 = lyricsManualBlend_;
        lyricsManualBlend_ = Approach(lyricsManualBlend_, lyricsManual_ ? 1.f : 0.f, dt, .14f);
        if (std::abs(lyricsManualBlend_ - (lyricsManual_ ? 1.f : 0.f)) < .003f) lyricsManualBlend_ = lyricsManual_ ? 1.f : 0.f;
        if (lyricsManualBlend_ != blendBefore2) moving = true;
        const float target = lyricsManual_ ? lyricsManualPos_ : l.synced ? float(std::max(0, lyricsActive_)) :
            (snapshot_.end > 0 ? float(double(snapshot_.PositionNow()) / double(snapshot_.end)) * float(l.lines.size() - 1) : 0.f);
        if (lyricsPos_ < 0) lyricsPos_ = target;
        const float before = lyricsPos_;
        lyricsPos_ = Approach(lyricsPos_, target, dt, lyricsManual_ ? .07f : .16f);
        if (std::abs(lyricsPos_ - target) < .002f) lyricsPos_ = target;
        return moving || lyricsPos_ != before;
    }
    // Milliseconds until the next lyric line (wallpaper idle wait), capped at 1 s.
    DWORD UntilNextLine() const {
        if (!LyricsView() || !currentLyrics_->synced || !snapshot_.playing) return 1000;
        const int64_t ms = LyricTimeMs();
        for (const auto& line : currentLyrics_->lines) {
            const int64_t wait = line.ms - ms - (line.words.empty() ? 150 : 0);
            if (wait > 0) return DWORD(std::clamp<int64_t>(wait, 15, 1000));
        }
        return 1000;
    }

    void MaybeRequestArtwork(bool immediate = false) {
        if (!pendingRequest_ || !artwork_) return;
        if (snapshot_.thumbHash != requestThumbHash_) { requestThumbHash_ = snapshot_.thumbHash; thumbSince_ = Now(); }
        const double now = Now();
        const bool settled = snapshot_.thumb && now - thumbSince_ >= 1.2;
        if (!immediate && !settled && now - requestSince_ < 3.5) return;
        ArtworkRequest r = *pendingRequest_; pendingRequest_.reset();
        r.animatedArtwork = animatedArtwork_;
        if (snapshot_.TrackKey() == trackKey_ && snapshot_.end > 0) r.durationMs = int(snapshot_.end / 10000);
        if (snapshot_.thumb) { r.thumb = snapshot_.thumb; r.thumbW = snapshot_.thumbW; r.thumbH = snapshot_.thumbH; }
        artwork_->Request(r);
    }

    void OnArtwork(const ArtworkResult& r) {
        if (r.key != artKey_) return;
        const int have = cur_ ? cur_->quality : -1;
        if (!r.imagePath.empty() && have < 1 && !pendingVideo_) {
            auto a = std::make_shared<Art>(); a->still = scene_.LoadFile(r.imagePath); a->quality = 1; a->palette = scene_.PaletteOfFile(r.imagePath);
            if (a->still) Show(a);
        }
        if (animatedArtwork_ && !r.videoPath.empty() && have < 2 && !pendingVideo_) {
            auto a = std::make_shared<Art>(); a->quality = 2;
            a->still = cur_ ? cur_->still : nullptr;
            if (cur_) a->palette = cur_->palette;
            a->video = std::make_unique<VideoLayer>();
            if (a->video->Open(gpu_.device.Get(), gpu_.mfManager.Get(), scene_.Dc(), r.videoPath)) pendingVideo_ = a;
            else Log(L"motion artwork could not be opened");
        }
    }

    int Loop() {
        double last = Now();
        MSG msg{};
        for (;;) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return int(msg.wParam);
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            const double now = Now(); const float dt = float(std::min(.1, now - last)); last = now;
            bool frame = false;
            if (pendingVideo_) {
                if (pendingVideo_->video->Failed()) { pendingVideo_->video->LogState(L"failed"); pendingVideo_.reset(); }
                else if (pendingVideo_->video->Update() && pendingVideo_->video->HasFrame()) { Show(pendingVideo_); pendingVideo_.reset(); frame = true; }
            }
            if (cur_ && cur_->video && cur_->video->Update()) frame = true;
            if (prev_ && prev_->video && prev_->video->Update()) frame = true;
            if (cur_ && cur_->video) cur_->video->SetPlaying(VideoShouldPlay());
            VideoPalette();
            if (wallpaper_) {
                // Shown while Apple Music plays (a short grace period covers track changes), faded out otherwise.
                if (snapshot_.has && snapshot_.playing) notPlayingSince_ = -1; else if (notPlayingSince_ < 0) notPlayingSince_ = now;
                const bool hasArt = cur_ && cur_->Image();
                // Settings > Music > Keep artwork when paused: a paused player keeps it (closing the player still hides it).
                const bool want = closing_ < 0 && hasArt && (forceShow_ || notPlayingSince_ < 0 || now - notPlayingSince_ < 2.5 || (keepPaused_ && snapshot_.has));
                if (want && !shown_) { ShowWindow(hwnd_, SW_SHOWNA); shown_ = true; dirty_ = true; }
                const float before = wallAlpha_, target = want ? 1.f : 0.f;
                wallAlpha_ = Approach(wallAlpha_, target, dt, .2f);
                if (std::abs(wallAlpha_ - target) < .004f) wallAlpha_ = target;
                const float mix = prev_ ? float((now - fadeStart_) / .9) : 1.f;
                const bool finishedTransition = prev_ && mix >= 1;
                if (finishedTransition) prev_.reset();
                if (!want && wallAlpha_ == 0 && shown_) { wallpaperFrames_.Clear(); Render(1, 0); ShowWindow(hwnd_, SW_HIDE); shown_ = false; }
                if (closing_ >= 0 && wallAlpha_ == 0) { DestroyWindow(hwnd_); continue; }
                const bool lyricsMoving = shown_ && !covered_ && StepLyrics(dt);
                UpdateRawInput();
                bool beatFrame = false;
                UpdateBeat();
                if (shown_ && !covered_ && LyricsView() && snapshot_.playing && (musicFps_ <= 0 || now - lastBeatFrame_ >= 1.0 / musicFps_)) {
                    beatFrame = true; lastBeatFrame_ = now;
                }
                const bool anim = wallAlpha_ != before || prev_ || finishedTransition || lyricsMoving || beatFrame;
                if (shown_ && (dirty_ || anim || frame)) { Render(prev_ ? std::min(1.f, mix) : 1.f, wallAlpha_); dirty_ = false; }
                else if (shown_ && ((cur_ && cur_->video && VideoShouldPlay()) || pendingVideo_)) docky::vblank::WaitFrame(hwnd_);
                else if (shown_ && !covered_ && LyricsView() && snapshot_.playing && musicFps_ <= 0) docky::vblank::WaitFrame(hwnd_);
                else if (shown_ && !covered_ && LyricsView() && snapshot_.playing)
                    WaitForMusicFrame(lastBeatFrame_ + 1.0 / musicFps_);
                else MsgWaitForMultipleObjects(0, nullptr, FALSE, shown_ && !covered_ ? UntilNextLine() : 1000, QS_ALLINPUT);
                continue;
            }
            // Controls: shown while the cursor moves or rests on the platter, then fade away.
            if (hideNow_) { controlsTarget_ = 0; hideNow_ = false; lastMove_ = now - 10; }
            else controlsTarget_ = (now - lastMove_ < 3.2 || pressed_ != Scene::None || (scene_.InPanel(mouseX_, mouseY_) && now - lastMove_ < 8)) ? 1.f : 0.f;
            const float before = controls_;
            controls_ = Approach(controls_, controlsTarget_, dt, controlsTarget_ > controls_ ? .09f : .22f);
            if (std::abs(controls_ - controlsTarget_) < .003f) controls_ = controlsTarget_;
            const bool controlsAnim = controls_ != before;
            if (controls_ == 0 && before != 0) SetCursor(nullptr);
            const float mix = prev_ ? float((now - fadeStart_) / .7) : 1.f;
            if (prev_ && mix >= 1) prev_.reset();
            const float appear = float(std::min(1.0, (now - opened_) / .42));
            const float closeT = closing_ >= 0 ? float((now - closing_) / .26) : 0.f;
            if (closeT >= 1) { DestroyWindow(hwnd_); continue; }
            // Lyrics: the scroll follows the line being sung with a short ease; the backdrop drifts while the view is up.
            bool lyricsAnim = StepLyrics(dt);
            UpdateBeat();
            if (LyricsView() && snapshot_.playing) lyricsAnim = true;
            if (now < toastUntil_ + .1) lyricsAnim = true;
            if (LyricsView() && !lyricsFullArt_) lyricsAnim = true;  // the card layout's backdrop drifts
            const bool anim = controlsAnim || prev_ || appear < 1 || closing_ >= 0 || lyricsAnim;
            if (!dirty_ && !frame && anim && musicFps_ > 0 && now - lastFrame_ < 1.0 / musicFps_) {
                WaitForMusicFrame(lastFrame_ + 1.0 / musicFps_);
            } else if (dirty_ || anim || frame) {
                lastFrame_ = now;
                Render(prev_ ? std::min(1.f, mix) : 1.f, closing_ >= 0 ? 1 - closeT : appear);
                dirty_ = false;
            } else if ((cur_ && cur_->video && snapshot_.playing) || pendingVideo_) {
                docky::vblank::WaitFrame(hwnd_);  // a motion frame may be due by the next composition
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 1000, QS_ALLINPUT);
            }
        }
    }

    void PublishWallpaperFrame(float appear, float mix) {
        using namespace docky::music_wallpaper;
        const ULONGLONG now = GetTickCount64();
        const bool settled = appear >= 1.f && mix >= 1.f;
        // While the artwork fades or crossfades the widgets follow at ~15 frames per second; a still picture is
        // re-sent at most every 200 ms (motion artwork) and the settled frame goes out at once.
        if (now - lastWallpaperFrame_ < (settled ? 200u : 66u) && (!settled || wallpaperFrameSettled_)) return;
        lastWallpaperFrame_ = now;
        auto* dc = scene_.Dc();
        constexpr auto format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const auto pixelFormat = D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED);
        const auto size = D2D1::SizeU(Width, Height);
        if (!wallpaperPreview_ || !wallpaperReadback_) {
            wallpaperPreview_.Reset();
            wallpaperReadback_.Reset();
            if (FAILED(dc->CreateBitmap(size, nullptr, 0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixelFormat), &wallpaperPreview_)) ||
                FAILED(dc->CreateBitmap(size, nullptr, 0, D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pixelFormat), &wallpaperReadback_)))
                return;
        }
        dc->SetTarget(wallpaperPreview_.Get());
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0));
        const auto bounds = D2D1::RectF(0, 0, float(Width), float(Height));
        dc->DrawBitmap(target_.Get(), bounds, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        const HRESULT result = dc->EndDraw();
        dc->SetTarget(target_.Get());
        if (FAILED(result) || FAILED(wallpaperReadback_->CopyFromBitmap(nullptr, wallpaperPreview_.Get(), nullptr))) return;
        D2D1_MAPPED_RECT data{};
        if (FAILED(wallpaperReadback_->Map(D2D1_MAP_OPTIONS_READ, &data))) return;
        RECT screen{};
        if (GetWindowRect(hwnd_, &screen)) {
            wallpaperFrames_.Publish(hwnd_, screen, data.bits, data.pitch);
            wallpaperFrameSettled_ = settled;
        }
        wallpaperReadback_->Unmap();
    }

    void Render(float mix, float appear) {
        if (!target_) return;
        Scene::Ui ui; ui.media = &snapshot_; ui.extend = config_.extend; ui.clock = config_.clock;
        ui.controls = controls_; ui.appear = appear; ui.volumeOk = volume_.Ok(); ui.volume = volumeLevel_; ui.muted = muted_;
        ui.hover = hover_; ui.pressed = pressed_; ui.seekPreview = seekPreview_;
        ui.lyricsAvailable = !wallpaper_ && currentLyrics_ && !currentLyrics_->Empty();
        ui.lyricsView = LyricsView(); ui.lyrics = currentLyrics_.get(); ui.lyricsPos = std::max(0.f, lyricsPos_); ui.lyricsActive = lyricsActive_;
        ui.lyricTimeMs = LyricTimeMs();
        ui.lyricsFullArt = lyricsFullArt_; ui.lyricsBlend = lyricsBlend_; ui.lyricsManual = lyricsManualBlend_;
        const double consumed = Now();
        musicTraceEvent_ = {};
        ui.beat = SampleMusic(consumed);
        ui.flowTime = flowClock_;
        if (!wallpaper_ && Now() < toastUntil_) ui.toast = toast_;
        ui.time = wallpaper_ ? 0.f : float(Now() - opened_);  // no backdrop drift on the desktop (no frame every refresh)
        if (ui.lyricsView) ui.clock = false;
        auto* dc = scene_.Dc();
        // Liquid silk for the lyrics side, rendered before Direct2D starts (half resolution of the widest screen).
        if (ui.lyricsView && lyricsBeat_ && cur_ && !cur_->palette.empty() && silk_.Ready()) {
            UINT sw = W_, sh = H_;
            if (wallpaper_) { sw = sh = 0; for (const auto& m : monitors_) { sw = std::max<UINT>(sw, UINT(m.screen.right - m.screen.left)); sh = std::max<UINT>(sh, UINT(m.screen.bottom - m.screen.top)); } }
            ui.silk = silk_.Render(dc, std::max<UINT>(64, UINT(sw * .56f / 2)), std::max<UINT>(64, sh / 2), flowClock_, std::min(1.f, musicTwist_.value), cur_->palette);
        }
        dc->SetTarget(target_.Get()); dc->BeginDraw();
        if (wallpaper_) {
            ui.wallpaper = true; ui.clock = false; ui.controls = 0;
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            deskLyricRects_.clear(); deskLyricsColumns_.clear();
            for (const Monitor& mon : monitors_) {
                const RECT& m = mon.screen;
                ui.safe = avoidBars_ ? D2D1::RectF(float(mon.work.left), float(mon.work.top), float(mon.work.right), float(mon.work.bottom)) : D2D1_RECT_F{};
                scene_.Draw(float(m.right - m.left), float(m.bottom - m.top), prev_.get(), cur_.get(), mix, ui, D2D1::Point2F(float(m.left), float(m.top)));
                dc->Flush();  // the effect graph is re-targeted for the next monitor
                for (auto [index, r] : scene_.LyricRects()) deskLyricRects_.push_back({index, D2D1::RectF(r.left + m.left, r.top + m.top, r.right + m.left, r.bottom + m.top)});
                if (const auto c = scene_.LyricsColumnRect(); c.right > c.left) deskLyricsColumns_.push_back(D2D1::RectF(c.left + m.left, c.top + m.top, c.right + m.left, c.bottom + m.top));
            }
        } else {
            scene_.Draw(float(W_), float(H_), prev_.get(), cur_.get(), mix, ui);
        }
        const HRESULT hr = dc->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) { Log(L"device lost"); PostQuitMessage(2); return; }
        if (FAILED(hr)) return;
        if (wallpaper_ && appear > 0.f) PublishWallpaperFrame(appear, mix);
        if (floatingScene_ && !colors_.Resolve(outputRtv_.Get(), scRgb_ ? outputWhite_ : 0.f, outputRegions_)) return;
        const double submitted = musicTraceEvent_.published ? Now() : 0;
        const HRESULT present = swap_->Present(1, 0);
        if (submitted) {
            const auto& event = musicTraceEvent_;
            Log(L"music timing kind=%u audio=%.6f sample_end=%.6f capture=%.6f analysis=%.6f publish=%.6f consume=%.6f present_call=%.6f present_return=%.6f hr=0x%08lx overruns=%u",
                unsigned(event.kind), event.at, event.sampleEnd, event.captured, event.analyzed, event.published, consumed,
                submitted, Now(), static_cast<unsigned long>(present), beat_.Overruns());
        }
    }

    static bool SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, UINT w, UINT h, const std::wstring& path);

    Config config_; Gpu gpu_; Scene scene_;
    HWND hwnd_ = nullptr; UINT W_ = 0, H_ = 0;
    ComPtr<IDXGISwapChain1> swap_; ComPtr<IDCompositionDevice> dcomp_; ComPtr<IDCompositionTarget> dtarget_; ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID2D1Bitmap1> target_;
    docky::color::ColorOutput colors_;
    ComPtr<ID3D11RenderTargetView> outputRtv_;
    std::vector<docky::color::ColorOutput::Region> outputRegions_;
    float outputWhite_ = 1.f;
    bool floatingScene_ = false, scRgb_ = false;
    std::unique_ptr<MediaWorker> media_; std::unique_ptr<ArtworkSource> artwork_;
    Volume volume_; float volumeLevel_ = volume_.Get(); bool muted_ = volume_.Muted();
    Media snapshot_; std::wstring trackKey_ = L"\x1", artKey_; uint64_t thumbHash_ = 0;
    std::shared_ptr<Art> cur_, prev_, pendingVideo_;
    double fadeStart_ = 0, opened_ = 0, closing_ = -1, lastMove_ = 0;
    float controls_ = 1, controlsTarget_ = 1, mouseX_ = -1, mouseY_ = -1, seekPreview_ = -1;
    int hover_ = 0, pressed_ = 0, minute_ = -1; bool dirty_ = true, hideNow_ = false;
    // wallpaper mode
    bool wallpaper_ = false, shown_ = false, covered_ = false, forceShow_ = false;
    struct Monitor { RECT screen, work; HMONITOR handle; };  // screen: in the window; work: in the monitor
    docky::music_wallpaper::Channel wallpaperFrames_;
    ComPtr<ID2D1Bitmap1> wallpaperPreview_, wallpaperReadback_;
    ULONGLONG lastWallpaperFrame_ = 0;
    bool wallpaperFrameSettled_ = false;
    HWND host_ = nullptr, ctl_ = nullptr; std::vector<Monitor> monitors_;
    bool avoidBars_ = true; liquid::settings::Revision settingsRevision_{};
    std::unique_ptr<LyricsSource> lyrics_; std::shared_ptr<lyrics::Lyrics> currentLyrics_;
    bool lyricsPending_ = false; double lyricsSince_ = 0; float lyricsPos_ = -1, lyricsBlend_ = 0; int lyricsActive_ = -1;
    bool lyricsFullArt_ = true, wallpaperLyrics_ = true;
    std::wstring lyricsRequestTag_;
    int lyricsRequestedDuration_ = 0;
    int64_t rawPosition_ = -1, rawUpdated_ = -1, anchorPosition_ = 0;
    double anchorClock_ = 0, clockRate_ = 1, seekDeadline_ = 0;
    bool clockPlaying_ = false, clockValid_ = false;
    // Lyrics scrolling (full-screen view): wheel or drag; returns to the sung line after a pause when lyricsAutoReturn_.
    bool lyricsManual_ = false, lyricsPress_ = false, lyricsDragged_ = false, lyricsAutoReturn_ = true;
    float lyricsManualPos_ = 0, lyricsPressPos_ = 0, lyricsPressX_ = 0, lyricsPressY_ = 0, lyricsManualBlend_ = 0; double lyricsScrolled_ = 0;
    std::vector<std::pair<int, D2D1_RECT_F>> deskLyricRects_; std::vector<D2D1_RECT_F> deskLyricsColumns_;
    SilkRenderer silk_;
    HANDLE musicFrameTimer_ = nullptr;
    int musicFps_ = 60;
    double lastFrame_ = 0;
    AudioBeat beat_;
    MusicSync musicSync_;
    MusicResponse musicResponse_ = MusicResponse::Balanced;
    std::array<RhythmEvent, static_cast<size_t>(RhythmKind::Count)> musicImpulses_{};
    MusicEnvelope musicLevel_, musicTwist_;
    RhythmEvent musicTraceEvent_{};
    const bool musicTrace_ = GetEnvironmentVariableW(L"DOCKY_MUSIC_TIMING", nullptr, 0) != 0;
    double lastMusicSample_ = 0, musicResetAt_ = 0;
    float flowClock_ = 0;
    bool lyricsBeat_ = true;
    int lyricsOffsetMs_ = 300, trackLyricOffsetMs_ = 0;
    double lastBeatFrame_ = 0, toastUntil_ = 0;
    std::wstring toast_;
    bool animatedArtwork_ = true, lyricsDoubleClickSeek_ = true, keepPaused_ = false, localTags_ = true;
    int lastLyricClick_ = -1;
    ULONGLONG lastLyricClickTime_ = 0;
    POINT lastLyricClickPoint_{};
    bool deskPress_ = false, rawInput_ = false; POINT deskPressPoint_{}; double deskPressTime_ = 0;
    std::optional<ArtworkRequest> pendingRequest_; double requestSince_ = 0, thumbSince_ = 0; uint64_t requestThumbHash_ = 0;
    float wallAlpha_ = 0; double notPlayingSince_ = -1;
};

bool App::SavePng(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, UINT w, UINT h, const std::wstring& path) {
    ComPtr<ID2D1Bitmap1> cpu;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, target, nullptr))) return false;
    D2D1_MAPPED_RECT map{}; if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &map))) return false;
    ComPtr<IWICImagingFactory> wic; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WritePixels(h, map.pitch, map.pitch * h, map.bits)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    cpu->Unmap();
    return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    if (docky::suite::Active() && docky::suite::StandaloneMusicRuntime(docky::suite::ProcessImage(GetCurrentProcessId()))) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto arg = [&](const wchar_t* name) -> std::wstring { for (int i = 1; i + 1 < argc; ++i) if (!wcscmp(argv[i], name)) return argv[i + 1]; return {}; };
    auto flag = [&](const wchar_t* name) { for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], name)) return true; return false; };
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    int code = 0;
    if (!arg(L"--snapshot").empty()) {
        UINT w = 1920, h = 1080; const auto size = arg(L"--size"); if (!size.empty()) swscanf_s(size.c_str(), L"%ux%u", &w, &h);
        const auto time = arg(L"--lyrics-time-ms");
        const auto ms = time.empty() ? 83000 : std::clamp<int64_t>(_wtoi64(time.c_str()), 0, INT_MAX);
        App app; code = app.Snapshot(arg(L"--snapshot"), arg(L"--image"), arg(L"--video"), w, h, !flag(L"--fill"),
            !flag(L"--no-clock"), arg(L"--lyrics"), flag(L"--card"), ms);
    } else if (flag(L"--lyrics-fetch") && argc >= 4) {  // --lyrics-fetch "Artist" "Title" ["Album"] [--duration seconds]
        int i = 1; while (wcscmp(argv[i], L"--lyrics-fetch")) ++i;
        LyricsRequest r{argv[i + 2], argv[i + 1], i + 3 < argc && argv[i + 3][0] != L'-' ? argv[i + 3] : L"", _wtoi(arg(L"--duration").c_str()) * 1000, true, L"diag"};
        r.appleCatalog = flag(L"--apple-catalog");
        LyricsSource source([](const LyricsResult&) {});
        const auto result = source.RunNow(r);
        Log(L"lyrics-fetch: %zu lines, synced=%d, word-timed=%d, source=%ls", result.lyrics.lines.size(),
            result.lyrics.synced, result.lyrics.WordTimed(), result.lyrics.source.c_str());
        for (size_t k = 0; k < result.lyrics.lines.size() && k < 4; ++k) Log(L"  [%d] %ls", result.lyrics.lines[k].ms, result.lyrics.lines[k].text.c_str());
    } else if (flag(L"--wallpaper")) {
        HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DockyNowPlayingWallpaper");
        if (GetLastError() != ERROR_ALREADY_EXISTS) { App app; code = app.RunWallpaper(inst, arg(L"--test-image"), arg(L"--lyrics")); }
        if (single) CloseHandle(single);
    } else if (flag(L"--fetch") && argc >= 4) {
        int i = 1; while (wcscmp(argv[i], L"--fetch")) ++i;
        ArtworkRequest r{i + 3 < argc ? argv[i + 3] : L"", argv[i + 1], i + 2 < argc ? argv[i + 2] : L""};
        if (const auto thumb = arg(L"--thumb"); !thumb.empty()) {  // stands in for the player's cover
            ComPtr<IWICImagingFactory> wic; ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> conv; UINT w = 0, h = 0;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateDecoderFromFilename(thumb.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
                SUCCEEDED(wic->CreateFormatConverter(&conv)) && SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(conv->GetSize(&w, &h))) {
                auto px = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
                if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px->size()), px->data()))) { r.thumb = px; r.thumbW = w; r.thumbH = h; }
            }
        }
        ArtworkSource source([](const ArtworkResult& res) { Log(L"fetch result key=%ls image=%ls video=%ls", res.key.c_str(), res.imagePath.c_str(), res.videoPath.c_str()); });
        source.RunNow(r);
    } else {
        HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DockyNowPlaying");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // A second launch toggles: close the open window.
            if (HWND other = FindWindowW(kClass, nullptr)) PostMessageW(other, WM_CLOSE, 0, 0);
        } else {
            App app; code = app.Run(inst);
        }
        if (single) CloseHandle(single);
    }
    MFShutdown();
    CoUninitialize();
    LocalFree(argv);
    return code;
}
