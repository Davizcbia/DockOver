#pragma once
// Schwarzschild null geodesics for the Black Hole Visualizer (DockDrop's visual base).
//
// Geometric units G = c = M = 1 (event horizon r = 2, photon sphere r = 3, critical impact parameter b_c = 3*sqrt(3)).
// A photon traced backwards from a distant observer with impact parameter b moves in a plane; with u = 1/r and the
// orbital angle phi measured from the observer direction its orbit obeys (Binet equation, exact GR):
//
//     d^2u/dphi^2 = 3 u^2 - u,       u(0) = 0,  u'(0) = 1/b
//
// b > b_c: the photon escapes again at phi_end = pi + alpha (alpha = total deflection, unbounded as b -> b_c+).
// b < b_c: the photon crosses the horizon (u = 1/2) at phi_end.
//
// The renderer needs u(b, phi) at arbitrary disk-crossing angles (accretion disk images of order 0, 1, 2 ...) and
// phi_end(b) for the background. Both are integrated once on the CPU (RK4) into look-up tables:
//   * 2D table  [column = b, row = phi]  -> u   (R32_FLOAT, columns non-uniform in b, dense around b_c)
//   * 1D table  [column = b]             -> phi_end
// Column mapping (shared with shaders/blackhole.hlsl, keep in sync): s in [0,1], s = 0.5 is b_c.
//   b < b_c : s = 0.5 - 0.5 * G(b_c - b, b_c)
//   b > b_c : s = 0.5 + 0.5 * G(b - b_c, bMax - b_c)
//   G(d, D) = log(1 + d/delta) / log(1 + D/delta)
// Columns are texel-centred; s = 0.5 is the border between column N/2-1 (captured) and N/2 (escaping), so the shader
// can clamp to one side and never interpolate across the capture boundary.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace docky::blackhole {

inline constexpr double kPi = 3.14159265358979323846;
inline const double kCriticalB = 3.0 * std::sqrt(3.0);  // 5.196152422706632

struct LutSpec {
    int columns = 1024;       // b samples (even)
    int rows = 1024;          // phi samples
    double bMax = 400.0;      // beyond this the shader uses the weak-field series
    double delta = 1e-4;      // log mapping knee around b_c
    double phiMax = 4.0 * kPi;  // covers disk images of order 0..2 (plus the start of order 3)
    double step = 5e-4;       // RK4 step in phi
};

inline double MapG(double d, double D, double delta) { return std::log1p(d / delta) / std::log1p(D / delta); }
inline double InvG(double g, double D, double delta) { return delta * std::expm1(g * std::log1p(D / delta)); }

// Column coordinate s in [0,1] for impact parameter b.
inline double BToS(double b, const LutSpec& spec) {
    if (b < kCriticalB) return 0.5 - 0.5 * MapG(kCriticalB - std::max(0.0, b), kCriticalB, spec.delta);
    return 0.5 + 0.5 * MapG(std::min(b, spec.bMax) - kCriticalB, spec.bMax - kCriticalB, spec.delta);
}
inline double SToB(double s, const LutSpec& spec) {
    if (s < 0.5) return kCriticalB - InvG((0.5 - s) * 2.0, kCriticalB, spec.delta);
    return kCriticalB + InvG((s - 0.5) * 2.0, spec.bMax - kCriticalB, spec.delta);
}

struct Trace {
    bool captured = false;
    double phiEnd = 0;  // escape (pi + deflection) or horizon crossing angle
};

// Integrates one photon. `sample(phi, u)` is called at every step (optional). Stops at escape, capture or phiLimit.
template <class Sample>
inline Trace Integrate(double b, double phiLimit, double h, Sample&& sample) {
    double u = 0.0, v = 1.0 / b, phi = 0.0;  // v = du/dphi
    Trace t; t.phiEnd = phiLimit;
    auto acc = [](double uu) { return 3.0 * uu * uu - uu; };
    sample(0.0, 0.0);
    while (phi < phiLimit) {
        const double k1u = v, k1v = acc(u);
        const double k2u = v + 0.5 * h * k1v, k2v = acc(u + 0.5 * h * k1u);
        const double k3u = v + 0.5 * h * k2v, k3v = acc(u + 0.5 * h * k2u);
        const double k4u = v + h * k3v, k4v = acc(u + h * k3u);
        const double un = u + h / 6.0 * (k1u + 2 * k2u + 2 * k3u + k4u);
        const double vn = v + h / 6.0 * (k1v + 2 * k2v + 2 * k3v + k4v);
        const double pn = phi + h;
        if (un < 0.0 && phi > 0.0) {  // back at infinity: escaped (linear root between the two steps)
            t.phiEnd = phi + h * u / (u - un);
            sample(pn, un);
            return t;
        }
        if (un >= 0.5) {  // horizon
            t.captured = true;
            t.phiEnd = phi + h * (0.5 - u) / (un - u);
            sample(pn, un);
            return t;
        }
        u = un; v = vn; phi = pn;
        sample(phi, u);
    }
    t.captured = b < kCriticalB;
    return t;
}

// Weak-field total deflection (second order), for b beyond the table.
inline double WeakDeflection(double b) { return 4.0 / b + 15.0 * kPi / (4.0 * b * b); }

struct Lut {
    LutSpec spec;
    std::vector<float> u;       // rows x columns, row-major (row = phi)
    std::vector<float> phiEnd;  // columns
};

// ~0.1-0.3 s single-threaded for the default size; `threads` splits columns.
inline Lut BuildLut(const LutSpec& spec = {}, int threads = 0);

}  // namespace docky::blackhole

// ---- implementation ------------------------------------------------------------------------------------------------
#include <thread>
namespace docky::blackhole {
inline Lut BuildLut(const LutSpec& spec, int threads) {
    Lut lut; lut.spec = spec;
    lut.u.assign(static_cast<size_t>(spec.columns) * spec.rows, 0.f);
    lut.phiEnd.assign(spec.columns, 0.f);
    const double rowStep = spec.phiMax / (spec.rows - 1);
    auto column = [&](int c) {
        const double s = (c + 0.5) / spec.columns;
        const double b = SToB(s, spec);
        int next = 0; double lastPhi = 0, lastU = 0;
        auto store = [&](int row, double value) { lut.u[static_cast<size_t>(row) * spec.columns + c] = static_cast<float>(std::clamp(value, -1.0, 1.0)); };
        const Trace t = Integrate(b, spec.phiMax + 4 * spec.step, spec.step, [&](double phi, double uu) {
            while (next < spec.rows && next * rowStep <= phi) {
                const double target = next * rowStep;
                const double f = phi > lastPhi ? (target - lastPhi) / (phi - lastPhi) : 1.0;
                store(next, lastU + (uu - lastU) * std::clamp(f, 0.0, 1.0));
                ++next;
            }
            lastPhi = phi; lastU = uu;
        });
        // Rows past the end: continue the trend so bilinear filtering right at phi_end stays meaningful
        // (escaped: u < 0, captured: u > 1/2); the shader never uses them as hits.
        for (; next < spec.rows; ++next) store(next, t.captured ? 1.0 : -1.0);
        lut.phiEnd[c] = static_cast<float>(t.phiEnd);
    };
    if (threads <= 0) threads = static_cast<int>(std::max(1u, std::min(8u, std::thread::hardware_concurrency())));
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k)
        pool.emplace_back([&, k] { for (int c = k; c < spec.columns; c += threads) column(c); });
    for (auto& th : pool) th.join();
    return lut;
}
}  // namespace docky::blackhole
