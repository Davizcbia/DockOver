#pragma once
// Lyrics model and LRC parsing for Now Playing. Pure functions, unit tested in tests/now_playing_hls_test.cpp.
//   [mm:ss.xx]Line            one or more time tags per line ([mm:ss], [mm:ss.x], [mm:ss.xxx], [mm:ss:xx])
//   [offset:+250]             shifts every line earlier by 250 ms (LRC convention)
//   <mm:ss.xx>word            word timing (enhanced LRC): stripped, the line timing is kept
//   [ti:][ar:][al:][by:]...   metadata: ignored
// Two lines with the same time (NetEase, QQ Music exports): the second is the first one's translation.
// Text without any time tag is plain (unsynced) lyrics, one line per text line.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

namespace nowplaying::lyrics {

struct Line { int ms = -1; std::wstring text, translation; };
struct Lyrics {
    std::vector<Line> lines;
    bool synced = false;
    std::wstring source;  // "file", "lrclib"
    bool Empty() const { return lines.empty(); }
    // Index of the line being sung at positionMs (-1 before the first line or when unsynced).
    int LineAt(int64_t positionMs) const {
        if (!synced) return -1;
        int found = -1;
        for (size_t i = 0; i < lines.size(); ++i) { if (lines[i].ms <= positionMs) found = int(i); else break; }
        return found;
    }
};

inline std::wstring TrimText(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx", "mm:ss:xx" into ms. -1 when it is not a time.
inline int ParseTime(const std::wstring& t) {
    size_t colon = t.find(L':'); if (colon == std::wstring::npos || colon == 0) return -1;
    for (size_t i = 0; i < colon; ++i) if (!std::iswdigit(t[i])) return -1;
    const int minutes = std::stoi(t.substr(0, colon));
    size_t i = colon + 1, start = i;
    while (i < t.size() && std::iswdigit(t[i])) ++i;
    if (i == start || i - start > 2) return -1;
    const int seconds = std::stoi(t.substr(start, i - start));
    int ms = 0;
    if (i < t.size()) {
        if (t[i] != L'.' && t[i] != L':') return -1;
        size_t f = ++i; while (i < t.size() && std::iswdigit(t[i])) ++i;
        if (i != t.size() || i == f) return -1;
        std::wstring frac = t.substr(f, std::min<size_t>(3, i - f)); while (frac.size() < 3) frac += L'0';
        ms = std::stoi(frac);
    }
    if (seconds > 59) return -1;
    return (minutes * 60 + seconds) * 1000 + ms;
}

inline Lyrics ParseLrc(const std::wstring& input) {
    Lyrics out;
    std::wstring text = input;
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    int offset = 0;
    std::vector<Line> timed; std::vector<std::wstring> plain;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find(L'\n', p); if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(p, e - p); p = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<int> times; size_t i = 0; bool meta = false;
        while (i < line.size() && line[i] == L'[') {
            const size_t close = line.find(L']', i); if (close == std::wstring::npos) break;
            const std::wstring tag = line.substr(i + 1, close - i - 1);
            const int ms = ParseTime(tag);
            if (ms >= 0) times.push_back(ms);
            else if (tag.rfind(L"offset:", 0) == 0) { try { offset = std::stoi(tag.substr(7)); } catch (...) {} meta = true; }
            else if (tag.find(L':') != std::wstring::npos) meta = true;  // [ti:...] etc.
            else break;
            i = close + 1;
        }
        std::wstring body = line.substr(i), clean;
        // Enhanced LRC word tags <mm:ss.xx>.
        for (size_t k = 0; k < body.size(); ++k) {
            if (body[k] == L'<') {
                const size_t close = body.find(L'>', k);
                if (close != std::wstring::npos && ParseTime(body.substr(k + 1, close - k - 1)) >= 0) { k = close; continue; }
            }
            clean += body[k];
        }
        clean = TrimText(clean);
        if (!times.empty()) { for (int t : times) timed.push_back({t, clean, L""}); }
        else if (!meta && !clean.empty()) plain.push_back(clean);
    }
    if (!timed.empty()) {
        std::stable_sort(timed.begin(), timed.end(), [](const Line& a, const Line& b) { return a.ms < b.ms; });
        for (auto& l : timed) {
            l.ms = std::max(0, l.ms - offset);
            if (!out.lines.empty() && out.lines.back().ms == l.ms && !l.text.empty() && !out.lines.back().text.empty() && out.lines.back().translation.empty()) {
                out.lines.back().translation = l.text; continue;
            }
            if (l.text.empty() && (out.lines.empty() || out.lines.back().text.empty())) continue;  // repeated gaps
            out.lines.push_back(l);
        }
        while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
        out.synced = !out.lines.empty();
    } else {
        for (auto& l : plain) out.lines.push_back({-1, l, L""});
    }
    return out;
}

// Credit lines some services put first ("作词 : ...", "Producer : ..."): a short label, a colon and a name, among the
// first lines before 40 s. Removed so the view starts with the song.
inline void DropCredits(Lyrics& l) {
    size_t keep = 0;
    while (keep < l.lines.size() && keep < 12) {
        const auto& t = l.lines[keep].text;
        size_t colon = t.find(L" : ");                                // "作词 : 名字", "Producer : Name"
        if (colon == std::wstring::npos) colon = t.find(L'\xFF1A');     // full-width colon
        const bool credit = l.lines[keep].ms < 40000 && colon != std::wstring::npos && colon > 0 && colon <= 14 && colon + 2 < t.size();
        if (!credit) break;
        ++keep;
    }
    l.lines.erase(l.lines.begin(), l.lines.begin() + std::ptrdiff_t(keep));
    while (!l.lines.empty() && l.lines.front().text.empty()) l.lines.erase(l.lines.begin());
}

// Adds a translation LRC (same time tags) as the lines' translations.
inline void MergeTranslation(Lyrics& l, const Lyrics& t) {
    if (!l.synced || !t.synced) return;
    size_t j = 0;
    for (auto& line : l.lines) {
        while (j < t.lines.size() && t.lines[j].ms < line.ms - 30) ++j;
        if (j < t.lines.size() && std::abs(t.lines[j].ms - line.ms) <= 30 && !t.lines[j].text.empty() && line.translation.empty() && t.lines[j].text != line.text)
            line.translation = t.lines[j].text;
    }
}

}  // namespace nowplaying::lyrics
