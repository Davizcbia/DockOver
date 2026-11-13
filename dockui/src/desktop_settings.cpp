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
    if(const auto* section=root.Find("dock")) if(const auto* diag=section->Find("diagnostics")){
        if(diag->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;return r;}
        r.value.dock.diagnostics=diag->boolean;
    }
    if(const auto* section=root.Find("dock")) if(const auto* hide=section->Find("autoHide")){
        if(hide->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.autoHide=hide->boolean;
    }
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
    if(const auto* section=root.Find("dock")) if(const auto* mask=section->Find("useMacIconMask")){
        if(mask->type!=dock::json::Value::Type::Bool){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.dock.useMacIconMask=mask->boolean;
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
    if(const auto* section=root.Find("controlCenter")) if(const auto* blur=section->Find("backdropBlur")){
        if(blur->type!=dock::json::Value::Type::Number||!std::isfinite(blur->number)||std::floor(blur->number)!=blur->number||blur->number<0||blur->number>100){r.status=LoadStatus::Invalid;r.value=Settings{};return r;}
        r.value.controlCenter.backdropBlur=static_cast<int>(blur->number);
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
bool Category(const std::string& s){for(const char* c:{"dock","menuBar","launchpad","controlCenter","appearance","taskbar","performance","updates"})if(s==c)return true;return false;}
std::string Encode(const std::string& s){return dock::json::Write(dock::json::Value::MakeString(s));}
}
static SaveStatus WriteScalar(const std::wstring& path,const std::string& section,const std::string& key,const std::string& value,bool boolean) {
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
            else {dock::json::Value parsed;std::string err;if(!dock::json::Parse(old,parsed,err)||parsed.type!=dock::json::Value::Type::Number)return SaveStatus::Invalid;}
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
SaveStatus WritePerformanceInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="animationFps"||key=="captureFps")&&ValidFrameCap(value);
 return valid?WriteScalar(path,"performance",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteDockInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=key=="downloadsCount"&&value>=3&&value<=10;
 return valid?WriteScalar(path,"dock",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteDensityInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=(key=="densityPreset"&&value>=0&&value<=3)||(key=="iconSize"&&value>=32&&value<=160)||
                  ((key=="gapX"||key=="gapY")&&value>=0&&value<=240);
 return valid?WriteScalar(path,"launchpad",key,std::to_string(value),false):SaveStatus::Invalid;
}
SaveStatus WriteControlCenterInt(const std::wstring& path,const std::string& key,int value){
 const bool valid=key=="backdropBlur"&&value>=0&&value<=100;
 return valid?WriteScalar(path,"controlCenter",key,std::to_string(value),false):SaveStatus::Invalid;
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
