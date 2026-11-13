#pragma once
#include "menu_model.h"
namespace liquid::menu {
std::vector<Menu> ReadNativeMenus(HWND target);
// Posts only verified standard WM_COMMAND; result means queued, not executed.
bool DispatchNativeCommand(const Menu& menu, const MenuItem& item);
}
