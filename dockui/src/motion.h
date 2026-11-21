#pragma once
// docky Motion Engine (header-only, no Windows dependency, unit-testable).
//
// What this is: a damped-harmonic-oscillator spring solved in CLOSED FORM, so a step of any dt gives
// the exact state (frame-rate independent, unconditionally stable) and a retarget keeps the current
// value AND velocity (no velocity discontinuity). A ballistic "Bounce" covers the Dock launch hop.
//
// Provenance (be careful when quoting this as "Apple's algorithm"):
//  * The (response, dampingFraction) and (duration, bounce) parameterisations follow the public
//    SwiftUI API and WWDC18 "Designing Fluid Interfaces" / WWDC23 "Animate with springs" (10158) as
//    recalled from those sessions: mass = 1, stiffness = (2*pi/duration)^2,
//    damping = 4*pi*(1-bounce)/duration for bounce >= 0 and 4*pi/(duration*(1+bounce)) for bounce < 0.
//    A web lookup in this session confirmed the two-parameter model and the -1..1 bounce range but did
//    NOT show the exact equations; they are not re-verified against the transcript.
//  * The numeric presets (Smooth/Snappy/Bouncy bounce 0 / 0.15 / 0.3, duration 0.5 s) are commonly
//    cited values; Apple's reference pages only describe them qualitatively. Treat as approximations.
//  * Apple's internal solver, additive-animation blending and per-platform tuning are NOT public;
//    nothing here claims to reproduce them. The closed-form oscillator is textbook physics.
//  * MomentumProject() is the projection function Apple published in its Fluid Interfaces sample.
#include <algorithm>
#include <array>
#include <cmath>

namespace docky::motion {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------
// Spring parameters (unit mass).
struct SpringSpec {
    double stiffness = 400.0;  // k, 1/s^2 (mass fixed at 1)
    double damping = 40.0;     // c, 1/s

    double AngularFrequency() const { return std::sqrt(std::max(stiffness, 0.0)); }
    // 1 = critically damped (no overshoot), <1 bounces, >1 over-damped.
    double DampingRatio() const {
        const double w = AngularFrequency();
        return w > 0.0 ? damping / (2.0 * w) : 1.0;
    }

    // SwiftUI .spring(response:dampingFraction:): response is the stiffness period in seconds.
    static SpringSpec FromResponse(double response, double dampingFraction) {
        response = std::max(response, 1e-3);
        const double w = 2.0 * kPi / response;
        return {w * w, 2.0 * dampingFraction * w};
    }
    // SwiftUI Spring(duration:bounce:). bounce in -1..1; 0 = smooth, >0 overshoots.
    static SpringSpec FromDuration(double duration, double bounce) {
        duration = std::max(duration, 1e-3);
        bounce = std::clamp(bounce, -1.0, 1.0);
        const double w = 2.0 * kPi / duration;
        const double zeta = bounce >= 0.0 ? 1.0 - bounce : 1.0 / (1.0 + bounce);
        return {w * w, 2.0 * zeta * w};
    }
    static SpringSpec Smooth(double duration = 0.5) { return FromDuration(duration, 0.0); }
    static SpringSpec Snappy(double duration = 0.5) { return FromDuration(duration, 0.15); }
    static SpringSpec Bouncy(double duration = 0.5) { return FromDuration(duration, 0.3); }
};

// Exact advance of x'' = -k x - c x' over t seconds. (x, v) is displacement from the target.
inline void SolveSpring(const SpringSpec& s, double& x, double& v, double t) {
    if (t <= 0.0) return;
    const double w0 = s.AngularFrequency();
    if (w0 <= 1e-9) {  // no restoring force: only damping acts, v' = -c v
        const double c = std::max(s.damping, 0.0);
        if (c <= 1e-9) { x += v * t; return; }
        const double e = std::exp(-c * t);
        x += v * (1.0 - e) / c;
        v *= e;
        return;
    }
    const double zeta = s.DampingRatio();
    const double zw = zeta * w0;
    const double e = std::exp(-zw * t);
    if (zeta < 1.0 - 1e-4) {  // under-damped
        const double wd = w0 * std::sqrt(1.0 - zeta * zeta);
        const double c = std::cos(wd * t), sn = std::sin(wd * t);
        const double A = x, B = (v + zw * x) / wd;
        x = e * (A * c + B * sn);
        v = e * ((B * wd - zw * A) * c - (A * wd + zw * B) * sn);
    } else if (zeta <= 1.0 + 1e-4) {  // critically damped
        const double A = x, B = v + w0 * x;
        x = e * (A + B * t);
        v = e * (B - w0 * (A + B * t));
    } else {  // over-damped; two decaying exponentials, no overflow for any t
        const double a = w0 * std::sqrt(zeta * zeta - 1.0);
        const double A = x, B = (v + zw * x) / a;
        const double e1 = std::exp((-zw + a) * t), e2 = std::exp((-zw - a) * t);
        x = 0.5 * ((A + B) * e1 + (A - B) * e2);
        v = 0.5 * ((A + B) * (-zw + a) * e1 + (A - B) * (-zw - a) * e2);
    }
}

// ---------------------------------------------------------------------------------------------
// One animated scalar. Retargeting keeps value and velocity (velocity inheritance).
class Spring {
public:
    Spring() = default;
    explicit Spring(double value) : value_(value), target_(value), origin_(value) {}

    void SetSpec(const SpringSpec& spec) { spec_ = spec; }
    const SpringSpec& Spec() const { return spec_; }
    // Rest thresholds in the property's own unit (DIP, px, or normalised 0..1).
    void SetRest(double position, double velocity) { restPos_ = position; restVel_ = velocity; }

    double Value() const { return value_; }
    double Velocity() const { return velocity_; }
    double Target() const { return target_; }
    float F() const { return static_cast<float>(value_); }
    bool Paused() const { return paused_; }

    // New destination; current value and velocity are preserved. No-op when unchanged so it can be
    // called every frame without resetting origin/rest.
    void SetTarget(double target) {
        if (target == target_) return;
        origin_ = target_;
        target_ = target;
        rest_ = false;
    }
    // Retarget and also replace the velocity (e.g. hand a drag-release velocity to the spring).
    void SetTarget(double target, double velocity) {
        SetTarget(target);
        velocity_ = velocity;
        rest_ = false;
    }
    void AddVelocity(double dv) { velocity_ += dv; rest_ = false; }
    // Teleport without animation (initial layout, reduced motion): value = target = v, velocity 0.
    void SnapTo(double v) { value_ = target_ = origin_ = v; velocity_ = 0.0; rest_ = true; }
    // Jump the current value (velocity 0) but keep the target, so the spring animates back to it.
    void SetValue(double v) { value_ = v; velocity_ = 0.0; rest_ = (value_ == target_); }
    // Controls.
    void Pause() { paused_ = true; }
    void Resume() { paused_ = false; }
    void Cancel() { target_ = origin_ = value_; velocity_ = 0.0; rest_ = true; }  // freeze in place
    void Finish() { SnapTo(target_); }                                           // jump to target
    void Reverse() { SetTarget(origin_); }  // head back to where this move began, velocity kept

    bool AtRest() const { return rest_; }
    bool Animating() const { return !rest_ && !paused_; }

    // Advance by dt seconds (any size; non-finite / negative dt is ignored, > 0.5 s is capped).
    // Returns true while still animating.
    bool Step(double dt) {
        if (rest_ || paused_) return !rest_ && !paused_;
        if (!(dt > 0.0) || !std::isfinite(dt)) return true;
        dt = std::min(dt, kMaxStep);
        double x = value_ - target_, v = velocity_;
        SolveSpring(spec_, x, v, dt);
        if (!std::isfinite(x) || !std::isfinite(v)) { SnapTo(target_); return false; }  // never poison state
        value_ = target_ + x;
        velocity_ = v;
        if (std::fabs(x) <= restPos_ && std::fabs(v) <= restVel_) { SnapTo(target_); return false; }
        return true;
    }

    static constexpr double kMaxStep = 0.5;

private:
    SpringSpec spec_ = SpringSpec::Smooth(0.35);
    double value_ = 0, velocity_ = 0, target_ = 0, origin_ = 0;
    double restPos_ = 1e-3, restVel_ = 1e-2;
    bool rest_ = true, paused_ = false;
};

// N properties animated together with one spec (2D position is two springs, never one distance).
template <int N>
class SpringN {
public:
    void SetSpec(const SpringSpec& s) { for (auto& c : c_) c.SetSpec(s); }
    void SetRest(double p, double v) { for (auto& c : c_) c.SetRest(p, v); }
    Spring& operator[](int i) { return c_[static_cast<size_t>(i)]; }
    const Spring& operator[](int i) const { return c_[static_cast<size_t>(i)]; }
    void SetTarget(const std::array<double, N>& t) { for (int i = 0; i < N; ++i) c_[i].SetTarget(t[i]); }
    void SnapTo(const std::array<double, N>& t) { for (int i = 0; i < N; ++i) c_[i].SnapTo(t[i]); }
    void Pause() { for (auto& c : c_) c.Pause(); }
    void Resume() { for (auto& c : c_) c.Resume(); }
    void Cancel() { for (auto& c : c_) c.Cancel(); }
    void Reverse() { for (auto& c : c_) c.Reverse(); }
    bool Step(double dt) { bool any = false; for (auto& c : c_) any |= c.Step(dt); return any; }
    bool Animating() const { for (const auto& c : c_) if (c.Animating()) return true; return false; }
private:
    std::array<Spring, N> c_{};
};

// ---------------------------------------------------------------------------------------------
// Ballistic hop for the Dock launch bounce: gravity plus restitution, solved per contact so any dt
// is exact. height >= 0 above the resting position. Re-launching while airborne keeps the current
// height (position continuous) and only restores upward speed.
struct BounceSpec {
    double apex = 21.0;         // first hop height, DIP
    double firstHopTime = 0.475;  // seconds for the first hop (up + down); sets gravity
    double restitution = 0.6;   // speed kept per impact; apex ratio is restitution^2
    double minApex = 3.0;       // stop when the next hop would be lower than this
};

class Bounce {
public:
    void Launch(const BounceSpec& s) {
        spec_ = s;
        // hop time T = 2*sqrt(2*apex/g) -> g = 8*apex/T^2 ; launch speed v0 = sqrt(2*g*apex)
        g_ = 8.0 * s.apex / (s.firstHopTime * s.firstHopTime);
        v_ = std::max(v_, std::sqrt(2.0 * g_ * s.apex));
        active_ = true;
    }
    void Cancel() { h_ = 0.0; v_ = 0.0; active_ = false; }
    bool Active() const { return active_; }
    double Height() const { return h_; }

    bool Step(double dt) {
        if (!active_) return false;
        if (!(dt > 0.0) || !std::isfinite(dt)) return true;
        dt = std::min(dt, 1.0);
        for (int guard = 0; dt > 0.0 && guard < 64; ++guard) {
            const double tHit = (v_ + std::sqrt(v_ * v_ + 2.0 * g_ * h_)) / g_;  // time until h returns to 0
            if (tHit > dt) {
                h_ += v_ * dt - 0.5 * g_ * dt * dt;
                v_ -= g_ * dt;
                dt = 0.0;
                break;
            }
            const double impact = v_ - g_ * tHit;  // negative: downward speed at the ground
            dt -= tHit;
            h_ = 0.0;
            v_ = -impact * spec_.restitution;
            if (v_ * v_ / (2.0 * g_) < spec_.minApex) { Cancel(); return false; }
        }
        return true;
    }

private:
    BounceSpec spec_{};
    double g_ = 1.0, h_ = 0.0, v_ = 0.0;
    bool active_ = false;
};

// ---------------------------------------------------------------------------------------------
// Helpers from the apple-design skill (Apple's published Fluid Interfaces sample code).
// Where a flick of `velocity` (units/s) would come to rest, as an offset from the release point.
inline double MomentumProject(double velocity, double decelerationRate = 0.998) {
    return (velocity / 1000.0) * decelerationRate / (1.0 - decelerationRate);
}
// Progressive resistance past a boundary; `overshoot` and result share a unit.
inline double Rubberband(double overshoot, double dimension, double constant = 0.55) {
    const double a = std::fabs(overshoot);
    const double r = (a * dimension * constant) / (dimension + constant * a);
    return overshoot < 0.0 ? -r : r;
}

// ---------------------------------------------------------------------------------------------
// DockOver motion tokens (1.0). Every popup, menu, panel and control uses these instead of its own numbers, so the
// whole app moves alike. Apple-style: critically damped (no bounce) for things that appear, disappear or follow the
// pointer; a small bounce only where macOS / iOS show one (switch knobs, banners, rows settling). Closing is faster
// than opening. Durations are the spring's perceptual duration (SwiftUI Spring(duration:bounce:)).
namespace tokens {
inline const SpringSpec PopoverOpen  = SpringSpec::FromDuration(0.24, 0.0);   // Control Center, Wi-Fi, sound, stacks
inline const SpringSpec PopoverClose = SpringSpec::FromDuration(0.18, 0.0);
inline const SpringSpec MenuOpen     = SpringSpec::FromDuration(0.20, 0.0);   // context menus, status menus
inline const SpringSpec MenuClose    = SpringSpec::FromDuration(0.14, 0.0);
inline const SpringSpec PanelOpen    = SpringSpec::FromDuration(0.34, 0.06);  // Notification Center sliding in
inline const SpringSpec PanelClose   = SpringSpec::FromDuration(0.24, 0.0);
inline const SpringSpec BannerIn     = SpringSpec::FromDuration(0.44, 0.14);  // notification banner from the edge
inline const SpringSpec BannerOut    = SpringSpec::FromDuration(0.30, 0.0);
inline const SpringSpec Press        = SpringSpec::FromDuration(0.18, 0.0);   // pressed state of buttons, sliders
inline const SpringSpec Toggle       = SpringSpec::FromDuration(0.38, 0.24);  // switch knob
inline const SpringSpec Tint         = SpringSpec::FromDuration(0.26, 0.0);   // colour changes
inline const SpringSpec Scroll       = SpringSpec::FromDuration(0.26, 0.0);   // programmatic scrolling
inline const SpringSpec Reflow       = SpringSpec::FromDuration(0.32, 0.10);  // rows moving to new places
inline const SpringSpec Appear       = SpringSpec::FromDuration(0.30, 0.0);   // new rows fading in
}  // namespace tokens

}  // namespace docky::motion
