#include "window_preview.h"
#include "backdrop_wgc.h"
#include "design_tokens.h"
#include "log.h"
#include "menu_surface.h"
#include "ui_language.h"

#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <tuple>
#include <utility>

namespace dock {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kPreviewClass[] = L"DockOverWindowPreview";
constexpr wchar_t kPreviewClassGdi[] = L"DockOverWindowPreviewGdi";
constexpr int kPrevious = -2, kNext = -3, kNoHit = -1, kButtonBase = 1000;
constexpr UINT_PTR kTimerCapture = 1, kTimerAppear = 2, kTimerHealth = 3;

// Layout (DIP). Corners are concentric: each nested shape's radius is its parent's minus the inset between them.
constexpr int kPad = 8;                               // panel edge -> card
constexpr int kGap = 8;                               // between cards
constexpr int kHeader = 38;                           // icon + app name strip
constexpr int kInset = 4;                             // card edge -> capsules and thumbnail well
constexpr int kBar = 24;                              // capsule height
constexpr float kCardRadius = 16.f;
constexpr float kInnerRadius = kCardRadius - kInset;  // 12: capsules (kBar / 2) and the thumbnail well
constexpr float kPanelRadius = kCardRadius + kPad;    // 24
constexpr int kDot = 12, kDotGap = 7, kDotPad = 8;    // traffic-light buttons inside their capsule
constexpr int kDotsWidth = kDotPad * 2 + kDot * 3 + kDotGap * 2;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

IDWriteFactory* WriteFactory() {
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> f;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

ComPtr<IDWriteTextFormat> Format(float size, DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> format;
    if (!WriteFactory() || FAILED(WriteFactory()->CreateTextFormat(liquid::design::type::Family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                                   DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(WriteFactory()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.Get());
    }
    return format;
}

D2D1_RECT_F ToDip(const RECT& r, float k) {
    return D2D1::RectF(r.left / k, r.top / k, r.right / k, r.bottom / k);
}

std::wstring TitleOf(HWND window, const std::wstring& fallback) {
    wchar_t title[512]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    return title[0] ? title : fallback;
}

// macOS window-button colours (close, minimize, zoom).
constexpr D2D1_COLOR_F kButtonColor[3] = {{1.f, 95 / 255.f, 87 / 255.f, 1.f},
                                          {254 / 255.f, 188 / 255.f, 46 / 255.f, 1.f},
                                          {40 / 255.f, 200 / 255.f, 64 / 255.f, 1.f}};
}  // namespace

WindowPreview::WindowPreview() = default;

WindowPreview::~WindowPreview() {
    Hide();
    surface_.reset();
    if (window_) DestroyWindow(window_);
}

bool WindowPreview::CreateWindowAndSurface(HWND owner) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    for (const wchar_t* name : {kPreviewClass, kPreviewClassGdi}) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = name;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    // GPU: the menu's glass surface (DirectComposition, no redirection bitmap). The live thumbnails are composed by DWM
    // above the window's content.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kPreviewClass, L"",
                              WS_POPUP, 0, 0, Px(200), Px(120), owner, nullptr, instance, this);
    if (window_) {
        auto surface = std::make_unique<liquid::popup::MenuSurface>();
        RECT r{};
        GetClientRect(window_, &r);
        UINT dpi = GetDpiForWindow(window_);
        if (!dpi) dpi = 96;
        if (surface->Init(window_, r.right, r.bottom, static_cast<float>(dpi))) {
            surface_ = std::move(surface);
            SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);  // the glass samples the screen without itself
            return true;
        }
        DLOG(L"window preview: glass surface unavailable; GDI panel");
        DestroyWindow(window_);
        window_ = nullptr;
    }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPreviewClassGdi, L"", WS_POPUP, 0, 0, 0, 0, owner,
                              nullptr, instance, this);
    if (!window_) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    return true;
}

bool WindowPreview::Show(HWND owner, UINT activateMessage, RECT anchor, UINT dpi, bool dark,
                         const std::wstring& appName, std::vector<HWND> sources,
                         const std::vector<std::uint32_t>& iconPixels, UINT iconW, UINT iconH) {
    if (sources.empty()) return false;
    if (!window_) {
        dpi_ = dpi;
        if (!CreateWindowAndSurface(owner)) return false;
    }
    if (Visible() && sources == sources_ && dpi == dpi_ && dark == dark_ && appName == name_) return true;
    const bool wasVisible = Visible();
    const bool preservePage = wasVisible && appName == name_;
    if (!preservePage || dpi != dpi_) anchor_ = anchor;
    owner_ = owner;
    activateMessage_ = activateMessage;
    dpi_ = dpi;
    dark_ = dark;
    name_ = appName;
    sources_ = std::move(sources);
    if (iconW && iconH && iconPixels.size() == static_cast<size_t>(iconW) * iconH) {
        icon_ = iconPixels; iconW_ = iconW; iconH_ = iconH;
    } else {
        icon_.clear(); iconW_ = iconH_ = 0;
    }
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    work_ = monitor.rcWork;
    // The system taskbar may reserve work area above the Dock's actual bottom edge.
    const int width = work_.right - work_.left - Px(16);
    const int usableHeight = (std::min(anchor_.top, work_.bottom) - work_.top) - Px(16);
    columns_ = std::clamp((width - Px(2 * kPad) + Px(kGap)) / Px(232 + kGap), 1, 4);
    cellWidth_ = std::clamp(width - Px(2 * kPad), Px(120), Px(232));
    // Card: inset, button bar, inset, thumbnail well, inset.
    const int chrome = Px(kInset * 3 + kBar);
    imageHeight_ = std::clamp(usableHeight - Px(kHeader + kPad) - chrome, Px(48), Px(136));
    cellHeight_ = imageHeight_ + chrome;
    const int rows = std::clamp((usableHeight - Px(kHeader + kPad) + Px(kGap)) / (cellHeight_ + Px(kGap)), 1, 2);
    pageSize_ = columns_ * rows;
    page_ = preservePage ? std::min(page_, (static_cast<int>(sources_.size()) - 1) / pageSize_) : 0;
    if (!surface_) {
        const BOOL darkTitle = dark_;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
    }
    if (!wasVisible && surface_) {
        appear_ = 0.f;
        shownAt_ = GetTickCount64();
        surface_->Materialize(0.f, false);
        surface_->Start();
        SetTimer(window_, kTimerAppear, 15, nullptr);
        SetTimer(window_, kTimerHealth, 1000, nullptr);
    }
    LayoutPage();
    return Visible();
}

bool WindowPreview::RequestSnapshot(const std::wstring& path) {
    if (!surface_ || !Visible()) return false;
    surface_->RequestSnapshot(path);
    InvalidateRect(window_, nullptr, FALSE);
    return true;
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
    if (window_) {
        KillTimer(window_, kTimerCapture);
        KillTimer(window_, kTimerAppear);
        KillTimer(window_, kTimerHealth);
    }
    if (surface_) surface_->Suspend();
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
    const int pad = Px(kPad), gap = Px(kGap), top = Px(kHeader), inset = Px(kInset), bar = Px(kBar);
    const int width = pad * 2 + cols * cellWidth_ + (cols - 1) * gap;
    const int height = top + pad + rows * cellHeight_ + (rows - 1) * gap;
    const int left = std::clamp<LONG>((anchor_.left + anchor_.right - width) / 2,
                                      work_.left, std::max(work_.left, work_.right - width));
    const int y = std::max<int>(work_.top, std::min(anchor_.top, work_.bottom) - Px(8) - height);
    const int arrow = Px(26);
    next_ = {width - pad - arrow, (top - arrow) / 2, width - pad, (top + arrow) / 2};
    previous_ = {next_.left - Px(4) - arrow, next_.top, next_.left - Px(4), next_.bottom};
    SetWindowPos(window_, HWND_TOPMOST, left, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (surface_) surface_->Resize(static_cast<UINT>(width), static_cast<UINT>(height), static_cast<float>(dpi_));
    for (int i = 0; i < count; ++i) {
        const int x = pad + i % cols * (cellWidth_ + gap);
        const int tileTop = top + i / cols * (cellHeight_ + gap);
        Tile tile{sources_[first + i], {x, tileTop, x + cellWidth_, tileTop + cellHeight_}};
        const int barTop = tileTop + inset;
        tile.image = {x + inset, barTop + bar + inset, x + cellWidth_ - inset, tileTop + cellHeight_ - inset};
        const int dot = Px(kDot), dotGap = Px(kDotGap), dotLeft = x + inset + Px(kDotPad);
        for (int b = 0; b < 3; ++b) {
            const int l = dotLeft + b * (dot + dotGap);
            tile.buttons[b] = {l - dotGap / 2, barTop, l + dot + dotGap / 2, barTop + bar};
        }
        tile.title = {x + inset + Px(kDotsWidth) + inset, barTop, x + cellWidth_ - inset, barTop + bar};
        if (SUCCEEDED(DwmRegisterThumbnail(window_, tile.source, &tile.thumbnail))) {
            SIZE size{};
            const int margin = Px(3);
            const int w = tile.image.right - tile.image.left - 2 * margin, h = tile.image.bottom - tile.image.top - 2 * margin;
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
            props.fVisible = TRUE;
            props.fSourceClientAreaOnly = FALSE;
            if (SUCCEEDED(DwmQueryThumbnailSourceSize(tile.thumbnail, &size)) && size.cx > 0 && size.cy > 0 && w > 0 && h > 0) {
                const double scale = std::min(double(w) / size.cx, double(h) / size.cy);
                const int sw = std::max(1, static_cast<int>(size.cx * scale));
                const int sh = std::max(1, static_cast<int>(size.cy * scale));
                const int sx = tile.image.left + (tile.image.right - tile.image.left - sw) / 2;
                const int sy = tile.image.top + (tile.image.bottom - tile.image.top - sh) / 2;
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
        for (int b = 0; b < 3; ++b)
            if (PtInRect(&tiles_[i].buttons[b], p)) return kButtonBase + static_cast<int>(i) * 3 + b;
        if (PtInRect(&tiles_[i].bounds, p)) return static_cast<int>(i);
    }
    return kNoHit;
}

// Close asks the window to close (it may still ask to save); minimize and zoom use the window's own system commands.
void WindowPreview::WindowCommand(int index, int button) {
    if (index < 0 || static_cast<size_t>(index) >= tiles_.size()) return;
    const HWND window = tiles_[index].source;
    if (!IsWindow(window)) return;
    WPARAM command = button == 0 ? SC_CLOSE : button == 1 ? SC_MINIMIZE : IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
    PostMessageW(window, WM_SYSCOMMAND, command, 0);
    if (button == 2) return;  // zoom keeps the window: its thumbnail follows
    sources_.erase(std::remove(sources_.begin(), sources_.end(), window), sources_.end());
    if (sources_.empty()) { Hide(); return; }
    page_ = std::min(page_, (static_cast<int>(sources_.size()) - 1) / pageSize_);
    LayoutPage();
}

void WindowPreview::DrawGlassContent(ID2D1DeviceContext* dc, bool gpu) {
    RECT client{};
    GetClientRect(window_, &client);
    const float k = static_cast<float>(dpi_) / 96.f;
    const D2D1_RECT_F panel = ToDip(client, k);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) return;
    auto ink = [&](float a) { brush->SetColor(D2D1::ColorF(1, 1, 1, a)); return brush.Get(); };
    if (!gpu) {  // no captured backdrop this frame: a dark plate in the glass's shape
        brush->SetColor(D2D1::ColorF(36 / 255.f, 38 / 255.f, 44 / 255.f, 0.97f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(panel, kPanelRadius, kPanelRadius), brush.Get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.left + .5f, panel.top + .5f, panel.right - .5f, panel.bottom - .5f),
                                                   kPanelRadius - .5f, kPanelRadius - .5f), ink(0.14f), 1.f);
    }
    static ComPtr<IDWriteTextFormat> nameFormat = Format(liquid::design::type::Emphasis, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    static ComPtr<IDWriteTextFormat> titleFormat = Format(liquid::design::type::Secondary, DWRITE_FONT_WEIGHT_NORMAL);
    static ComPtr<IDWriteTextFormat> arrowFormat = [] {
        auto f = Format(18.f, DWRITE_FONT_WEIGHT_NORMAL);
        if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        return f;
    }();

    // Header: the app's icon and name, page arrows on the right.
    const float icon = 22.f, headerLeft = kPad + 6.f, mid = kHeader / 2.f + 1.f;
    float textLeft = headerLeft;
    if (iconW_ && iconH_) {
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(iconW_, iconH_), icon_.data(), iconW_ * 4, &props, &bitmap))) {
            const float s = icon / static_cast<float>((std::max)(iconW_, iconH_));
            const float w = iconW_ * s, h = iconH_ * s;
            dc->DrawBitmap(bitmap.Get(), D2D1::RectF(headerLeft, mid - h / 2, headerLeft + w, mid + h / 2), 1.f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            textLeft += icon + 8.f;
        }
    }
    const bool paged = sources_.size() > static_cast<size_t>(pageSize_);
    const float nameRight = paged ? ToDip(previous_, k).left - 6.f : panel.right - kPad - 6.f;
    if (nameFormat)
        dc->DrawTextW(name_.c_str(), static_cast<UINT32>(name_.size()), nameFormat.Get(),
                      D2D1::RectF(textLeft, 0, (std::max)(textLeft, nameRight), static_cast<float>(kHeader)), ink(0.96f),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (paged && arrowFormat) {
        for (auto [id, box, glyph, enabled] : {std::tuple{kPrevious, previous_, L"‹", page_ > 0},
                                               std::tuple{kNext, next_, L"›", (page_ + 1) * pageSize_ < static_cast<int>(sources_.size())}}) {
            const D2D1_RECT_F r = ToDip(box, k);
            if (enabled && hovered_ == id) dc->FillRoundedRectangle(D2D1::RoundedRect(r, 13.f, 13.f), ink(0.18f));
            dc->DrawTextW(glyph, 1, arrowFormat.Get(), D2D1::RectF(r.left, r.top - 2.f, r.right, r.bottom - 2.f), ink(enabled ? 0.92f : 0.3f));
        }
    }

    // Cards.
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const auto& tile = tiles_[i];
        const int base = kButtonBase + static_cast<int>(i) * 3;
        const bool onButtons = hovered_ >= base && hovered_ < base + 3;
        const bool hot = hovered_ == static_cast<int>(i) || onButtons;
        const bool down = pressed_ == static_cast<int>(i);
        const D2D1_RECT_F card = ToDip(tile.bounds, k);
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, kCardRadius, kCardRadius), ink(down ? 0.24f : hot ? 0.17f : 0.09f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + .5f, card.top + .5f, card.right - .5f, card.bottom - .5f),
                                                   kCardRadius - .5f, kCardRadius - .5f), ink(hot ? 0.22f : 0.12f), 1.f);
        // Button capsule and title capsule (radius = card radius - inset).
        const float barTop = card.top + kInset;
        const D2D1_RECT_F dots = D2D1::RectF(card.left + kInset, barTop, card.left + kInset + kDotsWidth, barTop + kBar);
        dc->FillRoundedRectangle(D2D1::RoundedRect(dots, kInnerRadius, kInnerRadius), ink(0.10f));
        for (int b = 0; b < 3; ++b) {
            const float cx = dots.left + kDotPad + kDot / 2.f + b * (kDot + kDotGap), cy = barTop + kBar / 2.f;
            const auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), kDot / 2.f, kDot / 2.f);
            if (hot) {
                D2D1_COLOR_F c = kButtonColor[b];
                if (pressed_ == base + b) { c.r *= .75f; c.g *= .75f; c.b *= .75f; }
                brush->SetColor(c);
                dc->FillEllipse(ellipse, brush.Get());
            } else {
                dc->FillEllipse(ellipse, ink(0.32f));
            }
            if (onButtons) {  // glyphs appear while the pointer is over the buttons, as on macOS
                brush->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
                const float g = 2.9f, w = 1.2f;
                if (b == 0) {
                    dc->DrawLine(D2D1::Point2F(cx - g, cy - g), D2D1::Point2F(cx + g, cy + g), brush.Get(), w);
                    dc->DrawLine(D2D1::Point2F(cx - g, cy + g), D2D1::Point2F(cx + g, cy - g), brush.Get(), w);
                } else if (b == 1) {
                    dc->DrawLine(D2D1::Point2F(cx - g - .4f, cy), D2D1::Point2F(cx + g + .4f, cy), brush.Get(), w);
                } else {
                    dc->DrawLine(D2D1::Point2F(cx - g - .4f, cy), D2D1::Point2F(cx + g + .4f, cy), brush.Get(), w);
                    dc->DrawLine(D2D1::Point2F(cx, cy - g - .4f), D2D1::Point2F(cx, cy + g + .4f), brush.Get(), w);
                }
            }
        }
        const D2D1_RECT_F title = ToDip(tile.title, k);
        dc->FillRoundedRectangle(D2D1::RoundedRect(title, kInnerRadius, kInnerRadius), ink(0.10f));
        if (titleFormat) {
            const std::wstring text = TitleOf(tile.source, name_);
            dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), titleFormat.Get(),
                          D2D1::RectF(title.left + 10.f, title.top, title.right - 10.f, title.bottom), ink(0.9f),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        // Thumbnail well; DWM draws the live thumbnail over it.
        const D2D1_RECT_F well = ToDip(tile.image, k);
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.16f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(well, kInnerRadius, kInnerRadius), brush.Get());
        if (!tile.thumbnail && titleFormat) {
            const wchar_t* unavailable = docky::ui::Text(L"Unavailable");
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->DrawTextW(unavailable, static_cast<UINT32>(wcslen(unavailable)), titleFormat.Get(), well, ink(0.6f));
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
}

void WindowPreview::PaintGdi(HDC dc) {
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
    RECT heading{Px(kPad + 6), 0, bounds.right - Px(sources_.size() > static_cast<size_t>(pageSize_) ? 72 : kPad), Px(kHeader)};
    DrawTextW(dc, name_.c_str(), -1, &heading, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    for (auto [id, box, glyph, enabled] : {std::tuple{kPrevious, previous_, L"‹", page_ > 0},
                                           std::tuple{kNext, next_, L"›", (page_ + 1) * pageSize_ < static_cast<int>(sources_.size())}}) {
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
        for (int b = 0; b < 3; ++b) {
            const COLORREF c = b == 0 ? RGB(255, 95, 87) : b == 1 ? RGB(254, 188, 46) : RGB(40, 200, 64);
            const HBRUSH brush = CreateSolidBrush(c);
            const HGDIOBJ oldBrush = SelectObject(dc, brush);
            const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            const RECT& r = tile.buttons[b];
            const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, rad = Px(kDot) / 2;
            Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(brush);
        }
        RECT caption = tile.title;
        caption.left += Px(8);
        const std::wstring title = TitleOf(tile.source, name_);
        DrawTextW(dc, title.c_str(), -1, &caption, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!tile.thumbnail) {
            RECT image = tile.image;
            DrawTextW(dc, docky::ui::Text(L"Unavailable"), -1, &image, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void WindowPreview::Paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window_, &ps);
    if (!surface_) {
        PaintGdi(dc);
        EndPaint(window_, &ps);
        return;
    }
    EndPaint(window_, &ps);
    if (!surface_->DrawCustom(kPanelRadius, [this](ID2D1DeviceContext* d2d, bool gpu) { DrawGlassContent(d2d, gpu); }))
        DLOG(L"window preview: glass render failed");
}

// Reveal: like the menu, coverage waits (up to 0.25 s) for the first captured backdrop so no fallback frame flashes,
// then fades in over ~140 ms (at once with Reduce motion).
void WindowPreview::StepAppear() {
    if (!surface_) { KillTimer(window_, kTimerAppear); return; }
    const ULONGLONG now = GetTickCount64();
    if (!surface_->GlassReady() && now - shownAt_ < 250) return;
    BOOL animations = TRUE;
    const bool reduced = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && !animations;
    appear_ = reduced ? 1.f : (std::min)(1.f, appear_ + 15.f / 140.f);
    surface_->Materialize(appear_, false);
    if (appear_ >= 1.f) KillTimer(window_, kTimerAppear);
}

LRESULT CALLBACK WindowPreview::WndProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<WindowPreview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<WindowPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self && self->window_ == window ? self->Handle(message, w, l) : DefWindowProcW(window, message, w, l);
}

LRESULT WindowPreview::Handle(UINT message, WPARAM w, LPARAM l) {
    const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case kMsgCaptureAvailable:
        if (surface_ && Visible()) {
            if (surface_->Capture()) InvalidateRect(window_, nullptr, FALSE);
            if (surface_->CompletionEvent()) SetTimer(window_, kTimerCapture, 8, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (w == kTimerAppear) StepAppear();
        else if (w == kTimerCapture && surface_) {
            const HANDLE event = surface_->CompletionEvent();
            if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0 && surface_->Complete()) InvalidateRect(window_, nullptr, FALSE);
            if (!surface_->CompletionEvent()) KillTimer(window_, kTimerCapture);
        } else if (w == kTimerHealth && surface_ && surface_->CheckHealth()) InvalidateRect(window_, nullptr, FALSE);
        return 0;
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
        else if (hit >= kButtonBase) WindowCommand((hit - kButtonBase) / 3, (hit - kButtonBase) % 3);
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
