#pragma once
#include "taskbar_status.h"
#include <cstdint>
#include <string>

namespace liquid::desktop {
struct TaskbarStateSnapshot {
    uint32_t formatVersion = 1;
    UINT_PTR rawFlags = 0;
    bool autoHide = false;
    bool primaryWindowFound = false;
    uint64_t capturedFileTimeUtc = 0; // 100-ns ticks since 1601 UTC.
    bool valid = false;
    DWORD errorCode = ERROR_SUCCESS;
    std::wstring errorMessage;
};

enum class RecoveryPlan { InvalidOrUnknown, NoManagerChange, ExternalChangeOrUnknown, RequiresReview };
TaskbarStateSnapshot CaptureTaskbarSnapshot();
// Only a diagnostic plan; no Windows taskbar state is changed or restored.
RecoveryPlan PlanRecovery(const TaskbarStateSnapshot&, const TaskbarStatus&, bool managerChanged,
                          bool lastAppliedKnown, bool lastApplied, bool externalChangeObserved) noexcept;
// Creates LocalAppData\\LiquidDesktop if needed; does not create a snapshot.
std::wstring DefaultTaskbarSnapshotPath(std::wstring& error);
// Atomic same-directory publication, never replaces any existing file, even invalid ones.
bool SaveTaskbarSnapshotNew(const std::wstring&, const TaskbarStateSnapshot&, std::wstring& error);
bool LoadTaskbarSnapshot(const std::wstring&, TaskbarStateSnapshot&, std::wstring& error);
}
