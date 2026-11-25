// Physics and life-cycle checks for the Black Hole Visualizer (no GPU, no window).
#include "../src/blackhole/blackhole_anim.h"
#include "../src/blackhole/blackhole_payload.h"
#include "../src/blackhole/blackhole_debris.h"
#include "../src/blackhole/schwarzschild_lut.h"

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace docky::blackhole;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Trace Shoot(double b, double limit = 40.0) { return Integrate(b, limit, 2e-4, [](double, double) {}); }

int main() {
    // 1. Capture boundary: bisect on the integrator alone, compare with 3*sqrt(3).
    double lo = 4.0, hi = 7.0;
    for (int i = 0; i < 50; ++i) { const double mid = 0.5 * (lo + hi); (Shoot(mid).captured ? lo : hi) = mid; }
    std::printf("critical impact parameter: integrated %.6f, analytic %.6f\n", 0.5 * (lo + hi), kCriticalB);
    CHECK(std::fabs(0.5 * (lo + hi) - kCriticalB) < 1e-4, "b_c %.6f", 0.5 * (lo + hi));

    // 2. Weak field: deflection vs. the second-order series 4/b + 15 pi / (4 b^2).
    for (double b : {50.0, 100.0, 300.0}) {
        const double alpha = Shoot(b).phiEnd - kPi, series = WeakDeflection(b);
        std::printf("b=%.0f: deflection %.6f, series %.6f\n", b, alpha, series);
        CHECK(std::fabs(alpha - series) / series < 0.01, "weak field b=%.0f", b);
    }
    // 3. Strong field: deflection grows like -log(b - b_c) (Bozza 2002: slope 1 in units of M for Schwarzschild).
    const double a1 = Shoot(kCriticalB + 1e-3).phiEnd - kPi, a2 = Shoot(kCriticalB + 1e-5).phiEnd - kPi;
    std::printf("strong field: alpha(b_c+1e-3) %.4f, alpha(b_c+1e-5) %.4f, slope %.4f per ln (expected 1.0)\n", a1, a2, (a2 - a1) / std::log(100.0));
    CHECK(std::fabs((a2 - a1) / std::log(100.0) - 1.0) < 0.02, "log divergence slope");
    // Photon passing at the photon sphere loops: b slightly above b_c gives more than one full turn.
    CHECK(a2 > 2 * kPi, "relativistic images need alpha > 2 pi near b_c (got %.3f)", a2);

    // 4. Column mapping round trip and LUT contents.
    const LutSpec spec;
    for (double b : {0.5, 3.0, 5.0, 5.19, 5.2, 6.0, 20.0, 399.0}) {
        const double back = SToB(BToS(b, spec), spec);
        CHECK(std::fabs(back - b) < 1e-9 * std::max(1.0, b), "mapping b=%.3f -> %.12f", b, back);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const Lut lut = BuildLut(spec);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("LUT %dx%d built in %.1f ms\n", spec.columns, spec.rows, ms);
    const int half = spec.columns / 2;
    CHECK(SToB((half - 0.5) / spec.columns, spec) < kCriticalB && SToB((half + 0.5) / spec.columns, spec) > kCriticalB, "capture boundary between columns");
    // Captured columns end at the horizon; escaping columns end beyond pi.
    for (int c = 0; c < spec.columns; c += 37) {
        const double b = SToB((c + 0.5) / spec.columns, spec);
        if (b > kCriticalB) CHECK(lut.phiEnd[c] > kPi, "escape column %d phiEnd %.3f", c, lut.phiEnd[c]);
        else CHECK(lut.phiEnd[c] > 0 && lut.phiEnd[c] < spec.phiMax + 1, "capture column %d phiEnd %.3f", c, lut.phiEnd[c]);
    }
    // Closest approach of an escaping photon: u_max solves 1/b^2 = u^2 - 2u^3 (turning point); check one column.
    {
        const int c = half + spec.columns / 4;
        const double b = SToB((c + 0.5) / spec.columns, spec);
        double umax = 0; for (int r = 0; r < spec.rows; ++r) umax = std::max(umax, static_cast<double>(lut.u[static_cast<size_t>(r) * spec.columns + c]));
        const double residual = umax * umax - 2 * umax * umax * umax - 1 / (b * b);
        std::printf("turning point b=%.3f: r_min=%.4f, residual %.2e\n", b, 1 / umax, residual);
        CHECK(std::fabs(residual) < 1e-4 / (b * b) + 1e-6, "turning point residual %.3e", residual);
    }
    // Disk image of order 1 exists: for b just above b_c the photon still exists at phi = pi + pi/2.
    CHECK(lut.phiEnd[half + 2] > 1.5 * kPi, "order-1 image angle reachable");

    // 5. Life cycle: Spawn -> Idle, Consume -> Idle, Collapse -> Hidden; reduced motion completes at once.
    HoleAnimator a;
    int finished = 0; a.onFinished = [&](HolePhase) { ++finished; };
    a.Spawn(0); CHECK(a.Update(0).mass < 0.05f, "spawn starts small");
    a.Update(HoleAnimator::kSpawn + 0.01); CHECK(a.Phase() == HolePhase::Idle, "spawn ends in idle");
    a.Attract(10, 300, 0); const HoleVisual at = a.Update(11);
    CHECK(at.leanX == 0 && at.mass == 1.f && at.lens == 1.f && at.spin == 1.f, "a file does not change the hole (attract)");
    a.Consume(12);
    for (double tt = 12; tt < 12 + HoleAnimator::kConsume; tt += 0.05) {
        const HoleVisual cv = a.Update(tt);
        CHECK(cv.mass == 1.f && cv.lens == 1.f && cv.disk == 1.f && cv.spin == 1.f, "a file does not change the hole (consume)");
    }
    a.Update(12 + HoleAnimator::kConsume + 0.01); CHECK(a.Phase() == HolePhase::Idle, "consume returns to idle");
    a.Emit(20); { const HoleVisual ev = a.Update(20.3); CHECK(ev.ringFlash == 0.f && ev.mass == 1.f, "a file does not change the hole (emit)"); }
    a.Collapse(30); a.Update(30 + HoleAnimator::kCollapse + 0.01); CHECK(a.Phase() == HolePhase::Hidden && a.Update(31).mass == 0.f, "collapse hides");
    HoleAnimator r; r.SetReducedMotion(true); r.Spawn(0); const HoleVisual rv = r.Update(0);
    CHECK(r.Phase() == HolePhase::Idle && rv.spin == 0.f && rv.mass > 0.99f, "reduced motion: immediate, no spin");
    std::printf("finished callbacks: %d\n", finished);
    CHECK(finished == 3, "finished callbacks %d", finished);  // spawn, consume, collapse (emit was interrupted)

    // 6. Payload plunge: E = 1, L = 3.3 < 4 is always captured; the radius decreases monotonically and the distant
    //    observer's coordinate time stretches near the horizon (freezing).
    {
        const auto path = Plunge(20.0, 0.0, 3.3);
        bool monotonic = true; for (size_t k = 1; k < path.size(); ++k) monotonic = monotonic && path[k].r <= path[k - 1].r;
        const double tAt3 = TimeAtRadius(path, 3.0), tAt21 = TimeAtRadius(path, 2.1), tAt203 = TimeAtRadius(path, 2.03);
        std::printf("plunge: %zu steps, swept %.2f rad, t(r=3) %.1f, t(r=2.1) %.1f, t(r=2.03) %.1f M\n", path.size(), path.back().phi, tAt3, tAt21, tAt203);
        CHECK(monotonic && path.back().r <= 2.03, "plunge reaches the horizon monotonically");
        CHECK(path.back().phi > 0.5, "plunge swirls (phi %.3f)", path.back().phi);
        CHECK(tAt203 - tAt21 > (tAt21 - tAt3) * 0.3, "coordinate time stretches near the horizon (freezing)");
        // Free fall from rest at infinity (L = 0): dr/dt = -(1 - 2/r) sqrt(2/r).
        const auto radial = Plunge(10.0, 0.0, 0.0);
        const double t1 = TimeAtRadius(radial, 8.0), t2 = TimeAtRadius(radial, 7.9);
        const double expected = 0.1 / ((1 - 2 / 7.95) * std::sqrt(2 / 7.95));
        std::printf("radial infall dt(8 -> 7.9) %.4f, analytic %.4f\n", t2 - t1, expected);
        CHECK(std::fabs((t2 - t1) - expected) / expected < 0.01, "radial infall rate");
    }
    {
        PayloadSystem ps;
        PayloadGpu out[PayloadSystem::kSlots]; int sw = 0, em = 0;
        ps.tuning.tidal = 1;  // stretch is optional (off by default)
        ps.Hover(0, 200, 0, 30, 0);
        for (int k = 1; k <= 60; ++k) ps.Update(k / 60.0, 1 / 60.0, 10.0, out, sw, em, false);
        const float farStretch = out[0].stretch;
        ps.Hover(0, 70, 0, 30, 1.0);
        for (int k = 61; k <= 120; ++k) ps.Update(k / 60.0, 1 / 60.0, 10.0, out, sw, em, false);
        std::printf("tidal stretch: r=20M %.3f, r~7M %.3f\n", farStretch, out[0].stretch);
        CHECK(out[0].stretch > farStretch * 2.0f && farStretch < 1.6f, "closer is more stretched");
        CHECK(ps.Release(0, 2.0, 10.0), "release");
        int swallowed = 0; double t = 2.0; int n = 1;
        while (t < 8 && n > 0) { t += 1 / 120.0; n = ps.Update(t, 1 / 120.0, 10.0, out, sw, em, false); swallowed += sw; }
        std::printf("fall finished after %.2f s, swallowed %d\n", t - 2.0, swallowed);
        CHECK(swallowed == 1 && n == 0 && t - 2.0 < 6.0, "falls in once");
        ps.Emit(1, -150, 40, 30, 10.0, 10.0);
        int emittedCount = 0; t = 10.0; n = 1;
        float arriveStretch = -1, arriveShear = -1;
        while (t < 16 && n > 0) { t += 1 / 120.0; n = ps.Update(t, 1 / 120.0, 10.0, out, sw, em, false); emittedCount += em; if (em && n > 0) { arriveStretch = out[0].stretch; arriveShear = out[0].shear; } }
        CHECK(arriveStretch == 1.0f && arriveShear == 0.0f, "emitted icon arrives undeformed (stretch %.3f, shear %.3f)", arriveStretch, arriveShear);
        std::printf("emit finished after %.2f s\n", t - 10.0);
        CHECK(emittedCount == 1 && n == 0, "emit arrives and hands over");
    }

    // 7. Tidal shredding: the dropped icon falls freely from where the card was, with its velocity, and breaks under the
    //    tide into ever smaller fragments; it is drawn out along the radius; all of it ends at the horizon, without jumps.
    {
        std::vector<uint32_t> icon(256 * 256, 0xFFE0A020u);  // opaque orange square
        DebrisSystem debris; debris.SetIcon(0, icon.data());
        PayloadSystem ps;
        PayloadGpu out[PayloadSystem::kSlots]; int sw = 0, em = 0;
        ps.tuning.tidal = 3;
        ps.Hover(0, 300, -90, 40, 0);
        for (int k = 1; k <= 90; ++k) ps.Update(k / 60.0, 1 / 60.0, 15.0, out, sw, em, false);
        PayloadGpu card; double hvx = 0, hvy = 0;
        CHECK(ps.Handover(0, card, hvx, hvy) && ps.StateOf(0) == PayloadSystem::State::Off, "hovering card handed over on drop");
        const double tDrop = 1.5;
        debris.Drop(0, card, hvx / 15.0, hvy / 15.0, tDrop, 3.0);
        std::vector<DebrisGpu> parts;
        int pulses = debris.Update(tDrop, 0, 26.0, parts);
        double r0 = std::hypot(card.x, card.y);
        const double ext0 = debris.RadialExtent(0), cross0 = debris.CrossExtent(0);
        // The first frame after release: the pixels have barely moved (they start from the card's own motion).
        pulses += debris.Update(tDrop + 1 / 120.0, 1 / 120.0, 26.0, parts);
        const double firstStep = debris.MaxStep();
        int fragments = 0;
        double t = tDrop + 1 / 120.0, worstStep = 0, lastStep = firstStep, worstJerk = 0, extAtFirst = 0, crossAtFirst = 0, firstGone = -1;
        while (t < tDrop + 12) {
            t += 1 / 120.0;
            pulses += debris.Update(t, 1 / 120.0, 26.0, parts);
            worstStep = std::max(worstStep, debris.MaxStep());
            worstJerk = std::max(worstJerk, std::fabs(debris.MaxStep() - lastStep));
            lastStep = debris.MaxStep();
            if (firstGone < 0 && debris.Swallowed(0) > 0) { firstGone = t - tDrop; extAtFirst = debris.RadialExtent(0); crossAtFirst = debris.CrossExtent(0); fragments = debris.Fragments(0); }
            if (!debris.Active()) break;
        }
        std::printf("spaghetti: r0 %.1f M, pixels %d, first step %.4f M, radial extent %.2f -> %.2f M, width %.2f -> %.2f M at first swallow (%.2f s), worst step %.3f M, worst step change %.4f M, all gone %.2f s, swallowed %d escaped %d, pulses %d\n",
                    r0, debris.Emitted(0), firstStep, ext0, extAtFirst, cross0, crossAtFirst, firstGone, worstStep, worstJerk, t - tDrop,
                    debris.Swallowed(0), debris.Escaped(0), pulses);
        CHECK(debris.Emitted(0) > 3000, "every pixel of the icon falls (%d)", debris.Emitted(0));
        CHECK(firstStep < 0.02, "no jump on release (%.4f M in the first frame)", firstStep);
        CHECK(extAtFirst > ext0 * 1.35, "the tide draws it out along the radius (%.2f -> %.2f M)", ext0, extAtFirst);
        std::printf("shredded into %d fragments before the first bit reaches the horizon\n", fragments);
        CHECK(fragments > 200, "it is shredded on the way in (%d fragments)", fragments);
        CHECK(worstStep < 1.0 && worstJerk < 0.1, "smooth: no jumps (%.3f M per frame, change %.4f)", worstStep, worstJerk);
        CHECK(debris.Swallowed(0) == debris.Emitted(0), "released from rest, all of it falls in");
        CHECK(!debris.Active() && t - tDrop < 9.0, "all gone (%.2f s)", t - tDrop);
        CHECK(pulses == 1, "one consume pulse");
        double qx, qy; PayloadGpu c; c.x = 9; c.y = -3; c.half = 2; c.stretch = 3;
        CardForward(c, 0, 0, qx, qy);
        CHECK(std::hypot(qx - c.x, qy - c.y) < 1e-6, "card centre maps to itself");
        {   // linear, volume-kept: a corner offset (1, 0) along the radius goes to lam, across to 1/sqrt(lam)
            const double L0 = std::hypot(9.0, 3.0), urx = -9 / L0, ury = 3 / L0;
            CardForward(c, urx, ury, qx, qy);
            CHECK(std::fabs(std::hypot(qx - c.x, qy - c.y) - 3.0) < 1e-6, "radial stretch is lam");
            CardForward(c, -ury, urx, qx, qy);
            CHECK(std::fabs(std::hypot(qx - c.x, qy - c.y) - 1 / std::sqrt(3.0)) < 1e-6, "squeeze is 1/sqrt(lam)");
        }
        // Assemble: particles land on the icon cells at the arrival time.
        debris.SetIcon(1, icon.data());
        debris.Assemble(1, -12, 5, 2.5, 0, 1.2);
        debris.Update(1.2, 1 / 60.0, 30.0, parts);
        double worst = 0;
        for (const auto& q : parts) worst = std::max(worst, std::fabs(q.x + 12.0) - 2.5 + std::fabs(q.y - 5.0) - 2.5);
        CHECK(!parts.empty() && worst < 0.2, "assembled pixels sit on the icon (%.3f)", worst);
    }

    std::printf(failures ? "%d FAILURE(S)\n" : "all black hole checks passed\n", failures);
    return failures ? 1 : 0;
}
