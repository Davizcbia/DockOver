#include "desktop_settings.h"
#include "json_lite.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cwctype>
namespace liquid::settings {
std::wstring DefaultPath() {
    PWSTR p=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&p))) return {};
    std::wstring path(p); CoTaskMemFree(p); return path+L"\\LiquidDesktop\\settings.json";
}
Result Load(const std::wstring& path) {
    Result r;
    if(path.empty()){r.status=LoadStatus::Unavailable;return r;}
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE){
        DWORD e=GetLastError();r.status=(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)?LoadStatus::Missing:LoadStatus::Unavailable;return r;
    }
    LARGE_INTEGER size{}; DWORD got=0;
    if(!GetFileSizeEx(f,&size)||size.QuadPart<0||size.QuadPart>1024*1024){CloseHandle(f);r.status=LoadStatus::Invalid;return r;}
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');
    bool ok=ReadFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)!=FALSE && got==bytes.size();
    CloseHandle(f);
    if(!ok){r.status=LoadStatus::Unavailable;return r;}
    dock::json::Value root; std::string error;
    if(!dock::json::Parse(bytes,root,error)||!root.IsObject()){r.status=LoadStatus::Invalid;return r;}
    const auto* version=root.Find("schemaVersion");
    if(!version||version->type!=dock::json::Value::Type::Number||version->number!=1){r.status=LoadStatus::Invalid;return r;}
    for(const char* name:{"dock","menuBar","launchpad","controlCenter","appearance","taskbar"}){
        const auto* section=root.Find(name);
        if(section&&!section->IsObject()){r.status=LoadStatus::Invalid;return r;}
    }
    if (const auto* phone = root.Find("phonePairing"); phone && phone->IsObject()) {
        if (const auto* radius = phone->Find("ballRadius"); radius && radius->type == dock::json::Value::Type::Number
            && radius->number >= kPhoneBallMinimum && radius->number <= kPhoneBallMaximum
            && std::floor(radius->number) == radius->number) {
            r.value.phoneBallRadius = static_cast<int>(radius->number);
        }
        if (const auto* folder = phone->Find("receiveFolder"); folder && folder->type == dock::json::Value::Type::String) {
            r.value.phoneReceiveFolder = dock::json::FromUtf8(folder->string);
        }
        if (const auto* bubble = phone->Find("pcReceiveBubble"); bubble && bubble->type == dock::json::Value::Type::Bool)
            r.value.pcReceiveBubble = bubble->boolean;
        if (const auto* background = phone->Find("runInBackground"); background && background->type == dock::json::Value::Type::Bool)
            r.value.phoneBackground = background->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* diag=section->Find("diagnostics")){
        if(diag->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;return r;}
        r.value.dock.diagnostics=diag->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* hide=section->Find("autoHide")){
        if(hide->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.autoHide=hide->boolean;
    }
    if(const auto* section=root.Find("dock")) {
        // The new key is ignored by older components, whose iconSize reader only accepts 24..80.
        const auto* iconSize=section->Find("iconSizeV2");
        if (!iconSize) iconSize=section->Find("iconSize");
        if(iconSize) {
            if(iconSize->type!=dock::json::Value::Type::Number || !std::isfinite(iconSize->number) || std::floor(iconSize->number)!=iconSize->number || iconSize->number<1 || iconSize->number>65535){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
            r.value.dock.iconSize=static_cast<int>(iconSize->number);
        }
    }
    if(const auto* section=root.Find("dock")) if(const auto* mag=section->Find("magnification");mag&&mag->type==dock::json::Value::Type::Number&&std::isfinite(mag->number))
        r.value.dock.magnification=std::clamp(static_cast<int>(std::lround(mag->number)),0,150);
    if(const auto* section=root.Find("dock")) if(const auto* count=section->Find("downloadsCount")){
        if(count->type!=dock::json::Value::Type::Number){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.downloadsCount=std::clamp(static_cast<int>(count->number),3,10);
    }
    if(const auto* section=root.Find("dock")) if(const auto* preview=section->Find("downloadsPreview")){
        if(preview->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.downloadsPreview=preview->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* vertical=section->Find("downloadsVertical")){
        if(vertical->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.downloadsVertical=vertical->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* launch=section->Find("launchDesktop")){
        if(launch->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.launchDesktop=launch->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* genie=section->Find("genieMinimize")){
        if(genie->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.genieMinimize=genie->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* snap=section->Find("hideSnapLayouts")){
        if(snap->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.hideSnapLayouts=snap->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* game=section->Find("gameCompatibility")){
        if(game->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.gameCompatibility=game->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* low=section->Find("lowLatencyGlass")){  // tolerant: a bad value keeps the default (off)
        if(low->type==dock::json::Value::Type::Bool)r.value.dock.lowLatencyGlass=low->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* mask=section->Find("useMacIconMask")){
        if(mask->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.useMacIconMask=mask->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* cells=section->Find("statusCells")){  // tolerant: bad entries are dropped
        if(cells->type==dock::json::Value::Type::Array){r.value.dock.statusCells.clear();
            for(const auto& c:cells->array){if(c.type!=dock::json::Value::Type::String)continue;const std::wstring id=dock::json::FromUtf8(c.string);
                if(ValidStatusCell(id)&&r.value.dock.statusCells.size()<16&&std::find(r.value.dock.statusCells.begin(),r.value.dock.statusCells.end(),id)==r.value.dock.statusCells.end())r.value.dock.statusCells.push_back(id);}}
    }
    if(const auto* section=root.Find("dock")) if(const auto* v=section->Find("statusTileStyle")){if(v->type==dock::json::Value::Type::Number&&(v->number==0||v->number==1||v->number==2))r.value.dock.statusTileStyle=static_cast<int>(v->number);}
    if(const auto* section=root.Find("dock")) if(const auto* pct=section->Find("statusBatteryPercent")){if(pct->type==dock::json::Value::Type::Bool)r.value.dock.statusBatteryPercent=pct->boolean;}
    if(const auto* section=root.Find("dock")){  // removable Dock items (tolerant: a wrong type keeps the default)
        if(const auto* v=section->Find("showDownloads"))if(v->type==dock::json::Value::Type::Bool)r.value.dock.showDownloads=v->boolean;
        if(const auto* v=section->Find("showTrash"))if(v->type==dock::json::Value::Type::Bool)r.value.dock.showTrash=v->boolean;
        if(const auto* v=section->Find("showDivider"))if(v->type==dock::json::Value::Type::Bool)r.value.dock.showDivider=v->boolean;
        if(const auto* v=section->Find("tailOrder"))if(v->type==dock::json::Value::Type::Array)
            for(const auto& k:v->array){if(k.type!=dock::json::Value::Type::String)continue;const std::wstring key=dock::json::FromUtf8(k.string);auto& order=r.value.dock.tailOrder;
                if(ValidTailKey(key)&&order.size()<8&&std::find(order.begin(),order.end(),key)==order.end())order.push_back(key);}
    }
    if(const auto* section=root.Find("menuBar")) if(const auto* on=section->Find("enabled")){if(on->type==dock::json::Value::Type::Bool)r.value.menuBar.enabled=on->boolean;}
    if (const auto* section = root.Find("menuBar"))
        if (const auto* gradient = section->Find("themeGradient"); gradient && gradient->type == dock::json::Value::Type::Bool)
            r.value.menuBar.themeGradient = gradient->boolean;
    if(const auto* section=root.Find("menuBar")) if(const auto* on=section->Find("captureNotifications")){if(on->type==dock::json::Value::Type::Bool)r.value.menuBar.captureNotifications=on->boolean;}
    if(const auto* section=root.Find("menuBar")){  // a value out of range is ignored (default), never invalidates the file
        auto integer=[&](const char* key,int lo,int hi,int& out){if(const auto* v=section->Find(key);v&&v->type==dock::json::Value::Type::Number&&std::isfinite(v->number)&&v->number>=lo&&v->number<=hi)out=static_cast<int>(v->number);};
        integer("gradientOpacity",0,100,r.value.menuBar.gradientOpacity);
        integer("gradientLength",40,100,r.value.menuBar.gradientLength);
        integer("notificationBlur",0,100,r.value.menuBar.notificationBlur);integer("notificationBlurFade",40,400,r.value.menuBar.notificationBlurFade);integer("notificationShade",0,100,r.value.menuBar.notificationShade);
    }
    if(const auto* section=root.Find("menuBar")) if(const auto* on=section->Find("hideSystemNotifications")){if(on->type==dock::json::Value::Type::Bool)r.value.menuBar.hideSystemNotifications=on->boolean;}
    if(const auto* section=root.Find("menuBar")) if(const auto* value=section->Find("scalePercent")){
        if(value->type!=dock::json::Value::Type::Number||!std::isfinite(value->number)||std::floor(value->number)!=value->number||value->number<80||value->number>160){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.menuBar.scalePercent=static_cast<int>(value->number);
    }
    if(const auto* section=root.Find("launchpad")){
        auto integer=[&](const char* key,int lo,int hi,int& out){const auto* v=section->Find(key);if(!v)return true;
            if(v->type!=dock::json::Value::Type::Number||!std::isfinite(v->number)||std::floor(v->number)!=v->number||v->number<lo||v->number>hi)return false;
            out=static_cast<int>(v->number);return true;};
        auto& v=r.value.launchpad;
        if(!integer("densityPreset",0,3,v.densityPreset)||!integer("iconSize",32,160,v.iconSize)||
           !integer("gapX",0,240,v.gapX)||!integer("gapY",0,240,v.gapY)){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        if(const auto* hide=section->Find("hideLabels")){if(hide->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}v.hideLabels=hide->boolean;}
        if(const auto* show=section->Find("showWindows")){if(show->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}v.showWindows=show->boolean;}
    }
    if(const auto* section=root.Find("performance")){  // an unknown value is ignored (unlimited), never invalidates the file
        auto cap=[&](const char* key,int& out){if(const auto* v=section->Find(key);v&&v->type==dock::json::Value::Type::Number&&std::isfinite(v->number)&&ValidFrameCap(static_cast<int>(v->number)))out=static_cast<int>(v->number);};
        cap("animationFps",r.value.performance.animationFps);cap("captureFps",r.value.performance.captureFps);
    }
    if(const auto* section=root.Find("updates")){  // a value of the wrong type is ignored (default), never invalidates the file
        auto flag=[&](const char* key,bool& out){if(const auto* v=section->Find(key);v&&v->type==dock::json::Value::Type::Bool)out=v->boolean;};
        flag("autoCheck",r.value.updates.autoCheck);flag("autoInstall",r.value.updates.autoInstall);
    }
    if(const auto* section=root.Find("controlCenter")) if(const auto* artwork=section->Find("mediaArtworkBackground")) {
        if(artwork->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.controlCenter.mediaArtworkBackground=artwork->boolean;
    }
    if(const auto* section=root.Find("controlCenter")) if(const auto* v=section->Find("musicWallpaper");v&&v->type==dock::json::Value::Type::Bool)
        r.value.controlCenter.musicWallpaper=v->boolean;  // wrong type: default (off), never invalidates the file
    if(const auto* section=root.Find("controlCenter")) if(const auto* v=section->Find("wallpaperAvoidBars");v&&v->type==dock::json::Value::Type::Bool)
        r.value.controlCenter.wallpaperAvoidBars=v->boolean;  // wrong type: default (on)
    if(const auto* section=root.Find("controlCenter")) if(const auto* v=section->Find("wallpaperLyrics");v&&v->type==dock::json::Value::Type::Bool)
        r.value.controlCenter.wallpaperLyrics=v->boolean;
    if(const auto* section=root.Find("controlCenter")) if(const auto* v=section->Find("lyricsFullArtwork");v&&v->type==dock::json::Value::Type::Bool)
        r.value.controlCenter.lyricsFullArtwork=v->boolean;
    if(const auto* section=root.Find("controlCenter")) if(const auto* v=section->Find("lyricsAutoReturn");v&&v->type==dock::json::Value::Type::Bool)
        r.value.controlCenter.lyricsAutoReturn=v->boolean;
    if(const auto* section=root.Find("controlCenter")){
        auto shortcut=[&](const char* key,int& out){if(const auto* v=section->Find(key);v&&v->type==dock::json::Value::Type::Number&&v->number>=0&&v->number<=0x7FF)out=static_cast<int>(v->number);};
        shortcut("shortcutLyrics",r.value.controlCenter.shortcutLyrics);shortcut("shortcutArtwork",r.value.controlCenter.shortcutArtwork);
        shortcut("shortcutDesktop",r.value.controlCenter.shortcutDesktop);shortcut("shortcutNowPlaying",r.value.controlCenter.shortcutNowPlaying);
        if (const auto* v = section->Find("animatedArtwork"); v && v->type == dock::json::Value::Type::Bool)
            r.value.controlCenter.animatedArtwork = v->boolean;
        if (const auto* v = section->Find("lyricsDoubleClickSeek"); v && v->type == dock::json::Value::Type::Bool)
            r.value.controlCenter.lyricsDoubleClickSeek = v->boolean;
        if (const auto* v = section->Find("keepArtworkWhenPaused"); v && v->type == dock::json::Value::Type::Bool)
            r.value.controlCenter.keepArtworkWhenPaused = v->boolean;
        if (const auto* v = section->Find("localMusicTags"); v && v->type == dock::json::Value::Type::Bool)
            r.value.controlCenter.localMusicTags = v->boolean;
        if (const auto* v = section->Find("hideDesktopIconsWithWidgets"); v && v->type == dock::json::Value::Type::Bool)
            r.value.controlCenter.hideDesktopIconsWithWidgets = v->boolean;
        using Json = dock::json::Value;
        if (const auto* entries = section->Find("audioOutputs"); entries && entries->type == Json::Type::Array) {
            auto& outputs = r.value.controlCenter.audioOutputs;
            for (const auto& entry : entries->array) {
                const auto* id = entry.Find("id");
                if (!id || id->type != Json::Type::String || id->string.empty()) continue;
                AudioOutputPreference output;
                output.id = dock::json::FromUtf8(id->string);
                if (output.id.empty() || std::any_of(outputs.begin(), outputs.end(),
                    [&](const auto& existing) { return existing.id == output.id; })) continue;
                if (const auto* name = entry.Find("name"); name && name->type == Json::Type::String)
                    output.name = dock::json::FromUtf8(name->string);
                if (const auto* icon = entry.Find("icon"); icon && icon->type == Json::Type::String)
                    output.icon = icon->string;
                if (const auto* hidden = entry.Find("hidden"); hidden && hidden->type == Json::Type::Bool)
                    output.hidden = hidden->boolean;
                outputs.push_back(std::move(output));
            }
        }
        if(const auto* v=section->Find("lyricsBeat");v&&v->type==dock::json::Value::Type::Bool)r.value.controlCenter.lyricsBeat=v->boolean;
        if(const auto* v=section->Find("musicSensitivity");v&&v->type==dock::json::Value::Type::Number&&
           v->number>=0&&v->number<=100)r.value.controlCenter.musicSensitivity=static_cast<int>(v->number);
        if(const auto* v=section->Find("musicResponse");v&&v->type==dock::json::Value::Type::Number&&
           (v->number==0||v->number==1||v->number==2))r.value.controlCenter.musicResponse=static_cast<int>(v->number);
        if(const auto* v=section->Find("musicFps");v&&v->type==dock::json::Value::Type::Number){const int f=static_cast<int>(v->number);if(f==0||f==30||f==60||f==120||f==240)r.value.controlCenter.musicFps=f;}
        if(const auto* v=section->Find("lyricsOffsetMs");v&&v->type==dock::json::Value::Type::Number&&v->number>=-3000&&v->number<=3000)r.value.controlCenter.lyricsOffsetMs=static_cast<int>(v->number);
    }
    if(const auto* section=root.Find("controlCenter")) if(const auto* blur=section->Find("backdropBlur")){
        if(blur->type!=dock::json::Value::Type::Number||!std::isfinite(blur->number)||std::floor(blur->number)!=blur->number||blur->number<0||blur->number>100){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.controlCenter.backdropBlur=static_cast<int>(blur->number);
    }
    if(const auto* section=root.Find("appearance")){
        if(const auto* lang=section->Find("language");lang&&lang->type==dock::json::Value::Type::String&&(lang->string=="en"||lang->string=="zh-CN"))r.value.appearance.language=lang->string;
        if(const auto* order=section->Find("applicationsSort");order&&order->type==dock::json::Value::Type::Number&&(order->number==1||order->number==2))
            r.value.appearance.applicationsSort=static_cast<int>(order->number);
        for(const auto* key:{"settingsScalePercent","applicationsScalePercent"})if(const auto* v=section->Find(key)){
            const bool settings=std::strcmp(key,"settingsScalePercent")==0;const int lo=settings?75:60,hi=settings?125:120;
            if(v->type!=dock::json::Value::Type::Number||!std::isfinite(v->number)||std::floor(v->number)!=v->number||v->number<lo||v->number>hi){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
            (settings?r.value.appearance.settingsScalePercent:r.value.appearance.applicationsScalePercent)=static_cast<int>(v->number);
        }
    }
    if(const auto* section=root.Find("appearance")) if(const auto* mode=section->Find("mode")){
        if(mode->type!=dock::json::Value::Type::Number || !std::isfinite(mode->number) || std::floor(mode->number)!=mode->number || mode->number<0 || mode->number>2){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.appearance.mode=static_cast<int>(mode->number);
    }
    if(const auto* section=root.Find("taskbar")){
        auto& t=r.value.taskbar;
        auto boolean=[&](const char* key,bool& out){const auto* v=section->Find(key);if(!v)return true;if(v->type!=dock::json::Value::Type::Bool)return false;out=v->boolean;return true;};
        auto integer=[&](const char* key,int lo,int hi,int& out){const auto* v=section->Find(key);if(!v)return true;
            if(v->type!=dock::json::Value::Type::Number||!std::isfinite(v->number)||std::floor(v->number)!=v->number||v->number<lo||v->number>hi)return false;
            out=static_cast<int>(v->number);return true;};
        if(!boolean("hideOnStartup",t.hideOnStartup)||!boolean("hotkeyEnabled",t.hotkeyEnabled)||!boolean("freeWorkArea",t.freeWorkArea)||
           !integer("hotkeyModifiers",0,7,t.hotkeyModifiers)||!integer("hotkeyKey",0,254,t.hotkeyKey)){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
    }
    r.status=LoadStatus::Loaded;return r;
}
namespace {
struct Member { std::string key; size_t begin, end; };
void SkipWs(const std::string& s,size_t& p){while(p<s.size()&&(s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t'))++p;}
bool StringEnd(const std::string& s,size_t& p) {
    if(p>=s.size()||s[p++]!='"')return false;
    while(p<s.size()) {
        unsigned char c=static_cast<unsigned char>(s[p++]);
        if(c=='"')return true;
        if(c<0x20)return false;
        if(c=='\\') {if(p>=s.size())return false; char e=s[p++];if(e=='u'){for(int i=0;i<4;++i){if(p>=s.size())return false;char h=s[p++];if(!((h>='0'&&h<='9')||(h>='a'&&h<='f')||(h>='A'&&h<='F')))return false;}}else if(std::string("\"\\/bfnrt").find(e)==std::string::npos)return false;}
    }
    return false;
}
bool ValueEnd(const std::string& s,size_t& p,int depth);
bool ObjectMembers(const std::string& s,size_t& p,std::vector<Member>* members,int depth) {
    if(depth>32||p>=s.size()||s[p++]!='{')return false;
    SkipWs(s,p); if(p<s.size()&&s[p]=='}'){++p;return true;}
    while(p<s.size()){
        size_t start=p;if(!StringEnd(s,p))return false;
        dock::json::Value k;std::string err;
        if(!dock::json::Parse(s.substr(start,p-start),k,err)||k.type!=dock::json::Value::Type::String)return false;
        SkipWs(s,p);if(p>=s.size()||s[p++]!=':')return false;SkipWs(s,p);
        size_t begin=p;if(!ValueEnd(s,p,depth+1))return false;
        if(members){for(const auto& m:*members)if(m.key==k.string)return false;members->push_back({k.string,begin,p});}
        SkipWs(s,p);if(p>=s.size())return false;
        if(s[p]=='}'){++p;return true;} if(s[p++]!=',')return false;SkipWs(s,p);
    }
    return false;
}
bool ValueEnd(const std::string& s,size_t& p,int depth) {
    if(depth>32||p>=s.size())return false;
    if(s[p]=='"')return StringEnd(s,p);
    if(s[p]=='{')return ObjectMembers(s,p,nullptr,depth+1);
    if(s[p]=='['){++p;SkipWs(s,p);if(p<s.size()&&s[p]==']'){++p;return true;}
        while(p<s.size()){if(!ValueEnd(s,p,depth+1))return false;SkipWs(s,p);if(p>=s.size())return false;if(s[p]==']'){++p;return true;}if(s[p++]!=',')return false;SkipWs(s,p);}return false;}
    for(const char* lit:{"true","false","null"}){size_t n=strlen(lit);if(s.compare(p,n,lit)==0){p+=n;return true;}}
    size_t q=p;if(s[q]=='-')++q;if(q>=s.size())return false;
    if(s[q]=='0')++q;else if(s[q]>='1'&&s[q]<='9'){while(q<s.size()&&s[q]>='0'&&s[q]<='9')++q;}else return false;
    if(q<s.size()&&s[q]=='.'){++q;size_t b=q;while(q<s.size()&&s[q]>='0'&&s[q]<='9')++q;if(q==b)return false;}
    if(q<s.size()&&(s[q]=='e'||s[q]=='E')){++q;if(q<s.size()&&(s[q]=='+'||s[q]=='-'))++q;size_t b=q;while(q<s.size()&&s[q]>='0'&&s[q]<='9')++q;if(q==b)return false;}
    p=q;return true;
}
const Member* Find(const std::vector<Member>& list,const std::string& key){for(const auto& m:list)if(m.key==key)return &m;return nullptr;}
bool ReadBytes(const std::wstring& path,std::string& bytes,bool& missing) {
    missing=false; HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE){DWORD e=GetLastError();missing=(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND);return missing;}
    LARGE_INTEGER n{};DWORD got=0;
    bool ok=GetFileSizeEx(h,&n)&&n.QuadPart>=0&&n.QuadPart<=1024*1024;
    if(ok){bytes.resize(static_cast<size_t>(n.QuadPart));ok=ReadFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size();}
    CloseHandle(h);return ok;
}
bool Category(const std::string& s){for(const char* c:{"dock","menuBar","launchpad","controlCenter","appearance","taskbar","performance","updates","phonePairing"})if(s==c)return true;return false;}
std::string Encode(const std::string& s){return dock::json::Write(dock::json::Value::MakeString(s));}
}
// kind: 0 number, 1 boolean, 2 any JSON value (an existing value of a different kind is still refused for 0 / 1).
static SaveStatus WriteScalar(const std::wstring& path,const std::string& section,const std::string& key,const std::string& value,int kind) {
    const bool boolean=kind==1;
    if(path.empty()||!Category(section)||key.empty()||key=="schemaVersion")return SaveStatus::Invalid;
    size_t slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return SaveStatus::Unavailable;
    std::wstring dir=path.substr(0,slash);
    if(!CreateDirectoryW(dir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return SaveStatus::Unavailable;
    std::wstring lockPath=path+L".lock";HANDLE lock=INVALID_HANDLE_VALUE;
    for(int i=0;i<100;++i){lock=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(lock!=INVALID_HANDLE_VALUE)break;if(GetLastError()!=ERROR_SHARING_VIOLATION)return SaveStatus::Unavailable;Sleep(50);}
    if(lock==INVALID_HANDLE_VALUE)return SaveStatus::Busy;
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{lock};
    std::string bytes;bool missing=false;if(!ReadBytes(path,bytes,missing))return SaveStatus::Unavailable;
    if(missing)bytes="{\"schemaVersion\":1}";
    else if(Load(path).status!=LoadStatus::Loaded)return SaveStatus::Invalid;
    size_t pos=0;SkipWs(bytes,pos);std::vector<Member> root;
    if(!ObjectMembers(bytes,pos,&root,0))return SaveStatus::Invalid;
    const size_t rootEnd=pos;
    SkipWs(bytes,pos);if(pos!=bytes.size())return SaveStatus::Invalid;
    const auto* found=Find(root,section);
    std::string insert=Encode(key)+":"+value;
    if(found){
        size_t end=found->begin;std::vector<Member> members;
        if(!ObjectMembers(bytes,end,&members,0)||end!=found->end)return SaveStatus::Invalid;
        const auto* existing=Find(members,key);
        if(existing){
            const auto old=bytes.substr(existing->begin,existing->end-existing->begin);
            if(boolean){if(old!="true"&&old!="false")return SaveStatus::Invalid;}
            else if(kind==0){dock::json::Value parsed;std::string err;if(!dock::json::Parse(old,parsed,err)||parsed.type!=dock::json::Value::Type::Number)return SaveStatus::Invalid;}
            if(old==value)return SaveStatus::Saved;
            bytes.replace(existing->begin,existing->end-existing->begin,value);
        }else bytes.insert(found->end-1,(members.empty()?"":",")+insert);
    }else bytes.insert(rootEnd-1,(root.empty()?"":",")+Encode(section)+":{"+insert+"}");
    if(bytes.size()>1024*1024)return SaveStatus::Invalid;
    static std::atomic<unsigned long> seq{0};std::wstring temp;
    HANDLE out=INVALID_HANDLE_VALUE;
    for(int i=0;i<10&&out==INVALID_HANDLE_VALUE;++i){
        temp=path+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++seq);
        out=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    if(out==INVALID_HANDLE_VALUE)return SaveStatus::Unavailable;
    DWORD written=0;bool ok=WriteFile(out,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(out);
    if(!CloseHandle(out))ok=false;
    if(ok)ok=MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok){DeleteFileW(temp.c_str());return SaveStatus::Unavailable;}
    return SaveStatus::Saved;
}
SaveStatus WriteBool(const std::wstring& path,const std::string& section,const std::string& key,bool value){
 return WriteScalar(path,section,key,value?"true":"false",true);
}
SaveStatus WritePhoneBallRadius(const std::wstring& path, int radius) {
    if (radius < kPhoneBallMinimum || radius > kPhoneBallMaximum) return SaveStatus::Invalid;
    return WriteScalar(path, "phonePairing", "ballRadius", std::to_string(radius), 0);
}
SaveStatus WritePhoneReceiveFolder(const std::wstring& path, const std::wstring& folder) {
    const auto utf8 = dock::json::ToUtf8(folder);
    return WriteScalar(path, "phonePairing", "receiveFolder", Encode(utf8), 2);
}
SaveStatus WritePerformanceInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="animationFps"||key=="captureFps")&&ValidFrameCap(value);
 return valid?WriteScalar(path,"performance",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteDockInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="downloadsCount"&&value>=3&&value<=10)||(key=="magnification"&&value>=0&&value<=150)||(key=="iconSizeV2"&&value>=1&&value<=65535)||(key=="statusTileStyle"&&value>=0&&value<=2);
 return valid?WriteScalar(path,"dock",key,std::to_string(value),false):SaveStatus::Invalid;
}
bool ValidStatusCell(const std::wstring& id){
 for(const wchar_t* k:{L"combo",L"analog",L"date",L"digital",L"control",L"wifi",L"bluetooth",L"sound",L"battery",L"input",L"clock",L"tray"})if(id==k)return true;
 if(id.rfind(L"tray:",0)==0){  // a tray icon kept in the Dock: its tray key "tray:<exe name>#<uid or guid>" (tray_host.h)
  const size_t hash=id.find(L'#');if(id.size()>200||hash==std::wstring::npos||hash<6||hash+1>=id.size())return false;
  for(size_t i=5;i<id.size();++i){const wchar_t c=id[i];if(c<0x20||c==L'"'||c==L'\\'||c==L'/'||c==0x7f)return false;}
  return true;}
 if(id.rfind(L"ext:",0)!=0||id.size()<5||id.size()>132)return false;
 for(size_t i=4;i<id.size();++i){const wchar_t c=id[i];if(!(iswalnum(c)||c==L'.'||c==L'-'||c==L'_'))return false;}
 return true;
}
SaveStatus WriteStatusCells(const std::wstring& path,const std::vector<std::wstring>& cells){
 if(cells.size()>16)return SaveStatus::Invalid;
 dock::json::Value list;list.type=dock::json::Value::Type::Array;
 for(size_t i=0;i<cells.size();++i){if(!ValidStatusCell(cells[i])||std::find(cells.begin(),cells.begin()+static_cast<std::ptrdiff_t>(i),cells[i])!=cells.begin()+static_cast<std::ptrdiff_t>(i))return SaveStatus::Invalid;
  list.array.push_back(dock::json::Value::MakeString(dock::json::ToUtf8(cells[i])));}
 return WriteScalar(path,"dock","statusCells",dock::json::Write(list,0),2);
}
bool ValidTailKey(const std::wstring& key){
 for(const wchar_t* k:{L"status",L"status:combo",L"status:analog",L"status:date",L"status:digital",L"status:tray",L"downloads",L"trash"})if(key==k)return true;
 return false;
}
SaveStatus WriteTailOrder(const std::wstring& path,const std::vector<std::wstring>& keys){
 if(keys.size()>8)return SaveStatus::Invalid;
 dock::json::Value list;list.type=dock::json::Value::Type::Array;
 for(size_t i=0;i<keys.size();++i){if(!ValidTailKey(keys[i])||std::find(keys.begin(),keys.begin()+static_cast<std::ptrdiff_t>(i),keys[i])!=keys.begin()+static_cast<std::ptrdiff_t>(i))return SaveStatus::Invalid;
  list.array.push_back(dock::json::Value::MakeString(dock::json::ToUtf8(keys[i])));}
 return WriteScalar(path,"dock","tailOrder",dock::json::Write(list,0),2);
}
SaveStatus WriteDensityInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="densityPreset"&&value>=0&&value<=3)||(key=="iconSize"&&value>=32&&value<=160)||
                  ((key=="gapX"||key=="gapY")&&value>=0&&value<=240);
 return valid?WriteScalar(path,"launchpad",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteMenuBarScale(const std::wstring& path,int percent){
 return percent>=80&&percent<=160?WriteScalar(path,"menuBar","scalePercent",std::to_string(percent),false):SaveStatus::Invalid;
}
SaveStatus WriteMenuBarInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="gradientOpacity"&&value>=0&&value<=100)||(key=="gradientLength"&&value>=40&&value<=100)||(key=="notificationBlur"&&value>=0&&value<=100)||(key=="notificationBlurFade"&&value>=40&&value<=400)||(key=="notificationShade"&&value>=0&&value<=100);
 return valid?WriteScalar(path,"menuBar",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteAudioOutputs(const std::wstring& path, const std::vector<AudioOutputPreference>& outputs) {
    using Json = dock::json::Value;
    Json list;
    list.type = Json::Type::Array;
    for (const auto& output : outputs) {
        auto entry = Json::MakeObject();
        entry.object.emplace_back("id", Json::MakeString(dock::json::ToUtf8(output.id)));
        if (!output.name.empty()) entry.object.emplace_back("name", Json::MakeString(dock::json::ToUtf8(output.name)));
        if (!output.icon.empty()) entry.object.emplace_back("icon", Json::MakeString(output.icon));
        if (output.hidden) entry.object.emplace_back("hidden", Json::MakeBool(true));
        list.array.push_back(std::move(entry));
    }
    return WriteScalar(path, "controlCenter", "audioOutputs", dock::json::Write(list, 0), 2);
}
SaveStatus WriteControlCenterInt(const std::wstring& path,const std::string& key,int value){
 const bool shortcut=key=="shortcutLyrics"||key=="shortcutArtwork"||key=="shortcutDesktop"||key=="shortcutNowPlaying";
 const bool valid=(key=="backdropBlur"&&value>=0&&value<=100)||(shortcut&&value>=0&&value<=0x7FF)||(key=="lyricsOffsetMs"&&value>=-3000&&value<=3000)||(key=="musicSensitivity"&&value>=0&&value<=100)||(key=="musicResponse"&&value>=0&&value<=2)||(key=="musicFps"&&(value==0||value==30||value==60||value==120||value==240));
 return valid?WriteScalar(path,"controlCenter",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteApplicationsSort(const std::wstring& path,int order){
 return order>=0&&order<=2?WriteScalar(path,"appearance","applicationsSort",std::to_string(order),false):SaveStatus::Invalid;
}
SaveStatus WriteLanguage(const std::wstring& path,const std::string& language){
 return (language=="en"||language=="zh-CN")?WriteScalar(path,"appearance","language",Encode(language),2):SaveStatus::Invalid;
}
SaveStatus WriteUiScale(const std::wstring& path,const std::string& key,int percent){
 const bool valid=(key=="settingsScalePercent"&&percent>=75&&percent<=125)||(key=="applicationsScalePercent"&&percent>=60&&percent<=120);
 return valid?WriteScalar(path,"appearance",key,std::to_string(percent),false):SaveStatus::Invalid;
}
SaveStatus WriteTaskbarInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="hotkeyModifiers"&&value>=0&&value<=7)||(key=="hotkeyKey"&&value>=0&&value<=254);
 return valid?WriteScalar(path,"taskbar",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteAppearanceMode(const std::wstring& path,int mode){
 return mode>=0&&mode<=2?WriteScalar(path,"appearance","mode",std::to_string(mode),false):SaveStatus::Invalid;
}
BackupStatus BackupLegacy(const std::wstring& source) {
    if(source.empty())return BackupStatus::Unavailable;
    const auto backup=source+L".liquid-desktop-v02.bak";
    if(GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES)return BackupStatus::AlreadyExists;
    HANDLE in=CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(in==INVALID_HANDLE_VALUE)return BackupStatus::Unavailable;
    static std::atomic<unsigned long> seq{0};
    auto temp=backup+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++seq);
    HANDLE out=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(out==INVALID_HANDLE_VALUE){CloseHandle(in);return BackupStatus::Unavailable;}
    bool ok=true; char buf[65536]; DWORD n=0,w=0;
    while(ok){if(!ReadFile(in,buf,sizeof(buf),&n,nullptr)){ok=false;break;}if(!n)break;
        if(!WriteFile(out,buf,n,&w,nullptr)||w!=n)ok=false;}
    if(ok)ok=FlushFileBuffers(out)!=FALSE;
    if(!CloseHandle(out))ok=false;
    CloseHandle(in);
    if(ok)ok=MoveFileExW(temp.c_str(),backup.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok){DWORD e=GetLastError();DeleteFileW(temp.c_str());if(e==ERROR_ALREADY_EXISTS||e==ERROR_FILE_EXISTS)return BackupStatus::AlreadyExists;return BackupStatus::Unavailable;}
    return BackupStatus::Created;
}
Revision ReadRevision(const std::wstring& path) {
    Revision r;HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return r;BY_HANDLE_FILE_INFORMATION x{};
    if(GetFileInformationByHandle(h,&x)){r.present=true;r.volume=x.dwVolumeSerialNumber;r.indexHigh=x.nFileIndexHigh;r.indexLow=x.nFileIndexLow;r.timeHigh=x.ftLastWriteTime.dwHighDateTime;r.timeLow=x.ftLastWriteTime.dwLowDateTime;r.sizeHigh=x.nFileSizeHigh;r.sizeLow=x.nFileSizeLow;}
    CloseHandle(h);return r;
}
bool operator==(const Revision& a,const Revision& b){return a.present==b.present&&a.volume==b.volume&&a.indexHigh==b.indexHigh&&a.indexLow==b.indexLow&&a.timeHigh==b.timeHigh&&a.timeLow==b.timeLow&&a.sizeHigh==b.sizeHigh&&a.sizeLow==b.sizeLow;}

}
