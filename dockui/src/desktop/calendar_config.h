#pragma once
// calendars.txt as data, for the Calendar settings page (pure, unit tested in tests/calendar_config_test.cpp).
// The file stays the source of truth and stays hand-editable: one iCalendar link or .ics path per line, optionally
// followed by " #RRGGBB"; lines starting with # are comments and are kept as they are when the page saves.
#include <cwctype>
#include <string>
#include <vector>

namespace docky::desk::calconfig {

// The first six are the colours given, in order, to calendars without one (calendar_feed.cpp uses the same order);
// the colour dot on the settings page steps through all eight (Apple Calendar's palette).
inline constexpr unsigned kPalette[8] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF9F0A, 0xFF375F, 0x64D2FF, 0xFFD60A, 0xAC8E68};
inline constexpr int kDefaultColours = 6;

struct Entry {
    std::wstring source;       // https://, webcal:// or a file path, as written
    bool hasColour = false;
    unsigned colour = 0;       // RGB; meaningful when hasColour
};
struct Config {
    std::vector<std::wstring> comments;  // the leading # lines (the file's own help text), kept on save
    std::vector<Entry> entries;
};

inline std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || std::iswspace(s.back()))) s.pop_back();
    size_t i = 0; while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}
inline bool IsHex6(const std::wstring& s, size_t at) {
    if (at + 6 > s.size()) return false;
    for (size_t i = at; i < at + 6; ++i) if (!std::iswxdigit(s[i])) return false;
    return true;
}
// One non-comment line -> entry (the same rule as calendar_feed.cpp: " #RRGGBB" at the very end is the colour).
inline Entry ParseLine(std::wstring line) {
    Entry e; line = Trim(line);
    if (line.size() > 8 && line[line.size() - 7] == L'#' && line[line.size() - 8] == L' ' && IsHex6(line, line.size() - 6)) {
        e.hasColour = true; e.colour = static_cast<unsigned>(std::wcstoul(line.c_str() + line.size() - 6, nullptr, 16));
        line = Trim(line.substr(0, line.size() - 8));
    }
    e.source = line;
    return e;
}
inline Config Parse(const std::wstring& text) {
    Config c; bool body = false; size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start); if (end == std::wstring::npos) end = text.size();
        const std::wstring line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        if (line[0] == L'#') { if (!body) c.comments.push_back(line); continue; }  // later comments are dropped on save
        body = true;
        Entry e = ParseLine(line); if (!e.source.empty()) c.entries.push_back(e);
    }
    return c;
}
inline std::wstring Hex(unsigned rgb) { wchar_t b[8]; swprintf_s(b, L"%06X", rgb & 0xFFFFFF); return b; }
inline std::wstring Serialize(const Config& c) {
    std::wstring out;
    for (const auto& l : c.comments) out += l + L"\r\n";
    if (!c.comments.empty()) out += L"\r\n";
    for (const auto& e : c.entries) out += e.source + (e.hasColour ? L" #" + Hex(e.colour) : L"") + L"\r\n";
    return out;
}
// The colour a calendar is drawn in: its own, or the default for its position (as calendar_feed.cpp assigns them).
inline unsigned ColourOf(const Config& c, size_t index) {
    const Entry& e = c.entries[index];
    return e.hasColour ? e.colour : kPalette[index % kDefaultColours];
}
// The next colour in the palette after the one it shows now (the colour dot was clicked).
inline void CycleColour(Config& c, size_t index) {
    const unsigned now = ColourOf(c, index); int at = -1;
    for (int i = 0; i < 8; ++i) if (kPalette[i] == now) at = i;
    c.entries[index].hasColour = true; c.entries[index].colour = kPalette[(at + 1) % 8];
}

inline std::wstring Lower(std::wstring s) { for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch)); return s; }
inline bool IsLink(const std::wstring& s) {
    const std::wstring l = Lower(s);
    return l.rfind(L"https://", 0) == 0 || l.rfind(L"http://", 0) == 0 || l.rfind(L"webcal://", 0) == 0 || l.rfind(L"webcals://", 0) == 0;
}
// The same calendar written two ways (webcal:// vs https://, trailing slash, letter case of the scheme and host).
inline std::wstring Canonical(const std::wstring& source) {
    std::wstring s = Trim(source);
    if (IsLink(s)) {
        const size_t scheme = s.find(L"://"); std::wstring rest = s.substr(scheme + 3);
        const size_t slash = rest.find(L'/');
        rest = Lower(rest.substr(0, slash)) + (slash == std::wstring::npos ? L"" : rest.substr(slash));
        while (!rest.empty() && rest.back() == L'/') rest.pop_back();
        return L"https://" + rest;
    }
    return Lower(s);
}
inline int Find(const Config& c, const std::wstring& source) {
    const std::wstring k = Canonical(source);
    for (size_t i = 0; i < c.entries.size(); ++i) if (Canonical(c.entries[i].source) == k) return static_cast<int>(i);
    return -1;
}

enum class AddResult { Added, Empty, NotCalendar, Duplicate };
// Text pasted or typed into "Add Calendar". Accepts links (http, https, webcal) and .ics file paths (quotes from
// "Copy as path" are removed). `fileExists` is only asked about paths.
template <class Exists>
AddResult Add(Config& c, std::wstring text, Exists fileExists) {
    text = Trim(text);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') text = Trim(text.substr(1, text.size() - 2));
    if (text.empty()) return AddResult::Empty;
    if (text.find_first_of(L" \t") != std::wstring::npos && IsLink(text)) return AddResult::NotCalendar;  // two things pasted
    if (!IsLink(text) && !(Lower(text).size() > 4 && Lower(text).substr(text.size() - 4) == L".ics" && fileExists(text))) return AddResult::NotCalendar;
    if (Find(c, text) >= 0) return AddResult::Duplicate;
    c.entries.push_back(Entry{text, false, 0});
    return AddResult::Added;
}
inline void Remove(Config& c, size_t index) { if (index < c.entries.size()) c.entries.erase(c.entries.begin() + static_cast<std::ptrdiff_t>(index)); }

// What the settings page shows for a calendar it has no name for yet: the host of a link (never its secret path)
// or the file name of a local .ics.
inline std::wstring ShortName(const std::wstring& source) {
    if (IsLink(source)) {
        std::wstring rest = source.substr(source.find(L"://") + 3);
        rest = rest.substr(0, rest.find_first_of(L"/?#"));
        const size_t at = rest.rfind(L'@'); if (at != std::wstring::npos) rest = rest.substr(at + 1);  // user:pass@host
        if (Lower(rest).rfind(L"www.", 0) == 0) rest = rest.substr(4);
        return Lower(rest);
    }
    const size_t slash = source.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? source : source.substr(slash + 1);
    if (Lower(name).size() > 4 && Lower(name).substr(name.size() - 4) == L".ics") name.resize(name.size() - 4);
    return name;
}

}  // namespace docky::desk::calconfig
