// DockOver Orb Code: encoder / decoder checks, and a decoder for real renders.
//   docky-orb-test                     unit checks (bits round trip, synthetic camera images)
//   docky-orb-test <file.bmp> [hex]    decodes a docky-orb snapshot; with hex: checks the secret
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>

#include "../src/pairing/orb_code.h"

using namespace docky::pairing;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// A camera's view of the orb, drawn on the CPU: a dark card, a dense noisy particle sphere, the code arcs as dotted
// particle strokes, then turned (any angle), scaled, moved, optionally mirrored, blurred, with sensor noise and glare.
static double g_cx = 0, g_cy = 0;
// lit: per slot brightness 0..1 (dynamic code: the arcs flaring now); null = every arc fully lit. at: fixed centre
// (a sequence from a hand-held phone: nearly the same place every frame).
static GrayImage Synthetic(const OrbBits& bits, int size, double R, double turn, bool mirror, double blur, double noise, std::mt19937& rng,
                           const double* lit = nullptr, const double* at = nullptr) {
    GrayImage img; img.w = img.h = size; img.v.assign(static_cast<size_t>(size) * size, 0.f);
    std::uniform_real_distribution<double> u(0, 1);
    std::normal_distribution<double> n01(0, 1);
    const double cx = at ? at[0] : size * (0.45 + 0.1 * u(rng)), cy = at ? at[1] : size * (0.45 + 0.1 * u(rng));
    g_cx = cx; g_cy = cy;
    auto splat = [&](double x, double y, double v) {
        if (mirror) x = 2 * cx - x;
        const int xi = static_cast<int>(x), yi = static_cast<int>(y);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int xx = xi + dx, yy = yi + dy;
            if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
            const double d2 = (xx + 0.5 - x) * (xx + 0.5 - x) + (yy + 0.5 - y) * (yy + 0.5 - y);
            float& p = img.v[static_cast<size_t>(yy) * size + xx];
            p = static_cast<float>(std::min(1.0, p + v * std::exp(-d2 / 0.8)));
        }
    };
    // orb: dense, brighter towards the middle
    const int orbN = static_cast<int>(R * R * 1.4);
    for (int i = 0; i < orbN; ++i) {
        const double a = 2 * kPi * u(rng), r = R * std::sqrt(u(rng));
        splat(cx + r * std::cos(a), cy + r * std::sin(a), 0.35 + 0.5 * u(rng) * (1 - 0.5 * r / R));
    }
    // a faint halo of stray particles
    for (int i = 0; i < static_cast<int>(R * 6); ++i) {
        const double a = 2 * kPi * u(rng), r = R * (1.0 + 1.0 * u(rng));
        splat(cx + r * std::cos(a), cy + r * std::sin(a), 0.25 * u(rng));
    }
    // code arcs
    for (int ring = 0; ring < kRings; ++ring) for (int slot = 0; slot < kSlots; ++slot) {
        if (!bits[static_cast<size_t>(ring * kSlots + slot)]) continue;
        const double level = lit ? lit[ring * kSlots + slot] : 1.0;
        if (level <= 0.02) continue;
        const double r0 = kRingRadius[ring] * R;
        const int dots = static_cast<int>(2 * kPi * r0 / kSlots * kArcFill * 3);
        for (int k = 0; k < dots; ++k) {
            const double f = (u(rng) - 0.5) * kArcFill;
            const double a = 2 * kPi * (slot + 0.5 + f) / kSlots + turn;
            const double r = r0 + (u(rng) - 0.5) * 2 * kRingHalfWidth * R * 0.8;
            splat(cx + r * std::cos(a), cy + r * std::sin(a), (0.55 + 0.4 * u(rng)) * level);
        }
    }
    // blur (box, repeated), glare, noise
    for (int pass = 0; pass < static_cast<int>(blur); ++pass) {
        GrayImage t = img;
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            float s = 0; for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) s += t.at(x + dx, y + dy);
            img.v[static_cast<size_t>(y) * size + x] = s / 9;
        }
    }
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float& p = img.v[static_cast<size_t>(y) * size + x];
        p = static_cast<float>(std::clamp(p + 0.12 * x / size + noise * n01(rng), 0.0, 1.0));
    }
    return img;
}

static bool LoadBmp(const wchar_t* path, GrayImage& img) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1 || ih.biBitCount != 32) { fclose(f); return false; }
    fseek(f, static_cast<long>(fh.bfOffBits), SEEK_SET);
    const int w = ih.biWidth, h = std::abs(ih.biHeight);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    const bool ok = fread(px.data(), 1, px.size(), f) == px.size();
    fclose(f);
    if (!ok) return false;
    img.w = w; img.h = h; img.v.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const int sy = ih.biHeight > 0 ? h - 1 - y : y;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &px[(static_cast<size_t>(sy) * w + x) * 4];
            // the brightest channel: the orb glows blue (luminance would make it dimmer than white text)
            img.v[static_cast<size_t>(y) * w + x] = std::max({p[0], p[1], p[2]}) / 255.f;
        }
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--sat") {  // --sat <file.bmp> <hex>: a satellite code picture
        GrayImage img;
        if (!LoadBmp(argv[2], img)) { std::printf("cannot read %ls\n", argv[2]); return 2; }
        OrbPayload p; DecodeInfo di; SatSlots seen{};
        const bool ok = DecodeSatImage(img, p, &di, &seen);
        const unsigned long long want = std::wcstoull(argv[3], nullptr, 16);
        if (!ok) {  // gaps between neighbours on each orbit (turn-free) against the truth's
            OrbPayload t; t.version = 1; t.flags = 1; t.secret = want;
            const SatSlots truth = EncodeSat(t);
            for (int o = 0; o < kSatOrbits; ++o) {
                auto gaps = [](std::array<int, kSatPerOrbit> v) { std::sort(v.begin(), v.end()); std::string g; for (int i = 0; i < kSatPerOrbit; ++i) g += std::to_string((v[static_cast<size_t>((i + 1) % kSatPerOrbit)] - v[static_cast<size_t>(i)] + kSatPositions) % kSatPositions) + " "; return g; };
                std::printf("  orbit %d seen gaps %s| truth gaps %s\n", o, gaps(seen[static_cast<size_t>(o)]).c_str(), gaps(truth[static_cast<size_t>(o)]).c_str());
            }
        }
        std::printf("%s secret %016llx (centre %.1f,%.1f radius %.1f; blob %.1f,%.1f size %.1f)\n", ok ? "decoded" : "not decoded",
                    static_cast<unsigned long long>(p.secret), di.cx, di.cy, di.radius, di.blobX, di.blobY, di.s0);
        return ok && p.secret == want ? 0 : 1;
    }
    if (argc >= 4 && std::wstring(argv[1]) == L"--seq") {  // --seq <dir of orb_*.bmp> <hex>: a dynamic code, frame by frame
        const unsigned long long want = std::wcstoull(argv[3], nullptr, 16);
        TemporalDecoder dec; dec.Reset();
        for (int i = 0; i < 2000; ++i) {
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%ls\\orb_%04d.bmp", argv[2], i);
            GrayImage img;
            if (!LoadBmp(path, img)) {
                // diagnostics: what the accumulated picture looks like and where the decoder thinks the orb is
                const GrayImage& acc = dec.Accumulated();
                double cx = 0, cy = 0, s = 0; DecodeInfo di;
                const bool found = acc.w > 0 && LocateOrb(acc, cx, cy, s, &di);
                std::printf("not decoded after %d frames (accumulated picture: orb %s at %.1f,%.1f r %.1f; blob %.1f,%.1f size %.1f)\n", i,
                            found ? "found" : "not found", cx, cy, s, di.blobX, di.blobY, di.s0);
                wchar_t out[MAX_PATH]; swprintf_s(out, L"%ls\\accumulated.pgm", argv[2]);
                FILE* f = nullptr;
                if (acc.w > 0 && _wfopen_s(&f, out, L"wb") == 0 && f) {
                    std::fprintf(f, "P5\n%d %d\n255\n", acc.w, acc.h);
                    for (float v : acc.v) std::fputc(static_cast<int>(std::clamp(v, 0.f, 1.f) * 255), f);
                    std::fclose(f);
                }
                return 1;
            }
            OrbPayload p;
            if (dec.Feed(img, p)) {
                std::printf("decoded after %d frames: secret %016llx%s\n", i + 1, static_cast<unsigned long long>(p.secret), p.secret == want ? "" : " (WRONG)");
                return p.secret == want ? 0 : 1;
            }
        }
        return 1;
    }
    if (argc >= 2) {
        GrayImage img;
        if (!LoadBmp(argv[1], img)) { std::printf("cannot read %ls\n", argv[1]); return 2; }
        OrbPayload p; DecodeInfo info;
        if (argc >= 6) {  // diagnostics: ring score at a given centre and radius
            const double x = _wtof(argv[3]), y = _wtof(argv[4]), r = _wtof(argv[5]);
            std::printf("score at %.1f,%.1f r %.1f: %.5f\n", x, y, r, RingScore(img, x, y, r));
        }
        const bool ok = DecodeImage(img, p, &info);
        if (argc >= 6) std::printf("score at fit: %.5f\n", RingScore(img, info.cx, info.cy, info.radius));
        if (!ok) {
            std::printf("%ls: no orb code found (blob %.1f,%.1f size %.1f; ring fit %.1f,%.1f radius %.1f)\n", argv[1], info.blobX, info.blobY, info.s0, info.cx, info.cy, info.radius);
            return 1;
        }
        std::printf("%ls: version %u flags %u secret %016llx (centre %.1f,%.1f radius %.1f px, rotation %d, %s)\n", argv[1], p.version, p.flags,
                    static_cast<unsigned long long>(p.secret), info.cx, info.cy, info.radius, info.rotation, info.mirrored ? "mirrored" : "direct");
        if (argc >= 3) {
            const unsigned long long want = std::wcstoull(argv[2], nullptr, 16);
            if (p.secret != want) { std::printf("secret mismatch (expected %016llx)\n", want); return 1; }
        }
        return 0;
    }

    std::mt19937 rng(1234);
    std::uniform_int_distribution<unsigned long long> u64;
    // 1. bits: round trip under every rotation and mirror; a damaged code is rejected
    {
        int ok = 0, total = 0, rejected = 0, balanced = 0;
        for (int i = 0; i < 400; ++i) {
            OrbPayload p; p.flags = static_cast<uint8_t>(i & 15); p.secret = u64(rng);
            const OrbBits bits = Encode(p);
            balanced += Balanced(bits);
            const int rot = i % kSlots; const bool mir = (i / kSlots) & 1;
            OrbBits seen{};
            for (int ring = 0; ring < kRings; ++ring) for (int slot = 0; slot < kSlots; ++slot) {
                const int s = mir ? ((rot - slot) % kSlots + kSlots) % kSlots : (slot + rot) % kSlots;
                seen[static_cast<size_t>(ring * kSlots + s)] = bits[static_cast<size_t>(ring * kSlots + slot)];
            }
            OrbPayload q; int r = -1; bool m = false;
            ++total; ok += DecodeBits(seen, q, &r, &m) && q == p && r == rot && m == mir;
            OrbBits bad = seen; bad[static_cast<size_t>(40 + i % 50)] ^= 1;
            OrbPayload z; rejected += !DecodeBits(bad, z) || !(z == p);
        }
        std::printf("bits: %d/%d decoded, %d/400 single-bit errors rejected, %d/400 balanced\n", ok, total, rejected, balanced);
        CHECK(ok == total, "every rotation / mirror decodes");
        CHECK(rejected == 400, "a flipped bit never gives a wrong payload");
        CHECK(balanced > 300, "most secrets give balanced rings");
    }
    // 2. images: synthetic camera views
    {
        std::uniform_real_distribution<double> u(0, 1);
        int ok = 0, total = 0;
        for (int i = 0; i < 160; ++i) {
            OrbPayload p; p.secret = u64(rng);
            OrbBits bits = Encode(p);
            while (!Balanced(bits)) { p.secret = u64(rng); bits = Encode(p); }
            const int size = 220 + static_cast<int>(u(rng) * 200);
            const double R = size * (0.12 + 0.06 * u(rng));
            const double turn = 2 * kPi * u(rng);
            const GrayImage img = Synthetic(bits, size, R, turn, i % 3 == 0, 1 + 2 * u(rng), 0.03 + 0.05 * u(rng), rng);
            OrbPayload q; DecodeInfo di;
            const bool good = DecodeImage(img, q, &di) && q == p;
            if (!good && i < 40) std::printf("  miss: size %d R %.1f true c %.1f,%.1f score %.5f | got c %.1f,%.1f r %.1f score %.5f\n", size, R, g_cx, g_cy,
                                            RingScore(img, i % 3 == 0 ? g_cx : g_cx, g_cy, R), di.cx, di.cy, di.radius, RingScore(img, di.cx, di.cy, di.radius));
            if (!good && i < 40) std::printf("    blob %.1f,%.1f s0 %.1f\n", di.blobX, di.blobY, di.s0);
            ++total; ok += good;
        }
        std::printf("images: %d/%d decoded (any turn, mirror, blur, noise, glare)\n", ok, total);
        CHECK(ok >= total * 0.97, "synthetic camera views decode");
    }
    // 4. satellite code: positions round trip, then synthetic camera views (satellites with comet tails)
    {
        int ok = 0, rejected = 0;
        for (int i = 0; i < 300; ++i) {
            OrbPayload p; p.flags = static_cast<uint8_t>(i & 15); p.secret = u64(rng);
            const SatSlots sl = EncodeSat(p);
            const int rot = i % kSatPositions; const bool mir = (i / kSatPositions) & 1;
            SatSlots seen{};
            for (int o = 0; o < kSatOrbits; ++o) for (int k = 0; k < kSatPerOrbit; ++k) {
                const int v = sl[static_cast<size_t>(o)][static_cast<size_t>(k)];
                seen[static_cast<size_t>(o)][static_cast<size_t>(k)] = mir ? ((rot - v) % kSatPositions + kSatPositions) % kSatPositions : (v + rot) % kSatPositions;
            }
            OrbPayload q;
            ok += DecodeSatSlots(seen, q) && q == p;
            seen[1][3] = (seen[1][3] + 1) % kSatPositions;  // one satellite read a position off
            OrbPayload z;
            rejected += !DecodeSatSlots(seen, z) || !(z == p);
        }
        std::printf("satellites: %d/300 decoded under any turn / mirror, %d/300 misplaced satellites rejected\n", ok, rejected);
        CHECK(ok == 300 && rejected == 300, "satellite positions round trip");

        std::uniform_real_distribution<double> u(0, 1);
        std::normal_distribution<double> n01(0, 1);
        int iok = 0, itotal = 0;
        for (int i = 0; i < 120; ++i) {
            OrbPayload p; p.secret = u64(rng);
            const SatSlots sl = EncodeSat(p);
            const int size = 280 + static_cast<int>(u(rng) * 160);
            const double R = size * (0.11 + 0.04 * u(rng)), turn = 2 * kPi * u(rng);
            const bool mirror = i % 3 == 0;
            GrayImage img; img.w = img.h = size; img.v.assign(static_cast<size_t>(size) * size, 0.f);
            const double cx = size * (0.45 + 0.1 * u(rng)), cy = size * (0.45 + 0.1 * u(rng));
            auto splat = [&](double x, double y, double v, double rad) {
                if (mirror) x = 2 * cx - x;
                const int x0 = static_cast<int>(x - 2 * rad - 1), x1 = static_cast<int>(x + 2 * rad + 1), y0 = static_cast<int>(y - 2 * rad - 1), y1 = static_cast<int>(y + 2 * rad + 1);
                for (int yy = y0; yy <= y1; ++yy) for (int xx = x0; xx <= x1; ++xx) {
                    if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
                    const double d2 = (xx + 0.5 - x) * (xx + 0.5 - x) + (yy + 0.5 - y) * (yy + 0.5 - y);
                    float& px = img.v[static_cast<size_t>(yy) * size + xx];
                    px = static_cast<float>(std::min(1.0, px + v * std::exp(-d2 / (rad * rad))));
                }
            };
            for (int k = 0; k < static_cast<int>(R * R * 1.4); ++k) { const double a = 2 * kPi * u(rng), r = R * std::sqrt(u(rng)); splat(cx + r * std::cos(a), cy + r * std::sin(a), 0.35 + 0.4 * u(rng), 0.8); }
            for (int o = 0; o < kSatOrbits; ++o) for (int k = 0; k < kSatPerOrbit; ++k) {
                const double a0 = 2 * kPi * sl[static_cast<size_t>(o)][static_cast<size_t>(k)] / kSatPositions + turn, rr = kSatRadius[o] * R;
                splat(cx + rr * std::cos(a0), cy + rr * std::sin(a0), 0.95, std::max(1.2, R * 0.03));      // head
                for (int t = 1; t <= 12; ++t) {                                                                // tail, behind (smaller angle)
                    const double a = a0 - t * 0.012;
                    splat(cx + rr * std::cos(a), cy + rr * std::sin(a), 0.5 * (1 - t / 13.0), std::max(0.9, R * 0.018));
                }
            }
            for (int pass = 0; pass < 1 + i % 2; ++pass) {
                GrayImage t = img;
                for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
                    float sum = 0; for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) sum += t.at(x + dx, y + dy);
                    img.v[static_cast<size_t>(y) * size + x] = sum / 9;
                }
            }
            for (auto& px : img.v) px = static_cast<float>(std::clamp(px + 0.04 * n01(rng) + 0.05, 0.0, 1.0));
            OrbPayload q; DecodeInfo di;
            const bool good = DecodeSatImage(img, q, &di) && q == p;
            if (!good && i < 4) std::printf("  sat miss: R %.1f true c %.1f,%.1f score %.5f | got c %.1f,%.1f r %.1f score %.5f\n", R, cx, cy,
                                            RingScore(img, cx, cy, R, 6, kSatRadius, kSatGap), di.cx, di.cy, di.radius, RingScore(img, di.cx, di.cy, di.radius, 6, kSatRadius, kSatGap));
            ++itotal; iok += good;
        }
        std::printf("satellites: %d/%d synthetic camera views decoded\n", iok, itotal);
        CHECK(iok >= itotal * 0.97, "satellite code decodes from pictures");
    }
    // 3. dynamic code: a phone at 30 fps watching arcs that flare one after another
    {
        std::uniform_real_distribution<double> u(0, 1);
        int ok = 0, total = 0, wrong = 0; std::vector<int> needed;
        for (int i = 0; i < 40; ++i) {
            OrbPayload p; p.secret = u64(rng);
            OrbBits bits = Encode(p);
            while (!Balanced(bits)) { p.secret = u64(rng); bits = Encode(p); }
            const int size = 260 + static_cast<int>(u(rng) * 140);
            const double R = size * (0.12 + 0.05 * u(rng)), turn = 2 * kPi * u(rng), t0 = 5 * u(rng);
            const bool mirror = i % 4 == 0;
            const double centre[2] = {size * (0.47 + 0.06 * u(rng)), size * (0.47 + 0.06 * u(rng))};
            TemporalDecoder dec; dec.Reset(); dec.SetDecodeEvery(1);
            int frames = -1;
            for (int f = 0; f < 90; ++f) {  // 3 s at 30 fps
                const double t = t0 + f / 30.0;
                double lit[kBits];
                for (int ring = 0; ring < kRings; ++ring) for (int slot = 0; slot < kSlots; ++slot) lit[ring * kSlots + slot] = FlareLevel(ring, slot, t);
                const double at[2] = {centre[0] + 1.5 * (u(rng) - 0.5), centre[1] + 1.5 * (u(rng) - 0.5)};  // a steady hand
                const GrayImage img = Synthetic(bits, size, R, turn, mirror, 1 + (f % 2), 0.04, rng, lit, at);
                OrbPayload q; DecodeInfo di;
                if (dec.Feed(img, q, &di)) { frames = f + 1; if (!(q == p)) ++wrong; break; }
            }
            ++total;
            if (frames > 0) { ++ok; needed.push_back(frames); }
            if (frames < 0 && i < 3) std::printf("  dynamic miss after %d frames\n", dec.Frames());
        }
        std::sort(needed.begin(), needed.end());
        const int median = needed.empty() ? -1 : needed[needed.size() / 2], worst = needed.empty() ? -1 : needed.back();
        std::printf("dynamic: %d/%d decoded within 3 s at 30 fps, frames needed median %d (%.2f s), worst %d (%.2f s), wrong %d\n",
                    ok, total, median, median / 30.0, worst, worst / 30.0, wrong);
        CHECK(ok >= total - 1 && wrong == 0, "dynamic code decodes");
        // at any moment only about a quarter of the arcs are lit
        double litShare = 0; int n = 0;
        for (int f = 0; f < 120; ++f) for (int ring = 0; ring < kRings; ++ring) for (int slot = 0; slot < kSlots; ++slot) { litShare += FlareLevel(ring, slot, f / 30.0) > 0.5; ++n; }
        std::printf("dynamic: %.0f %% of the arcs lit in a frame on average\n", 100.0 * litShare / n);
    }
    std::printf(failures ? "%d FAILURE(S)\n" : "all orb code checks passed\n", failures);
    return failures ? 1 : 0;
}
