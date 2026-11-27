// docky-ext: command-line bridge to the docky extension protocol for scripts (PowerShell, Python, Node, batch).
//
//   docky-ext send menubar  <json | @file>   one message to the Menu Bar  (statusItem.set / statusItem.remove)
//   docky-ext send desktop  <json | @file>   one message to docky Desktop (widget.set / widget.remove / widget.gallery)
//   docky-ext serve menubar <json | @file>   send it with this process as notifyHwnd, then print every event as one
//   docky-ext serve desktop <json | @file>   JSON line on stdout until the input closes or Ctrl+C; the item/widget is
//                                            removed when this process exits
//   docky-ext gallery                        open docky Desktop's widget gallery
// Exit codes: 0 sent, 2 usage, 3 host not running or message rejected.
#include "../sdk/docky_extension.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string Utf8(const wchar_t* w) { return docky_sdk::detail::Utf8(w ? std::wstring(w) : std::wstring()); }
bool Load(const wchar_t* arg, std::string& out) {
    if (arg[0] == L'@') {
        std::ifstream f(arg + 1, std::ios::binary); if (!f) return false;
        std::ostringstream s; s << f.rdbuf(); out = s.str();
        if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF) out.erase(0, 3);
        return true;
    }
    out = Utf8(arg); return true;
}
const wchar_t* HostClass(const std::wstring& name) {
    return name == L"menubar" ? docky_sdk::kMenuBarHostClass : name == L"desktop" ? docky_sdk::kDesktopHostClass : nullptr;
}
// Inserts "notifyHwnd":<hwnd> right after the opening brace of the message.
std::string WithNotify(const std::string& json, HWND h) {
    const auto brace = json.find('{');
    if (brace == std::string::npos) return json;
    return json.substr(0, brace + 1) + "\"notifyHwnd\":" + docky_sdk::detail::Hwnd(h) + "," + json.substr(brace + 1);
}
LRESULT CALLBACK Listener(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_COPYDATA) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (cds && cds->dwData == docky_sdk::kCopyDataMagic && cds->lpData) {
            std::fwrite(cds->lpData, 1, cds->cbData, stdout); std::fputc('\n', stdout); std::fflush(stdout);
            return 1;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int Usage() { std::fputs("usage: docky-ext send|serve menubar|desktop <json | @file>\n       docky-ext gallery\n", stderr); return 2; }
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"gallery")
        return docky_sdk::detail::Send(docky_sdk::kDesktopHostClass, nullptr, R"({"v":1,"type":"widget.gallery"})") ? 0 : 3;
    if (argc != 4) return Usage();
    const std::wstring verb = argv[1];
    const wchar_t* cls = HostClass(argv[2]);
    std::string json;
    if (!cls || (verb != L"send" && verb != L"serve") || !Load(argv[3], json)) return Usage();
    if (verb == L"send") {
        if (!docky_sdk::detail::Send(cls, nullptr, json)) { std::fputs("docky-ext: host not running or message rejected\n", stderr); return 3; }
        return 0;
    }
    WNDCLASSW wc{}; wc.lpfnWndProc = Listener; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DockyExtCliListener";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"docky-ext", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 3;
    ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    if (!docky_sdk::detail::Send(cls, window, WithNotify(json, window))) { std::fputs("docky-ext: host not running or message rejected\n", stderr); return 3; }
    // Re-send when a host restarts, so the item comes back by itself.
    const UINT restarted = docky_sdk::HostStartedMessage();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == restarted) docky_sdk::detail::Send(cls, window, WithNotify(json, window));
        DispatchMessageW(&msg);
    }
    return 0;
}
