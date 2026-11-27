#pragma once
// The Dock's status group: the menu bar's status items as cells in one Dock tile, so the menu bar can be turned off.
// Ordinary cells are half an icon square and fill two per column (a column is half an icon wide): 1-2 cells are half an
// icon wide, 3-4 exactly one icon, ... The combined status cell ("combo": battery arc, Wi-Fi, volume dots) is a whole
// icon (two columns, both rows) and starts at a fresh column. The total is the tile's width factor for dock_model. Drawn with Direct2D vectors so it stays
// sharp while magnified. Pure layout helpers are unit tested (tests/status_group_test.cpp).
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dock::statusgroup {

// Live state the cells show (refreshed by the Dock: status every few seconds, volume on change, input layout polled).
struct State {
    bool batteryPresent = false, charging = false;
    bool plugged = false;          // on mains power (charging or full): the combined cell shows a bolt
    bool showPercent = false;      // the combined cell shows the battery level as a number (dock.statusBatteryPercent)
    bool saver = false;            // Windows Battery Saver is on: the battery ring is yellow (iPhone Low Power Mode)
    bool dark = true;              // glyphs are white on a dark Dock, black on a light one
    // Clock and Calendar tiles (live): local time and date; the weekday in the user's language ("Wed").
    int hour = 0, minute = 0, second = 0, day = 1;
    std::wstring weekday;
    int tileStyle = 0;             // Clock / Calendar plates: 0 light (white), 1 dark (black), 2 Liquid Glass
    int battery = -1;              // 0..100, -1 unknown
    int wifi = 0;                  // 0 unavailable, 1 not connected, 2 connected
    bool bluetoothOn = false;
    bool audio = false, muted = false;
    int volume = 0;                // 0..100
    std::wstring inputBadge;       // the layout badge, e.g. EN (status_popups InputBadge)
    std::wstring shortDate;
    std::wstring time;             // "9:41"
    // The Wi-Fi glyph of the active Workshop pack ("wifi.connected", premultiplied BGRA); drawn in the combined cell.
    std::shared_ptr<const std::vector<std::uint32_t>> wifiIcon;
    unsigned wifiW = 0, wifiH = 0;
    // Tray icons kept in the Dock (cells "tray:<exe>#<id>", see TrayCell): the live icon per cell id (premultiplied
    // BGRA) and the app's name. A kept icon whose app is not running has no entry and is drawn as a dim placeholder.
    struct TrayImage {
        std::wstring name;
        std::shared_ptr<const std::vector<std::uint32_t>> pixels;
        unsigned w = 0, h = 0;
        bool operator==(const TrayImage&) const = default;
    };
    std::map<std::wstring, TrayImage> trayIcons;
    // The Hidden Icons folder tile: up to nine hidden tray icons (not the kept ones), in the tray's order.
    std::vector<TrayImage> trayPreview;
    bool operator==(const State&) const = default;
};

// A tray icon kept in the Dock: its cell id is the icon's tray key ("tray:<exe name>#<uid or guid>", tray_host.h).
// (The plain "tray" cell is the Hidden Icons chevron.)
inline bool TrayCell(const std::wstring& id) { return id.size() > 5 && id.compare(0, 5, L"tray:") == 0; }

// Cells that are a whole icon (two columns, both rows): the combined status, the Clock face, the Calendar date, the
// digital Date & Time and the Hidden Icons folder. Each of them is also shown as a Dock tile of its own
// (StandaloneCell), apart from the half-size cells.
bool FullCell(const std::wstring& id);
// Whole-icon tiles drawn on their own plate (Clock / Calendar style): analog, date, digital, tray.
bool PlateCell(const std::wstring& id);
inline bool StandaloneCell(const std::wstring& id) { return FullCell(id); }
// Width factor of a group showing `cells` (none: 0, no tile).
float WidthFactor(const std::vector<std::wstring>& cells);
// Cell `index` inside the tile body (DIP rectangle), packed as described above.
D2D1_RECT_F CellRect(D2D1_RECT_F body, const std::vector<std::wstring>& cells, size_t index);
// The cell under `p`, or -1.
const wchar_t* SystemStatusPart(D2D1_RECT_F body, D2D1_POINT_2F point);
int CellAt(D2D1_RECT_F body, const std::vector<std::wstring>& cells, D2D1_POINT_2F p);
// The tooltip-style label shown above the hovered cell ("Wi-Fi", "Control Center", the extension id ...). A kept tray
// icon's label is its exe name without ".exe" here; the Dock shows the app's own name while it runs (State::trayIcons).
std::wstring CellLabel(const std::wstring& id);

// Draws the plate and every cell. `hover` highlights one cell (-1 none).
void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, D2D1_RECT_F body, const std::vector<std::wstring>& cells,
          const State& state, int hover, float opacity);

}  // namespace dock::statusgroup
