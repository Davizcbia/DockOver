#pragma once
// Glass Studio: a separate, movable, closable Win32 window on its own thread with its own message
// loop, so dragging or resizing it never stalls the Dock's render loop. It edits GlassParameters
// through GlassSettingsStore; the Dock picks up a consistent snapshot on its next frame.
#include <windows.h>

namespace dock {

void OpenGlassStudio(HWND dock);  // starts the Studio thread, or focuses the existing window
void ShutdownGlassStudio();       // closes the window and joins the thread (Dock exit)
HWND GlassStudioWindow();         // null when closed

}  // namespace dock
