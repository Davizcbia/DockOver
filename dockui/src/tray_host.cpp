#include "tray_host.h"
#include "log.h"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace docky::tray {

std::wstring KeyFor(const std::wstring& exePath, UINT uid, const GUID* guid) {
    std::wstring name = exePath;
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
    if (name.empty()) name = L"unknown";
    std::wstring key = L"tray:" + name + L"#";
    if (guid) {
        wchar_t text[64]{};
        StringFromGUID2(*guid, text, 64);
        key += text;
    } else key += std::to_wstring(uid);
    return key;
}

std::wstring DisplayName(const Icon& icon) {
    std::wstring tip = icon.tip;
    const auto nl = tip.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) tip.resize(nl);
    while (!tip.empty() && iswspace(tip.back())) tip.pop_back();
    if (!tip.empty()) return tip;
    std::wstring name = icon.exePath;
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    return name.empty() ? L"Unknown app" : name;
}

std::vector<Posted> ClickMessages(UINT callback, UINT version, UINT uid, bool right, POINT pt) {
    std::vector<Posted> out;
    if (!callback) return out;
    const bool v4 = version >= NOTIFYICON_VERSION_4;
    auto add = [&](UINT mouse) {
        if (v4) out.push_back({callback, MAKEWPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y)), MAKELPARAM(mouse, uid)});
        else out.push_back({callback, static_cast<WPARAM>(uid), static_cast<LPARAM>(mouse)});
    };
    if (right) { add(WM_RBUTTONDOWN); add(WM_RBUTTONUP); if (v4) add(WM_CONTEXTMENU); }
    else { add(WM_LBUTTONDOWN); add(WM_LBUTTONUP); if (v4) add(NIN_SELECT); }
    return out;
}

namespace {
bool Same(const Icon& icon, const NotifyIconData32& nid) {
    if (nid.uFlags & NIF_GUID) return icon.hasGuid && IsEqualGUID(icon.guid, nid.guidItem);
    return !icon.hasGuid && icon.hwnd == reinterpret_cast<HWND>(static_cast<ULONG_PTR>(nid.hWnd)) && icon.uid == nid.uID;
}
std::wstring Tip(const WCHAR* text, size_t max) { size_t n = 0; while (n < max && text[n]) ++n; return std::wstring(text, n); }
}  // namespace

bool Apply(std::vector<Icon>& icons, const TrayNotifyData& data, std::uint64_t& nextSerial,
           const IconLoader& loadIcon, const ProcessInfo& process, bool& accepted) {
    accepted = false;
    const auto& nid = data.nid;
    auto it = std::find_if(icons.begin(), icons.end(), [&](const Icon& i) { return Same(i, nid); });
    auto update = [&](Icon& icon) {
        bool changed = false;
        if (nid.uFlags & NIF_MESSAGE) icon.callback = nid.uCallbackMessage;
        if (nid.uFlags & NIF_TIP) { auto tip = Tip(nid.szTip, 128); if (tip != icon.tip) { icon.tip = std::move(tip); changed = true; } }
        if ((nid.uFlags & NIF_ICON) && nid.hIcon) {
            std::vector<std::uint32_t> px; UINT w = 0, h = 0;
            if (loadIcon && loadIcon(nid.hIcon, px, w, h) && (px != icon.pixels || w != icon.w)) { icon.pixels = std::move(px); icon.w = w; icon.h = h; changed = true; }
        }
        if (nid.uFlags & NIF_STATE) {
            const DWORD mask = nid.dwStateMask ? nid.dwStateMask : NIS_HIDDEN;
            if (mask & NIS_HIDDEN) { const bool hidden = (nid.dwState & NIS_HIDDEN) != 0; if (hidden != icon.hidden) { icon.hidden = hidden; changed = true; } }
        }
        if (nid.hWnd && !(nid.uFlags & NIF_GUID)) icon.hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(nid.hWnd));
        if ((nid.uFlags & NIF_GUID) && nid.hWnd) icon.hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(nid.hWnd));
        return changed;
    };
    switch (data.message) {
    case NIM_ADD: {
        if (it != icons.end()) { accepted = true; return update(*it); }  // re-add after TaskbarCreated
        Icon icon;
        icon.serial = ++nextSerial;
        icon.hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(nid.hWnd));
        icon.uid = nid.uID;
        icon.hasGuid = (nid.uFlags & NIF_GUID) != 0;
        if (icon.hasGuid) icon.guid = nid.guidItem;
        if (process) process(icon.hwnd, icon.pid, icon.exePath);
        icon.key = KeyFor(icon.exePath, icon.uid, icon.hasGuid ? &icon.guid : nullptr);
        update(icon);
        icons.push_back(std::move(icon));
        accepted = true;
        return true;
    }
    case NIM_MODIFY:
        if (it == icons.end()) return false;
        accepted = true;
        return update(*it);
    case NIM_DELETE:
        if (it == icons.end()) return false;
        icons.erase(it);
        accepted = true;
        return true;
    case NIM_SETVERSION:
        if (it == icons.end()) return false;
        it->version = nid.uVersion;
        accepted = true;
        return false;
    default:
        return false;
    }
}

namespace {
struct State {
    mutable std::mutex mutex;
    std::vector<Icon> icons;
    std::map<std::uint64_t, RECT> anchors;
    std::uint64_t serial = 0;
    HWND notify = nullptr; UINT message = 0;
    std::atomic<bool> pending{false};
    HWND window = nullptr; HWND explorer = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::map<DWORD, std::wstring> exeCache;
};
State& S() { static State s; return s; }
constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr UINT_PTR kZTimer = 1, kRebroadcastTimer = 2;

HWND FindExplorerTray(HWND self) {
    DWORD shellPid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    HWND h = nullptr, fallback = nullptr;
    while ((h = FindWindowExW(nullptr, h, kTrayClass, nullptr)) != nullptr) {
        if (h == self) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;
        if (shellPid && pid == shellPid) return h;
        if (!fallback) fallback = h;
    }
    return fallback;
}

bool LoadTrayIcon(DWORD handle, std::vector<std::uint32_t>& out, UINT& w, UINT& h) {
    HICON icon = reinterpret_cast<HICON>(static_cast<ULONG_PTR>(handle));
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    BITMAP bm{};
    HBITMAP source = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (source && GetObjectW(source, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 256) {
        w = static_cast<UINT>(bm.bmWidth);
        h = static_cast<UINT>(ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2);
        if (h > 0 && h <= 256) {
            BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = static_cast<LONG>(w);
            bi.bmiHeader.biHeight = -static_cast<LONG>(h); bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            HDC dc = GetDC(nullptr);
            std::vector<std::uint32_t> color(static_cast<size_t>(w) * h), mask(static_cast<size_t>(w) * h, 0);
            const bool haveColor = ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, h, color.data(), &bi, DIB_RGB_COLORS) == static_cast<int>(h);
            const bool haveMask = ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS) == static_cast<int>(h);
            ReleaseDC(nullptr, dc);
            if (haveColor) {
                bool alpha = false;
                for (auto p : color) if (p >> 24) { alpha = true; break; }
                out.resize(color.size());
                for (size_t i = 0; i < color.size(); ++i) {
                    const std::uint32_t p = color[i];
                    unsigned a = alpha ? (p >> 24) : ((haveMask && (mask[i] & 0xFFFFFF)) ? 0u : 255u);
                    const unsigned r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
                    out[i] = (a << 24) | (r << 16) | (g << 8) | b;
                }
                ok = true;
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

void ProcessOf(HWND hwnd, DWORD& pid, std::wstring& exe) {
    pid = 0; exe.clear();
    if (!hwnd) return;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;
    auto& cache = S().exeCache;
    if (auto it = cache.find(pid); it != cache.end()) { exe = it->second; return; }
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 4]{}; DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(p, 0, path, &n)) exe.assign(path, n);
        CloseHandle(p);
    }
    if (cache.size() > 512) cache.clear();
    cache[pid] = exe;
}

void Changed() {
    auto& s = S();
    if (s.notify && !s.pending.exchange(true)) PostMessageW(s.notify, s.message, 0, 0);
}

LRESULT Forward(HWND self, UINT msg, WPARAM w, LPARAM l, bool& forwarded) {
    forwarded = false;
    HWND explorer = FindExplorerTray(self);
    if (!explorer) return 0;
    DWORD_PTR result = 0;
    forwarded = SendMessageTimeoutW(explorer, msg, w, l, SMTO_ABORTIFHUNG, 4000, &result) != 0;
    return static_cast<LRESULT>(result);
}

LRESULT OnNotify(HWND self, WPARAM w, LPARAM l, const COPYDATASTRUCT* cds) {
    bool forwarded = false;
    const LRESULT explorerResult = Forward(self, WM_COPYDATA, w, l, forwarded);
    if (cds->cbData < offsetof(TrayNotifyData, nid) + offsetof(NotifyIconData32, szTip)) return explorerResult;
    TrayNotifyData data{};
    memcpy(&data, cds->lpData, (std::min)(static_cast<size_t>(cds->cbData), sizeof(data)));
    if (data.signature != kSignature) return explorerResult;
    bool accepted = false, changed = false;
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        changed = Apply(S().icons, data, S().serial, LoadTrayIcon, ProcessOf, accepted);
    }
    if (changed) Changed();
    return accepted ? TRUE : (forwarded ? explorerResult : FALSE);
}

LRESULT OnGetRect(HWND self, WPARAM w, LPARAM l, const COPYDATASTRUCT* cds) {
    if (cds->cbData >= sizeof(IconIdentifier)) {
        IconIdentifier id{}; memcpy(&id, cds->lpData, sizeof(id));
        if (id.magic == kSignature && (id.message == 1 || id.message == 2)) {
            std::lock_guard<std::mutex> lock(S().mutex);
            const GUID zero{}; const bool byGuid = !IsEqualGUID(id.guidItem, zero);
            for (const auto& icon : S().icons) {
                const bool match = byGuid ? (icon.hasGuid && IsEqualGUID(icon.guid, id.guidItem))
                                          : (icon.hwnd == reinterpret_cast<HWND>(static_cast<ULONG_PTR>(id.hWnd)) && icon.uid == id.uID);
                if (!match) continue;
                auto a = S().anchors.find(icon.serial);
                if (a == S().anchors.end()) break;
                const RECT& r = a->second;
                return id.message == 1 ? MAKELONG(static_cast<short>(r.left), static_cast<short>(r.top))
                                       : MAKELONG(static_cast<short>(r.right), static_cast<short>(r.bottom));
            }
        }
    }
    bool forwarded = false;
    return Forward(self, WM_COPYDATA, w, l, forwarded);
}

void PruneDead() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        auto& icons = S().icons;
        const auto before = icons.size();
        icons.erase(std::remove_if(icons.begin(), icons.end(), [](const Icon& i) { return !i.hwnd || !IsWindow(i.hwnd); }), icons.end());
        changed = icons.size() != before;
    }
    if (changed) Changed();
}

void KeepOnTop(HWND h) {
    // Above Explorer's taskbar in Z order so FindWindow("Shell_TrayWnd") returns this window; never shown.
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    HWND explorer = FindExplorerTray(h);
    if (explorer) {
        RECT r{};  // mirror Explorer's taskbar rectangle for apps that measure Shell_TrayWnd directly
        if (GetWindowRect(explorer, &r)) {
            RECT mine{}; GetWindowRect(h, &mine);
            if (!EqualRect(&r, &mine)) SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
    }
    if (explorer != S().explorer) {
        const bool restarted = S().explorer != nullptr && explorer != nullptr;
        S().explorer = explorer;
        if (restarted) { DLOG(L"tray: Explorer taskbar changed; asking apps to re-add icons"); SetTimer(h, kRebroadcastTimer, 1500, nullptr); }
    }
}

LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (!cds) break;
        if (cds->dwData == 1 && cds->lpData) return OnNotify(h, w, l, cds);
        if (cds->dwData == 3 && cds->lpData) return OnGetRect(h, w, l, cds);
        bool forwarded = false;  // AppBar (0) and anything else: Explorer answers
        return Forward(h, msg, w, l, forwarded);
    }
    case WM_TIMER:
        if (w == kZTimer) { KeepOnTop(h); PruneDead(); }
        else if (w == kRebroadcastTimer) { KillTimer(h, kRebroadcastTimer); PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0); }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h, kZTimer); PostQuitMessage(0); return 0;
    case WM_COMMAND:  // taskbar commands such as Show desktop (407) and Minimize all (415), sent by Shell.ToggleDesktop etc.
        if (HWND explorer = FindExplorerTray(h)) PostMessageW(explorer, msg, w, l);
        return 0;
    default:
        if (msg >= WM_USER && msg < 0xC000) {  // taskbar private messages go to Explorer (no pointer payloads)
            bool forwarded = false;
            return Forward(h, msg, w, l, forwarded);
        }
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

void ThreadMain(HANDLE ready) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = TrayProc; wc.hInstance = instance; wc.lpszClassName = kTrayClass;
    HWND h = nullptr;
    if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTrayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    S().window = h;
    SetEvent(ready);
    if (!h) { S().running = false; return; }
    S().explorer = FindExplorerTray(h);
    KeepOnTop(h);
    SetTimer(h, kZTimer, 250, nullptr);
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    DLOG(L"tray: host window %p, Explorer taskbar %p", h, S().explorer);
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    S().window = nullptr;
    S().running = false;
}
}  // namespace

bool Host::Start(HWND notify, UINT message) {
    auto& s = S();
    if (s.running) return true;
    s.notify = notify; s.message = message; s.running = true;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) { s.running = false; return false; }
    s.thread = std::thread(ThreadMain, ready);
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    if (!s.window) { if (s.thread.joinable()) s.thread.join(); s.running = false; return false; }
    return true;
}

void Host::Stop() {
    auto& s = S();
    if (s.window) PostMessageW(s.window, WM_CLOSE, 0, 0);
    if (s.thread.joinable()) s.thread.join();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.icons.clear(); s.anchors.clear(); s.notify = nullptr;
}

bool Host::Running() const { return S().running && S().window; }

std::vector<Icon> Host::Snapshot() const {
    S().pending = false;
    std::lock_guard<std::mutex> lock(S().mutex);
    return S().icons;
}

bool Host::Click(std::uint64_t serial, bool right, POINT pt) {
    Icon icon;
    {
        std::lock_guard<std::mutex> lock(S().mutex);
        auto it = std::find_if(S().icons.begin(), S().icons.end(), [&](const Icon& i) { return i.serial == serial; });
        if (it == S().icons.end()) return false;
        icon = *it;
    }
    if (!icon.hwnd || !IsWindow(icon.hwnd)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(icon.hwnd, &pid);
    if (pid) AllowSetForegroundWindow(pid);  // the app's menu / window may take the foreground
    bool ok = true;
    for (const auto& m : ClickMessages(icon.callback, icon.version, icon.uid, right, pt)) ok = PostMessageW(icon.hwnd, m.message, m.w, m.l) && ok;
    DLOG(L"tray: %ls click forwarded to %ls (v%u) ok=%d", right ? L"right" : L"left", icon.key.c_str(), icon.version, ok ? 1 : 0);
    return ok;
}

void Host::SetAnchors(const std::vector<std::pair<std::uint64_t, RECT>>& anchors) {
    std::lock_guard<std::mutex> lock(S().mutex);
    S().anchors.clear();
    for (const auto& [serial, rect] : anchors) S().anchors[serial] = rect;
}

Host& Get() { static Host host; return host; }

}  // namespace docky::tray
