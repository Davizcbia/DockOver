#include "quantize.hlsli"

Texture2D<float4> scene : register(t0);
cbuffer Output : register(b0) {
    float whiteScale;
    float3 unused;
};

float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    const float4 sample = scene.Load(int3(pos.xy, 0));
    if (whiteScale <= 0.0) return DitherBgra8(sample, pos.xy);

    // The existing brushes and media sources encode SDR sRGB. Decode straight
    // colour before premultiplying in linear light for the scRGB compositor.
    const float alpha = saturate(sample.a);
    if (alpha == 0.0) return 0;
    const float3 s = saturate(sample.rgb / alpha);
    const float3 linearColor = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    return float4(linearColor * (alpha * whiteScale), alpha);
}
