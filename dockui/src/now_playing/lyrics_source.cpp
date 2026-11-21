#include "lyrics_source.h"

#include "artwork_source.h"
#include "../json_lite.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <vector>
#include <cstdlib>

namespace nowplaying {
namespace {

constexpr wchar_t kAgent[] = L"DockOver Now Playing (lyrics)";

std::string ReadBytes(const std::wstring& path, size_t limit) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string out; LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size_t(size.QuadPart) <= limit) {
        out.resize(size_t(size.QuadPart)); DWORD read = 0;
        if (!ReadFile(f, out.data(), DWORD(out.size()), &read, nullptr)) read = 0;
        out.resize(read);
    }
    CloseHandle(f);
    return out;
}

bool WriteText(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; const BOOL ok = WriteFile(f, data.data(), DWORD(data.size()), &written, nullptr);
    CloseHandle(f); return ok && written == data.size();
}

bool FreshMarker(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    const auto t = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return t(now) - t(a.ftLastWriteTime) < 3ull * 24 * 3600 * 10000000ull;
}

std::wstring CacheDir() {
    const std::wstring dir = ArtworkSource::CacheDir() + L"\\lyrics";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring KeyFor(const LyricsRequest& r) {
    const std::wstring artist = NormalizeName(r.artist), title = NormalizeName(r.title);
    if (artist.empty() || title.empty()) return {};
    uint64_t hash = 1469598103934665603ull;
    // Bracketed qualifiers are part of the key: a remix has other lyrics timing than the original.
    for (unsigned char c : dock::json::ToUtf8(L"l4|" + artist + L"|" + title + L"|" + r.title)) { hash ^= c; hash *= 1099511628211ull; }
    wchar_t out[24]; swprintf_s(out, L"%016llx", static_cast<unsigned long long>(hash));
    return out;
}

std::string JsonString(const dock::json::Value& o, const char* key) {
    const auto* v = o.Find(key); return v && v->type == dock::json::Value::Type::String ? v->string : std::string{};
}
double JsonNumber(const dock::json::Value& o, const char* key) {
    const auto* v = o.Find(key); return v && v->type == dock::json::Value::Type::Number ? v->number : 0;
}

// LRCLIB record -> LRC text (synced preferred). Empty when it has neither.
// NetEase Cloud Music's public web API (no account): search, then the song's LRC and its translation. Results
// must name the same artist, the same title and version, and (when the player gives one) a length within 3 s.
// A catalog result is usable when its title (version words included) and artist match and, when the player gives
// the length, it is within 3 s. Returns a score (higher is better) or -1.
int Candidate(const LyricsRequest& r, const std::wstring& title, const std::vector<std::wstring>& artists, double lengthMs) {
    const std::wstring wantArtist = NormalizeName(r.artist);
    if (!SameRecording(title, r.title)) return -1;
    bool artistOk = false, exact = false;
    for (const auto& a : artists) {
        const std::wstring n = NormalizeName(a);
        if (n.empty()) continue;
        if (n == wantArtist) exact = artistOk = true;
        else if (wantArtist.find(n) != std::wstring::npos || n.find(wantArtist) != std::wstring::npos) artistOk = true;
    }
    if (!artistOk) return -1;
    if (r.durationMs > 0 && (lengthMs <= 0 || std::abs(lengthMs - r.durationMs) > 3000)) return -1;
    return (exact ? 2 : 0) + (r.durationMs > 0 && std::abs(lengthMs - r.durationMs) <= 1500 ? 2 : 0);
}

// Leading "Title - Artist" line some services add before the lyrics (QQ Music, Kugou).
void StripHeader(lyrics::Lyrics& l, const LyricsRequest& r) {
    const std::wstring title = NormalizeName(r.title);
    while (!l.lines.empty() && l.lines.front().ms < 15000) {
        const std::wstring& t = l.lines.front().text;
        if (t.find(L'-') == std::wstring::npos || NormalizeName(t).find(title) == std::wstring::npos) break;
        l.lines.erase(l.lines.begin());
    }
}

lyrics::Lyrics Finish(lyrics::Lyrics l, const LyricsRequest& r) {
    StripHeader(l, r); lyrics::DropCredits(l);
    for (auto& line : l.lines) line.translation.clear();  // original lyrics only (translations later)
    return l;
}

std::wstring HtmlDecode(std::wstring s) {
    std::wstring out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') { out += s[i]; continue; }
        const size_t end = s.find(L';', i); if (end == std::wstring::npos || end - i > 10) { out += s[i]; continue; }
        const std::wstring e = s.substr(i + 1, end - i - 1);
        wchar_t c = 0;
        if (e == L"amp") c = L'&'; else if (e == L"lt") c = L'<'; else if (e == L"gt") c = L'>'; else if (e == L"quot") c = L'"'; else if (e == L"apos") c = L'\'';
        else if (e.size() > 1 && e[0] == L'#') c = wchar_t(e[1] == L'x' || e[1] == L'X' ? std::wcstoul(e.c_str() + 2, nullptr, 16) : std::wcstoul(e.c_str() + 1, nullptr, 10));
        if (!c) { out += s[i]; continue; }
        out += c; i = end;
    }
    return out;
}

std::string Base64Decode(const std::string& in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26; if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62; if (c == '/' || c == '_') return 63; return -1;
    };
    std::string out; int buffer = 0, bits = 0;
    for (char c : in) {
        const int v = value(c); if (v < 0) continue;
        buffer = (buffer << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += char((buffer >> bits) & 0xFF); }
    }
    return out;
}

// QQ Music public web API (no account): search, then the song's LRC.
lyrics::Lyrics QQMusic(const LyricsRequest& r) {
    const wchar_t* referer = L"Referer: https://y.qq.com/\r\n";
    std::string body, error; dock::json::Value v;
    const std::string search = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&n=20&p=1&w=" + UrlEncodeQuery(r.title + L" " + r.artist);
    if (!HttpGet(search, body, 0, 0, 2u * 1024 * 1024, nullptr, referer) || !dock::json::Parse(body, v, error)) return {};
    const auto* data = v.Find("data"); const auto* song = data ? data->Find("song") : nullptr; const auto* list = song ? song->Find("list") : nullptr;
    if (!list || list->type != dock::json::Value::Type::Array) return {};
    std::string mid; int best = -1;
    for (const auto& s : list->array) {
        std::vector<std::wstring> artists;
        if (const auto* singers = s.Find("singer"); singers && singers->type == dock::json::Value::Type::Array)
            for (const auto& a : singers->array) artists.push_back(dock::json::FromUtf8(JsonString(a, "name")));
        const int score = Candidate(r, dock::json::FromUtf8(JsonString(s, "songname")), artists, JsonNumber(s, "interval") * 1000);
        if (score > best) { best = score; mid = JsonString(s, "songmid"); }
    }
    if (mid.empty()) return {};
    if (!HttpGet("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&songmid=" + mid, body, 0, 0, 2u * 1024 * 1024, nullptr, referer) ||
        !dock::json::Parse(body, v, error)) return {};
    const std::string text = JsonString(v, "lyric");
    if (text.empty()) return {};
    return Finish(lyrics::ParseLrc(HtmlDecode(dock::json::FromUtf8(text))), r);
}

// Kugou lyrics service (no account): search by "Artist - Title" and length, then download the LRC (base64).
lyrics::Lyrics Kugou(const LyricsRequest& r) {
    std::string body, error; dock::json::Value v;
    std::string search = "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword=" + UrlEncodeQuery(r.artist + L" - " + r.title);
    if (r.durationMs > 0) search += "&duration=" + std::to_string(r.durationMs);
    if (!HttpGet(search, body, 0, 0, 2u * 1024 * 1024, nullptr) || !dock::json::Parse(body, v, error)) return {};
    const auto* list = v.Find("candidates");
    if (!list || list->type != dock::json::Value::Type::Array) return {};
    std::string id, key; int best = -1;
    for (const auto& c : list->array) {
        const std::wstring singer = dock::json::FromUtf8(JsonString(c, "singer"));
        std::vector<std::wstring> artists{singer};
        for (size_t a = 0, b; a <= singer.size(); a = b + 1) {  // "A、B" / "A&B"
            b = singer.find_first_of(L"\x3001&,/", a); if (b == std::wstring::npos) b = singer.size();
            artists.push_back(singer.substr(a, b - a));
        }
        const int score = Candidate(r, dock::json::FromUtf8(JsonString(c, "song")), artists, JsonNumber(c, "duration"));
        if (score <= best) continue;
        const auto* idv = c.Find("id");
        std::string candidateId = idv && idv->type == dock::json::Value::Type::String ? idv->string : idv && idv->type == dock::json::Value::Type::Number ? std::to_string((long long)idv->number) : "";
        if (candidateId.empty()) continue;
        best = score; id = candidateId; key = JsonString(c, "accesskey");
    }
    if (id.empty() || key.empty()) return {};
    if (!HttpGet("https://lyrics.kugou.com/download?ver=1&client=pc&fmt=lrc&charset=utf8&id=" + id + "&accesskey=" + key, body, 0, 0, 2u * 1024 * 1024, nullptr) ||
        !dock::json::Parse(body, v, error)) return {};
    const std::string text = Base64Decode(JsonString(v, "content"));
    if (text.empty()) return {};
    return Finish(lyrics::ParseLrc(LyricsSource::DecodeText(text)), r);
}

lyrics::Lyrics NetEase(const LyricsRequest& r) {
    const std::wstring wantArtist = NormalizeName(r.artist);
    std::string body, error; dock::json::Value v;
    const std::string search = "https://music.163.com/api/search/get?type=1&limit=20&s=" + UrlEncodeQuery(r.title + L" " + r.artist);
    if (!HttpGet(search, body, 0, 0, 2u * 1024 * 1024, nullptr) || !dock::json::Parse(body, v, error)) return {};
    const auto* result = v.Find("result"); const auto* songs = result ? result->Find("songs") : nullptr;
    if (!songs || songs->type != dock::json::Value::Type::Array) return {};
    double bestId = 0; int bestScore = -1;
    for (const auto& song : songs->array) {
        const std::wstring nameRaw = dock::json::FromUtf8(JsonString(song, "name"));
        if (!SameRecording(nameRaw, r.title)) continue;
        bool artistOk = false, exact = false;
        if (const auto* artists = song.Find("artists"); artists && artists->type == dock::json::Value::Type::Array)
            for (const auto& a : artists->array) {
                const std::wstring n = NormalizeName(dock::json::FromUtf8(JsonString(a, "name")));
                if (n.empty()) continue;
                if (n == wantArtist) exact = artistOk = true;
                else if (wantArtist.find(n) != std::wstring::npos || n.find(wantArtist) != std::wstring::npos) artistOk = true;
            }
        if (!artistOk) continue;
        const double length = JsonNumber(song, "duration");
        if (r.durationMs > 0 && (length <= 0 || std::abs(length - r.durationMs) > 3000)) continue;
        const int score = (exact ? 2 : 0) + (r.durationMs > 0 && std::abs(length - r.durationMs) <= 1500 ? 2 : 0);
        if (score > bestScore) { bestScore = score; bestId = JsonNumber(song, "id"); }
    }
    if (bestId <= 0) return {};
    char url[160]; sprintf_s(url, "https://music.163.com/api/song/lyric?lv=1&tv=-1&id=%.0f", bestId);
    if (!HttpGet(url, body, 0, 0, 2u * 1024 * 1024, nullptr) || !dock::json::Parse(body, v, error)) return {};
    const auto* lrc = v.Find("lrc"); const std::string text = lrc ? JsonString(*lrc, "lyric") : std::string{};
    if (text.empty() || text.find("\xE7\xBA\xAF\xE9\x9F\xB3\xE4\xB9\x90\xEF\xBC\x8C\xE8\xAF\xB7\xE6\xAC\xA3\xE8\xB5\x8F") != std::string::npos) return {};  // "纯音乐，请欣赏"
    return Finish(lyrics::ParseLrc(dock::json::FromUtf8(text)), r);
}

// Saved form of the lyrics (LRC with translations as same-time lines), so the cache keeps translations.
std::string ToLrc(const lyrics::Lyrics& l) {
    std::wstring out;
    for (const auto& line : l.lines) {
        if (line.ms < 0) { out += line.text + L"\n"; continue; }
        wchar_t tag[24]; swprintf_s(tag, L"[%02d:%02d.%02d]", line.ms / 60000, (line.ms / 1000) % 60, (line.ms % 1000) / 10);
        out += tag + line.text + L"\n";
        if (!line.translation.empty()) out += tag + line.translation + L"\n";
    }
    return dock::json::ToUtf8(out);
}

std::string RecordText(const dock::json::Value& o) {
    std::string synced = JsonString(o, "syncedLyrics");
    return synced.empty() ? JsonString(o, "plainLyrics") : synced;
}

}  // namespace

std::wstring LyricsSource::CustomDir() {
    const std::wstring dir = ArtworkSource::CacheDir() + L"\\..\\lyrics";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t full[MAX_PATH]{};
    return GetFullPathNameW(dir.c_str(), MAX_PATH, full, nullptr) ? std::wstring(full) : dir;
}

std::wstring LyricsSource::DecodeText(const std::string& b) {
    auto convert = [&](UINT cp, const char* data, int n, DWORD flags) -> std::wstring {
        const int len = MultiByteToWideChar(cp, flags, data, n, nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(size_t(len), L'\0'); MultiByteToWideChar(cp, flags, data, n, w.data(), len); return w;
    };
    if (b.size() >= 2 && uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xFE) return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
    if (b.size() >= 2 && uint8_t(b[0]) == 0xFE && uint8_t(b[1]) == 0xFF) {
        std::wstring w; for (size_t i = 2; i + 1 < b.size(); i += 2) w += wchar_t((uint8_t(b[i]) << 8) | uint8_t(b[i + 1])); return w;
    }
    size_t skip = b.size() >= 3 && uint8_t(b[0]) == 0xEF && uint8_t(b[1]) == 0xBB && uint8_t(b[2]) == 0xBF ? 3 : 0;
    std::wstring w = convert(CP_UTF8, b.data() + skip, int(b.size() - skip), MB_ERR_INVALID_CHARS);
    if (!w.empty() || b.size() == skip) return w;
    return convert(936, b.data(), int(b.size()), 0);  // GBK, common for Chinese .lrc files
}

LyricsSource::LyricsSource(Callback callback) : callback_(std::move(callback)) { thread_ = std::thread([this] { Loop(); }); }
LyricsSource::~LyricsSource() {
    { std::lock_guard l(mutex_); stop_ = true; pending_.reset(); }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void LyricsSource::Request(const LyricsRequest& r) { { std::lock_guard l(mutex_); pending_ = r; } wake_.notify_all(); }
LyricsResult LyricsSource::RunNow(const LyricsRequest& r) { return {r.tag, Find(r)}; }

void LyricsSource::Loop() {
    for (;;) {
        LyricsRequest next;
        {
            std::unique_lock l(mutex_);
            wake_.wait(l, [&] { return stop_ || pending_.has_value(); });
            if (stop_) return;
            next = *pending_; pending_.reset();
        }
        LyricsResult result{next.tag, Find(next)};
        { std::lock_guard l(mutex_); if (stop_ || pending_) continue; }  // superseded
        callback_(result);
    }
}

namespace {
// Online lyrics in Traditional Chinese are shown in Simplified when the player names the track in Simplified.
void MatchScript(lyrics::Lyrics& l, const LyricsRequest& r) {
    const std::wstring names = r.artist + r.title;
    if (names.empty() || ToSimplified(names) != names) return;  // the player uses Traditional (or no Chinese)
    if (std::none_of(names.begin(), names.end(), [](wchar_t c) { return c >= 0x3400 && c < 0xA000; })) return;
    for (auto& line : l.lines) { line.text = ToSimplified(line.text); line.translation = ToSimplified(line.translation); }
}
}  // namespace

lyrics::Lyrics LyricsSource::Find(const LyricsRequest& raw) {
    ArtworkRequest a{raw.title, raw.artist, raw.album}; a = CleanRequest(a);
    LyricsRequest r = raw; r.artist = a.artist; r.album = a.album;
    if (r.title.empty()) return {};

    // 0. The playing local file's own lyrics (an .lrc beside it, or its lyrics tag).
    if (!r.localText.empty()) {
        auto parsed = lyrics::ParseLrc(r.localText);
        if (!parsed.Empty()) { parsed.source = L"local file"; Log(L"lyrics: local music file"); return parsed; }
    }

    // 1. The user's files.
    {
        const std::wstring dir = CustomDir();
        const std::wstring want[] = {NormalizeName(r.artist + L" - " + r.title), NormalizeName(r.title + L" - " + r.artist), NormalizeName(r.title)};
        WIN32_FIND_DATAW f{}; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
        std::wstring best; int bestRank = 99;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = f.cFileName; const size_t dot = name.find_last_of(L'.');
                if (dot == std::wstring::npos) continue;
                std::wstring ext = name.substr(dot); for (auto& c : ext) c = wchar_t(towlower(c));
                if (ext != L".lrc" && ext != L".txt") continue;
                const std::wstring stem = name.substr(0, dot), norm = NormalizeName(stem);
                for (int i = 0; i < 3; ++i)
                    if (norm == want[i] && SameVersion(stem, i == 2 ? r.title : (i == 0 ? r.artist + L" - " + r.title : r.title + L" - " + r.artist)) && i < bestRank) {
                        bestRank = i; best = dir + L"\\" + name;
                    }
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
        if (!best.empty()) {
            auto parsed = lyrics::ParseLrc(DecodeText(ReadBytes(best, 2u * 1024 * 1024)));
            if (!parsed.Empty()) { parsed.source = L"file"; Log(L"lyrics: file %ls", best.c_str()); return parsed; }
        }
    }

    // 2. Cache.
    const std::wstring key = KeyFor(r);
    if (key.empty()) return {};
    const std::wstring cache = CacheDir() + L"\\" + key + L".lrc", none = CacheDir() + L"\\" + key + L".none";
    if (const std::string cached = ReadBytes(cache, 2u * 1024 * 1024); !cached.empty()) {
        auto parsed = lyrics::ParseLrc(DecodeText(cached)); parsed.source = L"cache"; MatchScript(parsed, r);
        for (auto& line : parsed.lines) line.translation.clear();
        return parsed;
    }
    if (!r.online || FreshMarker(none)) return {};

    // 3. NetEase Cloud Music, QQ Music, Kugou (synced lyrics, broad coverage incl. Chinese), then 4. LRCLIB.
    struct Provider { const wchar_t* name; lyrics::Lyrics (*find)(const LyricsRequest&); };
    for (const Provider& p : {Provider{L"netease", NetEase}, Provider{L"qqmusic", QQMusic}, Provider{L"kugou", Kugou}}) {
        auto found = p.find(r);
        if (found.Empty() || !found.synced) continue;
        WriteText(cache, ToLrc(found));
        found.source = p.name; MatchScript(found, r);
        Log(L"lyrics: %ls %ls / %ls (%zu lines)", p.name, r.artist.c_str(), r.title.c_str(), found.lines.size());
        return found;
    }
    std::string text;
    {
        std::string url = "https://lrclib.net/api/get?artist_name=" + UrlEncodeQuery(r.artist) + "&track_name=" + UrlEncodeQuery(r.title);
        if (!r.album.empty()) url += "&album_name=" + UrlEncodeQuery(r.album);
        if (r.durationMs > 0) url += "&duration=" + std::to_string((r.durationMs + 500) / 1000);
        std::string body, error; dock::json::Value v;
        if (HttpGet(url, body, 0, 0, 2u * 1024 * 1024, kAgent) && dock::json::Parse(body, v, error) && v.IsObject()) {
            const double length = JsonNumber(v, "duration");
            if (r.durationMs <= 0 || length <= 0 || std::abs(length * 1000 - r.durationMs) <= 3000) text = RecordText(v);
        }
    }
    if (text.empty()) {
        const std::string url = "https://lrclib.net/api/search?track_name=" + UrlEncodeQuery(r.title) + "&artist_name=" + UrlEncodeQuery(r.artist);
        std::string body, error; dock::json::Value v;
        if (HttpGet(url, body, 0, 0, 4u * 1024 * 1024, kAgent) && dock::json::Parse(body, v, error) && v.type == dock::json::Value::Type::Array) {
            const std::wstring wantArtist = NormalizeName(r.artist);
            int bestScore = -1;
            for (const auto& o : v.array) {
                const std::wstring artist = NormalizeName(dock::json::FromUtf8(JsonString(o, "artistName")));
                const std::wstring trackRaw = dock::json::FromUtf8(JsonString(o, "trackName"));
                if (!SameRecording(trackRaw, r.title)) continue;
                if (artist != wantArtist && artist.find(wantArtist) == std::wstring::npos && wantArtist.find(artist) == std::wstring::npos) continue;
                const double length = JsonNumber(o, "duration");
                if (r.durationMs > 0 && (length <= 0 || std::abs(length * 1000 - r.durationMs) > 3000)) continue;
                const std::string t = RecordText(o); if (t.empty()) continue;
                const int score = (JsonString(o, "syncedLyrics").empty() ? 0 : 2) + (artist == wantArtist ? 1 : 0);
                if (score > bestScore) { bestScore = score; text = t; }
            }
        }
    }
    if (text.empty()) { Log(L"lyrics: none for %ls / %ls", r.artist.c_str(), r.title.c_str()); WriteText(none, "none"); return {}; }
    WriteText(cache, text);
    auto parsed = Finish(lyrics::ParseLrc(dock::json::FromUtf8(text)), r);
    parsed.source = L"lrclib"; MatchScript(parsed, r);
    Log(L"lyrics: lrclib %ls / %ls (%zu lines, %ls)", r.artist.c_str(), r.title.c_str(), parsed.lines.size(), parsed.synced ? L"synced" : L"plain");
    return parsed;
}

}  // namespace nowplaying
