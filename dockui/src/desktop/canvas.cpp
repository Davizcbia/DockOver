#include "canvas.h"
#include "../desktop_settings.h"
#include "../music_wallpaper_frame.h"
#include "../theme_state.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace docky::desk {
namespace {
D2D1_COLOR_F Rgb(unsigned rgb, float a = 1.f) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
bool SystemDark() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return true;
    return value == 0;
}
}  // namespace

Theme CurrentTheme() {
    const int mode = liquid::settings::Load(liquid::settings::DefaultPath()).value.appearance.mode;
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    const bool sys = SystemDark();
    const auto p = liquid::theme::Palette(mode, sys, contrast);
    Theme t;
    t.dark = liquid::theme::IsDark(mode, sys); t.contrast = contrast;
    t.ink = Rgb(p.primary); t.secondary = Rgb(p.secondary, contrast ? 1.f : 0.78f); t.accent = Rgb(p.accent); t.divider = Rgb(p.divider, 0.6f);
    // Glass tint over the blurred wallpaper (contrast: an opaque surface instead of glass).
    t.tint = contrast ? Rgb(p.surface) : t.dark ? D2D1::ColorF(0.09f, 0.09f, 0.11f, 0.46f) : D2D1::ColorF(1.f, 1.f, 1.f, 0.52f);
    if (!t.dark) t.secondary = Rgb(p.secondary, contrast ? 1.f : 0.9f);
    return t;
}

ID2D1Factory* D2D() { static ComPtr<ID2D1Factory> f; if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf()); return f.Get(); }
IDWriteFactory* DWrite() { static ComPtr<IDWriteFactory> f; if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.GetAddressOf())); return f.Get(); }
// Process-lifetime factory, deliberately never released: a static ComPtr would call Release() during static
// destruction, after OleUninitialize, when the WIC module may already be gone (measured: access violation at exit).
IWICImagingFactory* Wic() { static IWICImagingFactory* f = nullptr; if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)); return f; }

IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    static std::map<std::tuple<int, int, int, bool>, ComPtr<IDWriteTextFormat>> cache;
    const auto key = std::make_tuple(static_cast<int>(size * 10), static_cast<int>(weight), static_cast<int>(align), ellipsis);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (!DWrite() || FAILED(DWrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(DWrite()->CreateEllipsisTrimmingSign(f.Get(), &sign))) { DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; f->SetTrimming(&t, sign.Get()); }
    }
    cache[key] = f;
    return f.Get();
}

void Text(ID2D1RenderTarget* rt, const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F box, D2D1_COLOR_F color) {
    if (!rt || !format || text.empty()) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, &brush))) return;
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, box, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (text.empty() || !format || FAILED(DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.f, 200.f, &layout))) return 0;
    DWRITE_TEXT_METRICS m{}; layout->GetMetrics(&m); return m.widthIncludingTrailingWhitespace;
}

std::vector<D2D1_POINT_2F> WidgetOutline(D2D1_RECT_F r, float grow, int perCorner) {
    // The corner keeps its measured shape; an offset outline moves every point along the corner's normal, which for
    // a superellipse is close to growing the extent by the same amount.
    r = D2D1::RectF(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow);
    const float e = (std::max)(1.f, (std::min)({kCornerExtent + grow, (r.right - r.left) / 2, (r.bottom - r.top) / 2}));
    const float k = 2.f / kCornerExponent;
    std::vector<D2D1_POINT_2F> pts;
    pts.reserve(static_cast<size_t>(perCorner) * 4 + 4);
    // centre of each corner's quadrant and the signs that point it outwards: top-right, bottom-right, bottom-left, top-left
    const struct { float cx, cy, sx, sy; } corners[4] = {
        {r.right - e, r.top + e, 1, -1}, {r.right - e, r.bottom - e, 1, 1}, {r.left + e, r.bottom - e, -1, 1}, {r.left + e, r.top + e, -1, -1}};
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= perCorner; ++i) {
            // top-right runs from the top edge (angle 90) to the right edge (0); each next corner continues clockwise
            float t = (static_cast<float>(i) / perCorner) * 1.5707963f;
            if (c == 0 || c == 2) t = 1.5707963f - t;
            const float cs = std::pow(std::cos(t), k), sn = std::pow(std::sin(t), k);
            pts.push_back(D2D1::Point2F(corners[c].cx + corners[c].sx * e * cs, corners[c].cy + corners[c].sy * e * sn));
        }
    }
    return pts;
}

ComPtr<ID2D1Geometry> WidgetShape(D2D1_RECT_F r, ID2D1RenderTarget* rt, float grow) {
    ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(FactoryOf(rt)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    const auto pts = WidgetOutline(r, grow);
    sink->BeginFigure(pts.front(), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

ComPtr<ID2D1Factory> FactoryOf(ID2D1RenderTarget* rt) { ComPtr<ID2D1Factory> f; if (rt) rt->GetFactory(&f); if (!f) f = D2D(); return f; }
ComPtr<ID2D1Geometry> Rounded(D2D1_RECT_F r, float radius, ID2D1RenderTarget* rt) {
    ComPtr<ID2D1RoundedRectangleGeometry> g;
    FactoryOf(rt)->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &g);
    return g;
}

Canvas::~Canvas() { if (dc_) { SelectObject(dc_, old_); DeleteObject(bitmap_); DeleteDC(dc_); } }

ID2D1RenderTarget* Canvas::Begin(int w, int h, UINT dpi) {
    if (w <= 0 || h <= 0) return nullptr;
    if (w != w_ || h != h_ || !dc_) {
        if (dc_) { SelectObject(dc_, old_); DeleteObject(bitmap_); DeleteDC(dc_); dc_ = nullptr; }
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bitmap_) { DeleteDC(dc_); dc_ = nullptr; return nullptr; }
        old_ = SelectObject(dc_, bitmap_);
        w_ = w; h_ = h;
    }
    if (!rt_) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(D2D()->CreateDCRenderTarget(&props, &rt_))) return nullptr;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    RECT all{0, 0, w, h};
    if (FAILED(rt_->BindDC(dc_, &all))) return nullptr;
    rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return rt_.Get();
}

bool Canvas::End(HWND window, POINT screen) {
    if (!rt_ || !dc_) return false;
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) { rt_.Reset(); return false; }
    POINT src{0, 0}; SIZE size{w_, h_}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screenDc = GetDC(nullptr);
    const BOOL ok = UpdateLayeredWindow(window, screenDc, &screen, &size, dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDc);
    return ok != FALSE;
}

Wallpaper& Wallpaper::Get() { static Wallpaper w; return w; }
void Wallpaper::Invalidate() { built_ = false; monitors_.clear(); }

namespace {
// Decodes and scales an image file to exactly w x h (premultiplied BGRA).
bool Decode(const std::wstring& path, UINT w, UINT h, std::vector<std::uint32_t>& out) {
    if (!Wic() || !w || !h) return false;
    ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> conv;
    if (FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    if (FAILED(Wic()->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
    if (FAILED(Wic()->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    out.assign(static_cast<size_t>(w) * h, 0);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size() * 4), reinterpret_cast<BYTE*>(out.data())));
}
bool ImageSize(const std::wstring& path, UINT& w, UINT& h) {
    ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
    return Wic() && SUCCEEDED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
           SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h;
}
void BoxBlur(std::vector<std::uint32_t>& px, int w, int h, int r) {
    std::vector<std::uint32_t> tmp(px.size());
    auto pass = [&](const std::vector<std::uint32_t>& src, std::vector<std::uint32_t>& dst, bool horizontal) {
        const int lines = horizontal ? h : w, len = horizontal ? w : h;
        for (int l = 0; l < lines; ++l) {
            auto at = [&](int i) -> std::uint32_t { i = std::clamp(i, 0, len - 1); return horizontal ? src[static_cast<size_t>(l) * w + i] : src[static_cast<size_t>(i) * w + l]; };
            unsigned sum[4]{};
            for (int i = -r; i <= r; ++i) { const auto p = at(i); for (int c = 0; c < 4; ++c) sum[c] += (p >> (8 * c)) & 255; }
            const unsigned n = 2 * r + 1;
            for (int i = 0; i < len; ++i) {
                std::uint32_t v = 0; for (int c = 0; c < 4; ++c) v |= ((sum[c] + n / 2) / n) << (8 * c);
                (horizontal ? dst[static_cast<size_t>(l) * w + i] : dst[static_cast<size_t>(i) * w + l]) = v;
                const auto add = at(i + r + 1), sub = at(i - r);
                for (int c = 0; c < 4; ++c) sum[c] += ((add >> (8 * c)) & 255) - ((sub >> (8 * c)) & 255);
            }
        }
    };
    for (int k = 0; k < 3; ++k) { pass(px, tmp, true); pass(tmp, px, false); }
}
}  // namespace

std::wstring Wallpaper::Signature() {
    ComPtr<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) return {};
    DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL; dw->GetPosition(&position);
    COLORREF background = 0; dw->GetBackgroundColor(&background);
    std::wstring sig = std::to_wstring(static_cast<int>(position)) + L"|" + std::to_wstring(background);
    UINT count = 0; dw->GetMonitorDevicePathCount(&count);
    for (UINT i = 0; i < count && i < 8; ++i) {
        LPWSTR id = nullptr; if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        LPWSTR file = nullptr; dw->GetWallpaper(id, &file);
        CoTaskMemFree(id);
        const std::wstring path = file ? file : L""; if (file) CoTaskMemFree(file);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool have = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa);
        sig += L"|" + path + L"@" + (have ? std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(fa.ftLastWriteTime.dwHighDateTime) + L"." + std::to_wstring(fa.nFileSizeLow) : L"-");
    }
    return sig;
}

bool Wallpaper::Stale() {
    if (!built_) return false;  // built on the next paint anyway
    const std::wstring now = Signature();
    return !now.empty() && now != signature_;
}

bool Wallpaper::Build() {
    built_ = true; monitors_.clear();
    signature_ = Signature();
    ComPtr<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) return false;
    UINT count = 0; dw->GetMonitorDevicePathCount(&count);
    DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL; dw->GetPosition(&position);
    COLORREF background = RGB(30, 30, 34); dw->GetBackgroundColor(&background);
    for (UINT i = 0; i < count && i < 8; ++i) {
        LPWSTR id = nullptr; if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        RECT rect{}; LPWSTR file = nullptr;
        const bool ok = SUCCEEDED(dw->GetMonitorRECT(id, &rect)) && rect.right > rect.left;
        dw->GetWallpaper(id, &file);
        CoTaskMemFree(id);
        if (!ok) { if (file) CoTaskMemFree(file); continue; }
        Monitor m; m.rect = rect; m.w = (std::max)(1L, (rect.right - rect.left) / 4); m.h = (std::max)(1L, (rect.bottom - rect.top) / 4);
        const std::uint32_t bg = 0xFF000000u | (GetRValue(background) << 16) | (GetGValue(background) << 8) | GetBValue(background);
        m.pixels.assign(static_cast<size_t>(m.w) * m.h, bg);
        std::wstring path = file ? file : L""; if (file) CoTaskMemFree(file);
        UINT iw = 0, ih = 0;
        if (!path.empty() && ImageSize(path, iw, ih)) {
            double sx = static_cast<double>(m.w) / iw, sy = static_cast<double>(m.h) / ih, s = 1;
            int dw2 = m.w, dh2 = m.h;
            switch (position) {
            case DWPOS_FIT: s = (std::min)(sx, sy); dw2 = static_cast<int>(iw * s); dh2 = static_cast<int>(ih * s); break;
            case DWPOS_STRETCH: break;
            case DWPOS_CENTER: dw2 = static_cast<int>(iw / 4); dh2 = static_cast<int>(ih / 4); break;
            default: s = (std::max)(sx, sy); dw2 = static_cast<int>(std::ceil(iw * s)); dh2 = static_cast<int>(std::ceil(ih * s)); break;  // fill, tile, span
            }
            std::vector<std::uint32_t> img;
            if (dw2 > 0 && dh2 > 0 && dw2 < 8192 && dh2 < 8192 && Decode(path, static_cast<UINT>(dw2), static_cast<UINT>(dh2), img)) {
                const int ox = (m.w - dw2) / 2, oy = (m.h - dh2) / 2;
                for (int y = 0; y < m.h; ++y) for (int x = 0; x < m.w; ++x) {
                    const int sx2 = x - ox, sy2 = y - oy;
                    if (sx2 >= 0 && sy2 >= 0 && sx2 < dw2 && sy2 < dh2) m.pixels[static_cast<size_t>(y) * m.w + x] = img[static_cast<size_t>(sy2) * dw2 + sx2] | 0xFF000000u;
                }
            }
        }
        // Sharp copy at full resolution (liquid glass blurs and refracts it on the GPU itself).
        {
            const int fw = rect.right - rect.left, fh = rect.bottom - rect.top;
            m.sharp.assign(static_cast<size_t>(fw) * fh, bg);
            if (!path.empty() && iw && ih) {
                const double sx = static_cast<double>(fw) / iw, sy = static_cast<double>(fh) / ih;
                double s = 1; int dw3 = fw, dh3 = fh;
                switch (position) {
                case DWPOS_FIT: s = (std::min)(sx, sy); dw3 = static_cast<int>(iw * s); dh3 = static_cast<int>(ih * s); break;
                case DWPOS_STRETCH: break;
                case DWPOS_CENTER: dw3 = static_cast<int>(iw); dh3 = static_cast<int>(ih); break;
                default: s = (std::max)(sx, sy); dw3 = static_cast<int>(std::ceil(iw * s)); dh3 = static_cast<int>(std::ceil(ih * s)); break;
                }
                std::vector<std::uint32_t> img;
                if (dw3 > 0 && dh3 > 0 && dw3 <= 16384 && dh3 <= 16384 && Decode(path, static_cast<UINT>(dw3), static_cast<UINT>(dh3), img)) {
                    const int ox = (fw - dw3) / 2, oy = (fh - dh3) / 2;
                    for (int y = 0; y < fh; ++y) { const int sy2 = y - oy; if (sy2 < 0 || sy2 >= dh3) continue;
                        for (int x = 0; x < fw; ++x) { const int sx2 = x - ox; if (sx2 >= 0 && sx2 < dw3) m.sharp[static_cast<size_t>(y) * fw + x] = img[static_cast<size_t>(sy2) * dw3 + sx2] | 0xFF000000u; } }
                }
            }
        }
        BoxBlur(m.pixels, m.w, m.h, 7);
        monitors_.push_back(std::move(m));
    }
    return !monitors_.empty();
}

namespace {
struct MusicBackdrop {
    music_wallpaper::Channel source;
    RECT screen{};
    std::vector<std::uint32_t> sharp, blurred;

    void Composite(const RECT& dest, int width, int height, bool blur, std::vector<std::uint32_t>& out) const {
        const auto& pixels = blur ? blurred : sharp;
        RECT overlap{};
        if (pixels.empty() || !IntersectRect(&overlap, &dest, &screen)) return;
        constexpr int sw = music_wallpaper::Width, sh = music_wallpaper::Height;
        const double stepX = double(dest.right - dest.left) / width, stepY = double(dest.bottom - dest.top) / height;
        for (int y = 0; y < height; ++y) {
            const double sy = dest.top + (y + .5) * stepY;
            if (sy < overlap.top || sy >= overlap.bottom) continue;
            const double fy = std::clamp((sy - screen.top) * sh / (screen.bottom - screen.top) - .5, 0., sh - 1.);
            const int iy = static_cast<int>(fy), iy2 = (std::min)(iy + 1, sh - 1);
            const double ty = fy - iy;
            for (int x = 0; x < width; ++x) {
                const double sx = dest.left + (x + .5) * stepX;
                if (sx < overlap.left || sx >= overlap.right) continue;
                const double fx = std::clamp((sx - screen.left) * sw / (screen.right - screen.left) - .5, 0., sw - 1.);
                const int ix = static_cast<int>(fx), ix2 = (std::min)(ix + 1, sw - 1);
                const double tx = fx - ix;
                const auto p00 = pixels[static_cast<size_t>(iy) * sw + ix], p10 = pixels[static_cast<size_t>(iy) * sw + ix2];
                const auto p01 = pixels[static_cast<size_t>(iy2) * sw + ix], p11 = pixels[static_cast<size_t>(iy2) * sw + ix2];
                auto channel = [&](unsigned shift) {
                    const double top = (p00 >> shift & 255u) * (1 - tx) + (p10 >> shift & 255u) * tx;
                    const double bottom = (p01 >> shift & 255u) * (1 - tx) + (p11 >> shift & 255u) * tx;
                    return static_cast<unsigned>(std::lround(top * (1 - ty) + bottom * ty));
                };
                const unsigned inv = 255u - channel(24);
                if (inv == 255) continue;
                auto& bg = out[static_cast<size_t>(y) * width + x];
                std::uint32_t merged = 0xFF000000u;
                for (unsigned shift : {0u, 8u, 16u})
                    merged |= (channel(shift) + ((bg >> shift & 255u) * inv + 127u) / 255u) << shift;
                bg = merged;
            }
        }
    }
};
MusicBackdrop& Music() { static MusicBackdrop backdrop; return backdrop; }
}

bool Wallpaper::RefreshMusic() {
    auto& music = Music();
    if (!music.source.Read(music.screen, music.sharp)) return false;
    music.blurred = music.sharp;
    if (!music.blurred.empty()) BoxBlur(music.blurred, music_wallpaper::Width, music_wallpaper::Height, 4);
    return true;
}

bool Wallpaper::Sample(const RECT& screen, int outW, int outH, std::vector<std::uint32_t>& out) {
    if (!built_) Build();
    if (outW <= 0 || outH <= 0) return false;
    const POINT c{(screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2};
    const Monitor* m = nullptr;
    for (const auto& mon : monitors_) if (PtInRect(&mon.rect, c)) { m = &mon; break; }
    if (!m && !monitors_.empty()) m = &monitors_[0];
    out.assign(static_cast<size_t>(outW) * outH, 0xFF1E1E22u);
    if (!m) { Music().Composite(screen, outW, outH, true, out); return true; }
    const double fx = (screen.right - screen.left) / 4.0 / outW, fy = (screen.bottom - screen.top) / 4.0 / outH;
    const double x0 = (screen.left - m->rect.left) / 4.0, y0 = (screen.top - m->rect.top) / 4.0;
    for (int y = 0; y < outH; ++y) {
        const double sy = std::clamp(y0 + (y + 0.5) * fy - 0.5, 0.0, m->h - 1.0);
        const int iy = static_cast<int>(sy), iy2 = (std::min)(iy + 1, m->h - 1); const double ty = sy - iy;
        for (int x = 0; x < outW; ++x) {
            const double sx = std::clamp(x0 + (x + 0.5) * fx - 0.5, 0.0, m->w - 1.0);
            const int ix = static_cast<int>(sx), ix2 = (std::min)(ix + 1, m->w - 1); const double tx = sx - ix;
            const auto p00 = m->pixels[static_cast<size_t>(iy) * m->w + ix], p10 = m->pixels[static_cast<size_t>(iy) * m->w + ix2];
            const auto p01 = m->pixels[static_cast<size_t>(iy2) * m->w + ix], p11 = m->pixels[static_cast<size_t>(iy2) * m->w + ix2];
            std::uint32_t v = 0xFF000000u;
            for (int ch = 0; ch < 3; ++ch) {
                const auto g = [&](std::uint32_t p) { return static_cast<double>((p >> (8 * ch)) & 255); };
                const double top = g(p00) * (1 - tx) + g(p10) * tx, bottom = g(p01) * (1 - tx) + g(p11) * tx;
                v |= static_cast<std::uint32_t>(std::lround(top * (1 - ty) + bottom * ty)) << (8 * ch);
            }
            out[static_cast<size_t>(y) * outW + x] = v;
        }
    }
    Music().Composite(screen, outW, outH, true, out);
    return true;
}

bool Wallpaper::SampleSharp(const RECT& screen, std::vector<std::uint32_t>& out) {
    if (!built_) Build();
    const int w = screen.right - screen.left, h = screen.bottom - screen.top;
    if (w <= 0 || h <= 0) return false;
    out.assign(static_cast<size_t>(w) * h, 0xFF1E1E22u);
    for (const auto& m : monitors_) {
        RECT part{};
        if (!IntersectRect(&part, &screen, &m.rect) || m.sharp.empty()) continue;
        const int mw = m.rect.right - m.rect.left;
        for (LONG y = part.top; y < part.bottom; ++y)
            memcpy(&out[static_cast<size_t>(y - screen.top) * w + (part.left - screen.left)], &m.sharp[static_cast<size_t>(y - m.rect.top) * mw + (part.left - m.rect.left)], static_cast<size_t>(part.right - part.left) * 4);
    }
    Music().Composite(screen, w, h, false, out);
    return true;
}

namespace {
std::vector<std::pair<HWND, const Canvas*>>& Layers() { static std::vector<std::pair<HWND, const Canvas*>> l; return l; }
}
void RegisterLayer(HWND window, const Canvas* canvas) { UnregisterLayer(window); Layers().push_back({window, canvas}); }
void UnregisterLayer(HWND window) { auto& l = Layers(); l.erase(std::remove_if(l.begin(), l.end(), [&](const auto& e) { return e.first == window; }), l.end()); }
bool ComposeBackdrop(const RECT& screen, std::vector<std::uint32_t>& out) {
    if (!Wallpaper::Get().SampleSharp(screen, out)) return false;
    const int w = screen.right - screen.left;
    for (const auto& [window, canvas] : Layers()) {
        RECT r{}, part{};
        if (!canvas || !IsWindow(window) || !IsWindowVisible(window) || !GetWindowRect(window, &r) || !canvas->Pixels()) continue;
        if (r.right - r.left != canvas->Width() || r.bottom - r.top != canvas->Height() || !IntersectRect(&part, &r, &screen)) continue;
        const std::uint32_t* src = canvas->Pixels();
        for (LONG y = part.top; y < part.bottom; ++y)
            for (LONG x = part.left; x < part.right; ++x) {
                const std::uint32_t p = src[static_cast<size_t>(y - r.top) * canvas->Width() + (x - r.left)];
                const unsigned a = p >> 24;
                if (!a) continue;
                std::uint32_t& d = out[static_cast<size_t>(y - screen.top) * w + (x - screen.left)];
                if (a == 255) { d = p; continue; }
                std::uint32_t v = 0xFF000000u;
                for (int c = 0; c < 3; ++c) v |= ((((p >> (8 * c)) & 255) + (((d >> (8 * c)) & 255) * (255 - a) + 127) / 255) & 255) << (8 * c);
                d = v;
            }
    }
    return true;
}

Prefs& CurrentPrefs() { static Prefs p; return p; }

// ---- Liquid glass (widget settings: Widgets & Stacks > Edge highlight, Batteries > Liquid Glass) -----------------
// The rim follows the Dock shader's fixed reflection (shaders/glass.hlsl): bright hairlines on the top and bottom edges,
// a soft glow above the bottom one, a faint line along the sides, and a dark hairline just inside the edge. Each term
// weights a line drawn segment by segment along the outline by the outline's normal there.
void PaintGlassRim(ID2D1RenderTarget* rt, const std::vector<D2D1_POINT_2F>& outline, bool light, float scale) {
    const size_t n = outline.size();
    if (!rt || n < 3) return;
    ComPtr<ID2D1SolidColorBrush> b;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0), &b))) return;
    const float glare = std::clamp(CurrentPrefs().glassRim, 0.f, 1.5f) * scale;
    const float room = light ? 0.35f : 1.f;  // highlights get less room on a bright surface (the shader's "room")
    struct Seg { D2D1_POINT_2F a, b; float nx, ny; };
    std::vector<Seg> segs; segs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& a = outline[i]; const auto& c = outline[(i + 1) % n];
        const float len = std::hypot(c.x - a.x, c.y - a.y);
        if (len < 1e-3f) continue;
        segs.push_back({a, c, (c.y - a.y) / len, -(c.x - a.x) / len});  // outward normal of a clockwise y-down polygon
    }
    auto inset = [](const Seg& s, float d) { return std::make_pair(D2D1::Point2F(s.a.x - s.nx * d, s.a.y - s.ny * d), D2D1::Point2F(s.b.x - s.nx * d, s.b.y - s.ny * d)); };
    if (!light && glare > 0.f)  // the dark hairline just inside the edge (it fades at top and bottom, where the light is)
        for (const auto& s : segs) {
            const float k = 0.24f * (1.f - s.ny * s.ny) * (std::min)(1.f, glare / 0.6f);
            if (k <= 0.004f) continue;
            const auto [a, c] = inset(s, 1.7f);
            b->SetColor(D2D1::ColorF(0, 0, 0, k)); rt->DrawLine(a, c, b.Get(), 0.8f);
        }
    for (const auto& s : segs) {  // glows inside the rim: above the bottom edge, and a little under the top one
        const float down = (std::max)(s.ny, 0.f), up = (std::max)(-s.ny, 0.f);
        const float k = (0.16f * down * down + 0.07f * up * up) * glare * room;
        if (k <= 0.004f) continue;
        const auto [a, c] = inset(s, 1.8f);
        b->SetColor(D2D1::ColorF(1, 1, 1, std::clamp(k, 0.f, 1.f))); rt->DrawLine(a, c, b.Get(), 2.6f);
    }
    for (const auto& s : segs) {  // the rim line on the outline: top and bottom bright, a faint line on the sides
        const float k = (0.62f * glare * s.ny * s.ny + 0.07f * (std::min)(1.f, scale)) * room;
        b->SetColor(D2D1::ColorF(1, 1, 1, std::clamp(k, 0.f, 1.f))); rt->DrawLine(s.a, s.b, b.Get(), 1.f);
    }
    if (light) for (const auto& s : segs) { b->SetColor(D2D1::ColorF(0, 0, 0, 0.08f)); rt->DrawLine(s.a, s.b, b.Get(), 1.f); }
}

// A whole widget plate of liquid glass (Batteries, when "Liquid Glass background" is on): the wallpaper under it, sharp
// and frosted by Frost; within a bevel along the edge it is seen through a lens that pulls in what lies just outside
// the plate (stronger toward the edge, by Refraction), as the Dock's glass does; a dark body tone (Tint) keeps white
// content legible; then the rim.
Theme PaintLiquidPlate(ID2D1RenderTarget* rt, D2D1_RECT_F shape, const RECT& screen, const Theme& theme) {
    const Prefs& pr = CurrentPrefs();
    ComPtr<ID2D1SolidColorBrush> brush; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.03f), &brush);
    if (!brush) return theme;
    for (int i = 6; i >= 1; --i)  // soft shadow
        if (auto layer = WidgetShape(D2D1::RectF(shape.left, shape.top + 2.5f, shape.right, shape.bottom + 2.5f), rt, i * 1.8f)) rt->FillGeometry(layer.Get(), brush.Get());
    const float w = shape.right - shape.left, h = shape.bottom - shape.top, margin = 18.f;
    const float sx = (screen.right - screen.left) / (std::max)(1.f, w), sy = (screen.bottom - screen.top) / (std::max)(1.f, h);
    RECT big{screen.left - static_cast<LONG>(margin * sx), screen.top - static_cast<LONG>(margin * sy), screen.right + static_cast<LONG>(margin * sx), screen.bottom + static_cast<LONG>(margin * sy)};
    float pad = margin;
    std::vector<std::uint32_t> blurred, sharp;
    auto half = [](LONG v) { return (std::max)(2L, v / 2); };
    bool haveBlur = Wallpaper::Get().Sample(big, half(big.right - big.left), half(big.bottom - big.top), blurred);
    if (!haveBlur) { big = screen; pad = 0.f; haveBlur = Wallpaper::Get().Sample(big, half(big.right - big.left), half(big.bottom - big.top), blurred); }
    const float frost = std::clamp(pr.glassFrost, 0.f, 1.f), refraction = std::clamp(pr.glassRefraction, 0.f, 1.f);
    const bool haveSharp = frost < 0.99f && big.right - big.left <= 2048 && big.bottom - big.top <= 2048 && Wallpaper::Get().SampleSharp(big, sharp);
    ComPtr<ID2D1BitmapBrush> blurBrush, sharpBrush;
    D2D1_MATRIX_3X2_F blurBase = D2D1::Matrix3x2F::Identity(), sharpBase = D2D1::Matrix3x2F::Identity();
    auto makeBrush = [&](std::vector<std::uint32_t>& px, LONG pw, LONG ph, ComPtr<ID2D1BitmapBrush>& out, D2D1_MATRIX_3X2_F& base) {
        auto bmp = MakeBitmap(rt, px.data(), static_cast<UINT>(pw), static_cast<UINT>(ph)); if (!bmp) return;
        rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &out);
        const D2D1_SIZE_F bs = bmp->GetSize();
        base = D2D1::Matrix3x2F::Scale((w + 2 * pad) / bs.width, (h + 2 * pad) / bs.height) * D2D1::Matrix3x2F::Translation(shape.left - pad, shape.top - pad);
    };
    if (haveBlur) makeBrush(blurred, half(big.right - big.left), half(big.bottom - big.top), blurBrush, blurBase);
    if (haveSharp) makeBrush(sharp, big.right - big.left, big.bottom - big.top, sharpBrush, sharpBase);
    if (sharpBrush) sharpBrush->SetOpacity(1.f - frost);
    const D2D1_POINT_2F centre = D2D1::Point2F((shape.left + shape.right) / 2, (shape.top + shape.bottom) / 2);
    constexpr int kLayers = 10; constexpr float kBevel = 18.f;
    for (int k = 0; k <= kLayers; ++k) {  // outermost band first; each inner band covers the previous one
        auto geometry = WidgetShape(shape, rt, -kBevel * k / kLayers);
        if (!geometry) continue;
        const float edge = 1.f - static_cast<float>(k) / kLayers, m = 1.f - 0.14f * refraction * edge * edge;
        const auto lens = D2D1::Matrix3x2F::Scale(m, m, centre);
        if (blurBrush) { blurBrush->SetTransform(blurBase * lens); rt->FillGeometry(geometry.Get(), blurBrush.Get()); }
        if (sharpBrush) { sharpBrush->SetTransform(sharpBase * lens); rt->FillGeometry(geometry.Get(), sharpBrush.Get()); }
    }
    auto outline = WidgetShape(shape, rt);
    if (!outline) return theme;
    if (!blurBrush) { brush->SetColor(D2D1::ColorF(0.2f, 0.2f, 0.22f, 0.9f)); rt->FillGeometry(outline.Get(), brush.Get()); }
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.06f + 0.50f * std::clamp(pr.glassTint, 0.f, 1.f)));  // body tone
    rt->FillGeometry(outline.Get(), brush.Get());
    {   // a faint sheen over the top of the glass
        ComPtr<ID2D1GradientStopCollection> stops; ComPtr<ID2D1LinearGradientBrush> sheen;
        const D2D1_GRADIENT_STOP s[2] = {{0.f, D2D1::ColorF(1, 1, 1, 0.07f)}, {1.f, D2D1::ColorF(1, 1, 1, 0.f)}};
        if (SUCCEEDED(rt->CreateGradientStopCollection(s, 2, &stops)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, shape.top), D2D1::Point2F(0, shape.top + h * 0.45f)), stops.Get(), &sheen)))
            rt->FillGeometry(outline.Get(), sheen.Get());
    }
    PaintGlassRim(rt, WidgetOutline(shape, -0.5f, 28), false, 1.25f);
    Theme t = theme;
    t.dark = true; t.ink = D2D1::ColorF(1, 1, 1, 1); t.secondary = D2D1::ColorF(1, 1, 1, 0.62f);
    return t;
}

Theme PaintPlate(ID2D1RenderTarget* rt, D2D1_RECT_F shape, const RECT& screen, const Theme& theme, PlateTone tone) {
    if (tone == PlateTone::Glass && !theme.contrast) return PaintLiquidPlate(rt, shape, screen, theme);
    if (CurrentPrefs().style == 1 || theme.contrast) { PaintGlass(rt, shape, kRadius, screen, theme, 0.6f + 0.4f * CurrentPrefs().glass); return theme; }
    const bool lightPlate = tone == PlateTone::Light, blackPlate = tone == PlateTone::Black;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.05f), &brush);
    for (int i = 5; i >= 1; --i) {  // soft shadow
        const float g = i * 1.6f;
        if (auto layer = WidgetShape(D2D1::RectF(shape.left, shape.top + 2.f, shape.right, shape.bottom + 2.f), rt, g)) rt->FillGeometry(layer.Get(), brush.Get());
    }
    const bool light = !blackPlate && (lightPlate || !theme.dark);
    Theme t = theme;
    if (light) {
        t.dark = false; t.ink = D2D1::ColorF(0.08f, 0.08f, 0.09f, 1); t.secondary = D2D1::ColorF(0.24f, 0.24f, 0.26f, 0.6f);
        brush->SetColor(lightPlate ? D2D1::ColorF(0.985f, 0.985f, 0.99f, 1) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.97f));
    } else {
        t.dark = true; t.ink = D2D1::ColorF(1, 1, 1, blackPlate ? 1.f : 0.96f); t.secondary = D2D1::ColorF(0.92f, 0.92f, 0.96f, 0.55f);
        brush->SetColor(blackPlate ? D2D1::ColorF(0.015f, 0.015f, 0.018f, 1) : D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.94f));
    }
    auto outline = WidgetShape(shape, rt);
    if (!outline) return t;
    rt->FillGeometry(outline.Get(), brush.Get());
    PaintGlassRim(rt, WidgetOutline(shape, -0.5f, 28), light);  // the liquid glass rim (hairline on a light plate)
    return t;
}

bool CaptureScreen(const RECT& r, std::vector<std::uint32_t>& out) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        if (ok) {
            const auto* px = static_cast<const std::uint32_t*>(bits);
            out.assign(px, px + static_cast<size_t>(w) * h);
            for (auto& p : out) p |= 0xFF000000u;
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1RenderTarget* rt, const std::uint32_t* pixels, UINT w, UINT h) {
    ComPtr<ID2D1Bitmap> b;
    if (!rt || !pixels || !w || !h) return b;
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    rt->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4, props, &b);
    return b;
}

void PaintGlass(ID2D1RenderTarget* rt, D2D1_RECT_F shape, float radius, const RECT& screen, const Theme& theme, float strength) {
    // Soft shadow: stacked translucent rounded rectangles grown outwards and shifted down (no blur effect needed
    // on a DC render target). The opaque body covers their interior.
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.026f * strength), &brush);
    for (int i = 6; i >= 1; --i) {
        const float g = i * 1.8f;
        D2D1_RECT_F r{shape.left - g, shape.top - g + 2.5f, shape.right + g, shape.bottom + g + 2.5f};
        if (auto layer = WidgetShape(D2D1::RectF(shape.left, shape.top + 2.5f, shape.right, shape.bottom + 2.5f), rt, g)) rt->FillGeometry(layer.Get(), brush.Get());
        (void)r;
    }
    (void)radius;  // widget glass uses the measured widget outline
    auto shapeGeometry = WidgetShape(shape, rt);
    const float w = shape.right - shape.left, h = shape.bottom - shape.top;
    std::vector<std::uint32_t> px;
    const int sw = (std::max)(1, static_cast<int>(w / 2)), sh = (std::max)(1, static_cast<int>(h / 2));
    if (!theme.contrast && Wallpaper::Get().Sample(screen, sw, sh, px)) {
        if (auto bmp = MakeBitmap(rt, px.data(), static_cast<UINT>(sw), static_cast<UINT>(sh))) {
            ComPtr<ID2D1BitmapBrush> bb;
            if (SUCCEEDED(rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &bb))) {
                // The bitmap is in DIPs of its own (one texel per 2 DIP); map it onto the shape.
                const D2D1_SIZE_F bs = bmp->GetSize();
                bb->SetTransform(D2D1::Matrix3x2F::Scale(w / bs.width, h / bs.height) * D2D1::Matrix3x2F::Translation(shape.left, shape.top));
                rt->FillGeometry(shapeGeometry.Get(), bb.Get());
            }
        }
    }
    brush->SetColor(theme.tint);
    rt->FillGeometry(shapeGeometry.Get(), brush.Get());
    // The liquid glass rim (Glass Studio "Desktop widgets"); glass over the wallpaper is never a "light surface".
    PaintGlassRim(rt, WidgetOutline(shape, -0.5f, 28), false, theme.dark ? 1.f : 1.4f);
    if (theme.contrast) { brush->SetColor(theme.ink); rt->DrawGeometry(shapeGeometry.Get(), brush.Get(), 1.f); }
}

bool ShellImage(const std::wstring& path, int px, bool thumbnail, std::vector<std::uint32_t>& out, UINT& w, UINT& h) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP bitmap = nullptr;
    const SIIGBF flags = thumbnail ? (SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK) : (SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
    if (FAILED(factory->GetImage(SIZE{px, px}, flags, &bitmap)) || !bitmap) return false;
    BITMAP bm{}; GetObjectW(bitmap, sizeof(bm), &bm);
    const int bw = bm.bmWidth, bh = std::abs(bm.bmHeight);
    bool ok = false;
    if (bw > 0 && bh > 0 && bw <= 2048 && bh <= 2048) {
        out.assign(static_cast<size_t>(bw) * bh, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = bw; bi.bmiHeader.biHeight = -bh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bitmap, 0, static_cast<UINT>(bh), out.data(), &bi, DIB_RGB_COLORS) == bh;
        ReleaseDC(nullptr, dc);
        if (ok) {
            bool anyAlpha = false, straight = false;
            for (auto p : out) { const unsigned a = p >> 24; if (a) anyAlpha = true; if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) straight = true; }
            for (auto& p : out) {
                if (!anyAlpha) { p |= 0xFF000000u; continue; }
                if (straight) { const unsigned a = p >> 24; p = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255); }
            }
            w = static_cast<UINT>(bw); h = static_cast<UINT>(bh);
        }
    }
    DeleteObject(bitmap);
    return ok;
}

bool LoadImageFile(const std::wstring& path, int maxPx, std::vector<std::uint32_t>& out, UINT& w, UINT& h) {
    UINT iw = 0, ih = 0;
    if (!ImageSize(path, iw, ih)) return false;
    const double s = (std::min)(1.0, static_cast<double>(maxPx) / (std::max)(iw, ih));
    w = (std::max)(1u, static_cast<UINT>(iw * s)); h = (std::max)(1u, static_cast<UINT>(ih * s));
    return Decode(path, w, h, out);
}

std::wstring& DevPngDir() { static std::wstring dir; return dir; }

bool Canvas::SavePng(const std::wstring& path) const {
    ComPtr<IWICBitmap> bmp; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
    if (!Wic() || !bits_ || FAILED(Wic()->CreateBitmapFromMemory(w_, h_, GUID_WICPixelFormat32bppPBGRA, w_ * 4, w_ * h_ * 4, static_cast<BYTE*>(bits_), &bmp))) return false;
    if (FAILED(Wic()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    return SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

std::wstring DataDir() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::wstring(local) + L"\\DockyDesktop";
}

}  // namespace docky::desk
