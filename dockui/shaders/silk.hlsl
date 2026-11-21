// Liquid silk for the Now Playing lyrics side, shaded like glossy satin and coloured from the album artwork.
// Shape: one main twisting fold that always stays inside the lyrics area (its centre, angle and bend move but are
// bounded), a softer second fold, and a broad undulation. The beat ("twist") makes the folds bend and twist harder
// instead of brightening anything (gently). Colours are soft pools (one per palette colour, sized by its share of the
// artwork) that the same flow drags and swirls into each other, so they mix like paint rather than sit in bands.
// Drawn into a half-resolution texture each frame and composed (blurred) by Direct2D behind the lyrics.
cbuffer Silk : register(b0) {
    float2 resolution;  // texture size in pixels
    float time;         // flow clock (runs faster with louder music)
    float twist;        // 0..~1.3, smoothed beat: how hard the folds bend and twist
    float4 colors[6];   // rgb = palette colour, a = its share (shares sum to 1, largest first)
    float colorCount;
    float3 padding;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// One triangle covering the target (no vertex buffer).
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    const float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

// Cross-section of a twisted fold: a drape (one side up, the other down) rotating into a ridge along its length.
float Fold(float d, float width, float phase) {
    const float s = d / width;
    const float g = exp(-s * s);
    const float drape = s * g * 2.33;   // -1 .. 1
    const float ridge = g * 1.4 - 0.35;
    return cos(phase) * drape + sin(phase) * ridge;
}

// One fold along a line through centre at angle; the line bends along its length.
float FoldAt(float2 p, float2 centre, float angle, float bendAmp, float width, float twistRate, float t, float seed) {
    const float2 dir = float2(cos(angle), sin(angle));
    const float2 r = p - centre;
    const float along = dot(r, dir);
    const float bend = bendAmp * (sin(along * 3.0 + t * 0.8 + seed) + 0.35 * sin(along * 6.3 - t * 1.1 + seed * 2.0));
    const float d = dot(r, float2(-dir.y, dir.x)) - bend;
    return Fold(d, width * (1.0 + 0.2 * sin(along * 2.1 + t * 0.5 + seed)), along * twistRate + t * 0.45 + seed);
}

// p: x scaled by aspect, y 0..1. The lyrics are on the right ~60 % of the texture (the left part fades into the artwork).
float Height(float2 p, float t, float aspect) {
    const float k = twist;
    p += (0.045 + 0.04 * k) * float2(sin(p.y * 3.1 + t * 0.7), sin(p.x * 2.7 - t * 0.6));
    // Main fold: centre kept within x 0.55..0.80, y 0.30..0.70 of the area, near-diagonal, never leaves the screen.
    const float2 c1 = float2((0.675 + 0.125 * sin(t * 0.23)) * aspect, 0.5 + 0.2 * sin(t * 0.31 + 1.0));
    float h = FoldAt(p, c1, 1.2 + 0.35 * sin(t * 0.19), 0.095 + 0.08 * k, 0.19, 1.5 + 0.9 * k, t, 0.0);
    // Second, softer fold drifting over the rest.
    const float2 c2 = float2((0.35 + 0.15 * sin(t * 0.17 + 2.0)) * aspect, 0.5 + 0.3 * sin(t * 0.13 + 4.0));
    h += 0.6 * FoldAt(p, c2, -0.55 + 0.4 * sin(t * 0.15 + 1.0), 0.075 + 0.06 * k, 0.26, 1.1 + 0.7 * k, t * 0.8, 2.7);
    h += 0.22 * sin(p.x * 1.4 + p.y * 0.9 + t * 0.2);
    return h;
}

// Colours as soft pools: each palette colour has a pool drifting on its own slow path; its pull is its share, so the
// main colour fills most of the area. q is the position after the flow has dragged and swirled it.
float3 PoolColour(float2 q, float t, float aspect) {
    const int n = (int)max(colorCount, 1.0);
    float3 sum = colors[0].rgb * colors[0].a * 0.35;  // the main colour also fills the gaps
    float total = colors[0].a * 0.35;
    [unroll] for (int i = 0; i < 6; ++i) {
        if (i < n) {
            const float fi = float(i);
            const float2 c = float2((0.55 + 0.42 * sin(t * (0.11 + 0.023 * fi) + fi * 2.1)) * aspect,
                                    0.5 + 0.45 * sin(t * (0.09 + 0.019 * fi) + fi * 1.3 + 0.7));
            const float2 d = q - c;
            const float r = 0.2 + 0.4 * sqrt(colors[i].a);  // bigger pools for bigger shares
            const float w = pow(max(colors[i].a, 1e-4), 0.6) * exp(-dot(d, d) / (r * r));  // ^0.6: minor colours still show in their pools
            sum += colors[i].rgb * w; total += w;
        }
    }
    return sum / max(total, 1e-4);
}

float4 PSMain(VSOut input) : SV_Target {
    const float aspect = resolution.x / max(resolution.y, 1.0);
    const float2 p = float2(input.uv.x * aspect, input.uv.y);
    const float t = time;
    const float e = 2.0 / max(resolution.y, 1.0);
    const float h = Height(p, t, aspect);
    const float dx = (Height(p + float2(e, 0), t, aspect) - Height(p - float2(e, 0), t, aspect)) / (2 * e);
    const float dy = (Height(p + float2(0, e), t, aspect) - Height(p - float2(0, e), t, aspect)) / (2 * e);
    const float relief = 0.17;
    const float3 n = normalize(float3(-dx * relief, -dy * relief, 1));
    const float3 l = normalize(float3(-0.45, -0.6, 0.66));
    const float diffuse = saturate(dot(n, l));
    const float3 halfway = normalize(l + float3(0, 0, 1));
    const float specular = pow(saturate(dot(n, halfway)), 36.0) * 0.5 + pow(saturate(dot(n, halfway)), 8.0) * 0.08;
    const float sheen = pow(1.0 - saturate(n.z), 1.4);
    // The colours ride the flow: a two-layer swirl plus a push along the fold slope, so the pools smear into each other.
    float2 q = p;
    q += 0.22 * float2(sin(q.y * 2.2 + t * 0.35), sin(q.x * 1.9 - t * 0.30));
    q += 0.14 * float2(sin(q.y * 3.7 - t * 0.27 + 1.9), sin(q.x * 3.3 + t * 0.33 + 0.4));
    q += 0.035 * float2(dx, dy) / (1.0 + 0.08 * length(float2(dx, dy)));
    const float3 base = PoolColour(q, t, aspect);
    // Highlights follow the cloth's brightness: dark covers get a dim sheen, not grey-white streaks.
    const float lum = dot(base, float3(0.299, 0.587, 0.114));
    float3 c = base * (0.38 + 0.72 * diffuse) + specular * lerp(base, float3(1, 1, 1), 0.4) * (0.3 + 0.9 * lum) + sheen * base * 0.45;
    c *= 1.0 - 0.35 * saturate(-h * 0.7);  // creases fall into shadow
    return float4(c, 1);
}
