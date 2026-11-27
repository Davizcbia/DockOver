#include "status_group.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace dock::statusgroup {
namespace {
using Microsoft::WRL::ComPtr;
constexpr float kPi = 3.14159265358979f;

D2D1_POINT_2F Polar(D2D1_POINT_2F c, float r, float degrees) {
    const float a = degrees * kPi / 180.f;
    return D2D1::Point2F(c.x + r * std::cos(a), c.y + r * std::sin(a));
}

struct Painter {
    ID2D1DeviceContext* dc; IDWriteFactory* dwrite; float opacity; float ink = 1.f;  // 1 white glyphs, 0 black
    float px = 1.f;  // DIPs per device pixel: strokes never go thinner than about one pixel
    ComPtr<ID2D1Factory> factory; ComPtr<ID2D1SolidColorBrush> brush; ComPtr<ID2D1StrokeStyle> round;
    bool Init() {
        dc->GetFactory(&factory);
        if (!factory || FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) return false;
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
        float dpiX = 96.f, dpiY = 96.f; dc->GetDpi(&dpiX, &dpiY); px = dpiX > 0.f ? 96.f / dpiX : 1.f;
        return true;
    }
    void Color(float r, float g, float b, float a) { brush->SetColor(D2D1::ColorF(r, g, b, a * opacity)); }
    void White(float a) { Color(ink, ink, ink, a); }  // the glyph ink (white on dark, black on light)
    // Clockwise arc (y down; 0 degrees = +x, 90 = down), drawn as a fine polyline with round caps.
    void Arc(D2D1_POINT_2F c, float r, float from, float to, float width) {
        if (to - from < 0.5f) return;
        ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
        sink->BeginFigure(Polar(c, r, from), D2D1_FIGURE_BEGIN_HOLLOW);
        for (float a = from + 3.f; a < to; a += 3.f) sink->AddLine(Polar(c, r, a));
        sink->AddLine(Polar(c, r, to));
        sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close();
        dc->DrawGeometry(path.Get(), brush.Get(), width, round.Get());
    }
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, float width) { dc->DrawLine(a, b, brush.Get(), width, round.Get()); }
    void Dot(D2D1_POINT_2F c, float r) { dc->FillEllipse(D2D1::Ellipse(c, r, r), brush.Get()); }
    IDWriteTextFormat* Font(float size, bool bold) {
        static std::map<int, ComPtr<IDWriteTextFormat>> cache;
        const int key = static_cast<int>(size * 8.f) * 2 + (bold ? 1 : 0);
        auto& f = cache[key];
        if (!f && dwrite) {
            dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (std::max)(1.f, size), L"", &f);
            if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        }
        return f.Get();
    }
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, bool bold) {
        if (auto* f = Font(size, bold)) dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
    }
    // Wi-Fi: three concentric arcs over a dot; `base` is the dot, `r` the outer arc's radius.
    void Wifi(D2D1_POINT_2F base, float r, float width, bool connected, bool available) {
        White(available ? (connected ? 1.f : 0.45f) : 0.25f);
        for (int k = 1; k <= 3; ++k) Arc(base, r * k / 3.f, 225.f, 315.f, width);
        Dot(base, width * 0.75f);
    }
};

// ---- the cells -------------------------------------------------------------------------------------------------
// Combined status (iPhone StandBy style), measured 1:1 from the user's reference (294 px wide; all values x s, the
// cell side, from the cell centre): the battery ring is centred 0.0085 above the middle, centre-line radius 0.347,
// stroke 0.058 with round caps, its ends 30.7 degrees below the horizontal on both sides (241 degrees of arc). The four
// volume dots (radius 0.037) sit on the ring's own circle (radius 0.353) at 120, 100, 80 and 60 degrees, so they
// follow its curve across the gap. Wi-Fi fills a 0.333 x 0.245 box in the middle: the Workshop pack's own glyph when it
// has one, else three parts measured from the reference (a filled fan and two arcs around a pivot 0.112 below centre).
void WifiGlyph(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    const float alpha = st.wifi == 2 ? 1.f : st.wifi == 1 ? 0.45f : 0.25f;
    const float boxW = 0.333f * s, boxH = 0.245f * s;
    if (st.wifiIcon && st.wifiW && st.wifiH && st.wifiIcon->size() == static_cast<size_t>(st.wifiW) * st.wifiH) {
        // One bitmap per (device context, pixels): the glyph is cropped to its visible pixels and fitted into the box.
        static ID2D1DeviceContext* cachedDc = nullptr; static const void* cachedData = nullptr;
        static ComPtr<ID2D1Bitmap> bitmap; static D2D1_RECT_F visible{};
        if (cachedDc != p.dc || cachedData != st.wifiIcon.get()) {
            bitmap.Reset(); cachedDc = p.dc; cachedData = st.wifiIcon.get();
            const auto& px = *st.wifiIcon; unsigned x0 = st.wifiW, y0 = st.wifiH, x1 = 0, y1 = 0;
            for (unsigned y = 0; y < st.wifiH; ++y) for (unsigned x = 0; x < st.wifiW; ++x)
                if ((px[static_cast<size_t>(y) * st.wifiW + x] >> 24) > 24) { x0 = (std::min)(x0, x); y0 = (std::min)(y0, y); x1 = (std::max)(x1, x + 1); y1 = (std::max)(y1, y + 1); }
            if (x1 > x0 && y1 > y0) {
                visible = D2D1::RectF(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1));
                p.dc->CreateBitmap(D2D1::SizeU(st.wifiW, st.wifiH), px.data(), st.wifiW * 4,
                                   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap);
            }
        }
        if (bitmap) {
            const float vw = visible.right - visible.left, vh = visible.bottom - visible.top, k = (std::min)(boxW / vw, 1.25f * boxH / vh);  // as wide as the reference glyph
            const float w = vw * k, h = vh * k;
            // A template glyph: its alpha, painted in the ink (black on a light Dock).
            ComPtr<ID2D1BitmapBrush> brush;
            if (SUCCEEDED(p.dc->CreateBitmapBrush(bitmap.Get(), &brush))) {
                const D2D1_RECT_F dest = D2D1::RectF(c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2);
                brush->SetTransform(D2D1::Matrix3x2F::Translation(-visible.left, -visible.top) * D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(dest.left, dest.top));
                brush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                ComPtr<ID2D1Layer> layer;
                p.dc->PushLayer(D2D1::LayerParameters(dest, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.f, brush.Get()), nullptr);
                p.White(alpha); p.dc->FillRectangle(dest, p.brush.Get());
                p.dc->PopLayer();
            }
            return;
        }
    }
    const D2D1_POINT_2F pivot = D2D1::Point2F(c.x, c.y + 0.112f * s);
    const float stroke = 0.044f * s;
    p.White(alpha);
    p.Arc(pivot, 0.2126f * s, 225.f, 315.f, stroke);
    p.Arc(pivot, 0.124f * s, 225.f, 315.f, stroke);
    ComPtr<ID2D1PathGeometry> fan; ComPtr<ID2D1GeometrySink> sink;  // the bottom part: a filled quarter fan, softened
    if (SUCCEEDED(p.factory->CreatePathGeometry(&fan)) && SUCCEEDED(fan->Open(&sink))) {
        const float r = 0.052f * s;
        sink->BeginFigure(pivot, D2D1_FIGURE_BEGIN_FILLED);
        for (float a = 225.f; a <= 315.f; a += 5.f) sink->AddLine(Polar(pivot, r, a));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
        p.dc->FillGeometry(fan.Get(), p.brush.Get());
        p.dc->DrawGeometry(fan.Get(), p.brush.Get(), 0.018f * s, p.round.Get());
    }
}
// The bolt shown while plugged in (the user's reference: 0.17 s tall, 0.12 s wide, centred on the ring's top).
void Bolt(Painter& p, D2D1_POINT_2F c, float h) {
    const float w = h * 0.70f;
    const D2D1_POINT_2F pts[] = {{0.64f, 0.f}, {0.08f, 0.57f}, {0.47f, 0.57f}, {0.36f, 1.f}, {0.92f, 0.43f}, {0.53f, 0.43f}};
    ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(p.factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    auto at = [&](const D2D1_POINT_2F& q) { return D2D1::Point2F(c.x - w / 2 + q.x * w, c.y - h / 2 + q.y * h); };
    sink->BeginFigure(at(pts[0]), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < 6; ++i) sink->AddLine(at(pts[i]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
    p.dc->FillGeometry(path.Get(), p.brush.Get());
    p.dc->DrawGeometry(path.Get(), p.brush.Get(), h * 0.06f, p.round.Get());  // softened corners
}
void Combo(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    const D2D1_POINT_2F rc = D2D1::Point2F(c.x, c.y - 0.0085f * s);
    const float r = 0.347f * s, w = 0.058f * s, from = 149.3f, to = 390.7f, top = 270.f;
    const bool level = st.batteryPresent && st.battery >= 0;
    const bool number = st.showPercent && level, bolt = st.plugged;
    // What sits in the ring's top gap: the level as a number (bold, 0.136 s digits), the bolt, or both side by side.
    const std::wstring digits = number ? std::to_wstring(std::clamp(st.battery, 0, 100)) : std::wstring();
    float boltH = 0.17f * s, textSize = 0.19f * s;  // a 0.19 s font draws 0.136 s tall digits
    if (st.showPercent && st.plugged) { boltH = 0.19f * s; textSize = 0.21f * s; }  // both at once: larger, so both stay legible
    float textW = 0.f;
    if (number) if (auto* f = p.Font(textSize, true)) {
        ComPtr<IDWriteTextLayout> layout;
        if (p.dwrite && SUCCEEDED(p.dwrite->CreateTextLayout(digits.c_str(), static_cast<UINT32>(digits.size()), f, 4 * s, s, &layout))) {
            DWRITE_TEXT_METRICS m{}; layout->GetMetrics(&m); textW = m.width; }
    }
    float boltW = bolt ? boltH * 0.70f : 0.f, spacing = number && bolt ? 0.01f * s : 0.f;
    float content = textW + boltW + spacing;
    // The gap opens at most about 48 degrees each way ("100" with a bolt is shrunk to fit), so the ring stays a ring.
    const float maxContent = 2.f * (r * 0.74f - 0.02f * s - w / 2);
    if (content > maxContent) { const float k = maxContent / content; textSize *= k; textW *= k; boltH *= k; boltW *= k; spacing *= k; content = maxContent; }
    // Gap half-angle: the content plus 0.03 s of air on each side, plus the round cap (w / 2).
    const float gap = content > 0.f ? (std::asin(std::clamp((content / 2 + 0.02f * s + w / 2) / r, 0.f, 0.95f)) * 180.f / kPi) : 0.f;
    // The battery fills from the lower left, clockwise, over both arcs (the gap is skipped).
    const float left = top - gap - from, right = to - (top + gap), total = left + right;
    D2D1_COLOR_F fill = D2D1::ColorF(p.ink, p.ink, p.ink, 1);
    if (st.saver) fill = D2D1::ColorF(1.f, 0.80f, 0.0f, 1.f);                                            // Battery Saver: yellow
    else if (level && st.battery <= 20 && !st.charging) fill = D2D1::ColorF(1.f, 0.27f, 0.23f, 1.f);     // low: red
    else if (st.charging) fill = D2D1::ColorF(0.19f, 0.82f, 0.35f, 1.f);                                 // charging: green
    const float done = level ? total * std::clamp(st.battery, 0, 100) / 100.f : total;  // a desktop PC on mains power: full
    auto segment = [&](float a0, float a1, float filledTo) {  // one arc: track, then the filled part
        if (a1 - a0 <= 0.f) return;
        p.White(level ? 0.25f : 1.f); if (level) p.Arc(rc, r, a0, a1, w);
        const float f = (std::min)(a1, filledTo);
        if (f > a0) { p.Color(fill.r, fill.g, fill.b, 1.f); p.Arc(rc, r, a0, f, w); }
    };
    if (gap > 0.f) { segment(from, top - gap, from + done); segment(top + gap, to, top + gap + (done - left)); }
    else segment(from, to, from + done);
    // The gap's content, centred on the ring line at the top.
    const D2D1_POINT_2F mid = D2D1::Point2F(rc.x, rc.y - r);
    float x = mid.x - content / 2;
    if (bolt) { p.White(1.f); Bolt(p, D2D1::Point2F(x + boltW / 2, mid.y), boltH); x += boltW + spacing; }
    if (number) { p.White(1.f); p.Text(digits, D2D1::RectF(x, mid.y - textSize, x + textW, mid.y + textSize), textSize, true); }
    WifiGlyph(p, c, s, st);
    const int lit = !st.audio || st.muted ? 0 : (std::clamp(st.volume, 0, 100) + 24) / 25;
    for (int i = 0; i < 4; ++i) {  // left to right on the ring's circle
        p.White(i < lit ? 1.f : 0.3f);
        p.Dot(Polar(rc, 0.3534f * s, 120.f - 20.f * i), 0.0374f * s);
    }
}
// ---- Clock and Calendar tiles ----------------------------------------------------------------------------------
// Measured 1:1 from the user's reference (macOS Clock and Calendar icons, plate 902 px; all values x S, the plate side,
// from its centre). Styles: 0 light (white plate, dark marks), 1 dark (black plate, light marks), 2 Liquid Glass (a clear
// glass plate with a bright rim, light marks).
struct Inks { D2D1_COLOR_F hour, minute, number, hand, cap; };
Inks InksFor(int style) {
    if (style == 0) return {D2D1::ColorF(0.25f, 0.25f, 0.25f, 1), D2D1::ColorF(0.78f, 0.78f, 0.78f, 1), D2D1::ColorF(0.22f, 0.22f, 0.22f, 1), D2D1::ColorF(0, 0, 0, 1), D2D1::ColorF(0, 0, 0, 1)};
    return {D2D1::ColorF(1, 1, 1, 0.92f), D2D1::ColorF(1, 1, 1, 0.34f), D2D1::ColorF(1, 1, 1, 0.95f), D2D1::ColorF(1, 1, 1, 1), D2D1::ColorF(1, 1, 1, 1)};
}
// The icon plate: a rounded square (macOS icon corner) with a soft shadow; white or black with a gentle top-to-bottom
// gradient, or Liquid Glass (a faint body, a light sheen at the top and a bright rim that fades toward the bottom).
void TilePlate(Painter& p, D2D1_POINT_2F c, float S, int style) {
    const D2D1_RECT_F r = D2D1::RectF(c.x - S / 2, c.y - S / 2, c.x + S / 2, c.y + S / 2);
    const float rad = 0.225f * S;
    for (int i = 8; i >= 1; --i) {  // a soft shadow: many faint layers so no step shows
        p.Color(0, 0, 0, style == 2 ? 0.010f : 0.016f);
        const float g = i * 0.006f * S;
        p.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - g * 0.6f, r.top + g * 0.4f, r.right + g * 0.6f, r.bottom + g * 1.4f), rad + g, rad + g), p.brush.Get());
    }
    auto gradient = [&](D2D1_COLOR_F top, D2D1_COLOR_F bottom, float y0, float y1) {
        ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> brush;
        const D2D1_GRADIENT_STOP st[2] = {{0.f, top}, {1.f, bottom}};
        if (SUCCEEDED(p.dc->CreateGradientStopCollection(st, 2, &stops)) &&
            SUCCEEDED(p.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(c.x, y0), D2D1::Point2F(c.x, y1)), stops.Get(), &brush)))
            return brush;
        return ComPtr<ID2D1LinearGradientBrush>();
    };
    auto withOpacity = [&](D2D1_COLOR_F k) { k.a *= p.opacity; return k; };
    if (style == 0) {
        if (auto g = gradient(withOpacity(D2D1::ColorF(0.99f, 0.99f, 0.99f, 1)), withOpacity(D2D1::ColorF(0.925f, 0.925f, 0.925f, 1)), r.top, r.bottom))
            p.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.Get());
    } else if (style == 1) {
        if (auto g = gradient(withOpacity(D2D1::ColorF(0.17f, 0.17f, 0.18f, 1)), withOpacity(D2D1::ColorF(0.05f, 0.05f, 0.06f, 1)), r.top, r.bottom))
            p.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.Get());
        p.Color(1, 1, 1, 0.10f);
        p.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), p.brush.Get(), 1.f);
    } else {
        p.Color(1, 1, 1, 0.10f);
        p.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), p.brush.Get());
        if (auto g = gradient(withOpacity(D2D1::ColorF(1, 1, 1, 0.20f)), withOpacity(D2D1::ColorF(1, 1, 1, 0.f)), r.top, r.top + S * 0.55f))
            p.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g.Get());
        if (auto g = gradient(withOpacity(D2D1::ColorF(1, 1, 1, 0.75f)), withOpacity(D2D1::ColorF(1, 1, 1, 0.20f)), r.top, r.bottom)) {
            const float w = (std::max)(1.f, 0.012f * S);
            p.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2), rad, rad), g.Get(), w);
        }
    }
}
// The point where the ray from the centre at `degrees` (0 = 12 o'clock, clockwise) leaves a rounded square of half side
// `h` and corner radius `rr` (the ticks' outer ends follow the plate's shape, not a circle).
D2D1_POINT_2F RoundedRay(D2D1_POINT_2F c, float h, float rr, float degrees) {
    const float a = degrees * kPi / 180.f, dx = std::sin(a), dy = -std::cos(a);
    float t = h / (std::max)(std::fabs(dx), std::fabs(dy));
    const float px = dx * t, py = dy * t;
    if (std::fabs(px) > h - rr && std::fabs(py) > h - rr) {  // in a corner: meet the corner arc
        const float cx = std::copysign(h - rr, dx), cy = std::copysign(h - rr, dy), b = dx * cx + dy * cy;
        t = b + std::sqrt((std::max)(0.f, b * b - (cx * cx + cy * cy) + rr * rr));
    }
    return D2D1::Point2F(c.x + dx * t, c.y + dy * t);
}
IDWriteTextFormat* DisplayFont(Painter& p, float size, DWRITE_FONT_WEIGHT weight) {
    static std::map<long long, ComPtr<IDWriteTextFormat>> cache;
    auto& f = cache[static_cast<long long>(size * 8.f) * 1000 + weight];
    if (!f && p.dwrite) {
        p.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (std::max)(1.f, size), L"", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    }
    return f.Get();
}
void Centered(Painter& p, const std::wstring& text, D2D1_POINT_2F at, float size, DWRITE_FONT_WEIGHT weight, ID2D1Brush* brush) {
    if (auto* f = DisplayFont(p, size, weight))
        p.dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), f, D2D1::RectF(at.x - size * 3, at.y - size, at.x + size * 3, at.y + size), brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}
void AnalogClock(Painter& p, D2D1_POINT_2F c, float S, const State& st) {
    TilePlate(p, c, S, st.tileStyle);
    const Inks ink = InksFor(st.tileStyle);
    auto color = [&](D2D1_COLOR_F k) { p.Color(k.r, k.g, k.b, k.a); };
    // 60 ticks pointing at the centre; outer ends on a rounded square 0.466 S out whose corners follow the tile's own
    // (0.225 S radius, inset by 0.034 S), so every tick sits the same distance from the tile's edge.
    // At Dock size (about 46-70 device pixels) hairlines turn into grey smears: strokes keep at least ~1 pixel, the
    // faint minute ticks are left out below 80 pixels, and the numbers grow a little and get heavier.
    const float devS = S / p.px, minW = 1.05f * p.px;
    const bool compact = devS < 80.f, minuteTicks = devS >= 80.f;
    const float h = 0.466f * S, rr = (0.225f - (0.5f - 0.466f)) * S;
    for (int i = 0; i < 60; ++i) {
        const float deg = i * 6.f;
        const bool hour = i % 5 == 0, cardinal = i % 15 == 0;
        if (!hour && !minuteTicks) continue;
        const D2D1_POINT_2F outer = RoundedRay(c, h, rr, deg);
        const float len = cardinal ? 0.100f * S : hour ? 0.166f * S : 0.069f * S;
        const float dx = c.x - outer.x, dy = c.y - outer.y, d = std::hypot(dx, dy);
        const D2D1_POINT_2F inner = D2D1::Point2F(outer.x + dx / d * len, outer.y + dy / d * len);
        color(hour ? ink.hour : ink.minute);
        p.dc->DrawLine(outer, inner, p.brush.Get(), (std::max)((hour ? (compact ? 0.022f : 0.0155f) : 0.011f) * S, minW), p.round.Get());
    }
    color(ink.number);
    const float ns = (compact ? 0.168f : 0.150f) * S;  // 0.107 S tall digits (0.12 S when compact)
    const DWRITE_FONT_WEIGHT nw = compact ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM;
    const float ny = compact ? 0.278f : 0.285f, nx = compact ? 0.290f : 0.296f;
    Centered(p, L"12", D2D1::Point2F(c.x, c.y - ny * S), ns, nw, p.brush.Get());
    Centered(p, L"3", D2D1::Point2F(c.x + nx * S, c.y), ns, nw, p.brush.Get());
    Centered(p, L"6", D2D1::Point2F(c.x, c.y + ny * S), ns, nw, p.brush.Get());
    Centered(p, L"9", D2D1::Point2F(c.x - nx * S, c.y), ns, nw, p.brush.Get());
    // Hands: thin near the centre, then the thick blade; the orange second hand runs through with a short tail.
    auto dir = [](float deg) { const float a = deg * kPi / 180.f; return D2D1::Point2F(std::sin(a), -std::cos(a)); };
    auto hand = [&](float deg, float thin, float length, float width) {
        const auto d = dir(deg);
        color(ink.hand);
        p.dc->DrawLine(c, D2D1::Point2F(c.x + d.x * thin, c.y + d.y * thin), p.brush.Get(), (std::max)(0.018f * S, minW), p.round.Get());
        p.dc->DrawLine(D2D1::Point2F(c.x + d.x * thin, c.y + d.y * thin), D2D1::Point2F(c.x + d.x * length, c.y + d.y * length), p.brush.Get(), width, p.round.Get());
    };
    const float sec = static_cast<float>(st.second), min = st.minute + sec / 60.f, hr = (st.hour % 12) + min / 60.f;
    hand(hr * 30.f, 0.075f * S, 0.240f * S, 0.044f * S);
    hand(min * 6.f, 0.075f * S, 0.400f * S, 0.044f * S);
    color(ink.cap); p.dc->FillEllipse(D2D1::Ellipse(c, 0.030f * S, 0.030f * S), p.brush.Get());
    const auto sd = dir(sec * 6.f);
    p.Color(1.f, 0.584f, 0.f, 1.f);
    p.dc->DrawLine(D2D1::Point2F(c.x - sd.x * 0.066f * S, c.y - sd.y * 0.066f * S), D2D1::Point2F(c.x + sd.x * 0.405f * S, c.y + sd.y * 0.405f * S), p.brush.Get(), (std::max)(0.013f * S, minW), p.round.Get());
    p.dc->FillEllipse(D2D1::Ellipse(c, (std::max)(0.0175f * S, 1.2f * p.px), (std::max)(0.0175f * S, 1.2f * p.px)), p.brush.Get());
    if (st.tileStyle == 0) p.Color(1, 1, 1, 1); else p.Color(0.1f, 0.1f, 0.1f, 1);
    p.dc->FillEllipse(D2D1::Ellipse(c, 0.0075f * S, 0.0075f * S), p.brush.Get());
}
void CalendarDate(Painter& p, D2D1_POINT_2F c, float S, const State& st) {
    TilePlate(p, c, S, st.tileStyle);
    // The weekday: bold red, 0.144 S cap height, centred 0.288 S above the middle.
    p.Color(1.f, 0.23f, 0.19f, 1.f);
    Centered(p, st.weekday.empty() ? L"Wed" : st.weekday, D2D1::Point2F(c.x, c.y - 0.288f * S), 0.200f * S, DWRITE_FONT_WEIGHT_BOLD, p.brush.Get());
    // The day: 0.40 S tall digits, centred 0.085 S below the middle (clear of the bottom edge), glossy black (light) or
    // white (dark / glass).
    const std::wstring day = std::to_wstring(std::clamp(st.day, 1, 31));
    const float size = 0.585f * S;
    const D2D1_POINT_2F at = D2D1::Point2F(c.x, c.y + 0.085f * S);
    // The reference's digits are narrower than Segoe's: compress them 8 % horizontally about their centre.
    D2D1_MATRIX_3X2_F saved{}; p.dc->GetTransform(&saved);
    p.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.86f, 1.f, at) * saved);
    for (int i = 1; i <= (S / p.px >= 120.f ? 4 : 0); ++i) {  // a soft shadow under the digits (large sizes only: sub-pixel copies smear small digits)
        p.Color(0, 0, 0, st.tileStyle == 0 ? 0.035f : 0.06f);
        Centered(p, day, D2D1::Point2F(at.x + (i - 2.5f) * 0.003f * S, at.y + i * 0.006f * S), size, DWRITE_FONT_WEIGHT_NORMAL, p.brush.Get());
    }
    ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> fill;
    const D2D1_GRADIENT_STOP light[2] = {{0.f, D2D1::ColorF(0.30f, 0.30f, 0.30f, p.opacity)}, {1.f, D2D1::ColorF(0.10f, 0.10f, 0.10f, p.opacity)}};
    const D2D1_GRADIENT_STOP bright[2] = {{0.f, D2D1::ColorF(1, 1, 1, p.opacity)}, {1.f, D2D1::ColorF(0.80f, 0.80f, 0.82f, p.opacity)}};
    if (SUCCEEDED(p.dc->CreateGradientStopCollection(st.tileStyle == 0 ? light : bright, 2, &stops)) &&
        SUCCEEDED(p.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(at.x, at.y - 0.23f * S), D2D1::Point2F(at.x, at.y + 0.23f * S)), stops.Get(), &fill)))
        Centered(p, day, at, size, S / p.px < 80.f ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_NORMAL, fill.Get());  // a touch heavier at Dock size
    p.dc->SetTransform(saved);
}
void ControlCenter(Painter& p, D2D1_POINT_2F c, float s) {  // two stacked switches with knobs on opposite ends
    const float w = 0.46f * s, h = 0.20f * s, lw = 0.045f * s;
    for (int i = 0; i < 2; ++i) {
        const float y = c.y + (i ? 0.12f : -0.12f) * s;
        p.White(1.f);
        p.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - w / 2, y - h / 2, c.x + w / 2, y + h / 2), h / 2, h / 2), p.brush.Get(), lw);
        p.Dot(D2D1::Point2F(i ? c.x + w / 2 - h / 2 : c.x - w / 2 + h / 2, y), h * 0.32f);
    }
}
void Bluetooth(Painter& p, D2D1_POINT_2F c, float s, bool on) {
    const float x = c.x - 0.015f * s, half = 0.24f * s, wing = 0.12f * s;
    p.White(on ? 1.f : .35f);
    const D2D1_POINT_2F pts[6] = {{x - wing, c.y - half * 0.5f}, {x + wing, c.y + half * 0.5f}, {x, c.y + half}, {x, c.y - half}, {x + wing, c.y - half * 0.5f}, {x - wing, c.y + half * 0.5f}};
    // One open polyline with round caps and round joins (rounded corners at the arrow tips), slightly heavier.
    ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(p.factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW); sink->AddLines(pts + 1, 5); sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close();
    p.dc->DrawGeometry(path.Get(), p.brush.Get(), 0.062f * s, p.round.Get());
}
void Sound(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    const float x = c.x - 0.10f * s;
    p.White(st.audio ? 1.f : 0.35f);
    ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(p.factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(x - 0.12f * s, c.y - 0.07f * s), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F body[] = {{x - 0.04f * s, c.y - 0.07f * s}, {x + 0.06f * s, c.y - 0.17f * s}, {x + 0.06f * s, c.y + 0.17f * s},
                                      {x - 0.04f * s, c.y + 0.07f * s}, {x - 0.12f * s, c.y + 0.07f * s}};
        sink->AddLines(body, 5); sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
        p.dc->FillGeometry(path.Get(), p.brush.Get());
    }
    if (!st.audio) return;
    if (st.muted) { p.Line(D2D1::Point2F(x + 0.13f * s, c.y - 0.08f * s), D2D1::Point2F(x + 0.29f * s, c.y + 0.08f * s), 0.045f * s);
                    p.Line(D2D1::Point2F(x + 0.29f * s, c.y - 0.08f * s), D2D1::Point2F(x + 0.13f * s, c.y + 0.08f * s), 0.045f * s); return; }
    const int waves = st.volume <= 0 ? 0 : st.volume < 34 ? 1 : st.volume < 67 ? 2 : 3;
    for (int i = 0; i < waves; ++i) p.Arc(D2D1::Point2F(x + 0.04f * s, c.y), (0.12f + 0.08f * i) * s, -45.f, 45.f, 0.045f * s);
}
void Battery(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    const float w = 0.52f * s, h = 0.26f * s, lw = 0.035f * s;
    const D2D1_RECT_F body = D2D1::RectF(c.x - w / 2 - 0.02f * s, c.y - h / 2, c.x + w / 2 - 0.02f * s, c.y + h / 2);
    p.White(0.55f);
    p.dc->DrawRoundedRectangle(D2D1::RoundedRect(body, h * 0.3f, h * 0.3f), p.brush.Get(), lw);
    p.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right + 0.025f * s, c.y - h * 0.22f, body.right + 0.055f * s, c.y + h * 0.22f), lw, lw), p.brush.Get());
    const float pct = st.batteryPresent && st.battery >= 0 ? std::clamp(st.battery, 0, 100) / 100.f : 1.f;
    if (st.charging) p.Color(0.19f, 0.82f, 0.35f, 1.f); else if (st.batteryPresent && st.battery >= 0 && st.battery <= 20) p.Color(1.f, 0.27f, 0.23f, 1.f); else p.White(1.f);
    const float in = lw * 1.6f;
    if (pct > 0.f) p.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + in, body.top + in, body.left + in + (w - 2 * in) * pct, body.bottom - in), h * 0.18f, h * 0.18f), p.brush.Get());
}
void Input(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    const std::wstring badge = st.inputBadge.empty() ? L"A" : st.inputBadge;
    const float w = (badge.size() > 1 ? 0.58f : 0.46f) * s, h = 0.42f * s;
    const D2D1_RECT_F box = D2D1::RectF(c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2);
    p.White(1.f);
    p.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 0.09f * s, 0.09f * s), p.brush.Get(), 0.04f * s);
    p.Text(badge, box, (badge.size() > 1 ? 0.20f : 0.27f) * s, badge.size() > 1);
}
void Clock(Painter& p, D2D1_POINT_2F c, float s, const State& st) {
    p.White(1.f);
    p.Text(st.time, D2D1::RectF(c.x - s / 2, c.y - s / 2, c.x + s / 2, c.y + s / 2), (st.time.size() > 4 ? 0.27f : 0.31f) * s, true);
}
void Tray(Painter& p, D2D1_POINT_2F c, float s) {  // the notification area: an upward chevron over a short tray line
    p.White(1.f);
    const float w = 0.045f * s;
    p.Line(D2D1::Point2F(c.x - 0.15f * s, c.y - 0.01f * s), D2D1::Point2F(c.x, c.y - 0.14f * s), w);
    p.Line(D2D1::Point2F(c.x, c.y - 0.14f * s), D2D1::Point2F(c.x + 0.15f * s, c.y - 0.01f * s), w);
    p.White(0.75f);
    p.Line(D2D1::Point2F(c.x - 0.20f * s, c.y + 0.13f * s), D2D1::Point2F(c.x + 0.20f * s, c.y + 0.13f * s), w);
}
// A tray icon's pixels as a D2D bitmap, cached per pixel buffer (a new buffer arrives only when the app changes its icon).
ID2D1Bitmap1* TrayBitmap(Painter& p, const State::TrayImage& image) {
    if (!image.pixels || !image.w || !image.h || image.pixels->size() != static_cast<size_t>(image.w) * image.h) return nullptr;
    struct Cached { std::weak_ptr<const std::vector<std::uint32_t>> source; ComPtr<ID2D1Bitmap1> bitmap; };
    // The cache holds a reference to its device context: a new context (device recreated) can then never reuse the old
    // one's address and be handed bitmaps of another device.
    static std::map<const void*, Cached> cache; static ComPtr<ID2D1DeviceContext> cacheDc;
    if (cacheDc.Get() != p.dc) { cache.clear(); cacheDc = p.dc; }
    const void* key = image.pixels.get();
    if (const auto it = cache.find(key); it != cache.end() && it->second.bitmap && !it->second.source.expired()) return it->second.bitmap.Get();
    if (cache.size() > 96) cache.clear();
    Cached fresh; fresh.source = image.pixels;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(p.dc->CreateBitmap(D2D1::SizeU(image.w, image.h), image.pixels->data(), image.w * 4, &props, &fresh.bitmap))) return nullptr;
    return (cache[key] = std::move(fresh)).bitmap.Get();
}
// `image` fitted into a `side` square centred on `c`, on whole device pixels.
void DrawTrayImage(Painter& p, const State::TrayImage& image, D2D1_POINT_2F c, float side) {
    ID2D1Bitmap1* bitmap = TrayBitmap(p, image);
    if (!bitmap) return;
    const float k = side / static_cast<float>((std::max)(image.w, image.h));
    const float w = (std::max)(p.px, std::round(image.w * k / p.px) * p.px), h = (std::max)(p.px, std::round(image.h * k / p.px) * p.px);
    const float x = std::round((c.x - w / 2) / p.px) * p.px, y = std::round((c.y - h / 2) / p.px) * p.px;
    p.dc->DrawBitmap(bitmap, D2D1::RectF(x, y, x + w, y + h), p.opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
}
// Hidden Icons as an iOS folder: the Clock / Calendar plate holding up to nine of the hidden tray icons in a 3 x 3 grid,
// each on a small rounded chip like a miniature app icon (many tray icons are white glyphs, which a light plate would
// swallow). Nothing to preview (the menu bar holds the tray, or no icons): a large chevron on the plate.
void HiddenFolder(Painter& p, D2D1_POINT_2F c, float S, const State& st) {
    TilePlate(p, c, S, st.tileStyle);
    const Inks ink = InksFor(st.tileStyle);
    if (st.trayPreview.empty()) {
        p.Color(ink.number.r, ink.number.g, ink.number.b, ink.number.a);
        const float w = (std::max)(0.062f * S, 1.2f * p.px);
        p.Line(D2D1::Point2F(c.x - 0.17f * S, c.y + 0.075f * S), D2D1::Point2F(c.x, c.y - 0.095f * S), w);
        p.Line(D2D1::Point2F(c.x, c.y - 0.095f * S), D2D1::Point2F(c.x + 0.17f * S, c.y + 0.075f * S), w);
        return;
    }
    const float pad = 0.15f * S, gap = 0.05f * S, cell = (S - 2 * pad - 2 * gap) / 3.f;
    const auto snap = [&](float v) { return std::round(v / p.px) * p.px; };
    for (size_t i = 0; i < st.trayPreview.size() && i < 9; ++i) {
        const float x = snap(c.x - S / 2 + pad + static_cast<float>(i % 3) * (cell + gap)), y = snap(c.y - S / 2 + pad + static_cast<float>(i / 3) * (cell + gap));
        const float side = snap(cell);
        if (st.tileStyle == 0) p.Color(0.22f, 0.22f, 0.24f, 1.f); else if (st.tileStyle == 1) p.Color(0.30f, 0.30f, 0.32f, 1.f); else p.Color(1.f, 1.f, 1.f, 0.20f);
        p.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + side, y + side), 0.24f * side, 0.24f * side), p.brush.Get());
        DrawTrayImage(p, st.trayPreview[i], D2D1::Point2F(x + side / 2, y + side / 2), side * 0.72f);
    }
}
// Digital time and date (Lock Screen / StandBy style) on the Clock / Calendar plate: the weekday in Calendar red and the
// day above, the time large below in a heavy display face, narrowed a little like Apple's digits.
void DigitalClock(Painter& p, D2D1_POINT_2F c, float S, const State& st) {
    TilePlate(p, c, S, st.tileStyle);
    const Inks ink = InksFor(st.tileStyle);
    const bool compact = S / p.px < 80.f;
    // Top line: "Wed 18" - weekday red, day in the plate's ink, as one centred run.
    const std::wstring weekday = st.weekday.empty() ? L"Wed" : st.weekday, day = std::to_wstring(std::clamp(st.day, 1, 31));
    const float ds = (compact ? 0.205f : 0.185f) * S, dy = c.y - 0.215f * S;
    if (auto* f = DisplayFont(p, ds, DWRITE_FONT_WEIGHT_BOLD)) {
        ComPtr<IDWriteTextLayout> wl, nl;
        const std::wstring gap = L" ";
        if (p.dwrite && SUCCEEDED(p.dwrite->CreateTextLayout((weekday + gap).c_str(), static_cast<UINT32>(weekday.size() + 1), f, 4 * S, ds * 2, &wl)) &&
            SUCCEEDED(p.dwrite->CreateTextLayout(day.c_str(), static_cast<UINT32>(day.size()), f, 4 * S, ds * 2, &nl))) {
            DWRITE_TEXT_METRICS a{}, b{}; wl->GetMetrics(&a); nl->GetMetrics(&b);
            const float total = a.widthIncludingTrailingWhitespace + b.width, left = c.x - total / 2;
            // The layouts centre their text in a 4 S box: move each so its run starts where it should.
            p.Color(1.f, 0.23f, 0.19f, 1.f);
            p.dc->DrawTextLayout(D2D1::Point2F(left - a.left, dy - ds), wl.Get(), p.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            p.Color(ink.number.r, ink.number.g, ink.number.b, ink.number.a);
            p.dc->DrawTextLayout(D2D1::Point2F(left + a.widthIncludingTrailingWhitespace - b.left, dy - ds), nl.Get(), p.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }
    }
    // The time: as wide as the plate allows ("12:45" fills about 0.86 S), narrowed 12 % about its centre.
    const std::wstring time = st.time.empty() ? L"9:41" : st.time;
    const float ts = (time.size() > 4 ? 0.415f : 0.46f) * S;
    const D2D1_POINT_2F at = D2D1::Point2F(c.x, c.y + 0.115f * S);
    D2D1_MATRIX_3X2_F saved{}; p.dc->GetTransform(&saved);
    p.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.88f, 1.f, at) * saved);
    p.Color(ink.number.r, ink.number.g, ink.number.b, ink.number.a);
    Centered(p, time, at, ts, compact ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, p.brush.Get());
    p.dc->SetTransform(saved);
}
// A tray icon kept in the Dock: the app's live icon at a little over half the cell, on whole device pixels.
// Not running: a dim placeholder.
void TrayIcon(Painter& p, D2D1_POINT_2F c, float s, const State& st, const std::wstring& id) {
    const auto found = st.trayIcons.find(id);
    if (found == st.trayIcons.end() || !TrayBitmap(p, found->second)) {
        const float h = 0.44f * s;
        const D2D1_RECT_F box = D2D1::RectF(c.x - h / 2, c.y - h / 2, c.x + h / 2, c.y + h / 2);
        p.White(0.45f);
        p.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 0.1f * s, 0.1f * s), p.brush.Get(), 0.04f * s);
        std::wstring letter = CellLabel(id).substr(0, 1);
        for (auto& ch : letter) ch = static_cast<wchar_t>(towupper(ch));
        p.Text(letter, box, 0.26f * s, true);
        return;
    }
    DrawTrayImage(p, found->second, c, std::round(0.58f * s / p.px) * p.px);
}
void Extension(Painter& p, D2D1_POINT_2F c, float s, const std::wstring& id) {
    const float h = 0.44f * s;
    const D2D1_RECT_F box = D2D1::RectF(c.x - h / 2, c.y - h / 2, c.x + h / 2, c.y + h / 2);
    p.White(0.85f);
    p.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 0.1f * s, 0.1f * s), p.brush.Get(), 0.04f * s);
    std::wstring letter = id.size() > 4 ? id.substr(4, 1) : L"?";
    for (auto& ch : letter) ch = static_cast<wchar_t>(towupper(ch));
    p.Text(letter, box, 0.26f * s, true);
}
}  // namespace

namespace {
// Packing: columns of two half-size rows; "combo" takes two whole columns starting at an empty column.
struct Slot { int col = 0, row = 0, cols = 1, rows = 1; };
std::vector<Slot> Pack(const std::vector<std::wstring>& cells, int& columns) {
    std::vector<Slot> out; out.reserve(cells.size());
    int col = 0, row = 0;  // next free half-cell
    for (const auto& id : cells) {
        if (FullCell(id)) {
            if (row != 0) { ++col; row = 0; }  // a half-filled column stays as it is
            const int width = id == L"system" ? 6 : 2;
            out.push_back({col, 0, width, 2}); col += width;
        } else {
            out.push_back({col, row, 1, 1});
            if (++row == 2) { row = 0; ++col; }
        }
    }
    columns = col + (row != 0 ? 1 : 0);
    return out;
}
}  // namespace

float WidthFactor(const std::vector<std::wstring>& cells) {
    int columns = 0; Pack(cells, columns);
    return 0.5f * static_cast<float>(columns);
}

D2D1_RECT_F CellRect(D2D1_RECT_F body, const std::vector<std::wstring>& cells, size_t index) {
    int columns = 0; const auto slots = Pack(cells, columns);
    if (index >= slots.size() || columns <= 0) return D2D1::RectF(0, 0, 0, 0);
    const float cw = (body.right - body.left) / static_cast<float>(columns), ch = (body.bottom - body.top) / 2.f;
    const Slot& s = slots[index];
    return D2D1::RectF(body.left + cw * s.col, body.top + ch * s.row, body.left + cw * (s.col + s.cols), body.top + ch * (s.row + s.rows));
}

int CellAt(D2D1_RECT_F body, const std::vector<std::wstring>& cells, D2D1_POINT_2F p) {
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto r = CellRect(body, cells, i);
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

bool FullCell(const std::wstring& id) { return id == L"system" || id == L"combo" || PlateCell(id); }
bool PlateCell(const std::wstring& id) { return id == L"analog" || id == L"date" || id == L"digital" || id == L"tray"; }

std::wstring CellLabel(const std::wstring& id) {
    if (id == L"system") return L"Windows system status";
    if (id == L"combo") return L"Control Center";
    if (id == L"analog") return L"Clock";
    if (id == L"date") return L"Calendar";
    if (id == L"digital") return L"Date & Time";
    if (id == L"control") return L"Control Center";
    if (id == L"wifi") return L"Wi-Fi";
    if (id == L"bluetooth") return L"Bluetooth";
    if (id == L"sound") return L"Sound";
    if (id == L"battery") return L"Battery";
    if (id == L"input") return L"Input Menu";
    if (id == L"clock") return L"Digital Clock";
    if (id == L"tray") return L"Hidden Icons";
    if (id.rfind(L"ext:", 0) == 0) return id.substr(4);
    if (TrayCell(id)) {  // "tray:clash-verge.exe#1" -> "clash-verge"
        std::wstring name = id.substr(5, id.find(L'#') == std::wstring::npos ? std::wstring::npos : id.find(L'#') - 5);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
        return name.empty() ? std::wstring(L"Tray Icon") : name;
    }
    return id;
}

const wchar_t* SystemStatusPart(D2D1_RECT_F body, D2D1_POINT_2F point) {
    const float x = (point.x - body.left) / (std::max)(1.f, body.right - body.left);
    if (x < 0.155f) return L"wifi";
    if (x < 0.30f) return L"sound";
    if (x < 0.445f) return L"battery";
    if (x < 0.59f) return L"bluetooth";
    return L"digital";
}

void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, D2D1_RECT_F body, const std::vector<std::wstring>& cells,
          const State& st, int hover, float opacity) {
    if (!dc || cells.empty()) return;
    Painter p{dc, dwrite, opacity};
    if (!p.Init()) return;
    p.ink = st.dark ? 1.f : 0.f;
    // No plate: each cell sits on a faint, soft halo that fades out from its centre (a blurred shadow on a dark Dock,
    // a soft light veil on a light one), so the glyphs read over any backdrop without a visible frame.
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i] == L"system" || PlateCell(cells[i])) continue;  // these carry their own plate
        const auto r = CellRect(body, cells, i);
        const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        const float rx = (r.right - r.left) * 0.62f, ry = (r.bottom - r.top) * 0.62f, a = (st.dark ? 0.30f : 0.42f) * opacity;
        const float v = st.dark ? 0.f : 1.f;
        const D2D1_GRADIENT_STOP stops[4] = {{0.f, D2D1::ColorF(v, v, v, a)}, {0.45f, D2D1::ColorF(v, v, v, a * 0.75f)}, {0.75f, D2D1::ColorF(v, v, v, a * 0.3f)}, {1.f, D2D1::ColorF(v, v, v, 0.f)}};
        ComPtr<ID2D1GradientStopCollection> collection; ComPtr<ID2D1RadialGradientBrush> halo;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(0, 0), rx, ry), collection.Get(), &halo)))
            dc->FillEllipse(D2D1::Ellipse(c, rx, ry), halo.Get());
    }
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto r = CellRect(body, cells, i);
        const float s = (std::min)(r.right - r.left, r.bottom - r.top);
        const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        if (static_cast<int>(i) == hover) {  // hovered cell: a soft glow in the ink colour, fading out like the halo
            const D2D1_GRADIENT_STOP stops[2] = {{0.f, D2D1::ColorF(p.ink, p.ink, p.ink, 0.16f * opacity)}, {1.f, D2D1::ColorF(p.ink, p.ink, p.ink, 0.f)}};
            ComPtr<ID2D1GradientStopCollection> collection; ComPtr<ID2D1RadialGradientBrush> glow;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)) &&
                SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(0, 0), s * 0.55f, s * 0.55f), collection.Get(), &glow)))
                dc->FillEllipse(D2D1::Ellipse(c, s * 0.55f, s * 0.55f), glow.Get());
        }
        const std::wstring& id = cells[i];
        // The tiles land on whole device pixels (a plate edge between two pixels reads as blur).
        const auto snap = [&](float v) { return std::round(v / p.px) * p.px; };
        const float ts = snap(s), tl = snap(c.x - s / 2), tt = snap(c.y - s / 2);
        const D2D1_POINT_2F tc = D2D1::Point2F(tl + ts / 2, tt + ts / 2);
        if (id == L"system") {
            const float w = r.right - r.left;
            const float glyph = s * .56f;
            const auto place = [&](float x) { return D2D1::Point2F(r.left + w * x, c.y); };
            const auto wifi = place(.085f);
            p.Wifi(D2D1::Point2F(wifi.x, wifi.y + .16f * glyph), .36f * glyph, .06f * glyph, st.wifi == 2, st.wifi != 0);
            Sound(p, place(.225f), glyph, st);
            if (st.batteryPresent) Battery(p, place(.37f), glyph, st);
            else ControlCenter(p, place(.37f), glyph);
            Bluetooth(p, place(.51f), glyph, st.bluetoothOn);
            p.White(.25f);
            p.Line({r.left + w * .59f, c.y - s * .21f}, {r.left + w * .59f, c.y + s * .21f}, (std::max)(p.px, .6f));
            p.White(1.f);
            const float left = r.left + w * .62f, right = r.right - w * .025f;
            p.Text(st.time, {left, c.y - s * .36f, right, c.y}, s * .27f, true);
            p.Text(st.shortDate, {left, c.y, right, c.y + s * .36f}, s * .22f, false);
        } else if (id == L"combo") Combo(p, c, s * 1.28f, st);  // the ring reaches the icon edge, as large as an app icon
        else if (id == L"analog") AnalogClock(p, tc, ts, st);
        else if (id == L"date") CalendarDate(p, tc, ts, st);
        else if (id == L"digital") DigitalClock(p, tc, ts, st);
        else if (id == L"tray") HiddenFolder(p, tc, ts, st);
        else if (id == L"control") ControlCenter(p, c, s);
        else if (id == L"wifi") p.Wifi(D2D1::Point2F(c.x, c.y + 0.16f * s), 0.36f * s, 0.06f * s, st.wifi == 2, st.wifi != 0);
        else if (id == L"bluetooth") Bluetooth(p, c, s, st.bluetoothOn);
        else if (id == L"sound") Sound(p, c, s, st);
        else if (id == L"battery") Battery(p, c, s, st);
        else if (id == L"input") Input(p, c, s, st);
        else if (id == L"clock") Clock(p, c, s, st);
        else if (id == L"tray") Tray(p, c, s);
        else if (TrayCell(id)) TrayIcon(p, c, s, st, id);
        else Extension(p, c, s, id);
    }
}

}  // namespace dock::statusgroup
