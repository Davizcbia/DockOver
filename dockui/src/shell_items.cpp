#include "shell_items.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
// propkey.h after the shell headers avoids a PID_FIRST_USABLE redefinition warning.
#include <propkey.h>
#include <appmodel.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <thread>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dock {
namespace {
// A window hidden only because it is on another virtual desktop (shell-cloaked, with a desktop id that is not the
// current one). The macOS Dock shows apps from every Space as running; counting these keeps the Dock from shrinking
// and re-laying out on every desktop switch (measured 2026-11-10: three running apps dropped on each switch).
bool OnOtherDesktop(HWND hwnd) {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !(cloaked & DWM_CLOAKED_SHELL)) return false;
    static thread_local ComPtr<IVirtualDesktopManager> manager;
    static thread_local bool tried = false;
    if (!manager && !tried) { tried = true; CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)); }
    if (!manager) return false;
    GUID id{};
    if (FAILED(manager->GetWindowDesktopId(hwnd, &id)) || IsEqualGUID(id, GUID_NULL)) return false;
    BOOL current = TRUE;
    return SUCCEEDED(manager->IsWindowOnCurrentVirtualDesktop(hwnd, &current)) && !current;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring AbsoluteExePath(const std::wstring& path) {
    // Do not guess a working directory for bare executable names.
    if (path.size() < 3 || !((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                              (path[0] == L'\\' && path[1] == L'\\'))) return {};
    wchar_t full[32768];
    DWORD length = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!length || length >= ARRAYSIZE(full)) return {};
    return Lower(std::wstring(full, length));
}

// V0.18: AppUserModelID of a process (packaged apps only), lower-case; empty for unpackaged processes.
std::wstring ProcessAumid(DWORD pid) {
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process) return {};
    wchar_t id[APPLICATION_USER_MODEL_ID_MAX_LENGTH]{};
    UINT32 length = ARRAYSIZE(id);
    const LONG rc = GetApplicationUserModelId(process, &length, id);
    CloseHandle(process);
    return rc == ERROR_SUCCESS ? Lower(id) : std::wstring{};
}

BOOL CALLBACK FindCoreWindow(HWND child, LPARAM param) {
    wchar_t cls[64]{};
    GetClassNameW(child, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) { *reinterpret_cast<HWND*>(param) = child; return FALSE; }
    return TRUE;
}

// V0.18: AppUserModelID that the shell associates with a top-level window, lower-case; empty if none.
// 1) the window's property store (what the taskbar groups by), 2) for ApplicationFrameHost frames the hosted
// CoreWindow's process, 3) the window's own process (packaged Win32 apps). No assumption about the host process.
std::wstring WindowAumid(HWND hwnd) {
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal && *v.pwszVal) {
            std::wstring id = Lower(v.pwszVal); PropVariantClear(&v); return id;
        }
        PropVariantClear(&v);
    }
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    DWORD pid = 0;
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        HWND core = nullptr;
        EnumChildWindows(hwnd, FindCoreWindow, reinterpret_cast<LPARAM>(&core));
        if (!core) return {};
        GetWindowThreadProcessId(core, &pid);
    } else {
        GetWindowThreadProcessId(hwnd, &pid);
    }
    return ProcessAumid(pid);
}

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool EndsWithI(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

std::wstring FileName(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring Stem(const std::wstring& path) {
    std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring ExpandEnv(const std::wstring& s) {
    wchar_t buf[4096];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, ARRAYSIZE(buf));
    return (n > 0 && n <= ARRAYSIZE(buf)) ? std::wstring(buf) : s;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";
    struct LangCodePage {
        WORD lang, codepage;
    }* translations = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &len) ||
        len < sizeof(LangCodePage))
        return L"";
    wchar_t key[128];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].lang, translations[0].codepage);
    wchar_t* value = nullptr;
    if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &len) && value && len > 1)
        return Trim(value);
    return L"";
}

std::wstring ShellDisplayName(const std::wstring& parsingName) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&item)))) return L"";
    PWSTR name = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        result = name;
        CoTaskMemFree(name);
    }
    return result;
}

std::wstring ResolveShortcutTarget(const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return L"";
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ))) return L"";
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)) && path[0]) return ExpandEnv(path);
    // Windows 11's "File Explorer" pin targets a shell folder, not explorer.exe; identify it by AUMID.
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(link.As(&props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal &&
            _wcsicmp(pv.pwszVal, L"Microsoft.Windows.Explorer") == 0) {
            PropVariantClear(&pv);
            return EnvVar(L"WINDIR") + L"\\explorer.exe";
        }
        PropVariantClear(&pv);
    }
    return L"";
}

// Extracts the shell's own icon for any item (exe, lnk, AppsFolder entry) as premultiplied BGRA,
// trims transparent padding and re-centres it on a square canvas.
bool LoadIconPixels(const std::wstring& parsingName, UINT px, std::vector<uint32_t>& out, UINT& outW, UINT& outH) {
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        DLOG(L"icon: SHCreateItemFromParsingName failed 0x%08X for %ls", hr, parsingName.c_str());
        return false;
    }
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(hr = item.As(&factory))) return false;
    HBITMAP bitmap = nullptr;
    hr = factory->GetImage(SIZE{static_cast<LONG>(px), static_cast<LONG>(px)}, SIIGBF_ICONONLY, &bitmap);
    if (FAILED(hr) || !bitmap) {
        DLOG(L"icon: GetImage failed 0x%08X for %ls", hr, parsingName.c_str());
        return false;
    }
    BITMAP bm{};
    GetObjectW(bitmap, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    std::vector<uint32_t> buf(static_cast<size_t>(w) * h);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    const int lines = GetDIBits(screen, bitmap, 0, h, buf.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(bitmap);
    if (lines != h || w <= 0) return false;

    bool anyAlpha = false, straight = false;
    for (uint32_t p : buf) {
        const uint32_t a = p >> 24;
        if (a) anyAlpha = true;
        if (((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a) straight = true;
    }
    for (uint32_t& p : buf) {
        uint32_t a = p >> 24;
        if (!anyAlpha) {
            p |= 0xFF000000u;
            continue;
        }
        if (straight) {
            const uint32_t r = ((p >> 16) & 0xFF) * a / 255, g = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((buf[static_cast<size_t>(y) * w + x] >> 24) > 12) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (maxX < 0) {
        minX = minY = 0;
        maxX = w - 1;
        maxY = h - 1;
    }
    const int bw = maxX - minX + 1, bh = maxY - minY + 1;
    DLOG_DEBUG(L"icon: source %dx%d alpha=%d straight=%d content (%d,%d) %dx%d for %ls", w, h, anyAlpha ? 1 : 0,
         straight ? 1 : 0, minX, minY, bw, bh, FileName(parsingName).c_str());
    // V0.2: trimmed content fills the icon body exactly (macOS-measured 46 DIP); spacing comes from
    // the 14 DIP gap of the layout instead of padding baked into the bitmap.
    const int side = std::max(bw, bh);
    out.assign(static_cast<size_t>(side) * side, 0u);
    const int ox = (side - bw) / 2, oy = (side - bh) / 2;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            out[static_cast<size_t>(y + oy) * side + (x + ox)] = buf[static_cast<size_t>(y + minY) * w + (x + minX)];
    outW = outH = static_cast<UINT>(side);
    return true;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace
bool LoadShellIconPixels(const std::wstring& parsingName, UINT px, std::vector<uint32_t>& out, UINT& outW, UINT& outH) {
    return LoadIconPixels(parsingName, px, out, outW, outH);
}

std::wstring DefaultDataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\LiquidDock";
        CoTaskMemFree(base);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool LoadPinnedConfig(const std::wstring& path, std::vector<PinnedItem>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string bytes(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 1 << 20)), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    size_t start = (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, static_cast<int>(bytes.size() - start), nullptr, 0);
    std::wstring text(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, static_cast<int>(bytes.size() - start), text.data(), n);

    out.clear();
    bool launchpadSeen = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line == L"# liquid-desktop:launchpad:v1") {
            if (!launchpadSeen) { PinnedItem item; item.isLaunchpad=true; item.target=L"launchpad";
                item.labelOverride=L"Launchpad"; out.push_back(std::move(item)); launchpadSeen=true; }
            continue;
        }
        if (line.empty() || line[0] == L'#') continue;
        PinnedItem item;
        if (line == L"---") {
            item.isSeparator = true;
            item.target = L"---";
            out.push_back(std::move(item));
            continue;
        }
        const size_t a = line.find(L'|');
        item.target = Trim(line.substr(0, a));
        if (a != std::wstring::npos) {
            const size_t b = line.find(L'|', a + 1);
            item.args = Trim(line.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1));
            if (b != std::wstring::npos) item.labelOverride = Trim(line.substr(b + 1));
        }
        if (!item.target.empty()) out.push_back(std::move(item));
    }
    return true;
}

bool SavePinnedConfig(const std::wstring& path, const std::vector<PinnedItem>& items) {
    std::wstring text =
        L"# DockOver pinned items. One per line: target | arguments | label (arguments and label optional).\r\n"
        L"# target may be an .exe, a .lnk shortcut, or a shell parsing name such as shell:AppsFolder\\<AppUserModelID>.\r\n"
        L"# A line containing only --- inserts a separator.\r\n"
        L"# Drag files onto the Dock to pin them; right-click an icon to unpin. Press F3 in the Dock to reload.\r\n";
    for (const PinnedItem& item : items) {
        if (item.isDownloads || item.isRecycleBin || item.isTransient || item.isMinimized || item.isStatusGroup) continue; // running-only/system Downloads are never saved
        if (item.isLaunchpad) { text += L"# liquid-desktop:launchpad:v1\r\n"; continue; }
        if (item.isSeparator) {
            if (item.isSystemSeparator) continue;  // section boundaries are rebuilt on load
            text += L"---\r\n";
            continue;
        }
        text += item.target;
        if (!item.args.empty() || !item.labelOverride.empty()) text += L" | " + item.args;
        if (!item.labelOverride.empty()) text += L" | " + item.labelOverride;
        text += L"\r\n";
    }
    const std::string bytes = ToUtf8(text);
    const std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size()) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::vector<PinnedItem> DefaultPinnedItems() {
    std::vector<std::wstring> targets;
    const std::wstring windir = EnvVar(L"WINDIR");
    targets.push_back(windir + L"\\explorer.exe");
    for (const wchar_t* var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        const std::wstring edge = EnvVar(var) + L"\\Microsoft\\Edge\\Application\\msedge.exe";
        if (FileExists(edge)) {
            targets.push_back(edge);
            break;
        }
    }
    // The user's own taskbar pins (desktop apps) are the best guess for "common programs".
    const std::wstring pins = EnvVar(L"APPDATA") + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
    std::vector<std::wstring> links;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((pins + L"\\*.lnk").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            links.push_back(pins + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(links.begin(), links.end());
    targets.insert(targets.end(), links.begin(), links.end());
    targets.push_back(L"---");  // user apps | system utilities
    targets.push_back(windir + L"\\System32\\notepad.exe");
    targets.push_back(L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    targets.push_back(L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel");

    std::vector<PinnedItem> items;
    std::vector<std::wstring> seen;
    for (const std::wstring& t : targets) {
        if (items.size() >= 15) break;
        if (t == L"---") {
            if (!items.empty() && !items.back().isSeparator) {
                PinnedItem sep;
                sep.isSeparator = true;
                sep.target = t;
                items.push_back(std::move(sep));
            }
            continue;
        }
        std::wstring key;
        if (EndsWithI(t, L".lnk")) key = Lower(FileName(ResolveShortcutTarget(t)));
        else if (!StartsWithI(t, L"shell:")) key = Lower(FileName(t));
        else key = Lower(t);
        if (key.empty()) key = Lower(t);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        if (!StartsWithI(t, L"shell:") && !FileExists(t)) continue;
        if (StartsWithI(t, L"shell:") && ShellDisplayName(t).empty()) continue;
        seen.push_back(key);
        PinnedItem item;
        item.target = t;
        items.push_back(std::move(item));
    }
    while (!items.empty() && items.back().isSeparator) items.pop_back();
    return items;
}

void ResolvePinnedItem(PinnedItem& item, UINT iconPx) {
    if (item.isSeparator) return;
    const std::wstring t = ExpandEnv(item.target);
    item.label.clear();
    item.exeBase.clear();
    item.exePath.clear();
    item.fileLocation.clear();
    item.aumid.clear();
    item.isExplorer = false;
    if (StartsWithI(t, L"shell:")) {
        item.label = ShellDisplayName(t);
        static const std::wstring kApps = L"shell:appsfolder\\";
        if (StartsWithI(t, kApps) && t.size() > kApps.size()) item.aumid = Lower(t.substr(kApps.size()));
    } else {
        item.fileLocation = t;
        if (EndsWithI(t, L".lnk")) {
            item.label = Stem(t);
            const std::wstring resolved = ExpandEnv(ResolveShortcutTarget(t));
            // A shortcut may point to a document; only executable targets participate in running-app matching.
            if (EndsWithI(resolved, L".exe")) {
                item.exeBase = Lower(FileName(resolved));
                item.exePath = AbsoluteExePath(resolved);
            }
        } else if (EndsWithI(t, L".exe")) {
            item.label = FileDescription(t);
            if (item.label.empty()) item.label = Stem(t);
            item.exeBase = Lower(FileName(t));
            item.exePath = AbsoluteExePath(t);
        } else {
            item.label = ShellDisplayName(t);
        }
    }
    // Resolve bare executable names via Windows search so activation can still verify identity.
    if (item.exePath.empty() && !item.exeBase.empty() && t.find_first_of(L"\\/") == std::wstring::npos) {
        wchar_t resolved[32768];
        const DWORD length = SearchPathW(nullptr, t.c_str(), nullptr, ARRAYSIZE(resolved), resolved, nullptr);
        if (length && length < ARRAYSIZE(resolved)) item.exePath = AbsoluteExePath(resolved);
    }
    if (item.label.empty()) item.label = Stem(t);
    if (!item.labelOverride.empty()) item.label = item.labelOverride;
    item.isExplorer = item.exeBase == L"explorer.exe";
    item.pixels.clear();
    item.iconW = item.iconH = 0;
    if (!LoadIconPixels(t, iconPx, item.pixels, item.iconW, item.iconH)) DLOG(L"icon: no icon for %ls", t.c_str());
}

void LaunchPinnedItemAsync(const PinnedItem& item) {
    const std::wstring target = ExpandEnv(item.target);
    const std::wstring args = item.args;
    const std::wstring label = item.label;
    std::thread([target, args, label] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = target.c_str();
        sei.lpParameters = args.empty() ? nullptr : args.c_str();
        sei.nShow = SW_SHOWNORMAL;
        const BOOL ok = ShellExecuteExW(&sei);
        const DWORD err = ok ? 0 : GetLastError();
        DWORD pid = sei.hProcess ? GetProcessId(sei.hProcess) : 0;
        if (sei.hProcess) CloseHandle(sei.hProcess);
        DLOG(L"launch: %ls -> %ls (error %lu, pid %lu) [%ls]", label.c_str(), ok ? L"ok" : L"FAILED", err, pid,
             target.c_str());
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

bool ActivateAppWindow(const PinnedItem& item) {
    HWND hwnd = item.window;
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
        (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || (item.exePath.empty() && item.aumid.empty())) return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked && !OnOtherDesktop(hwnd)) return false;  // activating switches desktops
    if (!item.aumid.empty()) {  // V0.18: packaged app — re-verify the window still belongs to this AUMID
        if (WindowAumid(hwnd) != item.aumid) return false;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        return SetForegroundWindow(hwnd) != FALSE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process) return false;
    wchar_t path[32768]; DWORD length = ARRAYSIZE(path);
    const bool match = QueryFullProcessImageNameW(process, 0, path, &length) &&
                       AbsoluteExePath(path) == item.exePath;
    CloseHandle(process);
    if (!match) return false;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    return SetForegroundWindow(hwnd) != FALSE;
}

void OpenFileLocation(const PinnedItem& item) {
    if (item.fileLocation.empty()) return;
    const std::wstring params = L"/select,\"" + item.fileLocation + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

namespace {
struct RunningScan {
    HWND self = nullptr;
    std::map<std::wstring, HWND> byExe;
    std::map<std::wstring, HWND> byPath;
    std::set<DWORD> visitedPids;
    HWND explorerWindow = nullptr;
    bool wantAumid = false;                         // V0.18: only when some item is a packaged app
    std::map<std::wstring, HWND> byAumid;           // per window: one ApplicationFrameHost hosts many apps
    std::map<std::wstring, std::wstring> pathAumid; // exe path -> AUMID of its first window (packaged Win32)
};

BOOL CALLBACK EnumRunning(HWND hwnd, LPARAM param) {
    auto* scan = reinterpret_cast<RunningScan*>(param);
    if (hwnd == scan->self || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked && !OnOtherDesktop(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"CabinetWClass") == 0) {
        if (!scan->explorerWindow) scan->explorerWindow = hwnd;
        return TRUE;
    }
    std::wstring aumid;
    if (scan->wantAumid) { aumid = WindowAumid(hwnd); if (!aumid.empty()) scan->byAumid.emplace(aumid, hwnd); }  // before the per-process dedup
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || !scan->visitedPids.insert(pid).second) return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[32768];
    DWORD len = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &len)) {
        scan->byExe.emplace(Lower(FileName(path)), hwnd);
        const std::wstring full = AbsoluteExePath(path);
        if (!full.empty()) { scan->byPath.emplace(full, hwnd); if (!aumid.empty()) scan->pathAumid.emplace(full, aumid); }
    }
    CloseHandle(process);
    return TRUE;
}
}  // namespace

namespace {
struct AppWindowScan { const PinnedItem* item; HWND self; std::vector<HWND> out; };
BOOL CALLBACK EnumAppWindows(HWND hwnd, LPARAM param) {
    auto* scan = reinterpret_cast<AppWindowScan*>(param);
    if (hwnd == scan->self || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked && !OnOtherDesktop(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    if (scan->item->isExplorer) {
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, L"CabinetWClass") == 0) scan->out.push_back(hwnd);
        return TRUE;
    }
    if (!scan->item->aumid.empty()) {  // V0.18: packaged apps match per window by AUMID
        if (WindowAumid(hwnd) == scan->item->aumid) scan->out.push_back(hwnd);
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process) return TRUE;
    wchar_t path[32768]; DWORD len = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &len) && AbsoluteExePath(path) == scan->item->exePath) scan->out.push_back(hwnd);
    CloseHandle(process);
    return TRUE;
}
}  // namespace

std::vector<HWND> AppWindows(const PinnedItem& item, HWND self) {
    AppWindowScan scan{&item, self, {}};
    if (item.isExplorer || !item.exePath.empty() || !item.aumid.empty()) EnumWindows(EnumAppWindows, reinterpret_cast<LPARAM>(&scan));
    return scan.out;  // top-to-bottom Z order
}

bool RefreshRunningState(std::vector<PinnedItem>& items, HWND self, bool* scanSucceeded, std::vector<std::wstring>* unpinnedRunning) {
    if (scanSucceeded) *scanSucceeded = false;
    RunningScan scan;
    scan.self = self;
    scan.wantAumid = std::any_of(items.begin(), items.end(), [](const PinnedItem& it) { return !it.aumid.empty(); });
    if (!EnumWindows(EnumRunning, reinterpret_cast<LPARAM>(&scan))) {
        DLOG(L"running: window enumeration failed; preserving state");
        return false;
    }
    if (scanSucceeded) *scanSucceeded = true;
    bool changed = false;
    for (PinnedItem& item : items) {
        HWND window = nullptr;
        if (item.isSeparator || item.isDownloads || item.isRecycleBin || item.isLaunchpad || item.isMinimized || item.isStatusGroup) continue;
        if (item.isExplorer) {
            window = scan.explorerWindow;
        } else if (!item.aumid.empty()) {
            auto it = scan.byAumid.find(item.aumid);
            if (it != scan.byAumid.end()) window = it->second;
        } else if (!item.exePath.empty()) {
            auto it = scan.byPath.find(item.exePath);
            if (it != scan.byPath.end()) window = it->second;
        } else if (!item.exeBase.empty() && item.exeBase != L"explorer.exe") {
            auto it = scan.byExe.find(item.exeBase);
            if (it != scan.byExe.end()) window = it->second;
        }
        const bool running = window != nullptr;
        if (running != item.running) {
            changed = true;
            DLOG(L"running: %ls -> %ls", item.label.c_str(), running ? L"running" : L"not running");
        }
        item.running = running;
        item.window = window;
    }
    if (unpinnedRunning) {
        // Shell/system hosts and our own components are never shown as apps (UWP hosts are out of scope).
        static const wchar_t* kSkip[] = {L"explorer.exe", L"applicationframehost.exe", L"shellexperiencehost.exe", L"searchhost.exe",
                                         L"startmenuexperiencehost.exe", L"textinputhost.exe", L"systemsettings.exe", L"lockapp.exe",
                                         L"liquiddock.exe", L"liquidmenubar.exe", L"liquiddesktopmanager.exe"};
        for (const auto& [path, hwnd] : scan.byPath) {
            const std::wstring base = Lower(FileName(path));
            if (std::any_of(std::begin(kSkip), std::end(kSkip), [&](const wchar_t* s) { return base == s; })) continue;
            const auto pa = scan.pathAumid.find(path);
            const std::wstring pathAumid = pa != scan.pathAumid.end() ? pa->second : std::wstring{};
            const bool known = std::any_of(items.begin(), items.end(), [&](const PinnedItem& it) {
                return !it.isSeparator && (it.exePath == path || (it.exePath.empty() && !it.exeBase.empty() && it.exeBase == base) ||
                                           (!pathAumid.empty() && it.aumid == pathAumid));  // V0.18: packaged app pinned by AUMID
            });
            if (!known) unpinnedRunning->push_back(path);
        }
    }
    return changed;
}

}  // namespace dock
