#pragma once
// DockOver Orb Code: the pairing information carried by the particle orb (docky-orb), a camera-readable visual code
// in the spirit of the particle cloud Apple shows for Quick Start (whose own format is not published, so this is our
// own design). Pure C++, no Windows dependencies: the same encoder drives the renderer and the decoder will be ported
// to the phone's scanner (web page or app).
//
// Layout (all in the image plane, relative to the orb's centre c and radius R):
//   * the orb itself: a dense, bright particle sphere of radius R (the decoder's anchor for position and scale);
//   * three code rings around it at 1.30 R, 1.48 R and 1.78 R, each split into 32 slots of 11.25 degrees. A slot that
//     carries a 1 is an arc of bright particles over the middle 78 % of the slot; a 0 is empty. Slot j's centre is at the
//     angle 2 pi (j + 0.5) / 32 from the image's +x axis, towards +y (image y points down);
//   * 96 bits: ring 0 slots 0..7 hold the sync byte 0xB4 (tells the decoder the rotation and whether the image is
//     mirrored, as from a front camera), the other 88 hold the payload, whitened (XOR with a fixed LFSR sequence so the
//     rings never show long empty or full stretches): version (4) | flags (4) | secret (64) | CRC-16/CCITT (16).
//
// The 64-bit secret is a one-time pairing secret (a fresh one every 60 s, see docs/dockdrop-pairing.md): the scanner
// proves it knows it in a password-authenticated key exchange over the local network, so seeing the orb is what
// authorises the pairing; the secret never travels over the network.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace docky::pairing {

constexpr int kRings = 3, kSlots = 32, kBits = kRings * kSlots;
// Unevenly spaced (ratios 1.14 and 1.20): no change of scale lines up more than one ring with another, so the decoder
// cannot lock onto a wrong size.
constexpr float kRingRadius[kRings] = {1.30f, 1.48f, 1.78f};  // in orb radii
constexpr float kGapRadius[kRings] = {1.39f, 1.63f, 1.93f};   // between and beyond the rings (empty)
constexpr float kRingHalfWidth = 0.045f;                       // in orb radii
constexpr float kArcFill = 0.78f;                              // fraction of a slot a 1-arc covers
constexpr uint8_t kSync = 0xB4;
constexpr double kPi = 3.14159265358979323846;

struct OrbPayload {
    uint8_t version = 1;   // 4 bits
    uint8_t flags = 0;     // 4 bits (bit 0: the host offers clipboard sync; others reserved)
    uint64_t secret = 0;   // one-time pairing secret
    bool operator==(const OrbPayload& o) const { return version == o.version && flags == o.flags && secret == o.secret; }
};

using OrbBits = std::array<uint8_t, kBits>;  // [ring * kSlots + slot], 0 or 1

inline uint16_t Crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= static_cast<uint16_t>(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

// Whitening sequence for the 88 payload bits (x^7 + x^6 + 1, seed 0x5B).
inline std::array<uint8_t, kBits - 8> Whitening() {
    std::array<uint8_t, kBits - 8> w{};
    uint8_t s = 0x5B;
    for (auto& b : w) { const uint8_t bit = static_cast<uint8_t>(((s >> 6) ^ (s >> 5)) & 1); s = static_cast<uint8_t>(((s << 1) | bit) & 0x7F); b = bit; }
    return w;
}

inline OrbBits Encode(const OrbPayload& p) {
    uint8_t d[11] = {};
    d[0] = static_cast<uint8_t>(((p.version & 15) << 4) | (p.flags & 15));
    for (int i = 0; i < 8; ++i) d[1 + i] = static_cast<uint8_t>(p.secret >> (56 - 8 * i));
    const uint16_t crc = Crc16(d, 9);
    d[9] = static_cast<uint8_t>(crc >> 8); d[10] = static_cast<uint8_t>(crc);
    OrbBits bits{};
    for (int i = 0; i < 8; ++i) bits[i] = (kSync >> (7 - i)) & 1;
    const auto w = Whitening();
    for (int i = 0; i < 88; ++i) bits[8 + i] = static_cast<uint8_t>(((d[i / 8] >> (7 - i % 8)) & 1) ^ w[i]);
    return bits;
}

// Every ring shows at least 5 arcs and 5 gaps (the decoder's per-ring threshold needs both). The host draws a new
// secret until this holds (about 1 in 50 do not).
inline bool Balanced(const OrbBits& bits) {
    for (int ring = 0; ring < kRings; ++ring) {
        int ones = 0;
        for (int i = 0; i < kSlots; ++i) ones += bits[static_cast<size_t>(ring * kSlots + i)];
        if (ones < 5 || ones > kSlots - 5) return false;
    }
    return true;
}

// Payload from sampled slots ([ring][slot] as seen, unknown rotation / mirror). False when no rotation has the sync byte
// and a valid CRC.
inline bool DecodeBits(const OrbBits& seen, OrbPayload& out, int* rotation = nullptr, bool* mirrored = nullptr) {
    const auto w = Whitening();
    for (int m = 0; m < 2; ++m) for (int r = 0; r < kSlots; ++r) {
        auto at = [&](int i) {
            const int ring = i / kSlots, slot = i % kSlots;
            const int s = m ? ((r - slot) % kSlots + kSlots) % kSlots : (slot + r) % kSlots;
            return seen[static_cast<size_t>(ring * kSlots + s)];
        };
        uint8_t sync = 0;
        for (int i = 0; i < 8; ++i) sync = static_cast<uint8_t>((sync << 1) | at(i));
        if (sync != kSync) continue;
        uint8_t d[11] = {};
        for (int i = 0; i < 88; ++i) d[i / 8] = static_cast<uint8_t>(d[i / 8] | ((at(8 + i) ^ w[i]) << (7 - i % 8)));
        if (Crc16(d, 9) != ((d[9] << 8) | d[10])) continue;
        out.version = d[0] >> 4; out.flags = d[0] & 15; out.secret = 0;
        for (int i = 0; i < 8; ++i) out.secret = (out.secret << 8) | d[1 + i];
        if (rotation) *rotation = r;
        if (mirrored) *mirrored = m != 0;
        return true;
    }
    return false;
}

// ---- Image decoder (reference implementation; the scanner uses the same steps) ---------------------------------------
struct GrayImage {
    int w = 0, h = 0;
    std::vector<float> v;  // brightness 0..1, row-major: the brightest of R, G, B (the orb is blue; luminance would rank it
                           // below white text)
    float at(int x, int y) const { return v[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
    float bilinear(double x, double y) const {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
};

inline double RingScore(const GrayImage& img, double x0, double y0, double sc, int per = 6, const float* on = kRingRadius,
                        const float* off = kGapRadius) {
    constexpr int bins = kSlots * 2;
    double score = 0;
    for (int j = 0; j < 6; ++j) {
        const double r = (j < 3 ? on[j] : off[j - 3]) * sc;
        double v[bins] = {}, m = 0;
        for (int b = 0; b < bins; ++b) {
            double s2 = 0;
            for (int k = 0; k < per; ++k) {
                const double a = 2 * kPi * (b + (k + 0.5) / per) / bins;
                s2 += img.bilinear(x0 + r * std::cos(a), y0 + r * std::sin(a));
            }
            v[b] = s2 / per; m += v[b];
        }
        m /= bins;
        double var = 0; for (double x : v) var += (x - m) * (x - m);
        score += (j < 3 ? 1.0 : -1.0) * var / bins;
    }
    // The rings belong to an orb: bright inside (0.55 s), dark in the gap just outside it (1.20 s). Text, a card's edge
    // or a window frame can swing as much as a ring, but they have no orb in the middle.
    auto ringMean = [&](double r) {
        double m = 0;
        for (int k = 0; k < 48; ++k) { const double a = 2 * kPi * (k + 0.5) / 48; m += img.bilinear(x0 + r * std::cos(a), y0 + r * std::sin(a)); }
        return m / 48;
    };
    const double contrast = ringMean(0.55 * sc) - ringMean(1.20 * sc);  // 1.20: clear of the orb's orbiting streams (up to ~1.09)
    const double gate = std::clamp(contrast / 0.06, 0.0, 1.0);
    return score * gate * gate * (3 - 2 * gate);
}

struct DecodeInfo { double cx = 0, cy = 0, radius = 0; int rotation = 0; bool mirrored = false; double blobX = 0, blobY = 0, s0 = 0; };

// 1. The orb: a centre-surround box filter over several sizes finds it and its rough size (glare gradients cancel).
// 2. Centre and radius s jointly: where brightness along the three ring radii swings most (arcs and gaps) and along the
//    gaps between and beyond them least (RingScore); coarse on a blurred copy, then refined on the sharp image.
// 3. The slot grid's angular phase (any camera turn), then each slot's mean over its middle and the ring's width; slots
//    split into 1/0 by the largest gap between their sorted values per ring.
// 4. Every whole-slot rotation and mirror is tried against the sync byte and the CRC.
// About 80 ms for a 400 px image on one core; a live scanner refines from the previous frame's centre and scale.
// The orb's rough place and size: a centre-surround box filter over several sizes (glare gradients cancel).
inline void FindOrbBlob(const GrayImage& img, double& cx, double& cy, double& s0) {
    // -- 1. centre
    const int cell = std::max(2, std::min(img.w, img.h) / 64);
    const int gw = img.w / cell, gh = img.h / cell;
    std::vector<float> g(static_cast<size_t>(gw) * gh, 0.f);
    for (int y = 0; y < gh * cell; ++y) for (int x = 0; x < gw * cell; ++x) g[static_cast<size_t>(y / cell) * gw + x / cell] += img.v[static_cast<size_t>(y) * img.w + x];
    // Centre-surround on an integral image of the cells: mean over a box (the orb) minus the mean over a box 2.4 times
    // as large (its surroundings, rings included), over several sizes. A linear glare gradient cancels out, and the
    // dense orb outweighs the half-filled rings around it. The best box also gives the orb's size (box half-size h
    // cells ~ 0.9 R for a disc).
    std::vector<double> I(static_cast<size_t>(gw + 1) * (gh + 1), 0.0);
    for (int y = 0; y < gh; ++y) for (int x = 0; x < gw; ++x)
        I[static_cast<size_t>(y + 1) * (gw + 1) + x + 1] = g[static_cast<size_t>(y) * gw + x] + I[static_cast<size_t>(y) * (gw + 1) + x + 1] +
                                                        I[static_cast<size_t>(y + 1) * (gw + 1) + x] - I[static_cast<size_t>(y) * (gw + 1) + x];
    auto boxMean = [&](int x0, int y0, int x1, int y1) {  // inclusive cell range, clipped
        x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, gw - 1); y1 = std::min(y1, gh - 1);
        if (x1 < x0 || y1 < y0) return 0.0;
        const double sum = I[static_cast<size_t>(y1 + 1) * (gw + 1) + x1 + 1] - I[static_cast<size_t>(y0) * (gw + 1) + x1 + 1] -
                           I[static_cast<size_t>(y1 + 1) * (gw + 1) + x0] + I[static_cast<size_t>(y0) * (gw + 1) + x0];
        return sum / ((x1 - x0 + 1) * (y1 - y0 + 1));
    };
    cx = img.w / 2.0; cy = img.h / 2.0; s0 = 0; double bestResp = -1e18;
    // Boxes no smaller than 1/16 of the picture: the orb fills a good part of it when scanned, text and small lights
    // do not.
    for (int h = std::max(2, std::min(gw, gh) / 16); h * 2.4 * 2 < std::min(gw, gh); ++h) {
        const int H = static_cast<int>(h * 2.4 + 0.5);
        for (int y = H; y < gh - H; ++y) for (int x = H; x < gw - H; ++x) {
            const double resp = boxMean(x - h, y - h, x + h, y + h) - boxMean(x - H, y - H, x + H, y + H);
            if (resp > bestResp) { bestResp = resp; cx = (x + 0.5) * cell; cy = (y + 0.5) * cell; s0 = h * cell / 0.9; }
        }
    }
}

// Steps 1-2: where the orb is and how big (centre cx, cy and radius s, in pixels). False when nothing orb-like is there.
inline bool LocateOrb(const GrayImage& img, double& outX, double& outY, double& outS, DecodeInfo* info = nullptr,
                      const float* on = kRingRadius, const float* off = kGapRadius) {
    if (img.w < 32 || img.h < 32) return false;
    double cx, cy, s0;
    FindOrbBlob(img, cx, cy, s0);
    auto ringScore = [&](double x0, double y0, double sc) { return RingScore(img, x0, y0, sc, 6, on, off); };
    const double maxS = std::min(img.w, img.h) / 2.0 / 2.0;
    if (info) { info->blobX = cx; info->blobY = cy; info->s0 = s0; }
    if (s0 <= 0) return false;
    // The rings are thin, so the score peaks sharply: the coarse search runs on a blurred copy (box, ~0.05 s0), whose
    // wider peak a coarse grid cannot step over; the refinement below uses the sharp image.
    GrayImage soft = img;
    {
        const int rb = std::max(1, static_cast<int>(0.05 * s0 + 0.5));
        GrayImage t = img;
        for (int y = 0; y < img.h; ++y) {
            double acc = 0; int n = 0;
            for (int x = -rb; x < img.w + rb; ++x) {
                if (x + rb < img.w) { acc += img.at(x + rb, y); ++n; }
                if (x - rb - 1 >= 0) { acc -= img.at(x - rb - 1, y); --n; }
                if (x >= 0 && x < img.w && n > 0) t.v[static_cast<size_t>(y) * img.w + x] = static_cast<float>(acc / n);
            }
        }
        for (int x = 0; x < img.w; ++x) {
            double acc = 0; int n = 0;
            for (int y = -rb; y < img.h + rb; ++y) {
                if (y + rb < img.h) { acc += t.at(x, y + rb); ++n; }
                if (y - rb - 1 >= 0) { acc -= t.at(x, y - rb - 1); --n; }
                if (y >= 0 && y < img.h && n > 0) soft.v[static_cast<size_t>(y) * img.w + x] = static_cast<float>(acc / n);
            }
        }
    }
    // Refinement on the sharp image: centre on a shrinking grid, then the scale (scaleSpan: +- fraction searched first).
    auto refine = [&](double& x, double& y, double& sc, double scaleSpan) {
        double score = ringScore(x, y, sc);
        for (int it = 0; it < 4; ++it) {
            const double step = std::max(0.3, sc * 0.04 / (1 << it));
            double bx = x, by = y;
            for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) {
                const double v = ringScore(x + dx * step, y + dy * step, sc);
                if (v > score) { score = v; bx = x + dx * step; by = y + dy * step; }
            }
            x = bx; y = by;
            const double span = it == 0 ? scaleSpan : 0.036 / (1 << it);
            double bs = sc;
            for (int k = -12; k <= 12; ++k) {
                const double c = sc * (1 + k * span / 12);
                const double v = ringScore(x, y, c);
                if (v > score) { score = v; bs = c; }
            }
            sc = bs;
        }
        return score;
    };
    // Candidate A: straight from the blob (its centre and size are close when the orb is a dense disc, e.g. a dynamic
    // code's accumulated picture, where a coarse search on a blurred copy can be pulled off by the orb's halo).
    double ax = cx, ay = cy, as = s0;
    const double scoreA = refine(ax, ay, as, 0.15);
    // Candidate B: coarse search on the blurred copy (scales -40 % .. +50 %, centres within 0.3 s), then refined.
    double bestS = 0, bestScore = -1e9;
    {
        const double x0 = cx, y0 = cy;
        double bx = cx, by = cy;
        for (double sc = s0 * 0.6; sc < std::min(s0 * 1.5, maxS); sc *= 1.025) {
            const double step = std::max(0.75, 0.06 * sc);
            for (int dy = -5; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
                const double v = RingScore(soft, x0 + dx * step, y0 + dy * step, sc, 2, on, off);
                if (v > bestScore) { bestScore = v; bestS = sc; bx = x0 + dx * step; by = y0 + dy * step; }
            }
        }
        cx = bx; cy = by;
    }
    if (bestS <= 0) return false;
    bestScore = refine(cx, cy, bestS, 0.036);
    if (scoreA > bestScore) { bestScore = scoreA; cx = ax; cy = ay; bestS = as; }
    if (bestScore <= 0) return false;
    outX = cx; outY = cy; outS = bestS;
    if (info) { info->cx = cx; info->cy = cy; info->radius = bestS; }
    return true;
}

// Step 3: each slot's mean brightness at a given angular phase of the slot grid (in slots).
inline void SampleSlots(const GrayImage& img, double cx, double cy, double s, double phase, std::array<double, kBits>& val) {
    for (int ring = 0; ring < kRings; ++ring) {
        const double rr = kRingRadius[ring] * s, hw = kRingHalfWidth * s;
        for (int slot = 0; slot < kSlots; ++slot) {
            double sum = 0; int n = 0;
            for (int k = -4; k <= 4; ++k) {
                const double a = 2 * kPi * (slot + 0.5 + phase + k * 0.06) / kSlots;
                for (int j = -2; j <= 2; ++j) { const double r = rr + hw * j / 2.0; sum += img.bilinear(cx + r * std::cos(a), cy + r * std::sin(a)); ++n; }
            }
            val[static_cast<size_t>(ring * kSlots + slot)] = sum / n;
        }
    }
}

// The camera may be turned by any angle: the slot grid's phase is the one at which the slot means differ most (slot
// centres on arcs and on gaps, not on their edges); whole-slot turns are found by DecodeBits.
inline double BestPhase(const GrayImage& img, double cx, double cy, double s, double* contrast = nullptr) {
    double bestPhase = 0, bestVar = -1;
    std::array<double, kBits> val{};
    for (int k = 0; k < 16; ++k) {
        const double phase = k / 16.0 - 0.5;
        SampleSlots(img, cx, cy, s, phase, val);
        double var = 0;
        for (int ring = 0; ring < kRings; ++ring) {
            double m = 0; for (int i = 0; i < kSlots; ++i) m += val[static_cast<size_t>(ring * kSlots + i)]; m /= kSlots;
            for (int i = 0; i < kSlots; ++i) { const double d = val[static_cast<size_t>(ring * kSlots + i)] - m; var += d * d; }
        }
        if (var > bestVar) { bestVar = var; bestPhase = phase; }
    }
    if (contrast) *contrast = std::sqrt(bestVar / kBits);
    return bestPhase;
}

// Slots to bits: per ring, split at the largest gap between the sorted values.
inline OrbBits ThresholdSlots(const std::array<double, kBits>& val) {
    OrbBits seen{};
    for (int ring = 0; ring < kRings; ++ring) {
        std::array<double, kSlots> sorted{};
        for (int i = 0; i < kSlots; ++i) sorted[static_cast<size_t>(i)] = val[static_cast<size_t>(ring * kSlots + i)];
        std::sort(sorted.begin(), sorted.end());
        double gap = -1, thr = 0;
        for (int i = 3; i < kSlots - 3; ++i) if (sorted[static_cast<size_t>(i + 1)] - sorted[static_cast<size_t>(i)] > gap) { gap = sorted[static_cast<size_t>(i + 1)] - sorted[static_cast<size_t>(i)]; thr = 0.5 * (sorted[static_cast<size_t>(i + 1)] + sorted[static_cast<size_t>(i)]); }
        for (int i = 0; i < kSlots; ++i) seen[static_cast<size_t>(ring * kSlots + i)] = val[static_cast<size_t>(ring * kSlots + i)] > thr ? 1 : 0;
    }
    return seen;
}

// One picture of a static code (every arc lit at once).
inline bool DecodeImage(const GrayImage& img, OrbPayload& out, DecodeInfo* info = nullptr) {
    double cx, cy, s;
    if (!LocateOrb(img, cx, cy, s, info)) return false;
    std::array<double, kBits> val{};
    SampleSlots(img, cx, cy, s, BestPhase(img, cx, cy, s), val);
    int rot = 0; bool mir = false;
    if (!DecodeBits(ThresholdSlots(val), out, &rot, &mir)) return false;
    if (info) { info->rotation = rot; info->mirrored = mir; }
    return true;
}

// ---- Dynamic code ---------------------------------------------------------------------------------------------------
// The orb can show its code a little at a time: every arc flares briefly, each at its own moment (kFlarePeriod s, lit
// for about a quarter of it), so a single frame shows only a scatter of sparks round the orb. The scanner watches for
// a moment and keeps, per pixel, the brightest it has seen: places with an arc have flared at least once, empty places
// never do. That picture is then read like a static code (the same sync byte and CRC), so this decoder also reads a
// static orb, from its first frame.
constexpr double kFlarePeriod = 1.2, kFlareOn = 0.30;  // s

// Phase (0..1) of an arc's flare cycle: fixed per (ring, slot), spread so that neighbours flare at different times.
inline double FlarePhase(int ring, int slot) {
    uint32_t h = static_cast<uint32_t>(ring * 977 + slot * 131 + 7);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xFFFF) / 65536.0;
}
// Brightness of an arc's flare at time t (s): eases in over 60 ms, holds, eases out.
inline double FlareLevel(int ring, int slot, double t) {
    double x = t / kFlarePeriod + FlarePhase(ring, slot);
    x -= std::floor(x);
    const double on = kFlareOn / kFlarePeriod;
    auto ease = [](double v) { v = std::clamp(v, 0.0, 1.0); return v * v * (3 - 2 * v); };
    return ease(x / 0.05) * (1 - ease((x - on + 0.05) / 0.05));
}

class TemporalDecoder {
public:
    void Reset() { acc_.w = acc_.h = 0; acc_.v.clear(); frames_ = 0; havePrev_ = false; }
    int Frames() const { return frames_; }
    const GrayImage& Accumulated() const { return acc_; }
    // Feeds one camera frame. True once the code is complete (out filled). Every pixel keeps the brightest it has
    // been lately (a running maximum that fades by 1.5 % per frame, so a moving phone leaves no lasting trail): after
    // one flare cycle the picture holds every arc, and is read like a static code. A static orb decodes on frame 1.
    bool Feed(const GrayImage& img, OrbPayload& out, DecodeInfo* info = nullptr) {
        if (acc_.w != img.w || acc_.h != img.h) { acc_ = img; frames_ = 0; }
        else for (size_t i = 0; i < acc_.v.size(); ++i) acc_.v[i] = std::max(acc_.v[i] * 0.985f, img.v[i]);
        ++frames_;
        if (frames_ > 1 && frames_ % decodeEvery_ != 0) return false;  // decoding costs ~80 ms; frames are cheap
        // A reading counts only when it repeats (the next attempt reads the same payload): the 16-bit CRC, tried
        // against 64 turns and 2 mirrors on every attempt, lets an occasional wrong reading through.
        OrbPayload p;
        if (!DecodeImage(acc_, p, info)) return false;
        if (havePrev_ && prev_ == p) { out = p; return true; }
        prev_ = p; havePrev_ = true;
        return false;
    }
    void SetDecodeEvery(int n) { decodeEvery_ = std::max(1, n); }

private:
    GrayImage acc_;
    int frames_ = 0, decodeEvery_ = 3;
    OrbPayload prev_;
    bool havePrev_ = false;
};

// ---- Satellite code (geometric) ------------------------------------------------------------------------------------
// The same payload carried by position instead of light: three orbits round the orb (1.32 R, 1.56 R, 1.95 R; uneven,
// like the rings), each with 8 bright satellites at 8 of 64 evenly spaced positions, never two side by side (there is
// always an empty position between neighbours: a camera tells satellites 2 positions apart easily, 1 apart hardly).
// Such a set is any 8 of 56 places with the gaps put back (slot_i = c_i + i), C(56, 8) = 1.42e9 choices per orbit;
// three orbits carry 88 bits = version (4) | flags (4) | secret (64) | CRC-16 (16), as one number in base C(56, 8).
// The whole constellation turns (positions are relative) and may be mirrored; the decoder tries every whole-position
// turn and both mirrors against the CRC (a scanner accepts a reading only once it repeats). Every frame is a complete
// code.
constexpr int kSatOrbits = 3, kSatPositions = 64, kSatPerOrbit = 8, kSatPlaces = kSatPositions - kSatPerOrbit;
constexpr float kSatRadius[kSatOrbits] = {1.32f, 1.56f, 1.95f};
constexpr float kSatGap[kSatOrbits] = {1.44f, 1.75f, 2.12f};
using SatSlots = std::array<std::array<int, kSatPerOrbit>, kSatOrbits>;  // positions per orbit, ascending

inline uint64_t Binom(int n, int k) {
    if (k < 0 || k > n) return 0;
    uint64_t r = 1;
    for (int i = 1; i <= k; ++i) r = r * static_cast<uint64_t>(n - k + i) / static_cast<uint64_t>(i);
    return r;
}
// Combinatorial number system: value <-> the ascending places c0 < c1 < ... < c7 (of n) with value = sum C(c_i, i + 1).
inline std::array<int, kSatPerOrbit> Unrank(uint64_t v, int n) {
    std::array<int, kSatPerOrbit> c{};
    int m = n - 1;
    for (int i = kSatPerOrbit; i >= 1; --i) {
        while (Binom(m, i) > v) --m;
        c[static_cast<size_t>(i - 1)] = m;
        v -= Binom(m, i);
        --m;
    }
    return c;
}
inline uint64_t Rank(const std::array<int, kSatPerOrbit>& c) {
    uint64_t v = 0;
    for (int i = 0; i < kSatPerOrbit; ++i) v += Binom(c[static_cast<size_t>(i)], i + 1);
    return v;
}

// 88-bit payload <-> three digits in base M = C(56, 8) (M^3 > 2^88). Little arithmetic on 32-bit limbs, high first.
inline SatSlots EncodeSat(const OrbPayload& p) {
    uint8_t d[11] = {};
    d[0] = static_cast<uint8_t>(((p.version & 15) << 4) | (p.flags & 15));
    for (int i = 0; i < 8; ++i) d[1 + i] = static_cast<uint8_t>(p.secret >> (56 - 8 * i));
    const uint16_t crc = Crc16(d, 9);
    d[9] = static_cast<uint8_t>(crc >> 8); d[10] = static_cast<uint8_t>(crc);
    uint32_t limb[3] = {0, 0, 0};  // 88 bits: limb[0] holds the top 24
    limb[0] = (static_cast<uint32_t>(d[0]) << 16) | (static_cast<uint32_t>(d[1]) << 8) | d[2];
    limb[1] = (static_cast<uint32_t>(d[3]) << 24) | (static_cast<uint32_t>(d[4]) << 16) | (static_cast<uint32_t>(d[5]) << 8) | d[6];
    limb[2] = (static_cast<uint32_t>(d[7]) << 24) | (static_cast<uint32_t>(d[8]) << 16) | (static_cast<uint32_t>(d[9]) << 8) | d[10];
    const uint64_t M = Binom(kSatPlaces, kSatPerOrbit);
    SatSlots out{};
    for (int o = 0; o < kSatOrbits; ++o) {  // digit o = number mod M, then number /= M
        uint64_t rem = 0;
        for (int i = 0; i < 3; ++i) { const uint64_t cur = (rem << 32) | limb[i]; limb[i] = static_cast<uint32_t>(cur / M); rem = cur % M; }
        auto c = Unrank(rem, kSatPlaces);
        for (int i = 0; i < kSatPerOrbit; ++i) c[static_cast<size_t>(i)] += i;  // the gaps back in
        out[static_cast<size_t>(o)] = c;
    }
    return out;
}

// Positions as seen (any common turn, maybe mirrored) -> payload. False when no turn / mirror gives a valid reading.
inline bool DecodeSatSlots(const SatSlots& seen, OrbPayload& out) {
    const uint64_t M = Binom(kSatPlaces, kSatPerOrbit);
    for (int m = 0; m < 2; ++m) for (int r = 0; r < kSatPositions; ++r) {
        uint64_t digit[kSatOrbits] = {};
        bool ok = true;
        for (int o = 0; o < kSatOrbits && ok; ++o) {
            std::array<int, kSatPerOrbit> c{};
            for (int i = 0; i < kSatPerOrbit; ++i) {
                const int sidx = seen[static_cast<size_t>(o)][static_cast<size_t>(i)];
                c[static_cast<size_t>(i)] = m ? ((r - sidx) % kSatPositions + kSatPositions) % kSatPositions : (sidx - r + kSatPositions) % kSatPositions;
            }
            std::sort(c.begin(), c.end());
            for (int i = 0; i < kSatPerOrbit && ok; ++i) {
                c[static_cast<size_t>(i)] -= i;  // gaps out: places must still rise and stay below 56
                if (c[static_cast<size_t>(i)] < 0 || c[static_cast<size_t>(i)] >= kSatPlaces || (i > 0 && c[static_cast<size_t>(i)] <= c[static_cast<size_t>(i - 1)])) ok = false;
            }
            if (ok) digit[o] = Rank(c);
            if (digit[o] >= M) ok = false;
        }
        if (!ok) continue;
        uint32_t limb[3] = {0, 0, 0};  // number = d0 + M (d1 + M d2)
        for (int o = kSatOrbits - 1; o >= 0; --o) {
            uint64_t carry = digit[o];
            for (int i = 2; i >= 0; --i) { const uint64_t cur = static_cast<uint64_t>(limb[i]) * M + carry; limb[i] = static_cast<uint32_t>(cur); carry = cur >> 32; }
            if (carry) { ok = false; break; }
        }
        if (!ok || (limb[0] >> 24)) continue;  // more than 88 bits: not a payload
        uint8_t d[11] = {};
        d[0] = static_cast<uint8_t>(limb[0] >> 16); d[1] = static_cast<uint8_t>(limb[0] >> 8); d[2] = static_cast<uint8_t>(limb[0]);
        for (int i = 0; i < 4; ++i) { d[3 + i] = static_cast<uint8_t>(limb[1] >> (24 - 8 * i)); d[7 + i] = static_cast<uint8_t>(limb[2] >> (24 - 8 * i)); }
        if (Crc16(d, 9) != ((d[9] << 8) | d[10])) continue;
        out.version = d[0] >> 4; out.flags = d[0] & 15; out.secret = 0;
        for (int i = 0; i < 8; ++i) out.secret = (out.secret << 8) | d[1 + i];
        return true;
    }
    return false;
}

// One picture: find the orb (by the three satellite orbits), then on each orbit the 8 brightest peaks of the brightness
// along it (satellites; a comet tail trails each, dimmer than its head), the common angular phase of all 24, and the
// positions.
// The orb's disc against the gap outside it, measured right at a candidate edge s: mean just inside (0.94 s) minus just
// outside (1.06 s); largest where s is the disc's edge.
inline double DiscEdge(const GrayImage& img, double x0, double y0, double s) {
    double in = 0, out = 0;
    for (int k = 0; k < 64; ++k) {
        const double a = 2 * kPi * (k + 0.5) / 64, c = std::cos(a), sn = std::sin(a);
        in += img.bilinear(x0 + 0.94 * s * c, y0 + 0.94 * s * sn) + img.bilinear(x0 + 0.80 * s * c, y0 + 0.80 * s * sn);
        out += img.bilinear(x0 + 1.06 * s * c, y0 + 1.06 * s * sn) + img.bilinear(x0 + 1.18 * s * c, y0 + 1.18 * s * sn);
    }
    return (in - out) / 128;
}

// Satellites are a few points, not half-filled rings: brightness variance along an orbit is weak evidence, and a wrong
// scale can beat the right one. So the orb's own disc fixes centre and size first (its edge), and the orbits only
// refine that within +-8 %.
inline bool LocateSatOrb(const GrayImage& img, double& cx, double& cy, double& s, DecodeInfo* info = nullptr) {
    double s0;
    FindOrbBlob(img, cx, cy, s0);
    if (info) { info->blobX = cx; info->blobY = cy; info->s0 = s0; }
    if (s0 <= 0) return false;
    double best = -1e9, bx = cx, by = cy, bs = s0;
    for (double sc = s0 * 0.65; sc < s0 * 1.5; sc *= 1.02) {  // disc: coarse centre grid and every size
        const double step = std::max(1.0, 0.05 * sc);
        for (int dy = -5; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
            const double v = DiscEdge(img, cx + dx * step, cy + dy * step, sc);
            if (v > best) { best = v; bx = cx + dx * step; by = cy + dy * step; bs = sc; }
        }
    }
    if (best <= 0.02) return false;
    cx = bx; cy = by; s = bs;
    double score = RingScore(img, cx, cy, s, 6, kSatRadius, kSatGap);
    for (int it = 0; it < 4; ++it) {  // orbits: centre and scale, narrowly
        const double step = std::max(0.3, s * 0.02 / (1 << it));
        double nx = cx, ny = cy;
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) {
            const double v = RingScore(img, cx + dx * step, cy + dy * step, s, 6, kSatRadius, kSatGap);
            if (v > score) { score = v; nx = cx + dx * step; ny = cy + dy * step; }
        }
        cx = nx; cy = ny;
        const double span = it == 0 ? 0.08 : 0.02 / (1 << it);
        double ns = s;
        for (int k = -12; k <= 12; ++k) {
            const double c = s * (1 + k * span / 12);
            const double v = RingScore(img, cx, cy, c, 6, kSatRadius, kSatGap);
            if (v > score) { score = v; ns = c; }
        }
        s = ns;
    }
    // Finally the satellites themselves: centre and scale where the 8 brightest points of each orbit are brightest
    // (sparse points: their own peaks are the sharpest measure of where the orbits run).
    auto peaks = [&](double x0, double y0, double sc) {
        double total = 0;
        for (int o = 0; o < kSatOrbits; ++o) {
            std::array<double, 360> prof{};
            const double r = kSatRadius[o] * sc;
            for (int k = 0; k < 360; ++k) {
                const double a = 2 * kPi * k / 360, c = std::cos(a), sn = std::sin(a);
                double m = 0;
                for (int j = -1; j <= 1; ++j) m = std::max(m, static_cast<double>(img.bilinear(x0 + (r + 0.012 * sc * j) * c, y0 + (r + 0.012 * sc * j) * sn)));
                prof[static_cast<size_t>(k)] = m;
            }
            std::array<double, 360> sorted = prof;
            std::nth_element(sorted.begin(), sorted.begin() + 180, sorted.end());
            const double median = sorted[180];
            std::sort(sorted.begin(), sorted.end(), std::greater<double>());
            for (int i = 0; i < 24; ++i) total += sorted[static_cast<size_t>(i)] - median;  // ~3 samples per satellite head
        }
        return total;
    };
    double pk = peaks(cx, cy, s);
    for (int it = 0; it < 3; ++it) {
        const double span = it == 0 ? 0.06 : 0.02;
        double ns = s;
        for (int k = -12; k <= 12; ++k) { const double c = s * (1 + k * span / 12); const double v = peaks(cx, cy, c); if (v > pk) { pk = v; ns = c; } }
        s = ns;
        const double step = std::max(0.3, s * 0.01 / (1 << it));
        double nx = cx, ny = cy;
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) { const double v = peaks(cx + dx * step, cy + dy * step, s); if (v > pk) { pk = v; nx = cx + dx * step; ny = cy + dy * step; } }
        cx = nx; cy = ny;
    }
    if (info) { info->cx = cx; info->cy = cy; info->radius = s; }
    return score > 0;
}

inline bool DecodeSatImage(const GrayImage& img, OrbPayload& out, DecodeInfo* info = nullptr, SatSlots* seenOut = nullptr) {
    double cx, cy, s;
    if (!LocateSatOrb(img, cx, cy, s, info)) return false;
    constexpr int N = 1024;  // samples round an orbit
    std::array<double, kSatOrbits * kSatPerOrbit> angles{};
    std::array<double, kSatOrbits> orbitR{};
    // Satellites on each orbit at the current centre and scale: angles (in positions) and each orbit's radius.
    auto detect = [&]() -> bool {
    for (int o = 0; o < kSatOrbits; ++o) {
        std::vector<double> prof(N);
        // Each orbit's own radius, within +-5 % of the estimate: where its brightest points are brightest (a small
        // error in the overall scale grows with the radius; the outer orbit would otherwise miss its satellites).
        double r = kSatRadius[o] * s;
        {
            double bestR = r, bestV = -1;
            for (int q = -10; q <= 10; ++q) {
                const double rr = kSatRadius[o] * s * (1 + q * 0.005);
                std::array<double, 256> pr{};
                for (int k = 0; k < 256; ++k) { const double a = 2 * kPi * k / 256; pr[static_cast<size_t>(k)] = img.bilinear(cx + rr * std::cos(a), cy + rr * std::sin(a)); }
                std::sort(pr.begin(), pr.end(), std::greater<double>());
                double v = 0; for (int i = 0; i < 16; ++i) v += pr[static_cast<size_t>(i)];
                if (v > bestV) { bestV = v; bestR = rr; }
            }
            r = bestR;
        }
        orbitR[static_cast<size_t>(o)] = r;
        const double hw = 0.025 * s;
        for (int k = 0; k < N; ++k) {
            const double a = 2 * kPi * k / N, c = std::cos(a), sn = std::sin(a);
            double m = 0;
            for (int j = -2; j <= 2; ++j) m = std::max(m, static_cast<double>(img.bilinear(cx + (r + hw * j / 2) * c, cy + (r + hw * j / 2) * sn)));
            prof[static_cast<size_t>(k)] = m;
        }
        std::vector<double> sm(N);  // light smoothing (a satellite spans a few samples)
        for (int k = 0; k < N; ++k) sm[static_cast<size_t>(k)] = (prof[static_cast<size_t>((k + N - 2) % N)] + 2 * prof[static_cast<size_t>((k + N - 1) % N)] + 3 * prof[static_cast<size_t>(k)] + 2 * prof[static_cast<size_t>((k + 1) % N)] + prof[static_cast<size_t>((k + 2) % N)]) / 9;
        // the 8 highest true peaks (local maxima over +-0.45 positions: a comet's tail falls off steadily behind its
        // head, so no point of it is a peak), at least 1.4 positions apart (the code never puts two side by side)
        std::vector<int> idx;
        const int win = std::max(1, static_cast<int>(0.45 * N / kSatPositions));
        for (int k = 0; k < N; ++k) {
            bool peak = true;
            for (int d = 1; d <= win && peak; ++d) peak = sm[static_cast<size_t>(k)] >= sm[static_cast<size_t>((k + d) % N)] && sm[static_cast<size_t>(k)] >= sm[static_cast<size_t>((k - d + N) % N)];
            if (peak) idx.push_back(k);
        }
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return sm[static_cast<size_t>(a)] > sm[static_cast<size_t>(b)]; });
        const int minSep = static_cast<int>(1.4 * N / kSatPositions);
        std::vector<int> picked;
        for (int k : idx) {
            bool apart = true;
            for (int q : picked) { int dd = std::abs(k - q); dd = std::min(dd, N - dd); if (dd < minSep) { apart = false; break; } }
            if (apart) picked.push_back(k);
            if (static_cast<int>(picked.size()) == kSatPerOrbit) break;
        }
        if (static_cast<int>(picked.size()) < kSatPerOrbit) return false;
        for (int i = 0; i < kSatPerOrbit; ++i) angles[static_cast<size_t>(o * kSatPerOrbit + i)] = picked[static_cast<size_t>(i)] * double(kSatPositions) / N;  // in positions
    }
    return true;
    };
    // The centre and scale from the satellites themselves (the orb's edge is soft; its centre was only good to a few
    // pixels, enough to find the satellites but not to read their positions): each head's distance from the centre
    // along its own ray, then a least-squares fit of a centre shift d and scale s so that |head - centre| = k_orbit s.
    // Three rounds.
    for (int round = 0; round < 3; ++round) {
        if (!detect()) return false;
        double A[3][3] = {}, B[3] = {};
        for (int o = 0; o < kSatOrbits; ++o) for (int i = 0; i < kSatPerOrbit; ++i) {
            const double a = 2 * kPi * angles[static_cast<size_t>(o * kSatPerOrbit + i)] / kSatPositions;
            const double ux = std::cos(a), uy = std::sin(a);
            double bestR = orbitR[static_cast<size_t>(o)], bestV = -1;  // the head along its ray
            for (int q = -12; q <= 12; ++q) {
                const double rr = orbitR[static_cast<size_t>(o)] * (1 + q * 0.006);
                double v = 0;
                for (int t = -2; t <= 2; ++t) {  // a little across the ray too
                    const double at = a + t * 0.004;
                    v += img.bilinear(cx + rr * std::cos(at), cy + rr * std::sin(at)) * (t == 0 ? 2 : 1);
                }
                if (v > bestV) { bestV = v; bestR = rr; }
            }
            // r_i = u . d + k s   (d: centre shift, s: scale)
            const double row[3] = {ux, uy, static_cast<double>(kSatRadius[o])};
            for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) A[r][c] += row[r] * row[c]; B[r] += row[r] * bestR; }
        }
        // solve the 3x3 normal equations (Cramer)
        auto det3 = [](const double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
        const double D = det3(A);
        if (std::fabs(D) < 1e-9) break;
        double sol[3];
        for (int c = 0; c < 3; ++c) {
            double M[3][3];
            for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) M[r][k] = k == c ? B[r] : A[r][k];
            sol[c] = det3(M) / D;
        }
        if (!(sol[2] > 0.8 * s && sol[2] < 1.25 * s)) break;  // implausible: keep what we had
        cx += sol[0]; cy += sol[1]; s = sol[2];
    }
    if (!detect()) return false;

    // common phase: circular mean of the fractional parts
    double sx = 0, sy = 0;
    for (double a : angles) { sx += std::cos(2 * kPi * a); sy += std::sin(2 * kPi * a); }
    const double phase = std::atan2(sy, sx) / (2 * kPi);
    SatSlots seen{};
    for (int o = 0; o < kSatOrbits; ++o) {
        for (int i = 0; i < kSatPerOrbit; ++i) {
            const double a = angles[static_cast<size_t>(o * kSatPerOrbit + i)] - phase;
            seen[static_cast<size_t>(o)][static_cast<size_t>(i)] = (static_cast<int>(std::lround(a)) % kSatPositions + kSatPositions) % kSatPositions;
        }
    }
    if (info) { info->cx = cx; info->cy = cy; info->radius = s; }
    if (seenOut) *seenOut = seen;
    return DecodeSatSlots(seen, out);
}

}  // namespace docky::pairing
