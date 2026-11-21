#pragma once
#include <string>
#include <vector>
namespace liquid::settings {
// Empty categories reserve schema space; component-specific legacy stores remain authoritative.
// hideSnapLayouts: while a window is dragged, docky hides Windows' snap-layout bar (the Windows setting itself is untouched).
// gameCompatibility: hide the Dock and Menu Bar and stop desktop capture while a full-screen game is active, or while a
// program from the game list (GameListPath: game-compat.txt beside the settings file, one .exe name per line) runs.
// lowLatencyGlass (opt-in): the Dock's glass shows the desktop one display frame after it was composed (one-frame DXGI
// path, dockui/docs/glass-one-frame-latency-v01.md); off = the standard path.
struct Dock { bool diagnostics = false; bool useMacIconMask = true; bool autoHide = false; bool hideSnapLayouts = false; bool genieMinimize = false; bool gameCompatibility = false;
    bool lowLatencyGlass = false;
    int iconSize = 46;               // Dock icon size in DIP (screen width limits the live size)
    int magnification = 126;         // hover magnification in percent added to the icon (0 = none, 150 = 2.5x); default 2.26x
    int downloadsCount = 6;          // files the Downloads fan shows (3..10)
    bool downloadsVertical = false;  // the fan rises straight up instead of curving
    bool downloadsPreview = true;    // file thumbnails (images, PDFs, videos) instead of type icons
    bool launchDesktop = false;      // starting DockOver (the Manager) also starts DockOver Desktop; not the other way round
    // The Dock's status items, in order. Whole-icon tiles of their own: combo (battery, Wi-Fi, volume; opens Control
    // Center), analog (Clock), date (Calendar). Half-size cells, together in one group tile: control wifi bluetooth
    // sound battery input clock tray, and ext:<extension id>. Empty = nothing in the Dock.
    std::vector<std::wstring> statusCells{L"combo", L"analog", L"date", L"tray"};
    int statusTileStyle = 0;  // the Clock and Calendar tiles: 0 light, 1 dark, 2 Liquid Glass
    bool statusBatteryPercent = false;  // the status group's combined cell shows the battery level as a number
    // Dock items the user can remove (drag out of the Dock, or Remove from Dock) and add back (Dock menu > Show in Dock).
    bool showDownloads = true, showTrash = true;
    bool showDivider = true;  // the section divider(s) between the apps and the Dock's right-hand items
    // Order of the right-hand items (dragged by the user): "status:<combo|analog|date>" for a whole-icon status tile,
    // "status" for the half-size group, "downloads", "trash". Empty or missing keys: the default order.
    std::vector<std::wstring> tailOrder;
};
// True for a key Dock::tailOrder accepts.
bool ValidTailKey(const std::wstring& key);
// True for an id the status group can show (see Dock::statusCells).
bool ValidStatusCell(const std::wstring& id);
// enabled=false: the Manager does not run the Menu Bar at all (its process exits); status items live in the Dock.
// captureNotifications (opt-in): read Windows notifications into the Notification Center and show banners.
// hideSystemNotifications: while capturing, keep Windows' own notification popup off screen (DockOver's banner instead).
// notificationBlur: strength of the blur under the Notification Center column (0..100 %);
// notificationBlurFade: width of its fade toward the left (40..400 DIP); notificationShade: how much that background is
// darkened (0..100 %).
struct MenuBar { int scalePercent=100; bool enabled=true; bool themeGradient=true; int gradientOpacity=90, gradientLength=100; bool captureNotifications=false; bool hideSystemNotifications=true; int notificationBlur=20, notificationBlurFade=100, notificationShade=30; /* defaults: the user's tuning, 2026-11-19 */ }; struct Launchpad { int densityPreset=0, iconSize=96, gapX=80, gapY=48; bool hideLabels=false;
 bool showWindows=false; /* false (macOS): only the live wallpaper behind Launchpad; true: the live screen with its windows */ };
struct AudioOutputPreference { std::wstring id, name; std::string icon; bool hidden=false; };
struct ControlCenter { std::vector<AudioOutputPreference> audioOutputs; bool mediaArtworkBackground=false; bool musicWallpaper=false; /* desktop background shows the playing music's artwork (docky-now-playing --wallpaper) */ bool wallpaperAvoidBars=true; /* that artwork is fitted between the menu bar and the Dock */ bool wallpaperLyrics=true; /* lyrics on that desktop artwork */ bool lyricsFullArtwork=true; /* lyrics keep the borderless artwork (else: artwork card) */ bool animatedArtwork=true; bool lyricsDoubleClickSeek=true; bool keepArtworkWhenPaused=false; /* paused music keeps the artwork and lyrics on screen (else they fade out after 2.5 s) */ bool localMusicTags=true; /* a track named like a file (song.mp3) is looked up on this PC: its tags, cover and .lrc */ bool hideDesktopIconsWithWidgets=true; /* the hide-DockOver-Desktop shortcut also hides Windows desktop icons */ bool lyricsAutoReturn=true; /* scrolled lyrics return to the sung line */ int musicFps=60; /* frame rate cap of the music views' animation: 30/60/120/240, 0 = unlimited */ bool lyricsBeat=true; /* lyrics pulse with the music (WASAPI loopback) */ int lyricsOffsetMs=300; /* lyrics shown this much earlier (-3000..3000) */ int shortcutLyrics=0x34C, shortcutArtwork=0x34D, shortcutDesktop=0x344, shortcutNowPlaying=0x34E; /* global shortcuts, (MOD_* << 8) | virtual key, 0 = none; defaults Ctrl+Alt+L / M / D / N: desktop lyrics, music wallpaper, hide DockOver Desktop, full-screen Now Playing */ int backdropBlur=60; /* 0..100 %: strength of the blur behind the whole Control Center */ }; struct Appearance { int mode=0, settingsScalePercent=90, applicationsScalePercent=80, applicationsSort=0; std::string language="en"; }; // 0 system, 1 light, 2 dark
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
// Overall Menu Bar size, including text, icons, hit regions and reserved work area.
SaveStatus WriteMenuBarScale(const std::wstring& path, int percent);
// Menu Bar integers: notificationBlur 0..100, notificationBlurFade 40..400, notificationShade 0..100.
SaveStatus WriteMenuBarInt(const std::wstring& path, const std::string& key, int value);
SaveStatus WriteApplicationsSort(const std::wstring& path, int order);
SaveStatus WriteLanguage(const std::wstring& path, const std::string& language);
SaveStatus WriteUiScale(const std::wstring& path, const std::string& key, int percent);
// Taskbar shortcut integers: hotkeyModifiers 0..7 (Alt/Ctrl/Shift; Win is never stored), hotkeyKey 0..254.
SaveStatus WriteTaskbarInt(const std::wstring& path, const std::string& key, int value);
SaveStatus WriteAppearanceMode(const std::wstring& path, int mode);
// Control Center integers: backdropBlur 0..100.
SaveStatus WriteAudioOutputs(const std::wstring& path, const std::vector<AudioOutputPreference>& outputs);
SaveStatus WriteControlCenterInt(const std::wstring& path, const std::string& key, int value);
// Dock integers: downloadsCount 3..10, iconSizeV2 1..65535.
SaveStatus WriteDockInt(const std::wstring& path, const std::string& key, int value);
// The status group's cells (validated: known ids only, no duplicates, at most 16).
SaveStatus WriteStatusCells(const std::wstring& path, const std::vector<std::wstring>& cells);
// The order of the Dock's right-hand items (validated: ValidTailKey, no duplicates, at most 8).
SaveStatus WriteTailOrder(const std::wstring& path, const std::vector<std::wstring>& keys);
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
