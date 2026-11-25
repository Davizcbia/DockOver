#pragma once
// GPU side of the Black Hole Visualizer: Schwarzschild look-up tables, the desktop image with a mip chain, the lens
// shader (shaders/blackhole.hlsl) and GPU timestamp measurement. Runs on the caller's D3D11 device and thread.
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <vector>

#include "blackhole_debris.h"
#include <string>

namespace docky::blackhole {

struct LensConstants {
    float regionOrigin[2];
    float holeCenter[2];
    float desktopSize[2];
    float renderScale;
    float pxPerM;
    float sinIncl, cosIncl;
    float lensDistance;
    float regionRadius;
    float diskIn, diskOut;
    float diskOpacity;
    float diskBrightness;
    float time;
    float doppler;
    float heatTint;
    float ringFlash;
    float opacity;
    float taperStart;
    float bMax, lutDelta, phiMax;
    float showDisk;
    uint32_t spp;
    float flowPeriod;
    float spinSign;
    float overlayRadius;
    float payloadLodBias;
    uint32_t payloadCount;
    float payloadA[4][4];
    float payloadB[4][4];  // opacity, redshift g, slice, shear
    float debrisExtent;    // filled in by Render
    uint32_t debrisOn;     // filled in by Render
    float skyDrift[2];     // desktop px
    float debrisSin, debrisCos;  // tilt of the debris orbit plane (filled in by Render)
    float pad3[2];
};
static_assert(sizeof(LensConstants) % 16 == 0, "constant buffer size");

class LensRenderer {
public:
    bool Init(ID3D11Device* device);
    // The desktop image (any device-local or opened shared texture, B8G8R8A8). Copied into a mip-mapped texture when
    // `serial` changes (only then: a still desktop costs nothing).
    bool UpdateDesktop(ID3D11Texture2D* source, uint64_t serial);
    // A generated calibration grid instead of the desktop (--test-pattern).
    bool UseTestPattern(UINT width, UINT height);
    bool HasDesktop() const { return desktopSrv_ != nullptr; }
    UINT DesktopWidth() const { return deskW_; }
    UINT DesktopHeight() const { return deskH_; }
    // Payload icon for `slot` (0..3): kPayloadSize^2 premultiplied BGRA pixels.
    static constexpr UINT kPayloadSize = 256;
    bool SetPayloadIcon(int slot, const uint32_t* pixels);
    // Debris particles for the next Render: drawn into a lens-plane texture of texSize^2 covering [-extentM, extentM]^2
    // M; none (count 0) skips the pass.
    void SetDebris(const std::vector<DebrisGpu>& particles, float extentM, UINT texSize);
    // Allocates the debris target ahead of time (called every frame; a no-op when the size is unchanged), so the first
    // shatter never pays for a texture allocation.
    void PrepareDebris(UINT texSize) { EnsureDebrisTarget(ClampDebris(texSize)); }
    // Draws into `rtv` (width x height). Constants: table fields are filled in here.
    void Render(ID3D11RenderTargetView* rtv, UINT width, UINT height, LensConstants c);
    // Newest finished GPU time (ms) of the lens pass, or < 0 when none is available yet. Non-blocking.
    double PollGpuMs();
    // Copies a render target to a 32-bit BMP file (development: visual tests).
    bool SaveBmp(ID3D11Texture2D* target, const std::wstring& path);
    double LutBuildMs() const { return lutMs_; }

private:
    bool BuildLut();
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mirror_, clamp_, clampMip_;
    bool EnsureDebrisTarget(UINT size);
    static UINT ClampDebris(UINT s) { return s < 128 ? 128 : s > 2048 ? 2048 : s; }
    Microsoft::WRL::ComPtr<ID3D11VertexShader> debrisVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> debrisPs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> debrisBuf_, debrisCb_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> debrisBufSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> debrisTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debrisRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> debrisSrv_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> premulBlend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> noCull_;
    UINT debrisSize_ = 0, debrisCount_ = 0;
    float debrisExtent_ = 1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> payload_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> payloadSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> orbitSrv_, endSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> desktopSrv_;
    UINT deskW_ = 0, deskH_ = 0;
    uint64_t deskSerial_ = 0;
    float bMax_ = 400, delta_ = 1e-4f, phiMax_ = 0;
    double lutMs_ = 0;
    struct Timing { Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end; bool pending = false; };
    Timing timing_[6];
    unsigned timingNext_ = 0;
};

}  // namespace docky::blackhole
