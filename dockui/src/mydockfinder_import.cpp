#include "mydockfinder_import.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

#include "app_registry.h"
#include "launchpad_model.h"

using Microsoft::WRL::ComPtr;

namespace docky::mdf {
namespace {

std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}
bool ReadBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; GetFileSizeEx(f, &size);
    out.resize(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 16 << 20)));
    DWORD read = 0; ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr); CloseHandle(f);
    out.resize(read);
    return true;
}
std::wstring Decode(const std::string& b) {  // UTF-16 LE (BOM) or UTF-8 (optional BOM)
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
    const size_t start = b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const int n = MultiByteToWideChar(CP_UTF8, 0, b.data() + start, static_cast<int>(b.size() - start), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, b.data() + start, static_cast<int>(b.size() - start), w.data(), n);
    return w;
}
std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Stamp() {
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t b[32]{};
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
// Copies an existing file aside; returns the backup path (empty when there was nothing to back up).
bool Backup(const std::wstring& path, std::wstring& backup) {
    backup.clear();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    backup = path + L".before-mydockfinder-" + Stamp();
    return CopyFileW(path.c_str(), backup.c_str(), TRUE) != FALSE;
}
bool WriteAtomic(const std::wstring& path, const std::string& bytes) {
    const std::wstring tmp = path + L".import.tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

// ---- Launchpad --------------------------------------------------------------------------------------------------
struct PadItem { std::wstring path, name; };
std::wstring XmlUnescape(std::wstring s) {
    const std::pair<const wchar_t*, const wchar_t*> map[] = {{L"&quot;", L"\""}, {L"&apos;", L"'"}, {L"&lt;", L"<"}, {L"&gt;", L">"}, {L"&amp;", L"&"}};
    for (const auto& [from, to] : map) for (size_t p; (p = s.find(from)) != std::wstring::npos;) s.replace(p, wcslen(from), to);
    return s;
}
std::vector<PadItem> ParsePad(const std::wstring& xml) {
    std::vector<PadItem> out;
    size_t pos = 0;
    while ((pos = xml.find(L"<ico", pos)) != std::wstring::npos) {
        const size_t close = xml.find(L'>', pos);
        if (close == std::wstring::npos) break;
        const std::wstring tag = xml.substr(pos, close - pos);
        const size_t p = tag.find(L"path=\"");
        const size_t end = xml.find(L"</ico", close);
        if (p != std::wstring::npos && end != std::wstring::npos) {
            const size_t q = tag.find(L'"', p + 6);
            PadItem item; item.path = XmlUnescape(tag.substr(p + 6, q - p - 6)); item.name = Trim(XmlUnescape(xml.substr(close + 1, end - close - 1)));
            if (!item.path.empty()) out.push_back(std::move(item));
        }
        pos = close + 1;
    }
    return out;
}
// Candidate docky ids for a MyDockFinder shortcut: a .url's URL, a .lnk's target path or AppUserModelID.
std::vector<std::string> Candidates(const std::wstring& path) {
    std::vector<std::string> ids;
    const std::wstring lower = Lower(path);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".url") == 0) {
        std::string bytes; if (!ReadBytes(path, bytes)) return ids;
        const std::wstring text = Decode(bytes);
        const size_t u = text.find(L"URL=");
        if (u != std::wstring::npos) { const size_t e = text.find_first_of(L"\r\n", u); ids.push_back("app:" + Utf8(Lower(Trim(text.substr(u + 4, e - u - 4))))); }
        return ids;
    }
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return ids;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) return ids;
    wchar_t target[MAX_PATH * 4]{};
    if (SUCCEEDED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH)) && target[0]) {
        wchar_t expanded[MAX_PATH * 4]{};
        ExpandEnvironmentStringsW(target, expanded, ARRAYSIZE(expanded));
        ids.push_back("app:" + Utf8(Lower(expanded)));
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(link.As(&props))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) ids.push_back("app:" + Utf8(Lower(v.pwszVal)));
        PropVariantClear(&v);
    }
    return ids;
}

}  // namespace

std::wstring FindInstall() {
    wchar_t pf[MAX_PATH]{};
    for (const wchar_t* var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        if (!GetEnvironmentVariableW(var, pf, MAX_PATH)) continue;
        const std::wstring dir = std::wstring(pf) + L"\\Steam\\steamapps\\common\\MyDockFinder";
        if (GetFileAttributesW((dir + L"\\ico.ini").c_str()) != INVALID_FILE_ATTRIBUTES) return dir;
    }
    return {};
}

std::vector<std::wstring> DockLinesFromIcoIni(const std::wstring& text, std::vector<std::wstring>& skipped) {
    // Sections in numeric order.
    std::map<int, std::map<std::wstring, std::wstring>> sections;
    int current = -1;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos); if (end == std::wstring::npos) end = text.size();
        const std::wstring line = Trim(text.substr(pos, end - pos)); pos = end + 1;
        if (line.size() > 5 && line.front() == L'[' && line.back() == L']' && Lower(line.substr(1, 3)) == L"ico") { current = _wtoi(line.c_str() + 4); sections[current]; continue; }
        const size_t eq = line.find(L'=');
        if (current >= 0 && eq != std::wstring::npos) sections[current][Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    std::vector<std::wstring> lines;
    for (auto& [index, kv] : sections) {
        (void)index;
        const std::wstring app = Lower(kv[L"appname"]), tag = kv[L"tag"], file = kv[L"filepath"];
        if (kv.count(L"delimiter")) { lines.push_back(L"---"); continue; }
        if (app == L"launchpad|") { lines.push_back(L"# liquid-desktop:launchpad:v1"); continue; }
        if (app == L"downloads" || app == L"trash|") continue;  // docky always shows Downloads and the Recycle Bin itself
        if (app == L"controlpanel") { lines.push_back(L"shell:AppsFolder\\Microsoft.Windows.ControlPanel |  | " + (tag.empty() ? std::wstring(L"Control Panel") : tag)); continue; }
        if (file.empty()) { skipped.push_back(tag.empty() ? app : tag); continue; }  // clock, calendar and other widgets
        const std::wstring target = kv[L"uwp"] == L"1" ? L"shell:AppsFolder\\" + file : file;
        lines.push_back(tag.empty() ? target : target + L" |  | " + tag);
    }
    return lines;
}

namespace {

// Copies a folder tree (backups). Returns false when any file could not be copied.
bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!CreateDirectoryW(to.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring a = from + L"\\" + name, b = to + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = CopyTree(a, b) && ok;
        else ok = CopyFileW(a.c_str(), b.c_str(), TRUE) && ok;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}
// Deletes the files directly inside a folder (the Launchpad Apps shortcuts; it has no subfolders).
void ClearFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}
// Replace everything: one folder with docky's Dock list, Launchpad layout, settings, glass, custom icons and the
// Launchpad Apps shortcuts as they were (logs and preview builds are left out).
bool BackupAll(const std::wstring& dataDir, ImportResult& r) {
    const std::wstring root = dataDir + L"\\Backups";
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring folder = root + L"\\before-mydockfinder-" + Stamp();
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return false;
    for (const wchar_t* f : {L"pinned.txt", L"launchpad.json", L"custom-icons.json", L"glass.json", L"preview-glass.json"}) {
        const std::wstring a = dataDir + L"\\" + f;
        if (GetFileAttributesW(a.c_str()) != INVALID_FILE_ATTRIBUTES && !CopyFileW(a.c_str(), (folder + L"\\" + f).c_str(), TRUE)) return false;
    }
    for (const wchar_t* d : {L"Launchpad Apps", L"personal-icons"}) {
        const std::wstring a = dataDir + L"\\" + d;
        if (GetFileAttributesW(a.c_str()) != INVALID_FILE_ATTRIBUTES && !CopyTree(a, folder + L"\\" + d)) return false;
    }
    // Shared docky settings (Dock, Launchpad, Control Center options) live beside LiquidDock.
    const std::wstring settings = dataDir.substr(0, dataDir.find_last_of(L"\\/")) + L"\\LiquidDesktop\\settings.json";
    if (GetFileAttributesW(settings.c_str()) != INVALID_FILE_ATTRIBUTES && !CopyFileW(settings.c_str(), (folder + L"\\settings.json").c_str(), TRUE)) return false;
    r.backupFolder = folder;
    return true;
}

}  // namespace

bool Import(const std::wstring& mdfDir, const std::wstring& dataDir, bool replace, ImportResult& r) {
    // Dock
    std::string bytes;
    if (!ReadBytes(mdfDir + L"\\ico.ini", bytes)) { r.error = L"MyDockFinder's ico.ini could not be read."; return false; }
    const auto lines = DockLinesFromIcoIni(Decode(bytes), r.dockSkipped);
    if (lines.empty()) { r.error = L"MyDockFinder's Dock list is empty; nothing was changed."; return false; }
    std::wstring dockText =
        L"# DockOver pinned items. One per line: target | arguments | label (arguments and label optional).\r\n"
        L"# Imported from MyDockFinder.\r\n";
    for (const auto& l : lines) dockText += l + L"\r\n";
    const std::wstring pinned = dataDir + L"\\pinned.txt";
    if (replace) {
        if (!BackupAll(dataDir, r)) { r.error = L"DockOver's current items could not be backed up; nothing was changed."; return false; }
    } else if (!Backup(pinned, r.dockBackup)) {
        r.error = L"The current Dock list could not be backed up; nothing was changed."; return false;
    }
    if (!WriteAtomic(pinned, Utf8(dockText))) { r.error = L"The Dock list could not be written."; return false; }
    r.dockItems = static_cast<int>(lines.size());

    // Launchpad: MyDockFinder's order first; docky appends the remaining applications when Launchpad opens, unless
    // everything is replaced, in which case the remaining applications are hidden.
    std::string xmlBytes;
    const bool havePad = ReadBytes(mdfDir + L"\\padconfig.xml", xmlBytes);
    const std::wstring extra = dock::apps::ExtraAppsDir();
    if (replace && !extra.empty()) ClearFiles(extra);  // backed up above; only MyDockFinder's shortcuts are copied back
    if (!havePad) return true;  // no Launchpad data: Dock import alone
    std::vector<dock::apps::AppEntry> apps; std::wstring error;
    if (!dock::apps::DiscoverApps(apps, error)) { r.error = L"Applications could not be listed: " + error; return false; }
    std::set<std::string> ids; std::map<std::wstring, std::string> byName;
    for (const auto& a : apps) { ids.insert(a.id); byName.emplace(Lower(a.name), a.id); }
    dock::launchpad::Layout layout; std::set<std::string> used;
    for (const auto& item : ParsePad(Decode(xmlBytes))) {
        std::string id;
        for (const auto& c : Candidates(item.path)) if (ids.count(c)) { id = c; break; }
        if (id.empty()) { auto f = byName.find(Lower(item.name)); if (f != byName.end()) id = f->second; }
        if (id.empty()) {
            // No Start-menu entry (Steam games, portable launchers): copy the shortcut into docky's own Launchpad Apps
            // folder, which Launchpad also lists, and keep its place in MyDockFinder's order.
            const size_t dot = item.path.find_last_of(L'.');
            const size_t slash = item.path.find_last_of(L"\\/");
            std::wstring name = item.name.empty() ? item.path.substr(slash + 1, dot - slash - 1) : item.name;
            for (auto& ch : name) if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
            if (!extra.empty() && dot != std::wstring::npos) {
                SHCreateDirectoryExW(nullptr, extra.c_str(), nullptr);
                const std::wstring copy = extra + L"\\" + name + item.path.substr(dot);
                if (CopyFileW(item.path.c_str(), copy.c_str(), FALSE) && byName.find(Lower(name)) == byName.end()) {
                    id = "app:" + Utf8(Lower(copy)); byName.emplace(Lower(name), id); ++r.launchpadCopied;
                }
            }
        }
        if (id.empty() || !used.insert(id).second) { if (id.empty()) r.launchpadUnmatched.push_back(item.name); continue; }
        dock::launchpad::Node n; n.appId = id; layout.nodes.push_back(std::move(n));
    }
    if (layout.nodes.empty()) return true;
    if (replace)
        for (const auto& a : apps)
            if (!a.extraFile && !used.count(a.id)) layout.hidden.push_back(a.id);
    r.hidden = static_cast<int>(layout.hidden.size());
    const std::wstring lp = dataDir + L"\\launchpad.json";
    if (!replace && !Backup(lp, r.launchpadBackup)) { r.error = L"The current Launchpad order could not be backed up; it was not changed."; return false; }
    if (!dock::launchpad::Save(lp, layout, error)) { r.error = L"The Launchpad order could not be written: " + error; return false; }
    r.launchpadMatched = static_cast<int>(layout.nodes.size());
    return true;
}

}  // namespace docky::mdf
