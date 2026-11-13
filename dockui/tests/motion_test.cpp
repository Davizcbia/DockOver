// Motion Engine tests: exactness, stability, dt invariance, continuity on retarget, controls, bounce.
#include "motion.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace docky::motion;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static SpringSpec SpecFor(double zeta, double w0 = 20.0) { return {w0 * w0, 2.0 * zeta * w0}; }

// Independent reference: tiny-step semi-implicit Euler (does not share code with the closed form).
static void Reference(const SpringSpec& s, double& x, double& v, double t) {
    const double h = 1e-6;
    const int n = static_cast<int>(t / h);
    for (int i = 0; i < n; ++i) { v += (-s.stiffness * x - s.damping * v) * h; x += v * h; }
}

static Spring Fine(const SpringSpec& s, double from, double to) {
    Spring sp(from); sp.SetSpec(s); sp.SetRest(1e-12, 1e-12); sp.SetTarget(to); return sp;
}

int main() {
    // 1. Closed form matches brute-force integration in under-, critically and over-damped regimes.
    for (double zeta : {0.3, 0.8, 1.0, 1.7, 6.0}) {
        const SpringSpec s = SpecFor(zeta);
        double x = 1.0, v = -3.0, rx = 1.0, rv = -3.0;
        SolveSpring(s, x, v, 0.25);
        Reference(s, rx, rv, 0.25);
        CHECK(std::fabs(x - rx) < 2e-4 && std::fabs(v - rv) < 2e-3, "zeta %.2f x %.6f vs %.6f v %.5f vs %.5f", zeta, x, rx, v, rv);
    }

    // 2. Frame-rate independence: one 0.2 s step == 240 steps == uneven steps.
    for (double zeta : {0.4, 1.0, 2.5}) {
        const SpringSpec s = SpecFor(zeta);
        Spring a = Fine(s, 0, 100), b = Fine(s, 0, 100), c = Fine(s, 0, 100);
        a.Step(0.2);
        for (int i = 0; i < 240; ++i) b.Step(0.2 / 240.0);
        const double uneven[] = {0.013, 0.0071, 0.05, 0.0009, 0.1290};  // sums to 0.2
        for (double d : uneven) c.Step(d);
        CHECK(std::fabs(a.Value() - b.Value()) < 1e-9 && std::fabs(a.Value() - c.Value()) < 1e-9, "zeta %.1f %.12f %.12f %.12f", zeta, a.Value(), b.Value(), c.Value());
        CHECK(std::fabs(a.Velocity() - b.Velocity()) < 1e-7, "velocity dt-invariance");
    }

    // 3. Retarget keeps value AND velocity, and motion stays continuous afterwards.
    {
        Spring sp = Fine(SpecFor(0.8), 0, 100);
        sp.Step(0.06);
        const double v0 = sp.Value(), vel0 = sp.Velocity();
        CHECK(vel0 > 1.0, "should be moving, vel %.3f", vel0);
        sp.SetTarget(-50);
        CHECK(sp.Value() == v0 && sp.Velocity() == vel0, "retarget must not touch value/velocity");
        const double dt = 1.0 / 1000.0;
        const SpringSpec sa = sp.Spec();
        const double accel = -sa.stiffness * (v0 - sp.Target()) - sa.damping * vel0;  // finite force, not a velocity reset
        sp.Step(dt);
        CHECK(std::fabs(sp.Value() - v0) < std::fabs(vel0) * dt * 1.5 + std::fabs(accel) * dt * dt + 1e-6, "no positional jump after retarget");
        CHECK(std::fabs(sp.Velocity() - vel0) < std::fabs(accel) * dt * 1.1 + 1e-6, "velocity changes only by the spring force over dt (dv %.4f, a*dt %.4f)", sp.Velocity() - vel0, accel * dt);
        // Rapid retargeting every frame (mouse spam) stays finite and bounded.
        Spring cur = Fine(SpringSpec::FromResponse(0.08, 1.0), 0, 0);
        double maxStep = 0;
        double last = 0;
        for (int i = 0; i < 2400; ++i) {
            cur.SetTarget(200.0 * std::sin(i * 0.01) + (i % 7) * 3.0);
            cur.Step(1.0 / 240.0);
            maxStep = std::max(maxStep, std::fabs(cur.Value() - last));
            last = cur.Value();
            CHECK(std::isfinite(cur.Value()), "finite under retarget spam");
        }
        CHECK(maxStep < 30.0, "max per-frame step %.2f", maxStep);
    }

    // 4. Robustness: bad dt values never corrupt state; huge dt settles.
    {
        Spring sp = Fine(SpringSpec::FromResponse(0.2, 1.0), 0, 10);
        sp.Step(std::numeric_limits<double>::quiet_NaN());
        sp.Step(-1.0);
        sp.Step(std::numeric_limits<double>::infinity());  // not finite: ignored
        CHECK(sp.Value() == 0.0 && sp.Velocity() == 0.0, "invalid dt ignored");
        sp.Step(10.0);  // capped at kMaxStep, exact solution => essentially settled
        CHECK(std::isfinite(sp.Value()) && std::fabs(sp.Value() - 10.0) < 0.1, "after stall %.4f", sp.Value());
        Spring stiff(0.0); stiff.SetSpec({1e6, 2.0 * 50.0 * 1000.0}); stiff.SetTarget(1.0);
        stiff.Step(0.5);
        CHECK(std::isfinite(stiff.Value()) && std::isfinite(stiff.Velocity()), "stiff over-damped finite");
        Spring zero(0.0); zero.SetSpec({0.0, 0.0}); zero.SetTarget(1.0); zero.AddVelocity(2.0); zero.Step(0.1);
        CHECK(std::isfinite(zero.Value()), "zero stiffness finite");
    }

    // 5. Rest detection snaps exactly and re-arms on a new target.
    {
        Spring sp(0.0); sp.SetSpec(SpringSpec::FromResponse(0.15, 1.0)); sp.SetTarget(1.0);
        int frames = 0;
        while (sp.Step(1.0 / 240.0) && frames < 100000) ++frames;
        CHECK(sp.AtRest() && sp.Value() == 1.0 && sp.Velocity() == 0.0, "settled exactly, %d frames", frames);
        CHECK(frames < 240, "critically damped 0.15 s response settles in under a second, took %d frames", frames);
        sp.SetTarget(0.0);
        CHECK(!sp.AtRest() && sp.Animating(), "new target re-arms");
    }

    // 6. Damping behaviour: critical never overshoots, under-damped does and settles.
    {
        Spring crit = Fine(SpringSpec::FromResponse(0.3, 1.0), 0, 1);
        Spring bounce = Fine(SpringSpec::FromResponse(0.3, 0.5), 0, 1);
        double maxCrit = 0, maxBounce = 0;
        for (int i = 0; i < 600; ++i) { crit.Step(1.0 / 240); bounce.Step(1.0 / 240); maxCrit = std::max(maxCrit, crit.Value()); maxBounce = std::max(maxBounce, bounce.Value()); }
        CHECK(maxCrit <= 1.0 + 1e-9, "critical overshoot %.6f", maxCrit);
        CHECK(maxBounce > 1.1 && std::fabs(bounce.Value() - 1.0) < 1e-3, "bouncy peak %.3f end %.5f", maxBounce, bounce.Value());
    }

    // 7. Reverse mid-flight returns to the origin and keeps the velocity at the moment of reversal.
    {
        Spring sp = Fine(SpringSpec::FromResponse(0.3, 0.9), 0, 1);
        sp.Step(0.08);
        const double vel = sp.Velocity();
        sp.Reverse();
        CHECK(sp.Target() == 0.0 && sp.Velocity() == vel && sp.Value() > 0.0, "reverse keeps velocity");
        for (int i = 0; i < 480; ++i) sp.Step(1.0 / 240);
        CHECK(std::fabs(sp.Value()) < 1e-3, "returned to origin %.5f", sp.Value());
    }

    // 8. Pause / resume / cancel / finish.
    {
        Spring sp = Fine(SpringSpec::FromResponse(0.3, 1.0), 0, 1);
        sp.Step(0.05);
        const double v = sp.Value();
        sp.Pause();
        for (int i = 0; i < 100; ++i) sp.Step(1.0 / 60);
        CHECK(sp.Value() == v && sp.Paused() && !sp.Animating(), "paused holds state");
        sp.Resume();
        sp.Step(0.05);
        CHECK(sp.Value() > v, "resumed moves on");
        sp.Cancel();
        const double frozen = sp.Value();
        CHECK(sp.AtRest() && sp.Velocity() == 0.0 && sp.Target() == frozen, "cancel freezes in place");
        sp.SetTarget(5.0); sp.Finish();
        CHECK(sp.Value() == 5.0 && sp.AtRest(), "finish snaps to target");
    }

    // 9. Retargets at aligned times give the same result at any frame rate (decoupled from FPS).
    {
        auto run = [](int stepsPerSegment) {
            Spring sp = Fine(SpringSpec::FromResponse(0.25, 0.8), 0, 0);
            const double targets[] = {100, -40, 70};
            for (double t : targets) { sp.SetTarget(t); for (int i = 0; i < stepsPerSegment; ++i) sp.Step(0.1 / stepsPerSegment); }
            return sp.Value();
        };
        const double a = run(1), b = run(6), c = run(24), d = run(100);  // ~10 / 60 / 240 / 1000 fps equivalents
        CHECK(std::fabs(a - b) < 1e-8 && std::fabs(a - c) < 1e-8 && std::fabs(a - d) < 1e-8, "%.9f %.9f %.9f %.9f", a, b, c, d);
    }

    // 10. Parameter conversions (SwiftUI public parameterisation).
    {
        const SpringSpec r = SpringSpec::FromResponse(0.4, 1.0);
        CHECK(std::fabs(r.stiffness - std::pow(2 * kPi / 0.4, 2)) < 1e-9 && std::fabs(r.DampingRatio() - 1.0) < 1e-12, "response spec");
        CHECK(std::fabs(SpringSpec::FromDuration(0.5, 0.3).DampingRatio() - 0.7) < 1e-12, "bounce 0.3 -> zeta 0.7");
        CHECK(std::fabs(SpringSpec::FromDuration(0.5, -0.5).DampingRatio() - 2.0) < 1e-12, "bounce -0.5 -> zeta 2");
        CHECK(SpringSpec::Smooth().DampingRatio() == 1.0 && SpringSpec::Snappy().DampingRatio() < 1.0 && SpringSpec::Bouncy().DampingRatio() < SpringSpec::Snappy().DampingRatio(), "preset ordering");
    }

    // 11. Multi-property spring.
    {
        SpringN<2> p; p.SetSpec(SpringSpec::FromResponse(0.2, 1.0)); p.SetRest(1e-4, 1e-3);
        p.SnapTo({0, 0}); p.SetTarget({10, -4});
        int n = 0; while (p.Step(1.0 / 240) && n < 100000) ++n;
        CHECK(p[0].Value() == 10 && p[1].Value() == -4 && !p.Animating(), "both axes settle independently");
    }

    // 12. Bounce: apex, exact timing independent of dt, interruption continuity.
    {
        BounceSpec bs;
        auto run = [&](double dt, double& apex, double& endTime) {
            Bounce b; b.Launch(bs); apex = 0; endTime = 0;
            for (double t = 0; t < 5.0 && b.Active(); t += dt) { b.Step(dt); apex = std::max(apex, b.Height()); endTime = t + dt; }
        };
        double a1, e1, a2, e2, a3, e3;
        run(1.0 / 60, a1, e1); run(1.0 / 240, a2, e2); run(1.0 / 2000, a3, e3);
        CHECK(std::fabs(a3 - 21.0) < 0.1, "apex %.3f", a3);
        CHECK(std::fabs(e1 - e3) < 0.02 && std::fabs(e2 - e3) < 0.005, "end time %.4f %.4f %.4f", e1, e2, e3);
        CHECK(e3 > 0.6 && e3 < 1.0, "two short hops total %.3f s", e3);
        Bounce b; b.Launch(bs);
        for (int i = 0; i < 60; ++i) b.Step(1.0 / 240);  // falling or rising mid hop
        const double h = b.Height();
        b.Launch(bs);  // relaunch while airborne
        CHECK(b.Height() == h, "relaunch keeps height");
        b.Step(1.0 / 240);
        CHECK(std::fabs(b.Height() - h) < 5.0 && b.Height() >= 0.0, "continuous after relaunch");
        b.Step(std::numeric_limits<double>::quiet_NaN());
        CHECK(std::isfinite(b.Height()), "bounce nan dt");
    }

    // 13. Helpers.
    CHECK(std::fabs(MomentumProject(1000.0, 0.998) - 499.0) < 1e-9, "momentum projection");
    CHECK(Rubberband(100, 400) > 0 && Rubberband(100, 400) < 100 && Rubberband(-100, 400) == -Rubberband(100, 400), "rubberband");

    if (fails == 0) std::puts("PASS motion engine: exactness, dt invariance, retarget continuity, robustness, controls, bounce");
    return fails ? 1 : 0;
}
