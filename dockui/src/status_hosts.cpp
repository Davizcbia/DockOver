#include "status_hosts.h"

#include <shellapi.h>

#include <algorithm>
#include <memory>

#include "custom_icons.h"
#include "ext_protocol.h"
#include "log.h"
#include "tray_host.h"
#include "unified_menu.h"

namespace docky::statushost {
namespace {
HANDLE g_mutex = nullptr;          // owned while active
bool g_active = false, g_trayStarted = false;
HWND g_extHost = nullptr;
std::vector<extras::StatusItem> g_items;
ULONGLONG g_lastManifestLoad = 0;

void LoadImage(extras::StatusItem& item) {
    item.icon.reset();
    if (item.iconPath.empty()) return;
    auto image = std::make_shared<extras::Image>(); std::wstring why;
    if (dock::LoadCustomIconPixels(item.iconPath, image->pixels, image->w, image->h, why)) item.icon = image;
    else DLOG(L"status hosts: icon for %ls not loaded (%ls)", item.id.c_str(), why.c_str());
}
void ReloadManifests() {
    auto next = extras::LoadManifests(ext::ExtensionsRoot());
    for (auto& i : next) LoadImage(i);
    for (auto& i : g_items) if (!i.fromManifest && std::none_of(next.begin(), next.end(), [&](const extras::StatusItem& m) { return m.id == i.id; })) next.push_back(std::move(i));
    g_items = std::move(next);
    g_lastManifestLoad = GetTickCount64();
}
void Run(const std::wstring& run, const std::wstring& args, const std::wstring& folder) {
    if (run.empty()) return;
    ShellExecuteW(nullptr, L"open", run.c_str(), args.empty() ? nullptr : args.c_str(), folder.empty() ? nullptr : folder.c_str(), SW_SHOWNORMAL);
}
void Notify(const extras::StatusItem& item, const std::string& json) {
    if (!item.notify || !IsWindow(item.notify)) return;
    DWORD pid = 0; GetWindowThreadProcessId(item.notify, &pid); if (pid) AllowSetForegroundWindow(pid);
    ext::SendJson(item.notify, g_extHost, json, 300);
}
std::shared_ptr<const liquid::menu::Icon> RowIcon(const std::vector<std::uint32_t>& px, UINT w, UINT h, bool templ) {
    if (px.empty() || !w || !h || px.size() != static_cast<size_t>(w) * h) return nullptr;
    auto icon = std::make_shared<liquid::menu::Icon>(); icon->w = w; icon->h = h; icon->pixels = px; icon->templ = templ; return icon;
}
std::wstring Label(const extras::StatusItem& item) { return !item.tooltip.empty() ? item.tooltip : !item.title.empty() ? item.title : item.id; }

// The extension host window (same class as the Menu Bar's): statusItem.set / statusItem.remove over WM_COPYDATA.
LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        std::string json; if (!ext::ReadJson(l, json)) return 0;
        extras::Message msg; std::string error;
        if (!extras::ParseMessage(json, msg, error)) { DLOG(L"status hosts: message rejected (%hs)", error.c_str()); return 0; }
        if (!msg.item.notify && w) msg.item.notify = reinterpret_cast<HWND>(w);
        auto it = std::find_if(g_items.begin(), g_items.end(), [&](const extras::StatusItem& i) { return i.id == msg.item.id; });
        if (msg.action == extras::Action::Remove) { if (it != g_items.end()) g_items.erase(it); return 1; }
        if (it != g_items.end() && it->fromManifest) return 0;
        LoadImage(msg.item);
        if (it != g_items.end()) *it = std::move(msg.item); else if (g_items.size() < 64) g_items.push_back(std::move(msg.item));
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

void Start(HWND owner, UINT trayMessage) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = HostProc; wc.lpszClassName = ext::kMenuBarHostClass;
    if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        g_extHost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ext::kMenuBarHostClass, L"DockOver Status Extras", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (g_extHost) { ChangeWindowMessageFilterEx(g_extHost, WM_COPYDATA, MSGFLT_ALLOW, nullptr); PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"dockyHostStarted"), 0, 0); }
    }
    ReloadManifests();
    if (extras::LoadConfig(extras::ConfigPath()).trayHost) g_trayStarted = tray::Get().Start(owner, trayMessage);
    g_active = true;
    DLOG(L"status hosts: started in the Dock (tray %ls, %zu extension item(s))", g_trayStarted ? L"on" : L"off", g_items.size());
}
void Stop() {
    if (g_trayStarted) { tray::Get().Stop(); g_trayStarted = false; }
    if (g_extHost) { DestroyWindow(g_extHost); g_extHost = nullptr; }
    g_items.clear();
    g_active = false;
    DLOG(L"status hosts: stopped in the Dock");
}
}  // namespace

void Maintain(HWND owner, UINT trayMessage, bool wanted) {
    if (!wanted) {
        if (g_active) Stop();
        if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
        return;
    }
    if (g_active) {  // closed apps' items go; manifests are re-read every few seconds
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [](const extras::StatusItem& i) { return !i.fromManifest && i.notify && !IsWindow(i.notify); }), g_items.end());
        if (GetTickCount64() - g_lastManifestLoad > 5000) ReloadManifests();
        return;
    }
    if (!g_mutex) g_mutex = CreateMutexW(nullptr, FALSE, kHostsMutex);
    if (!g_mutex) return;
    const DWORD r = WaitForSingleObject(g_mutex, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) Start(owner, trayMessage);  // else the menu bar still holds it: next time
}

bool Active() { return g_active; }
const std::vector<extras::StatusItem>& Items() { return g_items; }

void ActivateExtension(HWND owner, const std::wstring& id, POINT bottomLeft) {
    auto it = std::find_if(g_items.begin(), g_items.end(), [&](const extras::StatusItem& i) { return i.id == id; });
    if (it == g_items.end()) return;
    const extras::StatusItem item = *it;
    if (item.menu.empty()) { Run(item.run, item.args, item.folder); Notify(item, extras::ClickedEvent(item.id, false)); return; }
    using liquid::menu::ItemKind;
    std::vector<liquid::menu::MenuItem> rows;
    for (size_t k = 0; k < item.menu.size(); ++k) {
        const auto& e = item.menu[k];
        if (e.separator) rows.push_back({L"", ItemKind::Separator, false, false, {}});
        else rows.push_back({e.title, ItemKind::Command, e.enabled, e.checked, {}, 0x5000 + static_cast<UINT>(k)});
    }
    liquid::popup::ShowAbove(owner, bottomLeft, std::move(rows), [item](const liquid::menu::MenuItem& row) {
        if (row.commandId < 0x5000 || row.commandId >= 0x5000 + item.menu.size()) return;
        const auto& e = item.menu[row.commandId - 0x5000];
        if (!e.run.empty()) Run(e.run, e.args, item.folder);
        Notify(item, extras::MenuEvent(item.id, e.id));
    });
}

void ShowHiddenIcons(HWND owner, POINT bottomLeft, const std::vector<std::wstring>& kept, KeepToggle keep) {
    using liquid::menu::ItemKind;
    struct Entry { std::wstring cell, name; std::uint64_t serial = 0; bool tray = false; };
    std::vector<Entry> entries; std::vector<liquid::menu::MenuItem> cells;
    auto isKept = [&](const std::wstring& id) { return std::find(kept.begin(), kept.end(), id) != kept.end(); };
    for (const auto& item : g_items) {
        Entry e{L"ext:" + item.id, Label(item)};
        liquid::menu::MenuItem m{e.name, ItemKind::Command, true, isKept(e.cell), {}, 0x3000 + static_cast<UINT>(entries.size())};
        if (item.icon) m.icon = RowIcon(item.icon->pixels, item.icon->w, item.icon->h, item.templateIcon);
        entries.push_back(std::move(e)); cells.push_back(std::move(m));
    }
    if (g_trayStarted)
        for (const auto& t : tray::Get().Snapshot()) {
            if (t.hidden || entries.size() >= 64) continue;
            Entry e{t.key, tray::DisplayName(t), t.serial, true};
            liquid::menu::MenuItem m{e.name, ItemKind::Command, true, isKept(e.cell), {}, 0x3000 + static_cast<UINT>(entries.size())};
            m.icon = RowIcon(t.pixels, t.w, t.h, false);
            entries.push_back(std::move(e)); cells.push_back(std::move(m));
        }
    if (cells.empty()) {
        liquid::popup::ShowAbove(owner, bottomLeft, {{g_active ? L"No icons" : L"Shown in the menu bar while it is on", ItemKind::Command, false, false, {}}}, [](const liquid::menu::MenuItem&) {});
        return;
    }
    const HWND popup = liquid::popup::ShowGridAbove(owner, bottomLeft, std::move(cells), L"Hidden Icons", [owner, entries, bottomLeft, keep](const liquid::menu::MenuItem& chosen) {
        if (chosen.commandId < 0x3000 || chosen.commandId - 0x3000 >= entries.size()) return;
        const Entry e = entries[chosen.commandId - 0x3000];
        POINT pt{}; GetCursorPos(&pt);
        if (!liquid::popup::LastSelectionSecondary()) {  // left click: the icon's own action
            if (e.tray) tray::Get().Click(e.serial, false, pt); else ActivateExtension(owner, e.cell.substr(4), bottomLeft);
            return;
        }
        // Right click: the app's own menu, and whether this icon is kept in the Dock's status group.
        std::vector<liquid::menu::MenuItem> rows{{e.tray ? L"Show App Menu" : L"Open", ItemKind::Command, true, false, {}, 1}};
        if (keep) { rows.push_back({L"", ItemKind::Separator, false, false, {}}); rows.push_back({L"Keep in Dock", ItemKind::Command, true, chosen.checked, {}, 2}); }
        liquid::popup::Show(owner, pt, std::move(rows), [owner, e, pt, bottomLeft, keep](const liquid::menu::MenuItem& row) {
            if (row.commandId == 1) { if (e.tray) tray::Get().Click(e.serial, true, pt); else ActivateExtension(owner, e.cell.substr(4), bottomLeft); }
            else if (row.commandId == 2 && keep) keep(e.cell);
        });
    });
    DLOG(L"hidden icons: grid of %zu item(s), %d column(s), %zu kept in the Dock", entries.size(), liquid::popup::GridColumns(entries.size()),
         static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [&](const Entry& e) { return isKept(e.cell); })));
    // Shell_NotifyIconGetRect answers with the icon's cell, so apps that open a flyout next to "their icon" use the cell.
    if (popup && g_trayStarted) {
        std::vector<std::pair<std::uint64_t, RECT>> anchors;
        for (size_t j = 0; j < entries.size(); ++j) { RECT r{}; if (entries[j].tray && liquid::popup::ItemScreenRect(popup, j, r)) anchors.push_back({entries[j].serial, r}); }
        tray::Get().SetAnchors(anchors);
    }
}

std::vector<tray::Icon> TrayIcons() { return g_trayStarted ? tray::Get().Snapshot() : std::vector<tray::Icon>{}; }

bool ClickTray(const std::wstring& key, bool right, const RECT& anchor) {
    if (!g_trayStarted) return false;
    for (const auto& t : tray::Get().Snapshot()) {
        if (t.key != key || t.hidden) continue;
        tray::Get().SetAnchors({{t.serial, anchor}});  // the Dock cell is where the icon is now
        POINT pt{}; GetCursorPos(&pt);
        return tray::Get().Click(t.serial, right, pt);
    }
    return false;
}

void Shutdown() { Maintain(nullptr, 0, false); }

}  // namespace docky::statushost
