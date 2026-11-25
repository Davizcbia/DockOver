#include "quantize.hlsli"

// DockDrop pairing orb (docky-orb): a Liquid Glass card with a GPU particle orb whose code rings carry the pairing
// information (src/pairing/orb_code.h). Three passes into a premultiplied BGRA target:
//   PSGlass     the card: the desktop behind it frosted (a coarse mip) and refracted at the rim, a dark tint, a specular
//               edge, a soft shadow outside;
//   VSParticle / PSParticle   additive particles: the orb, the code arcs, a faint halo; everything moves in the vertex
//               shader from per-particle constants and the clock (no per-frame CPU work);
//   PSOverlay   text, the connected checkmark and the flash, premultiplied "over".

cbuffer Constants : register(b0) {
    float2 targetSize;    // px
    float2 cardMin;       // card rectangle (px)
    float2 cardMax;
    float  corner;        // card corner radius (px)
    float  dpi;           // px per DIP
    float2 orbCenter;     // px
    float  orbR;          // px
    float  time;          // s
    float  absorb;        // s since the scan succeeded (< 0: not yet)
    float  intro;         // s since the orb appeared
    float  codeAlpha;     // code rings' opacity (a new code fades in)
    float  hasDesk;
    float2 deskOrigin;    // desktop texture px of target px (0, 0)
    float2 deskSize;      // desktop texture size (px)
    float  textMix;       // 0: "scan" text, 1: "connected" text
    float  textTop;       // px: top of the text block
    float  textHeight;    // px: height of one text block in the text texture
    float  codeMode;      // 0 static (every arc lit), 1 dynamic (arcs flare one after another), 2 spinning ring, 3 radar,
                          // 4 satellites (the arcs are replaced by comets on three orbits)
    float2 closeCenter;   // px: the close button (top left, as on macOS)
    float  closeState;    // 0 idle, 1 hovered, 2 pressed
    float  appear;        // 0..1: the card materialising (scale and opacity are applied on the CPU side / here)
};

Texture2D<float4> desk : register(t0);
Texture2D<float>  textTex : register(t1);
SamplerState linearClamp : register(s0);

struct Particle { float4 a; float4 b; };
StructuredBuffer<Particle> parts : register(t2);

static const float PI = 3.14159265;

// ---------------------------------------------------------------------------------------------------------------------
struct VSOut { float4 pos : SV_Position; };
VSOut VSFull(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// Continuous-curvature ("squircle") corners, as Apple draws them: the corner is a superellipse |x|^5 + |y|^5 = r^5
// instead of a circular arc, so the curvature rises smoothly from the straight edge instead of jumping. (An Lp-norm
// distance: exact on the straight edges, within a few % in the corners, plenty for anti-aliasing and the bevel.)
float RoundRect(float2 p, float2 c, float2 half, float r) {
    const float2 q = abs(p - c) - half + r;
    const float2 m = max(q, 0) / r;
    const float lp = r * pow(max(pow(m.x, 5.0) + pow(m.y, 5.0), 1e-12), 0.2);
    return lp + min(max(q.x, q.y), 0) - r;
}
float CardSdf(float2 p) { return RoundRect(p, 0.5 * (cardMin + cardMax), 0.5 * (cardMax - cardMin), corner); }

float3 Desk(float2 p, float lod) {
    if (hasDesk < 0.5) {  // no capture: a calm dark gradient
        const float2 u = p / targetSize;
        return lerp(float3(0.10, 0.12, 0.17), float3(0.05, 0.06, 0.10), u.y);
    }
    const float3 c = desk.SampleLevel(linearClamp, (deskOrigin + p) / deskSize, lod).rgb;
    return pow(saturate(c), 2.2);
}

float Hash2(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

// The shockwave after the orb collapses: a ring that runs out over the glass and bends what is behind it.
float Wave(float2 p) {
    if (absorb < 1.18) return 0;
    const float t = absorb - 1.18;
    const float rw = t * 620 * dpi, w = 16 * dpi;
    const float x = (length(p - orbCenter) - rw) / w;
    return exp(-x * x) * exp(-t * 2.2) * (x < 0 ? 1 : -0.6);
}

float4 PSGlass(VSOut i) : SV_Target {
    float2 p = i.pos.xy;
    const float d0 = CardSdf(p);
    // Gradient of the field: the rim's normal, and its length to turn the corner's Lp distance into true pixels (the
    // superellipse field runs slower than 1 px per px in the corners; un-normalised, the edge there was too sharp and
    // stair-stepped).
    const float e = 1.0;
    const float2 grad = float2(CardSdf(p + float2(e, 0)) - CardSdf(p - float2(e, 0)), CardSdf(p + float2(0, e)) - CardSdf(p - float2(0, e))) / (2 * e);
    const float gl = max(length(grad), 0.2);
    const float d = d0 / gl;
    const float2 n = grad / gl;
    // soft shadow under the card (everywhere; the card covers it)
    const float ds = CardSdf(p - float2(0, 10 * dpi)) / gl;
    const float shadow = 0.38 * exp(-max(ds, 0) / (14 * dpi)) * (1 - smoothstep(0, 46 * dpi, ds)) * appear;
    const float cover = saturate(0.5 - d);                     // pixel coverage: a full 1-px ramp across the edge
    if (cover <= 0) return float4(0, 0, 0, shadow);
    const float bevelW = 22 * dpi;
    const float bevel = saturate(1 + d / bevelW);             // 0 inside, 1 at the rim
    const float bend = bevel * bevel * bevel;
    float2 refr = -n * bend * 26 * dpi;
    const float wv = Wave(p);
    refr += normalize(p - orbCenter + 1e-4) * wv * 14 * dpi;
    float3 frost = Desk(p + refr, 6.2);
    const float3 sharp = Desk(p + refr * 1.6, 2.0);
    frost = lerp(frost, sharp, 0.35 * bend);
    // Frosted glass: the desktop behind it, blurred, under a light dark tint. Behind the orb the glass deepens into a
    // soft dark lens (like the night the orb floats in), so the orb and the light round it stand out on any desktop.
    float3 col = lerp(frost, float3(0.015, 0.025, 0.06), 0.42);
    {
        const float dl = length(p - orbCenter) / orbR;
        const float lens = 1 - smoothstep(1.55, 2.6, dl);
        col = lerp(col, col * 0.18 + float3(0.004, 0.008, 0.02), lens * 0.9 * saturate(intro / 0.6 + 0.2));
    }
    const float2 uv = (p - cardMin) / (cardMax - cardMin);
    col += float3(0.05, 0.07, 0.11) * (1 - uv.y) * 0.6;       // top sheen
    // Specular rim: bright where the edge faces the light (upper left), a fainter counter-light lower right; a soft
    // ~1.5 px line just inside the edge (a hard 1 px line aliases in the corners).
    const float rimX = (d + 1.1 * dpi) / (0.75 * dpi);
    const float rim = exp(-rimX * rimX);
    const float2 L = normalize(float2(-0.55, -0.85));
    const float lit = pow(saturate(dot(n, L)), 2.0), back = pow(saturate(dot(n, -L)), 3.0);
    col += rim * (0.75 * lit + 0.22 * back + 0.06);
    col += smoothstep(-10 * dpi, 0, d) * 0.035;                // inner glow along the edge
    {   // The orb's light on the glass: a soft bloom whose hue drifts slowly between blue, teal and violet (like
        // light in a nebula); it dims as the orb collapses, then the flash takes over.
        const float2 v = p - orbCenter;
        const float rr = length(v) / orbR;
        const float k = absorb > 0 ? saturate((absorb - 0.3) / 0.9) : 0;
        const float ang = atan2(v.y, v.x);
        const float drift = 0.5 + 0.5 * sin(ang * 2.0 + time * 0.35);
        const float3 tint = lerp(float3(0.04, 0.17, 0.45), lerp(float3(0.02, 0.22, 0.30), float3(0.16, 0.08, 0.40), drift), 0.45);
        col += tint * exp(-rr * rr * 0.5) * 0.6 * saturate(intro / 0.9) * (1 - k);
        col += float3(0.5, 0.8, 1.0) * max(wv, 0) * 0.35;  // the shockwave's bright front
        if (codeMode > 3.5) {  // satellite mode: the three orbits as hairlines (even all round: no peaks for the scanner)
            float orbit = 0;
            [unroll] for (int g = 0; g < 3; ++g) {
                const float rg = (g == 0 ? 1.32 : g == 1 ? 1.56 : 1.95) * orbR;
                orbit += exp(-pow((length(v) - rg) / (0.55 * dpi), 2));
            }
            col += float3(0.25, 0.5, 1.0) * orbit * 0.06 * saturate(intro / 1.0) * (1 - k);
        }
    }
    col += (Hash2(p + frac(time) * 61.0) - 0.5) * 0.012;  // film grain: the glass is a material, not a flat fill
    // card over its own shadow: coverage blends the two across the edge
    const float a = cover * appear;
    const float4 result = float4(pow(saturate(col), 1 / 2.2) * a, a + shadow * (1 - a));
    return codeMode < 0.5 ? DitherBgra8(result, p) : result;
}

// ---------------------------------------------------------------------------------------------------------------------
struct POut { float4 pos : SV_Position; float2 local : TEXCOORD0; float4 col : COLOR0; };

float Ease(float x) { x = saturate(x); return x * x * (3 - 2 * x); }

// Radar (code mode 3): a beam sweeps round the orb once every 3 s (clockwise on screen); what it passes lights up and
// fades like the phosphor of an old radar screen. Afterglow 1 right behind the beam, decaying with the angle since.
static const float kRadarPeriod = 3.0;
float RadarAngle() { return frac(time / kRadarPeriod) * 2 * PI; }
float Afterglow(float angle, float decay) {
    const float since = fmod(RadarAngle() - angle + 8 * PI, 2 * PI);  // how far the beam has moved on since
    return exp(-since * decay);
}

// Dynamic code: an arc's flare, exactly as orb_code.h FlarePhase / FlareLevel (period 1.2 s, lit ~0.3 s).
float FlarePhase(uint ring, uint slot) {
    uint h = ring * 977u + slot * 131u + 7u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xFFFFu) / 65536.0;
}
float FlareLevel(uint ring, uint slot, float t) {
    float x = t / 1.2 + FlarePhase(ring, slot);
    x -= floor(x);
    const float on = 0.30 / 1.2;
    return Ease(x / 0.05) * (1 - Ease((x - on + 0.05) / 0.05));
}
float Hash(float x) { return frac(sin(x * 91.3458) * 47453.5453); }

POut VSParticle(uint vid : SV_VertexID) {
    static const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
    const Particle P = parts[vid / 6];
    const float2 l = corners[vid % 6];
    const int type = (int)P.a.x;
    const float seed = P.b.w;
    float2 pos; float size; float3 col; float alpha;
    // Intro: particles gather from a wider, fainter cloud.
    const float in0 = Ease(intro / 0.9 - 0.25 * seed);
    const float ab = absorb;                                    // < 0: idle
    if (type == 0) {
        // ---- orb: particles on (and a little inside) a sphere, flowing along latitude bands, tilted towards the
        //      viewer; front ones larger and brighter, back ones dim (a glowing volume).
        float lat = P.a.y, lon = P.a.z, rad = P.a.w;
        float k = ab > 0 ? Ease((ab - 0.30) / 0.95) : 0;        // collapse
        const float spin = 0.22 + 0.16 * sin(lat * 3.0 + seed * 6.28);
        lon += time * P.b.x * spin * (1 + 5 * k * k) + k * 3.0 * (1 - abs(sin(lat)));
        // organic flow: slow, large swirls over the sphere (sums of drifting waves), like a fluid, not a turntable
        lat += 0.09 * sin(time * 0.43 + lon * 2.0 + seed * 9.0) + 0.05 * sin(time * 0.71 - lon * 3.0 + lat * 2.0);
        lon += 0.12 * sin(time * 0.37 + lat * 4.0) + 0.06 * sin(time * 0.9 + lat * 7.0 + lon);
        rad *= 1 + 0.035 * sin(time * 1.1 + lat * 5.0 + seed * 10.0) + 0.02 * sin(time * 0.6 + lon * 3.0);
        rad *= (1 + 1.6 * (1 - in0)) * (1 - 0.94 * k);
        float3 d = float3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon)) * rad;
        const float tilt = -0.42;
        d = float3(d.x, d.y * cos(tilt) - d.z * sin(tilt), d.y * sin(tilt) + d.z * cos(tilt));
        const float persp = 1 / (1 - 0.16 * d.z);
        pos = orbCenter + float2(d.x, -d.y) * orbR * persp;
        const float front = saturate(0.5 + 0.5 * d.z / max(rad, 1e-3));
        // depth of field: the far side is out of focus (larger, softer, dimmer), the near side crisp
        const float bokeh = lerp(2.1, 0.85, front);
        size = P.b.y * dpi * persp * bokeh;
        const float tw = 0.75 + 0.25 * sin(time * (2.0 + 3.0 * seed) + seed * 40.0);
        col = lerp(float3(0.16, 0.12, 0.85), float3(0.06, 0.40, 1.0), saturate(front * 1.6));   // indigo far, blue near
        col = lerp(col, float3(0.30, 0.85, 1.0), P.b.z * front);                              // cyan highlights
        col = lerp(col, float3(0.90, 0.97, 1.0), pow(front, 6) * 0.55);                       // white glints
        alpha = (0.10 + 0.20 * pow(front, 1.5)) * tw * in0 * (1 + 2.5 * k) / (bokeh * bokeh) * 1.6;
        if (seed > 0.992) {  // a few sparks that flare now and then
            const float flare = pow(saturate(sin(time * 1.3 + seed * 300.0)), 24);
            alpha += 0.9 * flare * front; size *= 1 + 1.2 * flare;
        }
        alpha *= ab > 0 ? 1 - Ease((ab - 1.15) / 0.12) : 1;
    } else if (type == 3) {
        // ---- streams: particles circling the sphere on tilted great circles (each stream its own axis and pace), the
        //      swirling ribbons that make the cloud read as a turning 3D volume
        const float th = P.a.y, ph = P.a.z;
        const float3 axis = float3(sin(th) * cos(ph), cos(th), sin(th) * sin(ph));
        const float3 u0 = normalize(cross(axis, abs(axis.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
        const float3 v0 = cross(axis, u0);
        float k = ab > 0 ? Ease((ab - 0.30) / 0.95) : 0;
        const float f = P.a.w + time * P.b.x * (1 + 4 * k * k);
        float rad = (1.02 + 0.05 * P.b.z) * (1 + 1.6 * (1 - in0)) * (1 - 0.94 * k);
        float3 d = (cos(f) * u0 + sin(f) * v0) * rad + axis * (P.b.z * 0.13);
        const float tilt = -0.42;
        d = float3(d.x, d.y * cos(tilt) - d.z * sin(tilt), d.y * sin(tilt) + d.z * cos(tilt));
        const float persp = 1 / (1 - 0.16 * d.z);
        pos = orbCenter + float2(d.x, -d.y) * orbR * persp;
        const float front = saturate(0.5 + 0.5 * d.z / max(rad, 1e-3));
        size = P.b.y * dpi * persp * (0.7 + 0.7 * front);
        // a stream is brightest at its head and fades along its tail
        const float tail = P.b.w;  // 1 at the head, 0 at the end of the tail
        col = lerp(float3(0.25, 0.65, 1.0), float3(0.85, 0.97, 1.0), front * front);
        alpha = (0.04 + 0.16 * pow(front, 2)) * (0.25 + 0.75 * tail) * in0 * (1 + 2.5 * k);
        alpha *= ab > 0 ? 1 - Ease((ab - 1.15) / 0.12) : 1;
    } else if (type == 1) {
        // ---- code arc: particles flow along a fixed arc (the arc is the bit; the flow is the life)
        const float t = frac(P.b.x + time * (0.05 + 0.03 * Hash(P.a.y * 13.0)) * (P.a.y > 1.45 ? -1 : 1));
        const float s = t * 2 - 1;
        float ang = P.a.z + s * P.a.w;
        float r = (P.a.y + P.b.z * 0.036) * orbR;
        // Code modes. 1, dynamic: an arc shows only while it flares (ring and slot from its radius and centre angle).
        // 2, spinning ring: the whole code turns slowly round the orb (every ring together: the scanner reads any turn,
        // so every frame is a complete code) and each arc flares brighter now and then without ever going out.
        float flare = 1, glow = 1;
        if (codeMode > 0.5 && ab < 0) {
            const uint ringIdx = P.a.y < 1.39 ? 0u : (P.a.y < 1.63 ? 1u : 2u);
            const uint slotIdx = (uint)floor(P.a.z / (2 * PI) * 32.0 + 0.001) & 31u;
            const float f = FlareLevel(ringIdx, slotIdx, time);
            if (codeMode < 1.5) flare = f;
            else if (codeMode < 2.5) glow = lerp(0.82, 1.15, f);  // a flare never dims an arc towards a gap (the scanner splits arcs from gaps)
            else glow = lerp(0.85, 1.32, Afterglow(P.a.z, 1.4));   // radar: lit as the beam passes, never below 0.85
        }
        if (codeMode > 1.5) ang += time * 0.22;
        float k = ab > 0 ? Ease((ab - 0.25 * seed) / 0.75) : 0;  // spiral into the orb
        ang += k * (2.4 + 1.5 * seed);
        r *= pow(saturate(1 - k), 1.4);
        r *= 1 + 0.4 * (1 - in0);
        pos = orbCenter + r * float2(cos(ang), sin(ang));
        size = P.b.y * dpi * (1 + 0.4 * k) * (0.9 + 0.2 * glow);
        const float ends = smoothstep(0.0, 0.18, 1 - abs(s));
        const float tw = 0.85 + 0.15 * sin(time * 4.0 + seed * 50.0);
        // The code is the orb's own corona: the same blue light as the sphere, streaming round it in arcs (the camera
        // reads the brightest of R, G, B, so blue is as clear to it as white). A slow swell of light runs round.
        const float sweep = pow(saturate(cos(ang - time * 0.7)), 6);
        col = lerp(float3(0.10, 0.42, 1.0), float3(0.38, 0.80, 1.0), 0.3 + 0.5 * seed + 0.2 * sweep);
        alpha = 0.62 * ends * tw * codeAlpha * in0 * (1 + 1.5 * k) * (1 - Ease((k - 0.85) / 0.15)) * (1 + 0.35 * sweep) * flare * glow * (codeMode > 3.5 ? 0 : 1);
    } else if (type == 4) {
        // ---- outer star rings (decoration, depth): tilted rings of stars round the orb, each in its own plane, slowly
        //      turning and precessing, nearer parts larger and brighter: a gyroscope of orbits filling the card. They
        //      never cross the code area (a mask by screen distance): there they pass "behind" the orb's light.
        const float ringId = P.a.w;
        const float rad = P.a.y;                                              // in orb radii
        const float tiltA = codeMode > 2.5 ? 0.0 : 0.62 + 0.38 * Hash(ringId * 3.1 + 1.0);  // radar: flat range rings
        const float axis = Hash(ringId * 7.7 + 2.0) * 2 * PI + time * (0.03 + 0.02 * Hash(ringId)) * (Hash(ringId * 5.3) > 0.5 ? 1 : -1);
        float k = ab > 0 ? Ease((ab - 0.15 - 0.03 * ringId) / 0.9) : 0;
        const float phi = P.a.z + (codeMode > 2.5 ? 0.0 : time * P.b.x / rad) * (1 + 3 * k) + (codeMode > 2.5 ? k * 3.0 : 0.0);
        float3 d = float3(cos(phi), sin(phi), 0) * rad;
        d = float3(d.x, d.y * cos(tiltA), d.y * sin(tiltA));                  // tilt about x
        d = float3(d.x * cos(axis) - d.y * sin(axis), d.x * sin(axis) + d.y * cos(axis), d.z);  // spin the tilt axis
        d *= (1 - 0.94 * k) * (1 + 0.5 * (1 - in0));
        const float persp = 1 / (1 - 0.07 * d.z);
        pos = orbCenter + d.xy * orbR * persp;
        const float front = saturate(0.5 + 0.5 * d.z / max(rad, 1e-3));
        size = P.b.y * dpi * persp * lerp(1.7, 0.85, front);                  // far side out of focus
        col = lerp(float3(0.20, 0.25, 0.85), float3(0.45, 0.80, 1.0), front);
        alpha = (0.16 + 0.50 * front * front) * (2.4 / rad) * in0 * (1 + 2 * k) / (size / (P.b.y * dpi));
        alpha *= 0.7 + 0.3 * sin(time * (1.0 + P.b.z) + P.b.w * 40.0);
        if (codeMode > 2.5) {  // radar: the rings sit still and dim; the beam lights them up as it passes (echoes)
            const float2 v = pos - orbCenter;
            alpha *= 0.35 + 2.2 * Afterglow(atan2(v.y, v.x), 2.2);
        }
        const float dist = length(pos - orbCenter) / orbR;
        alpha *= ab > 0 ? 1 : smoothstep(2.05, 2.45, dist);
        alpha *= ab > 0 ? 1 - Ease((ab - 1.1) / 0.15) : 1;
        alpha *= codeMode > 3.5 ? 0 : 1;
    } else if (type == 6) {
        // ---- satellites (code mode 4): comets on three orbits; their positions are the code (orb_code.h EncodeSat).
        //      The constellation turns as one; each comet's head is a tight bright knot, its tail trails behind along
        //      the orbit, thinning and fading. On a successful scan they spiral into the orb one after another.
        const float along = P.a.w;
        float k = ab > 0 ? Ease((ab - 0.08 * P.a.y - 0.2 * Hash(P.a.z * 5.0)) / 0.8) : 0;
        // tail: 0.055 rad (under one code position: a tail never reaches the next satellite's place)
        float ang = P.a.z + time * 0.18 - along * 0.055 + (along == 0 ? P.b.z * 0.02 : 0);
        float r = (P.a.y + P.b.x * (along == 0 ? 0.022 : 0.010)) * orbR;
        ang += k * (2.6 + 0.8 * Hash(P.a.z));
        r *= pow(saturate(1 - k), 1.4) * (1 + 0.4 * (1 - in0));
        pos = orbCenter + r * float2(cos(ang), sin(ang));
        size = P.b.y * dpi * (along == 0 ? 1.0 : 0.9 - 0.4 * along);
        const float pulse = 0.85 + 0.15 * sin(time * 2.3 + P.a.z * 17.0 + P.a.y * 5.0);
        col = along == 0 ? lerp(float3(0.55, 0.85, 1.0), float3(1, 1, 1), 0.55) : lerp(float3(0.30, 0.65, 1.0), float3(0.12, 0.30, 0.95), along);
        alpha = (along == 0 ? 0.75 * pulse : 0.22 * pow(saturate(1 - along), 2.0)) * codeAlpha * in0 * (1 + 1.5 * k) * (1 - Ease((k - 0.85) / 0.15));
        alpha *= codeMode > 3.5 ? 1 : 0;
    } else if (type == 5) {
        // ---- deep field: faint stars drifting slowly out of the depth towards the viewer (they grow and brighten as
        //      they come), the space the rings hang in
        const float cycle = 14.0;
        float z = frac(P.a.w - time / cycle);                                 // 1 far .. 0 near
        z = 0.08 + 0.92 * z;
        float k = ab > 0 ? Ease((ab - 0.1) / 1.0) : 0;
        const float2 dir = P.a.yz;                                            // direction in the picture, -1..1
        const float span = length(cardMax - cardMin) * 0.55;
        pos = orbCenter + dir * span * (0.18 / z) * (1 - 0.9 * k);
        const float near = saturate((1 - z) / 0.92);
        size = P.b.y * dpi * (0.5 + 1.4 * near * near);
        col = lerp(float3(0.35, 0.45, 0.95), float3(0.75, 0.88, 1.0), near);
        alpha = (0.05 + 0.32 * near * near) * saturate((1 - near) * 6) * saturate(near * 8) * in0;
        alpha *= smoothstep(2.1, 2.6, length(pos - orbCenter) / orbR) * (1 - k);
        if (codeMode > 2.5) {  // radar: stars are targets: they flash as the beam finds them
            const float2 v = pos - orbCenter;
            alpha *= 0.4 + 3.0 * Afterglow(atan2(v.y, v.x), 3.0);
        }
    } else {
        // ---- halo: faint dust slowly circling the orb
        float ang = P.a.z + time * (0.06 + 0.05 * seed) * (seed > 0.5 ? 1 : -1);
        float r = P.a.y * orbR * (1 + 0.04 * sin(time * 0.9 + seed * 20.0));
        float k = ab > 0 ? Ease((ab - 0.1 - 0.2 * seed) / 0.8) : 0;
        ang += k * 3.0; r *= pow(saturate(1 - k), 1.3);
        pos = orbCenter + r * float2(cos(ang), sin(ang));
        size = P.b.y * dpi;
        col = float3(0.35, 0.65, 1.0);
        alpha = 0.10 * (0.6 + 0.4 * sin(time * 1.5 + seed * 30.0)) * in0 * (1 - k) * (codeMode > 3.5 ? 0 : 1);
    }
    // Everything stays on the glass: fade out over the last 14 DIP before the card's edge.
    {
        const float2 e = min(pos - cardMin, cardMax - pos);
        alpha *= saturate(min(e.x, e.y) / (14 * dpi));
    }
    POut o;
    const float2 q = pos + l * size;
    o.pos = float4(q.x / targetSize.x * 2 - 1, 1 - q.y / targetSize.y * 2, 0, 1);
    o.local = l;
    o.col = float4(col * alpha * appear, 0);
    return o;
}

float4 PSParticle(POut i) : SV_Target {
    const float r2 = dot(i.local, i.local);
    const float core = exp(-r2 * 4.0) * saturate(1 - r2);
    const float glow = 0.10 * exp(-r2 * 1.3) * saturate(1 - r2);  // a little light around each mote
    return i.col * (core + glow);
}

// ---------------------------------------------------------------------------------------------------------------------
float Segment(float2 p, float2 a, float2 b, float t) {  // distance to the first t (0..1) of segment a-b
    const float2 e = (b - a) * t;
    const float h = saturate(dot(p - a, e) / max(dot(e, e), 1e-6));
    return length(p - a - e * h);
}

float4 PSOverlay(VSOut i) : SV_Target {
    const float2 p = i.pos.xy;
    float4 acc = 0;
    // Radar beam: a bright leading edge and a fading wake, from the orb out to the card's edge; kept faint over the code
    // rings (they light up themselves) so empty slots never look lit. Plus faint tick marks on the outer range ring.
    if (codeMode > 2.5) {
        // only on the glass (the overlay covers the whole window, shadow margin included)
        const float onCard = saturate(0.5 - CardSdf(p)) * saturate(-CardSdf(p) / (10 * dpi));
        const float fadeOut = (absorb > 0 ? 1 - saturate(absorb / 0.4) : saturate(intro / 0.8)) * onCard;
        const float2 v = p - orbCenter;
        const float rr = length(v) / orbR;
        const float since = fmod(RadarAngle() - atan2(v.y, v.x) + 8 * PI, 2 * PI);
        const float wake = exp(-since * 2.6) * (since < 2 * PI - 0.02 ? 1 : 0);
        const float edge = exp(-pow(since / 0.025, 2)) + exp(-pow((2 * PI - since) / 0.012, 2));
        float zone = smoothstep(1.0, 1.15, rr) * (1 - smoothstep(4.6, 5.4, rr));
        zone *= lerp(1.0, 0.18, smoothstep(1.18, 1.25, rr) * (1 - smoothstep(1.95, 2.05, rr)));  // quiet over the code
        const float3 beamCol = float3(0.20, 0.62, 1.0);
        acc.rgb += beamCol * (0.11 * wake + 0.22 * edge) * zone * fadeOut;
        // ticks every 10 degrees on the 3.5 R range ring, longer every 30
        const float tickA = atan2(v.y, v.x) / (2 * PI) * 36;
        const float tickD = abs(frac(tickA + 0.5) - 0.5) * (2 * PI / 36) * length(v);  // px from the nearest tick
        const bool major = abs(frac(tickA / 3 + 0.5 / 3) - 0.5 / 3) < 0.02;
        const float tickLen = (major ? 0.16 : 0.08) * orbR;
        const float tick = (1 - smoothstep(0.4 * dpi, 1.0 * dpi, tickD)) * (1 - smoothstep(0, tickLen, abs(length(v) - 3.62 * orbR)));
        acc.rgb += float3(0.35, 0.6, 1.0) * tick * (0.14 + 0.5 * Afterglow(atan2(v.y, v.x), 2.2)) * fadeOut;
    }
    // Flash as the orb collapses into its centre.
    if (absorb > 0) {
        const float f = exp(-pow((absorb - 1.22) / 0.10, 2));
        const float rr = length(p - orbCenter) / orbR;
        const float glow = f * (exp(-rr * rr * 3.0) + 0.35 * exp(-rr * 0.9));
        acc += float4(float3(0.55, 0.85, 1.0) * glow, 0);
        // The checkmark in a glass bubble.
        const float c = Ease((absorb - 1.25) / 0.35);
        if (c > 0) {
            const float2 q = (p - orbCenter) / orbR;
            const float bubble = length(q) - 0.62;
            const float fill = (1 - smoothstep(-0.01, 0.01, bubble)) * c;
            const float rim = (1 - smoothstep(0.0, 0.03, abs(bubble))) * c;
            acc += float4(float3(0.10, 0.45, 1.0) * fill * 0.55 + rim * 0.5, fill * 0.55);
            const float tk = Ease((absorb - 1.45) / 0.45);
            const float2 a = float2(-0.27, 0.02), b = float2(-0.07, 0.22), d = float2(0.30, -0.20);
            const float t1 = saturate(tk * 1.8), t2 = saturate(tk * 1.8 - 0.8);
            float dist = Segment(q, a, b, t1);
            if (t2 > 0) dist = min(dist, Segment(q, b, d, t2 / 1.0));
            const float stroke = (1 - smoothstep(0.045, 0.06, dist)) * (tk > 0 ? 1 : 0);
            acc = float4(acc.rgb * (1 - stroke) + stroke, max(acc.a, stroke));
        }
    }
    // Close button, top left (macOS): a small glass circle with an x; brighter on hover, pressed in on press.
    {
        const float cr = 11 * dpi * (closeState > 1.5 ? 0.92 : 1.0);
        const float2 q = p - closeCenter;
        const float dc = length(q) - cr;
        const float disc = saturate(0.5 - dc);
        if (disc > 0) {
            const float lift = closeState > 0.5 ? 1.0 : 0.0;
            float3 c = float3(1, 1, 1) * (0.10 + 0.08 * lift);
            const float edge = exp(-pow((dc + 0.9 * dpi) / (0.6 * dpi), 2));
            c += edge * 0.25;
            // the x: two strokes, 1.4 DIP wide, 3.6 DIP from the centre
            const float2 r = float2(q.x + q.y, q.x - q.y) * 0.70710678;
            const float arm = 4.0 * dpi, w = 0.75 * dpi;
            const float x1 = max(abs(r.y) - w, abs(r.x) - arm), x2 = max(abs(r.x) - w, abs(r.y) - arm);
            const float cross = saturate(0.5 - min(x1, x2)) * (0.55 + 0.4 * lift);
            c = lerp(c, float3(1, 1, 1), cross);
            const float ca = disc * (0.55 + 0.25 * lift + 0.2 * cross);
            acc = float4(acc.rgb * (1 - ca) + c * ca, max(acc.a, ca));
        }
    }
    // Text (two blocks stacked in the texture: "scan", "connected").
    const float2 cardC = 0.5 * (cardMin + cardMax);
    uint tw, th; textTex.GetDimensions(tw, th);
    const float2 tl = float2(cardC.x - (float)tw * 0.5, textTop);
    const float2 tp = p - tl;
    if (tp.x >= 0 && tp.y >= 0 && tp.x < (float)tw && tp.y < textHeight) {
        const float s0 = textTex.SampleLevel(linearClamp, (tp + 0.5) / float2(tw, th), 0);
        const float s1 = textTex.SampleLevel(linearClamp, (tp + float2(0, textHeight) + 0.5) / float2(tw, th), 0);
        const float cov = s0 * saturate(1 - 2 * textMix) + s1 * saturate(2 * textMix - 1);  // out, then in
        acc = float4(acc.rgb * (1 - cov) + cov * 0.94, max(acc.a, cov));
    }
    return acc * appear;
}
