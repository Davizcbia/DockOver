#include "quantize.hlsli"

// docky-bubble: the Optical Bubble's look (Phase 2, no code yet): a ball of clear liquid glass, ray traced per pixel.
//
// Model (ball units: radius 1, x right, y down, z away from the viewer; eye at z = -E, perspective):
//   * surface: a sphere displaced by h(n, t) = slow low-order modes (breathing, surface-tension wobble) + ripples:
//     wave packets that run out from impact points over the sphere and fade (new impacts keep coming);
//   * the eye ray is traced to the front surface (sphere tracing), refracted in, traced through the inside to the back
//     surface, refracted out - once per colour channel with its own index of refraction (dispersion: it only shows
//     where the rays bend hard, at the rim and on the ripples) - and looks up the background on a plane behind the
//     ball (the desktop, an image or a studio backdrop): the ball shows the scene behind it upside down and magnified,
//     moving with it;
//   * Fresnel reflection of a studio environment (soft boxes) and of the blurred background; a thin film (soap-film
//     interference) on parts of the surface tints reflections, strongest in a grazing rim light (bottom right);
//   * Beer-Lambert absorption along the path inside (thicker = bluer); caustics: light from the key light focused by
//     the ripples, seen on the far inside of the ball, and the ball-lens crescent opposite the light;
//   * small air bubbles rising slowly inside.

cbuffer Constants : register(b0) {
    float2 targetSize; float2 center;   // px
    float radius; float time; float mode; float opaque;  // mode 0 desktop, 1 image, 2 studio, 3 dark studio
    float2 deskOrigin; float2 deskSize; // desktop: the window's top-left in the desktop texture (px), its size
    float2 imgScale; float2 imgOffset;  // image: uv = px / targetSize * imgScale + imgOffset
    float dpi; float showBubbles; float rippleAmp; float codeMode;   // code 0 none, 1 A (optical), 2 B (invisible), 4 rays
    float codeAmp; float successAge; float phonePanel; float reducedMotion;
    uint4 codeBits[2];                                                 // Bubble Code v1 cells, ring after ring
    float4 mouseImpacts[3];                                           // xyz: surface normal, w: current amplitude
    float4 mouseFronts;                                               // distance travelled on the sphere (chord units)
    float4 waterDrops[16];                                           // xy: position in ball units, z: radius, w: opacity
    float splashCount; float sinkAge; float finishAge; float fileCount;
    uint4 qrBits[47];
    uint qrSize; float rimProgress; float rimOpacity; float qrPad;
    float4 pcPeers[5];  // screen position, radius, opacity
};
Texture2D bgTex : register(t0);
SamplerState samp : register(s0);

static const float E = 6.0;    // eye distance (ball radii)
static const float BZ = 7.0;   // background plane behind the ball centre (well past the ball's focus at ~1.8:
                               // the scene shows inverted and minified, filling the ball, as through a real lens ball)
static const float PI = 3.14159265;

struct VSOut { float4 pos : SV_Position; };
VSOut VSFull(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 hash3(float k) { return frac(sin(float3(k, k + 1.7, k + 3.1) * float3(12.9898, 78.233, 37.719)) * 43758.5453); }

// ---- the surface -------------------------------------------------------------------------------------------------
float lowModes(float3 n, float t) {
    return 0.030 * sin(dot(n, float3(0.80, 0.50, 0.30)) * 2.1 + t * 0.55)
         + 0.022 * sin(dot(n, float3(-0.30, 0.90, 0.40)) * 2.7 - t * 0.43 + 1.3)
         + 0.016 * sin(dot(n, float3(0.50, -0.20, 0.85)) * 3.3 + t * 0.71 + 2.1)
         + 0.010 * sin(dot(n, float3(-0.7, -0.4, 0.6)) * 4.1 - t * 0.9 + 0.4)
         + 0.006 * sin(dot(n, float3(0.2, -0.9, -0.4)) * 6.3 + t * 1.3 + 2.8)    // smaller lumps
         + 0.005 * sin(dot(n, float3(-0.9, 0.1, -0.4)) * 7.7 - t * 1.1 + 0.9)
         + 0.010 * sin(1.1 * t);                       // breathing
}
float ripples(float3 n, float t) {
    float s = 0;
    [unroll] for (int i = 0; i < 7; ++i) {
        const float T = 3.0 + i * 0.7;                 // a new impact every T s
        const float ph = t / T + i * 0.37;
        const float k = floor(ph), age = frac(ph) * T;
        float3 c = normalize(hash3(k * 7.13 + i * 13.37) * 2 - 1);
        c.z = -abs(c.z) * 0.6 + c.z * 0.4;             // mostly on the side the viewer sees
        c = normalize(c);
        const float d = acos(clamp(dot(n, c), -1, 1)); // distance over the sphere (radians)
        const float x = d - 0.5 * age;                 // behind the front of the packet
        const float env = exp(-x * x / 0.14) * exp(-age * 0.38) / (1 + 1.6 * d);
        s += env * sin(x * (26.0 + 4.0 * i));
    }
    return s * rippleAmp;
}
float surfacePacket(float3 normal, float3 origin, float front, float width) {
    // Chord distance grows monotonically with angular distance on the sphere.
    // Use a smooth trough and crest so the surface normal bends continuously.
    const float distance = sqrt(saturate(0.5 - 0.5 * dot(normal, origin))) * 2.0;
    const float x = (distance - front) / width;
    return exp(-x * x) * sin(2.2 * x);
}

float successFront() {
    const float t = saturate(successAge / 0.98);
    return 1.57 * (1.0 - pow(abs(1.0 - t), 1.20));
}

float surfaceDisturbance(float3 n) {
    float height = 0.0;
    if (successAge > 0 && successAge < 0.98 && reducedMotion < 0.5) {
        const float t = successAge / 0.98;
        const float envelope = smoothstep(0.0, 0.09, t) * (1.0 - smoothstep(0.68, 1.0, t));
        height += 0.035 * envelope * surfacePacket(n, float3(0, 0, -1), successFront(), 0.18);
    }
    [unroll] for (int k = 0; k < 3; ++k) {
        if (mouseImpacts[k].w > 0.0) {
            height += mouseImpacts[k].w * surfacePacket(n, mouseImpacts[k].xyz, mouseFronts[k], 0.15);
        }
    }
    // Keep mouse interaction below the camera's fixed sampling geometry.
    if (codeMode > 3.5 && successAge <= 0.0) height *= 0.28 * (1.0 - smoothstep(0.57, 0.93, length(n.xy)));
    return height;
}

// While a code is shown, calm the broad motion to keep the rim steady.
static float peerClockOffset = 0;
float H(float3 n) {
    const bool calm = qrSize != 0 || codeMode > 3.5 || (codeMode > 0.5 && codeMode < 1.5);
    const float wobble = calm ? lerp(0.25, 1.0, smoothstep(0.0, 0.74, successAge)) : 1.0;
    return lowModes(n, time + peerClockOffset) * wobble
        + ripples(n, time + peerClockOffset) + surfaceDisturbance(n);
}
float sdf(float3 p) { const float r = length(p); return (r - 1 - H(p / max(r, 1e-4))) * 0.72; }
float3 normalAt(float3 p) {
    const float e = 0.0012; const float2 k = float2(1, -1);
    return normalize(k.xyy * sdf(p + k.xyy * e) + k.yyx * sdf(p + k.yyx * e) + k.yxy * sdf(p + k.yxy * e) + k.xxx * sdf(p + k.xxx * e));
}

// ---- the world round it ---------------------------------------------------------------------------------------------
float3 studio(float2 px) {
    const float v = px.y / targetSize.y, u = px.x / targetSize.x - 0.5;
    float g = lerp(0.90, 0.62, smoothstep(0.0, 0.6, v));
    g = lerp(g, 0.04, smoothstep(0.45, 0.95, v));
    g *= 1 - 0.25 * u * u;
    return g * float3(0.95, 0.97, 1.0);
}
// A darker studio: mid grey above to dark grey below (never black: dark beams need room below the glass).
float3 studioDark(float2 px) {
    const float v = px.y / targetSize.y, u = px.x / targetSize.x - 0.5;
    float g = lerp(0.46, 0.30, smoothstep(0.0, 0.6, v));
    g = lerp(g, 0.10, smoothstep(0.45, 0.95, v));
    g *= 1 - 0.25 * u * u;
    return g * float3(0.95, 0.97, 1.0);
}
float3 bgAt(float2 px, float lod) {
    if (mode > 2.5) return studioDark(px);
    if (mode > 1.5) return studio(px);
    if (mode > 0.5) return bgTex.SampleLevel(samp, frac(px / targetSize * imgScale + imgOffset), lod).rgb;
    return bgTex.SampleLevel(samp, (deskOrigin + px) / deskSize, lod).rgb;
}
static const float3 KEY = normalize(float3(-0.55, -0.75, -0.55));   // towards the key light (upper left, in front)
static const float3 RIM = normalize(float3(0.65, 0.62, -0.25));     // towards a grazing rim light (lower right)
float3 env(float3 r) {
    // soft boxes (studio) + the background, blurred, as the room seen in the reflection
    float3 c = bgAt(center + r.xy * radius * 2.4, 5.5) * 0.75;
    c *= lerp(1.0, 0.45, saturate(r.y * 1.2));            // darker below
    // the key light as a soft-edged rectangular soft box (its reflection shows every ripple), a strip light above
    const float3 ku = normalize(cross(KEY, float3(0, 0, 1))), kv = cross(KEY, ku);
    const float dk = dot(r, KEY);
    if (dk > 0) {
        const float2 a = float2(dot(r, ku), dot(r, kv)) / dk;
        const float box = smoothstep(0.42, 0.30, abs(a.x)) * smoothstep(0.30, 0.20, abs(a.y));
        c += box * 2.2 + pow(abs(dk), 30) * 0.6;
    }
    c += smoothstep(-0.80, -0.95, r.y) * smoothstep(0.5, 0.1, abs(r.x)) * 1.3;          // strip light above
    c += pow(saturate(dot(r, normalize(float3(0.7, -0.6, -0.4)))), 40) * 0.5;  // a second, weaker box upper right
    return c;
}
float3 thinFilm(float cosT, float thick) {
    const float opd = 2 * 1.33 * thick * cosT;
    return 0.5 + 0.5 * cos(2 * PI * opd / float3(650, 540, 450));
}

// ---- inside ------------------------------------------------------------------------------------------------------------
// From a point just inside the front surface along d to the back surface: path length, exit point and outward normal.
float traceInside(float3 p, float3 d, out float3 q, out float3 nq) {
    // Sphere tracing is hopeless here (near the entry point the surface is always close: tiny steps). The chord of the
    // undisturbed sphere gives the exit to within the displacement; bisection on the true surface finishes it.
    const float b = dot(p, d), c = dot(p, p) - 1;
    const float ts = -b + sqrt(max(b * b - c, 0));
    float lo = max(ts - 0.12, 0.01), hi = ts + 0.12;
    if (sdf(p + d * lo) > 0) lo = max(ts * 0.5, 0.01);   // a deep dent: start further back
    [unroll] for (int i = 0; i < 14; ++i) {
        const float mid = 0.5 * (lo + hi);
        if (sdf(p + d * mid) < 0) lo = mid; else hi = mid;
    }
    const float t = 0.5 * (lo + hi);
    q = p + d * t;
    nq = normalAt(q);
    return t;
}
// The key light's caustics on the far inside: the light that entered on the lit side, focused where the surface
// curves (ripples), plus the bright crescent a ball lens throws opposite the light.
float caustic(float3 q) {
    const float b = dot(q, KEY), c = dot(q, q) - 1;
    const float s = -b + sqrt(max(b * b - c, 0));
    const float3 pl = normalize(q + KEY * s);                 // where that light came in
    const float3 t1 = normalize(cross(pl, abs(pl.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0))), t2 = cross(pl, t1);
    const float e = 0.025;
    const float lap = (H(normalize(pl + t1 * e)) + H(normalize(pl - t1 * e)) + H(normalize(pl + t2 * e)) + H(normalize(pl - t2 * e)) - 4 * H(pl)) / (e * e);
    const float far = saturate(dot(normalize(q), -KEY));
    const float crescent = exp(-pow((far - 0.9) / 0.07, 2));
    return (saturate(-lap * 0.09) * 1.6 + crescent * 0.7) * smoothstep(0.35, 0.95, far);
}
float3 bubblePos(int i, out float r, out float fade) {
    const float3 h = hash3(i * 17.31 + 1.0);
    r = 0.010 + 0.034 * h.x * h.x * h.x;
    const float age = frac(h.z + time * (0.027 + 0.043 * h.x));
    fade = smoothstep(0.0, 0.09, age) * (1.0 - smoothstep(0.91, 1.0, age));
    const float y = 0.80 - age * 1.60;
    const float s = sqrt(max(0, 1 - y * y)) * 0.86;
    const float drift = 0.045 * sin(time * (0.65 + h.y) + i);
    return float3(((h.y - 0.5) * 1.54 + drift) * s, y, (h.x - 0.5) * 1.38 * s);
}

float4 ShadeBall(float2 px, float2 ballCenter, float ballRadius, bool drawBackdrop,
    out float3 nOut, out float frontDistance) {
    nOut = 0;
    frontDistance = 2.0;
    const float2 uv = (px - ballCenter) / ballRadius;
    const float3 back = bgAt(px, 0);
    const float4 outside = drawBackdrop ? float4(back, 1) : float4(0, 0, 0, 0);
    if (dot(uv, uv) > 1.25 * 1.25) return outside;

    const float3 ro = float3(0, 0, -E), rd = normalize(float3(uv, E));
    // bounding sphere (radius 1.08), then sphere tracing to the surface
    const float bb = dot(ro, rd), cc = dot(ro, ro) - 1.08 * 1.08, disc = bb * bb - cc;
    if (disc < 0) return outside;
    float t = -bb - sqrt(disc); const float tEnd = -bb + sqrt(disc);
    float minD = 1e9; bool hit = false; float3 p = ro;
    [loop] for (int k = 0; k < 48; ++k) {
        p = ro + rd * t;
        const float d = sdf(p);
        minD = min(minD, d);
        if (d < 0.0004) { hit = true; break; }
        t += d;
        if (t > tEnd) break;
    }
    const float px1 = 1.0 / ballRadius;                        // one pixel in ball units
    // Grazing rays near the silhouette creep along the surface and can use up the steps a few hundredths of a pixel
    // from it. Calling those misses swapped neighbouring pixels between refraction and the rim reflection, which showed
    // as broken, segmented glow along the edge while the surface moved. Within a third of a pixel it is a hit.
    if (!hit && t <= tEnd && sdf(p) < px1 * 0.35) hit = true;
    if (!hit) {
        // anti-aliased silhouette: a thin reflective rim fading out over about a pixel
        const float a = saturate(0.6 - (minD / 0.72) / px1);
        if (a <= 0) return outside;
        const float3 n = normalize(p);
        const float3 rc = env(reflect(rd, n)) * 0.8;
        return drawBackdrop ? float4(lerp(back, rc, a), 1) : float4(rc * a, a);
    }
    const float3 n = normalAt(p);
    nOut = n;
    frontDistance = sqrt(saturate(0.5 + 0.5 * normalize(p).z)) * 2.0;
    const float cosI = saturate(dot(-rd, n));

    // refraction, once per colour (dispersion), each with its own path through the ball
    const float3 ior = float3(1.372, 1.384, 1.401);
    const float3 absorb = float3(0.20, 0.075, 0.02);           // per ball radius of path: a cool, clear blue
    float3 trans = 0; float3 qG = p; float tG = 0; float3 dG = rd;
    [loop] for (int ch = 0; ch < 3; ++ch) {  // a loop: unrolled, the three traces and blends are too much code
        const float3 d = refract(rd, n, 1.0 / ior[ch]);
        float3 q, nq;
        const float len = traceInside(p, d, q, nq);
        // The three ways out (reflected back inside, through to the background, out to the room) blend over a short
        // range instead of switching: each colour reaches them at a slightly different angle (dispersion), and hard
        // switches left red, green and blue segments along the rim.
        const float cosOut = dot(d, nq);
        const float k = 1 - ior[ch] * ior[ch] * (1 - cosOut * cosOut);  // < 0: total internal reflection
        const float wOut = smoothstep(0.0, 0.10, k);               // 0: all reflected back inside
        float v = 0;
        [branch] if (wOut < 1) {
            const float3 r2 = reflect(d, -nq);
            v = env(float3(r2.x, r2.y, -abs(r2.z)))[ch] * 0.55 * (1 - wOut);
        }
        [branch] if (wOut > 0) {
            const float3 dOut = refract(d, -nq, ior[ch]);
            const float wBack = smoothstep(0.0, 0.06, dOut.z);         // 1: through to the background plane
            float through = 0;
            [branch] if (wBack < 1) through = env(dOut)[ch] * 0.8 * (1 - wBack);
            [branch] if (wBack > 0) {
                const float s = (BZ - q.z) / dOut.z;
                const float2 b = (q.xy + dOut.xy * s) * E / (E + BZ);
                const float lod = saturate(1 - cosI) * 2.0;      // a little softer where the rays fan out
                through += bgAt(ballCenter + b * ballRadius, lod)[ch] * wBack;
            }
            v += through * wOut;
        }
        trans += float3(ch == 0, ch == 1, ch == 2) * (v * exp(-absorb[ch] * len));  // no indexed writes in a [loop]
        if (ch == 1) { qG = q; tG = len; dG = d; }
    }
    // caustics on the far inside (seen through the ball), warm-white light
    trans += min(caustic(qG), 2.0) * float3(1.0, 0.98, 0.92) * 0.8 * exp(-absorb * tG);

    // air bubbles on the way through
    if (showBubbles > 0.5) {
        float best = tG; float3 bc = 0; float br = 0; float fade = 0;
        [loop] for (int b = 0; b < 20; ++b) {
            float r, life;
            const float3 c = bubblePos(b, r, life);
            const float3 oc = p - c; const float B = dot(oc, dG), C = dot(oc, oc) - r * r, D = B * B - C;
            if (D > 0 && life > 0) {
                const float tb = -B - sqrt(D);
                if (tb > 0 && tb < best) { best = tb; bc = c; br = r; fade = life; }
            }
        }
        if (fade > 0) {
            const float3 hp = p + dG * best, hn = normalize(hp - bc);
            const float facing = saturate(dot(-dG, hn));
            const float3 airRay = refract(dG, hn, ior.g);
            const float reflection = 0.026 + 0.974 * pow(1.0 - facing, 5.0);
            const float3 reflectedRay = reflect(dG, hn);
            float3 transmitted = trans;
            float reflectWeight = reflection;
            if (dot(airRay, airRay) > 1e-5) {
                const float3 exitNormal = hn - 2.0 * dot(hn, airRay) * airRay;
                const float3 outRay = refract(airRay, -exitNormal, 1.0 / ior.g);
                // A short, size-scaled shift of the already rendered background approximates
                // the remaining path through the host sphere after leaving the air pocket.
                const float2 shift = (outRay.xy - dG.xy) * (br * ballRadius * 5.0);
                const float2 backdrop = ballCenter + qG.xy * (ballRadius * E / (E + BZ));
                transmitted = max(0.0, trans + (bgAt(backdrop + shift, 0.8) - bgAt(backdrop, 0.8)) * 0.8);
                transmitted += 0.018 * facing;
            } else {
                reflectWeight = 0.72;
            }
            float3 reflected = env(reflectedRay) * 0.44;
            reflected += 0.55 * pow(saturate(dot(reflectedRay, KEY)), 28.0);
            reflected += 0.22 * pow(saturate(dot(reflectedRay, RIM)), 12.0);
            // Keep air-pocket contrast softer where the camera reads the ray code.
            const float visibility = codeMode > 3.5 ? lerp(1.0, 0.55, smoothstep(0.29, 0.43, length(uv))) : 1.0;
            trans = lerp(trans, lerp(transmitted, reflected, reflectWeight), fade * visibility);
        }
    }

    // reflection: Fresnel, environment, thin film on parts of the surface, the rim light through the film
    const float F = 0.025 + 0.975 * pow(1 - cosI, 5);
    const float3 r = reflect(rd, n);
    // Transfer progress is the ball's own soap film spreading round the rim clockwise from twelve o'clock. Angle and
    // rim band come from the hit point and its normal, so the band follows the wobble and ripples of the surface.
    float spread = 0, meniscus = 0, flow = 0, active = 0;
    if (rimProgress > 0 && rimOpacity > 0 && qrSize == 0 && codeMode < 0.5) {
        const float turn = frac(atan2(p.x, -p.y) / (2 * PI) + 1);
        const float rim = smoothstep(0.42, 0.90, 1 - cosI);
        active = rimOpacity * smoothstep(0.0, 0.06, rimProgress);   // ease the ambient rim out as a transfer starts
        // Over the last few percent the film closes the ring, so 99% does not leave a notch at twelve o'clock.
        const float closing = smoothstep(0.955, 0.995, rimProgress);
        const float tail = rimProgress >= 1 ? 1 : lerp(smoothstep(0.0, 0.028, turn), 1, closing);
        const float head = rimProgress >= 1 ? 1 : lerp(1 - smoothstep(rimProgress - 0.030, rimProgress + 0.004, turn), 1, closing);
        const float gap = (turn - rimProgress) / 0.016;
        spread = rim * tail * head * rimOpacity;
        meniscus = rimProgress >= 1 ? 0 : rim * exp(-gap * gap) * rimOpacity * (1 - closing);
        flow = reducedMotion > 0.5 ? 0 : 140 * sin(turn * 2 * PI * 5 - time * 1.6);   // film streaming to the edge
    }
    const float thick = 420 + 260 * sin(dot(n, float3(1.7, 2.3, -1.1)) * 1.6 + time * 0.25) + 25000 * ripples(n, time)
        + flow * spread;
    const float cosT = sqrt(saturate(1 - (1 - cosI * cosI) / (1.33 * 1.33)));
    const float3 film = thinFilm(cosT, thick);
    const float patch = smoothstep(0.62, 0.92, 0.5 + 0.5 * sin(dot(n, float3(0.6, 0.7, 0.2)) * 3.1 + time * 0.17));
    float3 refl = env(r) * lerp(1, film * 1.9, max(patch * 0.8, spread * 0.9));
    // the grazing rim light, coloured by the film; during a transfer it gathers where the film has spread
    refl += pow(saturate(dot(r, RIM)), 9) * 3.2 * film * film * lerp(1, 0.15 + 0.85 * spread, active);
    float3 col = trans * (1 - F) + refl * F;
    // The film where it has spread catches light of its own (readable over a dark desktop too); its leading edge is a
    // brighter meniscus.
    const float3 sheen = lerp(film, 1, 0.30);
    col += spread * (sheen * sheen * 0.50 + 0.10) + meniscus * (0.30 + 0.28 * sheen);
    col += pow(saturate(dot(r, KEY)), 1500) * 4.0;             // sharp sun glint
    col = col / (1 + max(col - 0.9, 0) * 0.6);                  // soft shoulder for highlights
    return float4(saturate(col), 1);
}

// ---- the code (Bubble Code v1 layout, bubble-code.js) ---------------------------------------------------------------
// Pattern in the ball's projected disc (uv, radius 1): +1 bright, -1 dark; region 0 where the code leaves the picture
// alone (the core, outside the quiet zone).
static const float RR[4] = {0.785, 0.665, 0.545, 0.425};
static const int RN[4] = {54, 46, 38, 30}, RB[4] = {0, 54, 100, 138};
uint codeBit(int idx) { return (codeBits[idx >> 7][(idx >> 5) & 3] >> (idx & 31)) & 1; }
float codePattern(float2 uv, out float region) {
    const float r = length(uv);
    region = 1;
    if (r >= 1.18) { region = 0; return 0; }
    if (r >= 1.00) return -1;            // quiet zone
    if (r >= 0.93) return 1;             // rim
    if (r >= 0.85) return -1;            // guard
    if (r < 0.29) { region = 0; return 0; }  // core: the art's
    if (r < 0.33) return 1;              // inner ring
    float a = atan2(uv.y, uv.x); if (a < 0) a += 2 * PI;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (abs(r - RR[k]) > 0.05 * 0.86) continue;
        const float f = a / (2 * PI) * RN[k], fr = frac(f);
        const uint j = (uint)floor(f) % (uint)RN[k];
        if (codeBit(RB[k] + (int)j) && fr > 0.07 && fr < 0.93) return 1;
    }
    return -1;
}
// 2 x 2 samples: soft edges
float codeSoft(float2 uv, out float region) {
    const float q = 0.25 / radius;
    float r0, r1, r2, r3;
    const float v = codePattern(uv + float2(-q, -q), r0) + codePattern(uv + float2(q, -q), r1) + codePattern(uv + float2(-q, q), r2) + codePattern(uv + float2(q, q), r3);
    region = 0.25 * (r0 + r1 + r2 + r3);
    return 0.25 * v;
}

// A's light: 0..1. Rings of light: rim (0.93..1.0), inner ring (0.29..0.33), and the data rings, where runs of lit
// cells join into arcs, soft across (Gaussian) and at their ends.
float arcLight(float2 uv) {
    const float r = length(uv);
    const float pw = 1.5 / radius;                                   // edge softness: ~1.5 px
    float v = smoothstep(0.93 - pw, 0.93 + pw, r) * smoothstep(1.0 + pw, 1.0 - pw, r);
    v = max(v, smoothstep(0.29 - pw, 0.29 + pw, r) * smoothstep(0.33 + pw, 0.33 - pw, r));
    float a = atan2(uv.y, uv.x); if (a < 0) a += 2 * PI;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float dr = (r - RR[k]) / 0.030;
        if (abs(dr) > 2.2) continue;
        const float f = a / (2 * PI) * RN[k];
        const uint j = (uint)floor(f) % (uint)RN[k], jp = (j + 1) % (uint)RN[k], jm = (j + (uint)RN[k] - 1) % (uint)RN[k];
        const float fr = frac(f);
        const float me = codeBit(RB[k] + (int)j), nx = codeBit(RB[k] + (int)jp), pv = codeBit(RB[k] + (int)jm);
        // along the ring: lit cells join their lit neighbours; an end is rounded off inside its own cell
        float on = me;
        if (me > 0.5 && nx < 0.5) on *= smoothstep(0.92, 0.70, fr);
        if (me > 0.5 && pv < 0.5) on *= smoothstep(0.08, 0.30, fr);
        v = max(v, on * exp(-dr * dr));
    }
    return v;
}

float checkStroke(float2 p, float2 start, float2 end, float progress) {
    const float2 segment = (end - start) * max(progress, 0.0001);
    const float x = saturate(dot(p - start, segment) / max(dot(segment, segment), 1e-8));
    return (1.0 - smoothstep(0.026, 0.026 + 1.3 / radius, length(p - start - x * segment))) * step(0.001, progress);
}

float4 panelEdge(float4 color, float2 px) {
    if (phonePanel < 0.5) return color;
    const float corner = 38.0 * dpi;
    const float2 q = abs(px - targetSize * 0.5) - (targetSize * 0.5 - corner);
    const float2 u = max(q / corner, 0.0);
    const float2 u2 = u*u;
    const float level = sqrt(sqrt(dot(u2,u2)));
    const float gradient = length(u2*u) / max(level*level*level, 1e-5);
    const float distance = (level - 1.0) * corner / max(gradient, 0.1);
    const float coverage = saturate(0.5 - distance);
    const float lip = saturate(1.0 + distance / 1.6);
    color.rgb = lerp(color.rgb, float3(0.77, 0.82, 0.88), lip * 0.28);
    return color * coverage;
}

float4 FinishTransfer(float4 color, float2 uv) {
    const float rho = length(uv);
    if (finishAge < 0 || rho >= 1.10) return color;
    const float progress = reducedMotion > 0.5 ? 1.0 : smoothstep(0.12, 0.62, finishAge);
    if (reducedMotion < 0.5 && finishAge < 0.55) {
        float ring = (rho - lerp(0.62,0.13,smoothstep(0,0.55,finishAge))) / 0.037;
        color.rgb += 0.18 * exp(-ring*ring) * sin(PI*saturate(finishAge/0.55));
    }
    const float2 a = float2(-0.20, -0.01), b = float2(-0.06, 0.13), d = float2(0.24, -0.17);
    const float check = max(checkStroke(uv, a, b, saturate(progress / 0.32)),
                            checkStroke(uv, b, d, saturate((progress - 0.32) / 0.68)));
    color.rgb = lerp(color.rgb, float3(0.90, 0.97, 1.0), check);
    return color;
}

float4 PSPcPairing(VSOut input) : SV_Target {
    const float2 px = input.pos.xy;
    float3 result = bgAt(px, 0);
    float3 normal;
    float distance;
    const float2 uv = (px - center) / radius;
    const float mainDistance = length(uv);
    float4 selected = float4(center, radius, 1);
    int selectedIndex = 0;
    [unroll] for (int i = 0; i < 5; ++i) {
        const float4 peer = pcPeers[i];
        if (mainDistance >= 1.0 && peer.w > 0 && length(px - peer.xy) < peer.z * 1.16) {
            selected = peer;
            selectedIndex = i + 1;
        }
    }
    if (selectedIndex > 0)
        peerClockOffset = selectedIndex*13.17 + time*(0.16*sin(selectedIndex*7.91));
    [branch] if (length(px - selected.xy) <= selected.z * 1.16) {
        float4 ball = ShadeBall(px, selected.xy, selected.z, false, normal, distance);
        if (selectedIndex == 0) ball = FinishTransfer(ball, uv);
        // Absorb the overlapping part into the main surface.
        const float contact = selectedIndex == 0 ? 1.0 : selected.w * smoothstep(0.86, 1.01, mainDistance);
        result = result * (1.0 - ball.a * contact) + ball.rgb * contact;
    }
    return DitherBgra8(panelEdge(float4(result, 1), px), px);
}


float3 glassCode(float3 glass, float2 px) {
    // Flat sampling keeps phone cameras compatible. The four-module quiet
    // zone stays clear while all the surrounding reflection remains on the ball.
    const float modulePx = max(1.0, floor(radius * 1.24 / (qrSize + 8)));
    const float side = qrSize * modulePx;
    const float2 origin = floor(center - side * 0.5);
    const float2 codePixel = px - origin;
    const float2 local = codePixel - side * 0.5;
    const float quietRadius = (side * 0.5 + modulePx * 4.05) * 1.414214;
    const float spread = min(radius * 0.99, quietRadius + modulePx * 6.0);
    const float d = length(local);
    if (d >= spread) return glass;
    const float coverage = 1.0 - smoothstep(quietRadius, spread, d);
    const float3 frost = float3(0.80, 0.89, 0.92);
    float3 face = lerp(glass, frost, 0.83);
    if (all(codePixel >= 0.0) && all(codePixel < side)) {
        const uint2 cell = (uint2)floor(codePixel / modulePx);
        const uint bit = cell.y * qrSize + cell.x;
        const uint word = qrBits[bit >> 7][(bit >> 5) & 3];
        if (((word >> (bit & 31)) & 1u) != 0u) {
            face = lerp(float3(0.045, 0.085, 0.117), glass * 0.43, 0.20);
        }
    }
    return lerp(glass, face, coverage);
}

float4 finishPixel(float4 color, float2 px) {
    const float2 uv = (px - center) / radius;
    // Keep the finder and code clear during a scan. The spray becomes visible beyond the rim.
    if (splashCount > 0 && (codeMode < 3.5 || successAge > 0 || dot(uv, uv) > 1.04 * 1.04)) {
        [loop] for (int i = 0; i < (int)splashCount; ++i) {
            const float4 drop = waterDrops[i];
            const float2 d = (uv - drop.xy) / drop.z;
            const float r2 = dot(d, d);
            const float edgeAA = 1.0 / (drop.z * radius);
            if (r2 > (1.0 + edgeAA) * (1.0 + edgeAA)) continue;
            const float r = sqrt(r2);
            const float z = sqrt(saturate(1.0 - r2));
            const float3 n = float3(d, -z);
            const float3 refraction = bgAt(px - d * drop.z * radius * 5.2, 0);
            const float glint = pow(saturate(dot(n, KEY)), 24.0);
            const float rim = pow(1.0 - z, 3.0);
            const float3 water = lerp(refraction * 0.83 + 0.055, float3(0.67, 0.79, 0.86), rim * 0.44) + glint * 0.56;
            const float coverage = 1.0 - smoothstep(1.0 - edgeAA, 1.0 + edgeAA, r);
            color.rgb = lerp(color.rgb, saturate(water), coverage * drop.w);
        }
    }
    if (qrSize != 0) color.rgb = glassCode(color.rgb, px);
    return panelEdge(color, px);
}

float4 PSBubble(VSOut i) : SV_Target {
    const float2 px = i.pos.xy;
    const float2 uv = (px - center) / radius;
    float3 n;
    float frontDistance;
    const float rho = length(uv);
    float4 c = ShadeBall(px, center, radius, opaque > 0.5, n, frontDistance);
    // The camera samples the first radial band at 0.38 radii.
    if (qrSize == 0 && fileCount > 0 && sinkAge >= 0 && sinkAge < 1.45 &&
        reducedMotion < 0.5 && rho < 0.32 && successAge <= 0) {
        [unroll] for (int j = 0; j < 4; ++j) {
            if (j >= fileCount) break;
            float t = saturate((sinkAge - j * 0.07) / 1.2);
            float travel = smoothstep(0.0, 1.0, t);
            float angle = 2.4 * j - 1.9;
            float2 origin = float2(cos(angle), sin(angle)) * 0.22;
            float2 pos = lerp(origin, float2(0, 0.055), travel);
            float scale = lerp(0.056, 0.018, travel);
            float2 local = (uv - pos) / scale;
            float2 q = abs(local) - float2(0.52, 0.73);
            float edge = length(max(q, 0.0)) + min(max(q.x,q.y),0.0) - 0.12;
            edge = max(edge, (local.x-local.y-1.11)*0.707);
            float aa = max(1.0 / (radius * scale), 0.04);
            float outline = 1.0-smoothstep(0.02, 0.02+aa, abs(edge));
            float opacity = smoothstep(0.0,0.12,t)*(1.0-smoothstep(0.50,1.0,t));
            c.rgb = lerp(c.rgb,float3(0.72,0.88,0.95),outline*opacity*0.70);
        }
    }
    c = FinishTransfer(c, uv);
    if (codeMode > 3.5) {
        // Pass the remaining code opacity to PSRays. The crest uses the same
        // distance and front as the actual displacement on the sphere.
        if (successAge > 0.0) {
            c.a = reducedMotion > 0.5 ? 1.0 - smoothstep(0.0, 0.18, successAge)
                : smoothstep(-0.045, 0.135, frontDistance - successFront());
        }
        return c;
    }
    if (codeMode < 0.5) {
        const float4 result = finishPixel(c, px);
        return qrSize == 0 ? DitherBgra8(result, px) : result;
    }
    if (dot(uv, uv) > 1.2 * 1.2) return panelEdge(c, px);
    if (c.a < 1) c = float4(lerp(bgAt(px, 0), c.a > 0 ? c.rgb / c.a : 0, c.a), 1);   // the code needs the quiet zone: opaque
    float region;
    if (codeMode < 1.5) {
        // A: the code as light held in the glass. Lit cells run together into soft arcs of light (rounded ends, no
        // blocks) seen through the rippling surface, with a little dispersion; the rim is the ball's own bright edge,
        // the inner ring a caustic-like ring of light round the core. Unlit parts are the glass, a little dimmed; the
        // quiet zone is a soft shadow round the ball.
        const float2 cuv = uv + n.xy * 0.010;
        const float r = length(uv);
        if (r >= 1.2) return panelEdge(c, px);
        if (r < 0.29) return panelEdge(c, px);                                  // the core: the art's (up to the inner ring)
        const float3 glow = float3(arcLight(cuv * 1.006), arcLight(cuv), arcLight(cuv * 0.994));
        const float a = atan2(uv.y, uv.x);
        const float shimmer = 0.5 + 0.5 * sin(r * 11 - time * 0.9 + sin(a * 3 + time * 0.4) * 0.8);
        const float3 light = lerp(float3(0.72, 0.90, 1.00), float3(1.00, 0.97, 0.92), shimmer) * 1.05;
        float3 base = c.rgb * (r < 1.0 ? 0.30 : lerp(0.22, 1.0, smoothstep(1.02, 1.2, r)));   // dimmed glass / shadow
        return panelEdge(float4(saturate(base * (1 - glow * 0.6) + light * glow), 1), px);
    }
    // B: invisible watermark - the picture's brightness nudged up or down by codeAmp, nothing else
    const float s = codeSoft(uv, region);
    return panelEdge(float4(saturate(c.rgb * (1 + codeAmp * s * region)), 1), px);
}

// ---- rays: the code as faint beams of light from the centre (second pass over the finished ball) ---------------------
// 28 directions x 6 segments along the radius (0.38 .. 0.86 of the ball) = 168 cells, in the order the v1 encoder gives
// (segment 0 is the outermost and starts with the sync word). Each cell is a slightly brighter (1) or darker (0) beam,
// joined with its neighbours along the radius, fading at both ends. The scanner compares a beam with the glass halfway
// to the next one. Use the existing glass contrast whenever it already carries the correct sign and strength.
Texture2D ballTex : register(t1);
static const int RAYS = 28;
static const float SEG[7] = {0.86, 0.78, 0.70, 0.62, 0.54, 0.46, 0.38};
float lumAt(float2 px) { const float3 c = ballTex.Load(int3(px, 0)).rgb; return max(c.r, max(c.g, c.b)); }
float lumBil(float2 px) {
    const float2 p = px - 0.5, f = frac(p), q = floor(p) + 0.5;
    return lerp(lerp(lumAt(q), lumAt(q + float2(1, 0)), f.x), lerp(lumAt(q + float2(0, 1)), lumAt(q + float2(1, 1)), f.x), f.y);
}
float rayCorrection(int j, float r, float sig) {
    j = (int)((uint)(j + RAYS) % (uint)RAYS);
    // the cell's sign along the radius, blended over segment borders, faded at the ends
    float sgn = 0, wsum = 0;
    [unroll] for (int k = 0; k < 6; ++k) {
        const float lo = SEG[k + 1], hi = SEG[k];
        const float w = smoothstep(lo - 0.02, lo + 0.02, r) * smoothstep(hi + 0.02, hi - 0.02, r);
        sgn += w * (codeBit(k * RAYS + (int)j) ? 1.0 : -1.0); wsum += w;
    }
    // the glass alone: beam centre against halfway to the neighbours, at this radius
    // (weighted across the beam's width, as a camera's blur will see it)
    const float ac = (j + 0.5) * 2 * PI / RAYS, ah = 0.5 * 2 * PI / RAYS, aw = sig * 2 * PI / RAYS;
    float beamL = 0, beamW = 0, sideL = 0;
    [unroll] for (int q = -2; q <= 2; ++q) {
        const float w = exp(-0.36 * q * q);
        const float aq = ac + q * 0.6 * aw;
        beamL += w * lumBil(center + float2(cos(aq), sin(aq)) * r * radius); beamW += w;
        const float al = ac - ah + q * 0.6 * aw, ahh = ac + ah + q * 0.6 * aw;
        sideL += w * 0.5 * (lumBil(center + float2(cos(al), sin(al)) * r * radius) + lumBil(center + float2(cos(ahh), sin(ahh)) * r * radius));
    }
    const float art = clamp((beamL - sideL) / beamW, -0.5, 0.5);
    const float bitSign = wsum > 0 ? sgn / wsum : 0;
    // Blend the two one-sided corrections through a segment boundary. This keeps
    // the correction continuous even when neighbouring segments have opposite bits.
    const float brighten = max(0.0, codeAmp - art);
    const float darken = min(0.0, -codeAmp - art);
    return lerp(darken, brighten, 0.5 + 0.5 * bitSign);
}

// In beam spacings, the response is 0.70 at the centre and -0.30 at
// either side sample. At an adjacent beam centre it is -0.15, the mean
// of -0.30 and zero at that beam's side samples. Contributions therefore
// add without changing the ideal centre-minus-sides reading of neighbours.
float spreadProfile(float distance) {
    const float d = abs(distance);
    const float centre = 1.0 - smoothstep(0.0, 0.5, d);
    const float shoulder = 1.0 - smoothstep(0.5, 1.5, d);
    return centre - 0.30 * shoulder;
}

float4 PSRays(VSOut i) : SV_Target {
    const float2 px = i.pos.xy;
    const float4 ball = ballTex.Load(int3(px, 0));
    float3 col = ball.rgb;
    const float2 uv = (px - center) / radius;
    const float r = length(uv);
    // The ball's edge highlight, made even all round: a thin bright line just inside the outline that stands +0.22
    // above the glass and the background beside it (set against what is actually there, like the beams). The
    // scanner finds the ball by it; to the eye it is the ordinary bright rim of a glass ball.
    if (r > 0.94 && r < 1.06) {
        const float2 dir = uv / max(r, 1e-4);
        const float artRim = clamp(lumBil(center + dir * radius) - 0.5 * (lumBil(center + dir * 0.965 * radius) + lumBil(center + dir * 1.035 * radius)), -0.3, 0.3);
        const float rr = (r - 1.0) / 0.018;
        const float rim = 1.0 - (reducedMotion > 0.5 ? smoothstep(0.0, 0.18, successAge)
            : smoothstep(0.50, 0.94, successAge));
        col += (0.38 - artRim) * exp(-rr * rr) * float3(0.95, 0.98, 1.0) * rim;
        return finishPixel(float4(saturate(col), 1), px);
    }
    if (r < SEG[6] - 0.03 || r > SEG[0] + 0.03) return finishPixel(float4(col, 1), px);
    float a = atan2(uv.y, uv.x); if (a < 0) a += 2 * PI;
    // The outermost camera samples are 0.015 inside each 0.08-wide cell.
    // Confine the bend to the joins, leaving the sampled interior straight.
    const float inCell = frac((r - SEG[6]) / 0.08) * 0.08;
    const float edgeDistance = min(inCell, 0.08 - inCell);
    const float bendWidth = 0.010 + 0.003 * sin(3.0 * a + 13.0 * r);
    const float bendWeight = 1.0 - smoothstep(0.0, bendWidth, edgeDistance);
    const float bend = 0.19 * sin(5.0 * a + 29.0 * r) + 0.07 * sin(9.0 * a - 21.0 * r);
    const float f = a / (2 * PI) * RAYS + bendWeight * bend;
    const int j = (int)floor(f);
    const float u = frac(f) - 0.5;
    const float sig = clamp(0.04 / (r * 2 * PI / RAYS), 0.15, 0.35);
    const float ends = smoothstep(SEG[6] - 0.03, SEG[6] + 0.02, r) * smoothstep(SEG[0] + 0.03, SEG[0] - 0.02, r);
    const float correction = spreadProfile(u + 1.0) * rayCorrection(j - 1, r, sig)
                           + spreadProfile(u) * rayCorrection(j, r, sig)
                           + spreadProfile(u - 1.0) * rayCorrection(j + 1, r, sig);
    const float remaining = successAge > 0.0 ? ball.a : 1.0;
    col += correction * ends * float3(0.94, 0.98, 1.0) * remaining;
    return finishPixel(float4(saturate(col), 1), px);
}
