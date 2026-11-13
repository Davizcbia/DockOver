#include "dock_model.h"

#include <algorithm>
#include <cmath>

namespace dock {
namespace {
constexpr float kPi = 3.14159265358979f;

float Falloff(float d) {
    if (d >= 1.0f) return 0.0f;
    return 0.5f * (1.0f + std::cos(kPi * d));
}
}  // namespace

DockModel::DockModel() {
    hover_.SetSpec(dockmotion::HoverOut);
    hover_.SetRest(0.001, 0.01);        // normalised 0..1
    cursor_.SetSpec(dockmotion::Cursor);
    cursor_.SetRest(0.05, 1.0);         // DIP, DIP/s
}

void DockModel::SetTiles(const std::vector<bool>& separators) {
    sep_ = separators;
    bounce_.assign(sep_.size(), docky::motion::Bounce{});
    baseCenter_.resize(sep_.size());
    float edge = m.padX;
    for (int i = 0; i < Count(); ++i) {
        baseCenter_[i] = edge + TileBase(i) * 0.5f;
        edge += TileBase(i);
    }
    const float base = BaseWidth();
    maxExtra_ = 0.0f;
    std::vector<float> s;
    for (int k = 0; k <= 600; ++k) {
        const float mouse = base * static_cast<float>(k) / 600.0f;
        s.resize(sep_.size());
        for (int i = 0; i < Count(); ++i) s[i] = sep_[i] ? 1.0f : ScaleAt(Collapse(mouse) - Collapse(baseCenter_[i]), 1.0f);
        maxExtra_ = std::max(maxExtra_, MagWidth(s) - base);
    }
}

float DockModel::BaseWidth() const {
    float w = 2.0f * m.padX;
    for (int i = 0; i < Count(); ++i) w += TileBase(i);
    return Count() == 0 ? w + m.pitch : w;
}

float DockModel::DesiredWindowWidth() const { return BaseWidth() + 2.0f * maxExtra_ + 2.0f * m.sideMargin; }

float DockModel::DesiredWindowHeight() const {
    const float grow = m.icon * (m.maxScale - 1.0f);
    const float lift = grow * m.liftFactor;
    return std::max(m.labelSpace, m.bounceHeight) + grow + lift + m.DockHeight() + m.bottomMargin;
}

void DockModel::SetWindowSize(float width, float height) {
    winW_ = width;
    winH_ = height;
}

float DockModel::BaseLeft() const { return (winW_ - BaseWidth()) * 0.5f; }

RectF DockModel::BaseDockRect() const {
    RectF r;
    r.left = BaseLeft();
    r.right = r.left + BaseWidth();
    r.bottom = winH_ - m.bottomMargin;
    r.top = r.bottom - m.DockHeight();
    return r;
}

float DockModel::ScaleAt(float baseX, float h) const {
    // Distance is measured in resting coordinates, in units of the resting pitch.
    return 1.0f + (m.maxScale - 1.0f) * h * Falloff(std::fabs(baseX) / (m.rangeSlots * m.pitch));
}

// Magnification distances are measured with separator gaps collapsed to a point: the pointer crossing a separator then
// behaves like crossing between two neighbouring icons. Measured on raw positions the gap left no icon centre near the
// pointer, so the neighbours shrank and the whole Dock narrowed for a moment (reported 2026-11-10).
// The gap is compressed to kSeparatorKeep of its width rather than to a point: with a point the measured position
// stood still for the whole separator, so every icon froze until the pointer had crossed it (reported 2026-11-11).
// Keeping a fifth of it keeps the magnification moving; the Dock then slides about 8 DIP across a separator (was 22 DIP
// with centre anchoring, 1 DIP but frozen with a full collapse; measured by dock_model_visual_test).
constexpr float kSeparatorKeep = 0.2f;
float DockModel::Collapse(float x) const {
    float out = x;
    for (int i = 0; i < Count(); ++i) {
        if (!sep_[i]) continue;
        const float half = TileBase(i) * 0.5f, l = baseCenter_[i] - half, r = baseCenter_[i] + half;
        if (x >= r) out -= (r - l) * (1.0f - kSeparatorKeep);
        else if (x > l) out -= (x - l) * (1.0f - kSeparatorKeep);
    }
    return out;
}

void DockModel::Scales(float mouse, float h, std::vector<float>& s) const {
    s.resize(sep_.size());
    if (reducedMotion_) h = 0.0f;
    const float m0 = Collapse(mouse);
    for (int i = 0; i < Count(); ++i) s[i] = sep_[i] ? 1.0f : ScaleAt(m0 - Collapse(baseCenter_[i]), h);
}

float DockModel::MagWidth(const std::vector<float>& s) const {
    float w = 2.0f * m.padX;
    for (int i = 0; i < Count(); ++i) w += TileBase(i) * s[i];
    return Count() == 0 ? w + m.pitch : w;
}

void DockModel::SetPointer(bool inside, float x, float y) {
    lastX_ = x;
    lastY_ = y;
    bool active = false;
    if (inside && Count() > 0) {
        DockFrame f;
        Compute(f, lastNow_);
        const float grow = m.icon * (m.maxScale - 1.0f) * hover_.F();
        const float zoneTop = f.dockTop - grow * (1.0f + m.liftFactor);
        active = x >= f.dockLeft && x <= f.dockRight && y >= zoneTop && y <= winH_;
    }
    // Retargeting keeps the spring's value and velocity, so entering/leaving mid-animation reverses
    // smoothly instead of restarting.
    if (active != (hover_.Target() > 0.0)) hover_.SetSpec(active ? dockmotion::HoverIn : dockmotion::HoverOut);
    hover_.SetTarget(active ? 1.0 : 0.0);
    if (active) {
        const double target = x - BaseLeft();
        if (hover_.Value() < 0.02) cursor_.SnapTo(target);  // entering from rest: no sweep across the dock
        else cursor_.SetTarget(target);
    }
}

void DockModel::StartBounce(int index, double /*now*/) {
    if (reducedMotion_ || index < 0 || index >= Count() || sep_[index]) return;
    docky::motion::BounceSpec spec = dockmotion::Launch;
    spec.apex = m.bounceHeight;
    bounce_[static_cast<size_t>(index)].Launch(spec);
}

void DockModel::SetReducedMotion(bool enabled) {
    reducedMotion_ = enabled;
    if (enabled) {
        hover_.SnapTo(hover_.Target());
        cursor_.SnapTo(cursor_.Target());
        for (auto& b : bounce_) b.Cancel();
    }
}

bool DockModel::Update(double dt, double now) {
    lastNow_ = now;
    if (reducedMotion_) {
        hover_.SnapTo(hover_.Target());
        cursor_.SnapTo(cursor_.Target());
        return false;
    }
    hover_.Step(dt);
    cursor_.Step(dt);
    for (auto& b : bounce_) b.Step(dt);
    return IsAnimating(now);
}

bool DockModel::IsAnimating(double /*now*/) const {
    if (hover_.Animating()) return true;
    if (hover_.Value() > 0.0 && cursor_.Animating()) return true;
    for (const auto& b : bounce_) if (b.Active()) return true;
    return false;
}

float DockModel::BounceLift(int i, double /*now*/) const {
    return static_cast<float>(bounce_[static_cast<size_t>(i)].Height());
}

void DockModel::Compute(DockFrame& out, double now) const {
    const int n = Count();
    std::vector<float> s;
    const float cursor = cursor_.F(), hover = hover_.F();
    Scales(cursor, hover, s);
    std::vector<float> magCenter(static_cast<size_t>(n)), magLeft(static_cast<size_t>(n));
    float edge = m.padX;
    for (int i = 0; i < n; ++i) {
        const float w = TileBase(i) * s[i];
        magLeft[i] = edge;
        magCenter[i] = edge + w * 0.5f;
        edge += w;
    }
    const float base = BaseWidth();
    const float width = MagWidth(s);

    // Map the pointer's resting position into the magnified layout (piecewise linear between tile
    // centres) and shift so the content under the pointer stays under the pointer. Measured macOS
    // captures show the same behaviour: symmetric growth mid-dock, asymmetric near the ends.
    // Knots: plate edges and padding map 1:1 (padding does not scale), tile centres map to their
    // magnified centres. Pointers in the padding therefore never land inside the first/last tile.
    float mapped = cursor;
    if (n > 0 && hover > 0.0f) {
        std::vector<float> kb, km;
        kb.reserve(static_cast<size_t>(n) + 4);
        km.reserve(static_cast<size_t>(n) + 4);
        kb.push_back(0.0f);
        km.push_back(0.0f);
        kb.push_back(m.padX);
        km.push_back(m.padX);
        // Knots: icon centres, and both edges of each separator (it does not scale). Anchoring a separator at its centre
        // made the Dock slide sideways by ~11 DIP while the pointer crossed it (measured 2026-11-10), which read as the
        // Dock shrinking; with edge knots, crossing a separator maps like crossing between two icons.
        for (int i = 0; i < n; ++i) {
            if (sep_[i]) {
                const float half = TileBase(i) * 0.5f;
                kb.push_back(baseCenter_[i] - half); km.push_back(magLeft[i]);
                kb.push_back(baseCenter_[i] + half); km.push_back(magLeft[i] + TileBase(i) * s[i]);
                continue;
            }
            kb.push_back(baseCenter_[i]);
            km.push_back(magCenter[i]);
        }
        kb.push_back(base - m.padX);
        km.push_back(width - m.padX);
        kb.push_back(base);
        km.push_back(width);
        if (cursor <= 0.0f) {
            mapped = cursor;
        } else if (cursor >= base) {
            mapped = cursor + (width - base);
        } else {
            size_t k = 0;
            while (k + 2 < kb.size() && cursor > kb[k + 1]) ++k;
            const float span = std::max(1e-4f, kb[k + 1] - kb[k]);
            mapped = km[k] + (cursor - kb[k]) / span * (km[k + 1] - km[k]);
        }
    }
    float left = BaseLeft() + (cursor - mapped);
    const float minLeft = m.sideMargin * 0.25f;
    left = std::clamp(left, minLeft, std::max(minLeft, winW_ - minLeft - width));

    out.dockLeft = left;
    out.dockRight = left + width;
    out.dockBottom = winH_ - m.bottomMargin;
    out.dockTop = out.dockBottom - m.DockHeight();
    out.items.resize(static_cast<size_t>(n));
    const float iconBottom = out.dockBottom - m.padY;
    const float centerY = out.dockTop + m.DockHeight() * 0.5f;
    for (int i = 0; i < n; ++i) {
        ItemGeom& g = out.items[i];
        g.separator = sep_[i];
        g.tileLeft = left + magLeft[i];
        g.tileRight = g.tileLeft + TileBase(i) * s[i];
        g.cx = left + magCenter[i];
        if (g.separator) {
            g.size = 0.0f;
            g.left = g.cx;
            const float half = (m.icon + m.sepLineExtra) * 0.5f;
            g.lineTop = centerY - half;
            g.lineBottom = centerY + half;
            g.top = g.restTop = g.lineTop;
            continue;
        }
        g.size = m.icon * s[i];
        g.left = g.cx - g.size * 0.5f;
        // macOS icon canvases carry ~10% bottom margin that scales with magnification; Windows icons
        // are trimmed to their body, so the equivalent rise is applied explicitly.
        const float lift = (s[i] - 1.0f) * m.liftFactor * m.icon;
        g.restTop = iconBottom - lift - g.size;
        g.top = g.restTop - BounceLift(i, now);
    }
}

int DockModel::HitTest(const DockFrame& frame, float x, float y) const {
    for (int i = 0; i < static_cast<int>(frame.items.size()); ++i) {
        const ItemGeom& g = frame.items[i];
        if (g.separator) continue;
        const float t = std::min(g.top, frame.dockTop) - 2.0f;
        if (x >= g.tileLeft && x < g.tileRight && y >= t && y <= frame.dockBottom + m.bottomMargin) return i;
    }
    return -1;
}

int DockModel::HoveredIndex(const DockFrame& frame) const {
    if (hover_.Target() <= 0.0) return -1;
    for (int i = 0; i < static_cast<int>(frame.items.size()); ++i) {
        const ItemGeom& g = frame.items[i];
        if (!g.separator && lastX_ >= g.tileLeft && lastX_ < g.tileRight) return i;
    }
    return -1;
}

}  // namespace dock
