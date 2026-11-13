#pragma once
#include <windows.h>
namespace docky {
// `event` is an inherited, single-owner handle; never signal on failed/occluded Present.
inline bool SignalFirstPresent(HRESULT presentResult,HANDLE& event){
 if(presentResult!=S_OK || !event || event==INVALID_HANDLE_VALUE)return false;
 const bool ok=SetEvent(event)!=FALSE;CloseHandle(event);event=nullptr;return ok;
}
}
