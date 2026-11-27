#pragma once
#include "app_registry.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>

namespace dock::apps::steam {
namespace fs = std::filesystem;

// Steam writes quoted Valve KeyValues. Ignore braces and comments while reading the string tokens.
inline std::vector<std::string> ReadTokens(const fs::path& file, std::uintmax_t limit) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > limit) return {};
    std::ifstream input(file, std::ios::binary);
    std::string data(static_cast<size_t>(size), '\0');
    if (!input.read(data.data(), data.size())) return {};
    std::vector<std::string> tokens;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '/' && i + 1 < data.size() && data[i + 1] == '/') {
            i = data.find('\n', i);
            if (i == std::string::npos) break;
        } else if (data[i] == '"') {
            std::string value;
            while (++i < data.size() && data[i] != '"') {
                if (data[i] == '\\' && i + 1 < data.size() && (data[i + 1] == '\\' || data[i + 1] == '"')) ++i;
                value.push_back(data[i]);
            }
            if (i == data.size()) return {};
            tokens.push_back(std::move(value));
        }
    }
    return tokens;
}

inline std::wstring Widen(const std::string& utf8) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!size) return {};
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

inline std::wstring Directory() {
    wchar_t buffer[32768]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
        return buffer;
    for (DWORD view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
        size = sizeof(buffer);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ | view, nullptr, buffer, &size) == ERROR_SUCCESS)
            return buffer;
    }
    return {};
}

// Steam's current sharded cache includes common/type in each v42 index summary. Read
// the index only, leaving the binary KeyValues, tokens, and other cache data untouched.
inline unsigned AppType(const fs::path& root, std::string_view idText) {
    unsigned id = 0;
    const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size()) return 0;
    unsigned hash = id;
    hash = (hash ^ (hash >> 16)) * 0x85ebca6b;
    hash = (hash ^ (hash >> 13)) * 0xc2b2ae35;
    hash ^= hash >> 16;
    wchar_t filename[16]{};
    swprintf_s(filename, L"appinfo_%02x.vdf", hash & 0xff);
    std::ifstream input(root / L"appcache/appinfo" / filename, std::ios::binary);
    unsigned header[11]{};
    if (!input.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        header[0] != 0x0756442a || header[5] > 16 * 1024 * 1024) return 0;
    std::vector<char> index(header[5]);
    if (!input.read(index.data(), index.size())) return 0;
    const auto read = [&](size_t at) { unsigned value = 0; std::memcpy(&value, index.data() + at, sizeof(value)); return value; };
    constexpr size_t entryHeader = 16, summaryType = 37;
    for (size_t pos = 0; pos + entryHeader <= index.size();) {
        const unsigned summaryLength = read(pos + 12);
        if (summaryLength > index.size() - pos - entryHeader) break;
        if (read(pos) == id)
            return summaryLength >= summaryType + sizeof(unsigned) ? read(pos + entryHeader + summaryType) : 0;
        pos += entryHeader + summaryLength;
    }
    return 0;
}

inline std::vector<AppEntry> InstalledGames(const fs::path& root) {
    if (root.empty()) return {};
    std::set<fs::path> libraries{root.lexically_normal()};
    const auto tokens = ReadTokens(root / L"steamapps/libraryfolders.vdf", 8 * 1024 * 1024);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        const fs::path library = fs::path(Widen(tokens[i + 1])).lexically_normal();
        // Older Steam clients used numbered keys directly, newer clients use a "path" property.
        if (library.is_absolute() && (tokens[i] == "path" ||
            (!tokens[i].empty() && tokens[i].find_first_not_of("0123456789") == std::string::npos)))
            libraries.insert(library);
    }
    std::vector<AppEntry> games;
    std::set<std::string> seen;
    for (const auto& library : libraries) {
        const auto apps = library / L"steamapps";
        std::error_code error;
        for (fs::directory_iterator it(apps, error), end; !error && it != end; it.increment(error)) {
            const auto filename = it->path().filename().wstring();
            if (!filename.starts_with(L"appmanifest_") || !filename.ends_with(L".acf")) continue;
            const auto fileId = filename.substr(12, filename.size() - 16);
            if (fileId.empty() || fileId.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
            const auto fields = ReadTokens(it->path(), 1024 * 1024);
            std::string id, name, dir;
            for (size_t i = 0; i + 1 < fields.size(); ++i) {
                if (fields[i] == "appid" && id.empty()) id = fields[i + 1];
                else if (fields[i] == "name" && name.empty()) name = fields[i + 1];
                else if (fields[i] == "installdir" && dir.empty()) dir = fields[i + 1];
            }
            const auto display = Widen(name);
            const fs::path install = Widen(dir);
            std::error_code existsError;
            if (Widen(id) != fileId || id == "228980" || display.empty() || install.empty() ||
                install != install.filename() || install == L"." || install == L".." ||
                !fs::is_directory(apps / L"common" / install, existsError) || !seen.insert(id).second) continue;
            const unsigned type = AppType(root, id);
            const bool isGame = type == 0 || (type & (1u | 8u)) != 0;   // unknown older cache, game or demo
            games.push_back({"steam:" + id, display, L"steam://rungameid/" + fileId, true, isGame});
        }
    }
    return games;
}
}  // namespace dock::apps::steam
