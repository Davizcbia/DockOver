// docky-updater: installs a downloaded docky portable zip into the program folder.
//   docky-updater.exe --zip <file> --dir <program folder> [--version <v>] [--wait-pid <pid>] [--reopen-settings]
// Always started from a temporary copy (see update_client.cpp StartInstall), never from the folder it replaces.
// 1. Extract the zip (Windows' built-in tar.exe) into a staging folder and check its manifest.txt.
// 2. Wait for the process that started it, then ask a running docky to quit (the Manager's quit event: the same as
//    Menu Bar > Quit, which restores the Windows taskbar) and DockOver Desktop (WM_CLOSE to its host window: it does
//    not listen to that event), then wait for every program in the folder to exit.
// 3. Move each listed file aside (*.update-old), copy the new one in; on any failure put every old file back.
// 4. Start docky and DockOver Desktop again if they were running, and Settings > About if asked. Nothing is ever force-closed.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

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

// Processes whose executable lies in the program folder (the Manager, Dock, Menu Bar, Settings, guardian, ...).
// DockOver Desktop (widgets and Stacks) is a separate program that does not listen to the Manager's quit event. Its
// host window quits it on WM_CLOSE (the same as its own "Quit DockOver Desktop" menu item). Only a copy started from
// this folder is asked; returns how many were asked.
struct CloseDesktop { std::wstring prefix; int asked = 0; };
BOOL CALLBACK CloseDesktopWindow(HWND w, LPARAM l) {
    auto* c = reinterpret_cast<CloseDesktop*>(l);
    wchar_t cls[64]{};
    if (!GetClassNameW(w, cls, 64) || wcscmp(cls, L"DockyDesktopHostV1") != 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH * 4]{}; DWORD n = MAX_PATH * 4;
    const bool ours = QueryFullProcessImageNameW(p, 0, path, &n) && Lower(std::wstring(path, n)).rfind(c->prefix, 0) == 0;
    CloseHandle(p);
    if (ours && PostMessageW(w, WM_CLOSE, 0, 0)) ++c->asked;
    return TRUE;
}
int AskDesktopToQuit(const std::wstring& dir) {
    CloseDesktop c{Lower(dir) + L"\\"};
    EnumWindows(CloseDesktopWindow, reinterpret_cast<LPARAM>(&c));
    return c.asked;
}

std::vector<std::wstring> RunningIn(const std::wstring& dir) {
    std::vector<std::wstring> out;
    const std::wstring prefix = Lower(dir) + L"\\";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH * 4]{}; DWORD n = MAX_PATH * 4;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && Lower(std::wstring(path, n)).rfind(prefix, 0) == 0)
            out.push_back(std::wstring(pe.szExeFile) + L" (" + std::to_wstring(pe.th32ProcessID) + L")");
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
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
    if (!Exists(zip)) return Fail(L"The downloaded update is missing.");
    // Release folders keep the programs in runtime\ (DockOver.exe and DockOver Desktop.exe beside it); development
    // folders (build-borderless) have them at the top.
    const bool releaseLayout = Exists(dir + L"\\runtime\\LiquidDesktopManager.exe");
    const std::wstring programs = releaseLayout ? dir + L"\\runtime" : dir;
    if (!Exists(programs + L"\\LiquidDesktopManager.exe")) return Fail(L"The DockOver program folder was not found: " + dir);

    // 1. Extract and check.
    const std::wstring stage = zip + L".staging";
    RemoveTree(stage);
    if (!CreateDirectoryW(stage.c_str(), nullptr)) return Fail(L"Could not create a staging folder.");
    if (!Extract(zip, stage)) { RemoveTree(stage); return Fail(L"The update package could not be extracted."); }
    std::vector<std::wstring> files;
    if (!ReadManifest(stage + L"\\manifest.txt", files)) { RemoveTree(stage); return Fail(L"The update package has no valid manifest."); }
    bool hasManager = false, packageRelease = false;
    for (const auto& f : files) {
        if (!Exists(stage + L"\\" + f)) { RemoveTree(stage); return Fail(L"The update package is missing " + f + L"."); }
        if (Lower(f) == L"liquiddesktopmanager.exe") hasManager = true;
        if (Lower(f) == L"runtime\\liquiddesktopmanager.exe") hasManager = packageRelease = true;
    }
    if (!hasManager) { RemoveTree(stage); return Fail(L"The update package does not contain DockOver."); }
    if (packageRelease != releaseLayout) {
        RemoveTree(stage);
        return Fail(releaseLayout ? L"This update package is a development build and does not fit this DockOver folder."
                                  : L"This DockOver folder is a development copy; release packages install into a folder downloaded from GitHub. Download DockOver from GitHub instead.");
    }
    Log(L"staged %zu files", files.size());

    // 2. Close docky.
    if (waitPid) {
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) {
            if (WaitForSingleObject(p, 30000) != WAIT_OBJECT_0) Log(L"caller %lu still running after 30 s", waitPid);
            CloseHandle(p);
        }
    }
    // Only a Manager started from this folder is asked to quit (and restarted): another docky copy is left alone.
    bool wasRunning = false;
    for (const auto& r : RunningIn(dir))
        if (Lower(r).rfind(L"liquiddesktopmanager.exe ", 0) == 0) wasRunning = true;
    if (wasRunning) {
        if (HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\LiquidDesktopManagerQuitV1")) {
            SetEvent(quit);
            CloseHandle(quit);
            Log(L"asked DockOver to quit");
        } else Log(L"DockOver is running but its quit event is unavailable (error %lu)", GetLastError());
    }
    const bool desktopWasRunning = AskDesktopToQuit(dir) > 0;
    if (desktopWasRunning) Log(L"asked DockOver Desktop to quit");
    const std::wstring desktopExe = Exists(dir + L"\\DockOver Desktop.exe") ? dir + L"\\DockOver Desktop.exe" : dir + L"\\DockOverDesktop.exe";
    auto restart = [&] {
        if (wasRunning) Start(programs + L"\\LiquidDesktopManager.exe", L"", programs, CREATE_NO_WINDOW);
        if (desktopWasRunning && Exists(desktopExe)) Start(desktopExe, L"", dir, 0);
    };
    std::vector<std::wstring> running;
    for (int i = 0; i < 120; ++i) {  // up to 60 s
        running = RunningIn(dir);
        if (running.empty()) break;
        Sleep(500);
    }
    if (!running.empty()) {
        std::wstring list;
        for (const auto& r : running) list += L"\n  " + r;
        Log(L"still running:%ls", list.c_str());
        RemoveTree(stage);
        if (RunningIn(dir).empty()) restart();
        return Fail(L"These DockOver programs did not close, so the update was not installed:" + list);
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

    // 4. Start again.
    restart();
    if (reopenSettings) Start(programs + L"\\LiquidDesktopSettings.exe", L"--page about", programs, 0);
    Log(L"done (DockOver %ls, DockOver Desktop %ls)", wasRunning ? L"restarted" : L"was not running",
        desktopWasRunning ? L"restarted" : L"was not running");
    return 0;
}
