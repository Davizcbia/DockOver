#pragma once
// Lyrics model and LRC parsing for Now Playing. Pure functions, unit tested in tests/now_playing_hls_test.cpp.
//   [mm:ss.xx]Line            one or more time tags per line ([mm:ss], [mm:ss.x], [mm:ss.xxx], [mm:ss:xx])
//   [offset:+250]             shifts every line earlier by 250 ms (LRC convention)
//   <mm:ss.xxx>word           word timing (enhanced LRC)
//   [ti:][ar:][al:][by:]...   metadata: ignored
// Two lines with the same time (NetEase, QQ Music exports): the second is the first one's translation.
// Text without any time tag is plain (unsynced) lyrics, one line per text line.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace nowplaying::lyrics {

struct Word {
    size_t offset = 0, length = 0; // UTF-16 range in Line::text on Windows, including joined syllables
    int start = 0, end = -1;        // absolute milliseconds
    bool estimatedEnd = false;
};
struct Line { int ms = -1; std::wstring text, translation; std::vector<Word> words; };
struct Lyrics {
    std::vector<Line> lines;
    bool synced = false;
    std::wstring source;  // "file", "lrclib"
    bool Empty() const { return lines.empty(); }
    bool WordTimed() const {
        return std::any_of(lines.begin(), lines.end(), [](const Line& line) { return !line.words.empty(); });
    }
    // Index of the line being sung at positionMs (-1 before the first line or when unsynced).
    int LineAt(int64_t positionMs) const {
        if (!synced) return -1;
        int found = -1;
        for (size_t i = 0; i < lines.size(); ++i) { if (lines[i].ms <= positionMs) found = int(i); else break; }
        return found;
    }
};

inline float WordProgress(const Word& word, int64_t positionMs) {
    if (positionMs <= word.start) return 0;
    if (positionMs >= word.end) return 1;
    return static_cast<float>(positionMs - word.start) / static_cast<float>(word.end - word.start);
}

inline float SustainedWordGlow(const Word& word, int64_t time) {
    const int duration = word.end - word.start;
    if (word.estimatedEnd || duration < 750 || time <= word.start || time >= int64_t(word.end) + 240) return 0;
    const auto smooth = [](float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); };
    const auto attack = std::clamp(duration * .42f, 280.f, 940.f);
    const float strength = .48f + .52f * smooth((duration - 750) / 1000.f);
    const float settle = 1.f - .22f * smooth((float(time - word.start) / duration - .63f) / .37f);
    return strength * settle * smooth(float(time - word.start) / attack) *
        (1.f - smooth(float(time - word.end) / 240.f));
}

inline std::wstring TrimText(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx", "mm:ss:xx" into ms. -1 when it is not a time.
inline int ParseNumber(std::wstring_view text) {
    if (text.empty()) return -1;
    int n = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || n > (std::numeric_limits<int>::max() - (c - L'0')) / 10) return -1;
        n = n * 10 + (c - L'0');
    }
    return n;
}

inline int ParseTime(std::wstring_view t) {
    size_t colon = t.find(L':'); if (colon == std::wstring::npos || colon == 0) return -1;
    const int minutes = ParseNumber(t.substr(0, colon));
    size_t i = colon + 1, start = i;
    while (i < t.size() && std::iswdigit(t[i])) ++i;
    if (i == start || i - start > 2) return -1;
    const int seconds = ParseNumber(t.substr(start, i - start));
    int ms = 0;
    if (i < t.size()) {
        if (t[i] != L'.' && t[i] != L':') return -1;
        size_t f = ++i; while (i < t.size() && std::iswdigit(t[i])) ++i;
        if (i != t.size() || i == f) return -1;
        const auto frac = t.substr(f, std::min<size_t>(3, i - f));
        ms = ParseNumber(frac);
        if (ms < 0) return -1;
        for (size_t digits = frac.size(); digits < 3; ++digits) ms *= 10;
    }
    if (minutes < 0 || seconds < 0 || seconds > 59) return -1;
    const auto value = int64_t(minutes) * 60000 + seconds * 1000 + ms;
    return value <= std::numeric_limits<int>::max() ? int(value) : -1;
}

inline void AddWord(Line& line, size_t offset, int start, int end) {
    size_t right = line.text.size();
    while (offset < right && std::iswspace(line.text[offset])) ++offset;
    while (right > offset && std::iswspace(line.text[right - 1])) --right;
    if (right > offset && start >= 0 && (end < 0 || end >= start)) {
        if (line.words.empty() || start >= line.words.back().start)
            line.words.push_back({offset, right - offset, start, end});
    }
}

inline void TrimLine(Line& line) {
    size_t left = 0;
    while (left < line.text.size() && std::iswspace(line.text[left])) ++left;
    if (left == line.text.size()) { line.text.clear(); line.words.clear(); return; }
    line.text = TrimText(line.text);
    for (auto& word : line.words) word.offset -= left;
}

inline int ShiftTime(int time, int64_t delta) {
    return static_cast<int>(std::clamp<int64_t>(time + delta, 0, std::numeric_limits<int>::max()));
}

inline void ResolveWordEnds(Lyrics& lyrics) {
    for (size_t i = 0; i < lyrics.lines.size(); ++i) {
        auto& words = lyrics.lines[i].words;
        if (words.empty() || words.back().end >= 0) continue;
        const int start = words.back().start;
        const int cap = ShiftTime(start, 1200);
        const int next = i + 1 < lyrics.lines.size() ? lyrics.lines[i + 1].ms : cap;
        words.back().end = std::clamp(next, start, cap);
        words.back().estimatedEnd = true;
    }
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
        const std::wstring_view body = std::wstring_view(line).substr(i);
        Line parsed;
        size_t part = 0;
        int partStart = times.empty() ? -1 : times.front();
        bool hasWordTags = false;
        for (size_t k = 0; k < body.size(); ++k) {
            if (body[k] == L'<') {
                const size_t close = body.find(L'>', k + 1);
                const int ms = close == std::wstring_view::npos ? -1 : ParseTime(body.substr(k + 1, close - k - 1));
                if (ms >= 0) {
                    AddWord(parsed, part, partStart, ms);
                    part = parsed.text.size();
                    partStart = ms;
                    hasWordTags = true;
                    k = close;
                    continue;
                }
            }
            parsed.text += body[k];
        }
        if (hasWordTags) AddWord(parsed, part, partStart, -1);
        TrimLine(parsed);
        if (!times.empty()) {
            for (int t : times) {
                Line copy = parsed;
                copy.ms = t;
                const int64_t shift = int64_t(t) - times.front();
                for (auto& word : copy.words) {
                    word.start = ShiftTime(word.start, shift);
                    if (word.end >= 0) word.end = ShiftTime(word.end, shift);
                }
                timed.push_back(std::move(copy));
            }
        }
        else if (!meta && !parsed.text.empty()) plain.push_back(std::move(parsed.text));
    }
    if (!timed.empty()) {
        std::stable_sort(timed.begin(), timed.end(), [](const Line& a, const Line& b) { return a.ms < b.ms; });
        for (auto& l : timed) {
            l.ms = ShiftTime(l.ms, -int64_t(offset));
            for (auto& word : l.words) {
                word.start = ShiftTime(word.start, -int64_t(offset));
                if (word.end >= 0) word.end = ShiftTime(word.end, -int64_t(offset));
            }
            if (!out.lines.empty() && out.lines.back().ms == l.ms && !l.text.empty() && !out.lines.back().text.empty() && out.lines.back().translation.empty()) {
                out.lines.back().translation = l.text; continue;
            }
            if (l.text.empty() && (out.lines.empty() || out.lines.back().text.empty())) continue;  // repeated gaps
            out.lines.push_back(l);
        }
        while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
        out.synced = !out.lines.empty();
        ResolveWordEnds(out);
    } else {
        for (auto& l : plain) out.lines.push_back({-1, std::move(l), {}, {}});
    }
    return out;
}

// NetEase YRC uses absolute millisecond starts in both the line and word tags.
inline Lyrics ParseYrc(std::wstring_view input) {
    Lyrics lyrics;
    if (input.starts_with(L'\xFEFF')) input.remove_prefix(1);
    while (!input.empty()) {
        const auto newline = input.find(L'\n');
        auto row = input.substr(0, newline);
        input.remove_prefix(newline == std::wstring_view::npos ? input.size() : newline + 1);
        if (!row.starts_with(L'[')) continue;
        if (row.ends_with(L'\r')) row.remove_suffix(1);
        const auto bracket = row.find(L']');
        const auto comma = row.find(L',', 1);
        if (bracket == std::wstring_view::npos || comma == std::wstring_view::npos || comma >= bracket) continue;
        const int start = ParseNumber(row.substr(1, comma - 1));
        const int duration = ParseNumber(row.substr(comma + 1, bracket - comma - 1));
        if (start < 0 || duration < 0) continue;
        Line line;
        line.ms = start;
        size_t part = 0;
        int wordStart = -1, wordEnd = -1;
        for (size_t i = bracket + 1; i < row.size(); ++i) {
            if (row[i] == L'(') {
                const auto close = row.find(L')', i + 1);
                if (close != std::wstring_view::npos) {
                    const auto fields = row.substr(i + 1, close - i - 1);
                    const auto a = fields.find(L',');
                    const auto b = a == std::wstring_view::npos ? a : fields.find(L',', a + 1);
                    if (b != std::wstring_view::npos) {
                        const int ms = ParseNumber(fields.substr(0, a));
                        const int len = ParseNumber(fields.substr(a + 1, b - a - 1));
                        if (ms >= 0 && len >= 0 && ParseNumber(fields.substr(b + 1)) >= 0) {
                            AddWord(line, part, wordStart, wordEnd);
                            part = line.text.size();
                            wordStart = ms;
                            wordEnd = ShiftTime(ms, len);
                            i = close;
                            continue;
                        }
                    }
                }
            }
            line.text += row[i];
        }
        AddWord(line, part, wordStart, wordEnd);
        TrimLine(line);
        if (!line.text.empty()) lyrics.lines.push_back(std::move(line));
    }
    std::stable_sort(lyrics.lines.begin(), lyrics.lines.end(), [](const Line& a, const Line& b) { return a.ms < b.ms; });
    lyrics.synced = !lyrics.lines.empty();
    return lyrics;
}

inline Lyrics Parse(const std::wstring& text) {
    auto yrc = ParseYrc(text);
    return yrc.WordTimed() ? std::move(yrc) : ParseLrc(text);
}

inline std::wstring FormatLrc(const Lyrics& lyrics) {
    const auto tag = [](int ms, wchar_t open, wchar_t close) {
        wchar_t buffer[32]{};
        std::swprintf(buffer, std::size(buffer), L"%lc%02d:%02d.%03d%lc",
            open, ms / 60000, (ms / 1000) % 60, ms % 1000, close);
        return std::wstring(buffer);
    };
    std::wstring out;
    for (const auto& line : lyrics.lines) {
        if (line.ms < 0) { out += line.text + L'\n'; continue; }
        const auto stamp = tag(line.ms, L'[', L']');
        out += stamp;
        size_t cursor = 0;
        for (const auto& word : line.words) {
            out.append(line.text, cursor, word.offset - cursor);
            out += tag(word.start, L'<', L'>');
            out.append(line.text, word.offset, word.length);
            if (!word.estimatedEnd) out += tag(word.end, L'<', L'>');
            cursor = word.offset + word.length;
        }
        out.append(line.text, cursor);
        out += L'\n';
        if (!line.translation.empty()) out += stamp + line.translation + L'\n';
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
