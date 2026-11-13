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
    float bottomMargin = 5.5f;     // plate bottom to work-area bottom
    float sideMargin = 24.0f;      // transparent margin on each side of the window
    float labelSpace = 50.0f;      // label pill (25) + pointer (6) + gap to icon (16) + margin
    float bounceHeight = 21.0f;    // launch bounce amplitude (dynamic behaviour not yet measured)
    float shadowPad = 6.0f;        // extra hit/visible area kept around the plate when idle
    float DockHeight() const { return icon + 2.0f * padY; }
    float Gap() const { return pitch - icon; }
};

struct ItemGeom {
    float cx = 0;        // centre x
    float left = 0;      // icon body left
    float top = 0;       // icon body top including bounce
    float restTop = 0;   // icon body top without bounce (label anchor)
    float size = 0;      // icon body size (0 for separators)
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

    // One entry per tile; true marks a separator.
    void SetTiles(const std::vector<bool>& separators);
    int Count() const { return static_cast<int>(sep_.size()); }
    float BaseWidth() const;
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
    float TileBase(int i) const { return sep_[i] ? m.sepTile : m.pitch; }
    void Scales(float mouse, float h, std::vector<float>& s) const;
    float MagWidth(const std::vector<float>& s) const;
    float BounceLift(int i, double now) const;

    std::vector<bool> sep_;
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
