#pragma once
#include <string>
namespace liquid::settings {
// Empty categories reserve schema space; component-specific legacy stores remain authoritative.
// hideSnapLayouts: while a window is dragged, docky hides Windows' snap-layout bar (the Windows setting itself is untouched).
struct Dock { bool diagnostics = false; bool useMacIconMask = true; bool autoHide = false; bool hideSnapLayouts = false; bool genieMinimize = false;
    int downloadsCount = 6;          // files the Downloads fan shows (3..10)
    bool downloadsVertical = false;  // the fan rises straight up instead of curving
    bool downloadsPreview = true;    // file thumbnails (images, PDFs, videos) instead of type icons
    bool launchDesktop = false;      // starting DockOver (the Manager) also starts DockOver Desktop; not the other way round
};
struct MenuBar {}; struct Launchpad { int densityPreset=0, iconSize=96, gapX=80, gapY=48; bool hideLabels=false;
 bool showWindows=false; /* false (macOS): only the live wallpaper behind Launchpad; true: the live screen with its windows */ }; struct ControlCenter { int backdropBlur=60; /* 0..100 %: strength of the blur behind the whole Control Center */ }; struct Appearance { int mode=0; }; // 0 system, 1 light, 2 dark
// Windows taskbar integration. hideOnStartup is a preference only; the current visibility is runtime state
// owned by the Desktop Manager. hotkeyModifiers uses MOD_* bits (MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4);
// hotkeyKey 0 means "no shortcut". Default suggestion: Ctrl + Alt + T.
// freeWorkArea (opt-in, default off): give the hidden taskbar's reserved strip back to maximized windows.
struct Taskbar { bool hideOnStartup=false; bool hotkeyEnabled=true; int hotkeyModifiers=3; int hotkeyKey=0x54; bool freeWorkArea=false; };
// Frame-rate caps (Settings > General > Frame rate). 0 = unlimited (the display's refresh rate); otherwise 30/60/120/240.
// animationFps: every animated surface; captureFps: how often live glass backdrops take a new picture of the desktop.
struct Performance { int animationFps = 0, captureFps = 0; };
// Software update (Settings > About). autoCheck: the Manager looks for a new GitHub release once a day;
// autoInstall: it also downloads, verifies and installs it (docky restarts). Off: Settings shows it, a notification tells.
struct Updates { bool autoCheck = true; bool autoInstall = false; };
struct Settings {
    int schemaVersion = 1;
    Dock dock; MenuBar menuBar; Launchpad launchpad;
    ControlCenter controlCenter; Appearance appearance; Taskbar taskbar;
    Performance performance;
    Updates updates;
};
enum class LoadStatus { Loaded, Missing, Invalid, Unavailable };
struct Result { Settings value; LoadStatus status = LoadStatus::Missing; };
std::wstring DefaultPath();
Result Load(const std::wstring& path);
// Updates only one boolean member, under an exclusive cross-process lock.
// Unknown JSON is retained byte-for-byte; invalid/unsupported files are never replaced.
enum class SaveStatus { Saved, Invalid, Unavailable, Busy };
SaveStatus WriteBool(const std::wstring& path, const std::string& section,
                     const std::string& key, bool value);
// Only explicitly supported Launchpad density integers; same lock, validation and atomic replace.
SaveStatus WriteDensityInt(const std::wstring& path, const std::string& key, int value);
// Taskbar shortcut integers: hotkeyModifiers 0..7 (Alt/Ctrl/Shift; Win is never stored), hotkeyKey 0..254.
SaveStatus WriteTaskbarInt(const std::wstring& path, const std::string& key, int value);
SaveStatus WriteAppearanceMode(const std::wstring& path, int mode);
// Control Center integers: backdropBlur 0..100.
SaveStatus WriteControlCenterInt(const std::wstring& path, const std::string& key, int value);
// Dock integers: downloadsCount 3..10.
SaveStatus WriteDockInt(const std::wstring& path, const std::string& key, int value);
// Performance integers: animationFps / captureFps in {0 (unlimited), 30, 60, 120, 240}.
SaveStatus WritePerformanceInt(const std::wstring& path, const std::string& key, int value);
inline bool ValidFrameCap(int v) { return v == 0 || v == 30 || v == 60 || v == 120 || v == 240; }
// Explicit backup helper; never deletes or changes the legacy source or an existing backup.
enum class BackupStatus { Created, AlreadyExists, Unavailable };
BackupStatus BackupLegacy(const std::wstring& source);
struct Revision { bool present=false; unsigned long volume=0, indexHigh=0, indexLow=0, timeHigh=0, timeLow=0, sizeHigh=0, sizeLow=0; };
Revision ReadRevision(const std::wstring& path);
bool operator==(const Revision& a,const Revision& b);
inline bool operator!=(const Revision& a,const Revision& b){return !(a==b);}

}
