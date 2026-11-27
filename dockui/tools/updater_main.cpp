// docky-updater: installs a downloaded docky portable zip into the program folder.
//   docky-updater.exe --zip <file> --dir <program folder> [--version <v>] [--wait-pid <pid>] [--reopen-settings]
// Always started from a temporary copy (see update_client.cpp StartInstall), never from the folder it replaces.
// 1. Extract the zip (Windows' built-in tar.exe) into a staging folder and check its manifest.txt.
// 2. Ask the running suite components covered by the manifest to quit, then wait for them to exit.
//    The Manager restores the Windows taskbar as part of its regular shutdown.
// 3. Move each listed file aside (*.update-old), copy the new one in; on any failure put every old file back.
// 4. Start docky and DockOver Desktop again if they were running, and Settings > About if asked. Nothing is ever force-closed.
#include "../src/suite_install.h"
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring g_log;

void Log(const wchar_t* fmt, ...) {
    if (g_log.empty()) return;
    wchar_t line[2048];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    n += vswprintf_s(line + n, 2048 - n - 2, fmt, ap);
    va_end(ap);
    line[n++] = L'\r'; line[n++] = L'\n';
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, n, utf8.data(), bytes, nullptr, nullptr);
    HANDLE f = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &w, nullptr);
    CloseHandle(f);
}

std::wstring Lower(std::wstring s) { CharLowerBuffW(s.data(), static_cast<DWORD>(s.size())); return s; }

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    std::wstring s = (n && n < MAX_PATH * 4) ? std::wstring(buf, n) : p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void EnsureParent(const std::wstring& file) {
    for (size_t i = file.find(L'\\', 3); i != std::wstring::npos; i = file.find(L'\\', i + 1))
        CreateDirectoryW(file.substr(0, i).c_str(), nullptr);
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

struct Process {
    DWORD pid;
    std::wstring relativePath;
    std::wstring name;
};

std::vector<Process> RunningIn(const std::wstring& dir, const std::vector<std::wstring>& executables) {
    std::vector<Process> out;
    const std::wstring prefix = Lower(dir) + L"\\";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[32768]{}; DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            const auto image = Lower(std::wstring(path, n));
            if (image.starts_with(prefix)) {
                const auto relative = image.substr(prefix.size());
                if (std::find(executables.begin(), executables.end(), relative) != executables.end())
                    out.push_back({pe.th32ProcessID, relative, Lower(pe.szExeFile)});
            }
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
}

struct CloseRequest {
    const std::vector<Process>& running;
    bool managerOwnsCore;
    bool remember;
    const UINT dockDropQuit = RegisterWindowMessageW(L"DockOver.Update.Quit");
    std::wstring dockDropArgs;
    HWND nowPlaying = nullptr;
};

BOOL CALLBACK CloseWindow(HWND w, LPARAM l) {
    auto& c = *reinterpret_cast<CloseRequest*>(l);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    const auto p = std::find_if(c.running.begin(), c.running.end(), [pid](const Process& entry) { return entry.pid == pid; });
    if (p == c.running.end()) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (p->name == L"docky-bubble.exe") {
        if (c.remember && c.dockDropArgs != L"--pair-phone") {
            if (wcscmp(cls, L"DockyPhonePairing") == 0 && IsWindowVisible(w)) c.dockDropArgs = L"--pair-phone";
            else if (wcscmp(cls, L"DockDropDevicePreview") == 0 && IsWindowVisible(w)) c.dockDropArgs = L"--discover";
            else if (wcscmp(cls, L"DockyPhonePairing") == 0 || wcscmp(cls, L"DockDropDevicePreview") == 0) {
                if (c.dockDropArgs.empty()) c.dockDropArgs = L"--background";
            }
        }
        if (c.dockDropQuit) PostMessageW(w, c.dockDropQuit, 0, 0);
    } else if (p->name == L"docky-now-playing.exe") {
        if (wcscmp(cls, L"DockyNowPlaying") == 0) {
            c.nowPlaying = w;
            PostMessageW(w, WM_CLOSE, 0, 0);
        }
    } else if (wcscmp(cls, L"DockyDesktopHostV1") == 0 ||
               wcscmp(cls, L"DockyExtCliListener") == 0 ||
               (p->name == L"docky-blackhole.exe" && !GetWindow(w, GW_OWNER)) ||
               (p->name == L"dockoverapplications.exe" && !GetWindow(w, GW_OWNER)) ||
               (!c.managerOwnsCore &&
                (wcscmp(cls, L"LiquidDockV01") == 0 || wcscmp(cls, L"LiquidDesktopMenuBarV01") == 0 ||
                 wcscmp(cls, L"LiquidDesktopSettingsV01") == 0))) {
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

void RequestAuxiliaryExit(CloseRequest& request) {
    EnumWindows(CloseWindow, reinterpret_cast<LPARAM>(&request));
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"DockyNowPlayingWallpaperCtl", nullptr));) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (std::any_of(request.running.begin(), request.running.end(), [pid](const Process& p) {
                return p.pid == pid && p.name == L"docky-now-playing.exe";
            }))
            PostMessageW(w, WM_CLOSE, 0, 0);
    }
    for (const auto& p : request.running) {
        if (p.name != L"dockdroppc.exe") continue;
        const auto event = L"Local\\DockOver.Update.DockDropPc." + std::to_wstring(p.pid);
        if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str())) {
            SetEvent(quit);
            CloseHandle(quit);
        }
    }
}

bool Start(const std::wstring& exe, const std::wstring& args, const std::wstring& dir, DWORD flags) {
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi)) {
        Log(L"start failed: %ls (error %lu)", exe.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

bool Extract(const std::wstring& zip, const std::wstring& to) {
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (!Exists(tar)) { Log(L"tar.exe not found in %ls", sys); return false; }
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + to + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, to.c_str(), &si, &pi)) {
        Log(L"tar could not start (error %lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 180000) != WAIT_OBJECT_0) { Log(L"tar timed out"); CloseHandle(pi.hProcess); return false; }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code) Log(L"tar exit code %lu", code);
    return code == 0;
}

// manifest.txt: one relative path per line ("/" separators). Rejects anything that leaves the folder.
bool ReadManifest(const std::wstring& path, std::vector<std::wstring>& files) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string text(1024 * 1024, '\0'); DWORD got = 0;
    ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) { if (end == text.size()) break; continue; }
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (n <= 0) return false;
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), w.data(), n);
        for (auto& c : w) if (c == L'/') c = L'\\';
        if (w[0] == L'\\' || w.find(L':') != std::wstring::npos || (L"\\" + w + L"\\").find(L"\\..\\") != std::wstring::npos) return false;
        files.push_back(w);
        if (end == text.size()) break;
    }
    return !files.empty();
}

struct Replaced { std::wstring target, old; bool hadOld; };

void Rollback(std::vector<Replaced>& done) {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        SetFileAttributesW(it->target.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(it->target.c_str());
        if (it->hadOld && !MoveFileExW(it->old.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING))
            Log(L"ROLLBACK FAILED for %ls (error %lu); the previous file is at %ls", it->target.c_str(), GetLastError(), it->old.c_str());
    }
    done.clear();
}

int Fail(const std::wstring& message) {
    Log(L"FAILED: %ls", message.c_str());
    MessageBoxW(nullptr, (message + L"\n\nDockOver was not changed. Details: " + g_log).c_str(), L"DockOver Update", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring zip, dir, version;
    DWORD waitPid = 0;
    bool reopenSettings = false;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--zip" && i + 1 < argc) zip = argv[++i];
        else if (a == L"--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == L"--version" && i + 1 < argc) version = argv[++i];
        else if (a == L"--wait-pid" && i + 1 < argc) waitPid = wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--reopen-settings") reopenSettings = true;
    }
    if (argv) LocalFree(argv);
    {
        wchar_t buf[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
            const std::wstring base = std::wstring(buf) + L"\\LiquidDesktop";
            CreateDirectoryW(base.c_str(), nullptr);
            CreateDirectoryW((base + L"\\updates").c_str(), nullptr);
            g_log = base + L"\\updates\\updater.log";
        }
    }
    if (zip.empty() || dir.empty()) {
        MessageBoxW(nullptr, L"docky-updater is started by DockOver Settings when an update is installed.", L"DockOver Update", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    zip = FullPath(zip); dir = FullPath(dir);
    Log(L"---- update to %ls: zip=%ls dir=%ls", version.c_str(), zip.c_str(), dir.c_str());
    // Both the automatic check and Settings can launch a temporary installer.
    HANDLE install = CreateMutexW(nullptr, TRUE, L"Local\\DockOverSuiteUpdateV1");
    if (!install) return Fail(L"Could not acquire the update lock.");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(install);
        Log(L"another DockOver update is already running");
        return 2;
    }
    struct InstallLock { HANDLE handle; ~InstallLock() { CloseHandle(handle); } } lock{install};
    if (!Exists(zip)) return Fail(L"The downloaded update is missing.");
    // Release folders keep the programs in runtime\ (DockOver.exe and DockOver Desktop.exe beside it); development
    // folders (build-borderless) have them at the top.
    const bool releaseLayout = Exists(dir + L"\\runtime\\LiquidDesktopManager.exe");
    const std::wstring programs = releaseLayout ? dir + L"\\runtime" : dir;
    if (!Exists(programs + L"\\LiquidDesktopManager.exe")) return Fail(L"The DockOver program folder was not found: " + dir);

    // 1. Extract and check.
    const std::wstring stage = zip + L".stage-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(stage.c_str(), nullptr)) return Fail(L"Could not create a staging folder.");
    if (!Extract(zip, stage)) { RemoveTree(stage); return Fail(L"The update package could not be extracted."); }
    std::vector<std::wstring> files;
    if (!ReadManifest(stage + L"\\manifest.txt", files)) { RemoveTree(stage); return Fail(L"The update package has no valid manifest."); }
    std::vector<std::wstring> entries, executables;
    for (const auto& f : files) {
        const auto name = Lower(f);
        const DWORD attr = GetFileAttributesW((stage + L"\\" + f).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            std::find(entries.begin(), entries.end(), name) != entries.end()) {
            RemoveTree(stage);
            return Fail(L"The update package contains a missing, invalid or duplicate file: " + f);
        }
        entries.push_back(name);
        if (name.ends_with(L".exe")) executables.push_back(name);
    }
    const auto contains = [&](const std::wstring& entry) {
        return std::find(entries.begin(), entries.end(), entry) != entries.end();
    };
    const bool packageRelease = contains(L"runtime\\liquiddesktopmanager.exe");
    if (packageRelease != releaseLayout) {
        RemoveTree(stage);
        return Fail(releaseLayout ? L"This update package does not fit this DockOver folder."
                                  : L"This DockOver folder is a development copy. Release packages install into a folder downloaded from GitHub.");
    }
    const std::wstring prefix = releaseLayout ? L"runtime\\" : L"";
    for (const auto* name : {L"liquiddesktopmanager.exe", L"liquiddock.exe", L"liquidmenubar.exe",
                             L"liquiddesktopsettings.exe", L"docky-updater.exe", L"docky-bubble.exe",
                             L"dockdrop-pc\\dockdroppc.exe", L"dockdrop-pc\\dockdroppc.dll",
                             L"dockdrop-pc\\dockdroppc.deps.json", L"dockdrop-pc\\dockdroppc.runtimeconfig.json",
                             L"docky-now-playing.exe", L"applications-menu\\dockoverapplications.exe"}) {
        if (!contains(prefix + name)) {
            RemoveTree(stage);
            return Fail(L"The update package is missing " + prefix + name + L".");
        }
    }
    if (!contains(L"manifest.txt") ||
        (releaseLayout && (!contains(L"dockover.exe") || !contains(L"dockover desktop.exe")))) {
        RemoveTree(stage);
        return Fail(L"The update package is missing its manifest or suite launchers.");
    }
    Log(L"staged %zu files", files.size());

    // 2. Close the exact copies this update will replace. The Manager handles taskbar recovery and its children.
    if (waitPid) {
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) {
            WaitForSingleObject(p, 2000);
            CloseHandle(p);
        }
    }
    auto running = RunningIn(dir, executables);
    const std::wstring managerName = prefix + L"liquiddesktopmanager.exe";
    const std::wstring desktopName = releaseLayout ? L"dockover desktop.exe" : L"dockoverdesktop.exe";
    const auto isRunning = [&](const std::wstring& relative) {
        return std::any_of(running.begin(), running.end(), [&](const Process& p) { return p.relativePath == relative; });
    };
    const bool wasRunning = isRunning(managerName);
    const bool desktopWasRunning = isRunning(desktopName);
    CloseRequest request{running, wasRunning, true};
    if (wasRunning) {
        if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\LiquidDesktopManagerQuitV1")) {
            SetEvent(quit);
            CloseHandle(quit);
            Log(L"asked DockOver to quit");
        } else Log(L"DockOver quit event unavailable (error %lu)", GetLastError());
    }
    const auto restart = [&] {
        if (wasRunning && !isRunning(managerName)) Start(programs + L"\\LiquidDesktopManager.exe", L"", programs, CREATE_NO_WINDOW);
        if (desktopWasRunning && !isRunning(desktopName)) Start(dir + L"\\" + desktopName, L"", dir, 0);
        if (!request.dockDropArgs.empty() && !isRunning(prefix + L"docky-bubble.exe"))
            Start(programs + L"\\docky-bubble.exe", request.dockDropArgs, programs, 0);
        if (request.nowPlaying && !IsWindow(request.nowPlaying))
            Start(programs + L"\\docky-now-playing.exe", L"", programs, 0);
    };
    const auto started = GetTickCount64();
    while (!running.empty()) {
        request.managerOwnsCore = isRunning(managerName) && GetTickCount64() - started < 12000;
        RequestAuxiliaryExit(request);
        request.remember = false;
        if (GetTickCount64() - started >= 25000) break;
        Sleep(300);
        running = RunningIn(dir, executables);
    }
    if (!running.empty()) {
        std::wstring list;
        for (const auto& p : running) list += L"\n  " + p.relativePath + L" (" + std::to_wstring(p.pid) + L")";
        Log(L"still running:%ls", list.c_str());
        RemoveTree(stage);
        restart();
        return Fail(L"Close these DockOver programs or let their transfers finish, then retry the update:" + list);
    }

    // 3. Replace.
    std::vector<Replaced> done;
    bool ok = true;
    std::wstring failed;
    for (const auto& f : files) {
        Replaced r{dir + L"\\" + f, dir + L"\\" + f + L".update-old", false};
        EnsureParent(r.target);
        if (Exists(r.target)) {
            SetFileAttributesW(r.old.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(r.old.c_str());
            if (!MoveFileExW(r.target.c_str(), r.old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                ok = false; failed = f + L" (error " + std::to_wstring(GetLastError()) + L")"; break;
            }
            r.hadOld = true;
        }
        done.push_back(r);
        if (!CopyFileW((stage + L"\\" + f).c_str(), r.target.c_str(), FALSE)) {
            ok = false; failed = f + L" (error " + std::to_wstring(GetLastError()) + L")"; break;
        }
    }
    if (!ok) {
        Log(L"replace failed at %ls; rolling back %zu files", failed.c_str(), done.size());
        Rollback(done);
        RemoveTree(stage);
        restart();
        return Fail(L"Could not replace " + failed + L". The previous version was restored.");
    }
    for (const auto& r : done) if (r.hadOld) { SetFileAttributesW(r.old.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(r.old.c_str()); }
    RemoveTree(stage);
    DeleteFileW(zip.c_str());
    Log(L"installed %zu files", done.size());

    // 4. Register the installed suite and restart the components.
    if (releaseLayout) {
        docky::suite::Register(dir);
        docky::suite::StopStandaloneWindows();
    }
    restart();
    if (reopenSettings) Start(programs + L"\\LiquidDesktopSettings.exe", L"--page about", programs, 0);
    Log(L"done (DockOver %ls, DockOver Desktop %ls)", wasRunning ? L"restarted" : L"was not running",
        desktopWasRunning ? L"restarted" : L"was not running");
    return 0;
}
