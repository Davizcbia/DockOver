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
