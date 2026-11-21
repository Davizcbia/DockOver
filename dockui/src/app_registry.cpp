#include "app_registry.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <filesystem>
#include <cstdint>
#include <sstream>
#include <iomanip>

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

namespace {
bool IsStartApplication(IShellItem* item,const AppEntry& entry){
    SFGAOF attributes=0;
    if(SUCCEEDED(item->GetAttributes(SFGAO_FOLDER,&attributes))&&(attributes&SFGAO_FOLDER))return false;
    const auto name=Lower(entry.name);
    for(const auto* suffix:{L" autostart",L" help",L" user guide",L" documentation",L" readme",L" release notes",L" keyboard shortcuts"})
        if(EndsWith(name,suffix))return false;
    for(const auto* prefix:{L"add to archive",L".net runtime completion",L"read me",L"readme"})
        if(name.rfind(prefix,0)==0)return false;
    ComPtr<IShellItem2> typed;
    PWSTR link=nullptr;
    if(FAILED(item->QueryInterface(IID_PPV_ARGS(&typed)))||FAILED(typed->GetString(PKEY_Link_TargetParsingPath,&link)))return true;
    const std::wstring target=Lower(link?link:L"");CoTaskMemFree(link);
    if(target.empty())return true;
    if(!IsLaunchableEntry(entry.name,target))return false;
    const auto fileStart=target.find_last_of(L"\\/");
    const auto file=target.substr(fileStart==std::wstring::npos?0:fileStart+1);
    for(const auto* executable:{L"helpviewer.exe",L"hh.exe",L"msiexec.exe",L"uninstall.exe",L"uninst.exe"})
        if(file==executable)return false;
    if(file.rfind(L"unins",0)==0&&EndsWith(file,L".exe"))return false;
    if((target.find(L"\\help\\")!=std::wstring::npos||target.find(L"\\docs\\")!=std::wstring::npos)
       &&(name.find(L" help")!=std::wstring::npos||name.find(L" documentation")!=std::wstring::npos))return false;
    for(const auto* suffix:{L".bat",L".cmd",L".ps1",L".vbs",L".msi",L".doc",L".docx",L".xls",L".xlsx",L".lnk",L".reg"})
        if(EndsWith(target,suffix))return false;
    return true;
}

bool DiscoverShellApps(std::vector<AppEntry>& out,std::wstring& error,bool startOnly) {
    out.clear();error.clear();
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
        if (!IsLaunchableEntry(e.name, e.parsingName)||(startOnly&&!IsStartApplication(item.Get(),e))) continue;
        e.id = "app:" + Utf8(Lower(e.parsingName));
        if (!seen.insert(e.id).second) continue;
        out.push_back(std::move(e));
    }
    return true;
}
void SortApps(std::vector<AppEntry>& out){
    std::stable_sort(out.begin(),out.end(),[](const AppEntry& a,const AppEntry& b){return NameLess(a.name,b.name);});
}
}  // namespace

bool DiscoverApps(std::vector<AppEntry>& out,std::wstring& error){
    if(!DiscoverShellApps(out,error,false))return false;
    std::set<std::string> seen;
    for(const auto& entry:out)seen.insert(entry.id);
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
            if(const auto marker=e.name.rfind(L"__dockover_");marker!=std::wstring::npos)e.name.resize(marker);
            e.parsingName = extra + L"\\" + file;
            e.extraFile = true;
            e.id = "app:" + Utf8(Lower(e.parsingName));
            if (names.count(Lower(e.name)) || !seen.insert(e.id).second) continue;
            out.push_back(std::move(e));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::stable_sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) { return NameLess(a.name, b.name); });
    return true;
}

bool DiscoverStartMenuApps(std::vector<AppEntry>& out,std::wstring& error){
    if(!DiscoverShellApps(out,error,true))return false;
    SortApps(out);
    return true;
}

int AddFilesToLaunchpad(const std::vector<std::wstring>& paths,const std::wstring& directory,std::wstring& error){
    error.clear();if(directory.empty()){error=L"Launchpad's application directory is unavailable.";return 0;}
    const int created=SHCreateDirectoryExW(nullptr,directory.c_str(),nullptr);
    if(created!=ERROR_SUCCESS&&created!=ERROR_ALREADY_EXISTS&&created!=ERROR_FILE_EXISTS){error=L"Could not create Launchpad's application directory.";return 0;}
    int added=0;
    for(const auto& source:paths){
        const auto attributes=GetFileAttributesW(source.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY)){error=L"Some dropped files are missing or are folders.";continue;}
        std::error_code ec;const auto absolute=std::filesystem::absolute(source,ec).lexically_normal();if(ec){error=L"A dropped path could not be resolved.";continue;}
        const auto key=Lower(absolute.wstring());std::uint64_t hash=14695981039346656037ull;for(auto ch:key){hash^=static_cast<unsigned>(ch);hash*=1099511628211ull;}
        std::wostringstream suffix;suffix<<L"__dockover_"<<std::hex<<std::setw(16)<<std::setfill(L'0')<<hash;
        const auto extension=Lower(absolute.extension().wstring());const bool copy=extension==L".lnk"||extension==L".url";
        const std::wstring destination=directory+L"\\"+absolute.stem().wstring()+suffix.str()+(copy?extension:L".lnk");
        if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES)continue;
        if(copy){if(CopyFileW(absolute.c_str(),destination.c_str(),TRUE)){++added;continue;}if(GetLastError()!=ERROR_FILE_EXISTS)error=L"A shortcut could not be copied into Launchpad.";continue;}
        ComPtr<IShellLinkW> link;ComPtr<IPersistFile> persist;
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))||FAILED(link->SetPath(absolute.c_str()))||FAILED(link.As(&persist))){error=L"A launch shortcut could not be created.";continue;}
        link->SetWorkingDirectory(absolute.parent_path().c_str());
        HANDLE reservation=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(reservation==INVALID_HANDLE_VALUE)continue;CloseHandle(reservation);
        if(SUCCEEDED(persist->Save(destination.c_str(),TRUE)))++added;
        else{DeleteFileW(destination.c_str());error=L"A launch shortcut could not be saved.";}
    }
    return added;
}

}  // namespace dock::apps
