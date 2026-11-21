#pragma once

#include <d2d1_1.h>
#include <string_view>
#include <initializer_list>

namespace docky::audio_output {

enum class Shape { None, Speaker, Stereo, Monitor, TV, WiredBuds, WirelessBuds, Headphones, WiredHeadphones, Max, LongStem, ShortStem, Pro };

struct Preset {
    const char* key;
    const wchar_t* name;
    Shape shape;
};

inline constexpr Preset presets[] = {
    {"", L"Default", Shape::None},
    {"speaker", L"Single speaker", Shape::Speaker},
    {"stereo", L"Dual speakers", Shape::Stereo},
    {"tv", L"TV", Shape::TV},
    {"wired-earbuds", L"Wired earbuds", Shape::WiredBuds},
    {"wireless-earbuds", L"Wireless earbuds", Shape::WirelessBuds},
    {"headphones", L"Over-ear headphones", Shape::Headphones},
    {"wired-headphones", L"Wired over-ear headphones", Shape::WiredHeadphones},
    {"airpods-1", L"AirPods 1", Shape::LongStem},
    {"airpods-2", L"AirPods 2", Shape::LongStem},
    {"airpods-3", L"AirPods 3", Shape::ShortStem},
    {"airpods-4", L"AirPods 4", Shape::ShortStem},
    {"airpods-4-anc", L"AirPods 4 (ANC)", Shape::ShortStem},
    {"airpods-5", L"AirPods 5", Shape::ShortStem},
    {"airpods-5-wireless", L"AirPods 5 (Wireless Charging Case)", Shape::ShortStem},
    {"airpods-pro-1", L"AirPods Pro 1", Shape::Pro},
    {"airpods-pro-2-lightning", L"AirPods Pro 2 (Lightning)", Shape::Pro},
    {"airpods-pro-2-usbc", L"AirPods Pro 2 (USB-C)", Shape::Pro},
    {"airpods-pro-3", L"AirPods Pro 3", Shape::Pro},
    {"airpods-max-lightning", L"AirPods Max 1 (Lightning)", Shape::Max},
    {"airpods-max-usbc", L"AirPods Max 1 (USB-C)", Shape::Max},
    {"airpods-max-2", L"AirPods Max 2", Shape::Max},
};

inline Shape FindShape(std::string_view key) {
    for (const auto& preset : presets) if (preset.key == key) return preset.shape;
    return Shape::None;
}

inline void Draw(ID2D1DeviceContext* dc, ID2D1Brush* brush, D2D1_POINT_2F center, Shape shape) {
    auto point = [&](float x, float y) { return D2D1::Point2F(center.x + x, center.y + y); };
    auto line = [&](float x, float y, float x2, float y2) { dc->DrawLine(point(x, y), point(x2, y2), brush, 1.35f); };
    auto box = [&](float x, float y, float x2, float y2, float r, bool filled = false) {
        auto bounds = D2D1::RoundedRect(D2D1::RectF(center.x + x, center.y + y, center.x + x2, center.y + y2), r, r);
        if (filled) dc->FillRoundedRectangle(bounds, brush);
        else dc->DrawRoundedRectangle(bounds, brush, 1.25f);
    };
    auto dot = [&](float x, float y, float rx, float ry) { dc->FillEllipse(D2D1::Ellipse(point(x, y), rx, ry), brush); };
    switch (shape) {
    case Shape::Speaker:
    case Shape::Stereo: {
        auto speaker = [&](float x) {
            box(x - 3.5f, -7, x + 3.5f, 7, 1.4f);
            dot(x, -3.4f, .9f, .9f);
            dot(x, 2, 1.9f, 1.9f);
        };
        if (shape == Shape::Speaker) speaker(0);
        else { speaker(-4.6f); speaker(4.6f); }
        break;
    }
    case Shape::Monitor:
        box(-7, -5, 7, 4, 1);
        line(0, 4, 0, 7); line(-3, 7, 3, 7);
        break;
    case Shape::TV:
        box(-8, -6, 8, 4, 1.4f);
        line(-4, 4, -6, 7); line(4, 4, 6, 7);
        break;
    case Shape::Max:
    case Shape::Headphones:
    case Shape::WiredHeadphones: {
        const bool max = shape == Shape::Max;
        const float bottom = shape == Shape::WiredHeadphones ? 4.0f : 7.0f;
        dc->PushAxisAlignedClip(D2D1::RectF(center.x - 9, center.y - 9, center.x + 9, center.y - 1),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->DrawEllipse(D2D1::Ellipse(point(0, -1), 7, 7), brush, max ? 1.8f : 1.25f);
        dc->PopAxisAlignedClip();
        box(-8, -2, max ? -3.f : -4.f, bottom, max ? 2.3f : 1.5f, true);
        box(max ? 3.f : 4.f, -2, 8, bottom, max ? 2.3f : 1.5f, true);
        if (shape == Shape::WiredHeadphones) { line(6, bottom, 6, 7); line(6, 7, 2, 9); }
        break;
    }
    case Shape::WiredBuds:
    case Shape::WirelessBuds:
    case Shape::LongStem:
    case Shape::ShortStem:
    case Shape::Pro: {
        const bool longStem = shape == Shape::LongStem;
        const bool wired = shape == Shape::WiredBuds;
        for (float side : {-1.f, 1.f}) {
            const float x = side * 4.f;
            const float y = longStem ? -4.f : -2.5f;
            dot(x, y, shape == Shape::WirelessBuds ? 3.1f : 2.65f, 2.5f);
            if (shape == Shape::Pro) dot(x - side * 1.9f, y - .3f, 1.65f, 1.55f);
            if (shape != Shape::WirelessBuds)
                box(x + side * .7f - .95f, y, x + side * .7f + .95f, longStem ? 6.7f : 3.8f, .9f, true);
            if (wired) line(x + side * .7f, 3, 0, 7);
        }
        if (wired) line(0, 7, 0, 9);
        break;
    }
    case Shape::None: break;
    }
}

} // namespace docky::audio_output
