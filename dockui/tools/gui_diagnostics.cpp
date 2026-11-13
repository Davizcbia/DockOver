#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <string>
#include <vector>

// Development-only local observation. No messages, hooks, injection or remote endpoint.
namespace {
struct Context { DWORD pid; std::vector<std::wstring> found; HWND input=nullptr,bar=nullptr; };
struct Known { const wchar_t* cls; const wchar_t* kind; };
constexpr Known known[] = {
 {L"LiquidDockV01",L"Dock"}, {L"LiquidDesktopMenuBarV01",L"MenuBar"}, {L"dockyMenuBarInputV13",L"MenuInput"},
 {L"LiquidControlCenter",L"ControlCenter"}, {L"LiquidLaunchpad",L"Launchpad"},
 {L"LiquidDockIconEditor",L"IconEditor"}, {L"LiquidUnifiedPopup98d2",L"Popup"}
};
BOOL CALLBACK Report(HWND hwnd, LPARAM param) {
 auto& c=*reinterpret_cast<Context*>(param); DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid); if(pid!=c.pid)return TRUE;
 wchar_t cls[256]{}; if(!GetClassNameW(hwnd,cls,256))return TRUE;
 const Known* item=nullptr;for(const auto& k:known)if(wcscmp(k.cls,cls)==0){item=&k;break;}if(!item)return TRUE;
 RECT r{}; if(!GetWindowRect(hwnd,&r))return TRUE; HMONITOR mon=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);MONITORINFOEXW mi{};mi.cbSize=sizeof(mi);GetMonitorInfoW(mon,&mi);
 std::wprintf(L"window=%ls hwnd=0x%llx class=%ls pid=%lu visible=%d dpi=%u bounds=%ld,%ld,%ld,%ld monitor=%ls monitor_bounds=%ld,%ld,%ld,%ld\n",
   item->kind,static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd)),cls,pid,IsWindowVisible(hwnd)?1:0,GetDpiForWindow(hwnd),r.left,r.top,r.right,r.bottom,
   mi.szDevice,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right,mi.rcMonitor.bottom);
 if(IsWindowVisible(hwnd))c.found.emplace_back(item->kind);if(wcscmp(item->kind,L"MenuInput")==0)c.input=hwnd;if(wcscmp(item->kind,L"MenuBar")==0)c.bar=hwnd;return TRUE;
}
bool IsAdjacentComponent(DWORD pid){
 HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return false;
 wchar_t actual[32768]{};DWORD count=32768;BOOL ok=QueryFullProcessImageNameW(p,0,actual,&count);CloseHandle(p);if(!ok)return false;
 wchar_t self[32768]{};DWORD n=GetModuleFileNameW(nullptr,self,32768);if(!n||n>=32768)return false;
 std::wstring dir(self,n);auto pos=dir.find_last_of(L"\\/");if(pos==std::wstring::npos)return false;dir.resize(pos+1);
 for(const wchar_t* name:{L"LiquidDock.exe",L"LiquidMenuBar.exe"})if(_wcsicmp(actual,(dir+name).c_str())==0)return true;
 return false;
}
}
int wmain(int argc,wchar_t** argv){
 if((argc!=4&&argc!=5&&argc!=7)||wcscmp(argv[1],L"--dev")||wcscmp(argv[2],L"--pid")||(argc==5&&wcscmp(argv[4],L"--audit"))||(argc==7&&wcscmp(argv[4],L"--probe"))){std::fputs("usage: docky-gui-diagnostics --dev --pid <dock-or-menu-pid> [--audit | --probe screen-x screen-y]\n",stderr);return 2;}
 wchar_t* end=nullptr;unsigned long pid=wcstoul(argv[3],&end,10);if(!pid||!end||*end||!IsAdjacentComponent(pid)){std::fputs("refused: pid must be a running Dock or Menu Bar from this build directory\n",stderr);return 3;}
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 if(argc==7){wchar_t* xe=nullptr;wchar_t* ye=nullptr;long x=wcstol(argv[5],&xe,10),y=wcstol(argv[6],&ye,10);if(!xe||*xe||!ye||*ye)return 2;
  POINT pt{x,y};HWND target=WindowFromPoint(pt);DWORD targetPid=0;if(target)GetWindowThreadProcessId(target,&targetPid);wchar_t cls[256]{};
  if(targetPid==pid)GetClassNameW(target,cls,256);std::wprintf(L"probe=%ld,%ld owned=%d class=%ls\n",x,y,targetPid==pid?1:0,targetPid==pid?cls:L"external-or-none");return 0;}
 Context c{pid,{}};EnumWindows(Report,reinterpret_cast<LPARAM>(&c));
 if(argc==5){
  if(!c.input||!c.bar||!IsWindowVisible(c.input))return 4;RECT r{};if(!GetWindowRect(c.input,&r))return 4;
  HRGN region=CreateRectRgn(0,0,0,0);if(!region||GetWindowRgn(c.input,region)==ERROR){if(region)DeleteObject(region);return 4;}
  unsigned long inside=0,insideMiss=0,outside=0,outsideIntercept=0;
  for(int y=0;y<r.bottom-r.top;++y)for(int x=0;x<r.right-r.left;++x){const bool expected=PtInRegion(region,x,y)!=FALSE;POINT pt{r.left+x,r.top+y};HWND hit=WindowFromPoint(pt);
   if(expected){++inside;if(hit!=c.input)++insideMiss;}else{++outside;if(hit==c.input||hit==c.bar)++outsideIntercept;}}
  DeleteObject(region);std::wprintf(L"audit inside=%lu inside_miss=%lu outside=%lu outside_intercept=%lu\n",inside,insideMiss,outside,outsideIntercept);
  return insideMiss||outsideIntercept?5:0;
 }
 for(const auto& k:known)if(std::find(c.found.begin(),c.found.end(),k.kind)==c.found.end())std::wprintf(L"state=%ls visible=0-or-not-in-this-process\n",k.kind);
 return 0;
}
