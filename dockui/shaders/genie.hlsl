// Genie minimize: the captured window drawn as a continuous mesh (genie_gpu.cpp computes the vertices).
// The texture is premultiplied BGRA with a full mip chain, so the window stays clean while it shrinks into
// a Dock tile. a = edge coverage (the mesh has a one-pixel transparent fringe on each side for anti-aliasing).
Texture2D Window : register(t0);
SamplerState Linear : register(s0);

struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float a : TEXCOORD1; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float a : TEXCOORD1; };

PSIn VSMain(VSIn i) {
    PSIn o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv = i.uv;
    o.a = i.a;
    return o;
}

float4 PSMain(PSIn i) : SV_Target {
    return Window.Sample(Linear, i.uv) * saturate(i.a);
}
