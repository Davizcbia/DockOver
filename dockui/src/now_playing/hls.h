#pragma once
// Pure text helpers for the Now Playing artwork fetcher: Apple Music album page scanning and the
// small HLS subset Apple uses for motion artwork (VOD fMP4, one file addressed by byte ranges).
// No I/O here so the parsing is unit tested (tests/now_playing_hls_test.cpp).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace nowplaying::hls {

struct Variant { std::string uri, codecs; int width = 0, height = 0; uint64_t bandwidth = 0; };
struct Range { std::string uri; uint64_t offset = 0, length = 0; bool ranged = false; };

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out; size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        out.push_back(Trim(text.substr(p, e - p))); p = e + 1;
    }
    return out;
}

// Relative references against an absolute http(s) URL (only the forms HLS playlists use).
inline std::string Resolve(const std::string& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    const size_t scheme = base.find("://"); if (scheme == std::string::npos) return ref;
    if (ref.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + ref;
    const size_t hostEnd = base.find('/', scheme + 3);
    const std::string origin = hostEnd == std::string::npos ? base : base.substr(0, hostEnd);
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    std::string dir = base.substr(0, base.find_first_of("?#"));
    dir = dir.substr(0, dir.find_last_of('/') + 1);
    if (dir.size() <= scheme + 3) dir = origin + "/";
    return dir + ref;
}

// Value of KEY in an attribute list (KEY=VALUE,KEY="quoted, value"). Empty when missing.
inline std::string Attribute(const std::string& list, const std::string& key) {
    size_t i = 0;
    while (i < list.size()) {
        size_t eq = list.find('=', i); if (eq == std::string::npos) break;
        const std::string k = Trim(list.substr(i, eq - i));
        size_t v = eq + 1, end;
        std::string value;
        if (v < list.size() && list[v] == '"') {
            end = list.find('"', v + 1); if (end == std::string::npos) end = list.size();
            value = list.substr(v + 1, end - v - 1); end = list.find(',', end); if (end == std::string::npos) end = list.size();
        } else {
            end = list.find(',', v); if (end == std::string::npos) end = list.size();
            value = Trim(list.substr(v, end - v));
        }
        if (k == key) return value;
        i = end + 1;
    }
    return {};
}

inline std::vector<Variant> ParseMaster(const std::string& text, const std::string& baseUrl) {
    std::vector<Variant> out; const auto lines = Lines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string tag = "#EXT-X-STREAM-INF:";
        if (lines[i].rfind(tag, 0) != 0) continue;
        const std::string attrs = lines[i].substr(tag.size());
        size_t j = i + 1; while (j < lines.size() && (lines[j].empty() || lines[j][0] == '#')) ++j;
        if (j >= lines.size()) break;
        Variant v; v.uri = Resolve(baseUrl, lines[j]); v.codecs = Attribute(attrs, "CODECS");
        v.bandwidth = std::strtoull(Attribute(attrs, "BANDWIDTH").c_str(), nullptr, 10);
        const std::string res = Attribute(attrs, "RESOLUTION"); const size_t x = res.find('x');
        if (x != std::string::npos) { v.width = std::atoi(res.substr(0, x).c_str()); v.height = std::atoi(res.substr(x + 1).c_str()); }
        out.push_back(v); i = j;
    }
    return out;
}

// H.264 decodes everywhere on Windows; HEVC needs an optional Store extension, so it is only a fallback.
// Highest resolution not above maxHeight, then highest bandwidth.
inline const Variant* PickVariant(const std::vector<Variant>& variants, int maxHeight) {
    const Variant* best = nullptr; int bestRank = -1;
    for (const auto& v : variants) {
        const bool avc = v.codecs.find("avc1") != std::string::npos, hevc = v.codecs.find("hvc1") != std::string::npos || v.codecs.find("hev1") != std::string::npos;
        if (!avc && !hevc && !v.codecs.empty()) continue;  // e.g. audio-only
        if (v.height > maxHeight) continue;
        const int rank = (avc || v.codecs.empty()) ? 1 : 0;
        if (!best || rank > bestRank || (rank == bestRank && (v.height > best->height || (v.height == best->height && v.bandwidth > best->bandwidth)))) { best = &v; bestRank = rank; }
    }
    return best;
}

// "LEN@OFF" or "LEN" (continues after the previous range of the same resource).
inline bool ParseByteRange(const std::string& s, uint64_t previousEnd, uint64_t& offset, uint64_t& length) {
    const size_t at = s.find('@'); char* end = nullptr;
    length = std::strtoull(s.c_str(), &end, 10); if (end == s.c_str() || !length) return false;
    offset = at == std::string::npos ? previousEnd : std::strtoull(s.c_str() + at + 1, nullptr, 10);
    return true;
}

// Init section (EXT-X-MAP) first, then media segments in order. Empty when encrypted or unparseable.
inline std::vector<Range> ParseMedia(const std::string& text, const std::string& baseUrl) {
    std::vector<Range> out; const auto lines = Lines(text);
    if (lines.empty() || lines[0] != "#EXTM3U") return {};
    std::string pendingRange; uint64_t lastEnd = 0; std::string lastUri;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (line.rfind("#EXT-X-KEY:", 0) == 0 && Attribute(line.substr(11), "METHOD") != "NONE") return {};
        if (line.rfind("#EXT-X-MAP:", 0) == 0) {
            const std::string attrs = line.substr(11); Range r; r.uri = Resolve(baseUrl, Attribute(attrs, "URI"));
            const std::string br = Attribute(attrs, "BYTERANGE");
            if (!br.empty()) { if (!ParseByteRange(br, 0, r.offset, r.length)) return {}; r.ranged = true; lastEnd = r.offset + r.length; }
            lastUri = r.uri; out.push_back(r); continue;
        }
        if (line.rfind("#EXT-X-BYTERANGE:", 0) == 0) { pendingRange = line.substr(17); continue; }
        if (line[0] == '#') continue;
        Range r; r.uri = Resolve(baseUrl, line);
        if (!pendingRange.empty()) {
            if (!ParseByteRange(pendingRange, r.uri == lastUri ? lastEnd : 0, r.offset, r.length)) return {};
            r.ranged = true; lastEnd = r.offset + r.length;
        }
        pendingRange.clear(); lastUri = r.uri; out.push_back(r);
    }
    return out;
}

// Adjacent byte ranges of one resource become one request (Apple's motion artwork is a single file).
inline std::vector<Range> Coalesce(const std::vector<Range>& in) {
    std::vector<Range> out;
    for (const auto& r : in) {
        if (!out.empty() && r.ranged && out.back().ranged && out.back().uri == r.uri && out.back().offset + out.back().length == r.offset) out.back().length += r.length;
        else out.push_back(r);
    }
    return out;
}

inline std::string UnescapeJson(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char c = s[++i];
        if (c == 'u' && i + 4 < s.size()) {
            unsigned code = static_cast<unsigned>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16)); i += 4;
            if (code >= 0xD800 && code < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {  // surrogate pair
                const unsigned low = static_cast<unsigned>(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                if (low >= 0xDC00 && low < 0xE000) { code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00); i += 6; }
            }
            // UTF-8
            if (code < 0x80) out += char(code);
            else if (code < 0x800) { out += char(0xC0 | (code >> 6)); out += char(0x80 | (code & 0x3F)); }
            else if (code < 0x10000) { out += char(0xE0 | (code >> 12)); out += char(0x80 | ((code >> 6) & 0x3F)); out += char(0x80 | (code & 0x3F)); }
            else { out += char(0xF0 | (code >> 18)); out += char(0x80 | ((code >> 12) & 0x3F)); out += char(0x80 | ((code >> 6) & 0x3F)); out += char(0x80 | (code & 0x3F)); }
            continue;
        }
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    }
    return out;
}

// Motion artwork master playlist from an Apple Music album page (server-rendered JSON):
// "videoArtwork":{"dictionary":{"motionDetailSquare":{...,"video":"https://...m3u8"},"motionDetailTall":{...}}}
// Empty when the album has none ("videoArtwork":null).
inline std::string FindMotionVideo(const std::string& html, bool preferTall) {
    const size_t root = html.find("\"videoArtwork\":{");
    if (root == std::string::npos) return {};
    const size_t limit = std::min(html.size(), root + 64 * 1024);
    auto videoAfter = [&](const char* key) -> std::string {
        const size_t k = html.find(key, root); if (k == std::string::npos || k >= limit) return {};
        const size_t v = html.find("\"video\":\"", k); if (v == std::string::npos || v >= limit) return {};
        const size_t start = v + 9, end = html.find('"', start); if (end == std::string::npos) return {};
        std::string url = UnescapeJson(html.substr(start, end - start));
        if (url.rfind("https://", 0) != 0 || url.find(".m3u8") == std::string::npos) return {};
        return url;
    };
    const char* first = preferTall ? "\"motionDetailTall\"" : "\"motionDetailSquare\"";
    const char* second = preferTall ? "\"motionDetailSquare\"" : "\"motionDetailTall\"";
    std::string url = videoAfter(first);
    if (url.empty()) url = videoAfter(second);
    if (url.empty()) url = videoAfter("\"video\"");
    return url;
}

// Results of the public Apple Music web search page (music.apple.com/<storefront>/search?term=...), read from its
// server-rendered JSON. Used when the iTunes Search index does not have a release yet (new singles). Each item:
//   "subtitle":"Album · Artist","itemKind":"albums",...,"artwork":{"dictionary":{...,"url":"https://.../{w}x{h}bb.{f}"...}},
//   "title":"Name","contentDescriptor":{"kind":"album",...,"url":"https://music.apple.com/us/album/..."}
struct WebItem { std::string kind, title, artist, artwork, url; };

inline std::string JsonValueAfter(const std::string& s, const std::string& key, size_t from, size_t limit) {
    const std::string needle = "\"" + key + "\":\"";
    const size_t k = s.find(needle, from); if (k == std::string::npos || k >= limit) return {};
    const size_t start = k + needle.size(); size_t end = start;
    while (end < s.size() && s[end] != '"') end += s[end] == '\\' ? 2 : 1;
    return end < s.size() ? UnescapeJson(s.substr(start, end - start)) : std::string{};
}

inline std::vector<WebItem> ParseWebSearch(const std::string& html) {
    std::vector<WebItem> out;
    const std::string kindKey = "\"itemKind\":\"";
    size_t at = html.find(kindKey);
    while (at != std::string::npos) {
        const size_t next = html.find(kindKey, at + kindKey.size()), limit = next == std::string::npos ? html.size() : next;
        WebItem item;
        item.kind = JsonValueAfter(html, "itemKind", at, limit);
        const size_t back = at > 600 ? at - 600 : 0;
        const size_t sub = html.rfind("\"subtitle\":\"", at);
        const std::string subtitle = sub != std::string::npos && sub >= back ? JsonValueAfter(html, "subtitle", sub, at + 1) : std::string{};
        // "Album · Artist" (a middle dot between narrow no-break spaces): the artist is after the dot.
        const size_t dot = subtitle.find("\xC2\xB7");
        if (dot != std::string::npos) {
            size_t a = dot + 2;  // skip spaces and U+202F narrow no-break spaces
            for (;;) {
                if (a < subtitle.size() && subtitle[a] == ' ') ++a;
                else if (subtitle.compare(a, 3, "\xE2\x80\xAF") == 0) a += 3;
                else break;
            }
            item.artist = subtitle.substr(a);
        }
        const size_t art = html.find("\"artwork\":{\"dictionary\":{", at);
        if (art != std::string::npos && art < limit) item.artwork = JsonValueAfter(html, "url", art, limit);
        const size_t afterArt = art != std::string::npos && art < limit ? html.find('}', html.find("\"url\":\"", art)) : at;
        item.title = JsonValueAfter(html, "title", afterArt, limit);
        const size_t desc = html.find("\"contentDescriptor\":{", afterArt);
        if (desc != std::string::npos && desc < limit) item.url = JsonValueAfter(html, "url", desc, limit);
        if ((item.kind == "albums" || item.kind == "songs") && !item.title.empty() && !item.artist.empty() &&
            item.artwork.find("{w}x{h}") != std::string::npos && item.url.rfind("https://music.apple.com/", 0) == 0)
            out.push_back(item);
        at = next;
    }
    return out;
}

// HLS fMP4 media timestamps continue the stream timeline (Apple's motion artwork starts at 10 s), but a
// local file plays from 0: subtract the first decode time from every moof/traf/tfdt so the first frame
// shows at once. Returns false when the data is not a fragmented MP4 (left untouched).
inline bool RebaseFragments(std::string& mp4) {
    auto u32 = [&](size_t o) { return (uint64_t(uint8_t(mp4[o])) << 24) | (uint64_t(uint8_t(mp4[o + 1])) << 16) | (uint64_t(uint8_t(mp4[o + 2])) << 8) | uint8_t(mp4[o + 3]); };
    auto put32 = [&](size_t o, uint64_t v) { for (int i = 0; i < 4; ++i) mp4[o + i] = char((v >> (24 - 8 * i)) & 0xff); };
    struct Tfdt { size_t at; int version; uint64_t value; };
    std::vector<Tfdt> found;
    // Children of [begin, end): calls visit(type, payload, end) for each box.
    auto walk = [&](size_t begin, size_t end, auto&& visit) -> bool {
        size_t o = begin;
        while (o + 8 <= end) {
            uint64_t size = u32(o); size_t header = 8;
            if (size == 1) { if (o + 16 > end) return false; size = (u32(o + 8) << 32) | u32(o + 12); header = 16; }
            else if (size == 0) size = end - o;
            if (size < header || o + size > end) return false;
            visit(mp4.substr(o + 4, 4), o + header, size_t(o + size));
            o += size_t(size);
        }
        return true;
    };
    bool ok = walk(0, mp4.size(), [&](const std::string& type, size_t body, size_t end) {
        if (type != "moof") return;
        walk(body, end, [&](const std::string& t2, size_t b2, size_t e2) {
            if (t2 != "traf") return;
            walk(b2, e2, [&](const std::string& t3, size_t b3, size_t e3) {
                if (t3 != "tfdt" || e3 - b3 < 8) return;
                const int version = uint8_t(mp4[b3]);
                if (version == 1 && e3 - b3 >= 12) found.push_back({b3 + 4, 1, (u32(b3 + 4) << 32) | u32(b3 + 8)});
                else if (version == 0) found.push_back({b3 + 4, 0, u32(b3 + 4)});
            });
        });
    });
    if (!ok || found.empty()) return false;
    uint64_t base = found[0].value;
    for (const auto& f : found) base = std::min(base, f.value);
    for (const auto& f : found) {
        const uint64_t v = f.value - base;
        if (f.version == 1) { put32(f.at, v >> 32); put32(f.at + 4, v & 0xffffffffu); } else put32(f.at, v);
    }
    return true;
}

}  // namespace nowplaying::hls
