#pragma once
#include "minimize_dock.h"

#include "fullscreen_window.h"
#include "vblank.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>

namespace docky {

// A stage belongs to this monitor. Minimize only ordinary task windows, and remember
// which ones we moved so switching off restores the user's previous desktop.
class StageManager {
    struct Member {
        HWND window;
        DWORD process;
        unsigned group;
        bool parked;
        bool maximized;
    };
    struct Preview {
        int width{}, height{};
        std::vector<uint32_t> pixels;
    };
    struct Card {
        unsigned group;
        RECT bounds;
        HWND source;
        std::vector<HWND> layers;
        RECT preview, badge;
    };
    struct Dib {
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap{};
        HGDIOBJ old{};
        uint32_t* pixels{};
        int width{}, height{};
        Dib() = default;
        Dib(const Dib&) = delete;
        ~Dib() {
            Reset();
            if (dc)
                DeleteDC(dc);
        }
        void Reset() {
            if (bitmap) {
                SelectObject(dc, old);
                DeleteObject(bitmap);
                bitmap = nullptr;
            }
            pixels = nullptr;
        }
        bool Resize(int w, int h) {
            if (bitmap && w == width && h == height)
                return true;
            Reset();
            if (!dc || w <= 0 || h <= 0)
                return false;
            BITMAPINFO info{};
            auto& header = info.bmiHeader;
            header.biSize = sizeof(header);
            header.biWidth = w;
            header.biHeight = -h;
            header.biPlanes = 1;
            header.biBitCount = 32;
            void* bits{};
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bitmap)
                return false;
            old = SelectObject(dc, bitmap);
            pixels = static_cast<uint32_t*>(bits);
            width = w;
            height = h;
            return true;
        }
    };
    HWND owner_{}, shelf_{}, transition_{}, outgoing_{}, previewHost_{};
    Dib atlas_, shelfDib_;
    std::unordered_map<HWND, Preview> previews_;
    std::unordered_map<HWND, HICON> icons_;
    HWINEVENTHOOK moveEnd_{};
    HMONITOR monitor_{};
    std::vector<Member> members_;
    std::vector<Card> cards_;
    unsigned active_{}, nextGroup_{1}, pressed_{};
    double scroll_{}, maxScroll_{};
    int rowHeight_{};
    int panY_{};
    bool suspended_{}, reduced_{}, perspective_{true};
    double horizon_{}, planeLeft_{}, planeWidth_{1};
    HTHUMBNAIL animation_{}, outgoingThumbnail_{};
    RECT from_{}, to_{}, outgoingFrom_{}, workArea_{};
    UINT shelfDpi_{};
    HWND activating_{};
    HTHUMBNAIL dragThumbnail_{};
    RECT dragOrigin_{};
    POINT pressPoint_{};
    double started_{};
    vblank::Ticker ticker_;
    static constexpr auto frame = WM_APP + 412;
    static constexpr auto className = L"DockOverStageShelf";

    bool Live(const Member& member) const {
        DWORD pid{};
        return GetWindowThreadProcessId(member.window, &pid) && pid == member.process;
    }
    unsigned Group(HWND w) const {
        for (const auto& m : members_)
            if (m.window == w && Live(m))
                return m.group;
        return 0;
    }
    bool Eligible(HWND w) const {
        if (!fullscreen::OnMonitor(w, monitor_) || GetWindow(w, GW_OWNER) ||
            GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
            return false;
        wchar_t cls[80]{};
        GetClassNameW(w, cls, 80);
        if (w == GetShellWindow() || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman") ||
            !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
            return false;
        DWORD pid{};
        GetWindowThreadProcessId(w, &pid);
        return pid != GetCurrentProcessId() && GetWindowTextLengthW(w) > 0;
    }
    void Add(HWND w) {
        if (!Eligible(w) || Group(w))
            return;
        DWORD pid{};
        GetWindowThreadProcessId(w, &pid);
        unsigned group = 0;
        for (const auto& m : members_)
            if (m.process == pid && Live(m)) {
                group = m.group;
                break;
            }
        members_.push_back({w, pid, group ? group : nextGroup_++, false, false});
    }
    static BOOL CALLBACK Collect(HWND w, LPARAM arg) {
        reinterpret_cast<StageManager*>(arg)->Add(w);
        return TRUE;
    }
    static RECT PlaceThumb(HTHUMBNAIL thumb, RECT box) {
        SIZE size{};
        if (!thumb || FAILED(DwmQueryThumbnailSourceSize(thumb, &size)) || size.cx <= 0 || size.cy <= 0)
            return box;
        const double scale = (std::min)(double(box.right - box.left) / size.cx, double(box.bottom - box.top) / size.cy);
        const LONG w = LONG(size.cx * scale), h = LONG(size.cy * scale);
        box.left += (box.right - box.left - w) / 2;
        box.top += (box.bottom - box.top - h) / 2;
        box.right = box.left + w;
        box.bottom = box.top + h;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
        p.rcDestination = box;
        p.fVisible = TRUE;
        DwmUpdateThumbnailProperties(thumb, &p);
        return box;
    }
    static HICON AppIcon(HWND window) {
        DWORD_PTR handle{};
        SendMessageTimeoutW(window, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 15, &handle);
        if (!handle)
            handle = GetClassLongPtrW(window, GCLP_HICONSM);
        if (!handle)
            handle = GetClassLongPtrW(window, GCLP_HICON);
        return handle ? reinterpret_cast<HICON>(handle) : LoadIconW(nullptr, IDI_APPLICATION);
    }
    void CapturePreviews(int width, int height) {
        if (!previewHost_)
            return;
        struct Capture {
            HWND source;
            HTHUMBNAIL thumbnail;
            RECT bounds;
        };
        std::vector<Capture> captures;
        for (const auto& card : cards_) {
            for (HWND window : card.layers) {
                if (previews_.contains(window) || std::any_of(captures.begin(), captures.end(),
                                                              [&](const auto& item) { return item.source == window; }))
                    continue;
                HTHUMBNAIL thumbnail{};
                if (SUCCEEDED(DwmRegisterThumbnail(previewHost_, window, &thumbnail))) {
                    const int top = static_cast<int>(captures.size()) * height;
                    captures.push_back({window, thumbnail, {0, top, width, top + height}});
                }
            }
        }
        if (captures.empty())
            return;
        const int atlasHeight = static_cast<int>(captures.size()) * height;
        if (atlas_.Resize(width, atlasHeight)) {
            // DWM keeps minimized windows' thumbnails composited in this off-screen host.
            SetWindowPos(previewHost_, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 32,
                         GetSystemMetrics(SM_YVIRTUALSCREEN) - atlasHeight - 32, width, atlasHeight,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            for (auto& capture : captures)
                capture.bounds = PlaceThumb(capture.thumbnail, capture.bounds);
            DwmFlush();
            std::fill_n(atlas_.pixels, static_cast<size_t>(width) * atlasHeight, 0u);
            if (PrintWindow(previewHost_, atlas_.dc, 2)) {
                GdiFlush();
                for (const auto& capture : captures) {
                    const auto& r = capture.bounds;
                    Preview preview{r.right - r.left, r.bottom - r.top, {}};
                    preview.pixels.reserve(static_cast<size_t>(preview.width) * preview.height);
                    for (int y = r.top; y < r.bottom; ++y) {
                        const auto* row = atlas_.pixels + y * width + r.left;
                        preview.pixels.insert(preview.pixels.end(), row, row + preview.width);
                    }
                    if (std::any_of(preview.pixels.begin(), preview.pixels.end(),
                                    [](uint32_t pixel) { return (pixel & 0xffffffu) != 0; }))
                        previews_.emplace(capture.source, std::move(preview));
                }
            }
            ShowWindow(previewHost_, SW_HIDE);
        }
        for (const auto& capture : captures)
            DwmUnregisterThumbnail(capture.thumbnail);
    }
    static uint32_t Over(uint32_t destination, uint32_t source, unsigned alpha) {
        const unsigned keep = 255 - alpha;
        const auto channel = [&](unsigned shift) {
            return ((((source >> shift) & 255u) * alpha + ((destination >> shift) & 255u) * keep + 127) / 255) << shift;
        };
        return ((alpha + (destination >> 24) * keep / 255) << 24) | channel(16) | channel(8) | channel(0);
    }
    double PlaneScale(double x) const {
        constexpr double depth = .20;
        return perspective_ ? 1 - depth * std::clamp((x - planeLeft_) / planeWidth_, 0.0, 1.0) : 1;
    }
    double ProjectY(double y, double x) const {
        return horizon_ + (y - horizon_) * PlaneScale(x);
    }
    double UnprojectY(double y, double x) const {
        return horizon_ + (y - horizon_) / PlaneScale(x);
    }
    RECT ProjectBounds(RECT box) const {
        return {box.left,
                LONG(std::floor((std::min)(ProjectY(box.top, box.left), ProjectY(box.top, box.right)))),
                box.right,
                LONG(std::ceil((std::max)(ProjectY(box.bottom, box.left), ProjectY(box.bottom, box.right))))};
    }
    bool Hit(const Card& card, POINT pointer) const {
        if (PtInRect(&card.badge, pointer))
            return true;
        pointer.y = static_cast<LONG>(std::floor(UnprojectY(pointer.y + .5, pointer.x + .5)));
        return PtInRect(&card.bounds, pointer);
    }
    RECT DrawPreview(HWND source, RECT box) {
        const auto it = previews_.find(source);
        const Preview* preview = it == previews_.end() ? nullptr : &it->second;
        if (preview) {
            const double scale = (std::min)(double(box.right - box.left) / preview->width,
                                            double(box.bottom - box.top) / preview->height);
            const LONG w = (std::max)(1L, LONG(preview->width * scale));
            const LONG h = (std::max)(1L, LONG(preview->height * scale));
            box.left += (box.right - box.left - w) / 2;
            box.top += (box.bottom - box.top - h) / 2;
            box.right = box.left + w;
            box.bottom = box.top + h;
        }
        const int width = box.right - box.left;
        const double soft = (std::max)(2.0, 6.0 * shelfDpi_ / 96);
        const int bleed = static_cast<int>(soft * 2);
        const int xStart = (std::max)(0L, box.left - bleed);
        const int xEnd = (std::min)(LONG(shelfDib_.width), box.right + bleed);
        // Each thumbnail samples the same plane, including narrow windows and windows in a group.
        const double nearScale = PlaneScale(box.left), farScale = PlaneScale(box.right);
        for (int x = xStart; x < xEnd; ++x) {
            const double u = std::clamp((x + .5 - box.left) / width, 0.0, 1.0);
            const double scale = std::lerp(nearScale, farScale, u);
            const double top = horizon_ + (box.top - horizon_) * scale;
            const double bottom = horizon_ + (box.bottom - horizon_) * scale;
            const double textureU = u * farScale / scale;
            const int yStart = (std::max)(0, static_cast<int>(std::floor(top)) - bleed);
            const int yEnd = (std::min)(shelfDib_.height, static_cast<int>(std::ceil(bottom)) + bleed);
            const double dx = (std::max)({box.left - x - .5, x + .5 - box.right, 0.0});
            for (int y = yStart; y < yEnd; ++y) {
                const double v = (y + .5 - top) / (bottom - top);
                auto& pixel = shelfDib_.pixels[y * shelfDib_.width + x];
                const double dy = (std::max)({top + soft * .4 - y, y - bottom - soft * .4, 0.0});
                const double edge = std::clamp((std::min)(y + 1 - top, bottom - y), 0.0, 1.0);
                if (edge < 1 || dx > 0) {
                    const double fade = (std::max)(0.0, 1 - std::sqrt(dx * dx + dy * dy) / bleed);
                    pixel = Over(pixel, 0, static_cast<unsigned>(54 * fade * fade));
                }
                if (dx == 0 && edge > 0) {
                    uint32_t color = 0x313b49u;
                    if (preview) {
                        const int sx = (std::min)(preview->width - 1, static_cast<int>(textureU * preview->width));
                        const int sy = std::clamp(static_cast<int>(v * preview->height), 0, preview->height - 1);
                        color = preview->pixels[sy * preview->width + sx];
                    }
                    pixel = Over(pixel, color, static_cast<unsigned>(edge * 255 + .5));
                }
            }
        }
        return box;
    }
    void Layout(bool scrolling = false) {
        cards_.clear();
        MONITORINFO mi{sizeof(mi)};
        if (suspended_ || !GetMonitorInfoW(monitor_, &mi)) {
            ShowWindow(shelf_, SW_HIDE);
            return;
        }
        std::vector<Member> groups;
        for (const auto& m : members_)
            if (m.group != active_ && Live(m) &&
                std::none_of(groups.begin(), groups.end(), [&](const auto& g) { return g.group == m.group; }))
                groups.push_back(m);
        if (groups.empty()) {
            previews_.clear();
            ShowWindow(shelf_, SW_HIDE);
            return;
        }
        const int dpi = GetDpiForWindow(owner_);
        if (!scrolling) {
            previews_.clear();
            icons_.clear();
        }
        shelfDpi_ = dpi;
        workArea_ = mi.rcWork;
        const int width = MulDiv(156, dpi, 96), height = MulDiv(122, dpi, 96), pad = MulDiv(7, dpi, 96);
        const int margin = MulDiv(28, dpi, 96);
        const int gap = MulDiv(12, dpi, 96);
        const int stride = height + gap;
        const int extent = static_cast<int>(groups.size()) * stride - gap;
        const int viewport = (std::min)(extent, (std::max)(height, int(mi.rcWork.bottom - mi.rcWork.top) - 2 * margin));
        rowHeight_ = stride;
        maxScroll_ = extent - viewport;
        scroll_ = std::clamp(scroll_, 0.0, maxScroll_);
        const int offset = static_cast<int>(std::round(scroll_));
        POINT origin{mi.rcWork.left + pad, (mi.rcWork.top + mi.rcWork.bottom - viewport) / 2};
        horizon_ = (mi.rcMonitor.top + mi.rcMonitor.bottom) * .5 - origin.y;
        planeLeft_ = 2 * pad;
        planeWidth_ = (std::max)(1, width - 4 * pad);
        // The far side can bring rows that start beyond the near edge into view.
        const double top = (std::min)(0.0, UnprojectY(0, width));
        const double bottom = (std::max)(double(viewport), UnprojectY(viewport, width));
        const int first = (std::max)(0, static_cast<int>(std::floor((offset + top) / stride)));
        const int last = (std::min)(static_cast<int>(groups.size()), static_cast<int>(std::ceil((offset + bottom) / stride)));
        for (int i = first; i < last; ++i) {
            const auto& m = groups[i];
            const int rowTop = i * stride - offset;
            RECT bounds{0, rowTop, width, rowTop + height};
            Card card{m.group, bounds, m.window, {}, {}, {}};
            for (const auto& member : members_) {
                if (member.group == m.group && member.window != m.window && Live(member)) {
                    card.layers.push_back(member.window);
                    if (card.layers.size() == 2)
                        break;
                }
            }
            std::reverse(card.layers.begin(), card.layers.end());
            card.layers.push_back(m.window);
            cards_.push_back(std::move(card));
        }
        CapturePreviews(width - 4 * pad, height - 3 * pad);
        if (!shelfDib_.Resize(width, viewport))
            return;
        std::fill_n(shelfDib_.pixels, static_cast<size_t>(width) * viewport, 0u);
        for (auto& card : cards_) {
            RECT front = card.bounds;
            InflateRect(&front, -pad * 2, -pad);
            front.bottom -= pad;
            for (size_t i = 0; i < card.layers.size(); ++i) {
                const int depth = static_cast<int>(card.layers.size() - 1 - i);
                RECT box = front;
                if (depth) {
                    InflateRect(&box, -pad, -pad);
                    OffsetRect(&box, pad * depth, -pad * depth);
                }
                card.preview = DrawPreview(card.layers[i], box);
            }
            const int side = MulDiv(21, dpi, 96);
            const LONG x = (std::max)(1L, card.preview.left - pad);
            const LONG y = std::lround(ProjectY(card.preview.bottom, card.preview.left)) - side / 2;
            card.badge = {x, y, x + side, y + side};
            std::vector<HICON> icons;
            for (auto window = card.layers.rbegin(); window != card.layers.rend(); ++window) {
                const auto icon = icons_.try_emplace(*window, nullptr).first;
                if (!icon->second)
                    icon->second = AppIcon(*window);
                if (std::find(icons.begin(), icons.end(), icon->second) == icons.end())
                    icons.push_back(icon->second);
            }
            for (size_t n = 0; n < icons.size(); ++n)
                DrawIconEx(shelfDib_.dc, x + static_cast<int>(n) * side, y, icons[n], side, side, 0, nullptr,
                           DI_NORMAL);
            card.badge.right = x + static_cast<LONG>(icons.size()) * side;
        }
        GdiFlush();
        POINT zero{};
        SIZE size{width, viewport};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(shelf_, nullptr, &origin, &size, shelfDib_.dc, &zero, 0, &blend, ULW_ALPHA);
        UpdateShelfVisibility();
    }
    void ScrollBy(double delta) {
        if (pressed_)
            return;
        const double next = std::clamp(scroll_ + delta, 0.0, maxScroll_);
        if (next == scroll_)
            return;
        const bool moved = std::round(next) != std::round(scroll_);
        scroll_ = next;
        if (moved)
            Layout(true);
    }
    void UpdateShelfVisibility() {
        if (suspended_ || cards_.empty())
            return;
        RECT shelf{};
        POINT pointer{};
        if (!GetWindowRect(shelf_, &shelf))
            return;
        const bool hasPointer = GetCursorPos(&pointer) != FALSE;
        const HWND front = GetForegroundWindow();
        RECT frontBounds{}, overlap{};
        const bool covered = Group(front) == active_ && active_ && !IsIconic(front) &&
                             GetWindowRect(front, &frontBounds) && IntersectRect(&overlap, &shelf, &frontBounds);
        const int edge = MulDiv(5, shelfDpi_, 96);
        const bool atEdge = hasPointer && pointer.x >= workArea_.left && pointer.x <= shelf.left + edge &&
                            pointer.y >= workArea_.top && pointer.y < workArea_.bottom;
        const bool overShelf = hasPointer && IsWindowVisible(shelf_) && PtInRect(&shelf, pointer);
        const bool hide = covered && !atEdge && !overShelf && !pressed_ && !activating_;
        if (hide == (IsWindowVisible(shelf_) != FALSE))
            ShowWindow(shelf_, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
    void ClearDrag() {
        if (dragThumbnail_) {
            DwmUnregisterThumbnail(dragThumbnail_);
            dragThumbnail_ = nullptr;
            ShowWindow(transition_, SW_HIDE);
        }
    }
    void Drag(POINT pointer) {
        if (!pressed_)
            return;
        if (!dragThumbnail_) {
            if (abs(pointer.x - pressPoint_.x) < GetSystemMetrics(SM_CXDRAG) &&
                abs(pointer.y - pressPoint_.y) < GetSystemMetrics(SM_CYDRAG))
                return;
            const auto card =
                std::find_if(cards_.begin(), cards_.end(), [&](const Card& c) { return c.group == pressed_; });
            if (card == cards_.end() || !transition_)
                return;
            EndAnimation();
            if (FAILED(DwmRegisterThumbnail(transition_, card->source, &dragThumbnail_)))
                return;
            dragOrigin_ = ProjectBounds(card->preview);
            MapWindowPoints(shelf_, nullptr, reinterpret_cast<POINT*>(&dragOrigin_), 2);
        }
        RECT position = dragOrigin_;
        OffsetRect(&position, pointer.x - pressPoint_.x, pointer.y - pressPoint_.y);
        const int width = position.right - position.left, height = position.bottom - position.top;
        SetWindowPos(transition_, HWND_TOPMOST, position.left, position.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PlaceThumb(dragThumbnail_, {0, 0, width, height});
    }
    void EndAnimation() {
        ticker_.Stop();
        if (animation_)
            DwmUnregisterThumbnail(animation_);
        if (outgoingThumbnail_)
            DwmUnregisterThumbnail(outgoingThumbnail_);
        if (outgoing_)
            ShowWindow(outgoing_, SW_HIDE);
        outgoingThumbnail_ = nullptr;
        // An interrupted switch returns the outgoing group to its original place.
        if (activating_)
            for (auto& m : members_) {
                if (m.group == active_ && m.parked && Live(m)) {
                    ShowWindowAsync(m.window, SW_RESTORE);
                    dock::minimize::StageRestore(m.window);
                    m.parked = false;
                }
            }
        animation_ = nullptr;
        activating_ = nullptr;
        if (transition_)
            ShowWindow(transition_, SW_HIDE);
    }
    void Park(Member& member) {
        if (member.parked || IsIconic(member.window) || !Eligible(member.window))
            return;
        member.maximized = IsZoomed(member.window) != FALSE;
        dock::minimize::StagePark(member.window);
        member.parked = ShowWindowAsync(member.window, SW_SHOWMINNOACTIVE) != FALSE;
        if (!member.parked)
            dock::minimize::StageRestore(member.window);
    }
    bool Animate(HWND w, RECT start) {
        EndAnimation();
        if (reduced_ || !transition_ || !GetWindowRect(w, &to_))
            return false;
        if (IsIconic(w)) {
            WINDOWPLACEMENT placement{sizeof(placement)};
            MONITORINFO mi{sizeof(mi)};
            if (!GetWindowPlacement(w, &placement) || !GetMonitorInfoW(monitor_, &mi))
                return false;
            to_ = placement.rcNormalPosition;
            OffsetRect(&to_, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
            if (placement.flags & WPF_RESTORETOMAXIMIZED)
                to_ = mi.rcWork;
        }
        if (FAILED(DwmRegisterThumbnail(transition_, w, &animation_)))
            return false;
        activating_ = w;
        from_ = start;
        started_ = vblank::Seconds();
        const HWND foreground = GetForegroundWindow();
        auto old = std::find_if(members_.begin(), members_.end(), [&](const Member& m) {
            return m.group == active_ && m.window == foreground && Eligible(m.window);
        });
        if (old == members_.end())
            old = std::find_if(members_.begin(), members_.end(),
                               [&](const Member& m) { return m.group == active_ && Eligible(m.window); });
        if (outgoing_ && old != members_.end() && GetWindowRect(old->window, &outgoingFrom_) &&
            SUCCEEDED(DwmRegisterThumbnail(outgoing_, old->window, &outgoingThumbnail_))) {
            Tick();
            for (auto& m : members_)
                if (m.group == active_)
                    Park(m);
        } else
            Tick();
        ticker_.Start(shelf_, frame);
        return true;
    }
    void Tick() {
        if (!animation_)
            return;
        double t = (vblank::Seconds() - started_) / 0.26;
        if (t >= 1) {
            const HWND w = activating_;
            const unsigned group = Group(w);
            if (group) {
                Select(group, w);
                if (IsIconic(w))
                    ShowWindowAsync(w, SW_RESTORE);
                SetForegroundWindow(w);
            }
            EndAnimation();
            return;
        }
        // Critically damped response, normalized so thumbnail and real window meet exactly.
        const auto spring = [](double phase) { return 1 - (1 + 8 * phase) * std::exp(-8 * phase); };
        t = spring(t) / spring(1);
        auto mix = [&](LONG a, LONG b) { return LONG(a + (b - a) * t); };
        if (outgoingThumbnail_) {
            const RECT rect{mix(outgoingFrom_.left, from_.left), mix(outgoingFrom_.top, from_.top),
                            mix(outgoingFrom_.right, from_.right), mix(outgoingFrom_.bottom, from_.bottom)};
            SetWindowPos(outgoing_, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PlaceThumb(outgoingThumbnail_, {0, 0, rect.right - rect.left, rect.bottom - rect.top});
        }
        const RECT r{mix(from_.left, to_.left), mix(from_.top, to_.top), mix(from_.right, to_.right),
                     mix(from_.bottom, to_.bottom)};
        SetWindowPos(transition_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PlaceThumb(animation_, {0, 0, r.right - r.left, r.bottom - r.top});
    }
    void Select(unsigned group, HWND selected = nullptr, const RECT* start = nullptr) {
        if (!group)
            return;
        if (start && selected && Animate(selected, *start))
            return;
        if (group != active_) {
            // The stage just left is the first one in the recent-app strip.
            std::stable_partition(members_.begin(), members_.end(),
                                  [&](const Member& m) { return m.group == active_; });
        }
        active_ = group;
        HWND target = selected;
        for (auto& m : members_) {
            if (!Live(m))
                continue;
            if (m.group == group) {
                if (m.parked) {
                    ShowWindowAsync(m.window, SW_RESTORE);
                    dock::minimize::StageRestore(m.window);
                    m.parked = false;
                }
                if (!target)
                    target = m.window;
            } else
                Park(m);
        }
        if (target && start) {
            if (IsIconic(target))
                ShowWindowAsync(target, SW_RESTORE);
            SetForegroundWindow(target);
        }
        Layout();
    }
    void Click(POINT p) {
        const auto group = std::exchange(pressed_, 0u);
        const bool dragged = dragThumbnail_ != nullptr;
        ClearDrag();
        if (!group)
            return;
        for (const auto& c : cards_)
            if (c.group == group) {
                POINT screen = p;
                ClientToScreen(shelf_, &screen);
                if (p.x > c.bounds.right + GetSystemMetrics(SM_CXDRAG) && PtInRect(&workArea_, screen) && active_) {
                    const HWND source = c.source;
                    for (auto& m : members_)
                        if (m.group == group)
                            m.group = active_;
                    Select(active_);
                    SetForegroundWindow(source);
                } else if (!dragged && Hit(c, p)) {
                    RECT start = ProjectBounds(c.preview);
                    MapWindowPoints(shelf_, nullptr, reinterpret_cast<POINT*>(&start), 2);
                    Select(group, c.source, &start);
                }
                return;
            }
    }
    static void CALLBACK Moved(HWINEVENTHOOK, DWORD, HWND w, LONG object, LONG child, DWORD, DWORD) {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || !w)
            return;
        HWND shelf = FindWindowW(className, nullptr);
        DWORD pid{};
        GetWindowThreadProcessId(shelf, &pid);
        if (pid != GetCurrentProcessId())
            return;
        auto* self = reinterpret_cast<StageManager*>(GetWindowLongPtrW(shelf, GWLP_USERDATA));
        POINT cursor{};
        if (!self || self->suspended_ || !GetCursorPos(&cursor) || self->Group(w) != self->active_)
            return;
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(self->monitor_, &mi) || cursor.x < mi.rcMonitor.left ||
            cursor.x > mi.rcMonitor.left + MulDiv(156, GetDpiForWindow(shelf), 96) || cursor.y < mi.rcWork.top ||
            cursor.y >= mi.rcWork.bottom)
            return;
        if (std::count_if(self->members_.begin(), self->members_.end(),
                          [&](const auto& m) { return m.group == self->active_; }) < 2)
            return;
        for (auto& m : self->members_)
            if (m.window == w)
                m.group = self->nextGroup_++;
        HWND focus = nullptr;
        for (const auto& m : self->members_)
            if (m.group == self->active_ && self->Live(m)) {
                focus = m.window;
                break;
            }
        self->Select(self->active_, focus);
        if (focus)
            SetForegroundWindow(focus);
    }
    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<StageManager*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<StageManager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, msg, wp, lp);
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return LOWORD(lp) == HTCLIENT && HIWORD(lp) == WM_LBUTTONDOWN ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            for (const auto& c : self->cards_)
                if (self->Hit(c, p)) {
                    self->pressed_ = c.group;
                    self->pressPoint_ = p;
                    SetCapture(h);
                    break;
                }
            return 0;
        case WM_MOUSEMOVE:
            self->Drag(p);
            return 0;
        case WM_LBUTTONUP:
            self->Click(p);
            if (GetCapture() == h)
                ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            self->pressed_ = 0;
            self->ClearDrag();
            return 0;
        case WM_MOUSEWHEEL:
            self->ScrollBy(-GET_WHEEL_DELTA_WPARAM(wp) * self->rowHeight_ / double(WHEEL_DELTA));
            return 0;
        case WM_GESTURE: {
            GESTUREINFO info{sizeof(info)};
            const auto handle = reinterpret_cast<HGESTUREINFO>(lp);
            if (!GetGestureInfo(handle, &info) || info.dwID != GID_PAN)
                break;
            if (info.dwFlags & GF_BEGIN) {
                self->pressed_ = 0;
                self->ClearDrag();
                if (GetCapture() == h)
                    ReleaseCapture();
            } else {
                self->ScrollBy(self->panY_ - info.ptsLocation.y);
            }
            self->panY_ = info.ptsLocation.y;
            CloseGestureInfoHandle(handle);
            return 0;
        }
        case frame:
            self->ticker_.Ack();
            self->Tick();
            return 0;
        case WM_PRINT:
            if (self->shelfDib_.pixels) {
                BitBlt(reinterpret_cast<HDC>(wp), 0, 0, self->shelfDib_.width, self->shelfDib_.height,
                       self->shelfDib_.dc, 0, 0, SRCCOPY);
                return 0;
            }
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

  public:
    void SetPerspective(bool enabled) {
        if (perspective_ == enabled)
            return;
        perspective_ = enabled;
        if (shelf_)
            Layout(true);
    }
    bool Enabled() const {
        return shelf_ != nullptr;
    }
    void Toggle(HWND owner) {
        if (Enabled()) {
            Stop();
            return;
        }
        owner_ = owner;
        monitor_ = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        const DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        shelf_ = CreateWindowExW((ex & ~WS_EX_NOACTIVATE) | WS_EX_LAYERED, className, L"Stage Manager", WS_POPUP, 0, 0,
                                 1, 1, nullptr, nullptr, wc.hInstance, this);
        if (!shelf_)
            return;
        GESTURECONFIG pan{GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_INERTIA,
                          GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_GUTTER};
        SetGestureConfig(shelf_, 0, 1, &pan, sizeof(pan));
        previewHost_ =
            CreateWindowExW(ex, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        transition_ = CreateWindowExW(ex | WS_EX_TRANSPARENT, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      wc.hInstance, nullptr);
        outgoing_ = CreateWindowExW(ex | WS_EX_TRANSPARENT, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                    wc.hInstance, nullptr);
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        reduced_ = !animations;
        moveEnd_ = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, Moved, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        EnumWindows(Collect, reinterpret_cast<LPARAM>(this));
        const unsigned front = Group(GetForegroundWindow());
        if (!members_.empty())
            Select(front ? front : members_.front().group);
    }
    void Refresh(bool suspend) {
        if (!shelf_)
            return;
        const auto monitor = MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
        if (monitor != monitor_) {
            if (suspend) {
                if (!suspended_) {
                    suspended_ = true;
                    ClearDrag();
                    if (GetCapture() == shelf_)
                        ReleaseCapture();
                    EndAnimation();
                    Layout();
                }
                return;
            }
            const auto owner = owner_;
            Stop();
            Toggle(owner);
            return;
        }
        MONITORINFO info{sizeof(info)};
        const bool geometryChanged = GetMonitorInfoW(monitor_, &info) &&
                                     (!EqualRect(&workArea_, &info.rcWork) || shelfDpi_ != GetDpiForWindow(owner_));
        const bool changed = suspended_ != suspend;
        suspended_ = suspend;
        if (suspend) {
            if (changed) {
                ClearDrag();
                if (GetCapture() == shelf_)
                    ReleaseCapture();
                EndAnimation();
                Layout();
            }
            return;
        }
        if (activating_ || pressed_)
            return;
        const auto old = members_.size();
        std::erase_if(members_, [&](const auto& m) { return !Live(m) || !IsWindowVisible(m.window); });
        const HWND front = GetForegroundWindow();
        if (Eligible(front)) {
            Add(front);
            const unsigned group = Group(front);
            if (group && group != active_) {
                Select(group, front);
                return;
            }
        }
        if (old != members_.size() && !members_.empty() &&
            std::none_of(members_.begin(), members_.end(), [&](const Member& m) { return m.group == active_; })) {
            Select(members_.front().group);
            return;
        }
        if (changed || old != members_.size() || (geometryChanged && !cards_.empty()))
            Layout();
        else
            UpdateShelfVisibility();
    }
    void Stop() {
        if (moveEnd_) {
            UnhookWinEvent(moveEnd_);
            moveEnd_ = nullptr;
        }
        ClearDrag();
        EndAnimation();
        cards_.clear();
        previews_.clear();
        icons_.clear();
        atlas_.Reset();
        shelfDib_.Reset();
        if (previewHost_)
            DestroyWindow(previewHost_);
        previewHost_ = nullptr;
        if (shelf_)
            DestroyWindow(shelf_);
        if (transition_)
            DestroyWindow(transition_);
        if (outgoing_)
            DestroyWindow(outgoing_);
        shelf_ = transition_ = outgoing_ = nullptr;
        for (const auto& m : members_)
            if (m.parked && Live(m)) {
                ShowWindowAsync(m.window, m.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
                dock::minimize::StageRestore(m.window);
            }
        members_.clear();
        active_ = pressed_ = 0;
        scroll_ = maxScroll_ = 0;
        suspended_ = false;
    }
};
}
