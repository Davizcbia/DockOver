#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace docky::suite {
inline constexpr wchar_t kKey[] = L"Software\\DockOver";
inline constexpr wchar_t kRoot[] = L"SuiteRoot";

inline bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool ValidRoot(const std::wstring& root) {
    return !root.empty() && FileExists(root + L"\\DockOver.exe") &&
           FileExists(root + L"\\runtime\\LiquidDesktopManager.exe") &&
           FileExists(root + L"\\runtime\\docky-bubble.exe") &&
           FileExists(root + L"\\runtime\\docky-now-playing.exe");
}

inline std::wstring InstalledRoot() {
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, kRoot, RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS)
        return {};
    return ValidRoot(path) ? path : std::wstring{};
}

inline void Register(const std::wstring& root) {
    if (ValidRoot(root))
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, kRoot, REG_SZ, root.c_str(),
                        static_cast<DWORD>((root.size() + 1) * sizeof(wchar_t)));
}

inline std::wstring ProcessImage(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t image[32768]{};
    DWORD length = ARRAYSIZE(image);
    const bool good = QueryFullProcessImageNameW(process, 0, image, &length) != FALSE;
    CloseHandle(process);
    return good ? std::wstring(image, length) : std::wstring{};
}

inline bool StandaloneMusicRuntime(const std::wstring& image) {
    const auto slash = image.find_last_of(L"\\/");
    return slash != std::wstring::npos && FileExists(image.substr(0, slash + 1) + L"DockOverMusic.exe");
}

inline bool Active() {
    if (!InstalledRoot().empty()) return true;
    HANDLE manager = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LiquidDesktopManagerV05SingleInstance");
    if (!manager) manager = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\DockOverSuiteUpdateV1");
    if (!manager) return false;
    CloseHandle(manager);
    return true;
}

inline bool Launch(const std::wstring& root, const wchar_t* name, const std::wstring& arguments) {
    if (root.empty()) return false;
    const auto program = root + L"\\runtime";
    const auto exe = program + L"\\" + name;
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, program.c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

inline void StopStandaloneWindows() {
    const UINT gracefulQuit = RegisterWindowMessageW(L"DockOver.Update.Quit");
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        wchar_t cls[96]{};
        GetClassNameW(window, cls, ARRAYSIZE(cls));
        const std::wstring_view type(cls);
        const bool drop = type == L"DockDropPairing" || type == L"DockDropDevicePreview" ||
                          type == L"DockDropPcReceiveBubble" || type == L"DockDropPcSendBubble" ||
                          type == L"DockDropPcReceiveBanner";
        const bool music = type == L"DockOverMusicTray" || type == L"DockyNowPlaying" ||
                           type == L"DockOverMusicSettings";
        if (!drop && !music) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        const auto image = ProcessImage(pid);
        const auto slash = image.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return TRUE;
        const wchar_t* exe = image.c_str() + slash + 1;
        if (drop && _wcsicmp(exe, L"DockDrop.exe") == 0 && param) {
            PostMessageW(window, static_cast<UINT>(param), 0, 0);
        } else if (music && (_wcsicmp(exe, L"DockOverMusic.exe") == 0 ||
                            StandaloneMusicRuntime(image))) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, gracefulQuit);
    for (HWND window = nullptr; (window = FindWindowExW(HWND_MESSAGE, window, L"DockyNowPlayingWallpaperCtl", nullptr));) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (StandaloneMusicRuntime(ProcessImage(pid))) PostMessageW(window, WM_CLOSE, 0, 0);
    }
}
}  // namespace docky::suite
