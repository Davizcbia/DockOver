#pragma once
// Platform-independent Dock layout and animation model. All values are in DIPs relative to the
// Dock window's client area. V0.2 geometry is measured from macOS reference captures
// (dockui/docs/v0.2-visual-gap-report.md); 1 macOS pt == 1 DIP at the reference resolution.
#include <vector>

#include "motion.h"

namespace dock {

// Dock motion tuning, one place. Springs are (response s, damping fraction); response is the stiffness
// period, so 95% settle is about 0.75 * response for damping 1. These replace the former first-order
// exponential smoothing (tau 0.075 up / 0.13 down / 0.035 cursor). Values are tuned to the old
// settle times, not measured from macOS; dynamic macOS behaviour is not yet measured (see docs).
namespace dockmotion {
inline const docky::motion::SpringSpec HoverIn = docky::motion::SpringSpec::FromResponse(0.20, 1.0);
inline const docky::motion::SpringSpec HoverOut = docky::motion::SpringSpec::FromResponse(0.30, 1.0);
inline const docky::motion::SpringSpec Cursor = docky::motion::SpringSpec::FromResponse(0.08, 1.0);
inline const docky::motion::SpringSpec Reorder = docky::motion::SpringSpec::FromResponse(0.24, 1.0);
inline const docky::motion::SpringSpec AutoHideShow = docky::motion::SpringSpec::FromResponse(0.26, 1.0);  // old tau 0.08
inline const docky::motion::SpringSpec AutoHideHide = docky::motion::SpringSpec::FromResponse(0.34, 1.0);  // old tau 0.11
inline const docky::motion::BounceSpec Launch = {/*apex*/ 21.0, /*firstHopTime*/ 0.475, /*restitution*/ 0.6, /*minApex*/ 3.0};
}  // namespace dockmotion

struct DockMetrics {
    float icon = 46.0f;            // resting visible icon body
    float pitch = 62.0f;           // reference spacing: resting tile width (icon + 16 gap)
    float sepTile = 28.0f;         // resting separator tile (icon edge -> line -> icon edge = 42)
    float sepLineExtra = 10.0f;    // separator line height = icon + this
    float padX = 8.5f;             // plate edge to first tile edge (plate edge to icon body = 15.5)
    float padY = 15.0f;            // plate edge to icon body, top and bottom
    float radius = 22.0f;          // continuous-corner radius of the plate
    float maxScale = 2.26f;        // magnification directly under the pointer
    float rangeSlots = 4.0f;       // cosine falloff reaches zero this many resting pitches away
    float liftFactor = 0.11f;      // magnified icon bottom rises (s-1)*liftFactor*icon
    float bottomMargin = 5.5f;     // plate bottom to the window canvas edge
    float sideMargin = 24.0f;      // transparent margin on each side of the window
    float labelSpace = 50.0f;      // label pill (25) + pointer (6) + gap to icon (16) + margin
    float bounceHeight = 21.0f;    // launch bounce amplitude (dynamic behaviour not yet measured)
    float shadowPad = 6.0f;        // extra hit/visible area kept around the plate when idle
    void SetIconSize(float size) {
        const float k = size / 46.0f;
        icon = size; pitch = 62.0f * k; sepTile = 28.0f * k;
        sepLineExtra = 10.0f * k; padX = 8.5f * k; padY = 15.0f * k;
        radius = 22.0f * k;
    }
    float DockHeight() const { return icon + 2.0f * padY; }
    float Gap() const { return pitch - icon; }
};

struct ItemGeom {
    float cx = 0;        // centre x
    float left = 0;      // icon body left
    float top = 0;       // icon body top including bounce
    float restTop = 0;   // icon body top without bounce (label anchor)
    float size = 0;      // icon body size (0 for separators); the height of a wide tile
    float width = 0;     // icon body width: size x the tile's width factor (== size for ordinary icons)
    float tileLeft = 0, tileRight = 0;
    bool separator = false;
    float lineTop = 0, lineBottom = 0;  // separators only
};

struct DockFrame {
    std::vector<ItemGeom> items;
    float dockLeft = 0, dockRight = 0, dockTop = 0, dockBottom = 0;
};

struct RectF {
    float left = 0, top = 0, right = 0, bottom = 0;
};

class DockModel {
public:
    DockMetrics m;
    DockModel();

    // One entry per tile; true marks a separator. All icons are square (width factor 1).
    void SetTiles(const std::vector<bool>& separators);
    // Width factors per tile (ignored for separators): 1 = an ordinary icon, 0.5 / 1.5 / 2 ... = a wider or narrower
    // tile such as the status group. A tile rests at icon x factor + gap and magnifies as a whole like an icon.
    void SetTiles(const std::vector<bool>& separators, const std::vector<float>& widths);
    void SetIconSize(float size);
    void ResetHover();
    int Count() const { return static_cast<int>(sep_.size()); }
    float BaseWidth() const;
    int MaxIconSizeForWidth(float width) const;
    float MaxExtra() const { return maxExtra_; }
    float DesiredWindowWidth() const;
    float DesiredWindowHeight() const;
    void SetWindowSize(float width, float height);
    float WindowWidth() const { return winW_; }
    float WindowHeight() const { return winH_; }
    float BaseLeft() const;
    RectF BaseDockRect() const;

    void SetPointer(bool inside, float x, float y);
    void StartBounce(int index, double now);
    void SetReducedMotion(bool enabled);
    bool Update(double dt, double now);
    bool IsAnimating(double now) const;
    void Compute(DockFrame& out, double now) const;
    int HitTest(const DockFrame& frame, float x, float y) const;
    int HoveredIndex(const DockFrame& frame) const;
    float Hover() const { return hover_.F(); }
    bool PointerActive() const { return hover_.Target() > 0.0; }
    float Collapse(float baseX) const;  // resting x with separator gaps removed (magnification distance)
    float ScaleAt(float baseX, float h) const;  // exposed for diagnostics

private:
    void ComputeLayout(DockFrame& out, double now, float* targetCorrection) const;
    docky::motion::Spring edgeShift_;
    float Factor(int i) const { return static_cast<size_t>(i) < width_.size() ? width_[static_cast<size_t>(i)] : 1.0f; }
    float TileBase(int i) const { return sep_[i] ? m.sepTile : Factor(i) == 1.0f ? m.pitch : m.icon * Factor(i) + m.Gap(); }
    void Scales(float mouse, float h, std::vector<float>& s) const;
    float MagWidth(const std::vector<float>& s) const;
    float BounceLift(int i, double now) const;

    std::vector<bool> sep_;
    std::vector<float> width_;       // width factor per tile (1 for icons)
    std::vector<float> baseCenter_;  // resting tile centres, relative to the plate's left edge
    float maxExtra_ = 0;
    float winW_ = 0, winH_ = 0;
    bool reducedMotion_ = false;
    docky::motion::Spring hover_;   // 0..1 magnification amount, target 1 while the pointer is in the zone
    docky::motion::Spring cursor_;  // pointer x relative to the resting plate's left edge (DIP)
    float lastX_ = 0, lastY_ = 0;
    double lastNow_ = 0;
    std::vector<docky::motion::Bounce> bounce_;  // launch hop height per tile
};

}  // namespace dock
