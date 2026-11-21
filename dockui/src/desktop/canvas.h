#pragma once
// docky Desktop drawing layer: layered-window canvas (Direct2D into a premultiplied DIB), theme, and the
// widget glass. Widgets and stacks sit on the desktop, directly over the wallpaper, so their glass samples a
// blurred copy of the wallpaper itself (no screen capture, no capture border, nothing behind other windows).
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace docky::desk {
using Microsoft::WRL::ComPtr;

// Shared metrics (DIP), measured from the user's macOS 26 desktop (2026-11-11): small 164, gap 18, margin 16, so a
// medium/large widget (2 x 164 + 18 = 346) lines up with two small ones. Gap, margin and snap distance are user
// preferences (Prefs); these are the defaults.
inline constexpr float kSmall = 164.f, kWide = 346.f, kTall = 346.f, kRadius = 22.f;
// Widget corner (macOS 26/27 desktop widgets), measured 2026-11-11 from the user's @2x screenshot of the digital
// Clock plate (328 px = 164 pt): not a circular arc but a superellipse quadrant |x|^n + |y|^n = 1 that leaves the
// straight edges 39.4 pt from each corner, n = 3.05 (fit rms 0.34 px; a 22 pt circle was off by 5.3 px rms).
// kRadius remains for small UI parts (buttons, hover plates) that are not widget outlines.
inline constexpr float kCornerExtent = 39.4f, kCornerExponent = 3.05f;
inline bool LocaleFahrenheit() {
    DWORD m = 0;  // LOCALE_IMEASURE: 0 metric, 1 US
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&m), sizeof(m) / sizeof(wchar_t)) && m == 1;
}
struct Prefs {
    int style = 0;          // 0 solid plates (macOS full-colour), 1 glass over the wallpaper
    float gap = 18.f;       // space between snapped widgets
    float margin = 16.f;    // distance from the work-area edges
    float snap = 12.f;      // magnet distance while dragging (0: off)
    float glass = 0.6f;     // glass style only: tint strength 0..1
    bool menuBattery = true;               // accessory batteries as Menu Bar items (when docky's Menu Bar runs)
    bool fahrenheit = LocaleFahrenheit();  // weather units; default from the Windows measurement system
    bool clockDark = false;                // digital Clock: white digits on a black plate (else black on white)
    bool batteryGlass = true;              // Batteries: the whole plate is liquid glass over the wallpaper
    // Liquid glass, tuned in the widget settings (Widgets & Stacks: edge highlight; Batteries: the glass plate)
    float glassRim = 0.6f;                 // rim highlight on every widget edge (the Dock's fixed reflection), 0..1.5
    float glassRefraction = 0.6f;          // lens along the edge of a glass plate, 0..1
    float glassFrost = 0.3f;               // 0 sharp wallpaper .. 1 fully blurred
    float glassTint = 0.35f;               // dark body tone, 0..1
    std::vector<std::wstring> batteryWidgetOff, batteryMenuOff;  // devices unticked in Settings > Batteries (by name)
};
inline bool NameListed(const std::vector<std::wstring>& list, const std::wstring& name) {
    for (const auto& n : list) if (n == name) return true;
    return false;
}
Prefs& CurrentPrefs();
#define kGap (::docky::desk::CurrentPrefs().gap)
#define kMargin (::docky::desk::CurrentPrefs().margin)
inline constexpr float kShadow = 14.f;  // window padding around a widget for its soft shadow

struct Theme {
    bool dark = true, contrast = false;
    D2D1_COLOR_F ink{}, secondary{}, tint{}, accent{}, divider{};
};
Theme CurrentTheme();  // docky's appearance setting when present (Settings > Appearance), else the system theme

ID2D1Factory* D2D();
IDWriteFactory* DWrite();
IWICImagingFactory* Wic();
// Cached text format: size in DIP, weight, alignment.
IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool ellipsis = true);
void Text(ID2D1RenderTarget* rt, const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F box, D2D1_COLOR_F color);
float TextWidth(const std::wstring& text, IDWriteTextFormat* format);
// Geometry and stroke styles must come from the render target's own factory (the glass panels draw with the
// renderer's factory, layered windows with D2D()); mixing factories fails the whole frame.
ComPtr<ID2D1Factory> FactoryOf(ID2D1RenderTarget* rt);
ComPtr<ID2D1Geometry> Rounded(D2D1_RECT_F r, float radius, ID2D1RenderTarget* rt = nullptr);
// The widget outline (superellipse corners above), clockwise from the top edge; `grow` > 0 offsets it outwards
// (shadow layers), < 0 inwards. Points are dense enough to be drawn as a polygon at any widget size.
std::vector<D2D1_POINT_2F> WidgetOutline(D2D1_RECT_F r, float grow = 0.f, int perCorner = 28);
ComPtr<ID2D1Geometry> WidgetShape(D2D1_RECT_F r, ID2D1RenderTarget* rt, float grow = 0.f);

// One layered window's pixels. Begin() returns a render target in DIPs at the given DPI.
class Canvas {
public:
    ~Canvas();
    ID2D1RenderTarget* Begin(int widthPx, int heightPx, UINT dpi);
    // Ends drawing and shows the pixels at the window's screen position.
    bool End(HWND window, POINT screen);
    const std::uint32_t* Pixels() const { return static_cast<const std::uint32_t*>(bits_); }
    // Development only (--dev-png): this window's own pixels as a PNG; refuses to overwrite nothing else.
    bool SavePng(const std::wstring& path) const;
    int Width() const { return w_; }
    int Height() const { return h_; }
private:
    HDC dc_ = nullptr; HBITMAP bitmap_ = nullptr; HGDIOBJ old_ = nullptr; void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    ComPtr<ID2D1DCRenderTarget> rt_;
};

// Blurred wallpaper of each monitor (downsampled 4x, three box passes). Sampled for any screen rectangle.
class Wallpaper {
public:
    static Wallpaper& Get();
    void Invalidate();
    // True when the desktop wallpaper differs from the one last built (another file, the same file rewritten, a new
    // position or background colour). Windows does not always broadcast WM_SETTINGCHANGE for a new wallpaper (Settings
    // app, slideshow, third-party changers), so the glass kept showing the old one; Desktop checks this every 2 s.
    bool Stale();
    bool RefreshMusic();
    // Premultiplied BGRA crop of the blurred wallpaper covering `screen` (physical px), at outW x outH.
    bool Sample(const RECT& screen, int outW, int outH, std::vector<std::uint32_t>& out);
    // Full-resolution (unblurred) wallpaper pixels for `screen`, opaque BGRA, one texel per physical pixel.
    bool SampleSharp(const RECT& screen, std::vector<std::uint32_t>& out);
private:
    struct Monitor { RECT rect{}; int w = 0, h = 0; std::vector<std::uint32_t> pixels, sharp; };
    bool Build();
    static std::wstring Signature();
    bool built_ = false;
    std::wstring signature_;
    std::vector<Monitor> monitors_;
};

// Widget glass: blurred wallpaper + theme tint + 1 DIP specular rim + soft shadow. `screen` is where the shape is.
void PaintGlass(ID2D1RenderTarget* rt, D2D1_RECT_F shape, float radius, const RECT& screen, const Theme& theme, float strength = 1.f);

// Widget plate for the current style. Solid: an opaque rounded plate (light plate for `lightPlate` kinds such as the
// digital clock, a black one for `darkPlate`, else the theme's dark or light surface) with the liquid glass rim and a
// soft shadow. Returns the theme the content should use on that plate.
enum class PlateTone { Theme, Light, Black, Glass };
Theme PaintPlate(ID2D1RenderTarget* rt, D2D1_RECT_F shape, const RECT& screen, const Theme& theme, PlateTone tone);

// The liquid glass rim (Prefs::glassRim; the Dock shader's fixed reflection): `outline` is a closed clockwise polygon.
// On a light surface the highlight is mostly invisible, so a faint dark hairline keeps the edge.
void PaintGlassRim(ID2D1RenderTarget* rt, const std::vector<D2D1_POINT_2F>& outline, bool lightSurface, float scale = 1.f);
// A whole plate of liquid glass over the wallpaper (PlateTone::Glass): lens along the edge, frost, dark tint, rim.
Theme PaintLiquidPlate(ID2D1RenderTarget* rt, D2D1_RECT_F shape, const RECT& screen, const Theme& theme);

// Premultiplied BGRA -> D2D bitmap for this render target.
ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1RenderTarget* rt, const std::uint32_t* pixels, UINT w, UINT h);
// File / folder icon or thumbnail (IShellItemImageFactory), premultiplied BGRA. Call from a COM thread.
bool ShellImage(const std::wstring& path, int px, bool thumbnail, std::vector<std::uint32_t>& out, UINT& w, UINT& h);
// Image file (PNG/JPG/...) decoded to premultiplied BGRA, scaled to fit maxPx.
bool LoadImageFile(const std::wstring& path, int maxPx, std::vector<std::uint32_t>& out, UINT& w, UINT& h);

// What lies under a screen rectangle as far as docky Desktop knows: the wallpaper plus its own layered windows
// (widgets, the Stacks column). Feeds liquid glass without screen capture. Straight BGRA, alpha 255.
void RegisterLayer(HWND window, const Canvas* canvas);
void UnregisterLayer(HWND window);
bool ComposeBackdrop(const RECT& screen, std::vector<std::uint32_t>& out);
// What is on screen under `screen` right now (GDI BitBlt of the desktop, layered windows included). Windows marked
// WDA_EXCLUDEFROMCAPTURE are left out, so a top-most panel that excludes itself gets exactly the windows behind it,
// without Windows Graphics Capture (no capture border, no package identity). Straight BGRA, alpha 255.
bool CaptureScreen(const RECT& screen, std::vector<std::uint32_t>& out);

std::wstring DataDir();  // %LOCALAPPDATA%\DockyDesktop
std::wstring& DevPngDir();  // set by --dev-png; empty in normal runs
}  // namespace docky::desk
