#include "update_client.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <vector>

#include "docky_version.h"
#include "json_lite.h"

namespace docky::update {
namespace json = dock::json;
namespace {

constexpr uint64_t kMaxPackage = 512ull * 1024 * 1024;  // refuse anything larger than this as a download
constexpr uint64_t kMaxApiBody = 4ull * 1024 * 1024;

struct Inet {
    HINTERNET h = nullptr;
    Inet() = default;
    explicit Inet(HINTERNET v) : h(v) {}
    Inet(const Inet&) = delete;
    Inet& operator=(const Inet&) = delete;
    ~Inet() { if (h) WinHttpCloseHandle(h); }
};

std::wstring Win32Message(DWORD code) {
    wchar_t buf[512]{};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    if (!n) n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, GetModuleHandleW(L"winhttp.dll"), code, 0, buf, 512, nullptr);
    std::wstring s(buf, n);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.empty()) s = L"error " + std::to_wstring(code);
    return s;
}

std::wstring UserAgent() { return L"docky-updater/" + json::FromUtf8(DOCKY_VERSION); }

// GET over HTTPS. The body goes to *body (bounded by maxBody) or, when sink is set, to the sink in chunks.
// Redirects (GitHub's asset downloads) are followed by WinHTTP, HTTPS to HTTPS only.
bool HttpGet(const std::wstring& url, const wchar_t* headers, std::string* body, uint64_t maxBody,
             const std::function<bool(const char*, DWORD)>& sink, const std::function<void(uint64_t, uint64_t)>& progress,
             const std::atomic<bool>* cancel, DWORD& status, std::wstring& error) {
    status = 0;
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256]{}, path[4096]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    wchar_t extra[4096]{};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) { error = L"Invalid update address."; return false; }
    Inet session(WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) session.h = WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { error = Win32Message(GetLastError()); return false; }
    WinHttpSetTimeouts(session.h, 10000, 15000, 20000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Inet connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) { error = Win32Message(GetLastError()); return false; }
    const std::wstring object = std::wstring(path) + extra;
    Inet request(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) { error = Win32Message(GetLastError()); return false; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(request.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        const DWORD e = GetLastError();
        error = (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT) ? L"No connection to GitHub." :
                e == ERROR_WINHTTP_TIMEOUT ? L"GitHub did not respond in time." : Win32Message(e);
        return false;
    }
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    uint64_t total = 0;
    {
        wchar_t len[32]{}; DWORD bytes = sizeof(len);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &bytes, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(len, nullptr, 10);
    }
    if (status != 200) return true;  // the caller interprets the status; no body is read
    if (total > maxBody) { error = L"The download is larger than expected."; return false; }
    std::vector<char> buf(64 * 1024);
    uint64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) { error = L"Cancelled."; return false; }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) { error = Win32Message(GetLastError()); return false; }
        if (!got) break;
        done += got;
        if (done > maxBody) { error = L"The download is larger than expected."; return false; }
        if (sink) { if (!sink(buf.data(), got)) { error = L"Could not write the download."; return false; } }
        else if (body) body->append(buf.data(), got);
        if (progress) progress(done, total);
    }
    if (total && done != total) { error = L"The download was incomplete."; return false; }
    return true;
}

std::wstring LocalAppData() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (n && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

const json::Value* Member(const json::Value& v, const char* key, json::Value::Type type) {
    const auto* m = v.Find(key);
    return (m && m->type == type) ? m : nullptr;
}

}  // namespace

const char* CurrentVersion() { return DOCKY_VERSION; }
std::wstring Repository() { return json::FromUtf8(DOCKY_UPDATE_REPO); }

int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s, std::vector<long long>& nums, std::string& suffix) {
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        while (i < s.size()) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) break;
            long long n = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { n = (std::min)(n * 10 + (s[i] - '0'), 1000000000LL); ++i; }
            nums.push_back(n);
            if (i < s.size() && s[i] == '.') ++i; else break;
        }
        suffix = s.substr(i);
        if (!suffix.empty() && (suffix[0] == '-' || suffix[0] == '+')) suffix.erase(0, 1);
        if (!suffix.empty() && s[i] == '+') suffix.clear();  // build metadata does not order
    };
    std::vector<long long> na, nb; std::string sa, sb;
    split(a, na, sa); split(b, nb, sb);
    for (size_t i = 0; i < (std::max)(na.size(), nb.size()); ++i) {
        const long long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;  // 1.0.0 > 1.0.0-beta
    return sa < sb ? -1 : sa > sb ? 1 : 0;
}

CheckResult Check() {
    CheckResult r;
    const std::wstring url = L"https://api.github.com/repos/" + Repository() + L"/releases/latest";
    std::string body; DWORD status = 0;
    if (!HttpGet(url, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", &body, kMaxApiBody, nullptr, nullptr, nullptr, status, r.error))
        return r;
    if (status == 404) { r.status = CheckStatus::NoRelease; return r; }
    if (status == 403 || status == 429) { r.error = L"GitHub limits how often updates can be checked. Try again later."; return r; }
    if (status != 200) { r.error = L"GitHub answered with status " + std::to_wstring(status) + L"."; return r; }
    json::Value root; std::string perr;
    if (!json::Parse(body, root, perr) || !root.IsObject()) { r.error = L"GitHub's answer could not be read."; return r; }
    const auto* tag = Member(root, "tag_name", json::Value::Type::String);
    if (!tag || tag->string.empty()) { r.error = L"The latest release has no version tag."; return r; }
    Release rel;
    rel.version = tag->string;
    if (rel.version[0] == 'v' || rel.version[0] == 'V') rel.version.erase(0, 1);
    if (const auto* page = Member(root, "html_url", json::Value::Type::String)) rel.pageUrl = page->string;
    if (CompareVersions(rel.version, CurrentVersion()) <= 0) { r.status = CheckStatus::UpToDate; r.release = rel; return r; }
    const std::string prefix = Lower(DOCKY_UPDATE_ASSET);
    if (const auto* assets = Member(root, "assets", json::Value::Type::Array)) {
        for (const auto& a : assets->array) {
            if (!a.IsObject()) continue;
            const auto* name = Member(a, "name", json::Value::Type::String);
            const auto* dl = Member(a, "browser_download_url", json::Value::Type::String);
            if (!name || !dl) continue;
            const std::string n = Lower(name->string);
            if (n.rfind(prefix, 0) != 0 || n.size() < 4 || n.compare(n.size() - 4, 4, ".zip") != 0) continue;
            rel.assetName = name->string;
            rel.assetUrl = dl->string;
            if (const auto* sz = Member(a, "size", json::Value::Type::Number)) rel.size = static_cast<uint64_t>(sz->number);
            if (const auto* dg = Member(a, "digest", json::Value::Type::String)) {
                const std::string d = Lower(dg->string);
                if (d.rfind("sha256:", 0) == 0 && d.size() == 7 + 64) rel.sha256 = d.substr(7);
            }
            break;
        }
    }
    if (rel.assetUrl.empty()) { r.error = L"DockOver " + json::FromUtf8(rel.version) + L" has no Windows package attached yet."; return r; }
    r.status = CheckStatus::Available;
    r.release = rel;
    return r;
}

bool Download(const Release& rel, const std::wstring& zipPath, const std::function<void(uint64_t, uint64_t)>& progress,
              const std::atomic<bool>* cancel, std::wstring& error) {
    if (rel.sha256.size() != 64) { error = L"GitHub published no checksum for this package, so it cannot be verified."; return false; }
    if (rel.size > kMaxPackage) { error = L"The package is larger than expected."; return false; }
    const std::wstring part = zipPath + L".part";
    HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Could not create " + part + L": " + Win32Message(GetLastError()); return false; }
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    if (!ok) error = L"SHA-256 is unavailable.";
    uint64_t written = 0;
    DWORD status = 0;
    if (ok) {
        ok = HttpGet(json::FromUtf8(rel.assetUrl), nullptr, nullptr, kMaxPackage,
                     [&](const char* data, DWORD n) {
                         DWORD w = 0;
                         if (!WriteFile(file, data, n, &w, nullptr) || w != n) return false;
                         written += n;
                         return BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), n, 0));
                     },
                     [&](uint64_t done, uint64_t total) { if (progress) progress(done, total ? total : rel.size); }, cancel, status, error);
        if (ok && status != 200) { ok = false; error = L"The download failed (status " + std::to_wstring(status) + L")."; }
    }
    std::string hex;
    if (ok) {
        unsigned char digest[32]{};
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0));
        for (unsigned char c : digest) { char b[3]; std::snprintf(b, 3, "%02x", c); hex += b; }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!FlushFileBuffers(file)) ok = false;
    CloseHandle(file);
    if (ok && rel.size && written != rel.size) { ok = false; error = L"The download has the wrong size."; }
    if (ok && hex != rel.sha256) { ok = false; error = L"The download does not match its published checksum and was discarded."; }
    if (ok && !MoveFileExW(part.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false; error = L"Could not save the download: " + Win32Message(GetLastError());
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

std::wstring ProgramDir() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring WorkDir() {
    const std::wstring base = LocalAppData();
    if (base.empty()) return {};
    const std::wstring root = base + L"\\LiquidDesktop", dir = root + L"\\updates";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool StartInstall(const std::wstring& zipPath, const std::string& version, bool reopenSettings, bool waitForCaller, std::wstring& error) {
    const std::wstring program = ProgramDir(), work = WorkDir();
    if (program.empty() || work.empty()) { error = L"The program or update folder is unknown."; return false; }
    const std::wstring source = program + L"\\docky-updater.exe";
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) { error = L"docky-updater.exe is missing from " + program + L"."; return false; }
    {  // copies left by earlier updates (an exe cannot delete itself)
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((work + L"\\run-*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::wstring d = work + L"\\" + fd.cFileName;
                if (DeleteFileW((d + L"\\docky-updater.exe").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) RemoveDirectoryW(d.c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    // Run a copy: the installed docky-updater.exe is one of the files being replaced.
    const std::wstring runDir = work + L"\\run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(runDir.c_str(), nullptr)) { error = L"Could not prepare the updater: " + Win32Message(GetLastError()); return false; }
    const std::wstring exe = runDir + L"\\docky-updater.exe";
    if (!CopyFileW(source.c_str(), exe.c_str(), TRUE)) { error = L"Could not prepare the updater: " + Win32Message(GetLastError()); return false; }
    // A release folder: the programs are in runtime\ and DockOver.exe is beside it; the update replaces the whole folder.
    std::wstring installDir = program;
    {
        const size_t slash = program.find_last_of(L"\\/");
        if (slash != std::wstring::npos && _wcsicmp(program.c_str() + slash + 1, L"runtime") == 0 &&
            GetFileAttributesW((program.substr(0, slash) + L"\\DockOver.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
            installDir = program.substr(0, slash);
    }
    std::wstring cmd = L"\"" + exe + L"\" --zip \"" + zipPath + L"\" --dir \"" + installDir + L"\" --version \"" + json::FromUtf8(version) + L"\"";
    if (waitForCaller) cmd += L" --wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (reopenSettings) cmd += L" --reopen-settings";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, runDir.c_str(), &si, &pi)) {
        error = L"Could not start the updater: " + Win32Message(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int64_t UnixNow() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(t / 10000000ull) - 11644473600LL;
}

State LoadState() {
    State s;
    const std::wstring dir = WorkDir();
    if (dir.empty()) return s;
    HANDLE f = CreateFileW((dir + L"\\state.json").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    std::string text(64 * 1024, '\0'); DWORD got = 0;
    ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    json::Value root; std::string err;
    if (!json::Parse(text, root, err) || !root.IsObject()) return s;
    if (const auto* v = Member(root, "lastCheck", json::Value::Type::String)) s.lastCheck = _strtoi64(v->string.c_str(), nullptr, 10);  // a string: the JSON writer rounds large numbers
    if (const auto* v = Member(root, "latest", json::Value::Type::String)) s.latest = v->string;
    if (const auto* v = Member(root, "notified", json::Value::Type::String)) s.notified = v->string;
    return s;
}

void SaveState(const State& s) {
    const std::wstring dir = WorkDir();
    if (dir.empty()) return;
    auto root = json::Value::MakeObject();
    root.Set("lastCheck", json::Value::MakeString(std::to_string(s.lastCheck)));
    root.Set("latest", json::Value::MakeString(s.latest));
    root.Set("notified", json::Value::MakeString(s.notified));
    const std::string text = json::Write(root);
    const std::wstring path = dir + L"\\state.json", temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

}  // namespace docky::update
