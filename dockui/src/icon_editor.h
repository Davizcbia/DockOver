#pragma once
// Edit Icon window (V0.2): modeless Win32 editor for one Dock icon. It edits a temporary State only;
// nothing is persisted until Apply, which hands the state to the owner's callback. Cancel and closing
// the window discard the state. One editor at a time.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "custom_icons.h"

namespace dock::iconedit {

struct State {
    std::vector<uint32_t> source;  // icon pixels before the edit (custom image or original), BGRA
    UINT w = 0, h = 0;
    IconEdit edit;
    bool globalMask = true;        // Dock-wide mask setting, used when edit.mask == -1
    std::wstring newImage;         // chosen by Change Image (empty = keep)
    bool resetAll = false;         // Reset pressed: Apply removes the image override and all edits
};

struct Callbacks {
    // Final Dock pixels for the preview (same transform + mask path as the Dock). side = output square side.
    std::function<std::vector<uint32_t>(const State&, UINT& side)> render;
    // Lets the user pick an image; loads it into state.source. False = cancelled or failed (state unchanged).
    std::function<bool(State&)> changeImage;
    // Restores state.source to the original (system) icon.
    std::function<void(State&)> resetToOriginal;
    // Persists the state and refreshes the Dock. False keeps the editor open (an error was shown).
    std::function<bool(const State&)> apply;
};

bool Open(HWND owner, const std::wstring& title, State initial, Callbacks callbacks);
bool IsOpen();
void Close();

}  // namespace dock::iconedit
