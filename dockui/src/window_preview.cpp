#include "window_preview.h"
#include "ui_language.h"

#include <windowsx.h>
#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace dock {
namespace {
constexpr wchar_t kPreviewClass[] = L"DockOverWindowPreview";
constexpr int kPrevious = -2, kNext = -3, kNoHit = -1;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
}

WindowPreview::~WindowPreview() {
    Hide();
    if (window_) DestroyWindow(window_);
}

bool WindowPreview::Show(HWND owner, UINT activateMessage, RECT anchor, UINT dpi, bool dark,
                         const std::wstring& appName, std::vector<HWND> sources) {
    if (sources.empty()) return false;
    if (!window_) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPreviewClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPreviewClass, L"",
                                  WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, this);
        if (!window_) return false;
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    }
    if (Visible() && sources == sources_ && dpi == dpi_ && dark == dark_ && appName == name_) return true;
    const bool preservePage = Visible() && appName == name_;
    if (!preservePage || dpi != dpi_) anchor_ = anchor;
    owner_ = owner;
    activateMessage_ = activateMessage;
    dpi_ = dpi;
    dark_ = dark;
    name_ = appName;
    sources_ = std::move(sources);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    work_ = monitor.rcWork;
    // The system taskbar may reserve work area above the Dock's actual bottom edge.
    const int width = work_.right - work_.left - Px(16);
    const int usableHeight = (std::min(anchor_.top, work_.bottom) - work_.top) - Px(16);
    columns_ = std::clamp((width - Px(24) + Px(8)) / Px(204), 1, 4);
    cellWidth_ = std::clamp(width - Px(24), Px(80), Px(196));
    imageHeight_ = std::clamp(usableHeight - Px(76), Px(40), Px(114));
    cellHeight_ = imageHeight_ + Px(28);
    const int rows = std::clamp((usableHeight - Px(46)) / (cellHeight_ + Px(8)), 1, 2);
    pageSize_ = columns_ * rows;
    page_ = preservePage ? std::min(page_, (static_cast<int>(sources_.size()) - 1) / pageSize_) : 0;
    const BOOL darkTitle = dark_;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
    LayoutPage();
    return Visible();
}

void WindowPreview::ClearTiles() {
    for (const auto& tile : tiles_) {
        if (tile.thumbnail) DwmUnregisterThumbnail(tile.thumbnail);
    }
    tiles_.clear();
}

void WindowPreview::Hide() {
    if (window_ && GetCapture() == window_) ReleaseCapture();
    if (Visible()) ShowWindow(window_, SW_HIDE);
    ClearTiles();
    sources_.clear();
    hovered_ = pressed_ = kNoHit;
}

void WindowPreview::LayoutPage() {
    ClearTiles();
    hovered_ = pressed_ = kNoHit;
    const int first = page_ * pageSize_;
    const int count = std::min(pageSize_, static_cast<int>(sources_.size()) - first);
    const int pageCapacity = std::min(pageSize_, static_cast<int>(sources_.size()));
    const int cols = std::min(pageCapacity, columns_);
    const int rows = (pageCapacity + cols - 1) / cols;
    const int pad = Px(12), gap = Px(8), top = Px(34);
    const int width = pad * 2 + cols * cellWidth_ + (cols - 1) * gap;
    const int height = top + pad + rows * cellHeight_ + (rows - 1) * gap;
    const int left = std::clamp<LONG>((anchor_.left + anchor_.right - width) / 2,
                                    work_.left, std::max(work_.left, work_.right - width));
    const int y = std::max<int>(work_.top, std::min(anchor_.top, work_.bottom) - Px(8) - height);
    previous_ = {width - Px(64), Px(5), width - Px(40), Px(29)};
    next_ = {width - Px(36), Px(5), width - Px(12), Px(29)};
    SetWindowPos(window_, HWND_TOPMOST, left, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    for (int i = 0; i < count; ++i) {
        const int x = pad + i % cols * (cellWidth_ + gap);
        const int tileTop = top + i / cols * (cellHeight_ + gap);
        Tile tile{sources_[first + i], {x, tileTop, x + cellWidth_, tileTop + cellHeight_}};
        if (SUCCEEDED(DwmRegisterThumbnail(window_, tile.source, &tile.thumbnail))) {
            SIZE size{};
            const int margin = Px(5);
            const int w = cellWidth_ - 2 * margin, h = imageHeight_ - margin;
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
            props.fVisible = TRUE;
            if (SUCCEEDED(DwmQueryThumbnailSourceSize(tile.thumbnail, &size)) && size.cx > 0 && size.cy > 0) {
                const double scale = std::min(double(w) / size.cx, double(h) / size.cy);
                const int sw = std::max(1, static_cast<int>(size.cx * scale));
                const int sh = std::max(1, static_cast<int>(size.cy * scale));
                const int sx = x + (cellWidth_ - sw) / 2, sy = tileTop + margin + (h - sh) / 2;
                props.rcDestination = {sx, sy, sx + sw, sy + sh};
                if (SUCCEEDED(DwmUpdateThumbnailProperties(tile.thumbnail, &props))) {
                    tiles_.push_back(tile);
                    continue;
                }
            }
            DwmUnregisterThumbnail(tile.thumbnail);
            tile.thumbnail = nullptr;
        }
        tiles_.push_back(tile);
    }
    InvalidateRect(window_, nullptr, FALSE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

void WindowPreview::ChangePage(int delta) {
    const int target = page_ + delta;
    if (target < 0 || target * pageSize_ >= static_cast<int>(sources_.size())) return;
    page_ = target;
    LayoutPage();
}

int WindowPreview::Hit(POINT p) const {
    if (page_ > 0 && PtInRect(&previous_, p)) return kPrevious;
    if ((page_ + 1) * pageSize_ < static_cast<int>(sources_.size()) && PtInRect(&next_, p)) return kNext;
    for (size_t i = 0; i < tiles_.size(); ++i) {
        if (PtInRect(&tiles_[i].bounds, p)) return static_cast<int>(i);
    }
    return kNoHit;
}

void WindowPreview::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window_, &ps);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const COLORREF surface = highContrast ? GetSysColor(COLOR_WINDOW) : dark_ ? RGB(32, 33, 37) : RGB(243, 244, 247);
    const COLORREF card = highContrast ? GetSysColor(COLOR_BTNFACE) : dark_ ? RGB(48, 50, 56) : RGB(226, 230, 237);
    const COLORREF active = highContrast ? GetSysColor(COLOR_HIGHLIGHT) : dark_ ? RGB(65, 73, 87) : RGB(199, 214, 235);
    const COLORREF text = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : dark_ ? RGB(247, 247, 249) : RGB(26, 29, 36);
    Fill(dc, bounds, surface);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-Px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto oldFont = SelectObject(dc, font);
    SetTextColor(dc, text);
    RECT heading{Px(13), 0, bounds.right - Px(sources_.size() > static_cast<size_t>(pageSize_) ? 68 : 13), Px(34)};
    DrawTextW(dc, name_.c_str(), -1, &heading, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    for (auto [id, box, glyph, enabled] : std::array{
        std::tuple{kPrevious, previous_, L"\u2039", page_ > 0},
        std::tuple{kNext, next_, L"\u203a", (page_ + 1) * pageSize_ < static_cast<int>(sources_.size())}}) {
        if (!enabled) continue;
        if (hovered_ == id) Fill(dc, box, active);
        SetTextColor(dc, highContrast && hovered_ == id ? GetSysColor(COLOR_HIGHLIGHTTEXT) : text);
        DrawTextW(dc, glyph, -1, &box, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const auto& tile = tiles_[i];
        const bool selected = hovered_ == static_cast<int>(i) || pressed_ == static_cast<int>(i);
        Fill(dc, tile.bounds, selected ? active : card);
        SetTextColor(dc, highContrast && selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : text);
        if (!tile.thumbnail) {
            RECT image = tile.bounds;
            image.bottom = image.top + imageHeight_;
            DrawTextW(dc, docky::ui::Text(L"Unavailable"), -1, &image,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        wchar_t title[512]{};
        GetWindowTextW(tile.source, title, ARRAYSIZE(title));
        RECT caption = tile.bounds;
        caption.top += imageHeight_;
        caption.left += Px(7);
        caption.right -= Px(7);
        DrawTextW(dc, title[0] ? title : name_.c_str(), -1, &caption,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    EndPaint(window_, &ps);
}

LRESULT CALLBACK WindowPreview::WndProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<WindowPreview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<WindowPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->Handle(message, w, l) : DefWindowProcW(window, message, w, l);
}

LRESULT WindowPreview::Handle(UINT message, WPARAM w, LPARAM l) {
    const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_MOUSEMOVE: {
        const int hit = Hit(p);
        if (hit == hovered_) return 0;
        hovered_ = hit;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        hovered_ = kNoHit;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = Hit(p);
        if (pressed_ != kNoHit) SetCapture(window_);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = Hit(p);
        const bool clicked = hit != kNoHit && pressed_ == hit;
        pressed_ = kNoHit;
        if (GetCapture() == window_) ReleaseCapture();
        if (!clicked) { InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (hit == kPrevious || hit == kNext) ChangePage(hit == kPrevious ? -1 : 1);
        else SendMessageW(owner_, activateMessage_, reinterpret_cast<WPARAM>(tiles_[hit].source), 0);
        return 0;
    }
    case WM_CAPTURECHANGED: pressed_ = kNoHit; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_NCDESTROY: {
        const HWND window = window_;
        window_ = nullptr;
        return DefWindowProcW(window, message, w, l);
    }
    default: return DefWindowProcW(window_, message, w, l);
    }
}

} // namespace dock
