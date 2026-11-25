#pragma once

#include "../json_lite.h"

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace docky::pairing {

inline constexpr wchar_t kDropWidgetKind[] = L"dockdrop";
inline constexpr ULONG_PTR kLoadWidgetDraft = 0x44445731;
inline constexpr size_t kWidgetMaxFiles = 64;
inline constexpr uint64_t kWidgetMaxBytes = 128ull * 1024 * 1024;
inline constexpr size_t kWidgetMaxText = 65536;
inline constexpr size_t kWidgetMaxSegments = 64;

struct WidgetDraft {
    std::vector<std::wstring> files;
    std::vector<std::wstring> texts;
    bool Empty() const { return files.empty() && texts.empty(); }
    std::wstring JoinedText() const {
        std::wstring joined;
        for (const auto& text : texts) {
            if (!joined.empty()) joined += L"\n\n";
            joined += text;
        }
        return joined;
    }
};

// The desktop and either DockDrop executable share a per-user draft.
// Hold the lock across a read/modify/write so new drops can arrive during a send.
class WidgetQueue {
public:
    WidgetQueue() {
        wchar_t local[32768]{};
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
        if (!count || count >= ARRAYSIZE(local)) return;
        directory_ = std::wstring(local, count) + L"\\DockDrop";
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\DockDropDesktopWidgetQueueV1");
        if (!mutex_) return;
        const DWORD wait = WaitForSingleObject(mutex_, 200);
        locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~WidgetQueue() {
        if (locked_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    WidgetQueue(const WidgetQueue&) = delete;
    WidgetQueue& operator=(const WidgetQueue&) = delete;

    bool Read(WidgetDraft& draft) const {
        if (!locked_) return false;
        const auto path = Path();
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
            draft = {};
            return true;
        }
        const DWORD size = GetFileSize(file, nullptr);
        constexpr DWORD maxJson = 16 * 1024 * 1024;
        std::string data(size > 0 && size <= maxJson ? size : 0, '\0');
        DWORD read = 0;
        const bool ok = !data.empty() && ReadFile(file, data.data(), size, &read, nullptr) && read == size;
        CloseHandle(file);
        dock::json::Value value;
        std::string error;
        if (!ok || !dock::json::Parse(data, value, error)) return false;
        using Type = dock::json::Value::Type;
        const auto* paths = value.Find("files");
        if (!paths || paths->type != Type::Array || paths->array.size() > kWidgetMaxFiles) return false;
        WidgetDraft next;
        for (const auto& pathValue : paths->array) {
            if (pathValue.type != Type::String || pathValue.string.empty() ||
                pathValue.string.find('\0') != std::string::npos) return false;
            next.files.push_back(dock::json::FromUtf8(pathValue.string));
        }
        if (const auto* segments = value.Find("texts")) {
            if (segments->type != Type::Array || segments->array.size() > kWidgetMaxSegments) return false;
            size_t bytes = 0;
            for (const auto& segment : segments->array) {
                if (segment.type != Type::String || segment.string.empty() ||
                    segment.string.find('\0') != std::string::npos) return false;
                bytes += segment.string.size() + (next.texts.empty() ? 0 : 2);
                if (bytes > kWidgetMaxText) return false;
                next.texts.push_back(dock::json::FromUtf8(segment.string));
            }
        } else {
            const auto* legacy = value.Find("text");
            if (!legacy || legacy->type != Type::String || legacy->string.size() > kWidgetMaxText) return false;
            if (!legacy->string.empty()) next.texts.push_back(dock::json::FromUtf8(legacy->string));
        }
        draft = std::move(next);
        return true;
    }

    bool Write(const WidgetDraft& draft) const {
        if (!locked_) return false;
        const auto path = Path();
        if (draft.Empty()) {
            if (DeleteFileW(path.c_str())) return true;
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (!CreateDirectoryW(directory_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        using dock::json::Value;
        auto value = Value::MakeObject();
        Value paths;
        paths.type = Value::Type::Array;
        for (const auto& file : draft.files) paths.array.push_back(Value::MakeString(dock::json::ToUtf8(file)));
        value.Set("files", std::move(paths));
        Value segments;
        segments.type = Value::Type::Array;
        for (const auto& text : draft.texts) segments.array.push_back(Value::MakeString(dock::json::ToUtf8(text)));
        value.Set("texts", std::move(segments));
        const auto data = dock::json::Write(value, 0);
        const auto temp = path + L".tmp";
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const auto bytes = static_cast<DWORD>(data.size());
        const bool saved = WriteFile(file, data.data(), bytes, &written, nullptr) &&
            written == bytes && FlushFileBuffers(file);
        CloseHandle(file);
        if (saved && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        DeleteFileW(temp.c_str());
        return false;
    }

    bool Append(const WidgetDraft& drop) const {
        if (drop.Empty()) return false;
        WidgetDraft draft;
        if (!Read(draft)) return false;
        for (const auto& path : drop.files) {
            if (!Contains(draft.files, path)) draft.files.push_back(path);
        }
        if (draft.files.size() > kWidgetMaxFiles) return false;
        if (!drop.files.empty()) {
            uint64_t total = 0;
            for (const auto& path : draft.files) {
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
                    (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
                const uint64_t size = (uint64_t{attributes.nFileSizeHigh} << 32) | attributes.nFileSizeLow;
                if (size > kWidgetMaxBytes - total) return false;
                total += size;
            }
        }
        draft.texts.insert(draft.texts.end(), drop.texts.begin(), drop.texts.end());
        if (draft.texts.size() > kWidgetMaxSegments ||
            dock::json::ToUtf8(draft.JoinedText()).size() > kWidgetMaxText) return false;
        return Write(draft);
    }

    bool Acknowledge(const WidgetDraft& sent) const {
        if (sent.Empty()) return true;
        WidgetDraft draft;
        if (!Read(draft)) return false;
        std::erase_if(draft.files, [&](const auto& path) { return Contains(sent.files, path); });
        if (draft.texts.size() >= sent.texts.size() &&
            std::equal(sent.texts.begin(), sent.texts.end(), draft.texts.begin()))
            draft.texts.erase(draft.texts.begin(), draft.texts.begin() + sent.texts.size());
        return Write(draft);
    }

private:
    std::wstring Path() const { return directory_ + L"\\widget-draft.json"; }
    static bool Contains(const std::vector<std::wstring>& paths, const std::wstring& path) {
        return std::any_of(paths.begin(), paths.end(), [&](const auto& other) {
            return CompareStringOrdinal(path.c_str(), -1, other.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    }
    std::wstring directory_;
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
};

}  // namespace docky::pairing
