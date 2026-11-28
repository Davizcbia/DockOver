#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>
namespace liquid::settings {
inline std::vector<int> SearchCategories(std::wstring query) {
    std::transform(query.begin(),query.end(),query.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    const wchar_t* keywords[]={L"general accessibility animation windows visual effects frame rate fps refresh performance capture",L"appearance theme language english chinese settings interface size scale applications window windows key sort name frequently used recent launchpad only applications filter \u4EC5\u663E\u793A \u542F\u52A8\u53F0",L"dock icon mask lock dragging drag reorder 锁定 拖动 automatically hide diagnostics bottom offset gap discrete integrated gpu graphics windows system status tray clock date time battery wifi wi-fi bluetooth \u7A0B\u5E8F\u575E \u5E95\u90E8 \u504F\u79FB \u72EC\u7ACB\u663E\u5361 \u96C6\u6210\u663E\u5361",L"menu bar size scale text icons theme gradient length network speed upload download temperature cpu gpu sensor 网速 温度 渐变 wallpaper contrast height game gaming compatibility fullscreen full screen notification notifications center banner banners toast clock hide windows popup",L"launchpad icon density size spacing background wallpaper windows",L"control center",L"windows taskbar visibility auto hide show restore startup hotkey shortcut keyboard guardian",L"workshop import export resource packs",L"migration legacy backup",L"about dockover version check for update updates updater install automatic auto download release \u5173\u4E8E \u66F4\u65B0 \u5347\u7EA7 \u7248\u672C",L"music now playing wallpaper lyrics artwork album cover animation animated motion double click seek jump apple spotify netease qq flow silk beat timing offset shortcut frame rate"};
    std::vector<std::wstring> terms;
    for(size_t i=0;i<query.size();) {
        while(i<query.size() && std::iswspace(query[i])) ++i;
        const size_t start=i;while(i<query.size() && !std::iswspace(query[i])) ++i;
        if(i>start)terms.push_back(query.substr(start,i-start));
    }
    std::vector<int> result;
    const int order[]={0,1,2,3,4,5,10,6,7,8,9};  // Music sits under Control Center
    for(int i:order){const std::wstring text=keywords[i];bool matches=true;
        for(const auto& term:terms)if(text.find(term)==std::wstring::npos){matches=false;break;}
        if(matches)result.push_back(i);
    }
    return result;
}
}
