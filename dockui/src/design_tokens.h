#pragma once
// Liquid Desktop design tokens, V0.1. Sizes are DIPs; RGB values are sRGB.
// These are design roles, not platform or Apple system constants.
#include <cstdint>
namespace liquid::design {
namespace type {
// Family is for DirectWrite surfaces (DirectWrite falls back by itself where the family is absent).
// FamilyGdi is for GDI / GDI+ paths, which resolve the static family more reliably.
inline constexpr wchar_t Family[] = L"Segoe UI Variable Text";
inline constexpr wchar_t FamilyGdi[] = L"Segoe UI";
inline constexpr float Title=26.f, Body=14.f, Secondary=12.f, Caption=11.f, Numeric=14.f;
inline constexpr float Field=15.f, Emphasis=13.5f, Label=13.f;
}
namespace space {
inline constexpr float Xs=4.f, S=8.f, M=12.f, L=16.f, Xl=24.f, Xxl=32.f;
}
namespace radius {
inline constexpr float Window=16.f, Card=12.f, Button=8.f, Menu=12.f, Pill=14.f;
}
struct Palette {
    std::uint32_t background, navigation, surface, primary, secondary, divider;
    std::uint32_t accent, icon, selected, hover, pressed, disabled, focused, switchOff, onAccent;
};
inline constexpr Palette Light{
    0xF4F4F7,0xE6E7EB,0xFFFFFF,0x20232A,0x595D69,0xC7CBD2,
    0x386BD6,0x707B8E,0xCBD2DE,0xD7DBE4,0xBFC7D4,0x717785,0x2457C5,0x777D88,0xFFFFFF
};
inline constexpr Palette Dark{
    0x202127,0x282A31,0x32343D,0xF5F5F8,0xC5C8D2,0x565A65,
    0x8BAFFF,0xADB5C5,0x414D62,0x41444F,0x515767,0xB2B6C0,0xB2C9FF,0x858C9B,0xFFFFFF
};
namespace interaction {
inline constexpr float HoverOpacity=0.08f, PressedOpacity=0.16f, SelectedOpacity=1.f;
inline constexpr float DisabledOpacity=0.55f, FocusStroke=2.f;
}
// Semantic roles only. Rendering and accessibility fallbacks belong to each surface.
enum class MaterialRole { Navigation, Content, Popover, CompactControl, Fallback };
struct MaterialSpec { float tintOpacity; bool translucent; bool backdrop; };
inline constexpr MaterialSpec Material(MaterialRole role) {
    switch(role) {
    case MaterialRole::Navigation: return {0.82f,true,true};
    case MaterialRole::Content: return {1.f,false,false};
    case MaterialRole::Popover: return {0.72f,true,true};
    case MaterialRole::CompactControl: return {0.85f,true,true};
    default: return {1.f,false,false};
    }
}
namespace settings {
inline constexpr float SidebarWidth=282.f;
inline constexpr float SidebarTop=176.f;
inline constexpr float SidebarStep=40.f;
inline constexpr float SidebarRow=space::Xxl+space::Xs;
inline constexpr float ContentLeft=SidebarWidth+space::Xxl;
inline constexpr float ContentRight=space::Xxl;
inline constexpr float ContentTop=18.f;
inline constexpr float GroupCaptionTop=SidebarTop-space::S;
inline constexpr float CardTop=80.f;
inline constexpr float CardHeight=space::Xxl*2+space::M;
inline constexpr float SwitchWidth=space::Xxl+space::L, SwitchHeight=space::Xxl-space::Xs;
inline constexpr float SwitchTop=CardTop+space::Xl-space::Xs;
inline constexpr float SwitchRight=space::L+space::Xs;
inline constexpr float MinWidth=760.f, MinHeight=560.f;
}
}
