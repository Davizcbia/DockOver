#pragma once
// Windows "jump list" style entries for Dock context menus, read only through public shell APIs:
//  * Quick access (pinned + frequent folders), which is what the Explorer taskbar jump list shows.
//  * Recent documents Windows recorded for an AppUserModelID (IApplicationDocumentLists).
// Application-defined task entries (for example a browser's "New private window") are stored in an
// undocumented format and are not read.
#include <windows.h>

#include <string>
#include <vector>

#include "shell_items.h"

namespace dock::jump {
struct Entry {
    std::wstring label;        // display name
    std::wstring parsingName;  // shell parsing name (path or ::{CLSID} path) used to open it
};
std::vector<Entry> QuickAccessFolders(size_t max);
std::vector<Entry> RecentForApp(const std::wstring& aumid, size_t max);
// Explicit AppUserModelID for a Dock item: its shell:AppsFolder id, the .lnk's System.AppUserModel.ID, or the
// id of its running window. Empty when the app only has an implicit (not publicly derivable) id.
std::wstring AppIdFor(const PinnedItem& item);
bool Open(const std::wstring& parsingName, HWND owner);
}  // namespace dock::jump
