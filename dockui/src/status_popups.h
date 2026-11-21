#pragma once
// System status popups shared by the Menu Bar and the Dock's status group: the input menu (keyboard layouts) and the
// battery menu. Both are liquid::popup menus; `above` opens them upwards from a Dock cell (anchor = its bottom-left),
// otherwise downwards from a menu bar slot (anchor = its top-left).
#include <windows.h>
#include <functional>
#include <string>

#include "system_status.h"

namespace docky::statuspopup {

// The keyboard layout of the app being typed into: the foreground window's thread. `ownPid` windows (our own bar,
// Dock or popup) are skipped and `last` is returned, so opening a menu does not change what is shown. `target`
// receives the window a layout change should be requested from.
HKL ForegroundLayout(HKL last, HWND& target);
// The badge drawn for a layout: 拼 / 注 / あ / 한, else the two-letter language code ("EN").
std::wstring InputBadge(HKL layout);
// The row text for a layout ("Pinyin – Simplified", or the language's display name).
std::wstring InputName(HKL layout);

// Installed layouts with the current one ticked, then "Open Keyboard Settings…". Choosing one posts
// WM_INPUTLANGCHANGEREQUEST to `target`. Returns the popup (nullptr on failure); `closed` runs when it closes.
HWND ShowInputMenu(HWND owner, POINT anchor, bool above, HKL current, HWND target, std::function<void()> closed);
// Battery level / power source rows and "Battery Settings…" (`openControlCenter` adds "Open Control Center…").
HWND ShowBatteryMenu(HWND owner, POINT anchor, bool above, const liquid::status::Snapshot& status,
                     std::function<void()> openControlCenter, std::function<void()> closed);

}  // namespace docky::statuspopup
