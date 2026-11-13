// Conservative user-invoked recovery of docky's legacy taskbar window record.
// No fallback blindly reveals arbitrary windows; an ambiguous record is left unchanged.
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <map>
#include <string>
#include "taskbar_window.h"
#include "taskbar_guardian.h"
using namespace liquid::desktop;
namespace {
constexpr wchar_t kManagerGate[]=L"Local\\LiquidDesktopManagerV05SingleInstance";
std::wstring DefaultRecord(){PWSTR path=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path)))return {};
 std::wstring result(path);CoTaskMemFree(path);return result+L"\\LiquidDesktop\\taskbar-windows-v1.txt";}
}
int wmain(int argc,wchar_t** argv){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // physical taskbar and work-area rects
 std::wstring path;bool simulate=false,provided=false;
 for(int i=1;i<argc;++i){std::wstring a=argv[i];
  if(a==L"--record" && i+1<argc){path=argv[++i];provided=true;}
  else if(a==L"--simulate")simulate=true;
  else {std::fputs("Usage: docky-taskbar-restore [--record PATH] [--simulate --record TEST_PATH]\n",stderr);return 2;}}
 if(simulate && !provided){std::fputs("Simulation requires an explicit isolated test record.\n",stderr);return 2;}
 if(path.empty())path=DefaultRecord();
 if(path.empty()){std::fputs("Recovery record path unavailable; no window action.\n",stderr);return 2;}
 // Create the same Manager gate: keeping this handle open also prevents a new Manager from starting.
 HANDLE gate=CreateMutexW(nullptr,FALSE,kManagerGate);if(!gate){std::fputs("Cannot secure Manager gate; no window action.\n",stderr);return 3;}
 if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(gate);
  if(simulate){std::fputs("docky Manager or another recovery is active; no window action.\n",stderr);return 3;}
  // A Manager is running: ask its independent recovery guardian (works even if the Manager is unresponsive).
  HANDLE now=OpenEventW(EVENT_MODIFY_STATE,FALSE,docky::taskbar::kRestoreNowEvent);
  if(!now){std::fputs("docky is running without a recovery guardian (taskbar not hidden by docky); no window action.\n",stderr);return 3;}
  SetEvent(now);CloseHandle(now);
  for(int i=0;i<50;++i){TaskbarWindowOperation op;std::wstring error;
   if(LoadWindowOperation(path,op,error)&&op.phase==RecordPhase::Closed){std::puts("Recovery guardian restored the taskbar (record closed).");return 0;}Sleep(100);}
  std::fputs("Recovery guardian did not close the record within 5 s; it is preserved for review.\n",stderr);return 4;}
 DWORD attr=GetFileAttributesW(path.c_str());
 if(attr==INVALID_FILE_ATTRIBUTES){DWORD e=GetLastError();CloseHandle(gate);
  if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND){std::puts("No recovery record; no window action.");return 0;}
  std::printf("Cannot inspect record (%lu); no window action.\n",e);return 3;}
 DWORD tmpAttr=GetFileAttributesW((path+L".tmp").c_str());
 if(tmpAttr!=INVALID_FILE_ATTRIBUTES){std::fputs("Existing temporary record preserved; no window action.\n",stderr);CloseHandle(gate);return 3;}
 DWORD tmpError=GetLastError();if(tmpError!=ERROR_FILE_NOT_FOUND && tmpError!=ERROR_PATH_NOT_FOUND){std::fputs("Cannot inspect temporary record; no window action.\n",stderr);CloseHandle(gate);return 3;}
 TaskbarWindowOperation op;std::wstring error;
 if(!LoadWindowOperation(path,op,error)){std::fwprintf(stderr,L"Cannot load record: %ls; no window action.\n",error.c_str());CloseHandle(gate);return 3;}
 if(op.phase==RecordPhase::Closed){std::puts("Record already closed; no window action.");CloseHandle(gate);return 0;}
 if(!IsConservativeRecoveryRecord(op)){std::fputs("Conflict or ambiguous record preserved; no window action.\n",stderr);CloseHandle(gate);return 3;}
 WindowOps ops;
 std::map<ULONG_PTR,TaskbarWindowRecord> simulated;
 if(simulate){for(const auto& w:op.windows){auto r=w.original;r.visible=false;simulated[r.hwnd]=r;}
  ops.observe=[&](const TaskbarWindowRecord& wanted,TaskbarWindowRecord& out){auto i=simulated.find(wanted.hwnd);if(i==simulated.end())return false;out=i->second;return true;};
  ops.setVisible=[&](ULONG_PTR id,bool show){auto i=simulated.find(id);if(i==simulated.end())return false;i->second.visible=show;return true;};
 }else{
  DWORD shellPid=0;HWND shell=GetShellWindow();if(shell)GetWindowThreadProcessId(shell,&shellPid);
  if(!shellPid){std::fputs("Explorer identity unavailable; no window action.\n",stderr);CloseHandle(gate);return 3;}
  auto real=WindowOps::Real();ops=real;
  ops.observe=[real,shellPid](const TaskbarWindowRecord& wanted,TaskbarWindowRecord& out){
   if(wanted.pid!=shellPid)return false;return real.observe(wanted,out);};
 }
 std::wstring log;bool ok=false;
 if(simulate)ok=RestoreTaskbarWindows(path,ops,log);
 else ok=docky::taskbar::RecoverTaskbarRecord(path,log)!=docky::taskbar::RecoverResult::Refused;  // same lock + checks as Manager/guardian
 if(!log.empty())std::fwprintf(stdout,L"%ls",log.c_str());
 std::puts(ok?(simulate?"Simulated recovery complete.":"Recovery completed or owned windows no longer exist."):"Recovery incomplete; record preserved.");
 CloseHandle(gate);return ok?0:4;
}
