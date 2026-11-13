#include "taskbar_guard_sim.h"
#include "taskbar_guard_probe.h"
#include "taskbar_window.h"
#include <cstdio>
#include <vector>
namespace docky::guard_sim {
namespace {
using namespace liquid::desktop;
std::wstring GuardPath(){wchar_t p[32768]{};DWORD n=GetModuleFileNameW(nullptr,p,32768);if(!n||n>=32768)return {};std::wstring s(p,n);auto i=s.find_last_of(L"\\/");return i==std::wstring::npos?L"":s.substr(0,i+1)+L"docky-taskbar-guard-sim.exe";}
std::wstring Quote(const std::wstring& s){return L"\""+s+L"\"";} // test record path is generated in an existing directory
WindowOps FakeOps(HANDLE hidden){WindowOps o;
 o.observe=[hidden](const TaskbarWindowRecord& rec,TaskbarWindowRecord& now){now=rec;now.visible=WaitForSingleObject(hidden,0)!=WAIT_OBJECT_0;return true;};
 o.setVisible=[hidden](ULONG_PTR,bool show){return (show?ResetEvent(hidden):SetEvent(hidden))!=FALSE;};return o;}
bool Recover(HANDLE lock,HANDLE hidden,const std::wstring& record){
 const DWORD w=WaitForSingleObject(lock,5000);if(w!=WAIT_OBJECT_0&&w!=WAIT_ABANDONED)return false;
 // The fake event is an authoritative simulation state, unlike a real Pending journal.
 TaskbarWindowOperation op;std::wstring error;bool prepared=LoadWindowOperation(record,op,error);
 if(prepared){for(auto& entry:op.windows)if(entry.state==WindowState::Pending){entry.state=WindowState::Hidden;entry.hiddenByUs=true;}prepared=SaveWindowOperation(record,op,error);}
 std::wstring log;const bool ok=prepared&&RestoreTaskbarWindows(record,FakeOps(hidden),log);
 const bool visible=WaitForSingleObject(hidden,0)==WAIT_TIMEOUT;ReleaseMutex(lock);
 std::printf("guard-sim: recovery ok=%d visible=%d\n",ok,visible);std::fflush(stdout);return ok&&visible;
}
}
Session::~Session(){Stop();}
void Session::Close(){for(HANDLE* h:{&process_,&stop_,&ready_,&hidden_,&lock_}){if(*h)CloseHandle(*h);*h=nullptr;}}
bool Session::Start(HANDLE dock,HANDLE managerGate,const std::wstring& directory,bool crashAfterHide){
 if(process_||!dock||!managerGate||directory.empty())return false;
 DWORD attr=GetFileAttributesW(directory.c_str());if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY))return false;
 record_=directory+L"\\docky-guard-sim-record.txt";
 HANDLE file=CreateFileW(record_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 const char closed[]="liquid-taskbar-windows 1\nphase closed\n";DWORD written=0;
 bool ok=WriteFile(file,closed,sizeof(closed)-1,&written,nullptr)&&written==sizeof(closed)-1&&FlushFileBuffers(file);CloseHandle(file);
 if(!ok||GetFileAttributesW((record_+L".tmp").c_str())!=INVALID_FILE_ATTRIBUTES)return false;
 stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);hidden_=CreateEventW(nullptr,TRUE,FALSE,nullptr);lock_=CreateMutexW(nullptr,FALSE,nullptr);
 if(!stop_||!ready_||!hidden_||!lock_){Close();return false;}
 HANDLE source[]{GetCurrentProcess(),dock,stop_,ready_,hidden_,lock_,managerGate};HANDLE inherited[7]{};
 for(int i=0;i<7;++i)if(!DuplicateHandle(GetCurrentProcess(),source[i],GetCurrentProcess(),&inherited[i],0,TRUE,DUPLICATE_SAME_ACCESS)){ok=false;break;}
 if(ok){SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);auto list=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if(!bytes||!InitializeProcThreadAttributeList(list,1,0,&bytes))ok=false;
  else {if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))ok=false;
   if(ok){const std::wstring exe=GuardPath();std::wstring cmd=Quote(exe)+L" --simulate";for(auto h:inherited)cmd+=L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(h));cmd+=L" "+Quote(record_);
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=list;PROCESS_INFORMATION pi{};
    ok=!exe.empty()&&CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi);
    if(ok){process_=pi.hProcess;CloseHandle(pi.hThread);}}
   DeleteProcThreadAttributeList(list);}}
 for(auto h:inherited)if(h)CloseHandle(h);
 if(!ok){Close();return false;}
 HANDLE wait[]{ready_,process_,dock};if(WaitForMultipleObjects(3,wait,FALSE,5000)!=WAIT_OBJECT_0){Stop();return false;}
 const DWORD got=WaitForSingleObject(lock_,5000);if(got!=WAIT_OBJECT_0&&got!=WAIT_ABANDONED){Stop();return false;}
 if(WaitForSingleObject(process_,0)!=WAIT_TIMEOUT||WaitForSingleObject(dock,0)!=WAIT_TIMEOUT){ReleaseMutex(lock_);Stop();return false;}
 TaskbarWindowOperation pending;pending.phase=RecordPhase::Active;pending.windows.push_back({{L"Shell_TrayWnd",101,1,1,true},false,WindowState::Pending});std::wstring error;
 ok=SaveWindowOperation(record_,pending,error);
 if(ok)ok=SetEvent(hidden_)!=FALSE;
 if(ok){std::puts("guard-sim: simulated hidden after durable Pending");std::fflush(stdout);}
 if(ok&&crashAfterHide){TerminateProcess(GetCurrentProcess(),83);return false;} // explicit test-only crash, own Manager process
 if(ok){pending.windows[0].hiddenByUs=true;pending.windows[0].state=WindowState::Hidden;ok=SaveWindowOperation(record_,pending,error);}
 ReleaseMutex(lock_);
 if(!ok){Recover();Stop();return false;}return true;
}
bool Session::Recover(){return lock_&&hidden_&&!record_.empty()&&guard_sim::Recover(lock_,hidden_,record_);}
bool Session::RecoverAfterGuardExit(){const bool ok=Recover();Close();return ok;}
void Session::Stop(){if(process_){SetEvent(stop_);if(WaitForSingleObject(process_,5000)!=WAIT_OBJECT_0){std::puts("guard-sim: guard did not exit; simulated recovery attempted");Recover();}else{DWORD code=99;GetExitCodeProcess(process_,&code);if(code!=0)Recover();}}Close();}
int RunGuard(HANDLE manager,HANDLE dock,HANDLE stop,HANDLE ready,HANDLE hidden,HANDLE lock,HANDLE gate,const std::wstring& record){
 if(!gate||record.empty())return 2;
 auto reason=guard_probe::Wait(manager,dock,stop,ready);
 if(reason==guard_probe::Result::Invalid)return 3;
 return Recover(lock,hidden,record)?0:4;
}
}
