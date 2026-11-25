#include "quantize.hlsli"

// Black Hole Visualizer: a Schwarzschild black hole lensing the live desktop (DockDrop visual base).
//
// Every pixel is one photon traced backwards from a distant observer (orthographic camera). Its impact parameter
// b = rho / pxPerM (rho: distance from the hole centre). Its orbit u(b, phi) and fate phi_end(b) come from exact
// Schwarzschild geodesics integrated on the CPU (src/blackhole/schwarzschild_lut.h), so:
//   * b < b_c = 3*sqrt(3): captured -> pure black shadow (event horizon + photon sphere capture)
//   * b -> b_c: deflection diverges logarithmically -> photon ring with nested higher-order images
//   * accretion disk: thin disk in the hole's equatorial plane, inclined by `incl`; the photon crosses that plane at
//     phi_0 + n*pi; order n = 0 is the direct image, n = 1 the lensed image that wraps under/over the shadow, n = 2 the
//     photon-ring image. The disk's surface is the desktop itself (a top-down copy of the desktop around the hole).
//   * background: escaped photons leave in direction phi_end; the desktop is the "sky" behind the hole (azimuthal
//     equidistant map around the anti-observer direction), so the region behind the hole forms an Einstein ring and
//     its relativistic copies near the photon ring.
// Keep the column mapping in sync with schwarzschild_lut.h.

cbuffer Constants : register(b0) {
    float2 regionOrigin;   // desktop-texture px of the render target's top-left corner
    float2 holeCenter;     // desktop-texture px of the hole centre
    float2 desktopSize;    // desktop texture size (px)
    float  renderScale;    // render px per desktop px (low power renders smaller)
    float  pxPerM;         // desktop px per gravitational radius M
    float  sinIncl, cosIncl;   // disk inclination (0 = face-on, 90 deg = edge-on)
    float  lensDistance;   // source (desktop) plane distance behind the hole, in M: Einstein radius = 2*sqrt(D) M
    float  regionRadius;   // overlay radius (desktop px); lensing tapers to zero before it
    float  diskIn, diskOut;    // disk radii (M)
    float  diskOpacity;
    float  diskBrightness;
    float  time;           // flow-map phase input (s * spin)
    float  doppler;        // 0 = off, 1 = physical g^3 beaming + colour shift
    float  heatTint;       // 0 = pure desktop colours, 1 = blackbody-orange accretion glow
    float  ringFlash;      // extra brightness for higher-order images
    float  opacity;        // overall overlay opacity
    float  taperStart;     // fraction of regionRadius where the lens taper starts
    float  bMax, lutDelta, phiMax;
    float  showDisk;
    uint   spp;            // 1, 2 (adaptive 4 near the photon ring), 4
    float  flowPeriod;
    float  spinSign;
    float  overlayRadius;  // alpha edge of the overlay (desktop px); >= regionRadius when a payload is further out
    float  payloadLodBias;
    uint   payloadCount;
    float4 payloadA[4];    // x, y (M, hole-relative, y down), half size (M), radial stretch
    float4 payloadB[4];    // opacity, redshift factor g, texture slice, shear (streak bend along the infall spiral)
    float  debrisExtent;   // the debris texture covers [-debrisExtent, debrisExtent]^2 M of the lens plane
    uint   debrisOn;
    float2 skyDrift;       // desktop px: slow parallax of the "sky" behind the hole (an orbiting observer), tapered
    float  debrisSin, debrisCos;  // the debris orbit plane's tilt (0 face-on, 90 deg edge-on)
    float2 pad3;
};

Texture2D<float4> desktop : register(t0);
Texture2D<float>  orbitLut : register(t1);    // u(b, phi)
Texture2D<float>  endLut : register(t2);      // phi_end(b)
Texture2DArray<float4> payloadTex : register(t3);  // premultiplied icons, with mips
Texture2D<float4> debrisTex : register(t4);        // debris in its tilted orbit plane, as seen (blackhole_debris.hlsl), premultiplied linear
SamplerState linearMirror : register(s0);
SamplerState linearClamp : register(s1);
SamplerState linearClampMip : register(s2);

static const float PI = 3.14159265358979;
static const float BC = 5.19615242270663;  // 3*sqrt(3)

struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float MapG(float d, float D) { return log(1 + d / lutDelta) / log(1 + D / lutDelta); }
float LutColumn(float b, out bool captured) {
    uint w, h; endLut.GetDimensions(w, h);
    const float halfTexel = 0.5 / w;
    captured = b < BC;
    if (captured) return min(0.5 - 0.5 * MapG(BC - max(b, 0), BC), 0.5 - halfTexel);
    return max(0.5 + 0.5 * MapG(min(b, bMax) - BC, bMax - BC), 0.5 + halfTexel);
}
float PhiEnd(float b, float s) {
    if (b >= bMax) return PI + 4 / b + 15 * PI / (4 * b * b);
    return endLut.SampleLevel(linearClamp, float2(s, 0.5), 0);
}
float OrbitU(float s, float phi) { return orbitLut.SampleLevel(linearClamp, float2(s, phi / phiMax), 0); }

struct Hit { float2 xy; float r; float a; float g; };  // disk plane coordinates (M), radius, coverage, redshift factor
struct Geo {
    float2 bgPx;      // desktop px of the background sample
    float  esc;       // 1 escaped, 0 captured
    float  strong;    // 0..1, how strongly the background ray was bent (for the ring flash)
    float  b;
    Hit    hit[3];
    float3 plane[3];  // lens-plane crossings of order 0..2: xy (M, hole-relative), valid (1/0)
    float  planeFirst; // 1: the lens-plane crossing of each order comes before the disk crossing of that order
    float3 tde[3];    // crossings of the tilted debris orbit plane, order 0..2: xy (that plane's M), valid (1/0)
    float  phiP, phiD, phiT;  // crossing angles of order 0 (lens plane, disk, debris plane); order k adds k pi
};

Geo Trace(float2 renderPx) {
    Geo g;
    const float2 q = regionOrigin + renderPx / renderScale;
    float2 d = q - holeCenter;
    const float rho = max(length(d), 1e-4);
    const float2 e = d / rho;
    const float b = rho / pxPerM;
    g.b = b;
    bool captured;
    const float s = LutColumn(b, captured);
    g.esc = captured ? 0 : 1;
    const float phiEnd = captured ? endLut.SampleLevel(linearClamp, float2(s, 0.5), 0) : PhiEnd(b, s);

    // ---- background: escaped direction -> desktop "sky" behind the hole ----
    bool dummy;
    const float sEsc = LutColumn(max(b, BC + 1e-5), dummy);
    const float alpha = (captured ? PhiEnd(max(b, BC + 1e-5), sEsc) : phiEnd) - PI;  // total deflection
    const float chi = atan2(-sin(alpha), cos(alpha));                                 // wrapped, = -alpha for small alpha
    const float taper = 1 - smoothstep(taperStart * regionRadius, 0.92 * regionRadius, rho);
    // skyDrift: the observer slowly orbiting the hole moves the sky behind it; near the shadow its magnified images
    // stream round the Einstein ring and the counter-images move the other way (NASA SVS 14620, "Orbiting a bare black
    // hole"). Tapered with the lens so the rim stays pixel-identical to the desktop.
    g.bgPx = holeCenter + e * (rho + lensDistance * pxPerM * chi * taper) + skyDrift * taper;
    g.strong = saturate((alpha - 0.6) / 2.0);

    // ---- accretion disk crossings ----
    const float3 n = float3(0, -sinIncl, cosIncl);  // disk normal; far side of the disk appears above the hole
    const float3 d1 = float3(1, 0, 0);
    const float3 d2 = float3(0, cosIncl, sinIncl);  // n x d1
    const float A = e.y * n.y, B = n.z;            // e.n (e.x * n.x = 0), z.n
    float phi0 = atan2(-B, A); if (phi0 < 0) phi0 += PI;
    // ---- lens plane (through the hole, facing the observer): payloads. Crossed at phi = pi/2 + k*pi. ----
    g.planeFirst = PI * 0.5 <= phi0 ? 1 : 0;
    g.phiP = PI * 0.5; g.phiD = phi0;
    // ---- debris orbit plane (tilted like the disk, by its own angle): the shredded star ----
    {
        const float AT = -e.y * debrisSin, BT = debrisCos;
        float phiT = atan2(-BT, AT); if (phiT < 0) phiT += PI;
        g.phiT = phiT;
        const float3 t2 = float3(0, debrisCos, debrisSin);
        [unroll] for (int kt = 0; kt < 3; ++kt) {
            g.tde[kt] = 0;
            const float phi = phiT + kt * PI;
            if (debrisOn != 0 && phi < phiEnd && phi < phiMax) {
                const float u = OrbitU(s, phi);
                if (u > 1e-4) {
                    float sn, cs; sincos(phi, sn, cs);
                    const float3 P = (1 / u) * (sn * float3(e, 0) + cs * float3(0, 0, 1));
                    g.tde[kt] = float3(P.x, dot(P, t2), 1);
                }
            }
        }
    }
    [unroll] for (int kp = 0; kp < 3; ++kp) {
        g.plane[kp] = 0;
        const float phi = PI * 0.5 + kp * PI;
        if ((payloadCount > 0 || debrisOn != 0) && phi < phiEnd && phi < phiMax) {
            const float u = OrbitU(s, phi);
            if (u > 1e-4) g.plane[kp] = float3(e * (1 / u) * ((kp & 1) ? -1 : 1), 1);
        }
    }
    [unroll] for (int k = 0; k < 3; ++k) {
        Hit h; h.xy = 0; h.r = 0; h.a = 0; h.g = 1;
        const float phi = phi0 + k * PI;
        if (showDisk > 0.5 && phi < phiEnd && phi < phiMax) {
            const float u = OrbitU(s, phi);
            if (u > 1e-4) {
                const float r = 1 / u;
                float sn, cs; sincos(phi, sn, cs);
                const float3 P = r * (sn * float3(e, 0) + cs * float3(0, 0, 1));
                h.xy = float2(dot(P, d1), dot(P, d2));
                h.r = r;
                const float feather = 0.35 + 0.15 * k;
                h.a = smoothstep(diskIn, diskIn + feather, r) * (1 - smoothstep(diskOut - 1.5, diskOut, r));
                // Keplerian disk: 1+z = (1 - Omega * b * e.x * sin i) / sqrt(1 - 3/r)
                const float omega = pow(max(r, 1e-3), -1.5) * spinSign;
                h.g = sqrt(saturate(1 - 3 / r)) / max(1 - omega * b * e.x * sinIncl, 0.05);
            }
        }
        g.hit[k] = h;
    }
    return g;
}

float3 ToLinear(float3 c) { return pow(saturate(c), 2.2); }
float3 ToGamma(float3 c) { return pow(saturate(c), 1 / 2.2); }

float3 DiskColor(Hit h, int order, float2 gx, float2 gy) {
    // Flow map: the desktop on the disk turns with the Keplerian angular velocity r^-1.5 (differential rotation), in two
    // phases cross-faded so the shear never winds up.
    // Rigid rotation at the disk's mid-radius Keplerian rate (never shears), plus the differential (Keplerian minus
    // rigid) part through the flow map, whose two phases differ by little, so cross-fading shows no double image.
    const float omega = pow(max(h.r, 2.0), -1.5);
    const float omegaRef = pow(max(0.5 * (diskIn + diskOut), 1.0), -1.5);
    const float speed = 3.0;
    const float base = -spinSign * omegaRef * time * speed;
    const float p1 = frac(time / flowPeriod), p2 = frac(time / flowPeriod + 0.5);
    const float w1 = 1 - abs(2 * p1 - 1);
    float3 c = 0;
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        const float ph = (layer == 0 ? p1 : p2) - 0.5;
        const float ang = base - spinSign * (omega - omegaRef) * ph * flowPeriod * speed;
        float sa, ca; sincos(ang, sa, ca);
        const float2 xy = float2(ca * h.xy.x - sa * h.xy.y, sa * h.xy.x + ca * h.xy.y);
        const float2 uv = (holeCenter + xy * pxPerM) / desktopSize;
        const float w = layer == 0 ? w1 : 1 - w1;
        c += w * desktop.SampleGrad(linearMirror, uv, gx, gy).rgb;
    }
    float3 lin = ToLinear(c);
    const float lum = dot(lin, float3(0.2126, 0.7152, 0.0722));
    const float3 glow = float3(1.0, 0.55, 0.22) * (0.25 + 1.6 * lum);
    lin = lerp(lin, glow, heatTint);
    // Relativistic beaming and colour shift (specific intensity ~ g^3; blue gains more than red when g > 1).
    const float3 beam = pow(max(h.g, 0.05), float3(2.4, 3.0, 3.6));
    lin *= lerp(1.0.xxx, beam, doppler);
    lin *= diskBrightness * (order > 0 ? 1 + 2.5 * ringFlash : 1);
    return lin;
}

// Payload icons at a lens-plane crossing Q (M). Returns linear premultiplied colour.
float4 PayloadAt(float2 Q, int order) {
    float4 acc = 0;
    for (uint i = 0; i < payloadCount; ++i) {
        const float4 A = payloadA[i], Bp = payloadB[i];
        const float len = length(A.xy);
        const float2 ur = len > 1e-3 ? -A.xy / len : float2(1, 0);  // towards the hole
        const float2 ut = float2(-ur.y, ur.x);
        const float2 d = Q - A.xy;
        const float h = A.z, lam = A.w;
        // Linear tidal deformation (inverse of CardForward): stretched by lam along the radius, squeezed by
        // 1/sqrt(lam) across it (volume kept); lam limited so the inner edge stays outside r = 2M.
        const float lamE = clamp(lam, 1.0, max(1.0, (len - 2.0) / h));
        const float sq = sqrt(lamE);
        const float2 src = (dot(d, ur) / lamE) * ur + (dot(d, ut) * sq) * ut;
        const float2 uv = src / (2 * h) + 0.5;
        if (any(uv < 0) || any(uv > 1)) continue;
        uint tw, th, ts, levels; payloadTex.GetDimensions(0, tw, th, ts, levels);
        const float pixelM = 1 / (pxPerM * renderScale);
        const float lod = log2(max(tw / (2 * h) * pixelM * sq, 1e-4)) + order * 1.5 + payloadLodBias;
        float4 c = payloadTex.SampleLevel(linearClampMip, float3(uv, Bp.z), max(lod, 0));
        c.rgb = ToLinear(c.a > 0 ? c.rgb / c.a : 0) * c.a;  // premultiplied sRGB -> premultiplied linear
        // Gravitational (and, falling, kinematic) redshift: observed intensity ~ g^3..g^4, so it goes dark and red well
        // before the horizon (blue is lost first).
        const float gg = Bp.y;
        c.rgb *= pow(max(gg, 1e-3), float3(1.8, 2.5, 3.3)) * (order > 0 ? 1 + 2.5 * ringFlash : 1);
        c *= Bp.x;
        acc += (1 - acc.a) * c;
    }
    return acc;
}

// Debris (the shredded icon) where the photon crosses the debris orbit plane at Q (that plane's M); the texture holds
// it as seen (y foreshortened by cos tilt).
float4 DebrisAt(float2 Q, int order) {
    if (debrisOn == 0) return 0;
    const float2 uv = float2(Q.x, Q.y * debrisCos) / (2 * debrisExtent) + 0.5;
    if (any(uv < 0) || any(uv > 1)) return 0;
    float4 c = debrisTex.SampleLevel(linearClamp, uv, 0);
    c.rgb *= order > 0 ? 1 + 2.5 * ringFlash : 1;
    return c;
}

float4 Shade(Geo g, float2 bgGx, float2 bgGy, float2 dGx[3], float2 dGy[3], float smoothEsc) {
    float3 col = 0; float T = 1;  // front-to-back: the photon meets the n = 0 crossing first
    // Each order crosses the lens plane, the disk and the debris plane once; in the order the photon meets them.
    const int rP = (g.phiD < g.phiP ? 1 : 0) + (g.phiT < g.phiP ? 1 : 0);
    const int rD = (g.phiP <= g.phiD ? 1 : 0) + (g.phiT < g.phiD ? 1 : 0);
    [unroll] for (int k = 0; k < 3; ++k) {
        [unroll] for (int step = 0; step < 3; ++step) {
            if (step == rP) {
                // Only photons that escape reach the observer: a payload is never seen through the black shadow (its
                // light would have to come out of the captured region); it slides under the shadow's edge instead.
                if (g.plane[k].z > 0 && smoothEsc > 0 && payloadCount > 0) {
                    float4 p = PayloadAt(g.plane[k].xy, k);
                    p *= smoothEsc;
                    col += T * p.rgb; T *= 1 - p.a;
                }
            } else if (step == rD) {
                const Hit h = g.hit[k];
                if (h.a > 0) {
                    const float a = h.a * diskOpacity;
                    col += T * a * DiskColor(h, k, dGx[k], dGy[k]);
                    T *= 1 - a;
                }
            } else if (g.tde[k].z > 0 && smoothEsc > 0) {
                // Like the payload card: nothing is ever seen inside the black shadow. A falling pixel slides under the
                // shadow's (anti-aliased) edge and is gone; only light that escapes reaches the observer.
                const float4 p = DebrisAt(g.tde[k].xy, k) * smoothEsc;
                col += T * p.rgb; T *= 1 - p.a;
            }
        }
    }
    const float esc = smoothEsc;
    if (esc > 0) {
        float3 bg = ToLinear(desktop.SampleGrad(linearMirror, g.bgPx / desktopSize, bgGx, bgGy).rgb);
        bg *= 1 + 2.0 * ringFlash * g.strong;
        col += T * esc * bg;
    }
    return float4(col, 1);
}

float4 PSMain(VSOut input) : SV_Target {
    const float2 p = input.pos.xy;
    // Pixel-centre pass in uniform control flow: texture-space gradients for mip selection (strongly demagnified
    // photon-ring images pick coarse mips instead of aliasing).
    const Geo c = Trace(p);
    const float2 bgUv = c.bgPx / desktopSize;
    float2 bgGx = ddx(bgUv), bgGy = ddy(bgUv);
    float2 dGx[3], dGy[3];
    [unroll] for (int k = 0; k < 3; ++k) {
        const float2 duv = c.hit[k].xy * pxPerM / desktopSize;
        dGx[k] = ddx(duv); dGy[k] = ddy(duv);
    }
    const float bPerPx = 1 / (pxPerM * renderScale);
    const float dist = length(p / renderScale + regionOrigin - holeCenter);

    // Supersampling only where the map is strongly compressive (photon ring, higher-order disk images, shadow edge);
    // elsewhere one sample at the exact texel centre keeps the weak-field desktop as sharp as the real one.
    uint n = 1;
    if (c.b < BC + 3.0) n = spp;
    if (n == 2 && abs(c.b - BC) < 1.2) n = 4;  // balanced: 4 samples right at the photon ring
    const float sub = 1 / sqrt((float)n);
    [unroll] for (int k2 = 0; k2 < 3; ++k2) { dGx[k2] *= sub; dGy[k2] *= sub; }
    bgGx *= sub; bgGy *= sub;

    float4 acc = 0;
    if (n == 1) {
        // One sample: anti-alias the shadow edge analytically (coverage over one pixel of b).
        const float escCov = saturate((c.b - BC) / bPerPx + 0.5);
        Geo g = c;
        if (escCov > 0 && g.esc == 0) {  // edge pixel on the captured side: borrow the escaping ray
            g = Trace(p + normalize(p / renderScale + regionOrigin - holeCenter) * 0.75);
        }
        acc = Shade(g, bgGx, bgGy, dGx, dGy, escCov);
    } else {
        static const float2 o2[2] = { float2(-0.25, -0.25), float2(0.25, 0.25) };
        static const float2 o4[4] = { float2(-0.125, -0.375), float2(0.375, -0.125), float2(0.125, 0.375), float2(-0.375, 0.125) };
        for (uint i = 0; i < n; ++i) {
            const Geo g = Trace(p + (n == 2 ? o2[i] : o4[i]));
            acc += Shade(g, bgGx, bgGy, dGx, dGy, g.esc);
        }
        acc /= n;
    }
    // Edge: the lens has tapered to the identity mapping, so fading out shows the real desktop underneath seamlessly.
    const float edge = 1 - smoothstep(overlayRadius - 0.06 * regionRadius, overlayRadius, dist);
    const float a = edge * opacity;
    return DitherBgra8(float4(ToGamma(acc.rgb) * a, a), p);
}
