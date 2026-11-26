// docky-orb: standalone visual prototype of DockDrop pairing, a particle orb in the spirit of the one Apple shows for
// iPhone Quick Start, on a Liquid Glass card. The orb's code rings carry a one-time pairing secret (orb_code.h) that a
// phone's camera can read. This stage is visual only: no networking, nothing else in DockOver is started or changed.
//
//   Space        simulate "scanned": the code spirals into the orb, it collapses with a flash, a checkmark appears
//   R            reset: a new secret, the orb gathers again
//   Esc          quit (also the close button, top left)
// Command line:
//   --demo                         scan succeeds by itself 3.5 s after start (and again every 7 s)
//   --secret <hex>                 fixed secret (tests); default: a random one, renewed every 60 s
//   --static                       every code arc lit at once (default: dynamic, arcs flare one after another, the
//                                  scanner gathers them over ~1 s; orb_code.h)
//   --spin                         spinning ring: the whole code turns slowly and its arcs flare brighter in turn,
//                                  never going out (every frame is a complete code)
//   --sat                          satellites: 24 comets on three orbits carry the code by their positions
//                                  (orb_code.h EncodeSat); the constellation turns slowly
//   --radar                        radar: a beam sweeps round every 3 s, the code arcs and the stars light up as it
//                                  passes and fade (never below a floor: every frame is a complete code)
//   --no-desktop                   no desktop capture behind the glass (a dark gradient)
//   --snapshot-seq <dir> --snapshot-after <s> --snapshot-interval <s> --snapshot-count <n>   BMP frames of the card
//   --exit-after <s>               quit after this many seconds
//   --bench <s>                    frame statistics after this many seconds (printed to --bench-out <file>)
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../desktop/duplication.h"
#include "../vblank.h"
#include "orb_code.h"
#include "orb_ps_glass.h"
#include "orb_ps_overlay.h"
#include "orb_ps_particle.h"
#include "orb_vs_full.h"
#include "orb_vs_particle.h"

using Microsoft::WRL::ComPtr;
using namespace docky::pairing;

namespace {

constexpr float kCardW = 440, kCardH = 620, kMargin = 44;  // DIP
constexpr float kOrbR = 88, kOrbY = 250;                    // DIP (orb centre from the card's top)
constexpr double kCodeLife = 60.0;                          // s, then a new secret

struct Constants {
    float targetSize[2], cardMin[2], cardMax[2];
    float corner, dpi;
    float orbCenter[2];
    float orbR, time, absorb, intro, codeAlpha, hasDesk;
    float deskOrigin[2], deskSize[2];
    float textMix, textTop, textHeight, codeMode;
    float closeCenter[2], closeState, appear;
};
static_assert(sizeof(Constants) % 16 == 0, "constant buffer");
struct ParticleGpu { float a[4], b[4]; };

double Now() { return docky::vblank::Seconds(); }

struct Options {
    bool demo = false, desktop = true, dynamicCode = true;
    int codeMode = 1;  // 0 static, 1 dynamic, 2 spinning ring
    bool fixedSecret = false; uint64_t secret = 0;
    std::wstring seqDir; double seqAfter = 1.0, seqInterval = 0.1; int seqCount = 0;
    double exitAfter = 0, bench = 0; std::wstring benchOut;
} g_opt;

HWND g_wnd = nullptr;
bool g_quit = false;

uint64_t RandomSecret() {
    uint64_t v = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) v = std::random_device{}() * 0x9E3779B97F4A7C15ull;
    return v;
}

class App {
public:
    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_))) return false;
        ComPtr<IDXGIDevice> dxgi; dev_.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory_));
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
        if (FAILED(dcomp_->CreateTargetForHwnd(g_wnd, TRUE, &target_)) || FAILED(dcomp_->CreateVisual(&visual_))) return false;
        target_->SetRoot(visual_.Get());
        if (FAILED(dev_->CreateVertexShader(g_OrbVSFull, sizeof(g_OrbVSFull), nullptr, &vsFull_)) ||
            FAILED(dev_->CreateVertexShader(g_OrbVSParticle, sizeof(g_OrbVSParticle), nullptr, &vsPart_)) ||
            FAILED(dev_->CreatePixelShader(g_OrbPSGlass, sizeof(g_OrbPSGlass), nullptr, &psGlass_)) ||
            FAILED(dev_->CreatePixelShader(g_OrbPSParticle, sizeof(g_OrbPSParticle), nullptr, &psPart_)) ||
            FAILED(dev_->CreatePixelShader(g_OrbPSOverlay, sizeof(g_OrbPSOverlay), nullptr, &psOver_))) return false;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(Constants); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&cb, nullptr, &cb_))) return false;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &sampler_);
        D3D11_BLEND_DESC bd{};  // additive (particles)
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev_->CreateBlendState(&bd, &add_);
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;  // premultiplied over (overlay)
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        dev_->CreateBlendState(&bd, &over_);
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        dev_->CreateRasterizerState(&rd, &raster_);
        if (g_opt.desktop) {
            auto& sd2 = docky::desk::SharedDesktop::Get();
            sd2.AddUser(); deskUser_ = true;
            sd2.SetOwner(dev_.Get());
        }
        start_ = Now();
        NewCode(start_);
        return true;
    }

    void Shutdown() {
        if (deskUser_) { auto& sd = docky::desk::SharedDesktop::Get(); sd.SetOwner(nullptr); sd.RemoveUser(); }
        if (visual_) { visual_->SetContent(nullptr); dcomp_->Commit(); }
    }

    void Scan(double now) { if (absorbAt_ < 0) absorbAt_ = now; }
    void Reset(double now) { absorbAt_ = -1; introAt_ = now; NewCode(now); }

    void Frame() {
        const double now = Now();
        if (g_opt.demo) {  // scan after 3.5 s, connected for 3.5 s, then again
            const double t = std::fmod(now - start_, 7.0);
            if (t >= 3.5 && absorbAt_ < 0) Scan(now);
            if (t < 3.5 && absorbAt_ >= 0 && now - absorbAt_ > 3.4) Reset(now);
        }
        if (!g_opt.fixedSecret && absorbAt_ < 0 && now - codeAt_ > kCodeLife) NewCode(now);
        RECT cr{}; GetClientRect(g_wnd, &cr);
        const UINT w = static_cast<UINT>(std::max<LONG>(cr.right, 1)), h = static_cast<UINT>(std::max<LONG>(cr.bottom, 1));
        if (!EnsureSwap(w, h)) return;
        const float dpi = static_cast<float>(GetDpiForWindow(g_wnd)) / 96.f;
        if (dpi != textDpi_) BuildText(dpi);
        UpdateDesktop();

        Constants c{};
        c.targetSize[0] = static_cast<float>(w); c.targetSize[1] = static_cast<float>(h);
        // Appear: the card materialises (scale 0.94 -> 1 and opacity, critically damped: no overshoot), about 0.45 s.
        const double at = std::clamp((now - start_) / 0.45, 0.0, 1.0);
        const double ease = 1 - std::pow(1 - at, 3.0);
        const float grow = static_cast<float>(0.94 + 0.06 * ease);
        const float fullMinX = kMargin * dpi, fullMinY = kMargin * dpi * 0.8f;
        const float cxCard = fullMinX + kCardW * dpi * 0.5f, cyCard = fullMinY + kCardH * dpi * 0.5f;
        c.cardMin[0] = cxCard - kCardW * dpi * 0.5f * grow; c.cardMin[1] = cyCard - kCardH * dpi * 0.5f * grow;
        c.cardMax[0] = cxCard + kCardW * dpi * 0.5f * grow; c.cardMax[1] = cyCard + kCardH * dpi * 0.5f * grow;
        c.appear = static_cast<float>(ease);
        c.closeCenter[0] = c.cardMin[0] + 24 * dpi; c.closeCenter[1] = c.cardMin[1] + 24 * dpi;
        {   // close button state from the pointer (hover; pressed while the left button is down over it)
            POINT pt; GetCursorPos(&pt); ScreenToClient(g_wnd, &pt);
            const float dx = pt.x - c.closeCenter[0], dy = pt.y - c.closeCenter[1];
            const bool over = dx * dx + dy * dy < 14 * 14 * dpi * dpi && GetForegroundWindow() == g_wnd;
            c.closeState = over ? ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 2.f : 1.f) : 0.f;
        }
        c.corner = 52 * dpi * grow; c.dpi = dpi;  // a continuous (superellipse) corner reads ~2/3 as round as a circular one
        c.orbCenter[0] = cxCard; c.orbCenter[1] = fullMinY + kOrbY * dpi;
        c.orbR = kOrbR * dpi;
        c.time = static_cast<float>(now - start_);
        c.absorb = absorbAt_ >= 0 ? static_cast<float>(now - absorbAt_) : -1.f;
        c.intro = static_cast<float>(now - introAt_);
        // A new code: the rings fade in over 0.5 s.
        c.codeAlpha = static_cast<float>(std::clamp((now - codeAt_) / 0.5, 0.0, 1.0));
        c.hasDesk = deskSrv_ ? 1.f : 0.f;
        POINT org{0, 0}; ClientToScreen(g_wnd, &org);
        c.deskOrigin[0] = static_cast<float>(org.x - monitor_.left); c.deskOrigin[1] = static_cast<float>(org.y - monitor_.top);
        c.deskSize[0] = static_cast<float>(deskW_); c.deskSize[1] = static_cast<float>(deskH_);
        c.textMix = c.absorb > 0 ? static_cast<float>(std::clamp((c.absorb - 1.4) / 0.4, 0.0, 1.0)) : 0.f;
        c.codeMode = static_cast<float>(g_opt.codeMode);
        c.textTop = fullMinY + 470 * dpi; c.textHeight = static_cast<float>(textBlockH_);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); ctx_->Unmap(cb_.Get(), 0); }

        ComPtr<ID3D11Texture2D> back; swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> rtv; dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        const float clear[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* r = rtv.Get();
        ctx_->OMSetRenderTargets(1, &r, nullptr);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_.Get());
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cbs[] = {cb_.Get()};
        ctx_->VSSetConstantBuffers(0, 1, cbs); ctx_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* ss[] = {sampler_.Get()};
        ctx_->PSSetSamplers(0, 1, ss);
        ID3D11ShaderResourceView* srvs[] = {deskSrv_.Get(), textSrv_.Get(), partSrv_.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->VSSetShaderResources(2, 1, &srvs[2]);
        // glass
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->VSSetShader(vsFull_.Get(), nullptr, 0); ctx_->PSSetShader(psGlass_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);
        // particles
        ctx_->OMSetBlendState(add_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->VSSetShader(vsPart_.Get(), nullptr, 0); ctx_->PSSetShader(psPart_.Get(), nullptr, 0);
        ctx_->Draw(partCount_ * 6, 0);
        // overlay
        ctx_->OMSetBlendState(over_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->VSSetShader(vsFull_.Get(), nullptr, 0); ctx_->PSSetShader(psOver_.Get(), nullptr, 0);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* none[3] = {};
        ctx_->PSSetShaderResources(0, 2, none); ctx_->VSSetShaderResources(2, 1, none);

        // snapshots
        if (!g_opt.seqDir.empty() && seqIndex_ < g_opt.seqCount && now - start_ >= g_opt.seqAfter + seqIndex_ * g_opt.seqInterval) {
            wchar_t name[64]; swprintf_s(name, L"\\orb_%04d.bmp", seqIndex_++);
            SaveBmp(back.Get(), g_opt.seqDir + name);
        }
        swap_->Present(0, 0);
        dcomp_->Commit();
        const double t1 = Now();
        if (lastFrame_ > 0) { frameIv_.push_back(t1 - lastFrame_); }
        lastFrame_ = t1;
        if (g_opt.exitAfter > 0 && now - start_ > g_opt.exitAfter) { WriteBench(); g_quit = true; }
    }

    uint64_t Secret() const { return payload_.secret; }

private:
    void NewCode(double now) {
        OrbBits bits;
        do {
            payload_.version = 1; payload_.flags = 1;  // offers clipboard sync
            payload_.secret = g_opt.fixedSecret ? g_opt.secret : RandomSecret();
            bits = Encode(payload_);
        } while (!Balanced(bits) && !g_opt.fixedSecret);
        codeAt_ = now;
        BuildParticles(bits);
        wchar_t title[128]; swprintf_s(title, L"DockDrop pairing - %016llx", static_cast<unsigned long long>(payload_.secret));
        SetWindowTextW(g_wnd, title);
    }

    void BuildParticles(const OrbBits& bits) {
        std::vector<ParticleGpu> ps;
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> u(0, 1);
        // orb: mostly on the shell, some inside (depth), denser bands
        for (int i = 0; i < 22000; ++i) {
            ParticleGpu p{};
            const float z = 2 * u(rng) - 1;
            p.a[0] = 0; p.a[1] = std::asin(z); p.a[2] = 6.2831853f * u(rng);
            const float shell = u(rng);
            p.a[3] = shell < 0.78f ? 0.94f + 0.06f * u(rng) : 0.35f + 0.6f * std::sqrt(u(rng));
            p.b[0] = 0.6f + 0.8f * u(rng); p.b[1] = 0.9f + 1.3f * u(rng) * u(rng); p.b[2] = u(rng); p.b[3] = u(rng);
            ps.push_back(p);
        }
        // streams: 7 tilted great circles around the sphere, particles bunched towards each stream's head
        for (int sidx = 0; sidx < 7; ++sidx) {
            const float th = 0.35f + 2.4f * u(rng), ph = 6.2831853f * u(rng), speed = (0.35f + 0.35f * u(rng)) * (u(rng) < 0.5f ? -1.f : 1.f);
            const float head = 6.2831853f * u(rng);
            for (int i = 0; i < 900; ++i) {
                ParticleGpu p{};
                const float back = std::pow(u(rng), 1.3f) * 5.0f;  // how far behind the head
                p.a[0] = 3; p.a[1] = th; p.a[2] = ph; p.a[3] = head - back * (speed > 0 ? 1.f : -1.f);
                p.b[0] = speed; p.b[1] = 0.8f + 1.0f * u(rng); p.b[2] = 2 * u(rng) - 1; p.b[3] = 1 - back / 5.0f;
                ps.push_back(p);
            }
        }
        // code arcs: ~3 particles per DIP of arc length
        for (int ring = 0; ring < kRings; ++ring) for (int slot = 0; slot < kSlots; ++slot) {
            if (!bits[static_cast<size_t>(ring * kSlots + slot)]) continue;
            const float rr = kRingRadius[ring];
            const float half = static_cast<float>(kPi) * kArcFill / kSlots;  // half the arc's angle
            const float centre = static_cast<float>(2 * kPi * (slot + 0.5) / kSlots);
            const int n = static_cast<int>(2 * half * rr * kOrbR * 8.0f);
            for (int k = 0; k < n; ++k) {
                ParticleGpu p{};
                p.a[0] = 1; p.a[1] = rr; p.a[2] = centre; p.a[3] = half;
                p.b[0] = u(rng); p.b[1] = 1.0f + 0.6f * u(rng); p.b[2] = 2 * u(rng) - 1; p.b[3] = u(rng);
                ps.push_back(p);
            }
        }
        // outer star rings: seven tilted rings of stars from 2.3 R to 3.5 R (decoration and depth; see orb.hlsl type 4)
        for (int ring = 0; ring < 7; ++ring) {
            const float rad = 2.3f + 0.2f * ring;
            const int n = static_cast<int>(340 + 50 * ring);
            const float speed = 0.35f + 0.25f * u(rng);
            for (int i = 0; i < n; ++i) {
                ParticleGpu p{};
                p.a[0] = 4; p.a[1] = rad * (1 + 0.012f * (u(rng) - 0.5f)); p.a[2] = 6.2831853f * (i + 0.6f * u(rng)) / n; p.a[3] = static_cast<float>(ring);
                p.b[0] = speed; p.b[1] = 1.0f + 1.4f * u(rng) * u(rng); p.b[2] = u(rng) * 2.0f; p.b[3] = u(rng);
                ps.push_back(p);
            }
        }
        // deep field: faint stars coming slowly out of the depth
        for (int i = 0; i < 700; ++i) {
            ParticleGpu p{};
            const float a = 6.2831853f * u(rng), rr = 0.15f + 0.85f * std::sqrt(u(rng));
            p.a[0] = 5; p.a[1] = rr * std::cos(a); p.a[2] = rr * std::sin(a); p.a[3] = u(rng);
            p.b[1] = 0.6f + 0.9f * u(rng); p.b[3] = u(rng);
            ps.push_back(p);
        }
        // satellites (code mode 4, orb_code.h EncodeSat): on each of three orbits, 8 satellites at their positions, each
        // a bright head and a comet tail trailing behind (the constellation turns; orb.hlsl type 6)
        {
            const SatSlots sat = EncodeSat(payload_);
            for (int o = 0; o < kSatOrbits; ++o) for (int k = 0; k < kSatPerOrbit; ++k) {
                const float base = static_cast<float>(2 * kPi * sat[static_cast<size_t>(o)][static_cast<size_t>(k)] / kSatPositions);
                for (int i = 0; i < 46; ++i) {  // head: a tight bright cluster
                    ParticleGpu p{};
                    p.a[0] = 6; p.a[1] = kSatRadius[o]; p.a[2] = base; p.a[3] = 0;
                    const float rr = 0.5f * std::sqrt(u(rng)), aa = 6.2831853f * u(rng);
                    p.b[0] = rr * std::cos(aa); p.b[1] = 1.0f + 0.9f * u(rng); p.b[2] = rr * std::sin(aa); p.b[3] = u(rng);
                    ps.push_back(p);
                }
                for (int i = 0; i < 70; ++i) {  // tail: thins and fades behind the head
                    ParticleGpu p{};
                    const float along = std::pow(u(rng), 0.8f);
                    p.a[0] = 6; p.a[1] = kSatRadius[o]; p.a[2] = base; p.a[3] = along;
                    p.b[0] = (2 * u(rng) - 1) * (0.3f + 0.7f * along); p.b[1] = 0.8f + 0.7f * u(rng); p.b[2] = 0; p.b[3] = u(rng);
                    ps.push_back(p);
                }
            }
        }
        // halo dust (kept)
        for (int i = 0; i < 1400; ++i) {
            ParticleGpu p{};
            // outside the outermost code ring: drifting dust between the rings would fill the gaps (the scanner reads gaps as dark)
            p.a[0] = 2; p.a[1] = 1.98f + 0.6f * u(rng); p.a[2] = 6.2831853f * u(rng);
            p.b[1] = 0.7f + 0.6f * u(rng); p.b[3] = u(rng);
            ps.push_back(p);
        }
        partCount_ = static_cast<UINT>(ps.size());
        partBuf_.Reset(); partSrv_.Reset();
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = static_cast<UINT>(ps.size() * sizeof(ParticleGpu)); bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = sizeof(ParticleGpu);
        D3D11_SUBRESOURCE_DATA init{ps.data(), 0, 0};
        if (FAILED(dev_->CreateBuffer(&bd, &init, &partBuf_))) { partCount_ = 0; return; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = partCount_;
        dev_->CreateShaderResourceView(partBuf_.Get(), &sv, &partSrv_);
    }

    // Two text blocks (scan / connected) drawn once with GDI into a coverage texture.
    void BuildText(float dpi) {
        textDpi_ = dpi;
        const int w = static_cast<int>(kCardW * dpi), bh = static_cast<int>(96 * dpi);
        textBlockH_ = bh;
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -2 * bh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return; }
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, static_cast<size_t>(w) * 2 * bh * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        HFONT big = CreateFontW(-static_cast<int>(23 * dpi), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
        HFONT fontSmall = CreateFontW(-static_cast<int>(14 * dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
        auto block = [&](int top, const wchar_t* title, const wchar_t* sub) {
            RECT r{static_cast<LONG>(24 * dpi), top, static_cast<LONG>(w - 24 * dpi), top + static_cast<LONG>(36 * dpi)};
            SelectObject(dc, big); DrawTextW(dc, title, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            RECT r2{static_cast<LONG>(28 * dpi), top + static_cast<LONG>(40 * dpi), static_cast<LONG>(w - 28 * dpi), top + bh};
            SelectObject(dc, fontSmall); DrawTextW(dc, sub, -1, &r2, DT_CENTER | DT_WORDBREAK);
        };
        block(0, L"用手机扫描以配对", L"在手机浏览器打开下面的网址，把相机对准这团光点\ndavizcbia.github.io/dockdrop-scan");
        block(bh, L"已连接", L"现在可以用 DockDrop 互传文件、同步剪贴板。");
        GdiFlush();
        std::vector<uint8_t> cov(static_cast<size_t>(w) * 2 * bh);
        const uint8_t* px = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < cov.size(); ++i) cov[i] = std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]});
        SelectObject(dc, old); DeleteObject(bmp); DeleteObject(big); DeleteObject(fontSmall); DeleteDC(dc);
        D3D11_TEXTURE2D_DESC d{}; d.Width = static_cast<UINT>(w); d.Height = static_cast<UINT>(2 * bh); d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{cov.data(), static_cast<UINT>(w), 0};
        ComPtr<ID3D11Texture2D> tex;
        textSrv_.Reset();
        if (SUCCEEDED(dev_->CreateTexture2D(&d, &init, &tex))) dev_->CreateShaderResourceView(tex.Get(), nullptr, &textSrv_);
    }

    void UpdateDesktop() {
        if (!deskUser_) return;
        HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
        monitor_ = mi.rcMonitor;
        auto& sd = docky::desk::SharedDesktop::Get();
        sd.Want(monitor_);
        if (sd.Owned()) sd.PumpOwned(0);
        if (!sd.Valid()) return;
        if (!view_ || viewGen_ != sd.Generation()) {
            view_.Reset();
            if (HANDLE hnd = sd.SharedHandle()) dev_->OpenSharedResource(hnd, IID_PPV_ARGS(&view_));
            viewGen_ = sd.Generation(); lastSerial_ = 0;
        }
        const uint64_t serial = sd.Serial();
        if (!view_ || serial == lastSerial_) return;
        lastSerial_ = serial;
        D3D11_TEXTURE2D_DESC vd{}; view_->GetDesc(&vd);
        if (!desk_ || vd.Width != deskW_ || vd.Height != deskH_) {
            desk_.Reset(); deskSrv_.Reset();
            D3D11_TEXTURE2D_DESC d{}; d.Width = vd.Width; d.Height = vd.Height; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &desk_)) || FAILED(dev_->CreateShaderResourceView(desk_.Get(), nullptr, &deskSrv_))) { desk_.Reset(); deskSrv_.Reset(); return; }
            deskW_ = vd.Width; deskH_ = vd.Height;
        }
        const D3D11_BOX box{0, 0, 0, vd.Width, vd.Height, 1};
        ctx_->CopySubresourceRegion(desk_.Get(), 0, 0, 0, 0, view_.Get(), 0, &box);
        ctx_->GenerateMips(deskSrv_.Get());
    }

    bool EnsureSwap(UINT w, UINT h) {
        if (swap_ && w == swapW_ && h == swapH_) return true;
        if (swap_) { ctx_->ClearState(); ctx_->Flush(); if (SUCCEEDED(swap_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) { swapW_ = w; swapH_ = h; return true; } swap_.Reset(); }
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory_->CreateSwapChainForComposition(dev_.Get(), &d, nullptr, &swap_))) return false;
        visual_->SetContent(swap_.Get());
        swapW_ = w; swapH_ = h;
        return true;
    }

    bool SaveBmp(ID3D11Texture2D* tex, const std::wstring& path) {
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
        ctx_->CopyResource(st.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) { ctx_->Unmap(st.Get(), 0); return false; }
        const uint32_t row = d.Width * 4, size = row * d.Height;
        BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + size;
        BITMAPINFOHEADER ih{}; ih.biSize = sizeof(ih); ih.biWidth = static_cast<LONG>(d.Width); ih.biHeight = -static_cast<LONG>(d.Height); ih.biPlanes = 1; ih.biBitCount = 32; ih.biSizeImage = size;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        std::vector<uint8_t> line(row);
        for (UINT y = 0; y < d.Height; ++y) {  // premultiplied over a neutral grey, written opaque
            const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
            for (UINT x = 0; x < d.Width; ++x) {
                const int a = src[x * 4 + 3];
                for (int c = 0; c < 3; ++c) line[x * 4 + c] = static_cast<uint8_t>(std::min(255, src[x * 4 + c] + (255 - a) * 60 / 255));
                line[x * 4 + 3] = 255;
            }
            fwrite(line.data(), row, 1, f);
        }
        fclose(f);
        ctx_->Unmap(st.Get(), 0);
        return true;
    }

    void WriteBench() {
        if (g_opt.benchOut.empty() || frameIv_.size() < 10) return;
        std::vector<double> v(frameIv_.begin() + static_cast<long long>(frameIv_.size() / 10), frameIv_.end());
        std::sort(v.begin(), v.end());
        double mean = 0; for (double x : v) mean += x; mean /= v.size();
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_opt.benchOut.c_str(), L"w") != 0 || !f) return;
        fprintf(f, "frames %zu, %.1f fps, interval mean %.3f ms, p99 %.3f ms, max %.3f ms\n", v.size(), 1.0 / mean, mean * 1000,
                v[static_cast<size_t>(v.size() * 0.99)] * 1000, v.back() * 1000);
        fclose(f);
    }

    ComPtr<ID3D11Device> dev_; ComPtr<ID3D11DeviceContext> ctx_; ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDCompositionDevice> dcomp_; ComPtr<IDCompositionTarget> target_; ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDXGISwapChain1> swap_; UINT swapW_ = 0, swapH_ = 0;
    ComPtr<ID3D11VertexShader> vsFull_, vsPart_; ComPtr<ID3D11PixelShader> psGlass_, psPart_, psOver_;
    ComPtr<ID3D11Buffer> cb_, partBuf_; ComPtr<ID3D11ShaderResourceView> partSrv_, textSrv_, deskSrv_;
    ComPtr<ID3D11SamplerState> sampler_; ComPtr<ID3D11BlendState> add_, over_; ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Texture2D> view_, desk_; UINT deskW_ = 1, deskH_ = 1; uint64_t lastSerial_ = 0; unsigned viewGen_ = ~0u;
    RECT monitor_{};
    bool deskUser_ = false;
    UINT partCount_ = 0;
    float textDpi_ = 0; int textBlockH_ = 1;
    OrbPayload payload_;
    double start_ = 0, introAt_ = 0, absorbAt_ = -1, codeAt_ = 0, lastFrame_ = 0;
    int seqIndex_ = 0;
    std::vector<double> frameIv_;
};

App* g_app = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &pt);
        const float dpi = static_cast<float>(GetDpiForWindow(h)) / 96.f;
        const float cx = (kMargin + 24) * dpi, cy = (kMargin * 0.8f + 24) * dpi;  // close button (top left)
        if ((pt.x - cx) * (pt.x - cx) + (pt.y - cy) * (pt.y - cy) < 14 * 14 * dpi * dpi) return HTCLIENT;
        return HTCAPTION;  // drag the card anywhere
    }
    case WM_LBUTTONDOWN: SetCapture(h); return 0;  // only the close button is client area
    case WM_LBUTTONUP: {  // commit on release, and only if still over the button (drag away to cancel)
        ReleaseCapture();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const float dpi = static_cast<float>(GetDpiForWindow(h)) / 96.f;
        const float cx = (kMargin + 24) * dpi, cy = (kMargin * 0.8f + 24) * dpi;
        if ((pt.x - cx) * (pt.x - cx) + (pt.y - cy) * (pt.y - cy) < 14 * 14 * dpi * dpi) g_quit = true;
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_quit = true;
        else if (wp == VK_SPACE && g_app) g_app->Scan(Now());
        else if (wp == 'R' && g_app) g_app->Reset(Now());
        return 0;
    case WM_ENTERSIZEMOVE: SetTimer(h, 1, 8, nullptr); return 0;  // keep animating while the card is dragged
    case WM_EXITSIZEMOVE: KillTimer(h, 1); return 0;
    case WM_TIMER: if (g_app) g_app->Frame(); return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: g_quit = true; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--demo") g_opt.demo = true;
        else if (a == L"--no-desktop") g_opt.desktop = false;
        else if (a == L"--static") g_opt.codeMode = 0;
        else if (a == L"--spin") g_opt.codeMode = 2;
        else if (a == L"--radar") g_opt.codeMode = 3;
        else if (a == L"--sat") g_opt.codeMode = 4;
        else if (a == L"--secret") { g_opt.fixedSecret = true; g_opt.secret = std::wcstoull(next().c_str(), nullptr, 16); }
        else if (a == L"--snapshot-seq") g_opt.seqDir = next();
        else if (a == L"--snapshot-after") g_opt.seqAfter = _wtof(next().c_str());
        else if (a == L"--snapshot-interval") g_opt.seqInterval = std::max(0.005, _wtof(next().c_str()));
        else if (a == L"--snapshot-count") g_opt.seqCount = _wtoi(next().c_str());
        else if (a == L"--exit-after") g_opt.exitAfter = _wtof(next().c_str());
        else if (a == L"--bench-out") g_opt.benchOut = next();
    }
    LocalFree(argv);

    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"DockyOrb";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&wc);
    POINT cursor; GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    const float dpi = dx / 96.f;
    const int w = static_cast<int>((kCardW + 2 * kMargin) * dpi), h = static_cast<int>((kCardH + 2 * kMargin) * dpi);
    const int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    g_wnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"DockDrop pairing", WS_POPUP, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;
    // Not in screen capture: the glass sees the desktop behind it, never itself.
    SetWindowDisplayAffinity(g_wnd, WDA_EXCLUDEFROMCAPTURE);
    App app; g_app = &app;
    if (!app.Init()) { MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"DockDrop pairing", MB_ICONERROR); return 1; }
    ShowWindow(g_wnd, SW_SHOW);
    while (!g_quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) g_quit = true; TranslateMessage(&m); DispatchMessageW(&m); }
        if (g_quit) break;
        app.Frame();
        docky::vblank::WaitFrame(g_wnd);
    }
    app.Shutdown();
    g_app = nullptr;
    DestroyWindow(g_wnd);
    return 0;
}
