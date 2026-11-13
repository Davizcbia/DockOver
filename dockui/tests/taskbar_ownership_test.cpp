#include "taskbar_ownership.h"
#include <cstdio>
#include <string>
using namespace liquid::desktop;
#define CHECK(x) do { if(!(x)){std::fwprintf(stderr,L"FAIL line %d: %ls\n",__LINE__,error.c_str());return 1;} } while(0)
TaskbarStateSnapshot Mock(bool value) { TaskbarStateSnapshot s; s.valid=true; s.primaryWindowFound=true; s.autoHide=value; s.rawFlags=value?1:0; s.capturedFileTimeUtc=133000000000000000ull; return s; }
std::string ReadBytes(const std::wstring& p) {
 HANDLE f=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return {};
 LARGE_INTEGER size{};if(!GetFileSizeEx(f,&size)||size.QuadPart<0||size.QuadPart>4096){CloseHandle(f);return {};}
 std::string data(static_cast<size_t>(size.QuadPart),'\0');DWORD n=0;BOOL ok=ReadFile(f,data.data(),static_cast<DWORD>(data.size()),&n,nullptr);CloseHandle(f);return ok&&n==data.size()?data:std::string{};
}
int wmain(int argc,wchar_t** argv){
 std::wstring error;
 if(argc==3 && std::wstring(argv[1])==L"--try-lock") { TaskbarOperationStore child; return child.Open(argv[2],error)?4:0; }
 if(argc==3 && std::wstring(argv[1])==L"--crash-prepared") { TaskbarOperationStore child; if(!child.Open(argv[2],error)||child.Begin(Mock(false),true,error)!=BeginResult::Prepared) return 9; ExitProcess(73); }

 wchar_t tmp[MAX_PATH]{}; if(!GetTempPathW(MAX_PATH,tmp))return 20;
 std::wstring dir=std::wstring(tmp)+L"liquid-taskbar-ownership-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
 CHECK(CreateDirectoryW(dir.c_str(),nullptr));
 auto path=[&](int n){return dir+L"\\case"+std::to_wstring(n)+L".txt";};
 // Original disabled, target enabled, confirmed manager change and simulated clean restoration.
 std::string id; { TaskbarOperationStore s; CHECK(s.Open(path(1),error)); CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared); id=s.Current().operationId; CHECK(!s.Current().managerChangeConfirmed);
   TaskbarOperationStore blocked; CHECK(!blocked.Open(path(1),error)); error.clear();
   wchar_t self[MAX_PATH]{}; CHECK(GetModuleFileNameW(nullptr,self,MAX_PATH)); std::wstring cmd=L"\""+std::wstring(self)+L"\" --try-lock \""+path(1)+L"\"";
   STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{}; CHECK(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)); CHECK(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);DWORD exit=99;CHECK(GetExitCodeProcess(pi.hProcess,&exit)&&exit==0);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
   CHECK(s.ConfirmApplied(id,true,true,error)); CHECK(s.Current().phase==OperationPhase::Applied && s.Current().managerChangeConfirmed);
   CHECK(s.RequestRestore(id,true,ChangeEvidence::SimulatedNoExternalChange,error)); CHECK(s.Current().phase==OperationPhase::RestorePending);
   CHECK(s.ConfirmRestored(id,true,false,error)); CHECK(s.Current().phase==OperationPhase::Restored);CHECK(s.ConfirmRestored(id,true,false,error)); }
 { TaskbarOperationStore s;CHECK(s.Open(path(1),error));CHECK(s.Current().phase==OperationPhase::Restored && s.Current().operationId==id && !s.Current().lastObserved); }
 // No-op must not create an operation or recovery ownership.
 {TaskbarOperationStore s;CHECK(s.Open(path(2),error));CHECK(s.Begin(Mock(true),true,error)==BeginResult::NoChange);CHECK(s.Current().phase==OperationPhase::Idle);}
 // User/external changes or unverifiable attribution prohibit recovery.
 {TaskbarOperationStore s;CHECK(s.Open(path(3),error));CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);auto k=s.Current().operationId;CHECK(s.ConfirmApplied(k,true,true,error));CHECK(s.RequestRestore(k,false,ChangeEvidence::ExternalChange,error));CHECK(s.Current().phase==OperationPhase::Conflict);CHECK(!s.ConfirmRestored(k,true,false,error));}
 {TaskbarOperationStore s;CHECK(s.Open(path(4),error));CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);auto k=s.Current().operationId;CHECK(s.ConfirmApplied(k,true,true,error));CHECK(s.RequestRestore(k,true,ChangeEvidence::Unverifiable,error));CHECK(s.Current().phase==OperationPhase::Conflict);}
 // Simulated crash in each pending phase is persisted and restart becomes Conflict.
 for(int stage=0;stage<3;stage++){std::string k;{TaskbarOperationStore s;CHECK(s.Open(path(5+stage),error));CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);k=s.Current().operationId;if(stage>=1)CHECK(s.ConfirmApplied(k,true,true,error));if(stage>=2)CHECK(s.RequestRestore(k,true,ChangeEvidence::SimulatedNoExternalChange,error));}
   {TaskbarOperationStore s;CHECK(s.Open(path(5+stage),error));CHECK(s.Current().phase==OperationPhase::Conflict);CHECK(s.Begin(Mock(false),true,error)==BeginResult::Error);CHECK(s.ResolveAfterRestart(error));CHECK(s.Current().phase==OperationPhase::Conflict);CHECK(!s.ConfirmRestored(k,true,false,error));}}
 // Process exits without stack unwinding while holding an unfinished record and lock.
 { wchar_t self[MAX_PATH]{};CHECK(GetModuleFileNameW(nullptr,self,MAX_PATH));std::wstring cmd=L"\""+std::wstring(self)+L"\" --crash-prepared \""+path(12)+L"\"";
   STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};CHECK(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));CHECK(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);DWORD exit=0;CHECK(GetExitCodeProcess(pi.hProcess,&exit)&&exit==73);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
   TaskbarOperationStore reopened;CHECK(reopened.Open(path(12),error));CHECK(reopened.Current().phase==OperationPhase::Conflict);}
 // Invalid or future-format record must not be overwritten.
 for(int stage=0;stage<2;stage++){const auto p=path(8+stage);HANDLE f=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);const char* text=stage?"LIQUID_TASKBAR_OPERATION 99\n":"broken";DWORD n=0;CHECK(WriteFile(f,text,static_cast<DWORD>(strlen(text)),&n,nullptr));CloseHandle(f);TaskbarOperationStore s;CHECK(!s.Open(p,error));CHECK(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES);}
 // A snapshot alone or failed simulated execution never establishes ownership.
 {TaskbarOperationStore s;CHECK(s.Open(path(10),error));auto invalid=Mock(false);invalid.valid=false;CHECK(s.Begin(invalid,true,error)==BeginResult::Error);CHECK(s.Current().phase==OperationPhase::Idle);
  CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);auto k=s.Current().operationId;CHECK(!s.ConfirmApplied("wrong",true,true,error));CHECK(s.Current().phase==OperationPhase::Prepared);CHECK(s.ConfirmApplied(k,false,false,error));CHECK(s.Current().phase==OperationPhase::Conflict&&!s.Current().managerChangeConfirmed);}
 // A changed target after simulated apply is an external conflict even without a notification.
 {TaskbarOperationStore s;CHECK(s.Open(path(11),error));CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);auto k=s.Current().operationId;CHECK(s.ConfirmApplied(k,true,true,error));CHECK(s.RequestRestore(k,false,ChangeEvidence::SimulatedNoExternalChange,error));CHECK(s.Current().phase==OperationPhase::Conflict);}
 // Conflict details, cancellation, explicit confirmation, idempotence and restart.
 std::string conflictId;
 {TaskbarOperationStore s;CHECK(s.Open(path(13),error));CHECK(s.Begin(Mock(false),true,error)==BeginResult::Prepared);conflictId=s.Current().operationId;
  CHECK(s.ConfirmApplied(conflictId,true,true,error));CHECK(s.RequestRestore(conflictId,false,ChangeEvidence::ExternalChange,error));
  ConflictDetails d;CHECK(s.DescribeConflict(conflictId,false,d,error));CHECK(!d.original && d.expected && !d.current && d.operationId==conflictId);
  const auto before=ReadBytes(path(13));CHECK(!before.empty());CHECK(!s.AbandonConflict(conflictId,false,false,error));CHECK(ReadBytes(path(13))==before);
  CHECK(s.CancelConflict(conflictId,error));CHECK(ReadBytes(path(13))==before);
  wchar_t self[MAX_PATH]{};CHECK(GetModuleFileNameW(nullptr,self,MAX_PATH));std::wstring cmd=L"\""+std::wstring(self)+L"\" --try-lock \""+path(13)+L"\"";
  STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};CHECK(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));CHECK(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);DWORD exit=9;CHECK(GetExitCodeProcess(pi.hProcess,&exit)&&exit==0);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
 {TaskbarOperationStore s;CHECK(s.Open(path(13),error));CHECK(s.Current().phase==OperationPhase::Conflict);CHECK(!s.AbandonConflict("wrong",false,true,error));
  CHECK(s.AbandonConflict(conflictId,false,true,error));CHECK(s.Current().phase==OperationPhase::Abandoned && !s.Current().managerChangeConfirmed);
  const auto saved=ReadBytes(path(13));CHECK(!saved.empty());CHECK(s.AbandonConflict(conflictId,true,true,error));CHECK(ReadBytes(path(13))==saved);CHECK(!s.ConfirmRestored(conflictId,true,false,error));}
 {TaskbarOperationStore s;CHECK(s.Open(path(13),error));CHECK(s.Current().phase==OperationPhase::Abandoned);const auto saved=ReadBytes(path(13));CHECK(s.AbandonConflict(conflictId,false,true,error));CHECK(ReadBytes(path(13))==saved);}
 std::printf("PASS: no-op, ownership, atomic reload, repeated restore, external conflict, uncertain evidence, crash phases, corrupt/version, cross-process exclusion, conflict cancel/abandon/restart\n");
 return 0;
}
