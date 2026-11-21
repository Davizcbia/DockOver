#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>
namespace liquid::settings {
inline std::vector<int> SearchCategories(std::wstring query) {
    std::transform(query.begin(),query.end(),query.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    const wchar_t* keywords[]={L"general accessibility animation windows visual effects frame rate fps refresh performance capture",L"appearance theme language english chinese settings interface size scale applications window windows key sort name frequently used recent",L"dock icon mask automatically hide diagnostics",L"menu bar size scale text icons theme gradient wallpaper contrast height game gaming compatibility fullscreen full screen notification notifications center banner banners toast clock hide windows popup",L"launchpad icon density size spacing background wallpaper windows",L"control center",L"windows taskbar visibility auto hide show restore startup hotkey shortcut keyboard guardian",L"workshop import export resource packs",L"migration legacy backup",L"about DockOver version",L"music now playing wallpaper lyrics artwork album cover animation animated motion double click seek jump apple spotify netease qq flow silk beat timing offset shortcut frame rate"};
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
