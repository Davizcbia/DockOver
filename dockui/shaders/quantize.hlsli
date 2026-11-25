// A screen-space 8x8 ordered threshold. Keep it stable across frames so slow
// gradients do not sparkle. The mean offset is zero and the range is one LSB.
float QuantizeThreshold(float2 position) {
    uint2 p = uint2(position) & 7u;
    uint x = p.x ^ p.y;
    uint y = p.y;
    uint rank = ((x & 1u) << 5) | ((y & 1u) << 4) |
                ((x & 2u) << 2) | ((y & 2u) << 1) |
                ((x & 4u) >> 1) | ((y & 4u) >> 2);
    return (float(rank) + 0.5) / 64.0 - 0.5;
}

float4 DitherBgra8(float4 premultiplied, float2 position) {
    const float alpha = saturate(premultiplied.a);
    const float alphaCode = floor(alpha * 255.0 + 0.5);
    const float3 rgbCode = floor(clamp(premultiplied.rgb, 0.0, alpha) * 255.0 +
                                 QuantizeThreshold(position) + 0.5);
    return float4(clamp(rgbCode, 0.0, alphaCode), alphaCode) / 255.0;
}
