#pragma once
// Payloads: things (file icons) that approach, fall into, or come out of the black hole. Visual only (DockDrop will
// drive it later); no file logic.
//
// A payload is a flat card in the lens plane (the plane through the hole facing the observer). The shader finds where
// each photon crosses that plane (orders 0, 1, 2 from the Schwarzschild tables), so a payload has a direct image and,
// near or behind the hole, lensed secondary images on the opposite side of the shadow.
//
//   Hover    follows a target point (the dragged file under the cursor) with a little spring lag; tidally deformed
//            (TidalStretch): a linear stretch along the radius by lambda and a squeeze across it by 1/sqrt(lambda)
//            (volume kept), growing as the tide does, ~ r^-3, so it is barely visible far out and strong only close
//            in. The shader hides everything whose light would have to come out of the black shadow.
//   Fall     a real Schwarzschild plunge of a massive particle released with E = 1 and angular momentum L < 4 (always
//            captured): dr/dt and dphi/dt in the distant observer's coordinate time, so it spirals in, slows down and
//            freezes towards r = 2M while its redshift factor g = 1 - 2/r drives it to black. Time is scaled so the
//            visible part of the plunge takes about `fallSeconds`.
//   Emit     the time reverse of a plunge that ends at the destination: the payload unwinds out of the photon ring.
//   Cancel   fades out where it is (drag left the field).
#include <algorithm>
#include <cmath>
#include <vector>

namespace docky::blackhole {

// Tidal deformation of a self-gravitating body: the tide over its self-gravity goes as (rT / r)^3, and while that is
// small the body answers with a linear (homologous) stretch. rT = 10 M at tidal = 1; capped at 2.6 (beyond that the body
// comes apart, DebrisSystem).
inline double TidalStretch(double tidal, double r) {
    const double x = 10.0 / std::max(r, 2.0);
    return std::clamp(1.0 + 0.6 * tidal * x * x * x, 1.0, 2.6);
}

struct PayloadGpu {  // shader layout: float4 (x, y, half, stretch), float4 (opacity, g, slice, shear); M units, hole-relative
    float x = 0, y = 0, half = 0, stretch = 1;
    float opacity = 0, g = 1, slice = 0, shear = 0;
};


struct PlungeSample { double t, r, phi; };

// Coordinate-time plunge from (r0, phi0), inward, E = 1, angular momentum L (sign = swirl direction).
inline std::vector<PlungeSample> Plunge(double r0, double phi0, double L, double rEnd = 2.02) {
    std::vector<PlungeSample> path;
    double r = std::max(r0, rEnd + 0.05), phi = phi0, t = 0;
    path.push_back({t, r, phi});
    auto rates = [&](double rr, double& dr, double& dp) {
        const double f = 1 - 2 / rr;
        const double V = f * (1 + L * L / (rr * rr));
        const double radial = std::sqrt(std::max(0.0, 1.0 - V));
        dr = -radial * f;            // dr/dt = (dr/dtau) / (dt/dtau), dt/dtau = E / f
        dp = L / (rr * rr) * f;
    };
    while (r > rEnd && path.size() < 200000) {
        double dr, dp; rates(r, dr, dp);
        // Step in coordinate time small enough for 0.2 % radius change (near the horizon dr/dt -> 0: freezing).
        const double h = std::clamp(0.002 * r / std::max(std::fabs(dr), 1e-9), 1e-4, 0.5);
        double dr2, dp2; rates(std::max(r + 0.5 * h * dr, rEnd), dr2, dp2);  // midpoint
        r += h * dr2; phi += h * dp2; t += h;
        path.push_back({t, r, phi});
        if (t > 1e5) break;
    }
    return path;
}

inline PlungeSample SamplePath(const std::vector<PlungeSample>& p, double t) {
    if (p.empty()) return {0, 0, 0};
    if (t <= p.front().t) return p.front();
    if (t >= p.back().t) return p.back();
    const auto it = std::lower_bound(p.begin(), p.end(), t, [](const PlungeSample& s, double v) { return s.t < v; });
    const PlungeSample& b = *it; const PlungeSample& a = *(it - 1);
    const double f = (t - a.t) / std::max(b.t - a.t, 1e-12);
    return {t, a.r + (b.r - a.r) * f, a.phi + (b.phi - a.phi) * f};
}

// Coordinate time at which the path first reaches radius r (path is monotonic in r).
inline double TimeAtRadius(const std::vector<PlungeSample>& p, double r) {
    for (size_t i = 1; i < p.size(); ++i) if (p[i].r <= r) {
        const double f = (p[i - 1].r - r) / std::max(p[i - 1].r - p[i].r, 1e-12);
        return p[i - 1].t + (p[i].t - p[i - 1].t) * f;
    }
    return p.empty() ? 0 : p.back().t;
}

class PayloadSystem {
public:
    static constexpr int kSlots = 4;
    enum class State { Off, Hover, Fall, Emit, Cancel };
    struct Tuning {
        double tidal = 0.0;            // tidal stretch of the card (off by default: the icon keeps its shape and shreds)
        double fallSeconds = 1.25;     // emit: time to come out
        double infallSeconds = 2.2;    // drop: time for the plunge to reach the horizon (slower, so the shredding reads)
        double angularMomentum = 3.8;  // < 4: always captured; close to 4 it whirls round once near r = 4M first
        bool shred = true;             // dropped cards are handed to the debris system (Handover) instead of Release
    };
    Tuning tuning;

    // Target in screen px relative to the hole centre; halfPx = half the icon size in px.
    void Hover(int slot, float dx, float dy, float halfPx, double now) {
        Item& it = items_[slot];
        if (it.state != State::Hover) {
            const bool fresh = it.state == State::Off || it.state == State::Cancel;
            it = Item{}; it.state = State::Hover; it.start = now;
            it.px = dx; it.py = dy; it.vx = it.vy = 0; it.fadeIn = fresh;
        }
        it.tx = dx; it.ty = dy; it.half = halfPx;
    }
    // Drop: plunge from where the payload is now. False when nothing hovers in that slot.
    // Drop with shredding: the hovering card (as last drawn, M) goes to the debris system, which draws it from now on.
    // vx, vy: the card's velocity (px per second, from the drag) so the drop continues its motion.
    bool Handover(int slot, PayloadGpu& card, double& vx, double& vy) {
        Item& it = items_[slot];
        if (it.state != State::Hover || !it.hasLast) return false;
        card = it.last;
        vx = it.vx; vy = it.vy;
        it.state = State::Off;
        return true;
    }
    bool Release(int slot, double now, double pxPerM) {
        Item& it = items_[slot];
        if (it.state != State::Hover || pxPerM <= 0) return false;
        const double r0 = std::max(std::hypot(it.px, it.py) / pxPerM, 2.6);
        const double phi0 = std::atan2(it.py, it.px);
        it.L = Swirl(it.px, it.py);
        it.path = Plunge(r0, phi0, it.L);
        it.timeScale = tuning.infallSeconds / std::max(TimeAtRadius(it.path, 2.35), 1e-6);
        it.state = State::Fall; it.start = now; it.swallowed = false;
        return true;
    }
    // A payload leaving: comes out of the photon ring and settles at (dx, dy) px, then fades (hand-over to the receiver).
    // assemble: the card stays invisible until it arrives (the debris system shows its pixels assembling meanwhile).
    void Emit(int slot, float dx, float dy, float halfPx, double now, double pxPerM, bool assemble = false) {
        Item& it = items_[slot];
        it = Item{}; it.state = State::Emit; it.start = now; it.half = halfPx; it.tx = dx; it.ty = dy; it.assemble = assemble;
        const double r0 = std::max(std::hypot(dx, dy) / std::max(pxPerM, 1e-3), 3.0);
        it.L = -Swirl(dx, dy);
        it.path = Plunge(r0, std::atan2(dy, dx), it.L);
        it.emitFrom = TimeAtRadius(it.path, 2.35);  // starts at the photon-ring zone, not at the frozen horizon tail
        it.timeScale = tuning.fallSeconds / std::max(it.emitFrom, 1e-6);
    }
    void Cancel(int slot, double now) { Item& it = items_[slot]; if (it.state == State::Hover || it.state == State::Emit) { it.state = State::Cancel; it.start = now; } }
    bool Active() const { for (const auto& it : items_) if (it.state != State::Off) return true; return false; }
    State StateOf(int slot) const { return items_[slot].state; }
    // Display seconds from Emit() until the payload arrives at its destination.
    double EmitDuration(int slot) const { const Item& it = items_[slot]; return it.emitFrom * it.timeScale; }

    // Advances all payloads. Returns the number written to `out`; `swallowed` counts payloads that crossed the horizon
    // this frame (the hole reacts with a Consume pulse), `emitted` those that left.
    int Update(double now, double dt, double pxPerM, PayloadGpu out[kSlots], int& swallowed, int& emitted, bool reducedMotion) {
        int n = 0; swallowed = emitted = 0;
        const double k = std::max(pxPerM, 1e-3);
        for (int s = 0; s < kSlots; ++s) {
            Item& it = items_[s];
            if (it.state == State::Off) continue;
            double x = 0, y = 0, opacity = 1;  // x, y in M
            double relax = 1;  // Emit: tidal deformation and redshift relax to none as it arrives
            const double age = now - it.start;
            switch (it.state) {
            case State::Hover: {
                if (reducedMotion) { it.px = it.tx; it.py = it.ty; }
                else {  // critically damped spring towards the cursor (omega 26/s): a slight, living lag
                    const double w = 26.0, sub = std::min(dt, 0.05);
                    const double ax = w * w * (it.tx - it.px) - 2 * w * it.vx, ay = w * w * (it.ty - it.py) - 2 * w * it.vy;
                    it.vx += ax * sub; it.vy += ay * sub; it.px += it.vx * sub; it.py += it.vy * sub;
                }
                const double r = std::max(std::hypot(it.px, it.py) / k, 2.6);
                const double pull = std::min(0.3, 0.12 * std::pow(6.0 / r, 2.0));  // drawn slightly towards the hole
                x = it.px / k * (1 - pull); y = it.py / k * (1 - pull);
                opacity = it.fadeIn && !reducedMotion ? std::min(1.0, age / 0.18) : 1.0;
                break;
            }
            case State::Fall: case State::Emit: {
                const double total = it.path.empty() ? 0 : it.path.back().t;
                double tc = reducedMotion ? total : age / it.timeScale;  // coordinate time
                if (it.state == State::Emit) tc = reducedMotion ? 0 : it.emitFrom - tc;
                const PlungeSample p = SamplePath(it.path, tc);
                x = p.r * std::cos(p.phi); y = p.r * std::sin(p.phi);
                if (it.state == State::Fall) {
                    if (tc >= total) { it.state = State::Off; continue; }
                    opacity = 1;
                    if (!it.swallowed && p.r < 2.3) { it.swallowed = true; ++swallowed; }
                } else {
                    opacity = std::min(1.0, age / 0.12);
                    const double u = std::clamp(tc / std::max(it.emitFrom, 1e-6) / 0.4, 0.0, 1.0);
                    relax = u * u * (3 - 2 * u);
                    if (tc <= 0) {  // arrived: hold briefly, then fade (the receiving UI takes over)
                        const double hold = age - it.emitFrom * it.timeScale;
                        if (hold < 0.02 && !it.counted) { it.counted = true; ++emitted; }
                        opacity = std::clamp(1.0 - (hold - 0.35) / 0.3, 0.0, 1.0);
                        if (it.assemble) opacity *= std::min(1.0, hold / 0.12);
                        if (hold > 0.65 || reducedMotion) { it.state = State::Off; continue; }
                    } else if (it.assemble && !reducedMotion) {
                        opacity = 0;  // in flight as pixels
                    }
                }
                break;
            }
            case State::Cancel: {
                x = it.px / k; y = it.py / k;
                opacity = reducedMotion ? 0 : 1 - age / 0.25;
                if (opacity <= 0) { it.state = State::Off; continue; }
                break;
            }
            default: break;
            }
            const double r = std::max(std::hypot(x, y), 2.0001);
            PayloadGpu g;
            g.x = static_cast<float>(x); g.y = static_cast<float>(y);
            g.half = static_cast<float>(it.half / k);
            g.stretch = static_cast<float>(1.0 + (TidalStretch(tuning.tidal, r) - 1.0) * relax);
            g.shear = 0;
            g.opacity = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
            const double redshift = it.state == State::Fall || it.state == State::Emit ? 1 - 2 / r : std::sqrt(1 - 2 / r);
            g.g = static_cast<float>(std::clamp(1 - (1 - redshift) * relax, 0.0, 1.0));
            g.slice = static_cast<float>(s);
            if (it.state == State::Hover) { it.last = g; it.hasLast = true; }
            out[n++] = g;
        }
        return n;
    }

private:
    // Swirl direction: keep the angular momentum's sign stable (counter-clockwise on screen), magnitude from tuning.
    double Swirl(double, double) const { return tuning.angularMomentum; }
    struct Item {
        State state = State::Off;
        double start = 0;
        double tx = 0, ty = 0, px = 0, py = 0, vx = 0, vy = 0;  // px, hole-relative
        double half = 32;
        bool fadeIn = false, swallowed = false, counted = false, assemble = false, hasLast = false;
        PayloadGpu last;  // the card as last drawn while hovering
        std::vector<PlungeSample> path;
        double timeScale = 1, emitFrom = 0, L = 0;
    };
    Item items_[kSlots];
};

}  // namespace docky::blackhole
