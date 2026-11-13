#include "native_menu.h"
#include <algorithm>
#include <string>

namespace liquid::menu {
namespace {
constexpr UINT kMaxItems=120, kMaxDepth=8;
struct Raw { UINT type=0,state=0,id=0; HMENU sub=nullptr; std::wstring text; };
bool Read(HMENU menu,UINT pos,Raw& out){
    wchar_t text[512]{}; MENUITEMINFOW mi{sizeof(mi)};
    mi.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_ID|MIIM_SUBMENU|MIIM_STRING;mi.dwTypeData=text;mi.cch=ARRAYSIZE(text);
    if(!GetMenuItemInfoW(menu,pos,TRUE,&mi) || mi.cch>=ARRAYSIZE(text)-1)return false;
    out={mi.fType,mi.fState,mi.wID,mi.hSubMenu,std::wstring(text)};return true;
}
bool Enabled(const Raw& r){return !(r.state&(MFS_DISABLED|MFS_GRAYED)) && !(r.type&(MFT_OWNERDRAW|MFT_BITMAP));}
std::wstring Label(const std::wstring& raw){
    std::wstring out;for(size_t i=0;i<raw.size();++i){if(raw[i]==L'&' && i+1<raw.size()){if(raw[i+1]==L'&'){out+=L'&';++i;}continue;}out+=raw[i];}return out;
}
bool Standard(HMENU menu){MENUINFO m{sizeof(m)};m.fMask=MIM_STYLE;return GetMenuInfo(menu,&m) && !(m.dwStyle&MNS_NOTIFYBYPOS);}
std::vector<MenuItem> Walk(HMENU menu,std::vector<UINT> parent,UINT depth,UINT& budget){
    std::vector<MenuItem> result;if(depth>kMaxDepth||!IsMenu(menu))return result;
    const int n=GetMenuItemCount(menu);if(n<0)return result;
    for(int i=0;i<n && budget;++i){--budget;Raw r;if(!Read(menu,static_cast<UINT>(i),r))continue;
        MenuItem item;item.rawText=r.text;item.text=Label(r.text);item.checked=(r.state&MFS_CHECKED)!=0;
        item.path=parent;item.path.push_back(static_cast<UINT>(i));item.commandId=r.id;
        if(r.type&MFT_SEPARATOR){item.kind=ItemKind::Separator;}
        else if(r.sub){item.kind=ItemKind::Submenu;item.enabled=Enabled(r);if(item.enabled)item.children=Walk(r.sub,item.path,depth+1,budget);}
        else{item.kind=ItemKind::Command;item.enabled=Enabled(r)&&Standard(menu)&&r.id>0&&r.id<=0xffff;}
        if(item.text.empty() && item.kind!=ItemKind::Separator){item.text=L"(unsupported item)";item.enabled=false;}
        result.push_back(std::move(item));
    }return result;
}
bool Integrity(DWORD pid,DWORD& il){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;
    HANDLE tok=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&tok)){CloseHandle(process);return false;}CloseHandle(process);
    DWORD size=0;GetTokenInformation(tok,TokenIntegrityLevel,nullptr,0,&size);if(!size||size>65536){CloseHandle(tok);return false;}
    std::vector<BYTE> data(size);bool ok=GetTokenInformation(tok,TokenIntegrityLevel,data.data(),size,&size)!=FALSE;CloseHandle(tok);
    if(!ok)return false;auto* label=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data());
    UCHAR count=*GetSidSubAuthorityCount(label->Label.Sid);if(!count)return false;il=*GetSidSubAuthority(label->Label.Sid,count-1);return true;
}
}
std::vector<Menu> ReadNativeMenus(HWND target){
    std::vector<Menu> out;DWORD pid=0;if(!target||!IsWindow(target)||!GetWindowThreadProcessId(target,&pid)||!pid)return out;
    HMENU root=GetMenu(target);if(!root||!IsMenu(root))return out;int n=GetMenuItemCount(root);if(n<0)return out;
    UINT budget=kMaxItems;
    for(int i=0;i<n && budget;++i){--budget;Raw r;if(!Read(root,static_cast<UINT>(i),r)||!r.sub||!Enabled(r)||r.text.empty())continue;
        Menu m;m.title=Label(r.text);m.source=ProviderKind::Win32Native;m.target=target;m.targetPid=pid;m.root=root;
        m.items=Walk(r.sub,{static_cast<UINT>(i)},1,budget);out.push_back(std::move(m));
    }return out;
}
bool DispatchNativeCommand(const Menu& menu,const MenuItem& item){
    if(menu.source!=ProviderKind::Win32Native||!item.enabled||item.kind!=ItemKind::Command||item.path.size()<2||
       !menu.target||!IsWindow(menu.target))return false;
    DWORD pid=0;GetWindowThreadProcessId(menu.target,&pid);if(!pid||pid!=menu.targetPid)return false;
    DWORD self=0,target=0;if(!Integrity(GetCurrentProcessId(),self)||!Integrity(pid,target)||target>self)return false;
    HMENU current=GetMenu(menu.target);if(!current||current!=menu.root||!IsMenu(current))return false;
    for(size_t depth=0;depth<item.path.size();++depth){
        Raw now;if(!Read(current,item.path[depth],now)||!Enabled(now)||now.type&MFT_SEPARATOR)return false;
        if(depth+1==item.path.size()){
            if(now.sub||now.id!=item.commandId||now.text!=item.rawText||!Standard(current)||now.id==0||now.id>0xffff)return false;
            DWORD recheck=0;GetWindowThreadProcessId(menu.target,&recheck);if(recheck!=pid||GetMenu(menu.target)!=menu.root)return false;
            return PostMessageW(menu.target,WM_COMMAND,MAKEWPARAM(now.id,0),0)!=FALSE;
        }
        if(!now.sub || GetSubMenu(current,item.path[depth])!=now.sub)return false;current=now.sub;
    }return false;
}
}
