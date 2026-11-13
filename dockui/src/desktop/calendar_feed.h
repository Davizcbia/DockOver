#pragma once
// Calendar events for the Calendar widget, from iCalendar (.ics) subscriptions or files. Outlook, Google Calendar,
// iCloud and course sites such as Canvas all publish an "iCal / ICS" link; that link (https:// or webcal://) or a
// local .ics path goes on its own line in %LOCALAPPDATA%\DockyDesktop\calendars.txt, optionally followed by a colour
// "#RRGGBB". Feeds are fetched on a worker thread at start-up and every 30 minutes; nothing is uploaded anywhere.
#include <windows.h>
#include <string>
#include <vector>

namespace docky::desk {

struct CalEvent {
    std::wstring title;
    ULONGLONG start = 0, end = 0;  // local FILETIME ticks
    bool allDay = false;
    unsigned color = 0x0A84FF;     // RGB
};

// Pure helpers (unit tested): parse one feed's text into events overlapping [from, to).
std::vector<CalEvent> ParseIcs(const std::string& utf8, unsigned color, ULONGLONG from, ULONGLONG to);
ULONGLONG LocalNow();                                // local FILETIME ticks
ULONGLONG DayStart(ULONGLONG localTicks);            // local midnight of that day
inline constexpr ULONGLONG kDayTicks = 864000000000ull;

class CalendarFeeds {
public:
    void Start(HWND notify, UINT message);   // message is posted when the event list changed
    void Stop();
    void Refresh();                          // fetch now (also after calendars.txt changed)
    std::vector<CalEvent> Events() const;    // sorted by start, today - 1 .. today + 31
    bool Configured() const;
    static std::wstring ConfigPath();
};
CalendarFeeds& Calendars();
std::wstring& DevCalendar();  // development (--dev-calendar <file.ics>): use this feed instead of calendars.txt

}  // namespace docky::desk
