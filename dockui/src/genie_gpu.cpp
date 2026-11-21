#include "genie_gpu.h"
#include "backdrop_wgc.h"
#include "vblank.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "genie_ps.h"
#include "genie_vs.h"
#include "log.h"

namespace dock::minimize::gpu {
namespace {

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;

constexpr int kRows = 160;                 // mesh rows; each row is exact (the Genie is linear across a row)
constexpr wchar_t kOverlayClass[] = L"DockyGenieOverlay";
constexpr wchar_t kHostClass[] = L"DockyGenieCapture";

struct Vertex { float x, y, u, v, a; };
struct Command { enum Kind { Animate, Forget, Snapshot } kind; HWND window; RECT normal; int direction; double seconds; };
struct Texture { ComPtr<ID3D11ShaderResourceView> srv; SIZE size{}; };

HWND g_notify = nullptr;
UINT g_message = 0;
std::thread g_thread;
std::mutex g_mutex;
std::deque<Command> g_queue;
std::map<HWND, RECT> g_tiles;
bool g_quit = false;
HANDLE g_wake = nullptr;

bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

// True when a Forget for `w` (or Stop) is waiting: the running animation is cancelled.
bool Cancelled(HWND w) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_quit) return true;
    for (const auto& c : g_queue) if (c.kind == Command::Forget && c.window == w) return true;
    return false;
}
RECT Tile(HWND w) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_tiles.find(w);
    return it == g_tiles.end() ? RECT{} : it->second;
}
void Pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, indices;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    // per animation
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE waitable = nullptr;
    HWND overlay = nullptr;
    RECT bounds{};
    std::map<HWND, Texture> cache;
    struct Fresh { Texture tex; ULONGLONG tick = 0; };
    std::map<HWND, Fresh> fresh;  // snapshots of still-visible windows (Snapshot)

    // One frame of a capture item into a mip-mapped texture.
    bool Grab(const wgc::GraphicsCaptureItem& item, Texture& out) {
        bool ok = false;
        try {
            auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
            auto session = pool.CreateCaptureSession(item);
            if(!dock::PrepareCleanCaptureSession(session)){session.Close();pool.Close();return false;}
            session.StartCapture();
            const ULONGLONG t0 = GetTickCount64();
            while (!ok && GetTickCount64() - t0 < 400) {
                Pump();
                if (auto frame = pool.TryGetNextFrame()) {
                    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    ComPtr<ID3D11Texture2D> surface;
                    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&surface)))) {
                        D3D11_TEXTURE2D_DESC sdesc{}; surface->GetDesc(&sdesc);
                        const auto content = frame.ContentSize();
                        const UINT cw = (std::min)(sdesc.Width, static_cast<UINT>((std::max)(1, content.Width)));
                        const UINT ch = (std::min)(sdesc.Height, static_cast<UINT>((std::max)(1, content.Height)));
                        D3D11_TEXTURE2D_DESC d{}; d.Width = cw; d.Height = ch; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                        ComPtr<ID3D11Texture2D> tex;
                        if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &tex)) && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &out.srv))) {
                            const D3D11_BOX box{0, 0, 0, cw, ch, 1};
                            context->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
                            context->GenerateMips(out.srv.Get());
                            out.size = SIZE{static_cast<LONG>(cw), static_cast<LONG>(ch)};
                            ok = true;
                        }
                    }
                    frame.Close();
                } else Sleep(2);
            }
            session.Close(); pool.Close();
        } catch (const winrt::hresult_error& e) { DLOG(L"minimize: snapshot capture failed 0x%08X", static_cast<unsigned>(e.code())); }
        return ok;
    }
    // The window while it is still on screen (direct window capture; works only before it is minimized).
    bool SnapshotVisible(HWND w) {
        if (!IsWindow(w) || !IsWindowVisible(w) || IsIconic(w)) return false;
        // A window capture draws Windows' yellow frame around the user's window unless borderless capture is granted.
        if (!dock::BorderlessCaptureGranted()) return false;
        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(interop->CreateForWindow(w, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            Fresh f;
            if (!Grab(item, f.tex)) return false;
            f.tick = GetTickCount64();
            fresh[w] = f;
            while (fresh.size() > 4) {  // keep the few most recent
                auto oldest = fresh.begin();
                for (auto it = fresh.begin(); it != fresh.end(); ++it) if (it->second.tick < oldest->second.tick) oldest = it;
                fresh.erase(oldest);
            }
            return true;
        } catch (...) { return false; }
    }

    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) return false;
        ComPtr<IDXGIDevice> dxgi; device.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) return false;
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) return false;
        winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        if (FAILED(device->CreateVertexShader(g_GenieVS, sizeof(g_GenieVS), nullptr, &vs))) return false;
        if (FAILED(device->CreatePixelShader(g_GeniePS, sizeof(g_GeniePS), nullptr, &ps))) return false;
        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        if (FAILED(device->CreateInputLayout(il, 3, g_GenieVS, sizeof(g_GenieVS), &layout))) return false;

        D3D11_BUFFER_DESC vb{}; vb.ByteWidth = sizeof(Vertex) * (kRows + 1) * 4; vb.Usage = D3D11_USAGE_DYNAMIC;
        vb.BindFlags = D3D11_BIND_VERTEX_BUFFER; vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&vb, nullptr, &vertices))) return false;
        std::vector<UINT16> idx;
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < 3; ++c) {
                const UINT16 a = static_cast<UINT16>(r * 4 + c), b = static_cast<UINT16>(a + 1), d = static_cast<UINT16>(a + 4), e = static_cast<UINT16>(a + 5);
                idx.insert(idx.end(), {a, d, b, b, d, e});
            }
        D3D11_BUFFER_DESC ib{}; ib.ByteWidth = static_cast<UINT>(idx.size() * sizeof(UINT16)); ib.Usage = D3D11_USAGE_IMMUTABLE; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{idx.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&ib, &init, &indices))) return false;

        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX; sd.MipLODBias = -0.25f;  // a touch sharper while shrinking
        if (FAILED(device->CreateSamplerState(&sd, &sampler))) return false;
        D3D11_BLEND_DESC bd{}; auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bd, &blend))) return false;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rd, &raster))) return false;

        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = inst; wc.lpszClassName = kOverlayClass; RegisterClassW(&wc);
        wc.lpszClassName = kHostClass; RegisterClassW(&wc);
        // Click-through (layered + transparent) and never captured into a glass backdrop.
        overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  kOverlayClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!overlay) return false;
        SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);
        if (!GetEnvironmentVariableW(L"LIQUID_MINIMIZE_CAPTURABLE", nullptr, 0)) SetWindowDisplayAffinity(overlay, WDA_EXCLUDEFROMCAPTURE);
        if (FAILED(dcomp->CreateTargetForHwnd(overlay, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual))) return false;
        target->SetRoot(visual.Get());
        return true;
    }

    // The minimized window's last content: its DWM thumbnail in an off-screen host, captured once.
    bool Capture(HWND w) {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        HWND host = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"", WS_POPUP,
                                    vx - 20000, vy - 20000, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!host) return false;
        const DWM_WINDOW_CORNER_PREFERENCE square = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(host, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof(square));
        HTHUMBNAIL thumb = nullptr; SIZE src{};
        bool ok = false;
        if (SUCCEEDED(DwmRegisterThumbnail(host, w, &thumb)) && SUCCEEDED(DwmQueryThumbnailSourceSize(thumb, &src)) && src.cx > 0 && src.cy > 0 &&
            src.cx <= 16384 && src.cy <= 16384) {
            SetWindowPos(host, nullptr, vx - 20000 - src.cx, vy - 20000 - src.cy, src.cx, src.cy, SWP_NOZORDER | SWP_NOACTIVATE);
            DWM_THUMBNAIL_PROPERTIES p{};
            p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            p.rcDestination = RECT{0, 0, src.cx, src.cy}; p.fVisible = TRUE; p.opacity = 255; p.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(thumb, &p);
            ShowWindow(host, SW_SHOWNOACTIVATE);
            DwmFlush();
            try {
                auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                wgc::GraphicsCaptureItem item{nullptr};
                winrt::check_hresult(interop->CreateForWindow(host, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
                auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
                auto session = pool.CreateCaptureSession(item);
                // The host is our own window parked 20000 px off every monitor, so Windows' yellow capture frame (and
                // the cursor) can never show on screen: a clean session is preferred but not required here. Requiring
                // it sent every Genie to the thumbnail-band fallback (visible horizontal strips) whenever borderless
                // capture was not granted.
                if (!dock::PrepareCleanCaptureSession(session)) {
                    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
                    DLOG(L"minimize: Genie captures its off-screen host without borderless access");
                }
                session.StartCapture();
                const ULONGLONG t0 = GetTickCount64();
                while (!ok && GetTickCount64() - t0 < 400) {
                    Pump();
                    if (auto frame = pool.TryGetNextFrame()) {
                        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        ComPtr<ID3D11Texture2D> surface;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&surface)))) {
                            D3D11_TEXTURE2D_DESC sdesc{}; surface->GetDesc(&sdesc);
                            const auto content = frame.ContentSize();
                            const UINT cw = (std::min)(sdesc.Width, static_cast<UINT>((std::max)(1, content.Width)));
                            const UINT ch = (std::min)(sdesc.Height, static_cast<UINT>((std::max)(1, content.Height)));
                            D3D11_TEXTURE2D_DESC d{}; d.Width = cw; d.Height = ch; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                            d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                            ComPtr<ID3D11Texture2D> tex; Texture t;
                            if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &tex)) && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &t.srv))) {
                                const D3D11_BOX box{0, 0, 0, cw, ch, 1};
                                context->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
                                context->GenerateMips(t.srv.Get());
                                t.size = SIZE{static_cast<LONG>(cw), static_cast<LONG>(ch)};
                                cache[w] = t; ok = true;
                            }
                        }
                        frame.Close();
                    } else {
                        Sleep(2);
                    }
                }
                session.Close(); pool.Close();
            } catch (const winrt::hresult_error& e) {
                DLOG(L"minimize: Genie capture failed 0x%08X", static_cast<unsigned>(e.code()));
            }
        }
        if (thumb) DwmUnregisterThumbnail(thumb);
        DestroyWindow(host);
        return ok;
    }

    bool Begin(HWND notify) {
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(notify, MONITOR_DEFAULTTOPRIMARY), &mi);
        bounds = mi.rcMonitor;
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = static_cast<UINT>(bounds.right - bounds.left); d.Height = static_cast<UINT>(bounds.bottom - bounds.top);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForComposition(device.Get(), &d, nullptr, &sc1)) || FAILED(sc1.As(&swap))) return false;
        swap->SetMaximumFrameLatency(1);
        waitable = swap->GetFrameLatencyWaitableObject();
        ComPtr<ID3D11Texture2D> back; swap->GetBuffer(0, IID_PPV_ARGS(&back));
        if (!back || FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv))) { End(); return false; }
        const float clear[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(rtv.Get(), clear);
        swap->Present(0, 0);
        visual->SetContent(swap.Get()); dcomp->Commit();
        SetWindowPos(overlay, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    docky::vblank::Gate gate;  // Settings > General > Frame rate
    void Draw(const Texture& tex, const RECT& from, const RECT& to, double t) {
        if (waitable) WaitForSingleObjectEx(waitable, 100, TRUE);
        docky::vblank::Pause(gate.Remaining()); gate.Mark();
        const float W = static_cast<float>(bounds.right - bounds.left), H = static_cast<float>(bounds.bottom - bounds.top);
        auto nx = [&](double x) { return static_cast<float>((x - bounds.left) / W * 2.0 - 1.0); };
        auto ny = [&](double y) { return static_cast<float>(1.0 - (y - bounds.top) / H * 2.0); };
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        auto* v = static_cast<Vertex*>(m.pData);
        for (int r = 0; r <= kRows; ++r) {
            const double u = static_cast<double>(r) / kRows;
            const Row row = GenieRow(from, to, t, u);
            const float y = ny(row.y), fu = static_cast<float>(u);
            v[r * 4 + 0] = Vertex{nx(row.left - 1.0), y, 0.f, fu, 0.f};   // 1 px fringe: anti-aliased side edges
            v[r * 4 + 1] = Vertex{nx(row.left), y, 0.f, fu, 1.f};
            v[r * 4 + 2] = Vertex{nx(row.right), y, 1.f, fu, 1.f};
            v[r * 4 + 3] = Vertex{nx(row.right + 1.0), y, 1.f, fu, 0.f};
        }
        context->Unmap(vertices.Get(), 0);
        const float clear[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(rtv.Get(), clear);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const float factor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(blend.Get(), factor, 0xffffffff);
        D3D11_VIEWPORT vp{0, 0, W, H, 0, 1}; context->RSSetViewports(1, &vp); context->RSSetState(raster.Get());
        const UINT stride = sizeof(Vertex), offset = 0;
        context->IASetInputLayout(layout.Get()); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, tex.srv.GetAddressOf()); context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->DrawIndexed(kRows * 18, 0, 0);
        swap->Present(1, 0);
    }

    void End() {
        ShowWindow(overlay, SW_HIDE);
        if (visual) { visual->SetContent(nullptr); dcomp->Commit(); }
        if (context) { context->OMSetRenderTargets(0, nullptr, nullptr); context->ClearState(); context->Flush(); }
        rtv.Reset();
        if (waitable) { CloseHandle(waitable); waitable = nullptr; }
        swap.Reset();
    }

    void Shutdown() {
        End();
        cache.clear();
        target.Reset(); visual.Reset(); dcomp.Reset();
        if (overlay) { DestroyWindow(overlay); overlay = nullptr; }
    }
};

void Post(HWND w, int direction, bool ok) { PostMessageW(g_notify, g_message, MAKEWPARAM(direction + 1, ok ? 1 : 0), reinterpret_cast<LPARAM>(w)); }

void RunAnimation(Renderer& r, bool ready, const Command& c) {
    if (!ready) { Post(c.window, c.direction, false); return; }
    auto it = r.cache.find(c.window);
    if (c.direction > 0) {  // a picture taken while the window was still visible beats the (often blank) minimized one
        auto f = r.fresh.find(c.window);
        if (f != r.fresh.end() && GetTickCount64() - f->second.tick < 6000) {
            r.cache[c.window] = f->second.tex; r.fresh.erase(f);
            DLOG(L"minimize: Genie uses the snapshot taken while the window was visible");
            it = r.cache.find(c.window);
        } else it = r.cache.end();
    }
    if (it == r.cache.end()) {
        LARGE_INTEGER q0{}, q1{}, f{}; QueryPerformanceCounter(&q0);
        if (!r.Capture(c.window)) { DLOG(L"minimize: Genie capture unavailable, using thumbnail bands"); Post(c.window, c.direction, false); return; }
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&f);
        DLOG(L"minimize: Genie captured the window in %.1f ms", (q1.QuadPart - q0.QuadPart) * 1000.0 / f.QuadPart);
        it = r.cache.find(c.window);
    }
    if (!r.Begin(g_notify)) { Post(c.window, c.direction, false); return; }
    const Texture& tex = it->second;
    RECT tile = Tile(c.window);
    LARGE_INTEGER start{}, now{}, f{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
    bool cancelled = false; int frames = 0; double x = 0.0;
    for (;;) {
        Pump();
        if (Cancelled(c.window)) { cancelled = true; break; }
        QueryPerformanceCounter(&now);
        x = std::clamp(static_cast<double>(now.QuadPart - start.QuadPart) / f.QuadPart / c.seconds, 0.0, 1.0);
        const RECT latest = Tile(c.window); if (!Empty(latest)) tile = latest;
        const double t = c.direction > 0 ? x : 1.0 - x;
        r.Draw(tex, c.normal, FitTile(tile, tex.size), t);
        ++frames;
        if (x >= 1.0) break;
    }
    const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / f.QuadPart;
    DLOG(L"minimize: Genie %ls %d frames in %.0f ms (%.0f fps)%ls", c.direction > 0 ? L"minimize" : L"restore", frames, elapsed * 1000.0,
         elapsed > 0 ? frames / elapsed : 0.0, cancelled ? L" cancelled" : L"");
    if (!cancelled) {
        Post(c.window, c.direction, true);
        // Hold the last frame while the Dock shows the tile (or the window reappears), then hide the overlay.
        for (int i = 0; i < 3; ++i) { Pump(); r.Draw(tex, c.normal, FitTile(tile, tex.size), c.direction > 0 ? 1.0 : 0.0); }
    }
    r.End();
    if (c.direction < 0) r.cache.erase(c.window);
}

void Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Renderer r;
    bool ready = false;
    try { ready = r.Init(); } catch (...) { ready = false; }
    if (!ready) DLOG(L"minimize: GPU Genie unavailable, using thumbnail bands");
    for (;;) {
        MsgWaitForMultipleObjects(1, &g_wake, FALSE, INFINITE, QS_ALLINPUT);
        Pump();
        for (;;) {
            Command c{};
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_quit) goto done;
                if (g_queue.empty()) break;
                c = g_queue.front(); g_queue.pop_front();
            }
            if (c.kind == Command::Snapshot) { if (ready) r.SnapshotVisible(c.window); continue; }
            if (c.kind == Command::Forget) { r.cache.erase(c.window); r.fresh.erase(c.window); std::lock_guard<std::mutex> lock(g_mutex); g_tiles.erase(c.window); continue; }
            try { RunAnimation(r, ready, c); } catch (...) { r.End(); Post(c.window, c.direction, false); }
        }
    }
done:
    r.Shutdown();
    winrt::uninit_apartment();
}

}  // namespace

bool Start(HWND notify, UINT message) {
    if (g_thread.joinable()) return true;
    g_notify = notify; g_message = message; g_quit = false;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) return false;
    g_thread = std::thread(Run);
    return true;
}

void Stop() {
    if (!g_thread.joinable()) return;
    { std::lock_guard<std::mutex> lock(g_mutex); g_quit = true; }
    SetEvent(g_wake);
    g_thread.join();
    CloseHandle(g_wake); g_wake = nullptr;
    g_queue.clear(); g_tiles.clear();
}

void Animate(HWND window, const RECT& normal, int direction, double seconds) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_queue.push_back(Command{Command::Animate, window, normal, direction, seconds}); }
    SetEvent(g_wake);
}

void SetTile(HWND window, const RECT& tile) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tiles[window] = tile;
}

void Snapshot(HWND window) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& c : g_queue) if (c.kind == Command::Snapshot && c.window == window) return;  // one pending is enough
        g_queue.push_back(Command{Command::Snapshot, window, RECT{}, 0, 0.0});
    }
    SetEvent(g_wake);
}

void Forget(HWND window) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_queue.push_back(Command{Command::Forget, window, RECT{}, 0, 0.0}); }
    SetEvent(g_wake);
}

}  // namespace dock::minimize::gpu
