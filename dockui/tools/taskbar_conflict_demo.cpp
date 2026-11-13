#include "taskbar_ownership.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace liquid::desktop;
int main() {
    std::puts("SIMULATION ONLY: no Windows taskbar settings are read or changed.");
    wchar_t temp[MAX_PATH]{}; if (!GetTempPathW(MAX_PATH,temp)) return 1;
    const std::wstring path=std::wstring(temp)+L"LiquidDesktop-conflict-demo-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".txt";
    std::wstring error; TaskbarOperationStore store; if(!store.Open(path,error)) return 1;
    TaskbarStateSnapshot original; original.valid=true;original.primaryWindowFound=true;original.capturedFileTimeUtc=133000000000000000ull;
    if(store.Begin(original,true,error)!=BeginResult::Prepared) return 1;
    const auto id=store.Current().operationId;
    if(!store.ConfirmApplied(id,true,true,error)||!store.RequestRestore(id,false,ChangeEvidence::ExternalChange,error)) return 1;
    ConflictDetails d; if(!store.DescribeConflict(id,false,d,error)) return 1;
    std::printf("Conflict: original=%s expected=%s current=%s\n",d.original?"auto-hide on":"auto-hide off",d.expected?"auto-hide on":"auto-hide off",d.current?"auto-hide on":"auto-hide off");
    std::puts("1: Keep current state and abandon recovery.  2: Cancel and keep Conflict (default).");
    char input[32]{}; if(!std::fgets(input,sizeof(input),stdin) || std::strcmp(input,"1\n")!=0){store.CancelConflict(id,error);std::puts("Cancelled; Conflict retained.");return 0;}
    std::puts("Type ABANDON to confirm keeping the current state and relinquishing recovery:");
    if(!std::fgets(input,sizeof(input),stdin) || std::strcmp(input,"ABANDON\n")!=0){store.CancelConflict(id,error);std::puts("Not confirmed; Conflict retained.");return 0;}
    if(!store.AbandonConflict(id,d.current,true,error)){std::fputws(error.c_str(),stderr);return 1;}
    std::puts("Confirmed: recovery abandoned; current simulated state retained.");return 0;
}
