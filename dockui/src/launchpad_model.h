#pragma once
// Launchpad layout model: an ordered list of nodes (applications or folders) that flows into pages of
// columns x rows. Pure data + operations so it can be unit tested without any window.
// Persisted as JSON (%LOCALAPPDATA%\LiquidDock\launchpad.json) with atomic replace; a corrupt file is
// reported to the caller, which keeps it as .corrupt and starts from the alphabetical default.
#include <string>
#include <vector>

namespace dock::launchpad {

struct Node {
    bool folder = false;
    std::string appId;               // when !folder
    std::string folderId;            // when folder
    std::wstring folderName;
    std::vector<std::string> apps;   // folder contents, in order
    std::string Key() const { return folder ? "folder:" + folderId : appId; }
};

struct Layout {
    std::vector<Node> nodes;
    std::vector<std::string> hidden;  // installed apps kept out of Launchpad (MyDockFinder "Replace everything")
};

// Alphabetical default from discovered application ids (already sorted by name).
Layout DefaultLayout(const std::vector<std::string>& discovered);
// Keeps the saved order and folders, drops applications that are no longer installed, appends newly
// discovered applications at the end in discovery order, removes duplicates and dissolves folders that
// end up with fewer than two applications.
Layout Merge(const Layout& saved, const std::vector<std::string>& discovered);
void Normalize(Layout& layout);

bool MoveNode(Layout& layout, size_t from, size_t to);
// Drops node `dragged` onto node `target`: app+app creates a folder at the target's position; app onto
// folder appends to it. Returns the index of the resulting folder node, or -1 when nothing changed.
int DropOnto(Layout& layout, size_t dragged, size_t target, const std::wstring& newFolderName, const std::string& newFolderId);
bool MoveWithinFolder(Layout& layout, size_t folder, size_t from, size_t to);
// Takes app `appIndex` out of folder `folder` and inserts it as a top-level node at `insertAt`
// (index in the layout after removal). Dissolves the folder if fewer than two apps remain.
bool MoveOutOfFolder(Layout& layout, size_t folder, size_t appIndex, size_t insertAt);
bool RenameFolder(Layout& layout, size_t folder, const std::wstring& name);

std::string ToJson(const Layout& layout);
bool FromJson(const std::string& text, Layout& layout, std::wstring& error);
bool Load(const std::wstring& path, Layout& layout, std::wstring& error, bool* exists);
bool Save(const std::wstring& path, const Layout& layout, std::wstring& error);

}  // namespace dock::launchpad
