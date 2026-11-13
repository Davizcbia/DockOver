#include "frame_ready.h"
#include <dxgi.h>
#include <cstdio>
#include <string>
#include <vector>
int wmain(int argc,wchar_t** argv){
 if(argc==3 && std::wstring(argv[1])==L"--child"){
  auto event=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(_wcstoui64(argv[2],nullptr,10)));
  return docky::SignalFirstPresent(S_OK,event)?0:9;
 }
 HANDLE actual=CreateEventW(nullptr,TRUE,FALSE,nullptr),copy=nullptr;if(!actual)return 1;
 if(!DuplicateHandle(GetCurrentProcess(),actual,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS))return 2;
 if(docky::SignalFirstPresent(E_FAIL,copy)||WaitForSingleObject(actual,0)!=WAIT_TIMEOUT||!copy)return 3;
 if(docky::SignalFirstPresent(DXGI_STATUS_OCCLUDED,copy)||WaitForSingleObject(actual,0)!=WAIT_TIMEOUT||!copy)return 4;
 if(!docky::SignalFirstPresent(S_OK,copy)||copy||WaitForSingleObject(actual,0)!=WAIT_OBJECT_0)return 5;
 if(docky::SignalFirstPresent(S_OK,copy))return 6;
 CloseHandle(actual);
 // A separate mock Dock receives only the event through a restricted inherited handle list.
 SECURITY_ATTRIBUTES sa{};sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
 HANDLE event=CreateEventW(&sa,TRUE,FALSE,nullptr);if(!event)return 7;
 SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
 auto attrs=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());if(!InitializeProcThreadAttributeList(attrs,1,0,&bytes))return 8;
 if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&event,sizeof(event),nullptr,nullptr))return 9;
 wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return 10;
 std::wstring cmd=L"\""+std::wstring(path)+L"\" --child "+std::to_wstring(reinterpret_cast<ULONG_PTR>(event));
 STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=attrs;PROCESS_INFORMATION pi{};
 BOOL ok=CreateProcessW(path,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi);
 DeleteProcThreadAttributeList(attrs);if(!ok){CloseHandle(event);return 11;}
 const bool signaled=WaitForSingleObject(event,5000)==WAIT_OBJECT_0;
 const bool ended=WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0;DWORD code=99;if(ended)GetExitCodeProcess(pi.hProcess,&code);
 CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(event);if(!signaled||!ended||code!=0)return 12;
 std::puts("PASS only S_OK signals first Present; restricted inherited event works across processes");return 0;
}
