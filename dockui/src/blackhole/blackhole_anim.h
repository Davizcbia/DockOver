#pragma once
// Visual life cycle of the black hole, reserved for DockDrop (cross-device file transfer). This stage implements the
// visuals only: no networking, discovery or file logic lives here. A future DockDrop controller calls these entry
// points and reads Update() once per rendered frame; the renderer turns the returned HoleVisual into shader constants.
//
//   Spawn    the hole condenses out of nothing (mass 0 -> 1, lensing fades in)
//   Idle     steady state, accretion disk turning
//   Attract  a payload approaches     } A file's mass is nothing next to the hole's: size, lensing and spin stay
//   Consume  the payload falls in      } exactly as in Idle (these phases only report the life cycle to DockDrop;
//   Emit     a payload leaves          } Consume / Emit return to Idle on their own).
//   Collapse the hole evaporates (mass -> 0), then Hidden
//
// Reduced motion (Windows "Animation effects" off): every transition completes immediately and the disk stops
// turning; the end states are the same.
#include <algorithm>
#include <cmath>
#include <functional>

namespace docky::blackhole {

enum class HolePhase { Hidden, Spawn, Idle, Attract, Consume, Emit, Collapse };

inline const wchar_t* PhaseName(HolePhase p) {
    switch (p) {
    case HolePhase::Hidden: return L"Hidden";
    case HolePhase::Spawn: return L"Spawn";
    case HolePhase::Idle: return L"Idle";
    case HolePhase::Attract: return L"Attract";
    case HolePhase::Consume: return L"Consume";
    case HolePhase::Emit: return L"Emit";
    case HolePhase::Collapse: return L"Collapse";
    }
    return L"?";
}

// Multipliers applied on top of the user's parameters.
struct HoleVisual {
    float mass = 0.f;          // scales the hole's size (px per M); 0 = nothing drawn
    float lens = 1.f;          // scales the lens strength (source-plane distance)
    float spin = 1.f;          // scales the disk's rotation speed
    float disk = 1.f;          // disk brightness
    float ringFlash = 0.f;     // extra brightness of higher-order (photon-ring) images, 0..1
    float opacity = 0.f;       // overall opacity of the overlay
    float leanX = 0.f, leanY = 0.f;  // screen-px offset of the hole towards an attract target
};

class HoleAnimator {
public:
    // Durations in seconds.
    static constexpr double kSpawn = 0.9, kAttract = 0.6, kConsume = 0.75, kEmit = 0.8, kCollapse = 0.7;

    void SetReducedMotion(bool reduced) { reduced_ = reduced; }
    bool ReducedMotion() const { return reduced_; }
    HolePhase Phase() const { return phase_; }
    // Called when a phase finishes on its own (Spawn -> Idle, Consume -> Idle, Emit -> Idle, Collapse -> Hidden).
    std::function<void(HolePhase finished)> onFinished;

    void Spawn(double now) { Enter(HolePhase::Spawn, now); }
    void Idle(double now) { Enter(HolePhase::Idle, now); }
    // target: screen px of the approaching payload, relative to the hole centre.
    void Attract(double now, float targetDx, float targetDy) { targetX_ = targetDx; targetY_ = targetDy; Enter(HolePhase::Attract, now); }
    // Moves the attract target without restarting the phase (a payload following the cursor).
    void SetAttractTarget(float targetDx, float targetDy) { targetX_ = targetDx; targetY_ = targetDy; }
    void Consume(double now) { Enter(HolePhase::Consume, now); }
    void Emit(double now) { Enter(HolePhase::Emit, now); }
    void Collapse(double now) { Enter(HolePhase::Collapse, now); }

    HoleVisual Update(double now) {
        const double t = now - start_;
        HoleVisual v = Steady();
        auto done = [&](double duration) { return reduced_ || t >= duration; };
        auto p = [&](double duration) { return reduced_ ? 1.0 : std::clamp(t / duration, 0.0, 1.0); };
        switch (phase_) {
        case HolePhase::Hidden: v = HoleVisual{}; break;
        case HolePhase::Spawn: {
            const double k = p(kSpawn);
            v.mass = static_cast<float>(EaseOutCubic(k));  // no overshoot
            v.lens = static_cast<float>(EaseOutCubic(k));
            v.opacity = static_cast<float>(std::min(1.0, k * 2.5));
            v.disk = static_cast<float>(EaseOutCubic(std::clamp(k * 1.4 - 0.4, 0.0, 1.0)));
            if (done(kSpawn)) Finish(HolePhase::Idle, now);
            break;
        }
        case HolePhase::Idle: break;
        case HolePhase::Attract: break;  // holds until Consume / Idle / Collapse
        case HolePhase::Consume: if (done(kConsume)) Finish(HolePhase::Idle, now); break;
        case HolePhase::Emit: if (done(kEmit)) Finish(HolePhase::Idle, now); break;
        case HolePhase::Collapse: {
            const double k = p(kCollapse);
            const double m = 1.0 - EaseInCubic(k);
            v.mass = static_cast<float>(m);
            v.lens = static_cast<float>(m);
            v.disk = static_cast<float>(std::max(0.0, 1.0 - k * 1.6));
            v.opacity = static_cast<float>(std::min(1.0, (1.0 - k) * 3.0));
            if (done(kCollapse)) Finish(HolePhase::Hidden, now);
            break;
        }
        }
        if (reduced_) v.spin = 0.f;
        // Phase changes never jump: blend from the state shown when the phase began (180 ms, ease-out). Spawn starts
        // from nothing and Hidden ends there, so they need no blend.
        const double bt = now - blendStart_;
        if (!reduced_ && bt < kBlend && phase_ != HolePhase::Hidden) {
            const float w = static_cast<float>(EaseOutCubic(std::clamp(bt / kBlend, 0.0, 1.0)));
            auto mix = [&](float a, float b) { return a + (b - a) * w; };
            v.mass = mix(from_.mass, v.mass); v.lens = mix(from_.lens, v.lens); v.spin = mix(from_.spin, v.spin);
            v.disk = mix(from_.disk, v.disk); v.opacity = mix(from_.opacity, v.opacity);
            v.leanX = mix(from_.leanX, v.leanX); v.leanY = mix(from_.leanY, v.leanY);
        }
        last_ = v;
        return v;
    }
    const HoleVisual& Last() const { return last_; }

private:
    static double EaseOutCubic(double x) { const double a = 1.0 - x; return 1.0 - a * a * a; }
    static double EaseInCubic(double x) { return x * x * x; }
    static HoleVisual Steady() { HoleVisual v; v.mass = 1.f; v.opacity = 1.f; return v; }
    static constexpr double kBlend = 0.18;
    void Enter(HolePhase p, double now) {
        // A phase change in the middle of another blends from what is on screen (Spawn from nothing).
        if (p != HolePhase::Spawn && phase_ != HolePhase::Hidden) { from_ = last_; blendStart_ = now; }
        else blendStart_ = -1e9;
        phase_ = p; start_ = now;
    }
    void Finish(HolePhase next, double now) {
        const HolePhase finished = phase_;
        Enter(next, now);
        if (onFinished) onFinished(finished);
    }
    HolePhase phase_ = HolePhase::Hidden;
    double start_ = 0;
    float targetX_ = 0, targetY_ = 0;
    bool reduced_ = false;
    HoleVisual last_{}, from_{};
    double blendStart_ = -1e9;
};

}  // namespace docky::blackhole
