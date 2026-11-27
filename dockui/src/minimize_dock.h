#pragma once
// Minimize windows into the Dock (macOS; MyDockFinder "Minimize windows to the Dock" with the Genie effect).
//
// How (measured 2026-11-10): once a window is minimized PrintWindow returns black, but a DWM thumbnail of a minimized
// window still shows its last content, and a thumbnail can show just a band of its source (rcSource). MyDockFinder
// imports the DWM thumbnail APIs and offers "DWMThumbnail" as its capture method. So:
//  * a click-through, colour-keyed topmost overlay hosts DWM thumbnails (DWM composes them; no pixels are copied);
//  * Genie = one continuous GPU mesh of the captured window (genie_gpu.h, display-paced on its own thread); when that
//    capture is unavailable, ~96 horizontal bands of the window, each a thumbnail placed along the funnel;
//  * the Dock tile itself is a live thumbnail of the minimized window, fitted into the tile;
//  * clicking the tile plays the Genie in reverse and restores the window.
// Windows' own minimize animation would run at the same time, so Genie plays only while it is off (Settings > Dock >
// Genie minimize animation pauses it for the session; App restores it on exit).
#include <windows.h>

#include <functional>

namespace dock::minimize {

// changed(window, added): a minimized window gained (true) or lost (false) its Dock tile.
bool Init(HWND dock, std::function<void(HWND, bool)> changed);
void Shutdown();
// Stage Manager parks windows in its own shelf, which must not create Dock minimize tiles.
void StagePark(HWND window);
void StageRestore(HWND window);
bool StageOwnsMinimize(HWND window);
// Each Dock frame: the tile's icon rectangle in screen pixels (empty rect = not visible).
void SetTileRect(HWND window, const RECT& screen);
bool SuppressFallbackTile(HWND window);
// Tile clicked: reverse Genie, then restore and activate the window.
void Restore(HWND window);
// Drops tiles of windows that were closed or restored elsewhere (call about once a second).
void Prune();
// Keeps the overlay directly above the Dock while it shows tiles (each frame is fine: checked at most every 100 ms).
void KeepAbove();
void SetGenie(bool enabled);
bool Animating();
// A full-screen app (game) owns the monitor: drop the low-level mouse hook (every mouse packet of the game went through
// this thread), stop the periodic window snapshots and hide the full-monitor overlay so the game can use direct flip.
void SetGameMode(bool on);
// Launchpad (always on top, above the Dock) is open: hide the previews, which live on an always-on-top overlay and
// otherwise float above Launchpad. A minimize that starts meanwhile shows it again.
void SetCovered(bool covered);

}  // namespace dock::minimize
