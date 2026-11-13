#include "taskbar_snapshot.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <limits>
#include <sstream>
#include <iomanip>

namespace liquid::desktop {
namespace {
uint64_t Checksum(const std::string& text) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    return h;
}
std::string Serialize(const TaskbarStateSnapshot& s) {
    std::ostringstream o;
    o << "LIQUID_DESKTOP_TASKBAR_SNAPSHOT 1\nraw=" << static_cast<unsigned long long>(s.rawFlags)
      << "\nautohide=" << static_cast<int>(s.autoHide) << "\nprimary=" << static_cast<int>(s.primaryWindowFound)
      << "\ncaptured_filetime_utc=" << s.capturedFileTimeUtc << "\nvalid=1\nphase=observed\nowner=none\n";
    const auto body = o.str();
    o << "checksum_fnv1a64=" << std::hex << std::setw(16) << std::setfill('0') << Checksum(body) << "\n";
    return o.str();
}
std::wstring WinError(const wchar_t* action, DWORD error) {
    return std::wstring(action) + L" (Windows error " + std::to_wstring(error) + L")";
}
bool IsValid(const TaskbarStateSnapshot& s) {
    return s.formatVersion == 1 && s.valid && s.primaryWindowFound && s.capturedFileTimeUtc != 0 &&
           s.errorCode == ERROR_SUCCESS && s.errorMessage.empty() && s.autoHide == ((s.rawFlags & ABS_AUTOHIDE) != 0);
}
}
TaskbarStateSnapshot CaptureTaskbarSnapshot() {
    TaskbarStateSnapshot out;
    const TaskbarStatus before = QueryTaskbarStatus();
    const TaskbarStatus after = QueryTaskbarStatus();
    out.rawFlags = after.rawFlags; out.autoHide = after.autoHideFlag; out.primaryWindowFound = after.primaryWindowFound;
    FILETIME ft{}; GetSystemTimePreciseAsFileTime(&ft);
    out.capturedFileTimeUtc = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (!before.primaryWindowFound || !after.primaryWindowFound) {
        out.errorCode = ERROR_NOT_FOUND; out.errorMessage = L"Primary taskbar not found; query cannot be trusted";
    } else if (before.rawFlags != after.rawFlags) {
        out.errorCode = ERROR_RETRY; out.errorMessage = L"Shell state changed during query";
    } else out.valid = true;
    // ABM_GETSTATE has no distinct failure indicator: even valid is a best-effort observation.
    return out;
}
RecoveryPlan PlanRecovery(const TaskbarStateSnapshot& s, const TaskbarStatus& current, bool managerChanged,
                          bool lastAppliedKnown, bool lastApplied, bool externalChangeObserved) noexcept {
    if (!IsValid(s) || !current.primaryWindowFound) return RecoveryPlan::InvalidOrUnknown;
    if (externalChangeObserved || (!managerChanged && current.autoHideFlag != s.autoHide)) return RecoveryPlan::ExternalChangeOrUnknown;
    if (!managerChanged) return RecoveryPlan::NoManagerChange;
    if (!lastAppliedKnown || current.autoHideFlag != lastApplied) return RecoveryPlan::ExternalChangeOrUnknown;
    return RecoveryPlan::RequiresReview; // Matching bits cannot prove the user did not change them.
}
std::wstring DefaultTaskbarSnapshotPath(std::wstring& error) {
    PWSTR base = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base);
    if (FAILED(hr)) { error = L"LocalAppData lookup failed (HRESULT " + std::to_wstring(static_cast<long>(hr)) + L")"; return {}; }
    std::wstring dir(base); CoTaskMemFree(base); dir += L"\\LiquidDesktop";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { error = WinError(L"Create directory failed", GetLastError()); return {}; }
    return dir + L"\\taskbar-snapshot-v1.txt";
}
bool SaveTaskbarSnapshotNew(const std::wstring& path, const TaskbarStateSnapshot& snapshot, std::wstring& error) {
    if (!IsValid(snapshot)) { error = L"Invalid snapshot cannot be saved for recovery"; return false; }
    const std::string bytes = Serialize(snapshot);
    std::wstring temp;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (unsigned i=0; i<20 && f==INVALID_HANDLE_VALUE; ++i) {
        temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(i);
        f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (f==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS) { error=WinError(L"Create temporary snapshot failed",GetLastError()); return false; }
    }
    if (f==INVALID_HANDLE_VALUE) { error=L"Cannot allocate a unique temporary file"; return false; }
    DWORD written=0; BOOL ok=WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
    if (ok && written==bytes.size()) ok=FlushFileBuffers(f); else ok=FALSE;
    const DWORD writeError=ok ? ERROR_SUCCESS : GetLastError();
    if (!CloseHandle(f) && ok) { error=WinError(L"Close temporary snapshot failed",GetLastError()); DeleteFileW(temp.c_str()); return false; }
    if (!ok) { error=WinError(L"Write or flush snapshot failed",writeError); DeleteFileW(temp.c_str()); return false; }
    if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)) {
        const DWORD code=GetLastError(); DeleteFileW(temp.c_str());
        error=WinError(code==ERROR_ALREADY_EXISTS || code==ERROR_FILE_EXISTS ? L"Snapshot already exists; preserved" : L"Publish snapshot failed",code); return false;
    }
    return true;
}
bool LoadTaskbarSnapshot(const std::wstring& path, TaskbarStateSnapshot& out, std::wstring& error) {
    out = {};
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (f==INVALID_HANDLE_VALUE) { error=WinError(L"Open snapshot failed",GetLastError()); return false; }
    LARGE_INTEGER sz{}; if (!GetFileSizeEx(f,&sz) || sz.QuadPart<=0 || sz.QuadPart>1024) { CloseHandle(f); error=L"Snapshot size invalid"; return false; }
    std::string bytes(static_cast<size_t>(sz.QuadPart),'\0'); DWORD read=0;
    BOOL ok=ReadFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr); CloseHandle(f);
    if (!ok || read!=bytes.size()) { error=L"Snapshot read incomplete"; return false; }
    unsigned long long raw=0, time=0; int autoHide=0, primary=0, n=0;
    if (std::sscanf(bytes.c_str(),"LIQUID_DESKTOP_TASKBAR_SNAPSHOT 1\nraw=%llu\nautohide=%d\nprimary=%d\ncaptured_filetime_utc=%llu\nvalid=1\nphase=observed\nowner=none\n%n",&raw,&autoHide,&primary,&time,&n)!=4 ||
        raw>static_cast<unsigned long long>((std::numeric_limits<UINT_PTR>::max)()) || (autoHide!=0 && autoHide!=1) || primary!=1 || n<=0) { error=L"Snapshot format invalid"; return false; }
    TaskbarStateSnapshot parsed; parsed.rawFlags=static_cast<UINT_PTR>(raw); parsed.autoHide=autoHide!=0; parsed.primaryWindowFound=true;
    parsed.capturedFileTimeUtc=time; parsed.valid=true;
    if (!IsValid(parsed) || Serialize(parsed)!=bytes) { error=L"Snapshot corrupt, inconsistent, or unsupported"; return false; }
    out=parsed; return true;
}
}
