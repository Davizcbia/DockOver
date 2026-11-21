// Liquid Dock - Liquid Glass plate material (P0.3).
// Written independently for the Dock from the effect requirements in
// dockui/docs/p0.3-glass-lab-port.md (parameter names follow Glass Lab's JSON so presets transfer).
// It does not copy or translate Glass Lab's GPL-3.0-only shader; the techniques are standard:
// Lp-norm rounded-box SDF, finite-difference normals, Snell refraction (refract), Schlick Fresnel,
// Blinn-Phong highlights and a Reinhard-style highlight knee. All distances are physical pixels.

cbuffer GlassConstants : register(b0)
{
    float4 plate;        // centre.xy, half size.xy
    float4 geom;         // corner radius, bevel width, corner extent factor, corner exponent
    float4 placement;    // window origin on screen (x, y), dpi scale, opacity
    float4 sharpMap;     // backdrop screen origin (x, y), 1 / backdrop size (x, y)
    float4 blurMap;      // blur ROI screen origin (x, y), 1 / ROI size (x, y)
    float4 optics;       // refraction, refractive index, dispersion, magnification
    float4 mixing;       // frost, saturation, brightness, density
    float4 tone;         // glass lift, dark (0/1), reduced transparency (0/1), shadow amount
    float4 strokeLight;  // stroke width, stroke opacity, fixed reflection (0/1), glare
    float4 fixedLight;   // top light, bottom light, bottom glow, edge amount
    float4 dynLight;     // fresnel, specular, cos(light angle), sin(light angle)
    float4 tint;         // rgb, strength
    float4 pointer;      // pointer x, y (window px), hover, press
    float4 interact;     // strength, radius (px), debug view, plate rotation (radians, clockwise on screen)
    float4 extra;        // body tone amount (1 = normal glass, 0 = pure blur), edge softness (px), unused, unused
};

Texture2D SharpTex : register(t0);
Texture2D BlurTex  : register(t1);
SamplerState LinearClamp : register(s0);

struct VSOut { float4 pos : SV_Position; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Full-target triangle.
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Rounded box whose corners are superellipses: an Lp norm replaces the Euclidean length of the
// classic rounded-box SDF. extent > radius lets the curvature start earlier (continuous corner);
// near-capsule shapes blend to exact circular ends.
float PlateSdf(float2 p)
{
    float2 h = plate.zw;
    float shortHalf = min(h.x, h.y);
    float r = clamp(geom.x, 0.0, shortHalf);
    if (r < 0.01)
    {
        float2 q0 = abs(p) - h;
        return length(max(q0, 0.0)) + min(max(q0.x, q0.y), 0.0);
    }
    float capsule = saturate((r / max(shortHalf, 0.01) - 0.85) / 0.15);
    float e = min(r * lerp(geom.z, 1.0, capsule), shortHalf);
    float n = lerp(geom.w, 2.0, capsule);
    float2 q = abs(p) - h + e;
    float2 a = max(q, 0.0);
    float lp = pow(pow(a.x, n) + pow(a.y, n), 1.0 / n);
    return lp - e + min(max(q.x, q.y), 0.0);
}

// Screen <-> plate-local rotation (y down: positive angle turns clockwise).
float2 Rot(float2 v, float a)
{
    float c = cos(a), sn = sin(a);
    return float2(c * v.x - sn * v.y, sn * v.x + c * v.y);
}

float3 SampleSharp(float2 screen)
{
    float2 uv = (screen - sharpMap.xy) * sharpMap.zw;
    return SharpTex.SampleLevel(LinearClamp, uv, 0).rgb;
}

float3 SampleBlur(float2 screen)
{
    float2 uv = (screen - blurMap.xy) * blurMap.zw;
    return BlurTex.SampleLevel(LinearClamp, uv, 0).rgb;
}

float3 KneeHighlights(float3 c)
{
    // Linear to 0.8, then compresses smoothly toward 1.0 so bright backdrops keep gradation.
    float3 over = max(c - 0.8, 0.0);
    return min(c, 0.8) + over / (1.0 + over * 5.0);
}

float3 Saturate709(float3 c, float s)
{
    float y = dot(c, kLuma);
    float3 o = y + (c - y) * s;
    float top = max(o.r, max(o.g, o.b));
    if (top > 1.0 && top - y > 1e-4)
        o = y + (o - y) * ((max(1.0, y) - y) / (top - y));  // keep hue, pull back into gamut
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
    float s = placement.z;                    // DIP -> px
    float2 pix = input.pos.xy;                // window px (pixel centre)
    float2 p = Rot(pix - plate.xy, -interact.w);  // plate-local coordinates
    float d = PlateSdf(p);
    float aa = max(fwidth(d), 0.65) + max(extra.y, 0.0);  // extra.y > 0 defocuses the outline
    float coverage = 1.0 - smoothstep(-aa, aa, d);

    // Soft lift shadow below/around the plate (contact + wide term).
    float ds = PlateSdf(Rot(pix - plate.xy - float2(0.0, 2.0 * s), -interact.w));  // shadow falls down on screen
    float sh = 0.05 * exp(-max(ds, 0.0) / (2.0 * s)) + 0.02 * exp(-max(ds, 0.0) / (3.5 * s));
    sh *= (1.0 - smoothstep(7.0 * s, 11.0 * s, max(d, 0.0))) * tone.w;
    float outside = sh * (1.0 - coverage);
    float opacity = placement.w;
    if (coverage <= 0.0)
        return float4(0, 0, 0, outside * opacity);

    bool dark = tone.y > 0.5;
    if (tone.z > 0.5)
    {
        // Reduced transparency: opaque neutral material with the hairline.
        float3 solid = dark ? 0.12 : 0.94;
        float sw = strokeLight.x * s;
        float edge = sw > 0.0 ? smoothstep(-sw - aa, -sw + aa, d) : 0.0;
        solid *= 1.0 - strokeLight.y * edge * (dark ? 1.0 : 0.5);
        return float4(solid * coverage, coverage + outside) * opacity;
    }

    // Surface normal from the SDF gradient; bevel band of width bevelWidth inside the outline.
    float2 g = float2(PlateSdf(p + float2(0.5, 0)) - PlateSdf(p - float2(0.5, 0)),
                      PlateSdf(p + float2(0, 0.5)) - PlateSdf(p - float2(0, 0.5)));
    float2 n2 = Rot(normalize(g + 1e-5), interact.w);  // back to screen space for refraction and light
    float inside = max(-d, 0.0);
    float bevel = min(geom.y * s, min(plate.z, plate.w) * 0.6);
    float t = saturate(inside / max(bevel, 1.0));
    float slope = 1.0 - smoothstep(0.0, 1.0, t);                 // steep at the rim, flat (zero slope) inside
    float rim = 1.0 - smoothstep(0.0, 1.0, t);

    // Pointer: Gaussian influence; press deepens the lens, hover tilts it slightly (dynamic only).
    float2 pd = pix - pointer.xy;
    float ir = max(interact.y * s, 1.0);
    float influence = exp(-dot(pd, pd) / (2.0 * ir * ir)) * interact.x;
    float press = pointer.w * influence;
    float follow = strokeLight.z > 0.5 ? 0.0 : 1.0;
    float2 tilt = pd / ir * influence * (0.04 * pointer.z * follow + 0.15 * pointer.w);

    float3 n3 = normalize(float3(n2 * slope + tilt, max(sqrt(saturate(1.0 - slope * slope)), 0.05)));
    float3 ray = refract(float3(0, 0, -1), n3, 1.0 / max(optics.y, 1.0));
    if (dot(ray, ray) < 1e-6) ray = float3(-n3.xy, -0.2);
    float depth = min(bevel * 1.6, 28.0 * s) * optics.x * (1.0 + 0.25 * press);
    float2 shift = ray.xy / max(-ray.z, 0.2) * depth;
    float2 zoom = p * (1.0 / max(optics.w, 0.25) - 1.0);

    float2 screen = placement.xy + pix;
    float2 sp = screen + shift + zoom;
    float2 chroma = shift * optics.z;
    float3 clear = float3(SampleSharp(sp + chroma).r, SampleSharp(sp).g, SampleSharp(sp - chroma).b);
    // Both optical paths use the displaced coordinates, including dispersion.
    // Frost blends resolutions without exposing a sharp strip along the bevel.
    float3 frosted = float3(SampleBlur(sp + chroma).r, SampleBlur(sp).g, SampleBlur(sp - chroma).b);
    float f = saturate(mixing.x);
    float bodyMix = f * (2.0 - f);
    float innerBand = 1.0 - smoothstep(0.0, max(3.0 * s, bevel * 0.42), inside);
    float2 footprint = float2(-n2.y, n2.x) * (1.75 * s * innerBand);
    float3 softEdge = (SampleSharp(sp + footprint) + SampleSharp(sp - footprint)) * 0.5;
    clear = lerp(clear, softEdge, 0.35 * innerBand);
    float3 col = lerp(clear, frosted, bodyMix);
    col = Saturate709(col, mixing.y) * mixing.z;

    // Glass body tone: dark glass lifts dark backdrops slightly toward white and shades bright
    // backdrops toward grey; light glass veils toward near-white. density scales both.
    float density = saturate(mixing.w);
    float3 untoned = col;
    float luma = dot(col, kLuma);
    float3 base = dark ? 0.12 : 0.97;
    float3 lightGlass = lerp(col, base, 0.22 + 0.28 * density);
    float3 lifted = lerp(col, 1.0, saturate((0.025 + 0.035 * density) * tone.x));
    float3 shaded = lerp(col, base, (0.38 + 0.30 * density) * saturate(extra.z));  // extra.z: highlight dimming slider
    float3 darkGlass = lerp(lifted, shaded, smoothstep(0.30, 0.80, luma));
    col = dark ? darkGlass : lightGlass;
    col = lerp(untoned, col, saturate(extra.x));
    col = lerp(col, tint.rgb, tint.a * 0.25);

    // Hairline stroke just inside the outline (fades at top/bottom with fixed reflection).
    float sw = strokeLight.x * s;
    float stroke = sw > 0.0 ? smoothstep(-sw - aa, -sw + aa, d) : 0.0;
    if (strokeLight.z > 0.5) stroke *= 1.0 - n2.y * n2.y;
    col *= 1.0 - strokeLight.y * stroke * (dark ? 1.0 : 0.5);

    // Highlights get less room over bright backdrops and on denser material.
    float room = lerp(1.0, 0.5, smoothstep(0.25, 0.9, dot(col, kLuma))) * lerp(1.0, 0.5, density);
    float glare = strokeLight.w;
    if (strokeLight.z > 0.5)
    {
        // Fixed reflection (macOS-like): bright hairlines on the top and bottom edges only, a soft
        // glow above the bottom line, nothing on the sides. Coefficients from the V0.2 macOS
        // measurement (rim about +0.17 over the plate at the default glare).
        float topBottom = n2.y * n2.y;
        float rimLine = exp(-abs(d + 0.5 * s) / (0.55 * s)) * topBottom * (n2.y < 0.0 ? fixedLight.x : fixedLight.y);
        float bottomGlow = exp(-inside / (3.0 * s)) * pow(max(n2.y, 0.0), 2.0) * fixedLight.z;
        float topGlow = exp(-inside / (1.5 * s)) * pow(max(-n2.y, 0.0), 2.0) * fixedLight.x;
        col += (0.45 * rimLine + 0.10 * bottomGlow + 0.04 * topGlow) * glare * room * fixedLight.w;
    }
    else
    {
        float2 L = normalize(float2(dynLight.z, dynLight.w));
        float2 toPointer = normalize((pointer.xy - plate.xy) / max(plate.zw, 1.0) + float2(1e-5, -0.25));
        L = normalize(lerp(L, toPointer, saturate(pointer.z * interact.x * 0.5)));
        float facing = pow(saturate(dot(n2, L)), 3.0);
        float back = pow(saturate(dot(n2, -L)), 4.0);
        float schlick = 0.04 + 0.96 * pow(1.0 - n3.z, 5.0);
        col += schlick * rim * (0.45 * facing + 0.10 * back) * room * glare * dynLight.x;
        float3 key = normalize(normalize(float3(L * 0.9, 0.7)) + float3(0, 0, 1));
        float3 fill = normalize(normalize(float3(-L * 0.65, 0.9)) + float3(0, 0, 1));
        float spec = pow(saturate(dot(n3, key)), 40.0) + 0.3 * pow(saturate(dot(n3, fill)), 20.0);
        col += spec * exp(-inside / (1.1 * s)) * glare * 0.16 * room * dynLight.y;
        float outline = exp(-abs(d + 0.35 * s) * 2.8 / s);
        col += outline * (0.014 + glare * (0.18 * facing + 0.04 * back)) * room * fixedLight.w;
    }
    float yNorm = saturate((pix.y - (plate.y - plate.w)) / max(2.0 * plate.w, 1.0));
    col += (1.0 - yNorm) * (1.0 - yNorm) * (dark ? 0.006 : 0.012) * room;
    col += influence * pointer.z * follow * (0.008 + 0.025 * exp(-inside / (1.1 * s))) * room;
    col += press * exp(-inside / (1.1 * s)) * 0.018 * room;

    if (dark) col = KneeHighlights(col);  // always: keeps the glass's own rim/glare from clipping to white (the slider only scales 'shaded')

    int view = (int)interact.z;
    if (view == 1) col = lerp(float3(0.1, 0.3, 0.9), float3(0.9, 0.5, 0.1), frac(-d / (8.0 * s))) * (0.4 + 0.6 * t);
    else if (view == 2) col = n3 * 0.5 + 0.5;
    else if (view == 3) col = float3(shift / max(depth, 1.0) * 0.5 + 0.5, rim);
    else if (view == 4) col = frosted;
    else if (view == 5) col = clear;
    else if (view == 6) col = bodyMix;

    return float4(saturate(col) * coverage, coverage + outside) * opacity;
}
