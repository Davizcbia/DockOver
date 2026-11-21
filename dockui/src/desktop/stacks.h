#pragma once
// docky Desktop Stacks: the files on the Windows desktop (user + public) grouped into stacks along the right edge
// of the work area, like macOS "Use Stacks". A click expands a stack into a glass panel; files open, drag out to
// other apps, and right-click shows the real Explorer context menu.
#include "canvas.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace docky::desk {

enum class GroupBy { Kind, Date };
// InPlace: like macOS, a stack opens on the desktop itself (its files take the next desktop cells).
// Panel: a stack opens as a Liquid Glass panel next to it.
enum class StackStyle { InPlace, Panel };
struct FileEntry { std::wstring path, name; bool dir = false; ULONGLONG modified = 0; };  // modified: FILETIME ticks
struct Stack { std::wstring name; std::vector<FileEntry> files; };

// Pure grouping (unit tested). `now` is a FILETIME tick count in local time.
std::wstring KindOf(const std::wstring& name, bool dir);
std::vector<Stack> BuildStacks(std::vector<FileEntry> files, GroupBy by, ULONGLONG now);
std::vector<FileEntry> ScanDesktop();
std::wstring& DevFolder();  // development (--dev-stacks-folder): scan this folder instead of the desktop

// Forwards owner-drawn submenu messages ("Open with", "Send to") of an open Explorer context menu.
bool ForwardShellMenuMessage(UINT msg, WPARAM w, LPARAM l, LRESULT& result);

class Stacks {
public:
    bool Start(HWND host, GroupBy by, StackStyle style);
    void Stop();
    bool Running() const;
    void SetGroupBy(GroupBy by);
    void SetStyle(StackStyle style);
    // Free placement: stacks can be dragged to any desktop grid cell (x = column from the right edge, y = row). Cells are
    // keyed by stack name; OnCellsChanged reports them after a drag so the app can save them.
    void SetFreePlacement(bool on);
    void SetCells(const std::map<std::wstring, POINT>& cells);
    void OnCellsChanged(std::function<void(const std::map<std::wstring, POINT>&)> changed);
    void Rescan();
    void Relayout();      // work area / DPI / theme / wallpaper changed
    void Raise(bool raised);  // above the desktop window after "Show desktop", back to the bottom otherwise
    void DevOpen(int index);  // development: expand a stack at start-up (--dev-open-stack)
};
Stacks& GetStacks();

// Hides Explorer's own desktop icons while Stacks are on, so files are not shown twice (restored on exit and on a
// crash). Session-only: no Explorer setting is changed.
void HideDesktopIcons(bool hide);
// The Hide DockOver Desktop shortcut: hides Explorer's desktop icons that are showing, and shows them again afterwards.
void HideDesktopIconsForShortcut(bool hide);
void MaintainDesktopIcons();  // re-hide when Explorer recreated or re-showed its icon view

}  // namespace docky::desk
