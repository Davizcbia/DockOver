Texture2D<float4> oldRoi : register(t0);
Texture2D<float4> newRoi : register(t1);
RWByteAddressBuffer changed : register(u0);
[numthreads(16,16,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h; oldRoi.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    if (any(oldRoi.Load(int3(id.xy,0)) != newRoi.Load(int3(id.xy,0)))) {
        uint ignored; changed.InterlockedOr(0, 1, ignored);
    }
}
