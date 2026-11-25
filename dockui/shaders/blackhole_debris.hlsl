// Black Hole Visualizer: debris particles (icon pixels torn off by the hole's tides) drawn into the lens-plane texture
// (M coordinates around the hole). blackhole.hlsl samples that texture wherever a photon crosses the lens plane, so the
// stream is lensed like everything else. Particles: blackhole_debris.h (DebrisGpu layout).

struct Particle {
    float4 posSize;  // x, y (M, hole-relative, y down), half length along the motion, half width (M)
    float4 dir;      // unit motion direction (xy), softness (z: 0 = a crisp pixel, 1 = a soft wisp of gas), w: 1 = position
                     // and direction in the tilted debris orbit plane (seen at y * cosTilt), 0 = screen axes
    float4 color;    // premultiplied linear rgba (rgb may exceed alpha: hot gas glows)
};
StructuredBuffer<Particle> parts : register(t0);
cbuffer DebrisConstants : register(b0) {
    float extent;  // the texture covers [-extent, extent]^2 M (as seen: the orbit plane's y times cosTilt)
    float texelM;  // one texel in M
    float cosTilt;
    float pad;
};

struct VOut { float4 pos : SV_Position; float2 local : TEXCOORD0; float4 color : COLOR0; float soft : TEXCOORD1; };

VOut VSDebris(uint vid : SV_VertexID) {
    static const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
    const Particle p = parts[vid / 6];
    const float2 l = corners[vid % 6];
    float2 d = dot(p.dir.xy, p.dir.xy) > 1e-8 ? normalize(p.dir.xy) : float2(1, 0);
    float2 c = p.posSize.xy;
    float2 sz = p.posSize.zw;
    if (p.dir.w > 0.5) {  // orbit plane -> as seen: the length along the motion foreshortens, the width (a tube) does not
        c.y *= cosTilt;
        const float2 a = float2(d.x, d.y * cosTilt) * sz.x;
        const float la = length(a);
        d = la > 1e-6 ? a / la : float2(1, 0);
        sz.x = max(la, sz.y);
    }
    const float2 n = float2(-d.y, d.x);
    // Never thinner than ~0.7 texel (a sub-texel pixel would shimmer); keep the coverage by thinning the alpha instead.
    const float2 size = max(sz, 0.7 * texelM);
    const float cover = saturate(sz.x * sz.y / (size.x * size.y));
    const float2 q = c + d * l.x * size.x + n * l.y * size.y;
    VOut o;
    o.pos = float4(q.x / extent, -q.y / extent, 0, 1);
    o.local = l;
    o.color = p.color * cover;
    o.soft = p.dir.z;
    return o;
}

float4 PSDebris(VOut i) : SV_Target {
    // A pixel (square, slightly soft edge) that turns into a wisp of gas (gaussian) as it streams: overlapping wisps
    // read as a continuous filament.
    const float2 a = abs(i.local);
    const float sq = (1 - smoothstep(0.6, 1.0, a.x)) * (1 - smoothstep(0.6, 1.0, a.y));
    const float gs = exp(-3.0 * dot(i.local, i.local)) * saturate(1 - dot(i.local, i.local));
    return i.color * lerp(sq, gs, i.soft);
}
