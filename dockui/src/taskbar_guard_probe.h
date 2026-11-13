#pragma once
// Isolated watchdog protocol probe. This code has no taskbar access or persistent side effects.
// The manager/dock arguments must be process handles duplicated into the guard process.
#include <windows.h>

namespace docky::guard_probe {
enum class Result : int { ManagerExited=10, DockExited=11, RestoreRequested=12, Invalid=13 };
inline Result Wait(HANDLE manager, HANDLE dock, HANDLE restoreRequest, HANDLE ready) {
    if(!manager || !dock || !restoreRequest || !ready || manager==INVALID_HANDLE_VALUE ||
       dock==INVALID_HANDLE_VALUE || restoreRequest==INVALID_HANDLE_VALUE || ready==INVALID_HANDLE_VALUE)
        return Result::Invalid;
    HANDLE waits[]{manager,dock,restoreRequest};
    // Never advertise readiness if the manager, Dock, or restore request was already signaled.
    DWORD result=WaitForMultipleObjects(3,waits,FALSE,0);
    if(result==WAIT_OBJECT_0)return Result::ManagerExited;
    if(result==WAIT_OBJECT_0+1)return Result::DockExited;
    if(result==WAIT_OBJECT_0+2)return Result::RestoreRequested;
    if(result!=WAIT_TIMEOUT || !SetEvent(ready))return Result::Invalid;
    result=WaitForMultipleObjects(3,waits,FALSE,INFINITE);
    if(result==WAIT_OBJECT_0)return Result::ManagerExited;
    if(result==WAIT_OBJECT_0+1)return Result::DockExited;
    if(result==WAIT_OBJECT_0+2)return Result::RestoreRequested;
    return Result::Invalid;
}
}
