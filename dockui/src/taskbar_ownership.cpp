#include "taskbar_ownership.h"
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace liquid::desktop {
namespace {
uint64_t Hash(const std::string& s) { uint64_t v=14695981039346656037ull; for(unsigned char c:s){v^=c;v*=1099511628211ull;} return v; }
std::string Encode(const TaskbarOperation& s) {
    std::ostringstream o; o << "LIQUID_TASKBAR_OPERATION 1\nid=" << s.operationId << "\ntime=" << s.createdFileTimeUtc
      << "\nphase=" << static_cast<int>(s.phase) << "\noriginal=" << int(s.original) << "\ntarget=" << int(s.target)
      << "\nlast=" << int(s.lastObserved) << "\nmanager_confirmed=" << int(s.managerChangeConfirmed) << "\n";
    const auto body=o.str(); o << "checksum=" << std::hex << std::setw(16) << std::setfill('0') << Hash(body) << "\n"; return o.str();
}
bool WellFormed(const TaskbarOperation& s) {
    if(s.version!=1 || s.phase==OperationPhase::Idle || s.operationId.size()!=32 || !s.createdFileTimeUtc || s.original==s.target) return false;
    for(char c:s.operationId) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    if(s.phase<OperationPhase::Prepared || s.phase>OperationPhase::Abandoned) return false;
    if(s.phase==OperationPhase::Prepared && (s.managerChangeConfirmed || s.lastObserved!=s.original)) return false;
    if((s.phase==OperationPhase::Applied || s.phase==OperationPhase::RestorePending) && (!s.managerChangeConfirmed || s.lastObserved!=s.target)) return false;
    if(s.phase==OperationPhase::Restored && (!s.managerChangeConfirmed || s.lastObserved!=s.original)) return false;
    if(s.phase==OperationPhase::Abandoned && s.managerChangeConfirmed) return false;
    return true;
}
std::wstring Err(const wchar_t* what,DWORD code){return std::wstring(what)+L" (Windows error "+std::to_wstring(code)+L")";}
std::string NewId(){ GUID g{}; if(FAILED(CoCreateGuid(&g))) return {}; char id[33]{}; std::snprintf(id,sizeof(id),"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",g.Data1,g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]); return id; }
}
TaskbarOperationStore::~TaskbarOperationStore(){if(lock_!=INVALID_HANDLE_VALUE) CloseHandle(lock_);}
bool TaskbarOperationStore::Open(const std::wstring& path,std::wstring& error){
    if(lock_!=INVALID_HANDLE_VALUE || path.empty()){error=L"Store already open or path empty";return false;}
    path_=path; const auto lockPath=path+L".lock";
    lock_=CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock_==INVALID_HANDLE_VALUE){error=Err(L"Operation lock unavailable",GetLastError());return false;}
    if(!Load(error) || !ResolveAfterRestart(error)){CloseHandle(lock_);lock_=INVALID_HANDLE_VALUE;return false;} return true;
}
bool TaskbarOperationStore::OpenForInspection(const std::wstring& path,std::wstring& error){
    if(lock_!=INVALID_HANDLE_VALUE || path.empty()){error=L"Store already open or path empty";return false;}
    path_=path;const auto lockPath=path+L".lock";
    lock_=CreateFileW(lockPath.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock_==INVALID_HANDLE_VALUE){error=Err(L"Existing operation lock unavailable",GetLastError());return false;}
    inspectionOnly_=true;
    if(!Load(error)){CloseHandle(lock_);lock_=INVALID_HANDLE_VALUE;inspectionOnly_=false;return false;}return true;
}
bool TaskbarOperationStore::Load(std::wstring& error){
    HANDLE f=CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND){state_={};return true;} error=Err(L"Open record failed",GetLastError());return false;}
    LARGE_INTEGER size{}; if(!GetFileSizeEx(f,&size)||size.QuadPart<=0||size.QuadPart>1024){CloseHandle(f);error=L"Invalid record size";return false;}
    std::string text(static_cast<size_t>(size.QuadPart),'\0');DWORD n=0; BOOL ok=ReadFile(f,text.data(),static_cast<DWORD>(text.size()),&n,nullptr);CloseHandle(f);
    if(!ok||n!=text.size()){error=L"Incomplete record read";return false;}
    char id[33]{}; unsigned long long time=0; int phase=0,original=0,target=0,last=0,owned=0,consumed=0;
    if(std::sscanf(text.c_str(),"LIQUID_TASKBAR_OPERATION 1\nid=%32[0-9a-f]\ntime=%llu\nphase=%d\noriginal=%d\ntarget=%d\nlast=%d\nmanager_confirmed=%d\n%n",id,&time,&phase,&original,&target,&last,&owned,&consumed)!=7 || consumed<=0 || phase<1 || phase>6 || (original!=0&&original!=1)||(target!=0&&target!=1)||(last!=0&&last!=1)||(owned!=0&&owned!=1)){error=L"Corrupt or incompatible operation record";return false;}
    TaskbarOperation s; s.operationId=id;s.createdFileTimeUtc=time;s.phase=static_cast<OperationPhase>(phase);s.original=original!=0;s.target=target!=0;s.lastObserved=last!=0;s.managerChangeConfirmed=owned!=0;
    if(!WellFormed(s)||Encode(s)!=text){error=L"Corrupt or incompatible operation record";return false;}state_=s;return true;
}
bool TaskbarOperationStore::Commit(const TaskbarOperation& next,std::wstring& error){
    if(lock_==INVALID_HANDLE_VALUE || inspectionOnly_ || !WellFormed(next)){error=L"Store unlocked or transition invalid";return false;}
    const auto text=Encode(next);std::wstring tmp;HANDLE f=INVALID_HANDLE_VALUE;
    for(unsigned i=0;i<20 && f==INVALID_HANDLE_VALUE;++i){tmp=path_+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(i);f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS){error=Err(L"Create temporary record failed",GetLastError());return false;}}
    if(f==INVALID_HANDLE_VALUE){error=L"No unique temporary path";return false;}
    DWORD written=0;BOOL ok=WriteFile(f,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);if(ok && written==text.size()) ok=FlushFileBuffers(f);else ok=FALSE;
    DWORD code=ok?ERROR_SUCCESS:GetLastError();if(!CloseHandle(f)&&ok){ok=FALSE;code=GetLastError();}
    if(!ok){DeleteFileW(tmp.c_str());error=Err(L"Write record failed",code);return false;}
    if(!MoveFileExW(tmp.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){code=GetLastError();DeleteFileW(tmp.c_str());error=Err(L"Publish record failed",code);return false;}
    state_=next;return true;
}
bool TaskbarOperationStore::Match(const std::string& id,OperationPhase phase,std::wstring& error)const{
    if(lock_==INVALID_HANDLE_VALUE||state_.phase!=phase||id.empty()||id!=state_.operationId){error=L"Wrong operation, phase, or no lock";return false;}return true;
}
BeginResult TaskbarOperationStore::Begin(const TaskbarStateSnapshot& snap,bool target,std::wstring& error){
    if(lock_==INVALID_HANDLE_VALUE || (state_.phase!=OperationPhase::Idle && state_.phase!=OperationPhase::Restored && state_.phase!=OperationPhase::Abandoned)){error=L"Unfinished record or no lock";return BeginResult::Error;}
    if(!snap.valid || !snap.primaryWindowFound || snap.errorCode!=ERROR_SUCCESS || !snap.errorMessage.empty() || !snap.capturedFileTimeUtc || snap.formatVersion!=1 || snap.autoHide!=((snap.rawFlags&ABS_AUTOHIDE)!=0)){error=L"Invalid original snapshot";return BeginResult::Error;}
    if(snap.autoHide==target)return BeginResult::NoChange;
    TaskbarOperation n; n.operationId=NewId(); FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);n.createdFileTimeUtc=(uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
    n.phase=OperationPhase::Prepared;n.original=snap.autoHide;n.target=target;n.lastObserved=snap.autoHide;
    if(n.operationId.empty()||!Commit(n,error))return BeginResult::Error;return BeginResult::Prepared;
}
bool TaskbarOperationStore::ConfirmApplied(const std::string& id,bool executed,bool observed,std::wstring& error){
    if(!Match(id,OperationPhase::Prepared,error))return false;auto n=state_;n.lastObserved=observed;
    if(!executed||observed!=n.target){n.phase=OperationPhase::Conflict;return Commit(n,error);}
    n.managerChangeConfirmed=true;n.phase=OperationPhase::Applied;return Commit(n,error);
}
bool TaskbarOperationStore::RequestRestore(const std::string& id,bool observed,ChangeEvidence evidence,std::wstring& error){
    if(!Match(id,OperationPhase::Applied,error))return false;auto n=state_;n.lastObserved=observed;
    n.phase=(n.managerChangeConfirmed && observed==n.target && evidence==ChangeEvidence::SimulatedNoExternalChange)?OperationPhase::RestorePending:OperationPhase::Conflict;
    return Commit(n,error);
}
bool TaskbarOperationStore::ConfirmRestored(const std::string& id,bool executed,bool observed,std::wstring& error){
    if(lock_!=INVALID_HANDLE_VALUE && state_.phase==OperationPhase::Restored && id==state_.operationId && observed==state_.original)return true;
    if(!Match(id,OperationPhase::RestorePending,error))return false;auto n=state_;n.lastObserved=observed;
    n.phase=(executed && observed==n.original)?OperationPhase::Restored:OperationPhase::Conflict;return Commit(n,error);
}
bool TaskbarOperationStore::DescribeConflict(const std::string& id,bool simulatedCurrent,ConflictDetails& out,std::wstring& error) const {
    if(!Match(id,OperationPhase::Conflict,error))return false;
    out={state_.operationId,state_.original,state_.target,simulatedCurrent};return true;
}
bool TaskbarOperationStore::CancelConflict(const std::string& id,std::wstring& error) const {
    return Match(id,OperationPhase::Conflict,error); // Deliberately no disk write.
}
bool TaskbarOperationStore::AbandonConflict(const std::string& id,bool simulatedCurrent,bool userConfirmed,std::wstring& error){
    if(!userConfirmed){error=L"Explicit confirmation required";return false;}
    if(lock_!=INVALID_HANDLE_VALUE && !id.empty() && state_.phase==OperationPhase::Abandoned && state_.operationId==id)return true;
    if(!Match(id,OperationPhase::Conflict,error))return false;
    auto n=state_;n.phase=OperationPhase::Abandoned;n.lastObserved=simulatedCurrent;n.managerChangeConfirmed=false;
    return Commit(n,error);
}
bool TaskbarOperationStore::ResolveAfterRestart(std::wstring& error){
    if(lock_==INVALID_HANDLE_VALUE){error=L"Store not locked";return false;}
    if(state_.phase==OperationPhase::Prepared||state_.phase==OperationPhase::Applied||state_.phase==OperationPhase::RestorePending){auto n=state_;n.phase=OperationPhase::Conflict;return Commit(n,error);}return true;
}
std::wstring DefaultTaskbarOperationPath(std::wstring& error){
    auto p=DefaultTaskbarSnapshotPath(error);if(p.empty())return {};const auto i=p.find_last_of(L"\\/");return p.substr(0,i+1)+L"taskbar-operation-v1.txt";
}
}
