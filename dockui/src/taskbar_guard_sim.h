#pragma once
// V0.11: explicit isolated simulation only. No Windows taskbar calls are permitted here.
#include <windows.h>
#include <string>
namespace docky::guard_sim {
class Session {
public:
 ~Session();
 bool Start(HANDLE dock,HANDLE managerGate,const std::wstring& directory,bool crashAfterHide);
 void Stop();
 bool RecoverAfterGuardExit();
 HANDLE Process()const{return process_;}
private:
 bool Recover();void Close();
 HANDLE process_=nullptr,stop_=nullptr,ready_=nullptr,hidden_=nullptr,lock_=nullptr;
 std::wstring record_;
};
// Called only by the dedicated simulation guard executable with an explicit test record.
int RunGuard(HANDLE manager,HANDLE dock,HANDLE stop,HANDLE ready,HANDLE hidden,HANDLE lock,HANDLE gate,const std::wstring& record);
}
