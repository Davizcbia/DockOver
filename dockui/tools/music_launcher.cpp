#include "desktop_settings.h"
#include "suite_install.h"
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <string>

namespace {
namespace settings = liquid::settings;
constexpr wchar_t kClass[] = L"DockOverMusicTray";
constexpr wchar_t kWallpaperMutex[] = L"Local\\DockyNowPlayingWallpaper";
constexpr UINT kTray = WM_APP + 1;
constexpr UINT kShowMenu = WM_APP + 2;
enum Command { NowPlaying = 1, Wallpaper, Lyrics, Settings, Exit };
constexpr std::array shortcuts{Lyrics, Wallpaper, NowPlaying};

bool Exists(const wchar_t* name) {
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!handle) return false;
    CloseHandle(handle);
    return true;
}

bool ManagerRunning() { return Exists(L"Local\\LiquidDesktopManagerV05SingleInstance"); }

std::wstring Sibling(const wchar_t* name) {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    path.resize(separator + 1);
    return path + name;
}

HANDLE Launch(const wchar_t* name, const wchar_t* args = L"") {
    const auto path = Sibling(name);
    std::wstring command = L"\"" + path + L"\" " + args;
    STARTUPINFOW start{sizeof(start)};
    PROCESS_INFORMATION proc{};
    if (path.empty() || !CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                                      0, nullptr, nullptr, &start, &proc)) return nullptr;
    CloseHandle(proc.hThread);
    return proc.hProcess;
}

struct Tray {
    HWND window = nullptr;
    HANDLE ownedWallpaper = nullptr;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    NOTIFYICONDATAW icon{};
    std::wstring path = settings::DefaultPath();
    std::array<int, shortcuts.size()> hotkeys{};

    bool Chinese() const {
        const auto loaded = settings::Load(path);
        return loaded.value.appearance.language == "zh-CN" ||
               (loaded.status != settings::LoadStatus::Loaded &&
                PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
    }

    void AddIcon() {
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon.uCallbackMessage = kTray;
        icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wcscpy_s(icon.szTip, L"DockOver Music Player");
        Shell_NotifyIconW(NIM_ADD, &icon);
    }

    void StopWallpaper() {
        if (!ownedWallpaper) return;
        const auto control = FindWindowExW(HWND_MESSAGE, nullptr, L"DockyNowPlayingWallpaperCtl", nullptr);
        DWORD pid = 0;
        if (control && GetWindowThreadProcessId(control, &pid) && pid == GetProcessId(ownedWallpaper)) {
            PostMessageW(control, WM_CLOSE, 0, 0);
        }
    }

    void Sync() {
        if (ownedWallpaper && WaitForSingleObject(ownedWallpaper, 0) == WAIT_OBJECT_0) {
            CloseHandle(ownedWallpaper);
            ownedWallpaper = nullptr;
        }
        const auto loaded = settings::Load(path);
        if (loaded.status != settings::LoadStatus::Loaded && loaded.status != settings::LoadStatus::Missing) return;
        const auto& preferences = loaded.value.controlCenter;
        const bool managed = ManagerRunning();
        const std::array wanted = managed ? std::array{0, 0, 0} :
            std::array{preferences.shortcutLyrics, preferences.shortcutArtwork, preferences.shortcutNowPlaying};
        for (size_t i = 0; i < hotkeys.size(); ++i) {
            if (wanted[i] == hotkeys[i]) continue;
            if (hotkeys[i]) UnregisterHotKey(window, shortcuts[i]);
            hotkeys[i] = 0;
            if (wanted[i] && RegisterHotKey(window, shortcuts[i], UINT(wanted[i] >> 8) | MOD_NOREPEAT,
                                            UINT(wanted[i] & 0xff))) hotkeys[i] = wanted[i];
        }
        if (managed) return;
        if (!preferences.musicWallpaper) {
            StopWallpaper();
        } else if (!ownedWallpaper && !Exists(kWallpaperMutex)) {
            ownedWallpaper = Launch(L"docky-now-playing.exe", L"--wallpaper");
        }
    }

    void Menu() {
        const bool zh = Chinese();
        const auto preferences = settings::Load(path).value.controlCenter;
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, NowPlaying, zh ? L"打开正在播放" : L"Open Now Playing");
        AppendMenuW(menu, MF_STRING | (preferences.musicWallpaper ? MF_CHECKED : 0),
                    Wallpaper, zh ? L"音乐壁纸" : L"Music wallpaper");
        AppendMenuW(menu, MF_STRING | (preferences.wallpaperLyrics ? MF_CHECKED : 0),
                    Lyrics, zh ? L"桌面歌词" : L"Desktop lyrics");
        AppendMenuW(menu, MF_STRING, Settings, zh ? L"音乐设置" : L"Music settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Exit, zh ? L"退出" : L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);
        Run(command);
    }

    void Run(UINT command) {
        HANDLE launched = nullptr;
        switch (command) {
        case NowPlaying: launched = Launch(L"docky-now-playing.exe"); break;
        case Settings: launched = Launch(L"LiquidDesktopSettings.exe", L"--music-only"); break;
        case Wallpaper:
        case Lyrics: {
            const auto preferences = settings::Load(path).value.controlCenter;
            const auto status = settings::WriteBool(path, "controlCenter",
                command == Wallpaper ? "musicWallpaper" : "wallpaperLyrics",
                command == Wallpaper ? !preferences.musicWallpaper : !preferences.wallpaperLyrics);
            if (status == settings::SaveStatus::Saved) Sync();
            else MessageBoxW(window, Chinese() ? L"无法保存音乐设置。" : L"Could not save music settings.",
                             L"DockOver Music Player", MB_OK | MB_ICONWARNING);
            break;
        }
        case Exit: DestroyWindow(window); break;
        }
        if (launched) CloseHandle(launched);
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* tray = static_cast<Tray*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tray));
        tray->window = window;
    }
    auto* tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!tray) return DefWindowProcW(window, message, w, l);
    if (message == tray->taskbarCreated) { tray->AddIcon(); return 0; }
    switch (message) {
    case WM_TIMER:
        if (docky::suite::Active()) DestroyWindow(window);
        else tray->Sync();
        return 0;
    case WM_HOTKEY: tray->Run(static_cast<UINT>(w)); return 0;
    case kTray:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP) tray->Menu();
        return 0;
    case kShowMenu: tray->Menu(); return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &tray->icon);
        tray->StopWallpaper();
        if (tray->ownedWallpaper) CloseHandle(tray->ownedWallpaper);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (docky::suite::Active()) {
        if (!docky::suite::Launch(docky::suite::InstalledRoot(), L"docky-now-playing.exe", L""))
            MessageBoxW(nullptr, L"Open Music Player from DockOver.\n请从 DockOver 打开音乐播放器。", L"DockOver", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DockOverMusicTray");
    if (!single) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto previous = FindWindowW(kClass, nullptr)) PostMessageW(previous, kShowMenu, 0, 0);
        CloseHandle(single);
        return 0;
    }
    Tray tray;
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"DockOver Music Player", WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance, &tray)) {
        CloseHandle(single);
        return 1;
    }
    tray.AddIcon();
    tray.Sync();
    SetTimer(tray.window, 1, 3000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    CloseHandle(single);
    return 0;
}
