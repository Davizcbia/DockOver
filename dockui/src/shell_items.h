#pragma once
// Pinned-item configuration, real Windows icon extraction, launching and running-app detection.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dock {

struct PinnedItem {
    std::wstring target;         // path, .lnk, or shell: parsing name (e.g. shell:AppsFolder\<AUMID>)
    std::wstring args;           // optional launch arguments
    std::wstring labelOverride;  // optional display name from the config file
    bool isDownloads = false;   // synthetic system item; not persisted as an application
    bool isRecycleBin = false;  // synthetic system item (always last); never persisted, dragged or removed
    int recycleFull = -1;       // Recycle Bin state at the last icon load: 1 full, 0 empty, -1 unknown
    bool isLaunchpad = false;   // system item; its position is persisted as a reserved comment marker
    bool isSeparator = false;    // "---" line in the config
    bool isSystemSeparator = false;  // section boundary synthesized at runtime: never saved, never removable
    bool isTransient = false;    // running but not pinned: shown while running, never persisted
    bool isMinimized = false;    // a minimized window's tile (minimize_dock.h); window = that window; never persisted
    bool isStatusGroup = false;  // the menu bar status items as one Dock tile (status_group.h); from shared settings, never persisted
    std::vector<std::wstring> statusCells;  // status tiles: the cells this tile shows (one whole-icon cell, or the half-size group)
    std::string stableId;        // Application namespace; independent of display label and icon path.
    // Resolved data:
    std::wstring label;
    std::wstring exeBase;       // fallback for targets that contain no absolute path
    std::wstring exePath;       // normalized absolute executable path, when known
    std::wstring fileLocation;  // file-system path for "open file location"; empty for shell items
    std::wstring aumid;         // V0.18: lower-case AppUserModelID for shell:AppsFolder\<AUMID> (UWP / MSIX) items
    bool isExplorer = false;
    std::vector<uint32_t> pixels;  // premultiplied BGRA, square, trimmed to content
    UINT iconW = 0, iconH = 0;
    bool running = false;
    HWND window = nullptr;
};

std::wstring DefaultDataDir();
bool LoadPinnedConfig(const std::wstring& path, std::vector<PinnedItem>& out);
bool SavePinnedConfig(const std::wstring& path, const std::vector<PinnedItem>& items);
std::vector<PinnedItem> DefaultPinnedItems();
// Shell icon of any parsing name (e.g. a CLSID path), straight-alpha BGRA. Used for the Recycle Bin item.
bool LoadShellIconPixels(const std::wstring& parsingName, UINT px, std::vector<uint32_t>& out, UINT& outW, UINT& outH);
void ResolvePinnedItem(PinnedItem& item, UINT iconPx);
void LaunchPinnedItemAsync(const PinnedItem& item);
bool ActivateAppWindow(const PinnedItem& item);
// All activatable top-level windows of a pinned app, top-to-bottom in Z order (same filters as the running scan).
std::vector<HWND> AppWindows(const PinnedItem& item, HWND self);
void OpenFileLocation(const PinnedItem& item);
// unpinnedRunning (optional): absolute exe paths of running apps that match no Dock item (one per app;
// several windows of the same exe are merged). System shell hosts are filtered out.
bool RefreshRunningState(std::vector<PinnedItem>& items, HWND self, bool* scanSucceeded = nullptr,
                         std::vector<std::wstring>* unpinnedRunning = nullptr);

}  // namespace dock
