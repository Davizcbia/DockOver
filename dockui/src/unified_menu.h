#pragma once
#include "menu_model.h"
#include <functional>
#include <vector>
namespace liquid::popup {
using Select=std::function<void(const menu::MenuItem&)>;
using Closed=std::function<void()>;
// Copies a conventional HMENU. Unsupported owner-drawn items are disabled.
std::vector<menu::MenuItem> FromNative(HMENU menu);
// One popup per process; arguments are copied. Position is in physical screen pixels.
HWND Show(HWND owner,POINT anchor,std::vector<menu::MenuItem> items,Select select,Closed closed={});
// Opens upwards: `bottomLeft` is the popup's bottom-left corner (a Dock tile's top edge); the popup keeps that bottom
// edge when its rows change. Clamped to the work area like Show.
HWND ShowAbove(HWND owner,POINT bottomLeft,std::vector<menu::MenuItem> items,Select select,Closed closed={});
// Like ShowAbove, as a grid of icon cells (the Dock's Hidden Icons): each item is one cell showing its icon (or its first
// letter), a dot marks checked items, and the hovered item's text appears in a footer (`title` when none is hovered).
// Left and right clicks choose an item like a row (LastSelectionSecondary tells them apart). Columns: GridColumns.
HWND ShowGridAbove(HWND owner,POINT bottomLeft,std::vector<menu::MenuItem> items,const std::wstring& title,Select select,Closed closed={});
int GridColumns(size_t count);  // 1-6 items one row; up to 12 two balanced rows; then rows of 6
// Screen rectangle (px) of item `index` in an open popup (a grid cell or a row); false when there is none.
bool ItemScreenRect(HWND popup,size_t index,RECT& out);
void SetVisualCaptureTest(bool enabled);
// Inside a Select callback: true when the row was chosen with the right mouse button.
bool LastSelectionSecondary();
void SetTheme(bool opaque, COLORREF background, COLORREF foreground);
void Close();
// Read-only, in-process diagnostic; -1 if no matching popup or row.
int HighlightedRow(HWND popup);
// The cascading submenu window a popup currently has open (tests, diagnostics); nullptr when none.
HWND Submenu(HWND popup);
// Replace status rows in place without closing or changing focus; false if not our active popup.
bool UpdateItems(HWND popup,std::vector<menu::MenuItem> items);
// GPU readback for an explicitly requested local diagnostic; refuses existing paths.
bool RequestDiagnosticSnapshot(HWND popup,const std::wstring& path);
#ifdef LIQUID_MENU_DIAGNOSTICS
std::uint64_t DiagnosticContentVersion(HWND popup);
bool DiagnosticHasSurface(HWND popup);
#endif
}
