Texture2D<float4> source : register(t0);
RWTexture2D<float4> normalized : register(u0);
cbuffer CaptureColor : register(b0) {
    float inverseWhite;
    float3 unused;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID) {
    uint width, height;
    normalized.GetDimensions(width, height);
    if (thread.x >= width || thread.y >= height) return;
    const float3 value = saturate(source.Load(int3(thread.xy, 0)).rgb * inverseWhite);
    const float3 srgb = value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    normalized[thread.xy] = float4(srgb, 1);
}
