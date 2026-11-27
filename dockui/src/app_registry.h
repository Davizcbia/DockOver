#pragma once
// Application Registry (first slice, used by Launchpad): discovers the user's applications through the
// Shell's Applications folder (FOLDERID_AppsFolder). That folder is what Start > All apps shows: it
// merges per-user and all-users Start Menu shortcuts with packaged (UWP/MSIX) applications and
// resolves duplicates, so no private APIs or registry scraping are needed. Every entry launches and
// loads its icon through "shell:AppsFolder\<parsing name>", which works for both kinds of apps.
#include <windows.h>

#include <string>
#include <vector>

namespace dock::apps {

struct AppEntry {
    std::string id;            // "app:" + lower-case UTF-8 parsing name; stable across sessions
    std::wstring name;         // display name from the Shell
    std::wstring parsingName;  // AppUserModelID or known-folder-relative path
    bool extraFile = false;    // from ExtraAppsDir: parsingName is the shortcut's full path
    bool game = false;
    // Start-menu entries launch through the Applications folder; extra shortcuts (ExtraAppsDir) by their file path.
    std::wstring Target() const { return extraFile ? parsingName : L"shell:AppsFolder\\" + parsingName; }
};

// %LOCALAPPDATA%\LiquidDock\Launchpad Apps: .lnk/.url shortcuts that Launchpad lists in addition to the Start menu
// (e.g. games and launchers imported from MyDockFinder that have no Start-menu entry). Created on demand.
std::wstring ExtraAppsDir();

// Enumerates on the calling thread (COM must be initialised). Sorted by display name, duplicates and
// obvious non-applications (uninstallers, documents, web links) removed. Returns false on failure.
bool DiscoverApps(std::vector<AppEntry>& out, std::wstring& error);
// Native Shell application entries, filtered to exclude visible maintenance
// shortcuts and documents. Excludes Launchpad-only imports.
bool DiscoverStartMenuApps(std::vector<AppEntry>& out, std::wstring& error);
// Creates independent launch links/copies shortcuts; never moves or executes a source.
// A stable source-path suffix makes repeated drops idempotent. Explicit directory supports tests.
int AddFilesToLaunchpad(const std::vector<std::wstring>& paths, const std::wstring& directory, std::wstring& error);

// Pure filter used by DiscoverApps; exposed for tests.
bool IsLaunchableEntry(const std::wstring& name, const std::wstring& parsingName);

// Locale-aware, case-insensitive, digit-aware ordering of display names.
bool NameLess(const std::wstring& a, const std::wstring& b);

// Case-insensitive substring / word-prefix match used by Launchpad search.
bool MatchesQuery(const std::wstring& name, const std::wstring& query);

}  // namespace dock::apps
