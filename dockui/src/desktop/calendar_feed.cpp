#include "calendar_feed.h"
#include "canvas.h"
#include <urlmon.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace docky::desk {
namespace {
ULONGLONG Ticks(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
FILETIME Ft(ULONGLONG t) { return FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)}; }
ULONGLONG FromSystem(const SYSTEMTIME& st) { FILETIME f{}; SystemTimeToFileTime(&st, &f); return Ticks(f); }
SYSTEMTIME ToSystem(ULONGLONG t) { SYSTEMTIME st{}; const FILETIME f = Ft(t); FileTimeToSystemTime(&f, &st); return st; }

// "20261113", "20261113T101000", "20261113T151000Z" -> local ticks.
bool ParseWhen(const std::string& value, bool& allDay, ULONGLONG& out) {
    if (value.size() < 8) return false;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(atoi(value.substr(0, 4).c_str())); st.wMonth = static_cast<WORD>(atoi(value.substr(4, 2).c_str())); st.wDay = static_cast<WORD>(atoi(value.substr(6, 2).c_str()));
    if (!st.wYear || st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || st.wDay > 31) return false;
    allDay = value.size() < 15 || value[8] != 'T';
    if (!allDay) { st.wHour = static_cast<WORD>(atoi(value.substr(9, 2).c_str())); st.wMinute = static_cast<WORD>(atoi(value.substr(11, 2).c_str())); st.wSecond = static_cast<WORD>(atoi(value.substr(13, 2).c_str())); }
    if (!allDay && value.back() == 'Z') {  // UTC: convert with the DST rules of that date
        SYSTEMTIME local{};
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) return false;
        st = local;
    }
    // TZID times are taken as local wall-clock time (feeds normally use the reader's own zone).
    out = FromSystem(st);
    return true;
}
std::string Unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; o += (c == 'n' || c == 'N') ? ' ' : c; }
        else o += s[i];
    }
    return o;
}
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Param(const std::string& rule, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while ((at = rule.find(k, at)) != std::string::npos) {
        if (at == 0 || rule[at - 1] == ';') { const size_t end = rule.find(';', at); return rule.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size()); }
        at += k.size();
    }
    return {};
}
ULONGLONG AddMonths(ULONGLONG t, int months) {
    SYSTEMTIME st = ToSystem(t);
    int m = st.wMonth - 1 + months;
    st.wYear = static_cast<WORD>(st.wYear + m / 12); st.wMonth = static_cast<WORD>(m % 12 + 1);
    FILETIME f{};
    return SystemTimeToFileTime(&st, &f) ? Ticks(f) : 0;  // invalid dates (Feb 30) are skipped
}
}  // namespace

ULONGLONG LocalNow() { SYSTEMTIME st{}; GetLocalTime(&st); return FromSystem(st); }
ULONGLONG DayStart(ULONGLONG t) { return t - t % kDayTicks; }

std::vector<CalEvent> ParseIcs(const std::string& text, unsigned color, ULONGLONG from, ULONGLONG to) {
    // Unfold continuation lines (a line starting with a space or tab continues the previous one).
    std::vector<std::string> lines; std::string cur;
    std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) { cur += line.substr(1); continue; }
        if (!cur.empty()) lines.push_back(cur);
        cur = line;
    }
    if (!cur.empty()) lines.push_back(cur);
    std::vector<CalEvent> out;
    bool inEvent = false; std::string summary, dtstart, dtend, rrule;
    for (const auto& l : lines) {
        if (l == "BEGIN:VEVENT") { inEvent = true; summary.clear(); dtstart.clear(); dtend.clear(); rrule.clear(); continue; }
        if (!inEvent) continue;
        if (l == "END:VEVENT") {
            inEvent = false;
            CalEvent e; e.color = color; e.title = Wide(Unescape(summary));
            bool allDay = false, endAllDay = false; ULONGLONG start = 0, end = 0;
            if (!ParseWhen(dtstart, allDay, start)) continue;
            e.allDay = allDay;
            if (dtend.empty() || !ParseWhen(dtend, endAllDay, end) || end < start) end = start + (allDay ? kDayTicks : 0);
            const ULONGLONG length = end - start;
            auto add = [&](ULONGLONG s) { if (s < to && s + (std::max)(length, 1ull) > from && out.size() < 4000) { CalEvent x = e; x.start = s; x.end = s + length; out.push_back(std::move(x)); } };
            if (rrule.empty()) { add(start); continue; }
            const std::string freq = Param(rrule, "FREQ");
            const int interval = (std::max)(1, atoi(Param(rrule, "INTERVAL").c_str()));
            const int count = atoi(Param(rrule, "COUNT").c_str());
            ULONGLONG until = to; { bool u = false; ULONGLONG t = 0; const std::string v = Param(rrule, "UNTIL"); if (!v.empty() && ParseWhen(v, u, t)) until = (std::min)(until, t + (u ? kDayTicks : 0)); }
            std::vector<int> byDay;  // 0 = Sunday
            { const std::string v = Param(rrule, "BYDAY"); static const char* names[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
              for (int d = 0; d < 7; ++d) if (v.find(names[d]) != std::string::npos) byDay.push_back(d); }
            int produced = 0;
            for (int step = 0; step < 3000; ++step) {
                ULONGLONG base = start;
                if (freq == "DAILY") base = start + static_cast<ULONGLONG>(step) * interval * kDayTicks;
                else if (freq == "WEEKLY") base = start + static_cast<ULONGLONG>(step) * interval * 7 * kDayTicks;
                else if (freq == "MONTHLY") base = AddMonths(start, step * interval);
                else if (freq == "YEARLY") base = AddMonths(start, step * interval * 12);
                else { add(start); break; }
                if (!base) continue;
                if (base > until) break;
                if (freq == "WEEKLY" && !byDay.empty()) {
                    const int startDow = ToSystem(start).wDayOfWeek;
                    const ULONGLONG weekStart = base - static_cast<ULONGLONG>(startDow) * kDayTicks;
                    for (int d : byDay) {
                        const ULONGLONG s = weekStart + static_cast<ULONGLONG>(d) * kDayTicks;
                        if (s < start || s > until) continue;
                        if (count && produced >= count) break;
                        ++produced; add(s);
                    }
                } else { if (count && produced >= count) break; ++produced; add(base); }
                if (count && produced >= count) break;
                if (base > to) break;
            }
            continue;
        }
        const size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = l.substr(0, l.find_first_of(";:")), value = l.substr(colon + 1);
        if (name == "SUMMARY") summary = value;
        else if (name == "DTSTART") dtstart = value;
        else if (name == "DTEND") dtend = value;
        else if (name == "RRULE") rrule = value;
    }
    std::sort(out.begin(), out.end(), [](const CalEvent& a, const CalEvent& b) { return a.start != b.start ? a.start < b.start : a.allDay > b.allDay; });
    return out;
}

namespace {
struct Shared {
    mutable std::mutex mutex; std::vector<CalEvent> events; bool configured = false;
    HWND notify = nullptr; UINT message = 0; HANDLE wake = nullptr; std::atomic<bool> stop{false}; std::thread thread;
} g;
const unsigned kPalette[6] = {0x0A84FF, 0x30D158, 0xBF5AF2, 0xFF9F0A, 0xFF375F, 0x64D2FF};

std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); std::ostringstream s; s << f.rdbuf(); std::string out = s.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF) out.erase(0, 3);
    return out;
}
void Fetch() {
    std::vector<CalEvent> all; bool configured = false;
    std::wistringstream lines(DevCalendar().empty() ? Wide(ReadFileUtf8(CalendarFeeds::ConfigPath())) : DevCalendar());
    std::wstring line; int index = 0;
    const ULONGLONG today = DayStart(LocalNow()), from = today - kDayTicks, to = today + 32 * kDayTicks;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        while (!line.empty() && line.front() == L' ') line.erase(0, 1);
        if (line.empty() || line[0] == L'#') continue;
        unsigned color = kPalette[index++ % 6];
        if (line.size() > 8 && line[line.size() - 7] == L'#' && line[line.size() - 8] == L' ') {
            color = static_cast<unsigned>(wcstoul(line.c_str() + line.size() - 6, nullptr, 16)); line.resize(line.size() - 8);
        }
        configured = true;
        std::wstring source = line;
        if (source.rfind(L"webcal://", 0) == 0) source = L"https://" + source.substr(9);
        std::string text;
        if (source.rfind(L"http://", 0) == 0 || source.rfind(L"https://", 0) == 0) {
            wchar_t temp[MAX_PATH]{}, file[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp); GetTempFileNameW(temp, L"dkc", 0, file);
            if (SUCCEEDED(URLDownloadToFileW(nullptr, source.c_str(), file, 0, nullptr))) text = ReadFileUtf8(file);
            DeleteFileW(file);
        } else text = ReadFileUtf8(source);
        auto events = ParseIcs(text, color, from, to);
        all.insert(all.end(), events.begin(), events.end());
    }
    std::sort(all.begin(), all.end(), [](const CalEvent& a, const CalEvent& b) { return a.start != b.start ? a.start < b.start : a.allDay > b.allDay; });
    { std::lock_guard<std::mutex> l(g.mutex); g.events = std::move(all); g.configured = configured; }
    if (g.notify) PostMessageW(g.notify, g.message, 0, 0);
}
}  // namespace

std::wstring& DevCalendar() { static std::wstring v; return v; }
std::wstring CalendarFeeds::ConfigPath() { return DataDir() + L"\\calendars.txt"; }

void CalendarFeeds::Start(HWND notify, UINT message) {
    if (g.thread.joinable()) return;
    g.notify = notify; g.message = message; g.stop = false;
    g.wake = CreateEventW(nullptr, FALSE, TRUE, nullptr);  // signalled: fetch at once
    const std::wstring config = ConfigPath();
    if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES) {  // a commented template the user can fill in
        CreateDirectoryW(DataDir().c_str(), nullptr);
        std::ofstream f(config, std::ios::binary);
        f << "# DockOver Desktop calendars: one iCalendar (.ics) link or file per line, optionally followed by a colour.\r\n"
             "# Outlook: Settings > Calendar > Shared calendars > Publish a calendar > ICS link.\r\n"
             "# Google Calendar: Settings > your calendar > Secret address in iCal format.\r\n"
             "# Example:  https://example.com/calendar.ics #30D158\r\n";
    }
    g.thread = std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        while (!g.stop) {
            if (WaitForSingleObject(g.wake, 30 * 60 * 1000) == WAIT_FAILED || g.stop) break;
            Fetch();
        }
        CoUninitialize();
    });
}
void CalendarFeeds::Stop() { g.stop = true; if (g.wake) SetEvent(g.wake); if (g.thread.joinable()) g.thread.join(); if (g.wake) { CloseHandle(g.wake); g.wake = nullptr; } }
void CalendarFeeds::Refresh() { if (g.wake) SetEvent(g.wake); }
std::vector<CalEvent> CalendarFeeds::Events() const { std::lock_guard<std::mutex> l(g.mutex); return g.events; }
bool CalendarFeeds::Configured() const { std::lock_guard<std::mutex> l(g.mutex); return g.configured; }
CalendarFeeds& Calendars() { static CalendarFeeds c; return c; }

}  // namespace docky::desk
