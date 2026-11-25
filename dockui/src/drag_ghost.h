#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dock {

// The icon being dragged off the Dock, drawn in a small click-through window that follows the pointer anywhere on the
// screen (the Dock's own window is only as tall as the magnified icons, so above it the icon used to vanish). Like
// macOS: once letting go would remove the item, the icon turns translucent and a "Remove" label replaces its name.
class DragGhost {
public:
    DragGhost() = default;
    DragGhost(const DragGhost&) = delete;
    DragGhost& operator=(const DragGhost&) = delete;
    ~DragGhost();

    // `pixels`: premultiplied BGRA, w x h. `iconTopLeft`: screen pixels of the icon's top-left corner; `sizePx`: its
    // drawn side. Redraws only when the image, size, label or scale changes; otherwise it just moves the window.
    void Show(const std::vector<uint32_t>& pixels, UINT w, UINT h, uint64_t imageKey, POINT iconTopLeft, int sizePx,
              bool remove, const std::wstring& label, float scale);
    void Hide();
    bool Visible() const { return visible_; }

private:
    bool Render(const std::vector<uint32_t>& pixels, UINT w, UINT h, int sizePx, bool remove, const std::wstring& label, float scale);

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    uint64_t imageKey_ = 0;
    int sizePx_ = 0;
    bool remove_ = false;
    float scale_ = 0.f;
    std::wstring label_;
    SIZE size_{};
    POINT iconOffset_{};  // icon top-left inside the window
};

}  // namespace dock
