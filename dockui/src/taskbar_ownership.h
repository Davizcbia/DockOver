#pragma once
#include "taskbar_snapshot.h"
#include <string>

namespace liquid::desktop {
// Simulation-only state machine: no taskbar-writing API is exposed.
enum class OperationPhase { Idle, Prepared, Applied, RestorePending, Restored, Conflict, Abandoned };
struct TaskbarOperation {
    unsigned version = 1;
    std::string operationId; // 128-bit random GUID as 32 hexadecimal digits
    uint64_t createdFileTimeUtc = 0;
    OperationPhase phase = OperationPhase::Idle;
    bool original = false, target = false, lastObserved = false;
    bool managerChangeConfirmed = false;
};
struct ConflictDetails {
    std::string operationId;
    bool original = false;
    bool expected = false;
    bool current = false; // Supplied by simulator; not a Windows query.
};
enum class ChangeEvidence { Unverifiable, ExternalChange, SimulatedNoExternalChange };
enum class BeginResult { Prepared, NoChange, Error };
class TaskbarOperationStore {
public:
    TaskbarOperationStore() = default;
    ~TaskbarOperationStore();
    TaskbarOperationStore(const TaskbarOperationStore&) = delete;
    TaskbarOperationStore& operator=(const TaskbarOperationStore&) = delete;
    bool Open(const std::wstring& recordPath, std::wstring& error);
    // Acquires an existing lock and reads only: never changes a lifecycle record.
    bool OpenForInspection(const std::wstring& recordPath, std::wstring& error);
    const TaskbarOperation& Current() const { return state_; }
    BeginResult Begin(const TaskbarStateSnapshot& original, bool target, std::wstring& error);
    bool ConfirmApplied(const std::string& id, bool simulatedManagerExecuted, bool observed, std::wstring& error);
    bool RequestRestore(const std::string& id, bool observed, ChangeEvidence evidence, std::wstring& error);
    bool ConfirmRestored(const std::string& id, bool simulatedRestoreExecuted, bool observed, std::wstring& error);
    // Confirmation and observed state are simulation inputs, never authority to change Windows.
    bool DescribeConflict(const std::string& id, bool simulatedCurrent, ConflictDetails& out, std::wstring& error) const;
    bool CancelConflict(const std::string& id, std::wstring& error) const;
    bool AbandonConflict(const std::string& id, bool simulatedCurrent, bool userConfirmed, std::wstring& error);
    // Any unfinished record after restart is ambiguous; persist Conflict, never auto restore.
    bool ResolveAfterRestart(std::wstring& error);
private:
    bool Commit(const TaskbarOperation&, std::wstring& error);
    bool Load(std::wstring& error);
    bool Match(const std::string& id, OperationPhase phase, std::wstring& error) const;
    HANDLE lock_ = INVALID_HANDLE_VALUE;
    bool inspectionOnly_ = false;
    std::wstring path_;
    TaskbarOperation state_{};
};
// Derives the record path in LocalAppData/LiquidDesktop without modifying taskbar settings.
std::wstring DefaultTaskbarOperationPath(std::wstring& error);
}
