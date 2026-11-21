#include "status_popups.h"

#include <shellapi.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "log.h"
#include "unified_menu.h"

namespace docky::statuspopup {
namespace {
WORD LangOf(HKL layout) { return LOWORD(reinterpret_cast<UINT_PTR>(layout)); }
bool Simplified(WORD lang) { return SUBLANGID(lang) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(lang) == SUBLANG_CHINESE_SINGAPORE; }
HWND Open(HWND owner, POINT anchor, bool above, std::vector<liquid::menu::MenuItem> items, liquid::popup::Select select, std::function<void()> closed) {
    liquid::popup::Closed onClose = [closed] { if (closed) closed(); };
    return above ? liquid::popup::ShowAbove(owner, anchor, std::move(items), std::move(select), std::move(onClose))
                 : liquid::popup::Show(owner, anchor, std::move(items), std::move(select), std::move(onClose));
}
}  // namespace

HKL ForegroundLayout(HKL last, HWND& target) {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
    if (!fg || pid == GetCurrentProcessId()) return last;  // our own window in front: keep the app's layout
    target = fg;
    return GetKeyboardLayout(tid);
}

std::wstring InputBadge(HKL layout) {
    const WORD lang = LangOf(layout);
    switch (PRIMARYLANGID(lang)) {
    case LANG_CHINESE: return Simplified(lang) ? L"拼" : L"注";  // 拼 / 注
    case LANG_JAPANESE: return L"あ";                                 // あ
    case LANG_KOREAN: return L"한";                                   // 한
    default: break;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{}, iso[16]{};
    if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) && GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME, iso, 16)) {
        std::wstring s = iso; for (auto& ch : s) ch = static_cast<wchar_t>(towupper(ch));
        return s.substr(0, 2);
    }
    return L"A";
}

std::wstring InputName(HKL layout) {
    const WORD lang = LangOf(layout);
    if (PRIMARYLANGID(lang) == LANG_CHINESE) return Simplified(lang) ? L"Pinyin – Simplified" : L"Zhuyin – Traditional";
    if (PRIMARYLANGID(lang) == LANG_JAPANESE) return L"Japanese – Romaji";
    if (PRIMARYLANGID(lang) == LANG_KOREAN) return L"2-Set Korean";
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{}, name[128]{};
    if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) && GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, 128)) return name;
    return L"Keyboard";
}

HWND ShowInputMenu(HWND owner, POINT anchor, bool above, HKL current, HWND target, std::function<void()> closed) {
    using liquid::menu::ItemKind;
    HKL list[32]{};
    const int n = GetKeyboardLayoutList(32, list);
    std::vector<HKL> layouts(list, list + (std::max)(0, n));
    std::vector<liquid::menu::MenuItem> items;
    for (size_t i = 0; i < layouts.size(); ++i) items.push_back({InputName(layouts[i]), ItemKind::Command, true, layouts[i] == current, {}, 0xE400 + static_cast<UINT>(i)});
    if (layouts.empty()) items.push_back({L"No keyboard layouts", ItemKind::Command, false, false, {}});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    items.push_back({L"Open Keyboard Settings…", ItemKind::Command, true, false, {}, 0xE3FF});
    DLOG(L"status: opening input menu (%d layouts)", static_cast<int>(layouts.size()));
    return Open(owner, anchor, above, std::move(items), [layouts, target](const liquid::menu::MenuItem& item) {
        if (item.commandId == 0xE3FF) { ShellExecuteW(nullptr, L"open", L"ms-settings:keyboard", nullptr, nullptr, SW_SHOWNORMAL); return; }
        if (item.commandId < 0xE400 || item.commandId - 0xE400 >= layouts.size()) return;
        if (target && IsWindow(target)) {
            PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(layouts[item.commandId - 0xE400]));
            DLOG(L"status: input layout change requested");
        }
    }, std::move(closed));
}

HWND ShowBatteryMenu(HWND owner, POINT anchor, bool above, const liquid::status::Snapshot& s, std::function<void()> openControlCenter, std::function<void()> closed) {
    using liquid::menu::ItemKind;
    using liquid::status::PowerSource;
    std::vector<liquid::menu::MenuItem> items;
    const std::wstring level = !s.batteryKnown ? L"Battery: Unknown" : !s.batteryPresent ? L"Battery: Not present"
                             : s.batteryPercent < 0 ? L"Battery: Unknown" : L"Battery: " + std::to_wstring(s.batteryPercent) + L"%";
    items.push_back({level, ItemKind::Command, false, false, {}});
    if (s.batteryPresent)
        items.push_back({s.charging ? L"Charging" : s.powerSource == PowerSource::AC ? L"Power Source: Power Adapter" : L"Power Source: Battery", ItemKind::Command, false, false, {}});
    const wchar_t* source = s.powerSource == PowerSource::AC ? L"Power source: AC" : s.powerSource == PowerSource::Battery ? L"Power source: Battery" : L"Power source: Unknown";
    items.push_back({source, ItemKind::Command, false, false, {}});
    items.push_back({L"", ItemKind::Separator, false, false, {}});
    if (openControlCenter) items.push_back({L"Open Control Center…", ItemKind::Command, true, false, {}, 0xE101});
    items.push_back({L"Battery Settings…", ItemKind::Command, true, false, {}, 0xE102});
    return Open(owner, anchor, above, std::move(items), [openControlCenter](const liquid::menu::MenuItem& item) {
        if (item.commandId == 0xE101 && openControlCenter) openControlCenter();
        else if (item.commandId == 0xE102) ShellExecuteW(nullptr, L"open", L"ms-settings:powersleep", nullptr, nullptr, SW_SHOWNORMAL);
    }, std::move(closed));
}

}  // namespace docky::statuspopup
