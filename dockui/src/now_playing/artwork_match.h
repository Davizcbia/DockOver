#pragma once
// Image check for catalog matches: a 64-bit difference hash (dHash) of the cover the player reports
// (Windows media session thumbnail) is compared with each catalog candidate's cover, so a same-named
// album by the wrong artist, a different edition with other art, or an unrelated result is rejected.
// Pure functions, unit tested in tests/now_playing_hls_test.cpp.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nowplaying::match {

// BGRA (any alpha) pixels; the centred square is used so 16:9 thumbnails with the cover in the middle
// (some web players) compare with square catalog art.
inline uint64_t DHash(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
    if (!bgra || !w || !h) return 0;
    const uint32_t side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    float g[8][9];
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 9; ++gx) {
            // Box average of the cell (area sampling, so the result does not depend on the source size).
            const uint32_t ax = x0 + side * gx / 9, bx = std::max(ax + 1, x0 + side * (gx + 1) / 9);
            const uint32_t ay = y0 + side * gy / 8, by = std::max(ay + 1, y0 + side * (gy + 1) / 8);
            double sum = 0; uint32_t n = 0;
            const uint32_t step = std::max(1u, (bx - ax) / 12);
            for (uint32_t y = ay; y < by; y += step)
                for (uint32_t x = ax; x < bx; x += step) {
                    const uint8_t* p = bgra + size_t(y) * stride + size_t(x) * 4;
                    sum += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; ++n;
                }
            g[gy][gx] = n ? float(sum / n) : 0.f;
        }
    }
    uint64_t hash = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) hash = (hash << 1) | (g[y][x] > g[y][x + 1] ? 1u : 0u);
    return hash;
}

inline int Distance(uint64_t a, uint64_t b) { return std::popcount(a ^ b); }

// Same cover (resized, recompressed) stays well under this; different covers average ~32.
constexpr int kSameCover = 14;

// Dominant colours of a cover for the flowing background: k-means (k = count) on a coarse sample, clusters ordered
// by size weighted toward colourful ones. Returns 0xRRGGBB values (fewer when the image has fewer distinct colours).
// shares (optional) receives each returned colour's fraction of the image's pixels.
inline std::vector<uint32_t> Palette(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, int count = 4, std::vector<float>* shares = nullptr) {
    struct P { float r, g, b; };
    std::vector<P> px;
    if (!bgra || !w || !h || count < 1) return {};
    const uint32_t step = std::max(1u, std::max(w, h) / 48);
    for (uint32_t y = 0; y < h; y += step)
        for (uint32_t x = 0; x < w; x += step) { const uint8_t* p = bgra + size_t(y) * stride + size_t(x) * 4; px.push_back({p[2] / 255.f, p[1] / 255.f, p[0] / 255.f}); }
    // Start from points spread over brightness.
    std::vector<P> c(static_cast<size_t>(count));
    std::vector<P> sorted = px;
    std::sort(sorted.begin(), sorted.end(), [](const P& a, const P& b) { return a.r + a.g + a.b < b.r + b.g + b.b; });
    for (int k = 0; k < count; ++k) c[size_t(k)] = sorted[(sorted.size() - 1) * size_t(2 * k + 1) / size_t(2 * count)];
    std::vector<int> n(static_cast<size_t>(count));
    for (int iter = 0; iter < 10; ++iter) {
        std::vector<P> sum(size_t(count), {0, 0, 0}); std::fill(n.begin(), n.end(), 0);
        for (const auto& p : px) {
            int best = 0; float bd = 1e9f;
            for (int k = 0; k < count; ++k) { const float d = (p.r - c[k].r) * (p.r - c[k].r) + (p.g - c[k].g) * (p.g - c[k].g) + (p.b - c[k].b) * (p.b - c[k].b); if (d < bd) { bd = d; best = k; } }
            sum[size_t(best)].r += p.r; sum[size_t(best)].g += p.g; sum[size_t(best)].b += p.b; ++n[size_t(best)];
        }
        for (int k = 0; k < count; ++k) if (n[size_t(k)]) c[size_t(k)] = {sum[k].r / n[k], sum[k].g / n[k], sum[k].b / n[k]};
    }
    std::vector<std::pair<float, uint32_t>> ranked; std::vector<std::pair<uint32_t, float>> share;
    for (int k = 0; k < count; ++k) {
        if (!n[size_t(k)]) continue;
        const float mx = std::max({c[k].r, c[k].g, c[k].b}), mn = std::min({c[k].r, c[k].g, c[k].b});
        const float saturation = mx > 0 ? (mx - mn) / mx : 0;
        const uint32_t rgb = (uint32_t(std::lround(c[k].r * 255)) << 16) | (uint32_t(std::lround(c[k].g * 255)) << 8) | uint32_t(std::lround(c[k].b * 255));
        ranked.push_back({float(n[size_t(k)]) * (.55f + saturation) * (.35f + mx), rgb});  // near-black clusters count less
        share.push_back({rgb, float(n[size_t(k)]) / float(std::max<size_t>(1, px.size()))});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<uint32_t> out;
    for (const auto& r : ranked) if (std::find(out.begin(), out.end(), r.second) == out.end()) out.push_back(r.second);
    if (shares) {
        shares->assign(out.size(), 0.f);
        for (const auto& [rgb, f] : share) (*shares)[size_t(std::find(out.begin(), out.end(), rgb) - out.begin())] += f;
    }
    return out;
}

}  // namespace nowplaying::match
