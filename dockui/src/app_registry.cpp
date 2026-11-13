#include "app_registry.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <set>

using Microsoft::WRL::ComPtr;

namespace dock::apps {
namespace {

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

}  // namespace

bool IsLaunchableEntry(const std::wstring& name, const std::wstring& parsingName) {
    if (name.empty() || parsingName.empty()) return false;
    const std::wstring n = Lower(name), p = Lower(parsingName);
    // Uninstallers and maintenance shortcuts are not applications a user launches from a grid.
    for (const wchar_t* word : {L"uninstall", L"卸载", L"remove ", L"修复", L"repair "})
        if (n.find(word) != std::wstring::npos) return false;
    // Documents and links that Start Menu folders often contain.
    for (const wchar_t* ext : {L".url", L".htm", L".html", L".chm", L".pdf", L".txt", L".rtf", L".ini", L".log", L".md"})
        if (EndsWith(p, ext)) return false;
    if (p.rfind(L"http:", 0) == 0 || p.rfind(L"https:", 0) == 0) return false;
    return true;
}

bool NameLess(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                                  static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (r == 0) return _wcsicmp(a.c_str(), b.c_str()) < 0;
    return r == CSTR_LESS_THAN;
}

bool MatchesQuery(const std::wstring& name, const std::wstring& query) {
    if (query.empty()) return true;
    const std::wstring n = Lower(name), q = Lower(query);
    return n.find(q) != std::wstring::npos;
}

std::wstring ExtraAppsDir() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::wstring(local) + L"\\LiquidDock\\Launchpad Apps";
}

bool DiscoverApps(std::vector<AppEntry>& out, std::wstring& error) {
    out.clear();
    ComPtr<IShellItem> folder;
    HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr)) {
        error = L"Applications folder unavailable";
        return false;
    }
    ComPtr<IEnumShellItems> items;
    hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items));
    if (FAILED(hr)) {
        error = L"Applications folder cannot be enumerated";
        return false;
    }
    std::set<std::string> seen;
    for (;;) {
        ComPtr<IShellItem> item;
        if (items->Next(1, &item, nullptr) != S_OK) break;
        PWSTR display = nullptr, parsing = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display))) continue;
        if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing))) {
            CoTaskMemFree(display);
            continue;
        }
        AppEntry e;
        e.name = display;
        e.parsingName = parsing;
        CoTaskMemFree(display);
        CoTaskMemFree(parsing);
        if (!IsLaunchableEntry(e.name, e.parsingName)) continue;
        e.id = "app:" + Utf8(Lower(e.parsingName));
        if (!seen.insert(e.id).second) continue;
        out.push_back(std::move(e));
    }
    // Extra shortcuts (ExtraAppsDir): listed by file name, launched by path. A name already in the Start menu wins.
    std::set<std::wstring> names;
    for (const auto& e : out) names.insert(Lower(e.name));
    const std::wstring extra = ExtraAppsDir();
    WIN32_FIND_DATAW fd{};
    HANDLE find = extra.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((extra + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring file = fd.cFileName, lower = Lower(file);
            const size_t dot = file.find_last_of(L'.');
            if (dot == std::wstring::npos || (lower.compare(dot, std::wstring::npos, L".lnk") != 0 && lower.compare(dot, std::wstring::npos, L".url") != 0)) continue;
            AppEntry e;
            e.name = file.substr(0, dot);
            e.parsingName = extra + L"\\" + file;
            e.extraFile = true;
            e.id = "app:" + Utf8(Lower(e.parsingName));
            if (!names.insert(Lower(e.name)).second || !seen.insert(e.id).second) continue;
            out.push_back(std::move(e));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::stable_sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) { return NameLess(a.name, b.name); });
    return true;
}

}  // namespace dock::apps
