#pragma once
// Global "toggle Windows taskbar" shortcut model: validation, reserved combinations and display text.
// Registration itself uses RegisterHotKey/UnregisterHotKey (no keyboard hooks).
#include <windows.h>
#include <string>

namespace docky::hotkey {
// Default suggestion (MyDockFinder-like): Ctrl + Alt + T.
inline constexpr UINT kDefaultModifiers = MOD_CONTROL | MOD_ALT;
inline constexpr UINT kDefaultKey = 'T';

enum class Validity { Ok, Empty, NeedsCtrlOrAlt, UsesWinKey, ModifierOnly, Reserved };

inline bool IsModifierKey(UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: return true;
    default: return false;
    }
}

// Combinations the system or common workflows own; docky never claims them.
inline bool IsReserved(UINT modifiers, UINT vk) {
    const UINT m = modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    if (vk == VK_F12) return true;                                   // reserved for debuggers (RegisterHotKey docs)
    if (m == MOD_ALT && (vk == VK_TAB || vk == VK_F4 || vk == VK_ESCAPE || vk == VK_SPACE)) return true;
    if (m == (MOD_ALT | MOD_SHIFT) && vk == VK_TAB) return true;
    if (m == MOD_CONTROL && vk == VK_ESCAPE) return true;
    if (m == (MOD_CONTROL | MOD_SHIFT) && vk == VK_ESCAPE) return true;  // Task Manager
    if (m == (MOD_CONTROL | MOD_ALT) && (vk == VK_DELETE || vk == VK_TAB)) return true;
    if (vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_CANCEL) return true;
    return false;
}

inline Validity Validate(UINT modifiers, UINT vk) {
    if (!vk) return Validity::Empty;
    if (IsModifierKey(vk)) return Validity::ModifierOnly;
    if (modifiers & MOD_WIN) return Validity::UsesWinKey;         // Win+ shortcuts belong to Windows
    if (!(modifiers & (MOD_CONTROL | MOD_ALT))) return Validity::NeedsCtrlOrAlt;
    if (IsReserved(modifiers, vk)) return Validity::Reserved;
    return Validity::Ok;
}

inline const wchar_t* ValidityText(Validity v) {
    switch (v) {
    case Validity::Ok: return L"";
    case Validity::Empty: return L"No shortcut set.";
    case Validity::NeedsCtrlOrAlt: return L"Include Ctrl or Alt in the shortcut.";
    case Validity::UsesWinKey: return L"Windows key shortcuts are reserved by Windows.";
    case Validity::ModifierOnly: return L"Add a letter, number or function key.";
    case Validity::Reserved: return L"That shortcut is reserved by Windows.";
    }
    return L"";
}

inline std::wstring KeyName(UINT vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lparam = static_cast<LONG>(scan << 16);
    switch (vk) {  // extended keys need bit 24 for the right name
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK: lparam |= 1 << 24; break;
    default: break;
    }
    wchar_t name[64]{};
    if (scan && GetKeyNameTextW(lparam, name, 64) > 0) return name;
    return L"Key " + std::to_wstring(vk);
}

inline std::wstring Text(UINT modifiers, UINT vk) {
    if (!vk) return L"None";
    std::wstring s;
    if (modifiers & MOD_CONTROL) s += L"Ctrl + ";
    if (modifiers & MOD_ALT) s += L"Alt + ";
    if (modifiers & MOD_SHIFT) s += L"Shift + ";
    if (modifiers & MOD_WIN) s += L"Win + ";
    return s + KeyName(vk);
}
}  // namespace docky::hotkey
