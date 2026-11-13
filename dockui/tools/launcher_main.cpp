// DockOver.exe: the program users start from a release folder.
//   DockOver.exe            (this launcher)
//   DockOver Desktop.exe    (widgets and Stacks, a separate program)
//   runtime\                (the Desktop Manager, Dock, Menu Bar, Settings, guardian, updater, assets)
// Starts runtime\LiquidDesktopManager.exe without a console window (command-line arguments are passed on). When DockOver
// already runs, it opens DockOver Settings instead (the Manager's settings event), like starting an app again.
#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const std::wstring dir = self.substr(0, self.find_last_of(L"\\/"));
    const std::wstring runtime = dir + L"\\runtime", manager = runtime + L"\\LiquidDesktopManager.exe";

    if (HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LiquidDesktopManagerV05SingleInstance")) {
        CloseHandle(running);
        if (HANDLE open = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\LiquidDesktopSettingsOpenV1")) { SetEvent(open); CloseHandle(open); }
        return 0;
    }
    if (GetFileAttributesW(manager.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, (L"DockOver could not start: " + manager + L" is missing.\n\nKeep the runtime folder next to DockOver.exe.").c_str(),
                    L"DockOver", MB_OK | MB_ICONWARNING);
        return 1;
    }
    // Arguments after this program's own name.
    std::wstring args;
    {
        int argc = 0;
        if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) { args += L" \""; args += argv[i]; args += L"\""; }
            LocalFree(argv);
        }
    }
    std::wstring cmd = L"\"" + manager + L"\"" + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(manager.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, runtime.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"DockOver could not start.", L"DockOver", MB_OK | MB_ICONWARNING);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
