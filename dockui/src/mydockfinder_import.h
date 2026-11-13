#pragma once
// One-time import from MyDockFinder (Steam edition) into docky.
//
// MyDockFinder keeps (measured 2026-11-10 in the install folder):
//   ico.ini        UTF-16 LE, sections [ico1]..[icoN] in Dock order: tag (label), appname, filepath (exe / .lnk / folder,
//                  or the AppUserModelID when UWP=1), realpath, virtualpath, icopath (custom png), delimiter=1 for a
//                  separator; special appnames: "launchpad|", "downloads", "trash|", "cal|", "systemtime", "controlpanel".
//   padconfig.xml  UTF-8, <pad><pageN><icoM path="...\launchpad\Pinned\X.lnk|.url">Name</icoM>...: Launchpad order.
// docky keeps the Dock in %LOCALAPPDATA%\LiquidDock\pinned.txt (target | arguments | label) and the Launchpad order in
// launchpad.json (app ids "app:" + lower-case Shell parsing name). Both are backed up before they are replaced.
#include <string>
#include <vector>

namespace docky::mdf {

struct ImportResult {
    int dockItems = 0;
    std::vector<std::wstring> dockSkipped;       // items docky has no equivalent for (clock, calendar, ...)
    int launchpadMatched = 0, launchpadCopied = 0;      // copied: shortcuts added to docky's Launchpad Apps folder
    std::vector<std::wstring> launchpadUnmatched; // shortcuts with no matching application in docky's list
    std::wstring dockBackup, launchpadBackup;     // where the previous files were copied (merge import)
    std::wstring backupFolder;                    // replace: everything before the import (LiquidDock\Backups\...)
    int hidden = 0;                               // replace: other apps taken out of Launchpad
    std::wstring error;
};

// The MyDockFinder folder (Steam default), or empty when ico.ini is not found there.
std::wstring FindInstall();
// Converts ico.ini into pinned.txt lines (pure; exposed for tests).
std::vector<std::wstring> DockLinesFromIcoIni(const std::wstring& icoIniText, std::vector<std::wstring>& skipped);
// Runs both imports. COM must be initialised on the calling thread (application discovery, shortcut resolution).
// replace = false: the Dock list is replaced and Launchpad shows MyDockFinder's order first, then docky's other apps.
// replace = true: everything (Dock list, Launchpad layout and folders, Launchpad Apps shortcuts, settings files and
// custom icons) is first copied to LiquidDock\Backups\before-mydockfinder-<time>; the Dock list, Launchpad layout and
// Launchpad Apps shortcuts are then replaced so only MyDockFinder's items remain (other apps are hidden in Launchpad;
// apps installed later still appear). Nothing is changed when the backup fails.
bool Import(const std::wstring& mdfDir, const std::wstring& dockyDataDir, bool replace, ImportResult& result);

}  // namespace docky::mdf
