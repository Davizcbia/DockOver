#include "blackhole_renderer.h"

#include "schwarzschild_lut.h"
#include "../log.h"
#include "blackhole_ps.h"
#include "blackhole_vs.h"
#include "blackhole_debris_ps.h"
#include "blackhole_debris_vs.h"

#include <algorithm>

#include <chrono>
#include <cstdio>
#include <vector>

namespace docky::blackhole {

bool LensRenderer::Init(ID3D11Device* device) {
    device_ = device; device->GetImmediateContext(&ctx_);
    if (FAILED(device->CreateVertexShader(g_BlackHoleVS, sizeof(g_BlackHoleVS), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(g_BlackHolePS, sizeof(g_BlackHolePS), nullptr, &ps_))) return false;
    D3D11_BUFFER_DESC b{}; b.ByteWidth = sizeof(LensConstants); b.Usage = D3D11_USAGE_DYNAMIC; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER; b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&b, nullptr, &cb_))) return false;
    D3D11_SAMPLER_DESC s{}; s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    s.MaxLOD = 6.f;  // a seam in the gradients (antipode of the sky map) never blurs to a single colour
    if (FAILED(device->CreateSamplerState(&s, &mirror_))) return false;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD = 0;
    if (FAILED(device->CreateSamplerState(&s, &clamp_))) return false;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &clampMip_))) return false;
    {
        D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = kPayloadSize; d.MipLevels = 0; d.ArraySize = 4; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &payload_)) || FAILED(device->CreateShaderResourceView(payload_.Get(), nullptr, &payloadSrv_))) return false;
    }
    {   // debris: particle buffer, premultiplied-over blending, no culling (quads flip with the motion direction)
        if (FAILED(device->CreateVertexShader(g_BlackHoleDebrisVS, sizeof(g_BlackHoleDebrisVS), nullptr, &debrisVs_)) ||
            FAILED(device->CreatePixelShader(g_BlackHoleDebrisPS, sizeof(g_BlackHoleDebrisPS), nullptr, &debrisPs_))) return false;
        D3D11_BUFFER_DESC d{}; d.ByteWidth = static_cast<UINT>(sizeof(DebrisGpu) * DebrisSystem::kMaxParticles); d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = sizeof(DebrisGpu);
        if (FAILED(device->CreateBuffer(&d, nullptr, &debrisBuf_)) || FAILED(device->CreateShaderResourceView(debrisBuf_.Get(), nullptr, &debrisBufSrv_))) return false;
        D3D11_BUFFER_DESC c{}; c.ByteWidth = 16; c.Usage = D3D11_USAGE_DYNAMIC; c.BindFlags = D3D11_BIND_CONSTANT_BUFFER; c.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&c, nullptr, &debrisCb_))) return false;
        D3D11_BLEND_DESC bd{}; auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE; rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bd, &premulBlend_))) return false;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rd, &noCull_))) return false;
    }
    {   // Warm-up: one invisible particle through the debris pipeline, so the driver compiles it now and not on the
        // frame an icon shatters (that first-use stall was a visible hitch).
        if (EnsureDebrisTarget(128)) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx_->Map(debrisBuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                DebrisGpu z{}; memcpy(m.pData, &z, sizeof(z)); ctx_->Unmap(debrisBuf_.Get(), 0);
                const float dc[4] = {1, 1.f / 64, static_cast<float>(DebrisSystem::kTiltCos), 0};
                if (SUCCEEDED(ctx_->Map(debrisCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, dc, sizeof(dc)); ctx_->Unmap(debrisCb_.Get(), 0); }
                ID3D11RenderTargetView* r = debrisRtv_.Get();
                const D3D11_VIEWPORT vp{0, 0, 128, 128, 0, 1};
                ctx_->OMSetRenderTargets(1, &r, nullptr); ctx_->RSSetViewports(1, &vp);
                ctx_->OMSetBlendState(premulBlend_.Get(), nullptr, 0xFFFFFFFF); ctx_->RSSetState(noCull_.Get());
                ctx_->IASetInputLayout(nullptr); ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx_->VSSetShader(debrisVs_.Get(), nullptr, 0); ctx_->PSSetShader(debrisPs_.Get(), nullptr, 0);
                ID3D11Buffer* cb = debrisCb_.Get(); ctx_->VSSetConstantBuffers(0, 1, &cb);
                ID3D11ShaderResourceView* v = debrisBufSrv_.Get(); ctx_->VSSetShaderResources(0, 1, &v);
                ctx_->Draw(6, 0);
                ID3D11ShaderResourceView* nv = nullptr; ctx_->VSSetShaderResources(0, 1, &nv);
                ID3D11RenderTargetView* nr = nullptr; ctx_->OMSetRenderTargets(1, &nr, nullptr);
                ctx_->RSSetState(nullptr); ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            }
        }
    }
    for (auto& t : timing_) {
        D3D11_QUERY_DESC q{}; q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&q, &t.disjoint);
        q.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&q, &t.begin); device->CreateQuery(&q, &t.end);
    }
    return BuildLut();
}

bool LensRenderer::BuildLut() {
    const auto t0 = std::chrono::steady_clock::now();
    const LutSpec spec;
    const Lut lut = docky::blackhole::BuildLut(spec);
    lutMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bMax_ = static_cast<float>(spec.bMax); delta_ = static_cast<float>(spec.delta); phiMax_ = static_cast<float>(spec.phiMax);
    auto make = [&](UINT w, UINT h, const float* data, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data, w * 4u, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        return SUCCEEDED(device_->CreateTexture2D(&d, &init, &tex)) && SUCCEEDED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv));
    };
    const bool ok = make(spec.columns, spec.rows, lut.u.data(), orbitSrv_) && make(spec.columns, 1, lut.phiEnd.data(), endSrv_);
    DLOG(L"blackhole: Schwarzschild tables %dx%d built in %.1f ms (%ls)", spec.columns, spec.rows, lutMs_, ok ? L"ok" : L"upload failed");
    return ok;
}

bool LensRenderer::UpdateDesktop(ID3D11Texture2D* source, uint64_t serial) {
    if (!source) return false;
    D3D11_TEXTURE2D_DESC sd{}; source->GetDesc(&sd);
    if (!desktop_ || sd.Width != deskW_ || sd.Height != deskH_) {
        desktop_.Reset(); desktopSrv_.Reset();
        D3D11_TEXTURE2D_DESC d{}; d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &desktop_)) || FAILED(device_->CreateShaderResourceView(desktop_.Get(), nullptr, &desktopSrv_))) {
            desktop_.Reset(); desktopSrv_.Reset(); return false;
        }
        deskW_ = sd.Width; deskH_ = sd.Height; deskSerial_ = 0;
    }
    if (serial == deskSerial_ && serial != 0) return true;
    const D3D11_BOX box{0, 0, 0, sd.Width, sd.Height, 1};
    ctx_->CopySubresourceRegion(desktop_.Get(), 0, 0, 0, 0, source, 0, &box);
    ctx_->GenerateMips(desktopSrv_.Get());
    deskSerial_ = serial;
    return true;
}

bool LensRenderer::UseTestPattern(UINT w, UINT h) {
    // Calibration grid: coloured quadrants, a fine grid every 40 px, bold lines every 200 px, a white cross at the centre.
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const bool right = x >= w / 2, bottom = y >= h / 2;
        uint32_t r = right ? 70 : 30, g = bottom ? 70 : 30, b = (right != bottom) ? 120 : 50;
        if ((x / 40 + y / 40) % 2) { r += 25; g += 25; b += 25; }
        if (x % 200 < 3 || y % 200 < 3) { r = 240; g = 200; b = 60; }
        if ((x > w / 2 - 3 && x < w / 2 + 3) || (y > h / 2 - 3 && y < h / 2 + 3)) { r = g = b = 255; }
        px[static_cast<size_t>(y) * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&d, &init, &tex))) return false;
    return UpdateDesktop(tex.Get(), 1);
}

bool LensRenderer::SetPayloadIcon(int slot, const uint32_t* pixels) {
    if (!payload_ || slot < 0 || slot >= 4 || !pixels) return false;
    D3D11_TEXTURE2D_DESC d{}; payload_->GetDesc(&d);
    ctx_->UpdateSubresource(payload_.Get(), D3D11CalcSubresource(0, slot, d.MipLevels), nullptr, pixels, kPayloadSize * 4, 0);
    ctx_->GenerateMips(payloadSrv_.Get());
    return true;
}

bool LensRenderer::EnsureDebrisTarget(UINT size) {
    if (debrisTex_ && debrisSize_ == size) return true;
    debrisTex_.Reset(); debrisRtv_.Reset(); debrisSrv_.Reset(); debrisSize_ = 0;
    D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = size; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &debrisTex_)) || FAILED(device_->CreateRenderTargetView(debrisTex_.Get(), nullptr, &debrisRtv_)) ||
        FAILED(device_->CreateShaderResourceView(debrisTex_.Get(), nullptr, &debrisSrv_))) { debrisTex_.Reset(); return false; }
    debrisSize_ = size;
    return true;
}

void LensRenderer::SetDebris(const std::vector<DebrisGpu>& particles, float extentM, UINT texSize) {
    debrisCount_ = 0;
    if (particles.empty() || !debrisBuf_ || extentM <= 0) return;
    if (!EnsureDebrisTarget(ClampDebris(texSize))) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(debrisBuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const size_t n = std::min(particles.size(), DebrisSystem::kMaxParticles);
    memcpy(m.pData, particles.data(), n * sizeof(DebrisGpu)); ctx_->Unmap(debrisBuf_.Get(), 0);
    debrisCount_ = static_cast<UINT>(n); debrisExtent_ = extentM;
}

void LensRenderer::Render(ID3D11RenderTargetView* rtv, UINT width, UINT height, LensConstants c) {
    c.bMax = bMax_; c.lutDelta = delta_; c.phiMax = phiMax_;
    c.debrisOn = debrisCount_ > 0 ? 1u : 0u; c.debrisExtent = debrisExtent_;
    c.debrisSin = static_cast<float>(DebrisSystem::kTiltSin); c.debrisCos = static_cast<float>(DebrisSystem::kTiltCos);
    c.desktopSize[0] = static_cast<float>(deskW_); c.desktopSize[1] = static_cast<float>(deskH_);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &c, sizeof(c)); ctx_->Unmap(cb_.Get(), 0);
    Timing& t = timing_[timingNext_ % 6];
    const bool timed = !t.pending && t.disjoint && t.begin && t.end;
    if (timed) { ctx_->Begin(t.disjoint.Get()); ctx_->End(t.begin.Get()); }
    const float clear[4] = {0, 0, 0, 0};
    if (debrisCount_ > 0) {  // debris into the lens-plane texture first
        D3D11_MAPPED_SUBRESOURCE dm{};
        if (SUCCEEDED(ctx_->Map(debrisCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
            const float dc[4] = {debrisExtent_, 2 * debrisExtent_ / debrisSize_, static_cast<float>(DebrisSystem::kTiltCos), 0};
            memcpy(dm.pData, dc, sizeof(dc)); ctx_->Unmap(debrisCb_.Get(), 0);
        }
        ID3D11RenderTargetView* drtv = debrisRtv_.Get();
        ctx_->ClearRenderTargetView(drtv, clear);
        const D3D11_VIEWPORT dvp{0, 0, static_cast<float>(debrisSize_), static_cast<float>(debrisSize_), 0, 1};
        ctx_->OMSetRenderTargets(1, &drtv, nullptr); ctx_->RSSetViewports(1, &dvp);
        ctx_->OMSetBlendState(premulBlend_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->RSSetState(noCull_.Get());
        ctx_->IASetInputLayout(nullptr); ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(debrisVs_.Get(), nullptr, 0); ctx_->PSSetShader(debrisPs_.Get(), nullptr, 0);
        ID3D11Buffer* dcb = debrisCb_.Get(); ctx_->VSSetConstantBuffers(0, 1, &dcb);
        ID3D11ShaderResourceView* bsrv = debrisBufSrv_.Get(); ctx_->VSSetShaderResources(0, 1, &bsrv);
        ctx_->Draw(debrisCount_ * 6, 0);
        ID3D11ShaderResourceView* nsrv = nullptr; ctx_->VSSetShaderResources(0, 1, &nsrv);
    }
    ctx_->ClearRenderTargetView(rtv, clear);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    ctx_->OMSetRenderTargets(1, &rtv, nullptr); ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->RSSetState(nullptr);
    ctx_->IASetInputLayout(nullptr); ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0); ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.Get(); ctx_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[5] = {desktopSrv_.Get(), orbitSrv_.Get(), endSrv_.Get(), payloadSrv_.Get(), debrisCount_ > 0 ? debrisSrv_.Get() : nullptr};
    ctx_->PSSetShaderResources(0, 5, srvs);
    ID3D11SamplerState* samplers[3] = {mirror_.Get(), clamp_.Get(), clampMip_.Get()};
    ctx_->PSSetSamplers(0, 3, samplers);
    if (desktopSrv_) ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[5] = {};
    ctx_->PSSetShaderResources(0, 5, none);
    debrisCount_ = 0;  // SetDebris again for the next frame
    ID3D11RenderTargetView* noRtv = nullptr; ctx_->OMSetRenderTargets(1, &noRtv, nullptr);
    if (timed) { ctx_->End(t.end.Get()); ctx_->End(t.disjoint.Get()); t.pending = true; ++timingNext_; }
}

double LensRenderer::PollGpuMs() {
    double newest = -1;
    for (unsigned k = 0; k < 6; ++k) {
        Timing& t = timing_[(timingNext_ + k) % 6];  // oldest first
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        if (ctx_->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(t.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx_->GetData(t.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (!dj.Disjoint && dj.Frequency && b >= a) newest = static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
    }
    return newest;
}

bool LensRenderer::SaveBmp(ID3D11Texture2D* target, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d{}; target->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging))) return false;
    ctx_->CopyResource(staging.Get(), target);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) { ctx_->Unmap(staging.Get(), 0); return false; }
    const uint32_t rowBytes = d.Width * 4, size = rowBytes * d.Height;
    BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + size;
    BITMAPINFOHEADER ih{}; ih.biSize = sizeof(ih); ih.biWidth = static_cast<LONG>(d.Width); ih.biHeight = -static_cast<LONG>(d.Height); ih.biPlanes = 1; ih.biBitCount = 32; ih.biSizeImage = size;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    // Premultiplied BGRA over black, written opaque so viewers show what the overlay adds.
    std::vector<uint8_t> row(rowBytes);
    for (UINT y = 0; y < d.Height; ++y) {
        memcpy(row.data(), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, rowBytes);
        for (UINT x = 0; x < d.Width; ++x) row[x * 4 + 3] = 255;
        fwrite(row.data(), rowBytes, 1, f);
    }
    fclose(f);
    ctx_->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace docky::blackhole
