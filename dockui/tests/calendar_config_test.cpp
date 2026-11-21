#include <windows.h>
#include "desktop/calendar_config.h"
#include <cstdio>

using namespace docky::desk::calconfig;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main() {
    const std::wstring file =
        L"\xFEFF# DockOver Desktop calendars: one link per line.\r\n# Example:  https://example.com/calendar.ics #30D158\r\n\r\n"
        L"https://lms.example.edu/feed/abc123/learn.ics\r\n  webcal://p01.example.com/x.ics #ff375f  \r\n# later note\r\nC:\\cal\\work.ics\r\n";
    std::wstring text = file; if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    Config c = Parse(text);
    CHECK(c.comments.size() == 2);
    CHECK(c.entries.size() == 3);
    CHECK(c.entries[0].source == L"https://lms.example.edu/feed/abc123/learn.ics" && !c.entries[0].hasColour);
    CHECK(c.entries[1].source == L"webcal://p01.example.com/x.ics" && c.entries[1].hasColour && c.entries[1].colour == 0xFF375F);
    CHECK(c.entries[2].source == L"C:\\cal\\work.ics");

    // Colours: defaults by position, then cycling through the palette.
    CHECK(ColourOf(c, 0) == kPalette[0]);
    CHECK(ColourOf(c, 2) == kPalette[2]);
    CycleColour(c, 0); CHECK(c.entries[0].hasColour && c.entries[0].colour == kPalette[1]);
    CycleColour(c, 1); CHECK(c.entries[1].colour == kPalette[5]);
    c.entries[2].hasColour = true; c.entries[2].colour = 0x123456; CycleColour(c, 2); CHECK(c.entries[2].colour == kPalette[0]);  // unknown -> first

    // Serialize keeps the help text and round-trips.
    const std::wstring out = Serialize(c);
    CHECK(out.rfind(L"# DockOver Desktop calendars", 0) == 0);
    CHECK(out.find(L"# later note") == std::wstring::npos);
    const Config back = Parse(out);
    CHECK(back.entries.size() == 3 && back.entries[0].colour == kPalette[1] && back.comments.size() == 2);

    // Adding.
    auto none = [](const std::wstring&) { return false; };
    auto all = [](const std::wstring&) { return true; };
    CHECK(Add(c, L"   ", none) == AddResult::Empty);
    CHECK(Add(c, L"hello world", none) == AddResult::NotCalendar);
    CHECK(Add(c, L"D:\\missing.ics", none) == AddResult::NotCalendar);
    CHECK(Add(c, L"https://a.example/x.ics https://b.example/y.ics", none) == AddResult::NotCalendar);
    CHECK(Add(c, L"WEBCAL://LMS.example.edu/feed/abc123/learn.ics/", none) == AddResult::Duplicate);
    CHECK(Add(c, L"\"D:\\Cal\\Home.ICS\"", all) == AddResult::Added && c.entries.back().source == L"D:\\Cal\\Home.ICS");
    CHECK(Add(c, L"https://calendar.example.com/ical/secret/basic.ics", none) == AddResult::Added);
    CHECK(c.entries.size() == 5);
    Remove(c, 0); CHECK(c.entries.size() == 4 && c.entries[0].source.rfind(L"webcal://", 0) == 0);
    Remove(c, 99); CHECK(c.entries.size() == 4);

    // Names never show the secret part of a link.
    CHECK(ShortName(L"https://lms.example.edu/feed/abc123/learn.ics") == L"lms.example.edu");
    CHECK(ShortName(L"webcal://user:pw@WWW.Example.com?x=1") == L"example.com");
    CHECK(ShortName(L"C:\\cal\\Work Shifts.ics") == L"Work Shifts");

    // Empty file and a file with only the template.
    CHECK(Parse(L"").entries.empty());
    CHECK(Parse(L"# a\r\n# b\r\n").comments.size() == 2);

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("calendar_config: all checks passed\n");
    return 0;
}
