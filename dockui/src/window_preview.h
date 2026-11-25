#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace liquid::popup { class MenuSurface; }

namespace dock {

// One non-activating destination for the shell compositor's live thumbnails, on the right-click menu's liquid glass
// (Glass Studio "Menu" category). Header: the app's icon and name. One card per window: close / minimize / zoom
// buttons, the window title and its live thumbnail. Corners are concentric: panel = card + padding, card = inner
// capsules / thumbnail well + inset. Without a GPU surface it paints the same layout with GDI.
class WindowPreview {
public:
    WindowPreview();
    ~WindowPreview();
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    // iconPixels: premultiplied BGRA (the Dock tile's icon), may be empty.
    bool Show(HWND owner, UINT activateMessage, RECT anchor, UINT dpi, bool dark,
              const std::wstring& appName, std::vector<HWND> sources,
              const std::vector<std::uint32_t>& iconPixels = {}, UINT iconW = 0, UINT iconH = 0);
    void Hide();
    bool Visible() const { return window_ && IsWindowVisible(window_); }
    HWND Window() const { return window_; }
    // Diagnostics: the next glass frame is read back to `path` (BMP; the glass and drawn content, not the thumbnails).
    bool RequestSnapshot(const std::wstring& path);

private:
    struct Tile {
        HWND source;
        RECT bounds;        // the card, client px
        RECT image;         // the thumbnail well, client px
        RECT buttons[3];    // close, minimize, zoom (hit areas), client px
        RECT title;         // title capsule, client px
        HTHUMBNAIL thumbnail = nullptr;
    };

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT Handle(UINT message, WPARAM w, LPARAM l);
    bool CreateWindowAndSurface(HWND owner);
    void ClearTiles();
    void LayoutPage();
    void ChangePage(int delta);
    void Paint();
    void PaintGdi(HDC dc);
    void DrawGlassContent(ID2D1DeviceContext* dc, bool gpu);
    void WindowCommand(int tile, int button);
    void StepAppear();
    int Hit(POINT point) const;
    int Px(int dip) const { return MulDiv(dip, dpi_, 96); }
    float Dip(LONG px) const { return static_cast<float>(px) * 96.f / static_cast<float>(dpi_); }

    HWND window_ = nullptr, owner_ = nullptr;
    std::unique_ptr<liquid::popup::MenuSurface> surface_;
    UINT activateMessage_ = 0, dpi_ = 96;
    bool dark_ = true;
    std::wstring name_;
    std::vector<HWND> sources_;
    std::vector<std::uint32_t> icon_;
    UINT iconW_ = 0, iconH_ = 0;
    std::vector<Tile> tiles_;
    RECT anchor_{}, work_{}, previous_{}, next_{};
    int columns_ = 1, pageSize_ = 1, page_ = 0, hovered_ = -1, pressed_ = -1;
    int cellWidth_ = 0, imageHeight_ = 0, cellHeight_ = 0;
    ULONGLONG shownAt_ = 0;
    float appear_ = 0.f;
};

} // namespace dock
