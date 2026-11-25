#pragma once
// Debris: a dropped icon falls into the black hole by nothing but gravity (real-world physics, no scripted motion),
// and the reverse for emit: an icon assembling out of the photon ring. Visual only.
//
// Drop: spaghettification.
//   * On release every pixel of the icon (64 x 64) becomes a free particle exactly where the card was drawn and with
//     the card's own velocity (the dragged icon's motion; ~0 when it was held still): nothing jumps, nothing starts
//     moving faster than it was.
//   * From then on each pixel follows its own timelike Schwarzschild geodesic, integrated exactly, in the plane facing
//     the observer (the screen's plane, where the file was let go). Nothing else acts on it. Released from rest it
//     falls straight in, accelerating; the side nearer the hole falls faster than the far side (the tide), and all
//     pixels fall towards the same centre, so the icon is drawn out along the radius and squeezed across it, more and
//     more as it falls: it becomes a thin noodle. A sideways flick of the drag gives it angular momentum: it then
//     swings round the hole on a precessing orbit, or is captured, as the geodesic says.
//   * The distant observer's clock: close to the horizon every pixel slows down (it never visibly crosses r = 2M) and
//     its light is redshifted, g = 1/u^t = f/E (gravitational + transverse Doppler; the plane faces the observer, so
//     there is no line-of-sight motion), intensity ~ g^3: it goes dark red and fades to black at the edge of the
//     shadow. The lens shader shows all of it lensed, with its secondary images on the other side of the hole.
//   * Shredding (a solid breaking under the tide): the icon is a rigid body of finite strength. When a piece gets inside
//     its breaking radius it cracks into 2 to 4 irregular fragments (random seed points, nearest seed wins, ragged
//     edges; cracks tend to run across the radius, where the tide pulls). Smaller pieces are
//     stronger (fewer flaws, Weibull), breaking radius ~ size^0.15 +-35 %, so the icon crumbles progressively as it
//     falls: slabs, chunks, crumbs, then single pixels. A piece moves as a rigid body on the geodesic of its centre;
//     when it breaks its halves keep its velocity plus the tide's velocity field across it (apart along the radius,
//     together across it; Omega = sqrt(2/r^3)) and an uneven elastic release (they spring apart, each at its own
//     speed and slightly off line, and tumble), and from then on the tide pulls them apart on their own geodesics: the
//     cracks open and the bits fall in one after another, the inner ones first. Pixels are drawn as their own squares,
//     so a crack is a gap.
//
// Assemble (emit): the icon's cells climb out from just outside the photon ring along unwinding spirals, dim and red
// from the potential well, and land on their places at the destination when the card arrives (scripted, not a geodesic).
#include "blackhole_payload.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace docky::blackhole {

struct DebrisGpu {  // shader layout (blackhole_debris.hlsl): float4 x, y, halfAlong, halfAcross; float4 dir, softness, tilted; float4 premultiplied linear rgba
    float x = 0, y = 0, halfAlong = 0, halfAcross = 0;   // tilted = 1: x, y, dir in the orbit plane (seen at y cos tilt)
    float dx = 1, dy = 0, pad0 = 0, pad1 = 0;
    float r = 0, g = 0, b = 0, a = 0;
};

// Forward map of the shader's tidal card (PayloadAt inverts it): icon-local point (M, screen axes, [-half, half]^2) ->
// lens-plane point (M, hole-relative). The card is stretched symmetrically: both halves keep their scale at the body
// and are drawn out to h * lambda (the inner one never past r = 2M), thinning towards the tips.
inline void CardForward(const PayloadGpu& p, double sx, double sy, double& qx, double& qy) {
    // Linear tidal deformation (the shader's PayloadAt inverts it): stretch lam along the radius, 1/sqrt(lam) across,
    // lam limited so the inner edge stays outside r = 2M.
    const double len = std::hypot(p.x, p.y);
    double urx = 1, ury = 0;
    if (len > 1e-3) { urx = -p.x / len; ury = -p.y / len; }
    const double utx = -ury, uty = urx;
    const double h = std::max<double>(p.half, 1e-4);
    const double lam = std::clamp<double>(p.stretch, 1.0, std::max(1.0, (len - 2.0) / h));
    const double pr = (sx * urx + sy * ury) * lam, pt = (sx * utx + sy * uty) / std::sqrt(lam);
    qx = p.x + pr * urx + pt * utx; qy = p.y + pr * ury + pt * uty;
}

class DebrisSystem {
public:
    static constexpr int kGrid = 64;  // 256 px icon -> 4 x 4 px cells: the icon stays sharp when it turns into particles
    static constexpr int kSlots = 4;
    static constexpr size_t kMaxGas = 4200;      // particles per dropped icon (64 x 64)
    static constexpr size_t kMaxOrphans = 4200;  // particles of an earlier drop whose slot was reused: they carry on
    static constexpr size_t kMaxParticles = static_cast<size_t>(kSlots) * kMaxGas + kMaxOrphans;
    static constexpr double kTimeScale = 0.022;  // display seconds per M of coordinate time
    // The debris plane (0 face-on, 90 edge-on): face-on, the screen's own plane, where the file is let go.
    static constexpr double kTiltDeg = 0.0;
    static constexpr double kTiltCos = 1.0, kTiltSin = 0.0;

    // Icon cells for a slot: average premultiplied linear colour of each 8 x 8 block of a 256 x 256 premultiplied BGRA icon.
    void SetIcon(int slot, const uint32_t* pixels, int size = 256) {
        if (slot < 0 || slot >= kSlots || !pixels) return;
        auto& cells = icons_[slot];
        cells.assign(kGrid * kGrid, Cell{});
        const int step = size / kGrid;
        // Runs on the render thread when an icon arrives: a table instead of 200k pow() calls (that was a dropped frame).
        static const auto toLinear = [] { std::array<float, 256> t{}; for (int i = 0; i < 256; ++i) t[i] = static_cast<float>(std::pow(i / 255.0, 2.2)); return t; }();
        for (int cy = 0; cy < kGrid; ++cy) for (int cx = 0; cx < kGrid; ++cx) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int y = 0; y < step; ++y) for (int x = 0; x < step; ++x) {
                const uint32_t p = pixels[(cy * step + y) * size + cx * step + x];
                const uint32_t ia = (p >> 24) & 255;
                if (!ia) continue;
                const double pa = ia / 255.0;
                auto lin = [&](uint32_t c) { return toLinear[std::min<uint32_t>(255, (c * 255 + ia / 2) / ia)] * pa; };
                r += lin((p >> 16) & 255); g += lin((p >> 8) & 255); b += lin(p & 255); a += pa;
            }
            const double n = step * step;
            cells[cy * kGrid + cx] = Cell{static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n), static_cast<float>(a / n)};
        }
    }

    // The drop: the icon (card geometry as last drawn, M) is let go with the card's velocity (vx, vy, M per second, screen
    // axes): every pixel becomes a free particle there.
    void Drop(int slot, const PayloadGpu& card, double vx, double vy, double now, double tidal) {
        if (slot < 0 || slot >= kSlots) return;
        // A new drop into a slot whose earlier pixels are still falling: they carry on by themselves (never cut off).
        if (!slots_[slot].assemble) {
            for (const auto& q : parts_[slot]) if (q.alive && orphans_.size() < kMaxOrphans) {
                orphans_.push_back(q); orphans_.back().nb = -1;
                if (q.piece >= 0) {  // still in a piece: carries on with the piece's motion, on its own
                    const Particle& b = pieces_[slot][static_cast<size_t>(q.piece)].b;
                    orphans_.back().E = b.E; orphans_.back().L = b.L * orphans_.back().r / std::max(b.r, 1e-6); orphans_.back().ur = b.ur;
                    orphans_.back().piece = -1;
                }
            }
        }
        parts_[slot].clear();
        SlotState& st = slots_[slot];
        st = SlotState{};
        st.born = st.lastT = now;
        st.tidal = tidal;
        st.card = card; st.card.opacity = 1; st.card.shear = 0;
        st.h0 = card.half;
        if (icons_[slot].empty()) return;
        MakePixels(st, slot, now);
        auto& ps = parts_[slot];
        pieces_[slot].clear();
        // Coordinate velocity (M per M of coordinate time), the plane's axes (y / cos tilt).
        const double ux = vx * kTimeScale, uy = vy * kTimeScale / kTiltCos;
        // The whole icon: one rigid piece centred on the card, moving with the card.
        const double cX = st.card.x, cY = st.card.y / kTiltCos;
        Piece whole; whole.alive = true; whole.size = kGrid; whole.level = 0;
        whole.b.alive = true; whole.b.r = std::max(std::hypot(cX, cY), kDeadR + 0.2); whole.b.phi = std::atan2(cY, cX);
        Init(whole.b, ux, uy);
        for (size_t i = 0; i < ps.size(); ++i) {
            auto& q = ps[i];
            double qx, qy; CardForward(st.card, q.sx, q.sy, qx, qy);
            q.ox = qx - cX; q.oy = qy / kTiltCos - cY;
            q.piece = 0;
            q.E = whole.b.E;
            whole.pixels.push_back(static_cast<int>(i));
            ++st.emitted;
            if (whole.b.E < 1) ++st.bound;
        }
        // Strength: the whole icon gives way a little inside where it was let go (r_b), its smallest bits near the hole.
        st.rBreak = std::clamp(0.85 * whole.b.r, 6.0, 22.0);  // it starts to crack once it is well on its way
        st.cellM = std::max<double>(st.card.half, 0.05) * 2 / kGrid;
        for (const auto& q : ps) st.size0 = std::max(st.size0, std::hypot(q.ox, q.oy));
        whole.rb = st.rBreak;
        pieces_[slot].push_back(std::move(whole));
        PlacePixels(st, slot);
        st.active = true;
    }

    // Emit: cells climb out of the photon ring and land on the icon at (x, y) M, half size `half` M, at now + duration.
    void Assemble(int slot, double x, double y, double half, double now, double duration) {
        if (slot < 0 || slot >= kSlots) return;
        auto& ps = parts_[slot];
        ps.clear();
        pieces_[slot].clear();
        SlotState& st = slots_[slot];
        st = SlotState{};
        st.assemble = true; st.born = st.lastT = now; st.arrive = now + std::max(duration, 0.2);
        const auto& cells = icons_[slot];
        if (cells.empty()) return;
        const double D = std::max(duration, 0.2);
        uint32_t seed = 0x85EBCA6Bu * static_cast<uint32_t>(slot + 1);
        for (int cy = 0; cy < kGrid; ++cy) for (int cx = 0; cx < kGrid; ++cx) {
            const Cell& c = cells[cy * kGrid + cx];
            if (c.a < 0.04f) continue;
            Particle p; p.cell = c; p.alive = true;
            const double tx = x + ((cx + 0.5) / kGrid - 0.5) * 2 * half, ty = y + ((cy + 0.5) / kGrid - 0.5) * 2 * half;
            p.r1 = std::hypot(tx, ty); p.phi1 = std::atan2(ty, tx);
            p.r0 = 5.4 + 1.3 * Rand(seed);
            p.phi0 = p.phi1 - (1.1 + 1.7 * Rand(seed));
            p.t0 = now + 0.3 * D * Rand(seed);
            p.t1 = std::min(p.t0 + (0.55 + 0.15 * Rand(seed)) * D, now + D);
            p.half = static_cast<float>(half / kGrid * 1.15);
            p.r = p.r0; p.phi = p.phi0;
            ps.push_back(p);
        }
        st.active = !ps.empty();
    }

    void Clear(int slot) { if (slot >= 0 && slot < kSlots) { parts_[slot].clear(); pieces_[slot].clear(); slots_[slot] = SlotState{}; } }
    bool Active() const { for (const auto& s : slots_) if (s.active) return true; return !orphans_.empty(); }
    int Alive(int slot) const { int n = 0; for (const auto& p : parts_[slot]) n += p.alive; return n; }
    int Emitted(int slot) const { return slots_[slot].emitted; }
    int EmittedBound(int slot) const { return slots_[slot].bound; }
    int Swallowed(int slot) const { return slots_[slot].swallowedN; }
    int Escaped(int slot) const { return slots_[slot].escapedN; }
    // Radial extent (M) of the live pixels of a slot: max r - min r (tests: the noodle).
    double RadialExtent(int slot) const {
        double lo = 1e9, hi = 0;
        for (const auto& p : parts_[slot]) if (p.alive) { lo = std::min(lo, p.r); hi = std::max(hi, p.r); }
        return hi > lo ? hi - lo : 0;
    }
    // Number of separate fragments of a drop: rigid pieces plus free pixels (tests: how far it has been shredded).
    int Fragments(int slot) const {
        int n = 0;
        for (const auto& c : pieces_[slot]) n += c.alive;
        for (const auto& p : parts_[slot]) n += p.alive && p.piece < 0;
        return n;
    }
    // Width (M) of the live pixels across the radius, measured as their angular spread times the mean radius.
    double CrossExtent(int slot) const {
        double lo = 1e9, hi = -1e9, rs = 0; int n = 0;
        for (const auto& p : parts_[slot]) if (p.alive) { lo = std::min(lo, p.phi); hi = std::max(hi, p.phi); rs += p.r; ++n; }
        return n > 1 ? (hi - lo) * rs / n : 0;
    }
    double MaxStep() const { return maxStep_; }  // largest distance (M) any gas parcel moved in the last Update (tests)
    double MaxHeat(int slot) const { double q = 0; for (const auto& p : parts_[slot]) if (p.alive) q = std::max(q, p.q); return q; }
    double AzimuthSpread(int slot, double rMax = 1e9) const {  // radians of azimuth covered by live gas within rMax
        std::vector<double> a;
        for (const auto& p : parts_[slot]) if (p.alive && p.r < rMax) a.push_back(std::remainder(p.phi, 6.283185307179586));
        if (a.size() < 2) return 0;
        std::sort(a.begin(), a.end());
        double gap = a.front() + 6.283185307179586 - a.back();
        for (size_t i = 1; i < a.size(); ++i) gap = std::max(gap, a[i] - a[i - 1]);
        return 6.283185307179586 - gap;
    }

    // Advances everything by the time since the last call and writes the gas to `out`. Returns the number of drops whose
    // bound gas was mostly swallowed this frame (the hole answers with a Consume pulse). fieldM: lens field radius (M).
    int Update(double now, double dt, double fieldM, std::vector<DebrisGpu>& out) {
        out.clear();
        maxStep_ = 0;
        int swallowed = 0;
        for (int s = 0; s < kSlots; ++s) {
            SlotState& st = slots_[s];
            if (!st.active) continue;
            const double step = std::clamp(now - st.lastT, 0.0, 0.1);
            st.lastT = now;
            int alive = 0;
            if (st.assemble) {
                for (auto& p : parts_[s]) {
                    if (!p.alive) continue;
                    if (now >= st.arrive + 0.15) { p.alive = false; continue; }
                    const double u = std::clamp((now - p.t0) / std::max(p.t1 - p.t0, 1e-3), 0.0, 1.0);
                    const double e = u * u * (3 - 2 * u), ea = 1 - (1 - u) * (1 - u);
                    const double r = p.r0 + (p.r1 - p.r0) * e, phi = p.phi0 + (p.phi1 - p.phi0) * ea;
                    const double prevx = p.r * std::cos(p.phi), prevy = p.r * std::sin(p.phi);
                    p.r = r; p.phi = phi;
                    const double mx = r * std::cos(phi) - prevx, my = r * std::sin(phi) - prevy;
                    const double m = std::hypot(mx, my);
                    double dirx = 1, diry = 0;
                    if (m > 1e-6) { dirx = mx / m; diry = my / m; }
                    const double speed = dt > 0 ? m / dt * 0.02 : 0;
                    double fade = std::clamp((now - p.t0) / (0.15 * (p.t1 - p.t0) + 1e-3), 0.0, 1.0);
                    if (now > st.arrive) fade *= 1 - (now - st.arrive) / 0.15;  // the card takes over
                    const double heat = 0.6 * std::sin(3.14159265 * u);
                    const double g = (1 - 2 / r) * (1 - e) + e;  // out of the well: back to full brightness on landing
                    ++alive;
                    if (fade > 0) out.push_back(ShadeCell(p, fade, heat, g, speed, dirx, diry));
                }
                if (alive == 0) st.active = false;
                continue;
            }

            // ---- gas ----
            int boundAlive = 0;
            const double cool = std::exp(-step / 0.45);
            auto& ps = parts_[s];
            // Playback rate (Warp): one clock per drop, at the mean radius of what is left of it (a clock per fragment
            // would let the outer bits catch up with the inner ones and undo the tide's stretch).
            double rsum = 0; int rn = 0;
            for (const auto& p : ps) if (p.alive) { rsum += p.r; ++rn; }
            const double sim = step * (rn ? Warp(rsum / rn) : 1.0);
            StepPieces(st, s, sim);
            for (auto& p : ps) {
                if (!p.alive) continue;
                if (p.piece >= 0) { ++alive; if (p.E < 1) ++boundAlive; continue; }  // carried by its piece
                if (!StepGas(p, sim, cool, fieldM, now)) {
                    if (p.r <= kDeadR + 1e-9) ++st.swallowedN; else ++st.escapedN;
                    continue;
                }
                ++alive;
                if (p.E < 1) ++boundAlive;
            }
            DrawGas(ps, now, fieldM, out);
            if (!st.pulsed && st.bound > 0 && boundAlive <= st.bound * 0.35) { st.pulsed = true; ++swallowed; }
            if (alive == 0) st.active = false;
        }
        {   // orphaned gas (an earlier drop's, its slot reused)
            const double step = std::clamp(now - orphanT_, 0.0, 0.1);
            orphanT_ = now;
            const double cool = std::exp(-step / 0.45);
            for (auto& p : orphans_) StepGas(p, step, cool, fieldM, now);
            DrawGas(orphans_, now, fieldM, out);
            orphans_.erase(std::remove_if(orphans_.begin(), orphans_.end(), [](const Particle& q) { return !q.alive; }), orphans_.end());
        }
        return swallowed;
    }

private:
    static constexpr double kDeadR = 2.1;  // g = 1 - 2/r < 0.05: black, and only ever seen through the shadow
    static constexpr double kHeat = 110.0;  // dissipated orbital energy (per unit rest mass) -> heat content (sets the temperature scale: the ring's inner edge ~ white)
    struct Cell { float r = 0, g = 0, b = 0, a = 0; };
    struct Particle {
        Cell cell;
        double r = 0, phi = 0, ur = 0, L = 0, E = 1;  // geodesic state (ur = dr/dtau)
        bool alive = false;
        double t0 = 0, t1 = 0;                        // gas: birth time; assemble: path window
        double r0 = 0, phi0 = 0, r1 = 0, phi1 = 0;   // assemble path
        float half = 0;
        double q = 0;                                 // gas: heat content (dissipated, not yet radiated)
        int nb = -1;                                  // gas: the neighbouring pixel of the icon (drawn as a ribbon to it)
        double sx = 0, sy = 0;                        // gas: place in the card (M, screen axes) while still one body
        double ox = 0, oy = 0;                        // drop: offset from the card centre in the plane at release (M)
        int piece = -1;                               // drop: the rigid piece it belongs to; -1 = a free pixel
        double ang = 0, spin = 0;                     // drop: orientation of its square (rad), spin (rad per M)
        int gx = 0, gy = 0;                           // drop: cell of the icon
    };
    struct Piece {  // a rigid fragment of a dropped icon: cells [x0, x0 + size) x [y0, y0 + size)
        Particle b;           // its centre's geodesic
        double lx = 0, ly = 0;  // its centre's offset from the card centre at release (plane, M)
        double rb = 0;          // it breaks when its centre gets inside this radius
        double ang = 0, spin = 0;  // orientation (rad) and spin (rad per M of coordinate time): pieces tumble
        int x0 = 0, y0 = 0, size = 0, level = 0;
        bool alive = false;
        std::vector<int> pixels;
    };
    struct SlotState {
        bool active = false, assemble = false, pulsed = false, bodyAlive = false;
        double born = 0, lastT = 0, arrive = 0, tidal = 1, h0 = 1, burstAt = -1, rBreak = 10, cellM = 0.05, size0 = 1;
        int emitted = 0, bound = 0, swallowedN = 0, escapedN = 0;
        PayloadGpu card;
        Particle body;
    };

    static double Rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / 16777216.0; }

    // One gas parcel: geodesic step (dissipation and heating while bound), cooling. False when it is gone: swallowed at
    // the horizon, or escaped (unbound and past the field edge; bound gas on a wide orbit may leave the field and come
    // back, unless it goes very far).
    // Playback rate: far out a fall from rest is very slow at first (it looked frozen while it cracked), so the clock
    // runs faster there, smoothly with the radius, ~ r^1.5 like the free-fall time itself (x8.5 at 25 M, x1 from 6 M in),
    // so a drop from anywhere in the field keeps going; the paths are the exact free-fall
    // geodesics, only the slow far part is played faster.
    static double Warp(double r) { return std::clamp(std::pow(r / 6.0, 1.5), 1.0, 10.0); }

    bool StepGas(Particle& p, double step, double cool, double fieldM, double now) {
        const double x0 = p.r * std::cos(p.phi), y0 = p.r * std::sin(p.phi);
        const double dtc = step / kTimeScale;
        Advance(p, dtc, false);  // free fall: nothing but gravity acts
        p.ang += p.spin * dtc;
        if (!p.alive) return false;
        const double seen = p.r * std::hypot(std::cos(p.phi), std::sin(p.phi) * kTiltCos);  // projected radius
        if ((p.E >= 1 && seen > fieldM) || p.r > 3 * fieldM / kTiltCos || now - p.t0 > 18) { p.alive = false; return false; }
        p.q *= cool;
        maxStep_ = std::max(maxStep_, std::hypot(p.r * std::cos(p.phi) - x0, p.r * std::sin(p.phi) - y0));
        return true;
    }

    // On release: every pixel of the icon becomes a particle (its place in the card: sx, sy) with its ribbon neighbour.
    void MakePixels(SlotState& st, int slot, double now) {
        const auto& cells = icons_[slot];
        auto& ps = parts_[slot];
        const double h = std::max<double>(st.card.half, 0.05);
        const PayloadGpu& c = st.card;
        const double len = std::hypot(c.x, c.y);
        const double urx = len > 1e-3 ? -c.x / len : 1, ury = len > 1e-3 ? -c.y / len : 0;
        // Neighbour along the tide's axis (the radius), where the ribbon will be drawn out.
        const int ax = std::fabs(urx) >= std::fabs(ury) ? (urx > 0 ? 1 : -1) : 0;
        const int ay = ax == 0 ? (ury > 0 ? 1 : -1) : 0;
        std::vector<int> index(kGrid * kGrid, -1);
        for (int cy = 0; cy < kGrid; ++cy) for (int cx = 0; cx < kGrid; ++cx) {
            const Cell& cell = cells[cy * kGrid + cx];
            if (cell.a < 0.04f || ps.size() >= kMaxGas) continue;
            Particle p; p.cell = cell; p.alive = true; p.t0 = now;
            p.sx = ((cx + 0.5) / kGrid - 0.5) * 2 * h; p.sy = ((cy + 0.5) / kGrid - 0.5) * 2 * h;
            p.half = static_cast<float>(h / kGrid * 1.7);  // overlapping squares: no seams while it is whole (soft edges)
            p.gx = cx; p.gy = cy;
            index[cy * kGrid + cx] = static_cast<int>(ps.size());
            ps.push_back(p);
        }
        for (int cy = 0; cy < kGrid; ++cy) for (int cx = 0; cx < kGrid; ++cx) {
            const int i = index[cy * kGrid + cx];
            const int nx = cx + ax, ny = cy + ay;
            if (i < 0 || nx < 0 || ny < 0 || nx >= kGrid || ny >= kGrid) continue;
            ps[static_cast<size_t>(i)].nb = index[ny * kGrid + nx];
        }
    }

    // Rigid pieces: each moves on its centre's geodesic and breaks into four once the tide's stress on it exceeds its
    // strength (radius r_L = r_b 0.78^L for level L). Pixels of a broken single-cell piece become free.
    void StepPieces(SlotState& st, int slot, double step) {
        auto& pcs = pieces_[slot];
        auto& ps = parts_[slot];
        const size_t n = pcs.size();
        for (size_t i = 0; i < n; ++i) {
            if (!pcs[i].alive) continue;
            Particle& b = pcs[i].b;
            const double x0 = b.r * std::cos(b.phi), y0 = b.r * std::sin(b.phi);
            const double dtc = step / kTimeScale;
            Advance(b, dtc, false);
            pcs[i].ang += pcs[i].spin * dtc;
            maxStep_ = std::max(maxStep_, std::hypot(b.r * std::cos(b.phi) - x0, b.r * std::sin(b.phi) - y0));
            if (!b.alive) {  // a whole piece reached the horizon
                pcs[i].alive = false;
                for (int k : pcs[i].pixels) { auto& q = ps[static_cast<size_t>(k)]; if (q.alive) { q.alive = false; ++st.swallowedN; } }
                pcs[i].pixels.clear();
                continue;
            }
            if (b.r < pcs[i].rb) Split(st, slot, i);
        }
        pcs.erase(std::remove_if(pcs.begin(), pcs.end(), [](const Piece& q) { return !q.alive && q.pixels.empty(); }), pcs.end());
        // Re-index the pixels' pieces after the erase.
        for (size_t i = 0; i < pcs.size(); ++i) for (int k : pcs[i].pixels) ps[static_cast<size_t>(k)].piece = static_cast<int>(i);
        PlacePixels(st, slot);
    }

    // A piece fails: one crack, at a random place along it, with a jagged edge. Under tension the crack runs across the
    // radius (the tide pulls along it); a piece already long and thin along the radius cracks along it instead.
    void Split(SlotState& st, int slot, size_t idx) {
        auto& pcs = pieces_[slot];
        auto& ps = parts_[slot];
        Piece parent = std::move(pcs[idx]);
        pcs[idx].alive = false; pcs[idx].pixels.clear();
        const Particle& b = parent.b;
        const double f = 1 - 2 / b.r;
        const double drdt = b.ur * f / b.E, dpdt = b.L * f / (b.E * b.r * b.r);
        const double vx = drdt * std::cos(b.phi) - b.r * dpdt * std::sin(b.phi), vy = drdt * std::sin(b.phi) + b.r * dpdt * std::cos(b.phi);
        const double X = b.r * std::cos(b.phi), Y = b.r * std::sin(b.phi);
        const double omega = std::sqrt(2 / (b.r * b.r * b.r));  // tidal rate: the separation speed scale
        const double erx = X / b.r, ery = Y / b.r;
        const double pcs_ = std::cos(parent.ang), psn = std::sin(parent.ang);
        auto rot = [&](double ux, double uy, double& wx, double& wy) { wx = pcs_ * ux - psn * uy; wy = psn * ux + pcs_ * uy; };
        // The crack pattern: 2 to 4 fragments around random seed points (nearest seed wins, a Voronoi pattern), with
        // distance stretched along the radius (cracks tend to run across it, where the tide pulls) and ragged edges.
        const int nk = 2 + static_cast<int>(Rand(seed_) * 2.999);
        double seedX[4], seedY[4];
        for (int j = 0; j < nk; ++j) {
            const auto& q = ps[static_cast<size_t>(parent.pixels[static_cast<size_t>(Rand(seed_) * parent.pixels.size()) % parent.pixels.size()])];
            rot(q.ox - parent.lx, q.oy - parent.ly, seedX[j], seedY[j]);
        }
        double ext = 0;
        for (int k : parent.pixels) { const auto& q = ps[static_cast<size_t>(k)]; ext = std::max(ext, std::hypot(q.ox - parent.lx, q.oy - parent.ly)); }
        const double a1 = 2 + 5 * Rand(seed_), a2 = 2 + 5 * Rand(seed_), p1 = 6.283 * Rand(seed_), p2 = 6.283 * Rand(seed_);
        const double rough = 0.25 * std::max(ext, 1e-6);
        Piece kids[4];
        double sx[4] = {0, 0, 0, 0}, sy[4] = {0, 0, 0, 0};
        for (int k : parent.pixels) {
            const auto& q = ps[static_cast<size_t>(k)];
            double wx, wy; rot(q.ox - parent.lx, q.oy - parent.ly, wx, wy);
            const double nx = wx / std::max(ext, 1e-6), ny = wy / std::max(ext, 1e-6);
            const double jag = rough * (0.6 * std::sin(a1 * nx * 3.1 + p1) * std::cos(a2 * ny * 2.7 + p2) + 0.4 * std::sin((a1 + a2) * (nx + ny) * 2.3));
            int best = 0; double bd = 1e18;
            for (int j = 0; j < nk; ++j) {
                const double ex = wx - seedX[j], ey = wy - seedY[j];
                const double er = ex * erx + ey * ery, et = -ex * ery + ey * erx;
                const double d = std::sqrt(1.6 * er * er + et * et) + (j % 2 ? jag : -jag);
                if (d < bd) { bd = d; best = j; }
            }
            kids[best].pixels.push_back(k); sx[best] += q.ox; sy[best] += q.oy;
        }
        // The tide's velocity field across the piece: stretching along the radius, squeezing across it
        // (tidal tensor diag(2, -1) / r^3; the speed it builds up over a dynamical time ~ 1/Omega).
        // Plus the elastic energy stored in the strained solid, released by the crack: the halves spring apart
        // (~ Omega d away from the parent's centre), and the crack's torque sets them tumbling.
        auto kickAt = [&](double ddx, double ddy, double& kx, double& ky) {
            const double dr = ddx * erx + ddy * ery, dtx = ddx - dr * erx, dty = ddy - dr * ery;
            // The release is uneven (where the crack lets go first): strength and direction vary from piece to piece.
            const double e = 0.6 + 1.4 * Rand(seed_), ra = 6.283 * Rand(seed_), rr = 0.6 * Rand(seed_) * std::hypot(ddx, ddy);
            double ox = omega * (0.5 * dr * erx - 0.25 * dtx + e * ddx + rr * std::cos(ra));
            double oy = omega * (0.5 * dr * ery - 0.25 * dty + e * ddy + rr * std::sin(ra));
            // The release only nudges the pieces apart: never more than a third of the fall speed (no fragment is
            // thrown back out and left hanging behind).
            const double om = std::hypot(ox, oy), cap = 0.33 * std::hypot(vx, vy) + 0.002;
            if (om > cap) { ox *= cap / om; oy *= cap / om; }
            kx = vx + ox; ky = vy + oy;
        };
        for (int side = 0; side < nk; ++side) {
            Piece& c = kids[side];
            if (c.pixels.empty()) continue;
            c.lx = sx[side] / c.pixels.size(); c.ly = sy[side] / c.pixels.size();
            double dx, dy; rot(c.lx - parent.lx, c.ly - parent.ly, dx, dy);
            if (c.pixels.size() <= 3) {  // crumbs: every pixel free from now on
                for (int k : c.pixels) {
                    auto& q = ps[static_cast<size_t>(k)];
                    double qdx, qdy; rot(q.ox - parent.lx, q.oy - parent.ly, qdx, qdy);
                    q.piece = -1; q.r = std::hypot(X + qdx, Y + qdy); q.phi = std::atan2(Y + qdy, X + qdx);
                    if (q.r <= kDeadR + 0.05) { q.alive = false; ++st.swallowedN; continue; }
                    double kx, ky; kickAt(qdx, qdy, kx, ky);
                    // Crumbs fly off the broken edge unevenly: each its own extra speed and direction, its own size,
                    // spinning fast.
                    const double ca = 6.283 * Rand(seed_), cv = std::min(omega * std::max(ext, 0.2) * (0.3 + 1.2 * Rand(seed_)), 0.25 * std::hypot(vx, vy) + 0.002);
                    Init(q, kx + cv * std::cos(ca), ky + cv * std::sin(ca));
                    q.ang = 6.283 * Rand(seed_); q.spin = parent.spin + (2 * Rand(seed_) - 1) * 8.0 * omega;
                    q.half = static_cast<float>(q.half * (0.55 + 0.9 * Rand(seed_)));
                }
                continue;
            }
            c.b.alive = true; c.b.r = std::hypot(X + dx, Y + dy); c.b.phi = std::atan2(Y + dy, X + dx);
            if (c.b.r <= kDeadR + 0.05) { for (int k : c.pixels) { auto& q = ps[static_cast<size_t>(k)]; if (q.alive) { q.alive = false; ++st.swallowedN; } } continue; }
            double kx, ky; kickAt(dx, dy, kx, ky);
            Init(c.b, kx, ky);
            c.ang = parent.ang;
            c.spin = parent.spin + (2 * Rand(seed_) - 1) * 3.0 * omega;
            // Strength: smaller pieces hold out longer (fewer flaws, Weibull): breaking radius ~ size^0.15, +-35 %.
            double cext = 0;
            for (int k : c.pixels) { const auto& q = ps[static_cast<size_t>(k)]; cext = std::max(cext, std::hypot(q.ox - c.lx, q.oy - c.ly)); }
            c.rb = st.rBreak * std::pow(std::max(cext, st.cellM) / st.size0, 0.15) * (0.65 + 0.7 * Rand(seed_));
            c.alive = true;
            pcs.push_back(std::move(c));
        }
    }

    // Pixels of rigid pieces: at their offset from the piece's centre (pieces do not rotate).
    void PlacePixels(SlotState& st, int slot) {
        (void)st;
        auto& pcs = pieces_[slot];
        auto& ps = parts_[slot];
        for (const auto& c : pcs) {
            if (!c.alive) continue;
            const double X = c.b.r * std::cos(c.b.phi), Y = c.b.r * std::sin(c.b.phi);
            for (int k : c.pixels) {
                auto& q = ps[static_cast<size_t>(k)];
                if (!q.alive) continue;
                const double cs = std::cos(c.ang), sn = std::sin(c.ang);
                const double ux = q.ox - c.lx, uy = q.oy - c.ly;
                const double px = X + cs * ux - sn * uy, py = Y + sn * ux + cs * uy;
                q.r = std::max(std::hypot(px, py), kDeadR + 0.01); q.phi = std::atan2(py, px);
                q.E = c.b.E; q.ur = c.b.ur; q.L = c.b.L * q.r / std::max(c.b.r, 1e-6);
                q.ang = c.ang;
            }
        }
    }

    // Gas, drawn as a ribbon: each parcel is a segment to its neighbour of the original icon, so the stream stays
    // continuous however far it is drawn out (a parcel whose neighbour is gone, or far away, is a single pixel).
    void DrawGas(const std::vector<Particle>& ps, double now, double fieldM, std::vector<DebrisGpu>& out) const {
        for (const auto& p : ps) {
            if (!p.alive) continue;
            const double seen = p.r * std::hypot(std::cos(p.phi), std::sin(p.phi) * kTiltCos);
            if (seen > fieldM) continue;
            const double f = 1 - 2 / p.r;
            const double age = now - p.t0;
            const double fade = std::clamp((fieldM * 0.97 - seen) / (fieldM * 0.15), 0.0, 1.0);
            if (fade <= 0) continue;
            const double x = p.r * std::cos(p.phi), y = p.r * std::sin(p.phi);
            double cx = x, cy = y, along = p.half, dx = 1, dy = 0;
            const Particle* n = nullptr;  // drawn as its own square: a crack between pieces is a gap
            if (n && n->alive) {
                const double nx = n->r * std::cos(n->phi), ny = n->r * std::sin(n->phi);
                const double ex = nx - x, ey = ny - y, d = std::hypot(ex, ey);
                // A neighbour too far round the hole would be joined by a chord across it: then a single pixel.
                const double cap = std::min(6.0, 0.9 * std::min(p.r, n->r));
                if (d > 1e-6 && d < cap) { cx = 0.5 * (x + nx); cy = 0.5 * (y + ny); along = 0.5 * d + p.half; dx = ex / d; dy = ey / d; }
            }
            const double drdt = p.ur * f / p.E, dpdt = p.L * f / (p.E * p.r * p.r);
            const double vx = drdt * std::cos(p.phi) - p.r * dpdt * std::sin(p.phi), vy = drdt * std::sin(p.phi) + p.r * dpdt * std::cos(p.phi);
            if (along == p.half) {  // a lone pixel: along its motion
                (void)vx;  // a square turned with its fragment (or tumbling on its own)
                dx = std::cos(p.ang); dy = std::sin(p.ang);
            }
            // Redshift: gravitational and transverse (1/u^t = f/E) times the Doppler factor of the motion towards the
            // observer, whose direction has the component sin(tilt) along the plane's Y axis.
            const double g = std::clamp((f / p.E) / std::max(1 - vy * kTiltSin, 0.2), 0.0, 2.5);
            DebrisGpu o = ShadeGas(p, fade, g);
            o.x = static_cast<float>(cx); o.y = static_cast<float>(cy);
            // Shock-heated gas puffs up (pressure): the ring is thicker than the cold stream.
            const double puff = 1 + 2.0 * std::min(std::sqrt(std::max(p.q, 0.0)), 1.5);
            o.halfAlong = static_cast<float>(along + p.half * (puff - 1)); o.halfAcross = static_cast<float>(p.half * puff);
            o.dx = static_cast<float>(dx); o.dy = static_cast<float>(dy);
            o.pad0 = static_cast<float>(p.piece >= 0 ? 0.0 : std::clamp(age / 6.0, 0.0, 0.35));  // crisp pixels
            o.pad1 = 1;  // in the tilted plane
            out.push_back(o);
        }
    }

    // Geodesic state from a position (p.r, p.phi) and a coordinate velocity (kept timelike).
    static void Init(Particle& p, double vx, double vy) {
        double vr = vx * std::cos(p.phi) + vy * std::sin(p.phi);
        double om = (-vx * std::sin(p.phi) + vy * std::cos(p.phi)) / p.r;
        const double f = 1 - 2 / p.r;
        double A = vr * vr / f + p.r * p.r * om * om;
        if (A > 0.9 * f) { const double k = std::sqrt(0.9 * f / A); vr *= k; om *= k; A *= k * k; }
        const double ut = 1 / std::sqrt(std::max(f - A, 1e-9));
        p.E = f * ut; p.L = p.r * p.r * om * ut; p.ur = vr * ut;
    }

    // Schwarzschild timelike geodesic in the plane, velocity Verlet in proper time tau:
    //   r'' = -1/r^2 + L^2/r^3 - 3 L^2/r^4,  phi' = L/r^2,  dt/dtau = E/f.
    // dissipate (bound gas only): shocks damp radial motion and remove angular momentum; E is re-derived from the radial
    // equation (ur^2 = E^2 - f (1 + L^2/r^2)) and the energy given up goes into the parcel's heat.
    static void Advance(Particle& p, double dtCoord, bool dissipate) {
        double left = dtCoord;
        for (int it = 0; it < 96 && left > 1e-9; ++it) {
            const double f0 = 1 - 2 / p.r;
            const double dtau = std::min(left * f0 / p.E, 0.08 * p.r);
            auto acc = [&](double r) { const double L2 = p.L * p.L; return -1 / (r * r) + L2 / (r * r * r) - 3 * L2 / (r * r * r * r); };
            p.ur += 0.5 * dtau * acc(p.r);
            const double rOld = p.r;
            p.r += dtau * p.ur;
            if (p.r < kDeadR) { p.alive = false; return; }
            const double rm = 0.5 * (rOld + p.r);
            p.phi += dtau * p.L / (rm * rm);
            p.ur += 0.5 * dtau * acc(p.r);
            const double f1 = 1 - 2 / p.r;
            if (dissipate && p.E < 1) {
                const double e0 = std::sqrt(std::max(f1 * (1 + p.L * p.L / (p.r * p.r)) + p.ur * p.ur, 1e-8));
                p.ur *= std::exp(-0.03 * dtau);
                p.L *= std::exp(-0.0042 * dtau);
                p.E = std::sqrt(std::max(f1 * (1 + p.L * p.L / (p.r * p.r)) + p.ur * p.ur, 1e-8));
                p.q += std::max(e0 - p.E, 0.0) * kHeat;
            }
            left -= dtau * p.E / (0.5 * (f0 + f1));
        }
    }

    // Black-body colour (linear rgb, roughly unit luminance) for a temperature in units of the disc's inner-edge peak.
    static void Blackbody(double t, double& r, double& g, double& b) {
        static const double k[6][4] = {{0.0, 0.6, 0.05, 0.0}, {0.35, 1.0, 0.22, 0.03}, {0.65, 1.0, 0.52, 0.18},
                                       {0.95, 1.0, 0.82, 0.58}, {1.25, 0.92, 0.94, 1.0}, {1.7, 0.72, 0.82, 1.0}};
        t = std::clamp(t, 0.0, 1.7);
        int i = 0; while (i < 4 && t > k[i + 1][0]) ++i;
        const double u = (t - k[i][0]) / (k[i + 1][0] - k[i][0]);
        r = k[i][1] + (k[i + 1][1] - k[i][1]) * u; g = k[i][2] + (k[i + 1][2] - k[i][2]) * u; b = k[i][3] + (k[i + 1][3] - k[i][3]) * u;
    }

    // Gas colour: matter in the icon's colours, plus its thermal glow (black body at the observed temperature g T,
    // brightness ~ (g T)^4). Position and shape are set by DrawGas.
    static DebrisGpu ShadeGas(const Particle& p, double fade, double g) {
        DebrisGpu o;
        const Cell& c = p.cell;
        const double a = c.a;
        const double gc = std::clamp(g, 1e-3, 2.5);
        const double T = std::pow(std::max(p.q, 0.0), 0.25), Tobs = gc * T;
        double br, bg, bb; Blackbody(Tobs, br, bg, bb);
        const double I = std::min(1.5, 1.2 * Tobs * Tobs * Tobs * Tobs);
        const double coldMatter = std::max(0.0, 1 - T) * std::min(std::pow(gc, 3.0), 2.5);  // unshocked gas: icon colours, beamed
        const double r = c.r * coldMatter + br * I * a, gg = c.g * coldMatter + bg * I * a, b = c.b * coldMatter + bb * I * a;
        o.r = static_cast<float>(r * fade); o.g = static_cast<float>(gg * fade); o.b = static_cast<float>(b * fade);
        o.a = static_cast<float>(a * fade);
        return o;
    }

    // Assemble cells: the icon's own pixels (crisp), warm while climbing out, redshifted near the hole.
    static DebrisGpu ShadeCell(const Particle& p, double fade, double heat, double g, double speed, double dx, double dy) {
        DebrisGpu o;
        o.x = static_cast<float>(p.r * std::cos(p.phi)); o.y = static_cast<float>(p.r * std::sin(p.phi));
        o.halfAlong = static_cast<float>(p.half * (1 + std::min(1.5, 2.5 * speed))); o.halfAcross = p.half;
        o.dx = static_cast<float>(dx); o.dy = static_cast<float>(dy);
        const Cell& c = p.cell;
        const double a = c.a;
        const double lum = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / std::max(a, 1e-3);
        const double hr = 0.95 + 0.05 * heat, hg = 0.25 + 0.6 * heat, hb = 0.06 + 0.55 * heat;
        const double glow = heat * a * (0.6 + 0.8 * lum) * 2.4;
        double r = c.r * (1 - 0.55 * heat) + hr * glow, gg = c.g * (1 - 0.55 * heat) + hg * glow, b = c.b * (1 - 0.55 * heat) + hb * glow;
        const double gc = std::clamp(g, 1e-3, 1.0);
        r *= std::pow(gc, 1.8); gg *= std::pow(gc, 2.5); b *= std::pow(gc, 3.3);
        o.r = static_cast<float>(r * fade); o.g = static_cast<float>(gg * fade); o.b = static_cast<float>(b * fade);
        o.a = static_cast<float>(a * fade);
        return o;
    }

    std::vector<Cell> icons_[kSlots];
    std::vector<Particle> parts_[kSlots];
    std::vector<Piece> pieces_[kSlots];
    SlotState slots_[kSlots];
    std::vector<Particle> orphans_;
    double orphanT_ = 0;
    uint32_t seed_ = 0x2545F491u;
    double maxStep_ = 0;
};

}  // namespace docky::blackhole
