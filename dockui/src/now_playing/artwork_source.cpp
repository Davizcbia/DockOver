#include "artwork_source.h"

#include "artwork_match.h"
#include <string_view>
#include "hls.h"
#include "../json_lite.h"

#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <cctype>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <bit>
#include <cwctype>
#include <vector>

namespace nowplaying {
namespace {

struct Inet {
    HINTERNET h = nullptr;
    explicit Inet(HINTERNET handle) : h(handle) {}
    ~Inet() { if (h) WinHttpCloseHandle(h); }
};

std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size())); return s; }

// Traditional Chinese to Simplified (the Taiwan/Hong Kong stores use Traditional, NetEase/QQ Music Simplified).
std::wstring Simplified(const std::wstring& in) {
    if (in.empty() || std::none_of(in.begin(), in.end(), [](wchar_t c) { return c >= 0x3400 && c < 0xA000; })) return in;
    const int n = LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, in.c_str(), int(in.size()), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return in;
    std::wstring out(size_t(n), L'\0');
    return LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, in.c_str(), int(in.size()), out.data(), n, nullptr, nullptr, 0) > 0 ? out : in;
}

bool HasCjk(const std::wstring& s) { return std::any_of(s.begin(), s.end(), [](wchar_t c) { return (c >= 0x3040 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7B0); }); }

// Comparable form: lowercase, Simplified Chinese, bracketed qualifiers and " - Single"/" - EP" dropped,
// ASCII punctuation as spaces.
std::wstring Normalize(const std::wstring& in, bool stripBrackets = true) {
    std::wstring s = Simplified(Lower(in)), out;
    for (const wchar_t* suffix : {L" - single", L" - ep"}) {
        const size_t p = s.rfind(suffix); if (p != std::wstring::npos && p + wcslen(suffix) == s.size()) s.resize(p);
    }
    int depth = 0;
    for (wchar_t c : s) {
        const bool open = c == L'(' || c == L'[' || c == 0xFF08 || c == 0x3010;
        const bool close = c == L')' || c == L']' || c == 0xFF09 || c == 0x3011;
        if (stripBrackets && open) { ++depth; continue; }
        if (stripBrackets && close) { if (depth) --depth; continue; }
        if (depth) continue;
        const bool keep = !open && !close && (c >= 0x80 ? !std::iswspace(c) : std::iswalnum(c) != 0);
        if (keep) out += c; else if (!out.empty() && out.back() != L' ') out += L' ';
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// Words that make a different recording or arrangement. Normalize() drops bracketed text, so
// "A.F.B.1. (FIXION Remix)" and "A.F.B.1." compare equal by name; these must also be the same on both sides.
// Remaster/deluxe/explicit/version and similar packaging words are not counted.
std::vector<std::wstring> Qualifiers(const std::wstring& raw) {
    static const wchar_t* const words[] = {L"remix", L"remixed", L"rmx", L"mix", L"mixed", L"live", L"acoustic", L"instrumental", L"inst", L"vip",
        L"sped", L"slowed", L"nightcore", L"demo", L"karaoke", L"cover", L"rework", L"bootleg", L"flip", L"refix", L"remixes", L"extended", L"radio", L"edit",
        L"orchestral", L"reprise", L"unplugged", L"dub", L"remake", L"medley", L"mashup", L"piano", L"interlude", L"intro", L"outro",
        L"chopped", L"reverb", L"8d", L"tv", L"short"};
    static const wchar_t* const cjk[] = {L"现场", L"伴奏", L"纯音乐", L"重混", L"混音", L"翻唱", L"女声版", L"男声版", L"dj版", L"慢速", L"加速",
        L"降调", L"升调", L"钢琴版", L"吉他版", L"live版", L"演唱会", L"弹唱", L"合唱版", L"ライブ", L"リミックス", L"インスト"};
    std::wstring s = Simplified(Lower(raw));
    std::vector<std::wstring> out, tokens; std::wstring token;
    for (wchar_t c : s) {
        if (c < 0x80 && std::iswalnum(c)) token += c;
        else { if (!token.empty()) tokens.push_back(token); token.clear(); }
    }
    if (!token.empty()) tokens.push_back(token);
    for (const auto& t : tokens) for (const wchar_t* w : words) if (t == w) out.push_back(t);
    for (const wchar_t* w : cjk) if (s.find(w) != std::wstring::npos) out.push_back(w);
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// A version includes the full descriptor so two named remixes cannot match each other.
struct RecordingName {
    std::wstring base, version;
};

RecordingName ParseRecordingName(const std::wstring& raw) {
    size_t baseEnd = std::wstring::npos, start = 0, end = raw.size();
    if (!raw.empty() && std::wstring_view(L")]）】").find(raw.back()) != std::wstring_view::npos) {
        const size_t bracket = raw.find_last_of(L"([（【");
        if (bracket != std::wstring::npos && bracket > 0 && !Qualifiers(raw.substr(bracket + 1)).empty()) {
            baseEnd = bracket;
            start = bracket + 1;
            --end;
        }
    }
    if (baseEnd == std::wstring::npos) {
        for (const auto sep : {L" - ", L" – ", L" — "}) {
            const size_t dash = raw.rfind(sep);
            if (dash != std::wstring::npos && dash > 0 && !Qualifiers(raw.substr(dash + 3)).empty()) {
                baseEnd = dash;
                start = dash + 3;
                break;
            }
        }
    }
    if (baseEnd == std::wstring::npos)
        return {Normalize(raw), Qualifiers(raw).empty() ? L"" : Normalize(raw, false)};
    auto version = Normalize(raw.substr(start, end - start), false);
    for (const auto alias : {L" rmx", L" remixed"}) {
        const std::wstring_view word(alias);
        if (version.ends_with(word)) version.replace(version.size() - word.size(), word.size(), L" remix");
    }
    if (version == L"rmx" || version == L"remixed") version = L"remix";
    return {Normalize(raw.substr(0, baseEnd)), std::move(version)};
}

bool VersionMatches(const std::wstring& a, const std::wstring& b) {
    return ParseRecordingName(a).version == ParseRecordingName(b).version;
}

std::wstring PrimaryArtist(const std::wstring& normalized) {
    std::wstring s = normalized;
    for (const wchar_t* sep : {L" feat ", L" ft ", L" x ", L" & ", L" and "}) { const size_t p = s.find(sep); if (p != std::wstring::npos) s.resize(p); }
    return s;
}

bool Contains(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && a.find(b) != std::wstring::npos; }

// The Apple Music Windows app reports "Artist — Album" in the artist field.
ArtworkRequest Clean(ArtworkRequest r) {
    if (r.album.empty()) {
        const size_t p = r.artist.find(L" — ");
        if (p != std::wstring::npos) { r.album = r.artist.substr(p + 3); r.artist.resize(p); }
    }
    return r;
}

std::string UrlEncode(const std::wstring& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : dock::json::ToUtf8(text)) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string Storefront() {
    wchar_t geo[16]{};
    if (GetUserDefaultGeoName(geo, 16) == 3) { std::wstring g = Lower(geo); return dock::json::ToUtf8(g); }
    return "us";
}

std::string JsonString(const dock::json::Value& o, const char* key) {
    const auto* v = o.Find(key); return v && v->type == dock::json::Value::Type::String ? v->string : std::string{};
}

// art: cover URL template with {w}x{h} (and {f} for the format) for any size.
struct Candidate { std::string art, page; int score = 0; };

std::string ArtUrl(std::string t, int size) {
    const std::string n = std::to_string(size) + "x" + std::to_string(size);
    for (size_t at; (at = t.find("{w}x{h}")) != std::string::npos;) t.replace(at, 7, n);
    for (size_t at; (at = t.find("{f}")) != std::string::npos;) t.replace(at, 3, "jpg");
    return t;
}

// Score of a catalog result: the artist must match; then the album (album results) or the track name.
// anyArtist: a different artist spelling is allowed (the US store shows "JJ Lin" for 林俊杰); only used when the
// cover check decides, with a lower score.
// trackMs: the candidate's length (song results; 0 unknown). A remix, live take or other song with the same
// name scores 0: the version words must agree, and known lengths must be within 4 s.
int ResultScore(const ArtworkRequest& r, const std::wstring& artistIn, const std::wstring& name, const std::wstring& collection, bool album, bool anyArtist,
                int trackMs = 0) {
    const std::wstring wantArtist = Normalize(r.artist), wantPrimary = PrimaryArtist(wantArtist), artist = Normalize(artistIn);
    const bool artistOk = artist == wantArtist || Contains(artist, wantPrimary) || Contains(wantArtist, PrimaryArtist(artist));
    if (!artistOk && !anyArtist) return 0;
    const bool timed = !album && r.durationMs > 0 && trackMs > 0;
    const int durationDelta = timed ? std::abs(r.durationMs - trackMs) : 0;
    if (timed && durationDelta > 4000) return 0;
    if (!artistOk && !(timed && durationDelta <= 2000)) return 0;
    int score = 6;
    if (album) {
        if (r.album.empty() || Normalize(name, false) != Normalize(r.album, false)) return 0;
        // A versioned track on a generic release needs a track-level match.
        if (!r.title.empty() && !VersionMatches(r.title, L"") && !VersionMatches(r.title, r.album)) return 0;
    } else {
        const auto expected = ParseRecordingName(r.title);
        const auto actual = ParseRecordingName(name);
        if (expected.base.empty() || actual.base != expected.base || actual.version != expected.version) return 0;
        if (!collection.empty()) {
            const auto release = ParseRecordingName(collection);
            if (!r.album.empty()) {
                if (!VersionMatches(r.album, collection)) return 0;
            } else if (!release.version.empty() && release.version != expected.version) {
                return 0;
            }
        }
        if (!r.album.empty() && Normalize(collection, false) == Normalize(r.album, false)) score += 3;
    }
    if (score && artist == wantArtist) score += 2;
    if (score && timed && std::abs(r.durationMs - trackMs) <= 1500) score += 2;
    if (score && !artistOk) score = std::max(1, score - 4);
    return score;
}

void AddCandidate(std::vector<Candidate>& out, const std::string& art, std::string page, int score) {
    if (!score || art.empty() || page.empty()) return;
    page = page.substr(0, page.find('?'));
    auto same = std::find_if(out.begin(), out.end(), [&](const Candidate& c) { return c.page == page; });
    if (same != out.end()) same->score = std::max(same->score, score);
    else out.push_back({art, page, score});
}

// Fallback for releases the iTunes Search index does not have yet: the public Apple Music web search page.
void WebSearch(const ArtworkRequest& r, const std::string& country, const std::wstring& term, bool anyArtist, std::vector<Candidate>& out) {
    std::string html;
    std::wstring t = term; for (auto& c : t) if (c == L'+') c = L' ';
    if (!HttpGet("https://music.apple.com/" + country + "/search?term=" + UrlEncode(t), html, 0, 0, 8u * 1024 * 1024)) return;
    for (const auto& item : hls::ParseWebSearch(html)) {
        // Song cards carry no verified collection or duration in the current parser.
        if (item.kind != "albums" || r.album.empty()) continue;
        const int score = match::CatalogScore(r, dock::json::FromUtf8(item.artist), dock::json::FromUtf8(item.title), L"", true, anyArtist);
        AddCandidate(out, item.artwork, item.url, score);
    }
}

// Catalog results that plausibly are this release: the artist matches, and the album (album search) or the
// track (song search) name matches. A song result on the requested release scores higher.
void Search(const ArtworkRequest& r, const std::string& country, bool album, const std::wstring& term, bool anyArtist, std::vector<Candidate>& out) {
    const std::string url = "https://itunes.apple.com/search?media=music&limit=20&entity=" + std::string(album ? "album" : "song") +
                            "&country=" + country + "&term=" + UrlEncode(term);
    std::string body, error;
    if (!HttpGet(url, body, 0, 0, 2u * 1024 * 1024)) return;
    dock::json::Value root;
    if (!dock::json::Parse(body, root, error)) { Log(L"search: bad json"); return; }
    const auto* results = root.Find("results");
    if (!results || results->type != dock::json::Value::Type::Array) return;
    for (const auto& item : results->array) {
        const std::wstring collection = dock::json::FromUtf8(JsonString(item, "collectionName"));
        const auto* ms = item.Find("trackTimeMillis");
        const int trackMs = ms && ms->type == dock::json::Value::Type::Number ? int(ms->number) : 0;
        const int score = match::CatalogScore(r, dock::json::FromUtf8(JsonString(item, "artistName")),
                                      album ? collection : dock::json::FromUtf8(JsonString(item, "trackName")), collection, album, anyArtist, trackMs);
        std::string art = JsonString(item, "artworkUrl100");
        const size_t size = art.find("100x100bb"); if (size == std::string::npos) continue;
        art.replace(size, 7, "{w}x{h}");
        AddCandidate(out, art, JsonString(item, "collectionViewUrl"), score);
    }
}

// dHash of a catalog cover (small size is enough and cheap). 0 when it cannot be read.
uint64_t CoverHash(const std::string& url) {
    std::string jpg;
    if (!HttpGet(ArtUrl(url, 200), jpg, 0, 0, 1024 * 1024)) return 0;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic; Microsoft::WRL::ComPtr<IWICStream> stream; Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame; Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(jpg.data()), DWORD(jpg.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &frame)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096) return 0;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) return 0;
    return match::DHash(px.data(), w, h, w * 4);
}

bool WriteFileAll(const std::wstring& path, const std::string& data) {
    const std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != data.size()) { DeleteFileW(part.c_str()); return false; }
    return MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

std::string ReadFileAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string out; LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart < 1024 * 1024) {
        out.resize(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
        if (!ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) read = 0;
        out.resize(read);
    }
    CloseHandle(f);
    return out;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Marker written when an album has no motion artwork; looked up again after three days.
bool FreshMarker(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    const auto t = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return t(now) - t(a.ftLastWriteTime) < 3ull * 24 * 3600 * 10000000ull;
}

// Motion artwork is ~10-25 MB per album: keep the newest 1 GB.
void PruneVideos(const std::wstring& dir, const std::wstring& keep) {
    struct Item { std::wstring path; uint64_t size, time; };
    std::vector<Item> items; uint64_t total = 0;
    WIN32_FIND_DATAW f{}; HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const uint64_t size = (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow, time = (uint64_t(f.ftLastWriteTime.dwHighDateTime) << 32) | f.ftLastWriteTime.dwLowDateTime;
        items.push_back({dir + L"\\" + f.cFileName, size, time}); total += size;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.time < b.time; });
    for (const auto& i : items) {
        if (total <= 1024ull * 1024 * 1024) break;
        if (i.path == keep) continue;
        if (DeleteFileW(i.path.c_str())) total -= i.size;
    }
}

}  // namespace

ArtworkRequest CleanRequest(const ArtworkRequest& r) { return Clean(r); }
std::wstring NormalizeName(const std::wstring& s) { return Normalize(s); }
std::string UrlEncodeQuery(const std::wstring& s) { return UrlEncode(s); }
bool SameVersion(const std::wstring& a, const std::wstring& b) { return VersionMatches(a, b); }
bool SameRecording(const std::wstring& a, const std::wstring& b) {
    const auto left = ParseRecordingName(a), right = ParseRecordingName(b);
    return !left.base.empty() && left.base == right.base && left.version == right.version;
}
int match::CatalogScore(const ArtworkRequest& request, const std::wstring& artist, const std::wstring& name,
                        const std::wstring& collection, bool album, bool anyArtist, int durationMs) {
    return ResultScore(Clean(request), artist, name, collection, album, anyArtist, durationMs);
}
std::wstring ToSimplified(const std::wstring& s) { return Simplified(s); }

void Log(const wchar_t* format, ...) {
    static std::mutex m; std::lock_guard lock(m);
    wchar_t line[1024]; va_list args; va_start(args, format); _vsnwprintf_s(line, _TRUNCATE, format, args); va_end(args);
    OutputDebugStringW(L"[now-playing] "); OutputDebugStringW(line); OutputDebugStringW(L"\n");
    const std::wstring path = ArtworkSource::CacheDir() + L"\\now-playing.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 256 * 1024;
    HANDLE f = CreateFileW(path.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, big ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32]; sprintf_s(stamp, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    const std::string text = stamp + dock::json::ToUtf8(line) + "\r\n";
    DWORD written = 0; WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr); CloseHandle(f);
}

bool HttpGet(const std::string& url, std::string& body, uint64_t rangeOffset, uint64_t rangeLength, size_t maxBytes, const wchar_t* userAgent,
             const wchar_t* extraHeaders) {
    const wchar_t* agent = userAgent ? userAgent : L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    body.clear();
    const std::wstring wurl = dock::json::FromUtf8(url);
    wchar_t host[256]{}, path[4096]{}, extra[4096]{};
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) { Log(L"http: bad url"); return false; }
    Inet session(WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) session.h = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 8000, 10000, 15000, 30000);
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    Inet connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) return false;
    const std::wstring object = std::wstring(path) + extra;
    Inet request(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) return false;
    std::wstring headers;
    if (rangeLength) headers = L"Range: bytes=" + std::to_wstring(rangeOffset) + L"-" + std::to_wstring(rangeOffset + rangeLength - 1) + L"\r\n";
    if (extraHeaders) headers += extraHeaders;
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
        Log(L"http: request failed %lu", GetLastError()); return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200 && status != 206) { Log(L"http: status %lu", status); return false; }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (!available) break;
        if (body.size() + available > maxBytes) { Log(L"http: response too large"); return false; }
        const size_t at = body.size(); body.resize(at + available); DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, available, &read)) return false;
        body.resize(at + read);
    }
    if (rangeLength && status == 200) {  // server ignored Range: keep the requested slice
        if (rangeOffset + rangeLength > body.size()) return false;
        body = body.substr(static_cast<size_t>(rangeOffset), static_cast<size_t>(rangeLength));
    }
    return !rangeLength || body.size() == rangeLength;
}

std::wstring ArtworkSource::CacheDir() {
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L".";
    const std::wstring root = std::wstring(base) + L"\\LiquidDesktop", dir = root + L"\\artwork";
    CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ArtworkSource::KeyFor(const ArtworkRequest& raw) {
    const ArtworkRequest r = Clean(raw);
    const std::wstring artist = Normalize(r.artist, false);
    const std::wstring title = Normalize(r.title, false), album = Normalize(r.album, false);
    if (artist.empty() || (title.empty() && album.empty())) return {};
    uint64_t hash = 1469598103934665603ull;
    const std::wstring id = L"v4|" + artist + L"|" + album + L"|" + title;
    for (unsigned char c : dock::json::ToUtf8(id)) { hash ^= c; hash *= 1099511628211ull; }
    wchar_t out[24]; swprintf_s(out, L"%016llx", static_cast<unsigned long long>(hash));
    return out;
}

ArtworkSource::ArtworkSource(Callback callback) : callback_(std::move(callback)) { thread_ = std::thread([this] { Loop(); }); }

ArtworkSource::~ArtworkSource() {
    { std::lock_guard lock(mutex_); stop_ = true; pending_.reset(); activeKey_.clear(); }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();  // an in-flight HTTP call ends within its timeouts
}

std::wstring ArtworkSource::RequestKey(const ArtworkRequest& request) {
    return KeyFor(request) + (request.animatedArtwork ? L"-motion" : L"-still");
}

void ArtworkSource::Request(const ArtworkRequest& request) {
    { std::lock_guard lock(mutex_); pending_ = request; activeKey_ = RequestKey(request); }
    wake_.notify_all();
}

bool ArtworkSource::Cancelled(const std::wstring& key) { std::lock_guard lock(mutex_); return stop_ || activeKey_ != key; }

void ArtworkSource::Loop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC decodes catalog covers for the image check
    struct ComScope { HRESULT hr; ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); } } scope{com};
    for (;;) {
        ArtworkRequest next;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || pending_.has_value(); });
            if (stop_) return;
            next = *pending_; pending_.reset();
        }
        Process(next);
    }
}

void ArtworkSource::Process(const ArtworkRequest& raw) {
    const ArtworkRequest r = Clean(raw);
    const std::wstring key = KeyFor(r);
    if (key.empty()) return;
    const std::wstring requestKey = RequestKey(r);
    if (Cancelled(requestKey)) return;
    const std::wstring dir = CacheDir(), stem = dir + L"\\" + key + (r.preferTall ? L"-tall" : L"");
    const std::wstring jpg = dir + L"\\" + key + L".jpg", pageFile = dir + L"\\" + key + L".page";
    const std::wstring mp4 = stem + L".mp4", noVideo = stem + L".novideo", noMatch = dir + L"\\" + key + L".nomatch";
    if (!Exists(jpg) && FreshMarker(noMatch)) return;

    ArtworkResult result{key, Exists(jpg) ? jpg : L"", r.animatedArtwork && Exists(mp4) ? mp4 : L""};
    if (!result.imagePath.empty() || !result.videoPath.empty()) callback_(result);
    if (!result.videoPath.empty() ||
        (!result.imagePath.empty() && (!r.animatedArtwork || FreshMarker(noVideo)))) return;

    std::string page = ReadFileAll(pageFile);
    if (result.imagePath.empty() || page.empty()) {
        // With the player's cover: the candidate whose cover looks the same wins, and nothing is used when none
        // does (a wrong cover is worse than the player's own). Without it: only an exact name match.
        uint64_t want = 0;
        if (r.thumb && r.thumbW && r.thumbH && r.thumb->size() >= size_t(r.thumbW) * r.thumbH * 4) want = match::DHash(r.thumb->data(), r.thumbW, r.thumbH, r.thumbW * 4);
        const int bits = std::popcount(want);
        const bool check = bits >= 4 && bits <= 60;  // a flat cover says nothing
        const Candidate* chosen = nullptr;
        std::vector<Candidate> found;
        std::vector<std::pair<std::string, int>> coverDistances;
        int bestDistance = 99;
        const std::string local = Storefront();
        std::vector<std::string> countries{local};
        if (HasCjk(r.artist + r.title + r.album)) for (const char* c : {"tw", "hk", "jp"}) countries.push_back(c);  // Chinese/Japanese names
        if (local != "us") countries.push_back("us");
        countries.erase(std::unique(countries.begin(), countries.end()), countries.end());
        for (const auto& country : countries) {
            struct Query { int source; bool album; std::wstring term; };  // source 0: iTunes Search, 1: Apple Music web search
            std::vector<Query> queries;
            if (!r.title.empty()) {
                queries.push_back({0, false, r.artist + L" " + r.title});
                queries.push_back({0, false, r.title});
            }
            if (!r.album.empty()) {
                queries.push_back({0, true, r.artist + L" " + r.album});
                queries.push_back({1, true, r.artist + L" " + r.album});
            }
            for (const auto& q : queries) {
                if (Cancelled(requestKey)) return;
                found.clear();
                if (q.source == 0) Search(r, country, q.album, q.term, check, found);
                else WebSearch(r, country, q.term, false, found);
                std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
                if (!check) {
                    if (!found.empty() && found[0].score >= 8 &&
                        (found.size() == 1 || found[0].score > found[1].score))
                        chosen = &found[0];
                }
                else for (auto& c : found) {
                    if (Cancelled(requestKey)) return;
                    const auto previous = std::find_if(coverDistances.begin(), coverDistances.end(),
                        [&](const auto& entry) { return entry.first == c.art; });
                    int d;
                    if (previous != coverDistances.end()) d = previous->second;
                    else {
                        if (coverDistances.size() >= 10) continue;
                        const uint64_t hash = CoverHash(c.art);
                        d = hash ? match::Distance(hash, want) : 99;
                        coverDistances.emplace_back(c.art, d);
                    }
                    if (d < bestDistance) {
                        bestDistance = d;
                        if (d <= match::kSameCover) chosen = &c;
                    }
                }
                if (chosen) break;
            }
            if (chosen) break;
        }
        if (Cancelled(requestKey)) return;
        if (!chosen) {
            Log(L"no catalog match for %ls / %ls (%zu candidates, closest cover %d)", r.artist.c_str(), (r.album.empty() ? r.title : r.album).c_str(), found.size(), bestDistance);
            if (check) WriteFileAll(noMatch, "none");  // definitive only when the cover was compared
            return;
        }
        Log(L"matched %ls / %ls: score %d, cover distance %d", r.artist.c_str(), (r.album.empty() ? r.title : r.album).c_str(), chosen->score, check ? bestDistance : -1);
        const Candidate m = *chosen;
        page = m.page; WriteFileAll(pageFile, page);
        std::string image;
        if (result.imagePath.empty() && HttpGet(ArtUrl(m.art, 2000), image, 0, 0, 24u * 1024 * 1024) && WriteFileAll(jpg, image)) {
            result.imagePath = jpg; Log(L"still cached %ls", key.c_str());
            if (!Cancelled(requestKey)) callback_(result);
        }
    }
    if (!r.animatedArtwork || page.empty() || Cancelled(requestKey)) return;

    std::string html;
    if (!HttpGet(page, html, 0, 0, 12u * 1024 * 1024)) return;
    const std::string master = hls::FindMotionVideo(html, r.preferTall);
    if (master.empty()) { Log(L"no motion artwork %ls", key.c_str()); WriteFileAll(noVideo, "none"); return; }
    std::string text;
    if (Cancelled(requestKey) || !HttpGet(master, text, 0, 0, 1024 * 1024)) return;
    const auto variants = hls::ParseMaster(text, master);
    const hls::Variant* v = hls::PickVariant(variants, 2160);
    if (!v) { Log(L"no playable motion variant %ls", key.c_str()); WriteFileAll(noVideo, "none"); return; }
    if (Cancelled(requestKey) || !HttpGet(v->uri, text, 0, 0, 1024 * 1024)) return;
    const auto ranges = hls::Coalesce(hls::ParseMedia(text, v->uri));
    if (ranges.empty()) { WriteFileAll(noVideo, "none"); return; }
    std::string video, part;
    for (const auto& range : ranges) {
        if (Cancelled(requestKey)) return;
        if (!HttpGet(range.uri, part, range.offset, range.ranged ? range.length : 0, 64u * 1024 * 1024)) return;
        if (video.size() + part.size() > 96u * 1024 * 1024) return;
        video += part;
    }
    hls::RebaseFragments(video);
    if (!WriteFileAll(mp4, video)) return;
    Log(L"motion cached %ls %dx%d %ls (%zu KB)", key.c_str(), v->width, v->height, dock::json::FromUtf8(v->codecs).c_str(), video.size() / 1024);
    PruneVideos(dir, mp4);
    result.videoPath = mp4;
    if (!Cancelled(requestKey)) callback_(result);
}

}  // namespace nowplaying
