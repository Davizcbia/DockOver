#pragma once

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "phone_language.h"
#include "pairing_glass.h"
#include "trusted_phones.h"
#include "../desktop_settings.h"

namespace docky::pairing {
namespace phone_settings {

constexpr int kWidth = 520, kDeviceTop = 455, kRowHeight = 56;
// Window corner: concentric with the 18 DIP cards inset 24 DIP from the edge.
constexpr int kCornerRadius = 18 + 24, kShadow = 18;
constexpr int kBackgroundSwitch = 198, kPcBubbleSwitch = 199, kChangeFolder = 200, kOpenFolder = 201, kFirstPhone = 202;
constexpr int kFolderPath = 197;

struct Palette {
    COLORREF top, bottom, surface, edge, text, secondary, accent, tint, hover, remove;
};

inline Palette ChoosePalette() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto bg = GetSysColor(COLOR_WINDOW), fg = GetSysColor(COLOR_WINDOWTEXT);
        return {bg, bg, bg, fg, fg, fg, fg, bg, bg, fg};
    }
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (!light) return {RGB(39, 42, 51), RGB(28, 31, 39), RGB(46, 49, 59), RGB(67, 72, 86),
        RGB(246, 248, 253), RGB(188, 195, 209), RGB(160, 205, 255), RGB(54, 70, 94), RGB(62, 84, 114), RGB(255, 171, 165)};
    return {RGB(225, 235, 249), RGB(245, 247, 251), RGB(255, 255, 255), RGB(215, 224, 237),
        RGB(27, 39, 56), RGB(78, 96, 119), RGB(20, 86, 166), RGB(231, 241, 255), RGB(213, 232, 255), RGB(153, 54, 56)};
}

struct Window {
    const std::wstring& folder;
    const std::vector<TrustedPhone>& phones;
    const std::wstring& settingsPath;
    bool pcBubble;
    bool background = true;
    Palette color = ChoosePalette();
    double scale = 1.0;
    HWND path = nullptr;
    HBRUSH cardBrush = CreateSolidBrush(color.surface);
    HFONT title{}, body{}, section{}, action{};
    HWND dialog = nullptr;
    HWND frame = nullptr;  // anti-aliased rounded backing and shadow behind the dialog (see PlaceFrame)
    PairingGlass glass;
    std::vector<uint32_t> backdrop;
    int backdropWidth = 0, backdropHeight = 0;
    bool reducedMotion = false;
    struct SwitchMotion {
        float from = 0, to = 0, pressFrom = 0, pressTo = 0, dragPosition = 0;
        ULONGLONG slideAt = 0, pressAt = 0;
        int pointerDownX = 0;
        float pointerDownPosition = 0;
        bool pointerDown = false, dragging = false;
    } switches[2];

    ~Window() {
        if (frame) DestroyWindow(frame);
        for (auto font : {title, body, section, action}) if (font) DeleteObject(font);
        if (cardBrush) DeleteObject(cardBrush);
    }

    int Dip(int value) const { return static_cast<int>(std::lround(value * scale)); }
    int Height() const { return kDeviceTop + (phones.empty() ? 75 : static_cast<int>(phones.size()) * kRowHeight) + 25; }
    RECT Rect(int x, int y, int w, int h) const { return {Dip(x), Dip(y), Dip(x+w), Dip(y+h)}; }
    HFONT Font(int size, int weight) const {
        return CreateFontW(-Dip(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
};

inline Gdiplus::Color Color(COLORREF value) {
    return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value));
}

inline void Rounded(Gdiplus::Graphics& graphics, const RECT& bounds, int radius, Gdiplus::Color fill,
                    COLORREF border = CLR_INVALID) {
    const auto width = static_cast<float>(bounds.right - bounds.left), height = static_cast<float>(bounds.bottom - bounds.top);
    const float x = static_cast<float>(bounds.left) + 0.5f, y = static_cast<float>(bounds.top) + 0.5f;
    const auto diameter = std::max(1.0f, std::min({static_cast<float>(radius * 2), width - 1, height - 1}));
    const float right = x + width - 1.0f, bottom = y + height - 1.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(right - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, bottom - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(fill);
    graphics.FillPath(&brush, &path);
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(Color(border));
        graphics.DrawPath(&pen, &path);
    }
}

inline void Rounded(Gdiplus::Graphics& graphics, const RECT& bounds, int radius, COLORREF fill,
                    COLORREF border = CLR_INVALID) {
    Rounded(graphics, bounds, radius, Color(fill), border);
}

inline Gdiplus::Color Tint(COLORREF color, BYTE alpha) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

inline void ContinuousPath(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius) {
    // The same n=3 superellipse corner profile as the main DockOver Settings window.
    constexpr int segments = 40;
    static const auto quadrant = [] {
        std::array<Gdiplus::PointF, segments + 1> profile{};
        for (int i = 0; i <= segments; ++i) {
            const float t = 1.57079632679f * i / segments;
            profile[i] = {1.f - std::pow(std::max(0.f, std::sin(t)), 2.f / 3.f),
                          1.f - std::pow(std::max(0.f, std::cos(t)), 2.f / 3.f)};
        }
        profile.front() = {1, 0};
        profile.back() = {0, 1};
        return profile;
    }();
    const float r = std::min({radius, width * .5f, height * .5f});
    const float right = x + width, bottom = y + height;
    std::array<Gdiplus::PointF, (segments + 1) * 4> points;
    for (int i = 0; i <= segments; ++i) {
        const auto a = r * quadrant[i].X, b = r * quadrant[i].Y;
        points[i] = {right - a, y + b};
        points[i + segments + 1] = {right - b, bottom - a};
        points[i + 2 * (segments + 1)] = {x + a, bottom - b};
        points[i + 3 * (segments + 1)] = {x + b, y + a};
    }
    path.AddPolygon(points.data(), static_cast<INT>(points.size()));
}

inline void PaintPlate(Gdiplus::Graphics& graphics, const RECT& bounds, int radius, Gdiplus::Color color,
                       COLORREF border = CLR_INVALID) {
    Gdiplus::GraphicsPath path;
    ContinuousPath(path, bounds.left + .5f, bounds.top + .5f,
        static_cast<float>(bounds.right - bounds.left - 1), static_cast<float>(bounds.bottom - bounds.top - 1),
        static_cast<float>(radius));
    Gdiplus::SolidBrush fill(color);
    graphics.FillPath(&fill, &path);
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(Color(border));
        graphics.DrawPath(&pen, &path);
    }
}

inline void Label(HDC dc, const wchar_t* text, RECT bounds, HFONT font, COLORREF color,
                  UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX) {
    const auto oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &bounds, format);
    SelectObject(dc, oldFont);
}

inline void UpdateToggleName(HWND toggle, bool on) {
    const bool background = GetDlgCtrlID(toggle) == kBackgroundSwitch;
    const std::wstring name = std::wstring(Ui(background ? L"在后台运行" : L"电脑传输使用水球弹窗")) + L" · "
        + Ui(on ? L"已开启" : L"已关闭");
    SetWindowTextW(toggle, name.c_str());
}

struct Dib {
    BITMAPINFO info{};
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* pixels = nullptr;

    Dib(HDC compatible, int width, int height) {
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
        dc = CreateCompatibleDC(compatible);
        void* bits = nullptr;
        bitmap = CreateDIBSection(compatible, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dc && bitmap) {
            old = SelectObject(dc, bitmap);
            pixels = static_cast<uint32_t*>(bits);
        }
    }
    ~Dib() {
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

inline void CaptureBackdrop(Window& model, const RECT& screenRect) {
    const int w = (screenRect.right - screenRect.left) / 8, h = (screenRect.bottom - screenRect.top) / 8;
    if (w < 8 || h < 8 || model.color.top == model.color.bottom) return;
    BOOL transparency = TRUE;
    DWORD size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    if (!transparency) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    Dib capture(screen, w, h);
    if (capture.pixels) {
        SetStretchBltMode(capture.dc, HALFTONE);
        if (StretchBlt(capture.dc, 0, 0, w, h, screen, screenRect.left, screenRect.top,
                screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SRCCOPY | CAPTUREBLT)) {
            GdiFlush();
            const size_t count = static_cast<size_t>(w) * h;
            std::vector<uint32_t> horizontal(count);
            model.backdrop.resize(count);
            const int radius = std::clamp(model.Dip(28) / 8, 2, 9);
            const auto pass = [&](const uint32_t* source, uint32_t* dest, bool vertical) {
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        int r = 0, g = 0, b = 0;
                        for (int tap = -radius; tap <= radius; ++tap) {
                            const int px = vertical ? x : std::clamp(x + tap, 0, w - 1);
                            const int py = vertical ? std::clamp(y + tap, 0, h - 1) : y;
                            const auto value = source[static_cast<size_t>(py) * w + px];
                            r += (value >> 16) & 255;
                            g += (value >> 8) & 255;
                            b += value & 255;
                        }
                        const int taps = radius * 2 + 1;
                        dest[static_cast<size_t>(y) * w + x] = 0xff000000u |
                            (r / taps << 16) | (g / taps << 8) | b / taps;
                    }
                }
            };
            pass(capture.pixels, horizontal.data(), false);
            pass(horizontal.data(), model.backdrop.data(), true);
            const auto tint = model.color.top;
            for (auto& pixel : model.backdrop) {
                const auto mix = [](uint32_t blur, uint32_t color) { return (blur * 3 + color * 7) / 10; };
                pixel = 0xff000000u | (mix((pixel >> 16) & 255, GetRValue(tint)) << 16) |
                    (mix((pixel >> 8) & 255, GetGValue(tint)) << 8) | mix(pixel & 255, GetBValue(tint));
            }
            model.backdropWidth = w;
            model.backdropHeight = h;
        }
    }
    ReleaseDC(nullptr, screen);
}

inline void PaintBase(HDC dc, const Window& model, int left = 0, int top = 0) {
    auto bounds = model.Rect(0, 0, kWidth, model.Height());
    const int width = bounds.right, height = bounds.bottom;
    OffsetRect(&bounds, left, top);
    if (!model.backdrop.empty()) {
        BITMAPINFO image{};
        image.bmiHeader = {sizeof(BITMAPINFOHEADER), model.backdropWidth, -model.backdropHeight, 1, 32, BI_RGB};
        const int previous = SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, left, top, width, height, 0, 0, model.backdropWidth, model.backdropHeight,
            model.backdrop.data(), &image, DIB_RGB_COLORS, SRCCOPY);
        SetStretchBltMode(dc, previous);
    } else {
        const HBRUSH fill = CreateSolidBrush(model.color.top);
        FillRect(dc, &bounds, fill);
        DeleteObject(fill);
    }

    Gdiplus::Graphics canvas(dc);
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const bool contrast = model.color.top == model.color.bottom;
    const auto fill = Tint(model.color.surface, contrast ? 255 : 108);
    const auto edge = contrast ? model.color.edge : CLR_INVALID;
    const auto card = [&](int y, int h) {
        auto rect = model.Rect(24, y, 472, h);
        OffsetRect(&rect, left, top);
        PaintPlate(canvas, rect, model.Dip(18), fill, edge);
    };
    card(115, 111);
    card(243, 70);
    card(329, 70);
    card(kDeviceTop, model.Height() - kDeviceTop - 25);
}

constexpr UINT_PTR kSwitchAnimation = 1;
constexpr float kTrackWidth = 46.4f, kTrackHeight = 20.0f;
constexpr float kThumbWidth = 29.6f, kThumbHeight = 17.6f, kThumbTravel = 14.4f;

inline Window::SwitchMotion& Motion(Window& model, UINT id) { return model.switches[id == kBackgroundSwitch]; }
inline bool SwitchOn(const Window& model, UINT id) { return id == kPcBubbleSwitch ? model.pcBubble : model.background; }

inline float SwitchAmount(float from, float to, ULONGLONG start, ULONGLONG now, float duration, bool back = false) {
    if (from == to) return to;
    const float t = std::clamp(static_cast<float>(now - start) / duration, 0.0f, 1.0f);
    const float x = t - 1.0f;
    const float eased = back ? 1.0f + 2.65f * x * x * x + 0.65f * x * x : 1.0f + x * x * x;
    return std::clamp(std::lerp(from, to, eased), 0.0f, 1.0f);
}

inline void PaintSwitch(const DRAWITEMSTRUCT& item, Window& model, const RECT& bounds, bool focused, bool active) {
    auto& motion = Motion(model, item.CtlID);
    const auto now = GetTickCount64();
    const bool on = SwitchOn(model, item.CtlID);
    const bool contrast = model.color.top == model.color.bottom;
    const bool dark = GetRValue(model.color.top) < 128;
    const float position = motion.dragging ? motion.dragPosition :
        SwitchAmount(motion.from, motion.to, motion.slideAt, now, 420, true);
    const float press = model.reducedMotion ? (active ? 1.0f : 0.0f) :
        SwitchAmount(motion.pressFrom, motion.pressTo, motion.pressAt, now, 150);
    const float dpi = static_cast<float>(model.scale);
    const int width = item.rcItem.right - item.rcItem.left, height = item.rcItem.bottom - item.rcItem.top;
    const float cx = width * 0.5f, cy = height * 0.5f;
    const auto rect = [](float x, float y, float w, float h) -> RECT {
        return {std::lround(x - w * .5f), std::lround(y - h * .5f), std::lround(x + w * .5f), std::lround(y + h * .5f)};
    };
    const auto track = rect(cx, cy, kTrackWidth * dpi, kTrackHeight * dpi);
    const COLORREF trackColor = contrast ? (on ? model.color.text : model.color.surface)
        : (on ? RGB(96, 199, 95) : dark ? RGB(75, 75, 77) : RGB(218, 218, 218));
    const auto drawTrack = [&](HDC dc) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Rounded(graphics, track, model.Dip(10), trackColor, contrast ? model.color.text : CLR_INVALID);
    };
    Dib frame(item.hDC, width, height);
    const HDC dc = frame.pixels ? frame.dc : item.hDC;
    PaintBase(dc, model, -bounds.left, -bounds.top);
    drawTrack(dc);

    const float thumbCenter = cx + (position - .5f) * kThumbTravel * dpi;
    const auto resting = rect(thumbCenter, cy, kThumbWidth * dpi, kThumbHeight * dpi);
    bool lensDrawn = false;
    if (frame.pixels && press > .001f && !contrast) {
        const auto controls = dock::ControlLensGlass(kThumbHeight);
        const float expand = model.reducedMotion ? 0.0f : press;
        const float w = kThumbWidth * std::lerp(1.0f, controls.switchPressWidth, expand);
        const float h = kThumbHeight * std::lerp(1.0f, controls.switchPressHeight, expand);
        auto lens = dock::ControlLensGlass(h);
        lens.dark = dark;
        lens.opacity = press;
        lens.interactionStrength *= press;
        GdiFlush();
        const size_t count = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < count; ++i) frame.pixels[i] |= 0xff000000u;
        std::vector<uint32_t> refracted;
        POINT pointer{};
        GetCursorPos(&pointer);
        ScreenToClient(item.hwndItem, &pointer);
        const float x = thumbCenter / dpi, y = cy / dpi;
        if (model.glass.PreparePixels(width, height, frame.pixels) &&
            model.glass.CopyLayers({0, 0, width, height}, refracted, [&](ID3D11RenderTargetView* target) {
                return model.glass.Draw(target, dpi, {x-w*.5f, y-h*.5f, x+w*.5f, y+h*.5f},
                    lens, static_cast<float>(pointer.x), static_cast<float>(pointer.y), true, true);
            })) {
            std::copy(refracted.begin(), refracted.end(), frame.pixels);
            lensDrawn = true;
        }
    }
    if (!lensDrawn || press < .999f) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto color = contrast ? (on ? model.color.surface : model.color.text) : RGB(255, 255, 255);
        const auto alpha = static_cast<BYTE>(lensDrawn ? std::lround(255 * (1.0f - press)) : 255);
        Rounded(graphics, resting, model.Dip(9), Tint(color, alpha));
    }
    if (focused) DrawFocusRect(dc, &item.rcItem);
    // Publish the complete track, thumb and lens together, after the GPU readback.
    if (frame.pixels) BitBlt(item.hDC, 0, 0, width, height, dc, 0, 0, SRCCOPY);
}

inline void IconTile(Gdiplus::Graphics& canvas, const Window& model, int x, int y, bool folder) {
    PaintPlate(canvas, model.Rect(x, y, 40, 40), model.Dip(12), Tint(model.color.tint, 150));
    Gdiplus::GraphicsState state = canvas.Save();
    canvas.TranslateTransform(static_cast<float>(model.Dip(x)), static_cast<float>(model.Dip(y)));
    canvas.ScaleTransform(static_cast<float>(model.scale), static_cast<float>(model.scale));
    Gdiplus::Pen pen(Color(model.color.accent), 1.65f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (folder) {
        Gdiplus::Point points[]{{9, 28}, {9, 13}, {10, 12}, {17, 12}, {20, 15},
            {30, 15}, {31, 16}, {31, 27}, {30, 28}, {9, 28}};
        canvas.DrawLines(&pen, points, ARRAYSIZE(points));
        canvas.DrawLine(&pen, 9, 18, 31, 18);
    } else {
        Gdiplus::GraphicsPath phone;
        phone.AddArc(13, 8, 6, 6, 180, 90);
        phone.AddArc(21, 8, 6, 6, 270, 90);
        phone.AddArc(21, 26, 6, 6, 0, 90);
        phone.AddArc(13, 26, 6, 6, 90, 90);
        phone.CloseFigure();
        canvas.DrawPath(&pen, &phone);
        canvas.DrawLine(&pen, 18, 28, 22, 28);
    }
    canvas.Restore(state);
}

inline void Paint(HWND window, Window& model) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    const auto& p = model.color;
    PaintBase(dc, model);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        IconTile(graphics, model, 43, 134, true);
        Gdiplus::Pen separator(Tint(p.secondary, 44));
        for (size_t i = 0; i < model.phones.size(); ++i) {
            const int y = kDeviceTop + static_cast<int>(i) * kRowHeight;
            IconTile(graphics, model, 43, y + 8, false);
            if (i) graphics.DrawLine(&separator, model.Dip(100), model.Dip(y), model.Dip(476), model.Dip(y));
        }
    }
    auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
        IMAGE_ICON, model.Dip(48), model.Dip(48), LR_SHARED));
    if (icon) DrawIconEx(dc, model.Dip(25), model.Dip(22), icon, model.Dip(48), model.Dip(48), 0, nullptr, DI_NORMAL);
    Label(dc, Ui(L"接收设置"), model.Rect(81, 19, 295, 39), model.title, p.text);
    Label(dc, L"DockDrop", model.Rect(83, 56, 230, 20), model.section, p.secondary);
    Label(dc, Ui(L"接收文件夹"), model.Rect(30, 87, 450, 24), model.section, p.secondary);
    Label(dc, Ui(L"电脑传输使用水球弹窗"), model.Rect(43, 253, 365, 25), model.body, p.text);
    Label(dc, Ui(L"关闭后使用通知中心样式"), model.Rect(43, 279, 365, 20), model.section, p.secondary);
    Label(dc, Ui(L"在后台运行"), model.Rect(43, 339, 365, 25), model.body, p.text);
    Label(dc, Ui(L"关闭窗口后，已配对的手机仍可发送，水球会自动弹出"), model.Rect(43, 365, 365, 20), model.section, p.secondary,
          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    Label(dc, Ui(L"已配对的手机"), model.Rect(30, 426, 450, 24), model.section, p.secondary);
    if (model.phones.empty()) {
        Label(dc, Ui(L"还没有记住的手机。首次扫码后可选择记住电脑。"),
            model.Rect(43, kDeviceTop + 13, 434, 50), model.body, p.secondary, DT_WORDBREAK | DT_NOPREFIX);
    } else {
        for (size_t i = 0; i < model.phones.size(); ++i) {
            const int y = kDeviceTop + static_cast<int>(i) * kRowHeight;
            Label(dc, model.phones[i].name.c_str(), model.Rect(100, y+1, 279, kRowHeight),
                model.body, p.text);
        }
    }
    EndPaint(window, &ps);
}

inline void PaintButton(const DRAWITEMSTRUCT& item, Window& model) {
    const auto& p = model.color;
    const bool close = item.CtlID == IDCLOSE;
    const bool remove = item.CtlID >= kFirstPhone;
    const bool toggle = item.CtlID == kPcBubbleSwitch || item.CtlID == kBackgroundSwitch;
    const bool active = (item.itemState & ODS_SELECTED) != 0 || (toggle && Motion(model, item.CtlID).pointerDown);
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;
    RECT bounds{};
    GetWindowRect(item.hwndItem, &bounds);
    MapWindowPoints(nullptr, model.dialog, reinterpret_cast<POINT*>(&bounds), 2);
    if (toggle) {
        PaintSwitch(item, model, bounds, focused, active);
        return;
    }
    PaintBase(item.hDC, model, -bounds.left, -bounds.top);
    if (item.CtlID == kFolderPath) {
        Label(item.hDC, model.folder.c_str(), item.rcItem, model.body, p.text,
            DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        return;
    }
    {
        Gdiplus::Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        auto plate = item.rcItem;
        InflateRect(&plate, -1, -1);
        PaintPlate(graphics, plate, model.Dip(12), Tint(active ? p.hover : p.tint, close || active ? 230 : 160),
            focused || p.top == p.bottom ? p.accent : CLR_INVALID);
    }
    wchar_t title[128]{};
    if (remove) wcscpy_s(title, Ui(L"移除…"));
    else GetWindowTextW(item.hwndItem, title, ARRAYSIZE(title));
    Label(item.hDC, title, item.rcItem, model.action, remove ? p.remove : p.accent,
        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

// A dialog with child controls cannot be a per-pixel-alpha window, and a window region alone is aliased (and turns off
// the DWM border and rounding). So the dialog is clipped to a region 2 px inside its shape, and an owned layered window
// right behind it draws the same background with an anti-aliased edge, outline and soft shadow: the region's stairs sit
// on matching colour and only the smooth edge shows.
inline void PaintFrame(const Window& model, int width, int height) {
    if (!model.frame) return;
    const int s = model.Dip(kShadow), fw = width + 2 * s, fh = height + 2 * s;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && dib && bits) {
        const HGDIOBJ old = SelectObject(mem, dib);
        {
            Gdiplus::Bitmap canvas(fw, fh, fw * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
            Gdiplus::Graphics g(&canvas);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            const float r = static_cast<float>(model.Dip(kCornerRadius));
            for (int k = s; k >= 1; --k) {  // soft shadow, slightly lower than the panel
                const float t = 1.0f - static_cast<float>(k) / static_cast<float>(s);
                Gdiplus::GraphicsPath ring;
                ContinuousPath(ring, static_cast<float>(s - k), static_cast<float>(s - k) + s * 0.25f, static_cast<float>(width + 2 * k),
                      static_cast<float>(height + 2 * k), r + k);
                Gdiplus::SolidBrush shade(Gdiplus::Color(static_cast<BYTE>(std::lround(10.0f * t * t)), 0, 0, 0));
                g.FillPath(&shade, &ring);
            }
            Gdiplus::GraphicsPath panel;
            ContinuousPath(panel, static_cast<float>(s), static_cast<float>(s), static_cast<float>(width), static_cast<float>(height), r);
            const auto& p = model.color;
            const Gdiplus::Rect bounds(s, s, width, height);
            Gdiplus::SolidBrush background(Color(p.top));
            g.FillPath(&background, &panel);
            if (!model.backdrop.empty()) {
                Gdiplus::Bitmap blurred(model.backdropWidth, model.backdropHeight, model.backdropWidth * 4,
                    PixelFormat32bppRGB, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(model.backdrop.data())));
                const auto saved = g.Save();
                g.SetClip(&panel);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                g.DrawImage(&blurred, bounds, 0, 0, model.backdropWidth, model.backdropHeight, Gdiplus::UnitPixel);
                g.Restore(saved);
            }
            Gdiplus::GraphicsPath outline;
            ContinuousPath(outline, s + 0.5f, s + 0.5f, width - 1.0f, height - 1.0f, r - 0.5f);
            Gdiplus::Pen pen(Tint(p.edge, 144), 1.0f);
            g.DrawPath(&pen, &outline);
        }
        RECT dialog{};
        GetWindowRect(model.dialog, &dialog);
        POINT origin{dialog.left - s, dialog.top - s}, zero{};
        SIZE size{fw, fh};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(model.frame, screen, &origin, &size, mem, &zero, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

inline void PlaceFrame(HWND window, const Window& model) {
    if (!model.frame) return;
    RECT r{};
    GetWindowRect(window, &r);
    const int s = model.Dip(kShadow);
    SetWindowPos(model.frame, nullptr, r.left - s, r.top - s, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

inline void AnimateSwitch(HWND control, Window& model) {
    InvalidateRect(control, nullptr, FALSE);
    if (!model.reducedMotion) SetTimer(model.dialog, kSwitchAnimation, 15, nullptr);
}

inline void SetSwitchPress(HWND control, Window& model, bool pressed) {
    auto& motion = Motion(model, GetDlgCtrlID(control));
    const auto now = GetTickCount64();
    motion.pressFrom = SwitchAmount(motion.pressFrom, motion.pressTo, motion.pressAt, now, 150);
    motion.pressTo = pressed ? 1.0f : 0.0f;
    motion.pressAt = now;
    AnimateSwitch(control, model);
}

inline void CommitSwitch(HWND control, Window& model, bool on) {
    const UINT id = GetDlgCtrlID(control);
    auto& checked = id == kBackgroundSwitch ? model.background : model.pcBubble;
    auto& motion = Motion(model, id);
    const auto now = GetTickCount64();
    motion.from = motion.dragging ? motion.dragPosition : SwitchAmount(motion.from, motion.to, motion.slideAt, now, 420, true);
    motion.dragging = false;
    bool failed = false;
    if (on != checked) {
        const char* key = id == kBackgroundSwitch ? "runInBackground" : "pcReceiveBubble";
        failed = liquid::settings::WriteBool(model.settingsPath, "phonePairing", key, on) != liquid::settings::SaveStatus::Saved;
        if (!failed) {
            checked = on;
            UpdateToggleName(control, on);
        }
    }
    motion.to = checked ? 1.0f : 0.0f;
    if (model.reducedMotion) motion.from = motion.to;
    motion.slideAt = now;
    AnimateSwitch(control, model);
    if (failed) MessageBoxW(model.dialog, Ui(L"无法保存设置。"), L"DockDrop", MB_OK | MB_ICONWARNING);
}

inline LRESULT CALLBACK SwitchProc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& model = *reinterpret_cast<Window*>(data);
    const UINT id = GetDlgCtrlID(control);
    auto& motion = Motion(model, id);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        SetFocus(control);
        SetCapture(control);
        motion.pointerDown = true;
        motion.pointerDownX = GET_X_LPARAM(lp);
        motion.pointerDownPosition = SwitchAmount(motion.from, motion.to, motion.slideAt, GetTickCount64(), 420, true);
        SetSwitchPress(control, model, true);
        return 0;
    case WM_MOUSEMOVE:
        if (motion.pointerDown) {
            const int delta = GET_X_LPARAM(lp) - motion.pointerDownX;
            if (motion.dragging || std::abs(delta) >= GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(control))) {
                motion.dragging = true;
                motion.dragPosition = std::clamp(motion.pointerDownPosition + delta / static_cast<float>(model.scale * kThumbTravel), 0.0f, 1.0f);
            }
            InvalidateRect(control, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (motion.pointerDown) {
            RECT bounds{};
            GetClientRect(control, &bounds);
            const POINT pointer{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const bool checked = SwitchOn(model, id);
            const bool on = motion.dragging ? motion.dragPosition >= .5f : PtInRect(&bounds, pointer) ? !checked : checked;
            motion.pointerDown = false;
            ReleaseCapture();
            SetSwitchPress(control, model, false);
            CommitSwitch(control, model, on);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (motion.pointerDown) {
            motion.pointerDown = false;
            SetSwitchPress(control, model, false);
            CommitSwitch(control, model, SwitchOn(model, id));
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && (lp & (1L << 30)) == 0) SetSwitchPress(control, model, true);
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE) SetSwitchPress(control, model, false);
        break;
    case WM_KILLFOCUS:
        if (motion.pressTo > 0 && !motion.pointerDown) SetSwitchPress(control, model, false);
        break;
    }
    return DefSubclassProc(control, message, wp, lp);
}

inline void Init(HWND window, Window& model) {
    const HWND owner = GetWindow(window, GW_OWNER);
    RECT parent{};
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto work = monitor.rcWork;
    model.scale = std::min({GetDpiForWindow(owner) / 96.0,
        (work.right - work.left - 24) / static_cast<double>(kWidth),
        (work.bottom - work.top - 24) / static_cast<double>(model.Height())});
    model.title = model.Font(29, FW_SEMIBOLD);
    model.body = model.Font(16, FW_MEDIUM);
    model.section = model.Font(14, FW_SEMIBOLD);
    model.action = model.Font(14, FW_SEMIBOLD);
    const int width = model.Dip(kWidth), height = model.Dip(model.Height());
    SetWindowPos(window, nullptr,
        std::clamp(static_cast<int>((parent.left + parent.right - width) / 2), static_cast<int>(work.left), static_cast<int>(work.right) - width),
        std::clamp(static_cast<int>((parent.top + parent.bottom - height) / 2), static_cast<int>(work.top), static_cast<int>(work.bottom) - height),
        width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    model.dialog = window;
    RECT screenRect{};
    GetWindowRect(window, &screenRect);
    CaptureBackdrop(model, screenRect);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    model.reducedMotion = !animations;
    if (model.color.top != model.color.bottom) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            model.glass.Init(device.Get());
    }
    const int radius = model.Dip(kCornerRadius), inset = 2;
    {
        Gdiplus::GraphicsPath path;
        ContinuousPath(path, inset, inset, static_cast<float>(width - 2 * inset), static_cast<float>(height - 2 * inset),
            static_cast<float>(radius - inset));
        Gdiplus::Graphics graphics(window);
        Gdiplus::Region clip(&path);
        const HRGN shape = clip.GetHRGN(&graphics);
        if (shape && !SetWindowRgn(window, shape, TRUE)) DeleteObject(shape);
    }
    // Owned windows always sit above their owner, so the frame shares the dialog's owner and is placed right under it.
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DockDropSettingsFrame";
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    model.frame = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DockDropSettingsFrame", L"",
                                  WS_POPUP, 0, 0, 1, 1, GetWindow(window, GW_OWNER), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (model.frame) {
        PaintFrame(model, width, height);
        SetWindowPos(model.frame, window, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    const auto control = [&model, window](const wchar_t* type, const wchar_t* text, int id, DWORD style,
                                           int x, int y, int w, int h) {
        const auto rect = model.Rect(x,y,w,h);
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    };
    const auto button = [&control](const wchar_t* text, int id, int x, int y, int w, int h) {
        return control(L"BUTTON", text, id, WS_TABSTOP | BS_OWNERDRAW, x,y,w,h);
    };
    model.path = control(L"STATIC", model.folder.c_str(), kFolderPath, SS_OWNERDRAW, 100, 141, 365, 28);
    SendMessageW(model.path, WM_SETFONT, reinterpret_cast<WPARAM>(model.body), 0);
    button(Ui(L"更改…"), kChangeFolder, 100, 175, 100, 36);
    button(Ui(L"打开文件夹"), kOpenFolder, 210, 175, 115, 36);
    const auto toggle = [&](int id, int y) {
        const auto control = button(L"", id, 412, y, 75, 42);
        const bool on = SwitchOn(model, id);
        auto& motion = Motion(model, id);
        motion.from = motion.to = on ? 1.0f : 0.0f;
        UpdateToggleName(control, on);
        SetWindowSubclass(control, SwitchProc, 1, reinterpret_cast<DWORD_PTR>(&model));
    };
    toggle(kPcBubbleSwitch, 257);
    toggle(kBackgroundSwitch, 343);
    for (size_t i = 0; i < model.phones.size(); ++i) {
        const auto label = std::wstring(Ui(L"移除…")) + L" " + model.phones[i].name;
        button(label.c_str(), kFirstPhone + static_cast<int>(i), 391,
            kDeviceTop + 11 + static_cast<int>(i) * kRowHeight, 88, 34);
    }
    button(Ui(L"完成"), IDCLOSE, 403, 30, 89, 36);
}

inline INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* model = reinterpret_cast<Window*>(GetWindowLongPtrW(window, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        model = reinterpret_cast<Window*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(model));
        Init(window, *model);
        return TRUE;
    case WM_ERASEBKGND: return TRUE;
    case WM_WINDOWPOSCHANGED:
        if (model) PlaceFrame(window, *model);
        break;
    case WM_DESTROY:
        if (model && model->frame) { DestroyWindow(model->frame); model->frame = nullptr; }
        break;
    case WM_PAINT:
        if (model) { Paint(window, *model); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if (model) {
            auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, model->color.text);
            SetBkColor(dc, model->color.surface);
            return reinterpret_cast<INT_PTR>(model->cardBrush);
        }
        break;
    case WM_DRAWITEM:
        if (model) { PaintButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp), *model); return TRUE; }
        break;
    case WM_TIMER:
        if (model && wp == kSwitchAnimation) {
            bool running = false;
            const auto now = GetTickCount64();
            for (int id : {kPcBubbleSwitch, kBackgroundSwitch}) {
                auto& motion = Motion(*model, id);
                if (motion.from == motion.to && motion.pressFrom == motion.pressTo) continue;
                if (now - motion.slideAt >= 420) motion.from = motion.to;
                if (now - motion.pressAt >= 150) motion.pressFrom = motion.pressTo;
                running = running || motion.from != motion.to || motion.pressFrom != motion.pressTo;
                InvalidateRect(GetDlgItem(window, id), nullptr, FALSE);
            }
            if (!running) KillTimer(window, kSwitchAnimation);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (const UINT id = LOWORD(wp); model && (id == kBackgroundSwitch || id == kPcBubbleSwitch) && HIWORD(wp) == BN_CLICKED) {
            CommitSwitch(GetDlgItem(window, id), *model, !SwitchOn(*model, id));
            return TRUE;
        }
        if (HIWORD(wp) == BN_CLICKED) { EndDialog(window, LOWORD(wp)); return TRUE; }
        break;
    case WM_CLOSE:
        EndDialog(window, IDCLOSE);
        return TRUE;
    }
    return FALSE;
}

}  // namespace phone_settings

inline int ShowStyledPhoneSettings(HWND owner, const std::wstring& folder, const std::vector<TrustedPhone>& phones,
                                   const std::wstring& settingsPath) {
    const struct {
        DLGTEMPLATE dialog;
        WORD menu, windowClass;
        wchar_t title[9];
    } definition{{WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, 280, 190}, 0, 0, L"DockDrop"};
    const auto settings = liquid::settings::Load(settingsPath).value;
    phone_settings::Window model{folder, phones, settingsPath, settings.pcReceiveBubble};
    model.background = settings.phoneBackground;
    return static_cast<int>(DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &definition.dialog, owner,
        phone_settings::DialogProc, reinterpret_cast<LPARAM>(&model)));
}

}  // namespace docky::pairing
