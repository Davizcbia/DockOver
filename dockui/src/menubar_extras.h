#pragma once
// Menu Bar extras (V0.1): which items the right side of the menu bar shows, and third-party status items.
//
// Item ids:
//   builtin:wifi | builtin:sound | builtin:battery | builtin:control |
//   builtin:bluetooth | builtin:input                                  - docky's own status items
//   tray:<exe name>#<uid or guid>                                     - a Windows notification-area icon (tray_host.h)
//   ext:<id>                                                          - a status item from the extension protocol
// Visibility rules: built-ins are shown unless switched off; tray icons are hidden (they live in the Hidden Icons
// popup) unless promoted; extension items are shown unless the user hid them. Hidden items of any kind appear
// in the Hidden Icons popup, so nothing is ever unreachable.
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace docky::extras {

struct Image { UINT w = 0, h = 0; std::vector<std::uint32_t> pixels; };  // premultiplied BGRA

struct MenuEntry {
    std::wstring id, title, run, args;  // run/args: manifest items start a program (ShellExecute) when chosen
    bool separator = false, enabled = true, checked = false;
};

struct StatusItem {
    std::wstring id;          // without the "ext:" prefix
    std::wstring title, tooltip, iconPath;
    bool templateIcon = true;
    std::shared_ptr<Image> icon;
    std::vector<MenuEntry> menu;
    std::wstring run, args;   // manifest: program started on click when there is no menu
    std::wstring folder;      // manifest folder (working directory for run); empty for pushed items
    HWND notify = nullptr;    // pushed items: event receiver; the item disappears with this window
    bool fromManifest = false;
};

struct Config {
    bool wifi = true, sound = true, battery = true, control = true, bluetooth = true, input = true;
    bool hiddenIcons = true;  // the Hidden Icons chevron
    bool trayHost = true;     // collect notification-area icons (needed for Hidden Icons and promoted tray icons)
    std::vector<std::wstring> shown;   // promoted tray ids, in bar order (right to left)
    std::vector<std::wstring> hidden;  // extension ids the user moved into the Hidden Icons popup
};

std::wstring ConfigPath();  // %LOCALAPPDATA%\LiquidDesktop\menubar-items.json
Config LoadConfig(const std::wstring& path);
bool SaveConfig(const std::wstring& path, const Config& config);  // atomic replace
bool IsShown(const Config& config, const std::wstring& fullId);
void SetShown(Config& config, const std::wstring& fullId, bool shown);

// Protocol messages for the menu bar host.
enum class Action { None, Set, Remove };
struct Message { Action action = Action::None; StatusItem item; };
// Parses one JSON message ("statusItem.set" / "statusItem.remove"). False on malformed or foreign input.
bool ParseMessage(const std::string& utf8, Message& out, std::string& error);
// Manifests: <root>\<folder>\extension.json with a "statusItems" array (same fields as statusItem.set plus run/args).
std::vector<StatusItem> LoadManifests(const std::wstring& root);
// Event JSON sent to an item's notifyHwnd.
std::string ClickedEvent(const std::wstring& id, bool right);
std::string MenuEvent(const std::wstring& id, const std::wstring& entryId);

}  // namespace docky::extras
