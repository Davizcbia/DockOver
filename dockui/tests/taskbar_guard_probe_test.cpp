// Isolated child processes and kernel objects only: no real taskbar, registry or user files.
#include "taskbar_guard_probe.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Process { HANDLE process=nullptr, thread=nullptr; };
std::wstring Self(){wchar_t p[32768]{};DWORD n=GetModuleFileNameW(nullptr,p,32768);return n&&n<32768?std::wstring(p,n):L"";}
void Close(Process& p){if(p.thread)CloseHandle(p.thread);if(p.process)CloseHandle(p.process);p={};}
void StopOwned(Process& p){if(p.process && WaitForSingleObject(p.process,0)==WAIT_TIMEOUT){TerminateProcess(p.process,99);WaitForSingleObject(p.process,3000);}Close(p);}
bool Mock(Process& p){std::wstring cmd=L"\""+Self()+L"\" --mock";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
 if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;p={pi.hProcess,pi.hThread};return true;}
bool Guard(Process& p,HANDLE manager,HANDLE dock,HANDLE request,HANDLE ready,HANDLE visible){
 HANDLE passed[5]{};HANDLE source[]{manager,dock,request,ready,visible};bool ok=true;
 for(int i=0;i<5;++i)if(!DuplicateHandle(GetCurrentProcess(),source[i],GetCurrentProcess(),&passed[i],0,TRUE,DUPLICATE_SAME_ACCESS)){ok=false;break;}
 if(ok){SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
  auto attrs=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if(!InitializeProcThreadAttributeList(attrs,1,0,&bytes))ok=false;
  else {if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,passed,sizeof(passed),nullptr,nullptr))ok=false;
   if(ok){std::wstring cmd=L"\""+Self()+L"\" --guard";
    for(HANDLE h:passed)cmd+=L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(h));
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=attrs;PROCESS_INFORMATION pi{};
    ok=CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=FALSE;
    if(ok)p={pi.hProcess,pi.hThread};}
   DeleteProcThreadAttributeList(attrs);}}
 for(HANDLE h:passed)if(h)CloseHandle(h);return ok;
}
bool Run(const char* label,int trigger,DWORD expected,int premature=0){
 Process manager,dock,guard;HANDLE request=CreateEventW(nullptr,TRUE,FALSE,nullptr),ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),visible=CreateEventW(nullptr,TRUE,TRUE,nullptr);bool ok=request&&ready&&visible;
 if(ok)ok=Mock(manager)&&Mock(dock);
 if(ok&&premature){HANDLE dead=premature==1?manager.process:dock.process;TerminateProcess(dead,42);ok=WaitForSingleObject(dead,3000)==WAIT_OBJECT_0;}
 if(ok)ok=Guard(guard,manager.process,dock.process,request,ready,visible);
 if(ok&&!premature)ok=WaitForSingleObject(ready,3000)==WAIT_OBJECT_0 && ResetEvent(visible); // fake hide, no OS window
 if(ok&&!premature){if(trigger==0)ok=TerminateProcess(manager.process,42)!=FALSE;else if(trigger==1)ok=TerminateProcess(dock.process,43)!=FALSE;else if(trigger==2)ok=SetEvent(request)!=FALSE;else if(trigger==3)ok=TerminateProcess(guard.process,44)!=FALSE;}
 if(ok)ok=WaitForSingleObject(guard.process,5000)==WAIT_OBJECT_0;
 DWORD code=0;if(ok)ok=GetExitCodeProcess(guard.process,&code)&&code==expected;
 if(ok&&premature)ok=WaitForSingleObject(ready,0)==WAIT_TIMEOUT;
 if(ok&&trigger==3&&!premature){ // owner observes dead guard and restores simulated visibility
  ok=WaitForSingleObject(visible,0)==WAIT_TIMEOUT && SetEvent(visible);
 }
 if(ok)ok=WaitForSingleObject(visible,0)==WAIT_OBJECT_0;
 std::printf("%s %s (exit=%lu)\n",ok?"PASS":"FAIL",label,code);
 StopOwned(guard);StopOwned(manager);StopOwned(dock);if(request)CloseHandle(request);if(ready)CloseHandle(ready);if(visible)CloseHandle(visible);return ok;
}
}
int wmain(int argc,wchar_t** argv){
 if(argc==2 && std::wstring(argv[1])==L"--mock"){Sleep(INFINITE);return 0;}
 if(argc==7 && std::wstring(argv[1])==L"--guard"){
  auto parse=[](const wchar_t* s){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(s,nullptr,10)));};
  HANDLE ready=parse(argv[5]),visible=parse(argv[6]);
  auto result=docky::guard_probe::Wait(parse(argv[2]),parse(argv[3]),parse(argv[4]),ready);
  // Test-only fake visibility: guard restores it if it had acknowledged readiness.
  if(WaitForSingleObject(ready,0)==WAIT_OBJECT_0 && !SetEvent(visible))return 14;
  return static_cast<int>(result);
 }
 bool ok=true;
 ok=Run("manager process crash detected",0,10)&&ok;
 ok=Run("Dock process crash detected",1,11)&&ok;
 ok=Run("normal restore request detected",2,12)&&ok;
 ok=Run("guard failure detected by owner",3,44)&&ok;
 ok=Run("dead manager never reported ready",0,10,1)&&ok;
 ok=Run("dead Dock never reported ready",0,11,2)&&ok;
 ok=(docky::guard_probe::Wait(INVALID_HANDLE_VALUE,nullptr,nullptr,nullptr)==docky::guard_probe::Result::Invalid)&&ok;
 std::printf("%s invalid handles rejected\n",ok?"PASS":"FAIL");
 return ok?0:1;
}
