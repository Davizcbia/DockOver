#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <string>
#include <vector>

namespace dock {

// One non-activating destination for the shell compositor's live thumbnails.
class WindowPreview {
public:
    WindowPreview() = default;
    ~WindowPreview();
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    bool Show(HWND owner, UINT activateMessage, RECT anchor, UINT dpi, bool dark,
              const std::wstring& appName, std::vector<HWND> sources);
    void Hide();
    bool Visible() const { return window_ && IsWindowVisible(window_); }
    HWND Window() const { return window_; }

private:
    struct Tile {
        HWND source;
        RECT bounds;
        HTHUMBNAIL thumbnail = nullptr;
    };

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT Handle(UINT message, WPARAM w, LPARAM l);
    void ClearTiles();
    void LayoutPage();
    void ChangePage(int delta);
    void Paint();
    int Hit(POINT point) const;
    int Px(int dip) const { return MulDiv(dip, dpi_, 96); }

    HWND window_ = nullptr, owner_ = nullptr;
    UINT activateMessage_ = 0, dpi_ = 96;
    bool dark_ = true;
    std::wstring name_;
    std::vector<HWND> sources_;
    std::vector<Tile> tiles_;
    RECT anchor_{}, work_{}, previous_{}, next_{};
    int columns_ = 1, pageSize_ = 1, page_ = 0, hovered_ = -1, pressed_ = -1;
    int cellWidth_ = 0, imageHeight_ = 0, cellHeight_ = 0;
};

} // namespace dock
