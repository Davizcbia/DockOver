#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <cstdint>

namespace liquid::menu {
enum class ItemKind { Command, Submenu, Separator };
// Optional row icon (premultiplied BGRA). templ: monochrome artwork drawn in the row's text colour.
struct Icon { UINT w=0,h=0; std::vector<std::uint32_t> pixels; bool templ=false; };
enum class ProviderKind { Internal, Win32Native, Automation, AppAdapter, Unsupported };
struct MenuItem { std::wstring text; ItemKind kind=ItemKind::Command; bool enabled=false; bool checked=false; std::vector<MenuItem> children; UINT commandId=0; std::vector<UINT> path; std::wstring rawText; std::shared_ptr<const Icon> icon; };
struct Menu { std::wstring title; std::vector<MenuItem> items; ProviderKind source=ProviderKind::Unsupported; HWND target=nullptr; DWORD targetPid=0; HMENU root=nullptr; };
struct MenuBar { std::vector<Menu> menus; };
class Provider { public: virtual ~Provider()=default; virtual std::vector<Menu> GetMenus(HWND foreground) const=0; };
class InternalProvider final : public Provider {
public:
    std::vector<Menu> GetMenus(HWND) const override {
        Menu m; m.title=L"DockOver"; m.source=ProviderKind::Internal;
        m.items={{L"About DockOver",ItemKind::Command,false,false,{}},
                 {L"Settings",ItemKind::Command,true,false,{},0xF0F2},
                 {L"",ItemKind::Separator,false,false,{}},
                 {L"Quit DockOver",ItemKind::Command,true,false,{},0xF0F1}};
        return {std::move(m)};
    }
};
// Unknown apps must not be represented by invented File/Edit menus. Native HMENU,
// UI Automation and application-specific providers can be added before this fallback.
class UnsupportedProvider final : public Provider {
public: std::vector<Menu> GetMenus(HWND) const override { return {}; }
};
inline MenuBar Build(HWND foreground) {
    InternalProvider internal;UnsupportedProvider fallback;
    MenuBar result{internal.GetMenus(foreground)};auto extra=fallback.GetMenus(foreground);
    for(auto& menu:extra)result.menus.push_back(std::move(menu));return result;
}
}
