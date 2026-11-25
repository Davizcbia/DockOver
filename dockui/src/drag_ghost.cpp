#include "drag_ghost.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "design_tokens.h"

using Microsoft::WRL::ComPtr;

namespace dock {
namespace {

constexpr wchar_t kClass[] = L"DockyDragGhost";
constexpr float kRemoveOpacity = 0.45f;  // the same dimming the Dock used for "letting go removes it"

ID2D1Factory* Factory() {
    static ComPtr<ID2D1Factory> factory;
    if (!factory) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    return factory.Get();
}
IDWriteFactory* Writer() {
    static ComPtr<IDWriteFactory> writer;
    if (!writer) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writer.GetAddressOf()));
    return writer.Get();
}

HWND CreateGhostWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClass, L"",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h) SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);  // never inside the Dock's own glass backdrop
    return h;
}

}  // namespace

DragGhost::~DragGhost() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void DragGhost::Hide() {
    if (hwnd_ && visible_) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

void DragGhost::Show(const std::vector<uint32_t>& pixels, UINT w, UINT h, uint64_t imageKey, POINT iconTopLeft, int sizePx,
                     bool remove, const std::wstring& label, float scale) {
    if (pixels.empty() || !w || !h || sizePx <= 0) { Hide(); return; }
    if (!hwnd_ && !(hwnd_ = CreateGhostWindow())) return;
    const bool redraw = imageKey != imageKey_ || sizePx != sizePx_ || remove != remove_ || scale != scale_ || label != label_;
    if (redraw) {
        if (!Render(pixels, w, h, sizePx, remove, label, scale)) { Hide(); return; }
        imageKey_ = imageKey; sizePx_ = sizePx; remove_ = remove; scale_ = scale; label_ = label;
    }
    const int x = iconTopLeft.x - iconOffset_.x, y = iconTopLeft.y - iconOffset_.y;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | (visible_ ? 0 : SWP_SHOWWINDOW));
    visible_ = true;
}

bool DragGhost::Render(const std::vector<uint32_t>& pixels, UINT w, UINT h, int sizePx, bool remove, const std::wstring& label,
                       float scale) {
    ID2D1Factory* factory = Factory();
    IDWriteFactory* writer = Writer();
    if (!factory || !writer) return false;

    // Label (Dock label style: 13 DIP text on a dark rounded plate), 8 DIP above the icon.
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    float labelW = 0, labelH = 0;
    const float pad = 10.f * scale, gap = 8.f * scale;
    if (!label.empty() &&
        SUCCEEDED(writer->CreateTextFormat(liquid::design::type::Family, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 13.f * scale, L"", &format)) &&
        SUCCEEDED(writer->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format.Get(), 4000.f, 200.f, &layout))) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        labelW = std::ceil(m.width) + 2 * pad;
        labelH = std::ceil(m.height) + 8.f * scale;
    }
    const int width = static_cast<int>(std::ceil((std::max)(static_cast<float>(sizePx), labelW))) + 4;
    const int labelBlock = labelH > 0 ? static_cast<int>(std::ceil(labelH + gap)) : 0;
    const int height = labelBlock + sizePx + 2;
    iconOffset_ = POINT{(width - sizePx) / 2, labelBlock};

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (mem && dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        ComPtr<ID2D1DCRenderTarget> rt;
        RECT bounds{0, 0, width, height};
        if (SUCCEEDED(factory->CreateDCRenderTarget(&props, &rt)) && SUCCEEDED(rt->BindDC(mem, &bounds))) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            ComPtr<ID2D1Bitmap> icon;
            const auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, &bp, &icon))) {
                const float x = static_cast<float>(iconOffset_.x), y = static_cast<float>(iconOffset_.y);
                rt->DrawBitmap(icon.Get(), D2D1::RectF(x, y, x + sizePx, y + sizePx), remove ? kRemoveOpacity : 1.f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            if (layout) {
                ComPtr<ID2D1SolidColorBrush> brush;
                rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.86f), &brush);
                const float left = (width - labelW) * 0.5f;
                const auto plate = D2D1::RectF(left, 0, left + labelW, labelH);
                rt->FillRoundedRectangle(D2D1::RoundedRect(plate, 6.f * scale, 6.f * scale), brush.Get());
                brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(plate.left + 0.5f, plate.top + 0.5f, plate.right - 0.5f, plate.bottom - 0.5f),
                                                           6.f * scale, 6.f * scale), brush.Get(), 1.f);
                brush->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                rt->DrawTextLayout(D2D1::Point2F(left + pad, 4.f * scale), layout.Get(), brush.Get());
            }
            ok = SUCCEEDED(rt->EndDraw());
        }
        if (ok) {
            POINT zero{};
            SIZE size{width, height};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            ok = UpdateLayeredWindow(hwnd_, screen, nullptr, &size, mem, &zero, 0, &blend, ULW_ALPHA) != FALSE;
            size_ = size;
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace dock
