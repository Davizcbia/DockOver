// Liquid Dock - backdrop blur for the Liquid Glass material (P0.3), written for the Dock.
// Only the Dock's region of interest is processed: PSDown resamples it from the backdrop at
// 1/q resolution, PSBlur runs one direction of a separable Gaussian (two passes per update).
// The result is cached and recomputed only when the backdrop, ROI, radius or quality change.

cbuffer BlurConstants : register(b0)
{
    float4 roi;       // ROI screen origin (x, y), downsample factor q, unused
    float4 backdrop;  // backdrop screen origin (x, y), 1 / backdrop size (x, y)
    float4 passParams;      // texel step (x, y) in source UV, sigma (texels), tap radius (texels)
};

Texture2D Source : register(t0);
SamplerState LinearClamp : register(s0);

struct VSOut { float4 pos : SV_Position; };

float4 PSDown(VSOut input) : SV_Target
{
    // Average q x q backdrop pixels with bilinear taps at 2x2-pixel centres.
    float q = roi.z;
    float2 baseScreen = roi.xy + (input.pos.xy - 0.5) * q;
    float3 acc = 0;
    float count = 0;
    for (float y = 1.0; y < q; y += 2.0)
    {
        for (float x = 1.0; x < q; x += 2.0)
        {
            float2 uv = (baseScreen + float2(x, y)) * 1.0;
            uv = (uv - backdrop.xy) * backdrop.zw;
            acc += Source.SampleLevel(LinearClamp, uv, 0).rgb;
            count += 1.0;
        }
    }
    return float4(acc / max(count, 1.0), 1.0);
}

float4 PSBlur(VSOut input) : SV_Target
{
    float2 size;
    Source.GetDimensions(size.x, size.y);
    float2 uv = input.pos.xy / size;
    float sigma = max(passParams.z, 0.01);
    int radius = (int)min(passParams.w, 64.0);
    float3 acc = Source.SampleLevel(LinearClamp, uv, 0).rgb;
    float wsum = 1.0;
    // Pairs of taps sampled between texels (linear-filtering trick) halve the fetch count.
    for (int i = 1; i <= radius; i += 2)
    {
        float w1 = exp(-(i * i) / (2.0 * sigma * sigma));
        float w2 = exp(-((i + 1) * (i + 1)) / (2.0 * sigma * sigma));
        float w = w1 + w2;
        float o = (i * w1 + (i + 1) * w2) / max(w, 1e-6);
        float2 step = passParams.xy * o;
        acc += (Source.SampleLevel(LinearClamp, uv + step, 0).rgb + Source.SampleLevel(LinearClamp, uv - step, 0).rgb) * w;
        wsum += 2.0 * w;
    }
    return float4(acc / wsum, 1.0);
}
