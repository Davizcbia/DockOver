#pragma once

#include "phone_settings_window.h"
#include "../motion.h"
#include "../notification_card_metrics.h"
#include "../theme_state.h"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <charconv>
#include <string_view>

namespace docky::pairing::pc_receive {

constexpr UINT_PTR kTick = 1;
constexpr int kReject = 51, kAccept = 52, kOpen = 53;

struct Offer {
    std::wstring sender, file, bytes, folder;
    bool bubble = true, sending = false, reduced = false, accepted = false, complete = false, failed = false, declined = false;
    int width = notification_card::Width, height = 146, percent = 0;
    double scale = 1.0;
    int x = 0, y = 0;
    ULONGLONG last = 0, closeAt = 0;
    HWND decline{}, accept{}, open{};
    HFONT title{}, body{};
    HICON icon{};
    motion::Spring entrance;
    docky::vblank::Ticker entranceTicker;
    double stepTime = 0;
    std::string pending;
    phone_settings::Palette palette = phone_settings::ChoosePalette();

    ~Offer() {
        if (title) DeleteObject(title);
        if (body) DeleteObject(body);
        if (icon) DestroyIcon(icon);
    }
    int Dip(int value) const { return static_cast<int>(std::lround(value * scale)); }
    RECT Rect(int x0, int y0, int w, int h) const { return {Dip(x0), Dip(y0), Dip(x0 + w), Dip(y0 + h)}; }
    bool Terminal() const { return complete || failed; }
};

inline void Paint(HWND window, const Offer& offer) {
    using phone_settings::Color;
    using phone_settings::Label;
    using phone_settings::Rounded;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    const HDC memory = CreateCompatibleDC(dc);
    const HBITMAP frame = memory ? CreateCompatibleBitmap(dc, offer.Dip(offer.width), offer.Dip(offer.height)) : nullptr;
    const HGDIOBJ old = frame ? SelectObject(memory, frame) : nullptr;
    const HDC dest = frame ? memory : dc;
    const auto& colors = offer.palette;
    const auto bounds = offer.Rect(0, 0, offer.width, offer.height);
    const auto background = CreateSolidBrush(colors.surface);
    FillRect(dest, &bounds, background);
    DeleteObject(background);
    constexpr int iconSize = notification_card::Icon, iconX = notification_card::IconInset, iconY = 14;
    if (offer.icon) DrawIconEx(dest, offer.Dip(iconX), offer.Dip(iconY), offer.icon,
        offer.Dip(iconSize), offer.Dip(iconSize), 0, nullptr, DI_NORMAL);

    const int textX = static_cast<int>(std::lround(notification_card::TextInset));
    constexpr int top = 12;
    const int textWidth = offer.width - textX - 13;
    const wchar_t* state = offer.complete ? Ui(L"接收完成") : offer.failed ? Ui(L"传输已中断") :
        offer.accepted ? Ui(offer.percent >= 100 ? L"正在校验文件…" : L"正在接收文件…") : Ui(L"收到电脑传输请求");
    Label(dest, state, offer.Rect(textX, top, textWidth, 23), offer.title, colors.text);
    Label(dest, offer.sender.c_str(), offer.Rect(textX, top + 24, textWidth, 19), offer.body, colors.secondary);
    const std::wstring fileDetails = offer.file + L"  ·  " + offer.bytes;
    Label(dest, fileDetails.c_str(), offer.Rect(textX, top + 45, textWidth, 19), offer.body, colors.secondary);
    {
        Gdiplus::Graphics graphics(dest);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (offer.accepted && !offer.Terminal()) {
            const int progress = std::min(offer.percent, 99);
            Rounded(graphics, offer.Rect(19, offer.height - 36, offer.width - 38, 5), offer.Dip(2), colors.edge);
            const int fill = (offer.width - 38) * progress / 100;
            if (fill >= 4) Rounded(graphics, offer.Rect(19, offer.height - 36, fill, 5), offer.Dip(2), colors.accent);
        }
    }
    if (frame) {
        BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(frame);
    }
    if (memory) DeleteDC(memory);
    EndPaint(window, &ps);
}

inline bool Answer(const char* line, DWORD length) {
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, length, &written, nullptr) && written == length;
}

inline void PaintButton(const DRAWITEMSTRUCT& item, const Offer& offer) {
    const auto& colors = offer.palette;
    const auto background = CreateSolidBrush(colors.surface);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
    const bool primary = item.CtlID != kReject;
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    RECT bounds = item.rcItem;
    InflateRect(&bounds, -1, -1);
    phone_settings::Rounded(graphics, bounds, offer.Dip(13),
        pressed ? colors.hover : primary ? colors.tint : colors.surface, focused ? colors.accent : colors.edge);
    wchar_t text[80]{};
    GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    phone_settings::Label(item.hDC, text, bounds, offer.body, primary ? colors.accent : colors.text,
        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

inline void ApplyUpdate(HWND window, Offer& offer, std::string_view line, ULONGLONG now) {
    if (offer.Terminal()) return;
    if (line == "accepted" && offer.sending) {
        offer.accepted = true;
    } else if (line == "done" || line == "failed" || line == "declined") {
        offer.complete = line == "done" && offer.accepted;
        offer.failed = !offer.complete;
        offer.declined = line == "declined";
        if (offer.complete && offer.open) ShowWindow(offer.open, SW_SHOWNOACTIVATE);
        offer.closeAt = now + (offer.complete ? 5500 : 3600);
    } else if (offer.accepted && line.starts_with("p ")) {
        unsigned value = 0;
        const auto first = line.data() + 2, last = line.data() + line.size();
        const auto result = std::from_chars(first, last, value);
        if (result.ec == std::errc{} && result.ptr == last && value <= 100)
            offer.percent = std::max(offer.percent, static_cast<int>(value));
    }
    InvalidateRect(window, nullptr, FALSE);
}

inline void PollInput(HWND window, Offer& offer, ULONGLONG now) {
    if (offer.Terminal()) return;
    const HANDLE pipe = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        if (offer.accepted || offer.sending) ApplyUpdate(window, offer, "failed", now);
        else DestroyWindow(window);
        return;
    }
    if (!available) return;
    char bytes[256];
    DWORD read = 0;
    if (!ReadFile(pipe, bytes, std::min<DWORD>(available, sizeof(bytes)), &read, nullptr)) return;
    offer.pending.append(bytes, read);
    for (auto end = offer.pending.find('\n'); end != std::string::npos; end = offer.pending.find('\n')) {
        auto line = std::string_view(offer.pending).substr(0, end);
        if (line.ends_with("\r")) line.remove_suffix(1);
        ApplyUpdate(window, offer, line, now);
        offer.pending.erase(0, end + 1);
    }
    if (offer.pending.size() > 512) offer.pending.clear();
}

inline LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* offer = reinterpret_cast<Offer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!offer) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_PAINT: Paint(window, *offer); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: PaintButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp), *offer); return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == kAccept && !offer->accepted) {
            if (!Answer("accept\n", 7)) { DestroyWindow(window); return 0; }
            offer->accepted = true;
            ShowWindow(offer->accept, SW_HIDE);
            ShowWindow(offer->decline, SW_HIDE);
            InvalidateRect(window, nullptr, FALSE);
        } else if (LOWORD(wp) == kReject && !offer->accepted) {
            Answer("decline\n", 8);
            DestroyWindow(window);
        } else if (LOWORD(wp) == kOpen && offer->complete) {
            ShellExecuteW(window, L"open", offer->folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            DestroyWindow(window);
        }
        return 0;
    case docky::vblank::kMsgVBlank: {
        offer->entranceTicker.Ack();
        const double seconds=docky::vblank::Seconds();
        offer->entrance.Step(std::clamp(seconds-offer->stepTime,0.0,0.05));
        offer->stepTime=seconds;
        const double remaining=1.0-offer->entrance.Value();
        SetWindowPos(window,nullptr,offer->x+offer->Dip(static_cast<int>(remaining*offer->width)),offer->y,0,0,
                     SWP_NOACTIVATE|SWP_NOSIZE|SWP_NOZORDER);
        if(offer->entrance.AtRest())offer->entranceTicker.Stop();
        return 0;
    }
    case WM_TIMER: {
        const auto now = GetTickCount64();
        if (offer->Terminal() && now >= offer->closeAt) { DestroyWindow(window); return 0; }
        offer->last = now;
        PollInput(window, *offer, now);
        return 0;
    }
    case WM_CLOSE:
        if (!offer->accepted) Answer("decline\n", 8);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        offer->entranceTicker.Stop();
        KillTimer(window, kTick);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

inline int RunBanner(HINSTANCE instance, Offer& offer) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
    {
        DWORD light = 1, bytes = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
        HIGHCONTRASTW accessibility{sizeof(accessibility)};
        const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(accessibility), &accessibility, 0)
            && (accessibility.dwFlags & HCF_HIGHCONTRASTON) != 0;
        const int mode = liquid::settings::Load(liquid::settings::DefaultPath()).value.appearance.mode;
        const auto colors = liquid::theme::Palette(mode, light == 0, contrast);
        using liquid::theme::ColorRef;
        offer.palette.surface = ColorRef(colors.surface);
        offer.palette.text = ColorRef(colors.primary);
        offer.palette.secondary = ColorRef(colors.secondary);
        offer.palette.accent = ColorRef(colors.accent);
        offer.palette.edge = ColorRef(colors.divider);
        offer.palette.tint = ColorRef(colors.selected);
        offer.palette.hover = ColorRef(colors.hover);
    }
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DockDropPcReceiveBanner";
    RegisterClassW(&wc);
    const auto monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96, vertical = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical);
    offer.scale = dpi / 96.0;
    offer.x = mi.rcWork.right - offer.Dip(offer.width + notification_card::RightMargin);
    offer.y = std::max(mi.rcWork.top, mi.rcMonitor.top + offer.Dip(24)) + offer.Dip(10);
    offer.title = CreateFontW(-offer.Dip(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    offer.body = CreateFontW(-offer.Dip(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const int icon = offer.Dip(notification_card::Icon);
    offer.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, icon, icon, 0));
    BOOL animation = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    offer.reduced = !animation;
    offer.entrance.SetSpec(motion::tokens::BannerIn);
    offer.entrance.SnapTo(offer.reduced ? 1.0 : 0.0);
    offer.entrance.SetTarget(1.0);
    const int w = offer.Dip(offer.width), h = offer.Dip(offer.height);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
        wc.lpszClassName, L"DockDrop", WS_POPUP | WS_CLIPCHILDREN,
        offer.x + (!offer.reduced ? w : 0), offer.y,
        w, h, nullptr, nullptr, instance, &offer);
    if (!window) { Gdiplus::GdiplusShutdown(token); return 1; }
    const auto region = CreateRoundRectRgn(0, 0, w + 1, h + 1,
        offer.Dip(notification_card::Radius * 2), offer.Dip(notification_card::Radius * 2));
    if (region && !SetWindowRgn(window, region, FALSE)) DeleteObject(region);
    const auto button = [&](const wchar_t* title, int id, int x, int width, bool visible) {
        HWND child = CreateWindowW(L"BUTTON", title, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0),
            offer.Dip(x), offer.Dip(offer.height - 47), offer.Dip(width), offer.Dip(30), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(offer.body), 0);
        return child;
    };
    offer.decline = button(Ui(L"拒绝"), kReject, offer.width - 186, 76, true);
    offer.accept = button(Ui(L"接受"), kAccept, offer.width - 100, 81, true);
    offer.open = button(Ui(L"打开文件夹"), kOpen, offer.width - 145, 126, false);
    offer.last = GetTickCount64();
    offer.stepTime=docky::vblank::Seconds();
    offer.entranceTicker.Start(window,docky::vblank::kMsgVBlank);
    SetTimer(window,kTick,50,nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}

}  // namespace docky::pairing::pc_receive
