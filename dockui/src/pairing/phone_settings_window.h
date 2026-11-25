#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "phone_language.h"
#include "trusted_phones.h"
#include "../desktop_settings.h"

namespace docky::pairing {
namespace phone_settings {

constexpr int kWidth = 520, kDeviceTop = 455, kRowHeight = 56;
// Window corner: concentric with the 18 DIP cards inset 24 DIP from the edge.
constexpr int kCornerRadius = 18 + 24, kShadow = 18;
constexpr int kBackgroundSwitch = 198, kPcBubbleSwitch = 199, kChangeFolder = 200, kOpenFolder = 201, kFirstPhone = 202;

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

inline void Rounded(Gdiplus::Graphics& graphics, const RECT& bounds, int radius, COLORREF fill,
                    COLORREF border = CLR_INVALID) {
    const auto width = static_cast<float>(bounds.right - bounds.left), height = static_cast<float>(bounds.bottom - bounds.top);
    const float x = static_cast<float>(bounds.left) + 0.5f, y = static_cast<float>(bounds.top) + 0.5f;
    const auto diameter = static_cast<float>(radius * 2);
    const float right = x + width - 1.0f, bottom = y + height - 1.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(right - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, bottom - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(Color(fill));
    graphics.FillPath(&brush, &path);
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

inline void IconTile(Gdiplus::Graphics& canvas, const Window& model, int x, int y, bool folder) {
    Rounded(canvas, model.Rect(x, y, 40, 40), model.Dip(12), model.color.tint);
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

inline void Paint(HWND window, const Window& model) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    const auto& p = model.color;
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::Rect bounds(0, 0, client.right, client.bottom);
        Gdiplus::LinearGradientBrush background(bounds, Color(p.top), Color(p.bottom), Gdiplus::LinearGradientModeVertical);
        const Gdiplus::REAL factors[]{0.0f, 0.0f, 1.0f};
        const Gdiplus::REAL positions[]{0.0f, 78.0f / model.Height(), 1.0f};
        background.SetBlend(factors, positions, ARRAYSIZE(factors));
        graphics.FillRectangle(&background, bounds);
        Rounded(graphics, model.Rect(24, 115, 472, 111), model.Dip(18), p.surface, p.edge);
        IconTile(graphics, model, 43, 134, true);
        Rounded(graphics, model.Rect(24, 243, 472, 70), model.Dip(18), p.surface, p.edge);
        Rounded(graphics, model.Rect(24, 329, 472, 70), model.Dip(18), p.surface, p.edge);
        Rounded(graphics, model.Rect(24, kDeviceTop, 472, model.Height() - kDeviceTop - 25),
            model.Dip(18), p.surface, p.edge);
        Gdiplus::Pen separator(Color(p.edge));
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

inline void PaintButton(const DRAWITEMSTRUCT& item, const Window& model) {
    const auto& p = model.color;
    const bool close = item.CtlID == IDCLOSE;
    const bool remove = item.CtlID >= kFirstPhone;
    const bool active = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;
    const auto brush = CreateSolidBrush(close ? p.top : p.surface);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    if (item.CtlID == kPcBubbleSwitch || item.CtlID == kBackgroundSwitch) {
        const bool on = item.CtlID == kPcBubbleSwitch ? model.pcBubble : model.background;
        Gdiplus::Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto dip = [&model](int n) { return model.Dip(n); };
        const RECT track{dip(3), dip(5), dip(49), dip(29)};
        Rounded(graphics, track, dip(12), on ? p.accent : p.edge, focused ? p.text : CLR_INVALID);
        Gdiplus::SolidBrush thumb(Color(on ? p.top : p.text));
        graphics.FillEllipse(&thumb, dip(on ? 27 : 6), dip(8), dip(18), dip(18));
        return;
    }
    {
        Gdiplus::Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        auto bounds = item.rcItem;
        InflateRect(&bounds, -1, -1);
        Rounded(graphics, bounds, model.Dip(12), active ? p.hover : remove ? p.surface : p.tint,
            focused ? p.accent : p.edge);
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
            auto shape = [](Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
                const float d = 2.0f * radius;
                path.AddArc(x, y, d, d, 180, 90);
                path.AddArc(x + w - d, y, d, d, 270, 90);
                path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
                path.AddArc(x, y + h - d, d, d, 90, 90);
                path.CloseFigure();
            };
            for (int k = s; k >= 1; --k) {  // soft shadow, slightly lower than the panel
                const float t = 1.0f - static_cast<float>(k) / static_cast<float>(s);
                Gdiplus::GraphicsPath ring;
                shape(ring, static_cast<float>(s - k), static_cast<float>(s - k) + s * 0.25f, static_cast<float>(width + 2 * k),
                      static_cast<float>(height + 2 * k), r + k);
                Gdiplus::SolidBrush shade(Gdiplus::Color(static_cast<BYTE>(std::lround(10.0f * t * t)), 0, 0, 0));
                g.FillPath(&shade, &ring);
            }
            Gdiplus::GraphicsPath panel;
            shape(panel, static_cast<float>(s), static_cast<float>(s), static_cast<float>(width), static_cast<float>(height), r);
            const auto& p = model.color;
            const Gdiplus::Rect bounds(s, s, width, height);
            Gdiplus::LinearGradientBrush background(bounds, Color(p.top), Color(p.bottom), Gdiplus::LinearGradientModeVertical);
            const Gdiplus::REAL factors[]{0.0f, 0.0f, 1.0f};
            const Gdiplus::REAL positions[]{0.0f, 78.0f / model.Height(), 1.0f};
            background.SetBlend(factors, positions, ARRAYSIZE(factors));
            g.FillPath(&background, &panel);
            Gdiplus::GraphicsPath outline;
            shape(outline, s + 0.5f, s + 0.5f, width - 1.0f, height - 1.0f, r - 0.5f);
            Gdiplus::Pen pen(Color(p.edge), 1.0f);
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
    const int radius = model.Dip(kCornerRadius), inset = 2;
    HRGN shape = CreateRoundRectRgn(inset, inset, width - inset + 1, height - inset + 1, 2 * (radius - inset), 2 * (radius - inset));
    if (shape && !SetWindowRgn(window, shape, TRUE)) DeleteObject(shape);
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
    model.path = control(L"STATIC", model.folder.c_str(), 0, SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, 100, 141, 365, 28);
    SendMessageW(model.path, WM_SETFONT, reinterpret_cast<WPARAM>(model.body), 0);
    button(Ui(L"更改…"), kChangeFolder, 100, 175, 100, 36);
    button(Ui(L"打开文件夹"), kOpenFolder, 210, 175, 115, 36);
    const HWND toggle = button(L"", kPcBubbleSwitch, 423, 261, 53, 34);
    UpdateToggleName(toggle, model.pcBubble);
    const HWND background = button(L"", kBackgroundSwitch, 423, 347, 53, 34);
    UpdateToggleName(background, model.background);
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
    case WM_COMMAND:
        if (model && LOWORD(wp) == kBackgroundSwitch && HIWORD(wp) == BN_CLICKED) {
            if (liquid::settings::WriteBool(model->settingsPath, "phonePairing", "runInBackground", !model->background)
                    == liquid::settings::SaveStatus::Saved) {
                model->background = !model->background;
                const auto toggle = GetDlgItem(window, kBackgroundSwitch);
                UpdateToggleName(toggle, model->background);
                InvalidateRect(toggle, nullptr, FALSE);
            } else MessageBoxW(window, Ui(L"无法保存设置。"), L"DockDrop", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        if (model && LOWORD(wp) == kPcBubbleSwitch && HIWORD(wp) == BN_CLICKED) {
            if (liquid::settings::WriteBool(model->settingsPath, "phonePairing", "pcReceiveBubble", !model->pcBubble)
                    == liquid::settings::SaveStatus::Saved) {
                model->pcBubble = !model->pcBubble;
                const auto toggle = GetDlgItem(window, kPcBubbleSwitch);
                UpdateToggleName(toggle, model->pcBubble);
                InvalidateRect(toggle, nullptr, FALSE);
            } else MessageBoxW(window, Ui(L"无法保存设置。"), L"DockDrop", MB_OK | MB_ICONWARNING);
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
