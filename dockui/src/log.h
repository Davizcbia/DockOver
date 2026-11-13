#pragma once
// Thread-safe UTF-8 file logger shared by every docky process.
//
// - Levels: Error, Warn, Info (default threshold), Debug. DLOG is Info; per-frame and other chatty lines use DLOG_DEBUG,
//   which costs one comparison when debug logging is off. Debug logging is on with --log-debug on any docky process, the
//   environment variable DOCKY_LOG_LEVEL=debug, or a file named "log-debug.on" next to the log.
// - Size: a log rotates at 5 MB into <name>.1 (and .1 into .2), so one log never takes more than about 15 MB.
// - Writing: lines go to a queue that a background thread writes about twice a second, so render threads never wait for
//   the disk. Errors are written at once (and the queue with them), so the lines before a failure are on disk.
// - Repeats: the same message several times in a row is written once, then "previous message repeated N times".
#include <windows.h>
#include <share.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace dock {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

class Log {
public:
    static Log& Get() {
        static Log instance;
        return instance;
    }

    bool Open(const std::wstring& path) {
        std::unique_lock<std::mutex> lock(mutex_);
        CloseFileLocked();
        path_ = path;
        Rotate(true);
        file_ = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        // Debug threshold switches (see the header comment).
        wchar_t env[16]{};
        if (GetEnvironmentVariableW(L"DOCKY_LOG_LEVEL", env, 16) && _wcsicmp(env, L"debug") == 0) level_ = static_cast<int>(LogLevel::Debug);
        const size_t slash = path.find_last_of(L"\\/");
        const std::wstring flag = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"log-debug.on";
        if (GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES || wcsstr(GetCommandLineW(), L"--log-debug")) level_ = static_cast<int>(LogLevel::Debug);
        if (!writer_.joinable()) writer_ = std::thread([this] { WriterLoop(); });
        return file_ != nullptr;
    }

    void Close() {
        Flush();
        std::lock_guard<std::mutex> lock(mutex_);
        CloseFileLocked();
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        EmitRepeatLocked();
        WriteQueueLocked();
    }

    void SetLevel(LogLevel level) { level_ = static_cast<int>(level); }
    bool Enabled(LogLevel level) const { return static_cast<int>(level) <= level_.load(std::memory_order_relaxed); }

    void Write(LogLevel level, const wchar_t* format, ...) {
        if (!Enabled(level)) return;
        wchar_t message[2048];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, _TRUNCATE, format, args);
        va_end(args);
        // Untagged lines that report a failure count as warnings (the existing call sites predate the levels).
        if (level == LogLevel::Info && (wcsstr(message, L"failed") || wcsstr(message, L"FAILED") || wcsstr(message, L"ERROR"))) level = LogLevel::Warn;
        std::unique_lock<std::mutex> lock(mutex_);
        if (lastMessage_ == message && level != LogLevel::Error) { if (!repeats_++) repeatSince_ = GetTickCount64(); return; }
        EmitRepeatLocked();
        lastMessage_ = message;
        queue_.push_back(Format(level, message));
        if (IsDebuggerPresent()) OutputDebugStringW(queue_.back().c_str());
        if (level == LogLevel::Error) WriteQueueLocked();
        else if (queue_.size() >= 512) wake_.notify_one();
    }
    // Pre-level form: Info.
    void Write(const wchar_t* format, ...) {
        if (!Enabled(LogLevel::Info)) return;
        wchar_t message[2048];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, _TRUNCATE, format, args);
        va_end(args);
        Write(LogLevel::Info, L"%ls", message);
    }

private:
    static constexpr long long kMaxBytes = 5ll * 1024 * 1024;

    Log() = default;
    ~Log() {
        { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
        wake_.notify_one();
        if (writer_.joinable()) writer_.join();
        Close();
    }

    static std::wstring Format(LogLevel level, const wchar_t* message) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        static const wchar_t* tags[] = {L"E ", L"W ", L"", L"D "};
        wchar_t line[2200];
        _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %ls%ls\n", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), tags[static_cast<int>(level)], message);
        return line;
    }

    void EmitRepeatLocked() {
        if (repeats_ == 0) return;
        wchar_t note[96];
        _snwprintf_s(note, _TRUNCATE, L"(previous message repeated %llu more times)", static_cast<unsigned long long>(repeats_));
        repeats_ = 0;
        queue_.push_back(Format(LogLevel::Info, note));
    }

    void WriteQueueLocked() {
        if (queue_.empty()) return;
        if (file_) {
            for (const auto& line : queue_) fputws(line.c_str(), file_);
            fflush(file_);
            if (_ftelli64(file_) > kMaxBytes) {
                CloseFileLocked();
                Rotate(false);
                file_ = _wfsopen(path_.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
            }
        }
        queue_.clear();
    }

    // <name> -> <name>.1 -> <name>.2 (dropped). At Open only when the existing file is already over the limit.
    void Rotate(bool onlyIfLarge) {
        if (path_.empty()) return;
        if (onlyIfLarge) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &d)) return;
            const long long size = (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            if (size <= kMaxBytes) return;
        }
        const std::wstring one = Numbered(1), two = Numbered(2);
        MoveFileExW(one.c_str(), two.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!MoveFileExW(path_.c_str(), one.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // Another process still holds the file (an older build): start it over instead of growing it further.
            HANDLE h = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { SetEndOfFile(h); CloseHandle(h); }
        }
    }
    std::wstring Numbered(int n) const {
        const size_t dot = path_.find_last_of(L'.'), slash = path_.find_last_of(L"\\/");
        if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return path_ + L"." + std::to_wstring(n);
        return path_.substr(0, dot) + L"." + std::to_wstring(n) + path_.substr(dot);  // dock.log -> dock.1.log
    }

    void CloseFileLocked() {
        if (file_) fclose(file_);
        file_ = nullptr;
    }

    void WriterLoop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!quit_) {
            wake_.wait_for(lock, std::chrono::milliseconds(500));
            if (repeats_ && GetTickCount64() - repeatSince_ > 60000) EmitRepeatLocked();  // a line that keeps repeating: a count a minute
            WriteQueueLocked();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread writer_;
    std::deque<std::wstring> queue_;
    std::wstring path_, lastMessage_;
    unsigned long long repeats_ = 0, repeatSince_ = 0;
    std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
    bool quit_ = false;
    FILE* file_ = nullptr;
};

}  // namespace dock

#define DLOG(...) ::dock::Log::Get().Write(__VA_ARGS__)
#define DLOG_DEBUG(...) do { if (::dock::Log::Get().Enabled(::dock::LogLevel::Debug)) ::dock::Log::Get().Write(::dock::LogLevel::Debug, __VA_ARGS__); } while (0)
#define DLOG_WARN(...) ::dock::Log::Get().Write(::dock::LogLevel::Warn, __VA_ARGS__)
#define DLOG_ERROR(...) ::dock::Log::Get().Write(::dock::LogLevel::Error, __VA_ARGS__)
