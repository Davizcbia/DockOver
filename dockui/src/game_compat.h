#pragma once
// Game compatibility, manual game list (Settings > Menu Bar > Game compatibility > Games list).
//
// The user lists the programs (one .exe name per line, '#' starts a comment) during which DockOver pauses desktop
// capture and its non-essential overlays. Detection only compares the names in a Toolhelp process snapshot with that
// list: no process is opened, nothing is read from or written to another process, and no anti-cheat process is looked
// for. The Dock, the Menu Bar and DockOver Desktop each check every couple of seconds (cheap: one snapshot).
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

namespace docky::game {

// game-compat.txt beside the settings file (desktop.json).
inline std::wstring ListPath(const std::wstring& settingsPath) {
    const size_t slash = settingsPath.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : settingsPath.substr(0, slash + 1)) + L"game-compat.txt";
}

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }

// Lower-case executable names from the list; missing file = empty list. UTF-8 or ANSI text.
inline std::vector<std::wstring> LoadList(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line) && out.size() < 256) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // UTF-8 BOM
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line.erase(0, start);
        const int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring name(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), name.data(), n);
        if (const size_t s = name.find_last_of(L"\\/"); s != std::wstring::npos) name.erase(0, s + 1);  // a full path: its file name
        if (!name.empty()) out.push_back(Lower(name));
    }
    return out;
}

// First listed program that is running (its name), or empty.
inline std::wstring RunningListed(const std::vector<std::wstring>& list) {
    if (list.empty()) return {};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W e{sizeof(e)};
    std::wstring found;
    for (BOOL ok = Process32FirstW(snap, &e); ok && found.empty(); ok = Process32NextW(snap, &e)) {
        const std::wstring name = Lower(e.szExeFile);
        if (std::find(list.begin(), list.end(), name) != list.end()) found = e.szExeFile;
    }
    CloseHandle(snap);
    return found;
}

// Creates the list with instructions (all examples commented out) when it does not exist yet.
inline bool EnsureList(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "# DockOver game compatibility list\r\n"
           "# While one of these programs runs (and Game compatibility is on), DockOver hides the Dock and Menu Bar,\r\n"
           "# stops desktop capture and pauses its overlays; everything comes back when the program exits.\r\n"
           "# One executable name per line. Lines starting with # are ignored. Remove the # to use an example.\r\n"
           "# TslGame.exe\r\n"
           "# FortniteClient-Win64-Shipping.exe\r\n"
           "# VALORANT-Win64-Shipping.exe\r\n";
    return static_cast<bool>(out);
}

// Polls the list at most every `everyMs` (reloads the file when it changes). Owner thread only.
class Watcher {
public:
    // Returns the running listed program's name (empty: none). `enabled` false: always empty and no snapshots.
    const std::wstring& Poll(const std::wstring& settingsPath, bool enabled, ULONGLONG everyMs = 2000) {
        if (!enabled) { running_.clear(); return running_; }
        const ULONGLONG now = GetTickCount64();
        if (checked_ && now - checked_ < everyMs) return running_;
        checked_ = now;
        const std::wstring path = ListPath(settingsPath);
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const ULONGLONG stamp = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)
            ? (static_cast<ULONGLONG>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^ a.nFileSizeLow : 0;
        if (stamp != stamp_ || path != path_) { stamp_ = stamp; path_ = path; list_ = LoadList(path); }
        running_ = RunningListed(list_);
        return running_;
    }
private:
    std::wstring path_, running_;
    std::vector<std::wstring> list_;
    ULONGLONG stamp_ = 0, checked_ = 0;
};

}  // namespace docky::game
