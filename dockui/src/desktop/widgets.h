#pragma once
// docky Desktop widgets: built-in kinds and the template renderer used by third-party widgets.
#include "canvas.h"
#include "../json_lite.h"
#include <string>
#include <vector>

namespace docky::desk {

enum class Size { Small, Medium, Large };
D2D1_SIZE_F SizeDip(Size size);
const char* SizeName(Size size);
Size ParseSize(const std::string& name);

// A third-party widget (protocol "widget.set" or an extension manifest "widgets" entry).
// A setting a third-party widget declares ("settings" in widget.set or its manifest); shown on the widget's own page.
struct ExtSetting {
    std::string key;              // reported back as widget.setting {key, value}
    std::wstring title;
    std::string type = "toggle";  // "toggle" (value 0/1), "choice" (value = option index), "button" (options[0] = label)
    std::vector<std::wstring> options;
    int value = 0;                // the widget's default / current value
};
struct ExtWidget {
    std::wstring id, title, templ = L"text";
    dock::json::Value data = dock::json::Value::MakeObject();
    Size size = Size::Small;
    bool hasAccent = false; D2D1_COLOR_F accent{};
    HWND notify = nullptr;
    // Manifest data source: a program whose standard output is the template data (JSON object).
    std::wstring command, folder; int refreshSeconds = 0; ULONGLONG lastRun = 0; bool running = false;
    bool fromManifest = false;
    std::vector<ExtSetting> settings;
    std::vector<std::uint32_t> image; UINT imageW = 0, imageH = 0; std::wstring imagePath;  // "image" template cache
};
bool ParseWidgetMessage(const std::string& utf8, ExtWidget& out, std::string& type, std::string& error);
std::vector<ExtWidget> LoadWidgetManifests(const std::wstring& root);

struct Kind { std::wstring id, name; Size defaultSize; std::wstring group; };  // group: the gallery sidebar entry
std::vector<Kind> BuiltinKinds();  // clock (digital), clock.analog, calendar, battery, system
PlateTone PlateToneOf(const std::wstring& kind);  // the digital Clock: white, or black (Prefs::clockDark)
IDWriteTextFormat* IconFont(float size);  // Segoe Fluent Icons / MDL2 Assets glyphs  // solid style: drawn on a light plate (the digital clock)

// Paints one widget body (glass is painted by the caller) into `r` (DIP).
void PaintWidget(ID2D1RenderTarget* rt, const std::wstring& kind, Size size, D2D1_RECT_F r, const Theme& theme, const ExtWidget* ext);
// Row under a point for list templates (-1 none), used for widget.clicked events.
int ListRowAt(const ExtWidget& ext, Size size, D2D1_RECT_F r, D2D1_POINT_2F p);
UINT RefreshMs(const std::wstring& kind);
void SampleSystem();  // CPU / memory sampler for the System widget (call every ~2 s)
// Scrolling built-in widgets (Calendar, Weather). The host sets `offset` (DIP, 0 = the normal view) before PaintWidget;
// the painter clamps to and reports the range it can scroll (`min` .. `max`) for the wheel and the spring.
struct WidgetScroll { float offset = 0, min = 0, max = 0; };
WidgetScroll& PaintScroll();
bool Scrollable(const std::wstring& kind);
// What a click on a built-in widget opens (Clock app, Calendar, battery settings, Task Manager).
void OpenBuiltin(const std::wstring& kind);

}  // namespace docky::desk
