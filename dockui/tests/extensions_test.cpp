// Menu Bar extras, tray host message handling and the extension protocol (no windows, no Explorer).
#include "menubar_extras.h"
#include "tray_host.h"
#include "ext_protocol.h"
#include "../sdk/docky_extension.h"
#include <shellapi.h>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

using namespace docky;

static tray::TrayNotifyData Notify(DWORD message, DWORD hwnd, UINT uid, UINT flags) {
    tray::TrayNotifyData d{}; d.signature = tray::kSignature; d.message = message;
    d.nid.cbSize = sizeof(d.nid); d.nid.hWnd = hwnd; d.nid.uID = uid; d.nid.uFlags = flags;
    return d;
}

int main() {
    // --- config -----------------------------------------------------------------------------------------------
    extras::Config c;
    CHECK(extras::IsShown(c, L"builtin:wifi"));
    CHECK(!extras::IsShown(c, L"tray:app.exe#1"));          // tray icons start hidden
    CHECK(extras::IsShown(c, L"ext:com.example.a"));         // extension items start shown
    extras::SetShown(c, L"tray:app.exe#1", true);
    extras::SetShown(c, L"tray:app.exe#1", true);            // idempotent, no duplicates
    CHECK(c.shown.size() == 1);
    extras::SetShown(c, L"ext:com.example.a", false);
    CHECK(!extras::IsShown(c, L"ext:com.example.a"));
    extras::SetShown(c, L"builtin:battery", false);
    CHECK(!c.battery);
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"docky-extras-test-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    CHECK(extras::SaveConfig(path, c));
    auto loaded = extras::LoadConfig(path);
    CHECK(!loaded.battery && loaded.wifi && loaded.shown.size() == 1 && loaded.shown[0] == L"tray:app.exe#1");
    CHECK(loaded.hidden.size() == 1 && loaded.hidden[0] == L"ext:com.example.a");
    DeleteFileW(path.c_str());
    auto missing = extras::LoadConfig(path);                 // missing file: defaults
    CHECK(missing.wifi && missing.hiddenIcons && missing.trayHost);

    // --- protocol ---------------------------------------------------------------------------------------------
    extras::Message m; std::string error;
    CHECK(extras::ParseMessage(R"({"type":"statusItem.set","id":"com.example.timer","title":"25:00","notifyHwnd":4660,
        "menu":[{"id":"start","title":"Start"},{"separator":true},{"id":"x","title":"Run","run":"calc.exe"}]})", m, error));
    CHECK(m.action == extras::Action::Set && m.item.id == L"com.example.timer" && m.item.title == L"25:00");
    CHECK(m.item.notify == reinterpret_cast<HWND>(static_cast<ULONG_PTR>(4660)));
    CHECK(m.item.menu.size() == 3 && m.item.menu[1].separator);
    CHECK(m.item.menu[2].run.empty());                       // pushed items can never make docky run programs
    CHECK(!extras::ParseMessage(R"({"type":"statusItem.set","id":"bad id!"})", m, error));
    CHECK(!extras::ParseMessage(R"({"type":"nope","id":"a"})", m, error));
    CHECK(!extras::ParseMessage("not json", m, error));
    CHECK(extras::ParseMessage(R"({"type":"statusItem.remove","id":"a.b"})", m, error) && m.action == extras::Action::Remove);
    CHECK(extras::ParseMessage(R"({"type":"statusItem.set","id":"x","title":"0123456789012345678901234567890123456789"})", m, error) && m.item.title.size() == 32);
    const auto ev = extras::MenuEvent(L"com.example.timer", L"start");
    CHECK(docky_sdk::detail::Field(ev, "type") == "statusItem.menu" && docky_sdk::detail::Field(ev, "item") == "start");
    CHECK(docky_sdk::detail::Field(extras::ClickedEvent(L"a\"b", true), "id") == "a\"b");
    CHECK(docky_sdk::detail::Escape("a\"\n") == "\"a\\\"\\n\"");
    CHECK(ext::kCopyDataMagic == docky_sdk::kCopyDataMagic && std::wcscmp(ext::kMenuBarHostClass, docky_sdk::kMenuBarHostClass) == 0
          && std::wcscmp(ext::kDesktopHostClass, docky_sdk::kDesktopHostClass) == 0);

    // --- manifests --------------------------------------------------------------------------------------------
    const std::wstring root = std::wstring(temp) + L"docky-ext-root-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW((root + L"\\hello").c_str(), nullptr);
    {
        const char* manifest = R"({"id":"com.example.hello","statusItems":[{"id":"hello","title":"Hi","icon":"icon.png","menu":[{"title":"Open","run":"notepad.exe"}]}]})";
        HANDLE f = CreateFileW((root + L"\\hello\\extension.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0; WriteFile(f, manifest, static_cast<DWORD>(std::strlen(manifest)), &written, nullptr); CloseHandle(f);
    }
    auto items = extras::LoadManifests(root);
    CHECK(items.size() == 1 && items[0].fromManifest && items[0].menu.size() == 1 && items[0].menu[0].run == L"notepad.exe");
    CHECK(items.size() == 1 && items[0].iconPath == root + L"\\hello\\icon.png");
    DeleteFileW((root + L"\\hello\\extension.json").c_str()); RemoveDirectoryW((root + L"\\hello").c_str()); RemoveDirectoryW(root.c_str());

    // --- tray messages ----------------------------------------------------------------------------------------
    std::vector<tray::Icon> icons; std::uint64_t serial = 0; bool accepted = false;
    auto loader = [](DWORD h, std::vector<std::uint32_t>& px, UINT& w, UINT& hh) { px.assign(4, 0xFF000000u | h); w = hh = 2; return true; };
    auto proc = [](HWND, DWORD& pid, std::wstring& exe) { pid = 7; exe = L"C:\\Apps\\Twinkle Tray.exe"; };
    auto add = Notify(NIM_ADD, 0x100, 1, NIF_MESSAGE | NIF_ICON | NIF_TIP);
    add.nid.uCallbackMessage = WM_APP + 5; add.nid.hIcon = 0x42; wcscpy_s(add.nid.szTip, L"Twinkle Tray (90%)\nline two");
    CHECK(tray::Apply(icons, add, serial, loader, proc, accepted) && accepted && icons.size() == 1);
    CHECK(icons[0].key == L"tray:twinkle tray.exe#1" && icons[0].callback == WM_APP + 5 && icons[0].w == 2);
    CHECK(tray::DisplayName(icons[0]) == L"Twinkle Tray (90%)");
    CHECK(!tray::Apply(icons, add, serial, loader, proc, accepted) && accepted && icons.size() == 1);  // re-add: same icon
    auto ver = Notify(NIM_SETVERSION, 0x100, 1, 0); ver.nid.uVersion = NOTIFYICON_VERSION_4;
    tray::Apply(icons, ver, serial, loader, proc, accepted);
    CHECK(icons[0].version == NOTIFYICON_VERSION_4);
    auto hide = Notify(NIM_MODIFY, 0x100, 1, NIF_STATE); hide.nid.dwState = NIS_HIDDEN; hide.nid.dwStateMask = NIS_HIDDEN;
    CHECK(tray::Apply(icons, hide, serial, loader, proc, accepted) && icons[0].hidden);
    auto other = Notify(NIM_MODIFY, 0x100, 2, NIF_TIP);       // unknown icon: ignored
    CHECK(!tray::Apply(icons, other, serial, loader, proc, accepted) && !accepted);
    auto guidAdd = Notify(NIM_ADD, 0x200, 9, NIF_GUID | NIF_TIP); guidAdd.nid.guidItem = GUID{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    CHECK(tray::Apply(icons, guidAdd, serial, loader, proc, accepted) && icons.size() == 2 && icons[1].hasGuid);
    CHECK(icons[1].key.find(L"{00000001-0002-0003-0405-060708090A0B}") != std::wstring::npos);
    auto del = Notify(NIM_DELETE, 0x100, 1, 0);
    CHECK(tray::Apply(icons, del, serial, loader, proc, accepted) && icons.size() == 1 && icons[0].hasGuid);

    // --- click messages ---------------------------------------------------------------------------------------
    auto v4 = tray::ClickMessages(WM_APP + 5, NOTIFYICON_VERSION_4, 3, true, POINT{100, 30});
    CHECK(v4.size() == 3 && LOWORD(v4[2].l) == WM_CONTEXTMENU && HIWORD(v4[2].l) == 3 && LOWORD(v4[2].w) == 100 && HIWORD(v4[2].w) == 30);
    auto old = tray::ClickMessages(WM_APP + 5, 0, 3, false, POINT{});
    CHECK(old.size() == 2 && old[0].w == 3 && old[0].l == WM_LBUTTONDOWN && old[1].l == WM_LBUTTONUP);
    CHECK(tray::ClickMessages(0, 4, 1, false, POINT{}).empty());
    static_assert(sizeof(tray::NotifyIconData32) == 956);          // shell32's 32-bit-handle NOTIFYICONDATAW layout

    std::printf(failures ? "extensions test: %d failure(s)\n" : "extensions test: all passed\n", failures);
    return failures ? 1 : 0;
}
