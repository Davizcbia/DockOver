#include "widgets.h"
#include "../ui_language.h"
#include "calendar_feed.h"
#include "devices.h"
#include "weather.h"
#include "../pairing/widget_queue.h"
#include <map>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace docky::desk {
namespace {
using dock::json::Value;
constexpr float kPi = 3.14159265358979f;
constexpr float kPad = 16.f;  // inner padding (design_tokens space::L)
D2D1_COLOR_F Hex(unsigned rgb, float a = 1.f) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
const D2D1_COLOR_F kRed = Hex(0xFF453A), kOrange = Hex(0xFF9F0A), kGreen = Hex(0x30D158), kYellow = Hex(0xFFD60A), kBlue = Hex(0x0A84FF);
std::wstring W(const std::string& s) { return dock::json::FromUtf8(s); }
std::wstring Str(const Value& o, const char* key) { const Value* v = o.Find(key); return v && v->type == Value::Type::String ? W(v->string) : std::wstring(); }
double Num(const Value& o, const char* key, double fallback) { const Value* v = o.Find(key); return v && v->type == Value::Type::Number ? v->number : fallback; }

ComPtr<ID2D1SolidColorBrush> Brush(ID2D1RenderTarget* rt, D2D1_COLOR_F c) { ComPtr<ID2D1SolidColorBrush> b; rt->CreateSolidColorBrush(c, &b); return b; }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

void Ring(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float radius, float stroke, float value, D2D1_COLOR_F color, const Theme& t) {
    auto track = Brush(rt, t.dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.09f));
    rt->DrawEllipse(D2D1::Ellipse(c, radius, radius), track.Get(), stroke);
    value = std::clamp(value, 0.f, 1.f);
    if (value <= 0.001f) return;
    ComPtr<ID2D1StrokeStyle> round;
    FactoryOf(rt)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    auto ink = Brush(rt, color);
    if (value >= 0.999f) { rt->DrawEllipse(D2D1::Ellipse(c, radius, radius), ink.Get(), stroke); return; }
    ComPtr<ID2D1PathGeometry> arc; ComPtr<ID2D1GeometrySink> sink;
    FactoryOf(rt)->CreatePathGeometry(&arc); arc->Open(&sink);
    const float a0 = -kPi / 2, a1 = a0 + value * 2 * kPi;
    sink->BeginFigure(D2D1::Point2F(c.x + radius * std::cos(a0), c.y + radius * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + radius * std::cos(a1), c.y + radius * std::sin(a1)), D2D1::SizeF(radius, radius), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, value > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close();
    rt->DrawGeometry(arc.Get(), ink.Get(), stroke, round.Get());
}

void Caption(ID2D1RenderTarget* rt, const std::wstring& text, D2D1_RECT_F r, D2D1_COLOR_F color) {
    Text(rt, text, Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + kPad, r.top + 12, r.right - kPad, r.top + 30), color);
}

// ---- Clock ---------------------------------------------------------------------------------------------------
std::wstring FormatTime(const SYSTEMTIME& t, const wchar_t* format) { wchar_t b[64]{}; GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, format, b, 64); return b; }
std::wstring FormatDate(const SYSTEMTIME& t, const wchar_t* format) { wchar_t b[96]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, format, b, 96, nullptr); return b; }
std::wstring Upper(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towupper(c)); return s; }

// Condensed display face for the digital clock (Bahnschrift ships with Windows 10/11; Segoe UI Variable otherwise).
IDWriteTextFormat* Display(float size) {
    static std::map<int, ComPtr<IDWriteTextFormat>> cache;
    auto& f = cache[static_cast<int>(size * 4)];
    if (!f) {
        if (FAILED(DWrite()->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, size, L"", &f)))
            DWrite()->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    }
    return f.Get();
}
// Glyphs from the system icon font (Segoe Fluent Icons on Windows 11, MDL2 Assets on 10).
IDWriteTextFormat* Glyphs(float size) {
    static std::map<int, ComPtr<IDWriteTextFormat>> cache;
    auto& f = cache[static_cast<int>(size * 4)];
    if (!f) {
        ComPtr<IDWriteFontCollection> fonts; UINT32 index = 0; BOOL exists = FALSE;
        DWrite()->GetSystemFontCollection(&fonts);
        const wchar_t* family = fonts && SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        DWrite()->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    }
    return f.Get();
}

// Digital clock (the macOS 26 desktop clock): large condensed time on a light plate, framed by 60 second ticks that
// darken as the minute passes.
void PaintDigital(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    const float w = r.right - r.left, h = r.bottom - r.top, inset = 11.f, tick = 7.f;
    // Three parallel curves: the plate's outline, the ticks' outer ends and their inner ends. Both tick ends are true
    // offsets of the plate outline itself: from each outline point along its inward normal by `inset` and by
    // `inset + tick`, so every tick is `tick` long, points along the plate's normal, and the ring's two edges keep the
    // plate's corner exactly (concentric), not a resized copy of it. Ticks are spaced evenly along the outer curve.
    // The ring's base is a rounded rectangle with circular corners of the plate's apparent radius (kRingRadius): the
    // plate's superellipse corner, offset inwards, pinches into a near point at the diagonal, while the macOS clock's
    // ring turns its corners on a clear circular arc (measured on the user's reference: ring radius about 16 pt at an
    // 11 pt margin), enlarged to 25 pt so the inner ends (18 pt, concentric) still turn a visible arc rather than a point.
    constexpr float kRingRadius = 36.f;
    std::vector<D2D1_POINT_2F> plate;
    {
        const float rr = (std::min)({kRingRadius, w / 2, h / 2});
        // clockwise from the top-right corner, like WidgetOutline: top-right, bottom-right, bottom-left, top-left
        const struct { float cx, cy, a0; } arcs[4] = {{r.right - rr, r.top + rr, -1.5707963f}, {r.right - rr, r.bottom - rr, 0.f},
                                                      {r.left + rr, r.bottom - rr, 1.5707963f}, {r.left + rr, r.top + rr, 3.1415927f}};
        for (const auto& a : arcs)
            for (int i = 0; i <= 24; ++i) { const float ang = a.a0 + 1.5707963f * i / 24; plate.push_back(D2D1::Point2F(a.cx + rr * std::cos(ang), a.cy + rr * std::sin(ang))); }
    }
    const size_t count = plate.size();
    std::vector<D2D1_POINT_2F> normal(count), outer(count);
    {
        std::vector<D2D1_POINT_2F> seg(count);  // inward normal of segment i (plate[i] -> plate[i+1]); clockwise, y down
        for (size_t i = 0; i < count; ++i) {
            const auto& p0 = plate[i]; const auto& p1 = plate[(i + 1) % count];
            const float len = (std::max)(1e-4f, std::hypot(p1.x - p0.x, p1.y - p0.y));
            seg[i] = D2D1::Point2F(-(p1.y - p0.y) / len, (p1.x - p0.x) / len);
        }
        for (size_t i = 0; i < count; ++i) {
            const auto& s0 = seg[(i + count - 1) % count]; const auto& s1 = seg[i];
            const float nx = s0.x + s1.x, ny = s0.y + s1.y, len = (std::max)(1e-4f, std::hypot(nx, ny));
            normal[i] = D2D1::Point2F(nx / len, ny / len);
            outer[i] = D2D1::Point2F(plate[i].x + normal[i].x * inset, plate[i].y + normal[i].y * inset);
        }
    }
    std::vector<float> run(count + 1, 0.f);
    for (size_t i = 0; i < count; ++i) { const auto& p0 = outer[i]; const auto& p1 = outer[(i + 1) % count]; run[i + 1] = run[i] + std::hypot(p1.x - p0.x, p1.y - p0.y); }
    const float perimeter = run.back();
    const float topCentre = [&] {  // the outline starts at the top-right corner; find where it crosses the top centre
        const float cx = (r.left + r.right) / 2;
        for (size_t i = 0; i < count; ++i) { const auto& p0 = outer[i]; const auto& p1 = outer[(i + 1) % count];
            if (std::fabs(p0.y - p1.y) < 0.01f && p0.y < (r.top + r.bottom) / 2 && ((p0.x - cx) * (p1.x - cx) <= 0)) return run[i] + std::fabs(cx - p0.x); }
        return 0.f; }();
    auto pointAt = [&](float d, D2D1_POINT_2F& p, D2D1_POINT_2F& n) {  // d from top centre, clockwise; n = inward normal
        d = std::fmod(d + topCentre + perimeter, perimeter);
        size_t i = static_cast<size_t>(std::upper_bound(run.begin(), run.end(), d) - run.begin());
        i = i == 0 ? 0 : i - 1; if (i >= count) i = count - 1;
        const size_t j = (i + 1) % count;
        const float u = (d - run[i]) / (std::max)(1e-4f, run[i + 1] - run[i]);
        p = D2D1::Point2F(outer[i].x + (outer[j].x - outer[i].x) * u, outer[i].y + (outer[j].y - outer[i].y) * u);
        const float nx = normal[i].x + (normal[j].x - normal[i].x) * u, ny = normal[i].y + (normal[j].y - normal[i].y) * u, len = (std::max)(1e-4f, std::hypot(nx, ny));
        n = D2D1::Point2F(nx / len, ny / len);
    };
    auto brush = Brush(rt, t.ink);
    ComPtr<ID2D1StrokeStyle> round;
    FactoryOf(rt)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    // Every tick points at the centre of the plate (the macOS desktop clock): upright at 12 and 6, level at 3 and 9,
    // and leaning more and more toward the corners on either side, 45 degrees in the corners themselves. Same width
    // for all sixty.
    const D2D1_POINT_2F centre = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    // Outer ends in the corners: the ring's corner arc is rounder than the plate's corner, so there a tick is extended
    // outwards (its inner end stays) until it meets the true `inset` offset of the plate outline itself; along the
    // straight edges the two curves coincide and nothing changes. The outer edge of the ring then follows the plate's
    // corner at an even margin while the inner edge keeps its circular arc.
    std::vector<D2D1_POINT_2F> rim;
    {
        const auto edge = WidgetOutline(r, 0.f, 64);
        const size_t m = edge.size();
        std::vector<D2D1_POINT_2F> seg(m);
        for (size_t k = 0; k < m; ++k) {
            const auto& p0 = edge[k]; const auto& p1 = edge[(k + 1) % m];
            const float len = (std::max)(1e-4f, std::hypot(p1.x - p0.x, p1.y - p0.y));
            seg[k] = D2D1::Point2F(-(p1.y - p0.y) / len, (p1.x - p0.x) / len);
        }
        for (size_t k = 0; k < m; ++k) {
            const auto& s0 = seg[(k + m - 1) % m]; const auto& s1 = seg[k];
            const float nx = s0.x + s1.x, ny = s0.y + s1.y, len = (std::max)(1e-4f, std::hypot(nx, ny));
            rim.push_back(D2D1::Point2F(edge[k].x + nx / len * inset, edge[k].y + ny / len * inset));
        }
    }
    auto reachOut = [&](D2D1_POINT_2F q, D2D1_POINT_2F out) {  // distance from q along `out` to the rim curve
        float best = -1.f;
        for (size_t k = 0; k < rim.size(); ++k) {
            const auto& a = rim[k]; const auto& b = rim[(k + 1) % rim.size()];
            const float ex = b.x - a.x, ey = b.y - a.y, den = out.x * ey - out.y * ex;
            if (std::fabs(den) < 1e-6f) continue;
            const float t = ((a.x - q.x) * ey - (a.y - q.y) * ex) / den, u = ((a.x - q.x) * out.y - (a.y - q.y) * out.x) / den;
            if (t > 0.f && u >= 0.f && u <= 1.f && (best < 0.f || t < best)) best = t;
        }
        return best;
    };
    for (int i = 0; i < 60; ++i) {
        D2D1_POINT_2F p{}, n{};
        pointAt(perimeter * i / 60.f, p, n);
        const float dx = centre.x - p.x, dy = centre.y - p.y, len = (std::max)(1e-4f, std::hypot(dx, dy));
        const D2D1_POINT_2F dir = D2D1::Point2F(dx / len, dy / len);
        // The band stays `tick` deep everywhere: a leaning tick is longer, so that its inner end still lands on the curve
        // parallel to the outline (outer ends at `inset`, inner ends at `inset + tick`) and the ring never bulges.
        const float depth = tick / (std::max)(0.5f, dir.x * n.x + dir.y * n.y);
        const bool past = i <= now.wSecond;
        const float age = past ? static_cast<float>(now.wSecond - i) / 60.f : 0.f;
        brush->SetColor(Alpha(t.ink, past ? 0.85f - 0.45f * age : 0.16f));
        const D2D1_POINT_2F q = D2D1::Point2F(p.x + dir.x * depth, p.y + dir.y * depth);  // inner end: unchanged
        const float reach = reachOut(q, D2D1::Point2F(-dir.x, -dir.y));
        const D2D1_POINT_2F o = reach > depth ? D2D1::Point2F(q.x - dir.x * reach, q.y - dir.y * reach) : p;  // only ever longer
        rt->DrawLine(o, q, brush.Get(), 1.9f, round.Get());
    }
    std::wstring time = FormatTime(now, L"h:mm");
    float fs = (size == Size::Small ? 0.43f : 0.36f) * (std::min)(w, h * (size == Size::Small ? 1.f : 2.f));
    while (fs > 20 && TextWidth(time, Display(fs)) > w - 2 * (inset + tick + 8)) fs -= 2;
    const float textH = size == Size::Small ? h : h - 30.f;
    Text(rt, time, Display(fs), D2D1::RectF(r.left, r.top, r.right, r.top + textH), t.ink);
    if (size != Size::Small)
        Text(rt, Upper(FormatDate(now, L"dddd, MMMM d")), Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(r.left, r.bottom - 40, r.right, r.bottom - 22), t.secondary);
}

void ClockFace(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float radius, const Theme& t, const SYSTEMTIME& now) {
    auto face = Brush(rt, t.dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.04f));
    rt->FillEllipse(D2D1::Ellipse(c, radius, radius), face.Get());
    auto ink = Brush(rt, t.ink), dim = Brush(rt, t.secondary);
    for (int i = 0; i < 60; ++i) {
        const bool hour = i % 5 == 0;
        if (!hour && radius < 60) continue;
        const float a = i * kPi / 30, outer = radius - 5, inner = outer - (hour ? radius * 0.11f : radius * 0.05f);
        rt->DrawLine(D2D1::Point2F(c.x + inner * std::sin(a), c.y - inner * std::cos(a)), D2D1::Point2F(c.x + outer * std::sin(a), c.y - outer * std::cos(a)),
                     hour ? ink.Get() : dim.Get(), hour ? 1.6f : 0.8f);
    }
    if (radius >= 50) {
        auto* f = Font(radius * 0.2f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        for (int n = 1; n <= 12; ++n) {
            const float a = n * kPi / 6, d = radius * 0.68f, x = c.x + d * std::sin(a), y = c.y - d * std::cos(a);
            Text(rt, std::to_wstring(n), f, D2D1::RectF(x - 14, y - 12, x + 14, y + 12), t.ink);
        }
    }
    ComPtr<ID2D1StrokeStyle> round;
    FactoryOf(rt)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    const float sec = now.wSecond, min = now.wMinute + sec / 60.f, hr = (now.wHour % 12) + min / 60.f;
    auto hand = [&](float angle, float length, float width, ID2D1Brush* b, float tail) {
        rt->DrawLine(D2D1::Point2F(c.x - tail * std::sin(angle), c.y + tail * std::cos(angle)), D2D1::Point2F(c.x + length * std::sin(angle), c.y - length * std::cos(angle)), b, width, round.Get());
    };
    hand(hr * kPi / 6, radius * 0.50f, radius * 0.075f, ink.Get(), 0);
    hand(min * kPi / 30, radius * 0.78f, radius * 0.055f, ink.Get(), 0);
    auto accent = Brush(rt, kOrange);
    hand(sec * kPi / 30, radius * 0.86f, (std::max)(1.2f, radius * 0.02f), accent.Get(), radius * 0.16f);
    rt->FillEllipse(D2D1::Ellipse(c, radius * 0.05f, radius * 0.05f), accent.Get());
}

void PaintAnalog(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    const float h = r.bottom - r.top;
    if (size == Size::Small) { ClockFace(rt, D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2), h / 2 - 12, t, now); return; }
    const float face = size == Size::Large ? (r.right - r.left) / 2 - 24 : h / 2 - 12;
    const D2D1_POINT_2F c = size == Size::Large ? D2D1::Point2F((r.left + r.right) / 2, r.top + 24 + face) : D2D1::Point2F(r.left + 12 + face, (r.top + r.bottom) / 2);
    ClockFace(rt, c, face, t, now);
    const D2D1_RECT_F info = size == Size::Large ? D2D1::RectF(r.left + kPad, c.y + face + 12, r.right - kPad, r.bottom - kPad)
                                                 : D2D1::RectF(c.x + face + 18, r.top + kPad, r.right - kPad, r.bottom - kPad);
    const auto align = size == Size::Large ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING;
    const float mid = (info.top + info.bottom) / 2;
    Text(rt, FormatTime(now, L"h:mm"), Font(40.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, align), D2D1::RectF(info.left, mid - 34, info.right, mid + 12), t.ink);
    Text(rt, FormatDate(now, L"dddd, MMMM d"), Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM, align), D2D1::RectF(info.left, mid + 12, info.right, mid + 32), t.secondary);
}

// ---- Calendar ------------------------------------------------------------------------------------------------
void MonthGrid(ID2D1RenderTarget* rt, D2D1_RECT_F g, const Theme& t, const SYSTEMTIME& now, bool title) {
    float top = g.top;
    if (title) { Text(rt, Upper(FormatDate(now, L"MMMM")), Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(g.left, top, g.right, top + 18), kRed); top += 22; }
    const float cw = (g.right - g.left) / 7, rows = 7, ch = (g.bottom - top) / rows;
    auto* head = Font(10.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    auto* day = Font((std::min)(12.5f, ch * 0.62f), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    DWORD firstDay = 6;  // locale first day of week (0 = Monday)
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&firstDay), sizeof(firstDay) / sizeof(wchar_t));
    const int first = (static_cast<int>(firstDay) + 1) % 7;  // in SYSTEMTIME terms (0 = Sunday)
    for (int i = 0; i < 7; ++i) {
        SYSTEMTIME d{}; d.wYear = 2023; d.wMonth = 1; d.wDay = static_cast<WORD>(1 + (first + i) % 7); d.wDayOfWeek = static_cast<WORD>((first + i) % 7);  // 2023-01-01 is a Sunday
        std::wstring name = FormatDate(d, L"ddd"); if (!name.empty()) name = name.substr(0, 1);
        Text(rt, name, head, D2D1::RectF(g.left + i * cw, top, g.left + (i + 1) * cw, top + ch), t.secondary);
    }
    SYSTEMTIME firstOfMonth = now; firstOfMonth.wDay = 1;
    FILETIME ft{}; SystemTimeToFileTime(&firstOfMonth, &ft); FileTimeToSystemTime(&ft, &firstOfMonth);  // fills wDayOfWeek
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int count = days[now.wMonth - 1];
    if (now.wMonth == 2 && ((now.wYear % 4 == 0 && now.wYear % 100 != 0) || now.wYear % 400 == 0)) count = 29;
    const int offset = (firstOfMonth.wDayOfWeek - first + 7) % 7;
    auto red = Brush(rt, kRed);
    for (int d = 1; d <= count; ++d) {
        const int cell = offset + d - 1, row = cell / 7 + 1, col = cell % 7;
        const D2D1_RECT_F box = D2D1::RectF(g.left + col * cw, top + row * ch, g.left + (col + 1) * cw, top + (row + 1) * ch);
        const bool today = d == now.wDay;
        if (today) { const float rr = (std::min)(cw, ch) * 0.46f; rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((box.left + box.right) / 2, (box.top + box.bottom) / 2), rr, rr), red.Get()); }
        const int weekday = (first + col) % 7;
        Text(rt, std::to_wstring(d), day, box, today ? D2D1::ColorF(1, 1, 1, 1) : (weekday == 0 || weekday == 6) ? t.secondary : t.ink);
    }
}

D2D1_COLOR_F EventInk(unsigned rgb, const Theme& t) {  // event colour, lifted a little on dark plates so text reads
    D2D1_COLOR_F c = Hex(rgb);
    if (t.dark) { c.r = c.r * 0.8f + 0.2f; c.g = c.g * 0.8f + 0.2f; c.b = c.b * 0.8f + 0.2f; }
    return c;
}
std::wstring EventTime(const CalEvent& e) {
    if (e.allDay) return L"All day";
    SYSTEMTIME st{}; const FILETIME f{static_cast<DWORD>(e.start), static_cast<DWORD>(e.start >> 32)}; FileTimeToSystemTime(&f, &st);
    std::wstring s = FormatTime(st, L"h:mm"), ap = FormatTime(st, L"tt");
    return s + ap;
}
std::wstring DayHeader(ULONGLONG day, ULONGLONG today) {
    if (day == today) return L"TODAY";
    if (day == today + kDayTicks) return L"TOMORROW";
    SYSTEMTIME st{}; const FILETIME f{static_cast<DWORD>(day), static_cast<DWORD>(day >> 32)}; FileTimeToSystemTime(&f, &st);
    return Upper(FormatDate(st, L"dddd, MMM d"));
}
// All-day / today event pill (left column of the medium Calendar).
void Pill(ID2D1RenderTarget* rt, const CalEvent& e, D2D1_RECT_F box, const Theme& t) {
    const D2D1_COLOR_F ink = EventInk(e.color, t);
    auto b = Brush(rt, Alpha(Hex(e.color), t.dark ? 0.22f : 0.16f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(box, (box.bottom - box.top) / 2, (box.bottom - box.top) / 2), b.Get());
    const float d = box.bottom - box.top - 6;
    b->SetColor(Hex(e.color));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.left + 3 + d / 2, (box.top + box.bottom) / 2), d / 2, d / 2), b.Get());
    Text(rt, L"\uE787", Glyphs(d * 0.62f), D2D1::RectF(box.left + 3, box.top + 3, box.left + 3 + d, box.bottom - 3), D2D1::ColorF(0, 0, 0, 0.7f));
    Text(rt, e.title, Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(box.left + d + 9, box.top, box.right - 6, box.bottom), ink);
}
// Upcoming events grouped by day: header, accent bar, titles in the event colour, then the first event's time.
float Upcoming(ID2D1RenderTarget* rt, const std::vector<CalEvent>& events, ULONGLONG today, ULONGLONG from, D2D1_RECT_F box, const Theme& t, bool blocks) {
    float y = box.top;
    size_t i = 0;
    while (i < events.size() && events[i].start + (events[i].allDay ? kDayTicks : 1) <= from) ++i;
    int groups = 0;
    while (i < events.size() && y + 40 < box.bottom) {
        const ULONGLONG day = DayStart(events[i].start < from ? from : events[i].start);
        size_t j = i; std::vector<const CalEvent*> group;
        while (j < events.size() && DayStart(events[j].start < from ? from : events[j].start) == day) { group.push_back(&events[j]); ++j; }
        Text(rt, DayHeader(day, today), Font(11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(box.left, y, box.right, y + 16), t.secondary);
        y += 20;
        const float top = y;
        const bool block = blocks && groups > 0;  // later days: a tinted block per the macOS reference
        if (group.size() > 1 && !block) { Text(rt, std::to_wstring(group.size()) + L" Events", Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(box.left + 10, y, box.right, y + 17), t.ink); y += 17; }
        size_t shown = 0;
        for (const CalEvent* e : group) {
            if (y + 17 > box.bottom - (block ? 4 : 18)) break;
            Text(rt, e->title, Font(12.5f, block ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(box.left + 10, y, box.right - 4, y + 17), EventInk(e->color, t));
            y += 17; ++shown;
            if (block) { Text(rt, EventTime(*e), Font(11.5f), D2D1::RectF(box.left + 10, y, box.right - 4, y + 15), EventInk(e->color, t)); y += 17; }
            if (shown >= (block ? 1u : 2u)) break;
        }
        if (!block && y + 16 <= box.bottom) { Text(rt, EventTime(*group.front()), Font(11.5f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(box.left + 10, y, box.right, y + 16), t.secondary); y += 16; }
        auto bar = Brush(rt, Hex(group.front()->color));
        if (block) { auto bg = Brush(rt, Alpha(Hex(group.front()->color), 0.16f)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left - 2, top - 3, box.right, y + 2), 6, 6), bg.Get()); }
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 1, top, box.left + 4, y), 1.5f, 1.5f), bar.Get());
        y += 10; ++groups; i = j;
    }
    return y;
}

// ---- Scrolling widgets (Calendar, Weather) ---------------------------------------------------------------------
// Content inside `view` drawn moved up by `offset`, with 14 DIP fades at the edges where more content continues
// (top when scrolled down, bottom while more is below). The layer clips to `view`.
class ScrollView {
public:
    ScrollView(ID2D1RenderTarget* rt, D2D1_RECT_F view, float offset, bool fadeTop, bool fadeBottom) : rt_(rt) {
        rt->GetTransform(&old_);
        const float h = (std::max)(1.f, view.bottom - view.top), edge = (std::min)(0.45f, 14.f / h);
        const D2D1_GRADIENT_STOP stops[4] = {{0.f, D2D1::ColorF(0, 0, 0, fadeTop ? 0.f : 1.f)}, {edge, D2D1::ColorF(0, 0, 0, 1)},
                                             {1.f - edge, D2D1::ColorF(0, 0, 0, 1)}, {1.f, D2D1::ColorF(0, 0, 0, fadeBottom ? 0.f : 1.f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 4, &collection)))
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, view.top), D2D1::Point2F(0, view.bottom)), collection.Get(), &mask_);
        if (SUCCEEDED(rt->CreateLayer(nullptr, &layer_)))
            rt->PushLayer(D2D1::LayerParameters(view, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.f, mask_.Get()), layer_.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -offset) * old_);
    }
    ~ScrollView() { rt_->SetTransform(old_); if (layer_) rt_->PopLayer(); }
private:
    ID2D1RenderTarget* rt_;
    D2D1_MATRIX_3X2_F old_{};
    ComPtr<ID2D1LinearGradientBrush> mask_;
    ComPtr<ID2D1Layer> layer_;
};

// ---- macOS 26 Calendar widget pieces (reference screenshots 2026-11-19) -------------------------------------------
SYSTEMTIME TicksToTime(ULONGLONG ticks) { SYSTEMTIME st{}; const FILETIME f{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}; FileTimeToSystemTime(&f, &st); return st; }
// "7:30 – 8:30PM", "11PM – 12AM", "10:10 – 11:10AM": minutes only when not :00, the period once when both ends share it.
std::wstring TimeRange(const CalEvent& e) {
    if (e.allDay) return L"All day";
    const SYSTEMTIME a = TicksToTime(e.start), b = TicksToTime(e.end > e.start ? e.end : e.start);
    auto hm = [](const SYSTEMTIME& st) { return FormatTime(st, st.wMinute ? L"h:mm" : L"h"); };
    const std::wstring pa = FormatTime(a, L"tt"), pb = FormatTime(b, L"tt");
    if (e.end <= e.start) return hm(a) + pa;
    return hm(a) + (pa == pb ? L"" : pa) + L" – " + hm(b) + pb;
}
std::wstring StartTime(const CalEvent& e) {
    if (e.allDay) return L"All day";
    const SYSTEMTIME a = TicksToTime(e.start);
    return FormatTime(a, a.wMinute ? L"h:mm" : L"h") + FormatTime(a, L"tt");
}
std::wstring FirstLine(const std::wstring& s) { const size_t n = s.find_first_of(L"\r\n"); return n == std::wstring::npos ? s : s.substr(0, n); }
// Text with the AM/PM letters in small capitals, as macOS sets event times.
void SmallCapsTime(ID2D1RenderTarget* rt, const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight, D2D1_RECT_F box, D2D1_COLOR_F color) {
    ComPtr<IDWriteTextLayout> layout;
    auto* f = Font(size, weight);
    if (!f || FAILED(DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, box.right - box.left, box.bottom - box.top, &layout))) { Text(rt, text, f, box, color); return; }
    for (size_t i = 0; i + 1 < text.size(); ++i)
        if ((text[i] == L'A' || text[i] == L'P') && text[i + 1] == L'M') layout->SetFontSize(size * 0.8f, DWRITE_TEXT_RANGE{static_cast<UINT32>(i), 2});
    auto b = Brush(rt, color);
    rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), b.Get());
    rt->PopAxisAlignedClip();
}
// Event block: a rounded tint of the event colour with a bar on its left; title (bold), location, time, all in the
// event colour.
// Measured on the reference: a three-line block is ~40 DIP, a two-line block ~28.
constexpr float kEventLine = 12.f;
float EventBlockHeight(const CalEvent& e, bool withLocation) { return 2.5f + kEventLine * (withLocation && !e.location.empty() ? 3 : 2) + 4.5f; }
void EventBlock(ID2D1RenderTarget* rt, const CalEvent& e, D2D1_RECT_F box, const Theme& t, bool withLocation, bool startOnly = false) {
    const D2D1_COLOR_F ink = EventInk(e.color, t);
    auto b = Brush(rt, Alpha(Hex(e.color), t.dark ? 0.24f : 0.16f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(box, 5.f, 5.f), b.Get());
    b->SetColor(Hex(e.color));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 4.f, box.top + 4.f, box.left + 7.f, box.bottom - 4.f), 1.5f, 1.5f), b.Get());
    const float x = box.left + 12.f, right = box.right - 6.f;
    float y = box.top + 2.f;
    const float avail = box.bottom - box.top;
    Text(rt, e.title, Font(11.5f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(x, y, right, y + kEventLine + 3), ink);
    y += kEventLine;
    if (withLocation && !e.location.empty() && avail >= 3 * kEventLine + 5) { Text(rt, FirstLine(e.location), Font(11.f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(x, y, right, y + kEventLine + 3), Alpha(ink, 0.9f)); y += kEventLine; }
    if (avail >= 2 * kEventLine + 3) SmallCapsTime(rt, startOnly ? StartTime(e) : TimeRange(e), 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(x, y, right, y + kEventLine + 3), Alpha(ink, 0.9f));
}
// Hour timeline (large Calendar): `hours` rows from `firstHour` (fractional while scrolling) of `day`, hour labels left
// of hairlines, events as blocks from their start to their end (at least two lines tall), and on today the red
// current-time line. Returns how many events of that day fall outside the shown hours (or are all-day).
int Timeline(ID2D1RenderTarget* rt, const std::vector<CalEvent>& events, ULONGLONG day, float firstHour, int hours, D2D1_RECT_F box, const Theme& t, bool today) {
    const float labelW = 20.f, lineLeft = box.left + labelW + 6.f, step = (box.bottom - box.top) / hours;
    auto line = Brush(rt, t.dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.1f));
    auto* label = Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    for (int h = static_cast<int>(std::floor(firstHour)); h <= static_cast<int>(std::ceil(firstHour + hours)); ++h) {
        const float y = box.top + (h - firstHour) * step;
        if (y < box.top - 9 || y > box.bottom + 9) continue;
        const int hh = ((h % 24) + 24) % 24, h12 = hh % 12 == 0 ? 12 : hh % 12;
        Text(rt, std::to_wstring(h12), label, D2D1::RectF(box.left - 4, y - 8, box.left + labelW, y + 8), t.secondary);
        rt->DrawLine(D2D1::Point2F(lineLeft, y), D2D1::Point2F(box.right, y), line.Get(), 1.f);
    }
    const double hourTicks = static_cast<double>(kDayTicks / 24);
    const ULONGLONG from = day + static_cast<ULONGLONG>(std::max(0.0, firstHour * hourTicks)), to = from + static_cast<ULONGLONG>(hours * hourTicks);
    auto yOf = [&](ULONGLONG ticks) { return box.top + static_cast<float>((static_cast<double>(ticks) - static_cast<double>(from)) / (static_cast<double>(to) - static_cast<double>(from))) * (box.bottom - box.top); };
    int hidden = 0;
    float lastBottom = -1e9f;
    for (const auto& e : events) {
        if (DayStart(e.start) != day) continue;
        const ULONGLONG endT = e.end > e.start ? e.end : e.start + kDayTicks / 48;
        if (e.allDay || e.start >= to || endT <= from) { ++hidden; continue; }
        float top = yOf(e.start), bottom = yOf(endT);  // may start above the box while scrolling: the fade clips it
        top = (std::max)(top, lastBottom + 2.f);  // overlapping events stack below each other instead of on top
        bottom = (std::max)(bottom, top + 2 * kEventLine + 5.f);
        if (top >= box.bottom) { ++hidden; continue; }
        EventBlock(rt, e, D2D1::RectF(lineLeft + 1.f, top + 1.f, box.right, (std::min)(bottom, box.bottom + 6.f) - 1.f), t, false, true);
        lastBottom = bottom;
    }
    if (today) {
        const ULONGLONG now = LocalNow();
        if (now >= from && now <= to) {
            const float y = yOf(now);
            auto red = Brush(rt, kRed);
            rt->DrawLine(D2D1::Point2F(box.left - 2, y), D2D1::Point2F(box.right, y), red.Get(), 1.5f);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.left - 2, y), 3.5f, 3.5f), red.Get());
        }
    }
    return hidden;
}

void PaintCalendar(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    const std::wstring weekday = Upper(FormatDate(now, L"dddd"));
    const auto events = Calendars().Events();
    const ULONGLONG nowTicks = LocalNow(), today = DayStart(nowTicks), tomorrow = today + kDayTicks;
    const bool configured = Calendars().Configured();
    WidgetScroll& sc = PaintScroll();
    auto empty = [&](D2D1_RECT_F box) {
        Text(rt, configured ? docky::ui::Text(L"No upcoming events") : docky::ui::Text(L"No calendars"), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(box.left, box.top, box.right, box.top + 18), t.secondary);
        if (!configured) Text(rt, docky::ui::Text(L"Add one in Widget Settings"), Font(11.5f), D2D1::RectF(box.left, box.top + 18, box.right, box.top + 34), t.secondary);
    };
    auto header = [&](float left, float right) {
        Text(rt, weekday, Font(12.f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(left, r.top + 14, right, r.top + 30), kRed);
        Text(rt, std::to_wstring(now.wDay), Font(40.f, DWRITE_FONT_WEIGHT_REGULAR), D2D1::RectF(left - 2, r.top + 24, right, r.top + 74), t.ink);
    };
    const float mid = (r.left + r.right) / 2;
    if (size == Size::Large) {
        // Two hour timelines (macOS large Calendar): today from the current hour, tomorrow from its first event.
        // Scrolling moves both through the day (hours above and below), each kept within its own day.
        header(r.left + kPad, mid - 6);
        const D2D1_RECT_F todayBox = D2D1::RectF(r.left + kPad + 4, r.top + 88, mid - 8, r.bottom - 30);
        const float step = (todayBox.bottom - todayBox.top) / 7, base = static_cast<float>((std::min)(static_cast<int>(now.wHour), 17));
        sc.min = -base * step; sc.max = (17 - base) * step;
        const float shiftHours = std::clamp(sc.offset, sc.min, sc.max) / step;
        int firstTomorrow = 9;
        for (const auto& e : events) if (DayStart(e.start) == tomorrow && !e.allDay) { firstTomorrow = TicksToTime(e.start).wHour; break; }
        firstTomorrow = (std::min)(firstTomorrow, 17);
        const float todayFirst = std::clamp(base + shiftHours, 0.f, 17.f), tomorrowFirst = std::clamp(firstTomorrow + shiftHours, 0.f, 17.f);
        int todayHidden = 0, tomorrowHidden = 0;
        {   // hours sliding past the top and bottom fade out
            const bool moved = std::fabs(sc.offset) > 0.5f;
            ScrollView view(rt, D2D1::RectF(r.left, todayBox.top - 10, mid, todayBox.bottom + 10), 0, moved, moved);
            todayHidden = Timeline(rt, events, today, todayFirst, 7, todayBox, t, true);
        }
        Text(rt, docky::ui::Text(L"TOMORROW"), Font(12.f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(mid + 14, r.top + 14, r.right - kPad, r.top + 30), t.secondary);
        {
            const D2D1_RECT_F box = D2D1::RectF(mid + 10, r.top + 50, r.right - kPad, r.bottom - 30);
            const bool moved = std::fabs(sc.offset) > 0.5f;
            ScrollView view(rt, D2D1::RectF(mid, box.top - 10, r.right, box.bottom + 10), 0, moved, moved);
            tomorrowHidden = Timeline(rt, events, tomorrow, tomorrowFirst, 7, box, t, false);
        }
        // "3 more events": what the two timelines could not show, with up to three of their colours as bars.
        const int more = todayHidden + tomorrowHidden;
        if (more > 0) {
            std::vector<unsigned> colors;
            for (const auto& e : events)
                if ((DayStart(e.start) == today || DayStart(e.start) == tomorrow) && colors.size() < 3 && std::find(colors.begin(), colors.end(), e.color) == colors.end()) colors.push_back(e.color);
            float x = mid + 14;
            const float y = r.bottom - 24;
            for (unsigned c : colors) { auto b = Brush(rt, Hex(c)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 2, x + 3, y + 15), 1.5f, 1.5f), b.Get()); x += 5; }
            Text(rt, std::to_wstring(more) + (more == 1 ? L" more event" : L" more events"), Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 4, y, r.right - kPad, y + 17), t.secondary);
        }
        if (!configured) empty(D2D1::RectF(r.left + kPad, r.bottom - 48, mid - 8, r.bottom - 12));
        return;
    }
    if (size == Size::Small) {
        // The upcoming list scrolls under the date (all events of the next month).
        const float left = r.left + kPad;
        header(left, r.right - kPad);
        const D2D1_RECT_F list = D2D1::RectF(left, r.top + 80, r.right - kPad, r.bottom - 8);
        float end = list.top;
        {
            ScrollView view(rt, D2D1::RectF(r.left, list.top - 4, r.right, r.bottom), sc.offset, sc.offset > sc.min + 0.5f, sc.offset < sc.max - 0.5f);
            end = Upcoming(rt, events, today, nowTicks, D2D1::RectF(list.left, list.top, list.right, list.top + 3000), t, false);
        }
        sc.min = 0; sc.max = (std::max)(0.f, end - list.bottom + 4);
        if (end <= list.top) empty(list);
        return;
    }
    // Medium (macOS "Up Next"): the date and today's next event on the left; on the right the rest of today, TOMORROW
    // and the following days as event blocks, which scroll.
    const float left = r.left + kPad, colRight = mid - 5;
    header(left, colRight);
    std::vector<const CalEvent*> rest;
    std::vector<std::pair<ULONGLONG, std::vector<const CalEvent*>>> later;  // tomorrow and the next days, by day
    for (const auto& e : events) {
        const ULONGLONG endT = e.allDay ? DayStart(e.start) + kDayTicks : (e.end > e.start ? e.end : e.start + 1);
        if (endT <= nowTicks) continue;
        const ULONGLONG d = DayStart(e.start < nowTicks ? nowTicks : e.start);
        if (d == today) { rest.push_back(&e); continue; }
        if (d > today + 7 * kDayTicks) continue;
        if (later.empty() || later.back().first != d) later.push_back({d, {}});
        later.back().second.push_back(&e);
    }
    float y = r.top + 76;
    size_t used = 0;
    while (used < rest.size()) {
        const float h = EventBlockHeight(*rest[used], true);
        if (y + h > r.bottom - 14) break;
        EventBlock(rt, *rest[used], D2D1::RectF(left - 2, y, colRight, y + h), t, true);
        y += h + 5; ++used;
    }
    if (rest.empty()) Text(rt, configured ? docky::ui::Text(L"No more events today") : docky::ui::Text(L"No calendars"), Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, colRight, y + 18), t.secondary);
    const float rl = mid + 5, rr = r.right - kPad, top = r.top + 14, bottom = r.bottom - 12;
    // Reference rhythm: blocks 5 apart, a day header 3 below the last block before it and 2 above its first block.
    float ry = top;
    {
        ScrollView view(rt, D2D1::RectF(mid, r.top + 6, r.right, r.bottom - 4), sc.offset, sc.offset > sc.min + 0.5f, sc.offset < sc.max - 0.5f);
        for (size_t i = used; i < rest.size(); ++i) {
            const float h = EventBlockHeight(*rest[i], true);
            EventBlock(rt, *rest[i], D2D1::RectF(rl, ry, rr, ry + h), t, true);
            ry += h + 5;
        }
        for (const auto& [day, list] : later) {
            if (ry > top) ry += 3;
            Text(rt, DayHeader(day, today), Font(12.f, DWRITE_FONT_WEIGHT_BOLD), D2D1::RectF(rl, ry, rr, ry + 16), t.secondary);
            ry += 18;
            for (const CalEvent* e : list) {
                const float h = EventBlockHeight(*e, false);
                EventBlock(rt, *e, D2D1::RectF(rl, ry, rr, ry + h), t, false);
                ry += h + 5;
            }
        }
    }
    sc.min = 0; sc.max = (std::max)(0.f, ry - 5 - bottom);
    if (ry == top && rest.size() <= used) empty(D2D1::RectF(rl, top, rr, bottom));
}

// ---- Batteries and system: 2 x 2 rings with a device glyph, as the macOS Batteries widget ----------------------
void GlyphRing(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float radius, float value, D2D1_COLOR_F color, const wchar_t* glyph, const Theme& t, bool present) {
    Ring(rt, c, radius, radius * 0.17f, present ? value : 0.f, color, t);
    if (present && glyph) Text(rt, glyph, Glyphs(radius * 0.62f), D2D1::RectF(c.x - radius, c.y - radius, c.x + radius, c.y + radius), t.ink);
}
void PaintBattery(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t) {
    // This PC first, then accessories (Bluetooth / XInput) as Windows reports them; empty rings fill the rest, as on macOS.
    auto list = Devices().Snapshot();
    { const auto& off = CurrentPrefs().batteryWidgetOff;  // devices unticked for the widget in Settings
      list.erase(std::remove_if(list.begin(), list.end(), [&](const DeviceBattery& d) { return NameListed(off, d.name); }), list.end()); }
    if (list.empty()) { SYSTEM_POWER_STATUS ps{}; GetSystemPowerStatus(&ps); list.push_back({L"This PC", -1, DeviceKind::PC, false}); (void)ps; }
    auto color = [](const DeviceBattery& d) { return d.percent >= 0 && d.percent <= 20 && !d.charging ? kRed : kGreen; };
    auto value = [](const DeviceBattery& d) { return d.percent < 0 ? 1.f : d.percent / 100.f; };
    auto glyph = [](const DeviceBattery& d) { return d.kind == DeviceKind::PC && d.percent < 0 ? L"\uE7F4" : DeviceGlyph(d.kind); };
    auto percent = [](const DeviceBattery& d) { return d.percent < 0 ? std::wstring(L"AC") : std::to_wstring(d.percent) + L"%"; };
    auto bolt = [&](const DeviceBattery& d, D2D1_POINT_2F c, float rad) {
        if (!d.charging) return;
        auto b2 = Brush(rt, kGreen); const float s = rad * 0.42f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + rad * 0.72f, c.y - rad * 0.72f), s, s), b2.Get());
        Text(rt, L"\uE945", Glyphs(s * 1.1f), D2D1::RectF(c.x + rad * 0.72f - s, c.y - rad * 0.72f - s, c.x + rad * 0.72f + s, c.y - rad * 0.72f + s), D2D1::ColorF(0, 0, 0, 0.85f));
    };
    if (size == Size::Small) {
        const float cell = (r.right - r.left - 2 * 12) / 2, rad = cell / 2 - 6;
        for (int i = 0; i < 4; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(r.left + 12 + cell * (i % 2 + 0.5f), r.top + 12 + cell * (i / 2 + 0.5f));
            const bool have = i < static_cast<int>(list.size());
            GlyphRing(rt, c, rad, have ? value(list[i]) : 0.f, have ? color(list[i]) : kGreen, have ? glyph(list[i]) : nullptr, t, have);
            if (have) bolt(list[i], c, rad);
        }
        return;
    }
    if (size == Size::Medium) {
        const float rad = 28.f, y = r.top + 54;
        for (int i = 0; i < 4; ++i) {
            const float cx = r.left + (r.right - r.left) * (i + 0.5f) / 4;
            const bool have = i < static_cast<int>(list.size());
            GlyphRing(rt, D2D1::Point2F(cx, y), rad, have ? value(list[i]) : 0.f, have ? color(list[i]) : kGreen, have ? glyph(list[i]) : nullptr, t, have);
            if (!have) continue;
            bolt(list[i], D2D1::Point2F(cx, y), rad);
            Text(rt, percent(list[i]), Font(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cx - 40, y + rad + 10, cx + 40, y + rad + 28), t.ink);
        }
        return;
    }
    // Large: one row per device with name, percentage and a level bar (the macOS large Batteries widget).
    float y = r.top + 16;
    for (size_t i = 0; i < list.size() && y + 46 <= r.bottom - 8; ++i) {
        const auto& d = list[i];
        const D2D1_POINT_2F c = D2D1::Point2F(r.left + kPad + 18, y + 20);
        GlyphRing(rt, c, 18.f, value(d), color(d), glyph(d), t, true);
        Text(rt, d.name, Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + kPad + 46, y + 2, r.right - kPad - 56, y + 20), t.ink);
        Text(rt, percent(d) + (d.charging ? L" \u26A1" : L""), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.right - kPad - 60, y + 2, r.right - kPad, y + 20), t.ink);
        auto track = Brush(rt, t.dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.08f));
        const D2D1_RECT_F bar = D2D1::RectF(r.left + kPad + 46, y + 26, r.right - kPad, y + 31);
        rt->FillRoundedRectangle(D2D1::RoundedRect(bar, 2.5f, 2.5f), track.Get());
        track->SetColor(color(d));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * value(d), bar.bottom), 2.5f, 2.5f), track.Get());
        y += 46;
    }
}

struct SystemSample { float cpu = 0, memory = 0; ULONGLONG memTotal = 0, memUsed = 0; } g_system;
void PaintSystem(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t) {
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    const bool disk = GetDiskFreeSpaceExW(L"C:\\", nullptr, &totalBytes, &freeBytes) && totalBytes.QuadPart;
    const float values[3] = {g_system.cpu, g_system.memory, disk ? 1.f - static_cast<float>(freeBytes.QuadPart) / static_cast<float>(totalBytes.QuadPart) : 0.f};
    const wchar_t* glyphs[3] = {L"\uE9D9", L"\uEEA0", L"\uEDA2"};  // CPU, memory, hard drive
    const wchar_t* names[3] = {L"CPU", L"Memory", L"Disk"};
    const D2D1_COLOR_F colors[3] = {kBlue, kOrange, kGreen};
    if (size == Size::Small) {
        const float cell = (r.right - r.left - 2 * 12) / 2, rad = cell / 2 - 6;
        for (int i = 0; i < 4; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(r.left + 12 + cell * (i % 2 + 0.5f), r.top + 12 + cell * (i / 2 + 0.5f));
            if (i < 3) GlyphRing(rt, c, rad, values[i], colors[i], glyphs[i], t, true);
            else Text(rt, std::to_wstring(static_cast<int>(std::lround(values[0] * 100))) + L"%", Font(17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false), D2D1::RectF(c.x - rad, c.y - 12, c.x + rad, c.y + 12), t.ink);
        }
        return;
    }
    const float rad = 30.f, y = r.top + 56;
    for (int i = 0; i < 3; ++i) {
        const float cx = r.left + (r.right - r.left) * (i + 0.5f) / 3;
        GlyphRing(rt, D2D1::Point2F(cx, y), rad, values[i], colors[i], glyphs[i], t, true);
        Text(rt, std::wstring(names[i]) + L" " + std::to_wstring(static_cast<int>(std::lround(values[i] * 100))) + L"%", Font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cx - 60, y + rad + 8, cx + 60, y + rad + 26), t.ink);
    }
}

// ---- Weather (macOS layout: sky-coloured plate, place, large temperature, condition, H/L; hourly; 6-day) -----------
// Weather icons are drawn here (original vector shapes), sized by `s` around centre c.
void Cloud(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float s, D2D1_COLOR_F color) {
    auto b = Brush(rt, color);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - s * 0.18f, c.y + s * 0.02f), s * 0.24f, s * 0.22f), b.Get());
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + s * 0.08f, c.y - s * 0.08f), s * 0.30f, s * 0.29f), b.Get());
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + s * 0.30f, c.y + s * 0.06f), s * 0.18f, s * 0.17f), b.Get());
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - s * 0.40f, c.y + s * 0.02f, c.x + s * 0.47f, c.y + s * 0.23f), s * 0.1f, s * 0.1f), b.Get());
}
void Sun(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float s) {
    auto b = Brush(rt, Hex(0xFFD60A));
    rt->FillEllipse(D2D1::Ellipse(c, s * 0.24f, s * 0.24f), b.Get());
    ComPtr<ID2D1StrokeStyle> round;
    FactoryOf(rt)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    for (int i = 0; i < 8; ++i) {
        const float a = i * kPi / 4, r0 = s * 0.34f, r1 = s * 0.46f;
        rt->DrawLine(D2D1::Point2F(c.x + r0 * std::cos(a), c.y + r0 * std::sin(a)), D2D1::Point2F(c.x + r1 * std::cos(a), c.y + r1 * std::sin(a)), b.Get(), s * 0.07f, round.Get());
    }
}
void Moon(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float s) {
    ComPtr<ID2D1EllipseGeometry> a, b; ComPtr<ID2D1PathGeometry> moon; ComPtr<ID2D1GeometrySink> sink;
    auto f = FactoryOf(rt);
    f->CreateEllipseGeometry(D2D1::Ellipse(c, s * 0.32f, s * 0.32f), &a);
    f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(c.x + s * 0.18f, c.y - s * 0.14f), s * 0.28f, s * 0.28f), &b);
    if (FAILED(f->CreatePathGeometry(&moon)) || FAILED(moon->Open(&sink))) return;
    a->CombineWithGeometry(b.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get()); sink->Close();
    auto brush = Brush(rt, D2D1::ColorF(0.98f, 0.97f, 0.88f, 1));
    rt->FillGeometry(moon.Get(), brush.Get());
}
void SkyIcon(ID2D1RenderTarget* rt, Sky sky, bool day, D2D1_POINT_2F c, float s) {
    ComPtr<ID2D1StrokeStyle> round;
    FactoryOf(rt)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.97f), grey = D2D1::ColorF(0.86f, 0.88f, 0.92f, 1);
    auto drops = [&](D2D1_COLOR_F color, float len, bool dots) {
        auto b = Brush(rt, color);
        for (int i = 0; i < 3; ++i) {
            const float x = c.x - s * 0.2f + i * s * 0.2f, y = c.y + s * 0.33f;
            if (dots) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + s * 0.05f), s * 0.05f, s * 0.05f), b.Get());
            else rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x - s * 0.06f, y + len), b.Get(), s * 0.06f, round.Get());
        }
    };
    switch (sky) {
    case Sky::Clear: if (day) Sun(rt, c, s); else Moon(rt, c, s); break;
    case Sky::PartlyCloudy:
        if (day) Sun(rt, D2D1::Point2F(c.x + s * 0.16f, c.y - s * 0.16f), s * 0.8f); else Moon(rt, D2D1::Point2F(c.x + s * 0.16f, c.y - s * 0.16f), s * 0.8f);
        Cloud(rt, D2D1::Point2F(c.x - s * 0.06f, c.y + s * 0.08f), s * 0.85f, white); break;
    case Sky::Cloudy: Cloud(rt, c, s, white); break;
    case Sky::Fog: { auto b = Brush(rt, white); for (int i = 0; i < 3; ++i) { const float y = c.y - s * 0.18f + i * s * 0.18f; rt->DrawLine(D2D1::Point2F(c.x - s * 0.36f + (i % 2) * s * 0.08f, y), D2D1::Point2F(c.x + s * 0.36f - ((i + 1) % 2) * s * 0.08f, y), b.Get(), s * 0.08f, round.Get()); } break; }
    case Sky::Drizzle: Cloud(rt, D2D1::Point2F(c.x, c.y - s * 0.1f), s, grey); drops(Hex(0x64D2FF), s * 0.08f, false); break;
    case Sky::Rain: Cloud(rt, D2D1::Point2F(c.x, c.y - s * 0.1f), s, grey); drops(Hex(0x64D2FF), s * 0.16f, false); break;
    case Sky::Snow: Cloud(rt, D2D1::Point2F(c.x, c.y - s * 0.1f), s, grey); drops(white, 0, true); break;
    case Sky::Thunder: {
        Cloud(rt, D2D1::Point2F(c.x, c.y - s * 0.12f), s, D2D1::ColorF(0.72f, 0.74f, 0.80f, 1));
        ComPtr<ID2D1PathGeometry> bolt; ComPtr<ID2D1GeometrySink> sink; FactoryOf(rt)->CreatePathGeometry(&bolt); bolt->Open(&sink);
        const D2D1_POINT_2F p[6] = {{c.x + s * 0.04f, c.y + s * 0.06f}, {c.x - s * 0.12f, c.y + s * 0.30f}, {c.x, c.y + s * 0.30f}, {c.x - s * 0.06f, c.y + s * 0.48f}, {c.x + s * 0.14f, c.y + s * 0.22f}, {c.x + s * 0.02f, c.y + s * 0.22f}};
        sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED); sink->AddLines(p + 1, 5); sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
        auto b = Brush(rt, Hex(0xFFD60A)); rt->FillGeometry(bolt.Get(), b.Get()); break;
    }
    }
}
// Plate gradient by sky and time of day (top -> bottom), like the macOS widget's backgrounds.
void SkyPlate(ID2D1RenderTarget* rt, D2D1_RECT_F r, Sky sky, bool day) {
    unsigned top = 0x2F7AD6, bottom = 0x6CB4EE;  // clear day
    if (!day) { top = 0x0B1633; bottom = 0x23375F; if (sky != Sky::Clear && sky != Sky::PartlyCloudy) { top = 0x262C3A; bottom = 0x454D5C; } }
    else if (sky == Sky::PartlyCloudy) { top = 0x4F86C6; bottom = 0x8FB6DF; }
    else if (sky == Sky::Cloudy || sky == Sky::Fog) { top = 0x8A93A6; bottom = 0xB7BDC8; }
    else if (sky == Sky::Drizzle || sky == Sky::Rain) { top = 0x4E5B6B; bottom = 0x758392; }
    else if (sky == Sky::Snow) { top = 0x8899AE; bottom = 0xC2CCD8; }
    else if (sky == Sky::Thunder) { top = 0x353A4D; bottom = 0x596076; }
    ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> grad;
    const D2D1_GRADIENT_STOP st[2] = {{0.f, Hex(top)}, {1.f, Hex(bottom)}};
    if (FAILED(rt->CreateGradientStopCollection(st, 2, &stops)) ||
        FAILED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)), stops.Get(), &grad))) return;
    if (auto shape = WidgetShape(r, rt)) rt->FillGeometry(shape.Get(), grad.Get());
}
std::wstring Deg(float v) { return std::to_wstring(static_cast<int>(std::lround(v))) + L"°"; }
std::wstring HourLabel(int h, bool first) {
    if (first) return L"Now";
    const int h12 = h % 12 == 0 ? 12 : h % 12;
    return std::to_wstring(h12) + (h < 12 ? L"AM" : L"PM");
}

// One forecast day: name, icon, low, range bar on the week's shared scale, high (macOS 10-day forecast row).
float WeatherDayRow(ID2D1RenderTarget* rt, const WeatherData& d, const WeatherDay& day, float left, float right, float yy, bool compact) {
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1), soft = D2D1::ColorF(1, 1, 1, 0.82f);
    const float span = (std::max)(1.f, d.weekHigh - d.weekLow);
    Text(rt, day.name, Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, yy, left + 60, yy + 26), white);
    SkyIcon(rt, SkyOf(day.code), true, D2D1::Point2F(left + (compact ? 58.f : 78.f), yy + 13), 17.f);
    if (compact) {  // small widget: no bar
        Text(rt, Deg(day.low) + L"  " + Deg(day.high), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(left + 70, yy, right, yy + 26), white);
        return yy + 28;
    }
    Text(rt, Deg(day.low), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(left + 96, yy, left + 136, yy + 26), soft);
    const float bx0 = left + 144, bx1 = right - 44, by = yy + 13;
    auto track = Brush(rt, D2D1::ColorF(0, 0, 0, 0.18f)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, by - 2.5f, bx1, by + 2.5f), 2.5f, 2.5f), track.Get());
    ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> grad;
    const D2D1_GRADIENT_STOP st[2] = {{0.f, Hex(0x5AC8FA)}, {1.f, Hex(0xFFCC00)}};
    rt->CreateGradientStopCollection(st, 2, &stops);
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(bx0, 0), D2D1::Point2F(bx1, 0)), stops.Get(), &grad);
    const float a = bx0 + (bx1 - bx0) * (day.low - d.weekLow) / span, b = bx0 + (bx1 - bx0) * (day.high - d.weekLow) / span;
    if (grad) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a, by - 2.5f, (std::max)(b, a + 5), by + 2.5f), 2.5f, 2.5f), grad.Get());
    Text(rt, Deg(day.high), Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(bx1 + 4, yy, right, yy + 26), white);
    return yy + 28;
}

// Weather: the current conditions, the hourly row and the 10-day forecast as one column that scrolls (the sky stays).
void PaintWeather(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r) {
    const WeatherData d = WeatherService().Data();
    const Sky sky = SkyOf(d.code);
    SkyPlate(rt, r, d.valid ? sky : Sky::Cloudy, d.valid ? d.day : true);
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1), soft = D2D1::ColorF(1, 1, 1, 0.82f);
    const float left = r.left + kPad;
    WidgetScroll& sc = PaintScroll();
    if (!d.valid) {
        sc.min = sc.max = 0;
        Text(rt, docky::ui::Text(L"Weather"), Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, r.top + 14, r.right - kPad, r.top + 34), white);
        Text(rt, d.error.empty() ? docky::ui::Text(L"Loading…") : d.error, Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(left, r.top + 40, r.right - kPad, r.top + 58), soft);
        if (!d.error.empty()) Text(rt, docky::ui::Text(L"Set a city in Widget Settings"), Font(11.5f), D2D1::RectF(left, r.top + 58, r.right - kPad, r.top + 74), soft);
        return;
    }
    float end = r.bottom;
    {
        ScrollView view(rt, r, sc.offset, sc.offset > sc.min + 0.5f, sc.offset < sc.max - 0.5f);
        // Left block (all sizes): place, temperature, condition / H-L.
        const float colRight = size == Size::Small ? r.right - kPad : r.left + 170;
        float y = r.top + 13;
        if (d.automatic) { Text(rt, docky::ui::Text(L"NEARBY LOCATION"), Font(10.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, colRight, y + 13), soft); y += 13; }
        Text(rt, d.place + (d.automatic ? L" ↗" : L""), Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, colRight, y + 20), white);
        Text(rt, Deg(d.temp), Font(44.f, DWRITE_FONT_WEIGHT_LIGHT), D2D1::RectF(left - 2, y + 18, colRight, y + 72), white);
        auto divider = Brush(rt, D2D1::ColorF(1, 1, 1, 0.22f));
        float yy = 0;
        if (size == Size::Small) {
            SkyIcon(rt, sky, d.day, D2D1::Point2F(left + 10, r.bottom - 42), 20.f);
            Text(rt, ConditionText(d.code, d.day), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, r.bottom - 32, r.right - kPad, r.bottom - 16), white);
            Text(rt, L"H:" + Deg(d.high) + L" L:" + Deg(d.low), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, r.bottom - 18, r.right - kPad, r.bottom - 2), white);
            // Below the first screen: the daily forecast.
            yy = r.bottom + 10;
            rt->FillRectangle(D2D1::RectF(left, yy - 5, r.right - kPad, yy - 4.5f), divider.Get());
            for (const auto& day : d.days) yy = WeatherDayRow(rt, d, day, left, r.right - kPad, yy, true);
        } else {
            // Medium / large: condition at the top right, hourly row below, then the daily forecast.
            SkyIcon(rt, sky, d.day, D2D1::Point2F(r.right - kPad - 12, r.top + 22), 22.f);
            Text(rt, ConditionText(d.code, d.day), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left + 180, r.top + 38, r.right - kPad, r.top + 54), white);
            Text(rt, L"H:" + Deg(d.high) + L" L:" + Deg(d.low), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left + 180, r.top + 54, r.right - kPad, r.top + 70), white);
            const float rowTop = r.top + 98, cw = (r.right - r.left - 2 * kPad) / 6;
            // Hourly rows of six: the first under the conditions, the rest of the day below it when scrolled.
            const size_t rows = size == Size::Large ? 1 : (std::min<size_t>)(3, (d.hours.size() + 5) / 6);
            float hy = rowTop;
            for (size_t row = 0; row < rows; ++row, hy += 64) {
                for (size_t c = 0; c < 6; ++c) {
                    const size_t i = row * 6 + c;
                    if (i >= d.hours.size()) break;
                    const float cx = left + cw * (c + 0.5f);
                    Text(rt, HourLabel(d.hours[i].hour, i == 0), Font(11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cx - cw / 2, hy, cx + cw / 2, hy + 14), soft);
                    SkyIcon(rt, SkyOf(d.hours[i].code), d.hours[i].day, D2D1::Point2F(cx, hy + 28), 16.f);
                    Text(rt, Deg(d.hours[i].temp), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cx - cw / 2, hy + 40, cx + cw / 2, hy + 56), white);
                }
            }
            yy = hy + 2;
            rt->FillRectangle(D2D1::RectF(left, yy, r.right - kPad, yy + 0.5f), divider.Get());
            yy += 8;
            for (const auto& day : d.days) yy = WeatherDayRow(rt, d, day, left, r.right - kPad, yy, false);
        }
        end = yy + 6;
    }
    sc.min = 0; sc.max = (std::max)(0.f, end - r.bottom);
}

// ---- Third-party templates ---------------------------------------------------------------------------------
void PaintExt(ID2D1RenderTarget* rt, Size size, D2D1_RECT_F r, const Theme& t, const ExtWidget& e) {
    const D2D1_COLOR_F accent = e.hasAccent ? e.accent : t.accent;
    const Value& d = e.data;
    if (e.templ == L"image" && !e.image.empty()) {
        if (auto bmp = MakeBitmap(rt, e.image.data(), e.imageW, e.imageH)) {
            const float bw = static_cast<float>(e.imageW), bh = static_cast<float>(e.imageH), w = r.right - r.left, h = r.bottom - r.top;
            const float s = (std::max)(w / bw, h / bh);
            ComPtr<ID2D1BitmapBrush> brush;
            rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush);
            const D2D1_SIZE_F bs = bmp->GetSize();
            brush->SetTransform(D2D1::Matrix3x2F::Scale(s * bw / bs.width, s * bh / bs.height) * D2D1::Matrix3x2F::Translation(r.left + (w - bw * s) / 2, r.top + (h - bh * s) / 2));
            auto shape = WidgetShape(r, rt); rt->FillGeometry(shape.Get(), brush.Get());
        }
        if (!e.title.empty()) Text(rt, e.title, Font(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + kPad, r.bottom - 34, r.right - kPad, r.bottom - 14), D2D1::ColorF(1, 1, 1, 0.95f));
        return;
    }
    Caption(rt, e.title, r, accent);
    if (e.templ == L"list") {
        const Value* items = d.Find("items");
        float y = r.top + 38; const float row = 26;
        if (items && items->type == Value::Type::Array) {
            for (const Value& it : items->array) {
                if (y + row > r.bottom - 10) break;
                if (!it.IsObject()) continue;
                float left = r.left + kPad;
                if (const Value* c = it.Find("checked"); c && c->type == Value::Type::Bool) {
                    auto b = Brush(rt, c->boolean ? accent : t.secondary);
                    const auto circle = D2D1::Ellipse(D2D1::Point2F(left + 7, y + row / 2), 7, 7);
                    if (c->boolean) rt->FillEllipse(circle, b.Get()); else rt->DrawEllipse(circle, b.Get(), 1.4f);
                    left += 22;
                }
                const std::wstring detail = Str(it, "detail");
                const float detailW = detail.empty() ? 0 : (std::min)(TextWidth(detail, Font(12.f)) + 4, (r.right - left) * 0.45f);
                Text(rt, Str(it, "title"), Font(13.f, DWRITE_FONT_WEIGHT_MEDIUM), D2D1::RectF(left, y, r.right - kPad - detailW, y + row), t.ink);
                if (!detail.empty()) Text(rt, detail, Font(12.f, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.right - kPad - detailW, y, r.right - kPad, y + row), t.secondary);
                y += row;
            }
        }
        return;
    }
    if (e.templ == L"gauge") {
        const float value = static_cast<float>(Num(d, "value", 0));
        const float ringR = size == Size::Small ? 42.f : 46.f;
        const D2D1_POINT_2F c = size == Size::Small ? D2D1::Point2F((r.left + r.right) / 2, r.top + 96) : D2D1::Point2F(r.left + kPad + ringR + 4, r.top + 96);
        Ring(rt, c, ringR, 7.f, value, accent, t);
        std::wstring label = Str(d, "label"); if (label.empty()) label = std::to_wstring(static_cast<int>(std::lround(value * 100))) + L"%";
        Text(rt, label, Font(20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false), D2D1::RectF(c.x - ringR, c.y - 14, c.x + ringR, c.y + 14), t.ink);
        const std::wstring detail = Str(d, "detail");
        if (size != Size::Small) Text(rt, detail, Font(15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(c.x + ringR + 24, c.y - 12, r.right - kPad, c.y + 12), t.ink);
        else Text(rt, detail, Font(12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(r.left + 8, r.bottom - 26, r.right - 8, r.bottom - 8), t.secondary);
        return;
    }
    // text (default)
    const float headline = size == Size::Small ? 34.f : 40.f;
    Text(rt, Str(d, "headline"), Font(headline, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + kPad, r.top + 34, r.right - kPad, r.top + 34 + headline * 1.35f), t.ink);
    const std::wstring body = Str(d, "body");
    if (!body.empty()) {
        ComPtr<IDWriteTextFormat> wrap;  // body wraps; others are single line
        if (SUCCEEDED(DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"", &wrap))) {
            wrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            Text(rt, body, wrap.Get(), D2D1::RectF(r.left + kPad, r.top + 38 + headline * 1.35f, r.right - kPad, r.bottom - 30), t.ink);
        }
    }
    Text(rt, Str(d, "footnote"), Font(11.f), D2D1::RectF(r.left + kPad, r.bottom - 28, r.right - kPad, r.bottom - 12), t.secondary);
}

bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::ostringstream s; s << f.rdbuf(); out = s.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF) out.erase(0, 3);
    return out.size() < 1024 * 1024;
}
bool ValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 128) return false;
    for (wchar_t c : id) if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L'_')) return false;
    return true;
}
bool ParseObject(const Value& o, ExtWidget& w, bool manifest) {
    w.id = Str(o, "id"); if (!ValidId(w.id)) return false;
    w.title = Str(o, "title"); if (w.title.size() > 40) w.title.resize(40);
    w.templ = Str(o, "template"); if (w.templ != L"list" && w.templ != L"gauge" && w.templ != L"image") w.templ = L"text";
    if (const Value* s = o.Find("size"); s && s->type == Value::Type::String) w.size = ParseSize(s->string);
    if (const Value* d = o.Find("data"); d && d->IsObject()) w.data = *d;
    const std::wstring accent = Str(o, "accent");
    if (accent.size() == 7 && accent[0] == L'#') { w.accent = Hex(static_cast<unsigned>(wcstoul(accent.c_str() + 1, nullptr, 16))); w.hasAccent = true; }
    if (const Value* n = o.Find("notifyHwnd"); n && n->type == Value::Type::Number && n->number > 0) w.notify = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(n->number));
    if (const Value* list = o.Find("settings"); list && list->type == Value::Type::Array) {
        for (const Value& v : list->array) {
            if (!v.IsObject() || w.settings.size() >= 12) continue;
            ExtSetting st;
            if (const Value* k = v.Find("key"); k && k->type == Value::Type::String) st.key = k->string;
            if (st.key.empty() || st.key.size() > 64) continue;
            st.title = Str(v, "title"); if (st.title.empty()) st.title = std::wstring(st.key.begin(), st.key.end());
            if (st.title.size() > 32) st.title.resize(32);
            if (const Value* t = v.Find("type"); t && t->type == Value::Type::String && (t->string == "choice" || t->string == "button")) st.type = t->string;
            if (const Value* op = v.Find("options"); op && op->type == Value::Type::Array)
                for (const Value& x : op->array) if (x.type == Value::Type::String && st.options.size() < 4) { std::wstring n = W(x.string); if (n.size() > 16) n.resize(16); st.options.push_back(n); }
            if (st.type == "choice" && st.options.size() < 2) continue;
            if (const Value* val = v.Find("value")) {
                if (val->type == Value::Type::Bool) st.value = val->boolean ? 1 : 0;
                else if (val->type == Value::Type::Number) st.value = static_cast<int>(val->number);
                else if (val->type == Value::Type::String) for (size_t i = 0; i < st.options.size(); ++i) if (st.options[i] == W(val->string)) st.value = static_cast<int>(i);
            }
            if (st.type == "choice") st.value = std::clamp(st.value, 0, static_cast<int>(st.options.size()) - 1);
            w.settings.push_back(std::move(st));
        }
    }
    if (manifest) { w.command = Str(o, "command"); w.refreshSeconds = static_cast<int>(std::clamp(Num(o, "refresh", 0), 0.0, 86400.0)); if (w.refreshSeconds && w.refreshSeconds < 10) w.refreshSeconds = 10; }
    return true;
}
}  // namespace

IDWriteTextFormat* IconFont(float size) { return Glyphs(size); }

D2D1_SIZE_F SizeDip(Size s) { return s == Size::Small ? D2D1::SizeF(kSmall, kSmall) : s == Size::Medium ? D2D1::SizeF(kWide, kSmall) : D2D1::SizeF(kWide, kTall); }
const char* SizeName(Size s) { return s == Size::Small ? "small" : s == Size::Medium ? "medium" : "large"; }
Size ParseSize(const std::string& n) { return n == "medium" ? Size::Medium : n == "large" ? Size::Large : Size::Small; }

bool ParseWidgetMessage(const std::string& utf8, ExtWidget& out, std::string& type, std::string& error) {
    Value root;
    if (!dock::json::Parse(utf8, root, error)) return false;
    if (!root.IsObject()) { error = "not an object"; return false; }
    const Value* t = root.Find("type");
    type = t && t->type == Value::Type::String ? t->string : std::string();
    if (type == "widget.gallery" || type == "widget.settings") return true;
    if (type == "widget.remove") { out.id = Str(root, "id"); if (!ValidId(out.id)) { error = "invalid id"; return false; } return true; }
    if (type != "widget.set") { error = "unsupported type"; return false; }
    if (!ParseObject(root, out, false)) { error = "invalid id"; return false; }
    return true;
}

std::vector<ExtWidget> LoadWidgetManifests(const std::wstring& root) {
    std::vector<ExtWidget> out;
    WIN32_FIND_DATAW fd{};
    HANDLE find = root.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring folder = root + L"\\" + fd.cFileName;
        std::string text, error; Value manifest;
        if (!ReadAll(folder + L"\\extension.json", text) || !dock::json::Parse(text, manifest, error) || !manifest.IsObject()) continue;
        const Value* list = manifest.Find("widgets");
        if (!list || list->type != Value::Type::Array) continue;
        for (const Value& o : list->array) {
            ExtWidget w;
            if (!o.IsObject() || out.size() >= 64 || !ParseObject(o, w, true)) continue;
            w.fromManifest = true; w.folder = folder; w.notify = nullptr;
            out.push_back(std::move(w));
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

std::vector<Kind> BuiltinKinds() {
    return {{L"clock", L"Clock", Size::Small, L"Clock"}, {L"clock.analog", L"Clock (Analog)", Size::Small, L"Clock"}, {L"calendar", L"Calendar", Size::Medium, L"Calendar"},
            {L"weather", L"Weather", Size::Small, L"Weather"}, {L"battery", L"Batteries", Size::Small, L"Batteries"},
            {L"system", L"System", Size::Small, L"System"},
            {docky::pairing::kDropWidgetKind, L"DockDrop", Size::Small, L"DockDrop"}};
}

WidgetScroll& PaintScroll() { static WidgetScroll scroll; return scroll; }
bool Scrollable(const std::wstring& kind) { return kind == L"calendar" || kind == L"weather"; }

DropWidgetLayout LayoutDropWidget(D2D1_RECT_F r) {
    DropWidgetLayout layout;
    layout.columns = std::max(1, static_cast<int>((r.right - r.left - 24.f) / 44.f));
    layout.rows = r.bottom - r.top > 240.f ? 3 : 1;
    layout.open = D2D1::RectF(r.right - 62.f, r.top + 9.f, r.right - 12.f, r.top + 32.f);
    layout.first = D2D1::RectF(r.left + 14.f, r.top + 39.f, r.left + 54.f, r.top + 80.f);
    layout.preview = D2D1::RectF(r.left + 14.f, r.top + 42.f + layout.rows * 46.f, r.right - 14.f, r.bottom - 33.f);
    layout.footer = D2D1::RectF(r.left + 14.f, r.bottom - 29.f, r.right - 14.f, r.bottom - 11.f);
    layout.previous = D2D1::RectF(r.right - 69.f, r.bottom - 31.f, r.right - 43.f, r.bottom - 9.f);
    layout.next = D2D1::RectF(r.right - 40.f, r.bottom - 31.f, r.right - 14.f, r.bottom - 9.f);
    return layout;
}

D2D1_RECT_F DropWidgetLayout::Tile(int slot) const {
    const float dx = static_cast<float>(slot % columns) * 44.f;
    const float dy = static_cast<float>(slot / columns) * 46.f;
    return D2D1::RectF(first.left + dx, first.top + dy, first.right + dx, first.bottom + dy);
}

static void PaintDropPreview(ID2D1RenderTarget* rt, const std::wstring& text, D2D1_RECT_F r, const Theme& theme) {
    static ComPtr<IDWriteTextFormat> format;
    if (!format && SUCCEEDED(DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f, L"", &format))) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(DWrite()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) format->SetTrimming(&trimming, ellipsis.Get());
    }
    Text(rt, text, format.Get(), r, theme.ink);
}

static void PaintDockDrop(ID2D1RenderTarget* rt, D2D1_RECT_F r, const Theme& theme,
                          const DropWidgetState& state) {
    const auto center = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f - 12.f);
    const auto layout = LayoutDropWidget(r);
    Caption(rt, L"DockDrop", r, theme.secondary);
    ComPtr<ID2D1SolidColorBrush> brush;
    auto tint = theme.accent;
    tint.a = state.hovering ? 0.26f : 0.14f;
    if (FAILED(rt->CreateSolidColorBrush(tint, &brush))) return;
    if (state.entries.empty()) {
        rt->FillEllipse(D2D1::Ellipse(center, 23.f, 23.f), brush.Get());
        brush->SetColor(theme.accent);
        rt->DrawLine(D2D1::Point2F(center.x, center.y - 11.f), D2D1::Point2F(center.x, center.y + 5.f), brush.Get(), 1.8f);
        rt->DrawLine(D2D1::Point2F(center.x - 5.f, center.y), D2D1::Point2F(center.x, center.y + 5.f), brush.Get(), 1.8f);
        rt->DrawLine(D2D1::Point2F(center.x + 5.f, center.y), D2D1::Point2F(center.x, center.y + 5.f), brush.Get(), 1.8f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(center.x - 11.f, center.y + 9.f, center.x + 11.f, center.y + 12.f), 1.5f, 1.5f), brush.Get(), 1.5f);
        Text(rt, docky::ui::Text(L"Drop files or text"), Font(12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER),
            D2D1::RectF(r.left + 8.f, center.y + 31.f, r.right - 8.f, center.y + 49.f), theme.secondary);
    } else {
        const int count = static_cast<int>(state.entries.size());
        const int start = state.page * layout.Capacity();
        const int end = std::min(count, start + layout.Capacity());
        for (int index = start; index < end; ++index) {
            const auto tile = layout.Tile(index - start);
            const auto& item = state.entries[index];
            auto fill = index == state.hovered ? theme.accent : theme.ink;
            fill.a = index == state.hovered ? 0.24f : 0.08f;
            brush->SetColor(fill);
            rt->FillRoundedRectangle(D2D1::RoundedRect(tile, 9.f, 9.f), brush.Get());
            const auto image = D2D1::RectF(tile.left + 7.f, tile.top + 7.f, tile.right - 7.f, tile.bottom - 7.f);
            if (!item.clipboard && !item.icon.empty()) {
                const auto bitmap = MakeBitmap(rt, item.icon.data(), item.iconW, item.iconH);
                if (bitmap) rt->DrawBitmap(bitmap.Get(), image);
            } else if (item.clipboard) {
                const float x = (image.left + image.right) * 0.5f, y = (image.top + image.bottom) * 0.5f;
                brush->SetColor(theme.accent);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(x - 8, y - 7, x + 8, y + 10), 2.5f, 2.5f), brush.Get(), 1.5f);
                rt->FillRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(x - 4, y - 10, x + 4, y - 5), 2.f, 2.f), brush.Get());
                rt->DrawLine(D2D1::Point2F(x - 4, y), D2D1::Point2F(x + 4, y), brush.Get(), 1.25f);
                rt->DrawLine(D2D1::Point2F(x - 4, y + 4), D2D1::Point2F(x + 2, y + 4), brush.Get(), 1.25f);
            } else {
                Text(rt, L"\uE8A5", IconFont(21.f), image, theme.ink);
            }
        }
        if (state.hovered >= start && state.hovered < end) {
            auto box = layout.preview;
            const int visibleRows = (end - start + layout.columns - 1) / layout.columns;
            box.top -= (layout.rows - visibleRows) * 46.f;
            auto fill = theme.ink;
            fill.a = 0.085f;
            brush->SetColor(fill);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 8.f, 8.f), brush.Get());
            PaintDropPreview(rt, state.entries[state.hovered].preview,
                D2D1::RectF(box.left + 7.f, box.top + 4.f, box.right - 7.f, box.bottom - 4.f), theme);
        }
    }
    if (state.hovering || state.focused) {
        auto border = theme.accent;
        border.a = state.hovering ? 0.72f : 0.42f;
        brush->SetColor(border);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(r.left + 2.f, r.top + 2.f, r.right - 2.f, r.bottom - 2.f), 21.f, 21.f), brush.Get(), 1.25f);
    }
    brush->SetColor(tint);
    rt->FillRoundedRectangle(D2D1::RoundedRect(layout.open, 11.5f, 11.5f), brush.Get());
    Text(rt, docky::ui::Text(L"Open"), Font(11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), layout.open, theme.ink);

    auto footer = layout.footer;
    const int pages = (static_cast<int>(state.entries.size()) + layout.Capacity() - 1) / layout.Capacity();
    if (pages > 1) {
        footer.right = layout.previous.left - 3;
        Text(rt, L"\u2039", Font(17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
            layout.previous, state.page > 0 ? theme.ink : theme.secondary);
        Text(rt, L"\u203A", Font(17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
            layout.next, state.page + 1 < pages ? theme.ink : theme.secondary);
    }
    std::wstring status = pages > 1 ? std::to_wstring(state.page + 1) + L" / " + std::to_wstring(pages) :
        state.focused || state.entries.empty() ? L"Ctrl+V to paste" : L"Hover to preview";
    if (state.hovering) status = L"Release to stage";
    else if (state.notice == DropWidgetNotice::InvalidDrop) status = L"Couldn't stage item";
    else if (state.notice == DropWidgetNotice::Unavailable) status = L"DockDrop unavailable";
    else if (state.notice == DropWidgetNotice::EmptyClipboard) status = L"No text to paste";
    Text(rt, status, Font(11.f, DWRITE_FONT_WEIGHT_MEDIUM), footer, state.hovering ? theme.ink : theme.secondary);
}

void PaintWidget(ID2D1RenderTarget* rt, const std::wstring& kind, Size size, D2D1_RECT_F r,
                 const Theme& t, const ExtWidget* ext, const DropWidgetState* drop) {
    if (kind == L"clock") PaintDigital(rt, size, r, t);
    else if (kind == L"clock.analog") PaintAnalog(rt, size, r, t);
    else if (kind == L"calendar") PaintCalendar(rt, size, r, t);
    else if (kind == L"battery") PaintBattery(rt, size, r, t);
    else if (kind == L"weather") PaintWeather(rt, size, r);
    else if (kind == L"system") PaintSystem(rt, size, r, t);
    else if (kind == docky::pairing::kDropWidgetKind) PaintDockDrop(rt, r, t, drop ? *drop : DropWidgetState{});
    else if (ext) PaintExt(rt, size, r, t, *ext);
    else {
        Caption(rt, L"Widget", r, t.secondary);
        Text(rt, L"Waiting for its app\u2026", Font(13.f), D2D1::RectF(r.left + kPad, r.top + 40, r.right - kPad, r.top + 60), t.secondary);
    }
}

int ListRowAt(const ExtWidget& e, Size, D2D1_RECT_F r, D2D1_POINT_2F p) {
    if (e.templ != L"list" || p.y < r.top + 38) return -1;
    const int row = static_cast<int>((p.y - (r.top + 38)) / 26);
    const Value* items = e.data.Find("items");
    return items && items->type == Value::Type::Array && row < static_cast<int>(items->array.size()) ? row : -1;
}

PlateTone PlateToneOf(const std::wstring& kind) {
    if (kind == L"clock") return CurrentPrefs().clockDark ? PlateTone::Black : PlateTone::Light;
    if (kind == L"battery" && CurrentPrefs().batteryGlass) return PlateTone::Glass;
    if (kind == docky::pairing::kDropWidgetKind) return PlateTone::SubtleGradient;
    return PlateTone::Theme;
}

UINT RefreshMs(const std::wstring& kind) {
    if (kind == L"clock" || kind == L"clock.analog") return 1000;
    if (kind == L"calendar") return 30000;
    if (kind == L"weather") return 60000;  // also repainted when new data arrives
    if (kind == L"system") return 2000;
    if (kind == L"battery") return 60000;  // also repainted when the device list changes
    if (kind == docky::pairing::kDropWidgetKind) return 1000;
    return 60000;
}

void SampleSystem() {
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const ULONGLONG i = u(idle), total = u(kernel) + u(user);
        if (lastTotal && total > lastTotal) g_system.cpu = 1.f - static_cast<float>(i - lastIdle) / static_cast<float>(total - lastTotal);
        lastIdle = i; lastTotal = total;
    }
    MEMORYSTATUSEX m{sizeof(m)};
    if (GlobalMemoryStatusEx(&m)) { g_system.memory = m.dwMemoryLoad / 100.f; g_system.memTotal = m.ullTotalPhys; g_system.memUsed = m.ullTotalPhys - m.ullAvailPhys; }
    g_system.cpu = std::clamp(g_system.cpu, 0.f, 1.f);
}

void OpenBuiltin(const std::wstring& kind) {
    const wchar_t* target = kind.rfind(L"clock", 0) == 0 ? L"ms-clock:" : kind == L"calendar" ? L"ms-actioncenter:" : kind == L"battery" ? L"ms-settings:batterysaver" : kind == L"system" ? L"taskmgr.exe" : kind == L"weather" ? L"msnweather:" : nullptr;
    if (target) ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace docky::desk
