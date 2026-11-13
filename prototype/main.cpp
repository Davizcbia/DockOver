#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <cstdint>
#include <cmath>

static FILE* logFile = nullptr;
static void logMessage(const char* format, ...) {
    if (!logFile) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(logFile, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fputc(10, logFile);
    fflush(logFile);
}

using Microsoft::WRL::ComPtr;

static void checkResult(HRESULT hr, const char* operation) {
    logMessage("%s HRESULT=0x%08lX", operation, static_cast<unsigned long>(hr));
    if (FAILED(hr)) throw std::runtime_error(operation);
}
#define check(expression) checkResult((expression), #expression)

class Renderer {
    HWND window_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> drawing_;
    ComPtr<ID2D1Bitmap1> bitmap_;
    bool alphaDiagnostic_ = false;

    void showAlphaDiagnostic() {
        ComPtr<ID3D11Texture2D> backBuffer;
        check(swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        check(device_->CreateTexture2D(&desc, nullptr, &staging));
        context_->CopyResource(staging.Get(), backBuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        unsigned counts[256]{};
        unsigned badPremultiplied = 0;
        unsigned outsidePixels = 0;
        unsigned outsideNonzero = 0;
        const float pad = 4.0f * GetDpiForWindow(window_) / 96.0f;
        const float radius = std::max(0.0f, std::min({24.0f * GetDpiForWindow(window_) / 96.0f,
            (desc.Width - 2.0f * pad) / 2.0f, (desc.Height - 2.0f * pad) / 2.0f}));
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                const auto* source = row + 4 * x;
                const auto alpha = source[3];
                ++counts[alpha];
                if (source[0] > alpha || source[1] > alpha || source[2] > alpha)
                    ++badPremultiplied;
                if (radius > 0) {
                    const float px = x + 0.5f;
                    const float py = y + 0.5f;
                    const float nearestX = std::clamp(px, pad + radius, desc.Width - pad - radius);
                    const float nearestY = std::clamp(py, pad + radius, desc.Height - pad - radius);
                    if (std::hypot(px - nearestX, py - nearestY) > radius + 1.0f) {
                        ++outsidePixels;
                        if (alpha != 0) ++outsideNonzero;
                    }
                }
                auto* target = pixels.data() + (static_cast<size_t>(y) * desc.Width + x) * 4;
                target[0] = alpha;
                target[1] = alpha;
                target[2] = alpha;
                target[3] = 255;
            }
        }
        auto alphaAt = [&](UINT x, UINT y) {
            return static_cast<const uint8_t*>(mapped.pData)[y * mapped.RowPitch + x * 4 + 3];
        };
        logMessage("Alpha samples size=%ux%u corners=%u,%u,%u,%u center=%u",
            desc.Width, desc.Height, alphaAt(0, 0), alphaAt(desc.Width - 1, 0),
            alphaAt(0, desc.Height - 1), alphaAt(desc.Width - 1, desc.Height - 1),
            alphaAt(desc.Width / 2, desc.Height / 2));
        for (unsigned i = 0; i < 256; ++i)
            if (counts[i]) logMessage("Alpha histogram %u=%u", i, counts[i]);
        logMessage("Premultiplied violations=%u, definitely outside pixels=%u nonzero=%u",
            badPremultiplied, outsidePixels, outsideNonzero);
        context_->Unmap(staging.Get(), 0);

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> mask;
        check(drawing_->CreateBitmap(D2D1::SizeU(desc.Width, desc.Height), pixels.data(),
            desc.Width * 4, &props, &mask));
        drawing_->BeginDraw();
        drawing_->DrawBitmap(mask.Get(), D2D1::RectF(0, 0, static_cast<float>(desc.Width),
            static_cast<float>(desc.Height)));
        check(drawing_->EndDraw());
    }

    void createTarget() {
        ComPtr<IDXGISurface> surface;
        check(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)));
        D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        check(drawing_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap_));
        drawing_->SetTarget(bitmap_.Get());
    }

public:
    explicit Renderer(HWND window) : window_(window) { initialize(); }
    void toggleAlphaDiagnostic() {
        alphaDiagnostic_ = !alphaDiagnostic_;
        logMessage("Alpha diagnostic %s", alphaDiagnostic_ ? "on" : "off");
        render();
    }

    void initialize() {
        logMessage("Initializing graphics");
        bitmap_.Reset(); drawing_.Reset(); d2dDevice_.Reset(); factory_.Reset();
        visual_.Reset(); target_.Reset(); composition_.Reset(); swap_.Reset();
        context_.Reset(); device_.Reset();

        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device_, nullptr, &context_));
        ComPtr<IDXGIDevice> dxgi;
        check(device_.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> dxgiFactory;
        check(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));
        RECT rect{};
        GetClientRect(window_, &rect);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = std::max<LONG>(1, rect.right);
        desc.Height = std::max<LONG>(1, rect.bottom);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(dxgiFactory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swap_));
        check(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition_)));
        check(composition_->CreateTargetForHwnd(window_, TRUE, &target_));
        check(composition_->CreateVisual(&visual_));
        check(visual_->SetContent(swap_.Get()));
        check(target_->SetRoot(visual_.Get()));
        check(composition_->Commit());
        D2D1_FACTORY_OPTIONS options{};
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &options, reinterpret_cast<void**>(factory_.GetAddressOf())));
        check(factory_->CreateDevice(dxgi.Get(), &d2dDevice_));
        check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &drawing_));
        createTarget();
    }

    void resize() {
        RECT rect{};
        GetClientRect(window_, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return;
        drawing_->SetTarget(nullptr);
        bitmap_.Reset();
        logMessage("Resize requested client=%ldx%ld", rect.right, rect.bottom);
        check(swap_->ResizeBuffers(0, rect.right, rect.bottom, DXGI_FORMAT_UNKNOWN, 0));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        check(swap_->GetDesc1(&desc));
        logMessage("Swap chain size=%ux%u", desc.Width, desc.Height);
        createTarget();
        render();
    }

    void render() {
        D2D1_SIZE_F size = bitmap_->GetSize();
        float scale = GetDpiForWindow(window_) / 96.0f;
        drawing_->BeginDraw();
        drawing_->Clear(D2D1::ColorF(0, 0.0f));
        ComPtr<ID2D1SolidColorBrush> brush;
        check(drawing_->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.7f, 0.85f, 0.48f), &brush));
        float pad = 4 * scale;
        auto shape = D2D1::RoundedRect(D2D1::RectF(pad, pad, size.width-pad, size.height-pad),
            24 * scale, 24 * scale);
        drawing_->FillRoundedRectangle(shape, brush.Get());
        check(drawing_->EndDraw());
        if (alphaDiagnostic_) showAlphaDiagnostic();
        check(swap_->Present(1, 0));
        static bool first = true;
        if (first) { logMessage("First frame presented"); first = false; }
    }
};

static HWND checkerWindow = nullptr;
static LRESULT CALLBACK checkerProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH colors[] = {CreateSolidBrush(RGB(255, 35, 180)), CreateSolidBrush(RGB(0, 230, 80))};
        constexpr int cell = 32;
        for (int y = 0; y < client.bottom; y += cell) {
            for (int x = 0; x < client.right; x += cell) {
                RECT square{x, y, x + cell, y + cell};
                FillRect(dc, &square, colors[((x / cell) + (y / cell)) % 2]);
            }
        }
        DeleteObject(colors[0]);
        DeleteObject(colors[1]);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static Renderer* renderer = nullptr;
static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
        case WM_CLOSE:
            logMessage("WM_CLOSE");
            break;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            RECT bounds{};
            GetWindowRect(hwnd, &bounds);
            const int border = MulDiv(10, GetDpiForWindow(hwnd), 96);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const bool left = x < bounds.left + border;
            const bool right = x >= bounds.right - border;
            const bool top = y < bounds.top + border;
            const bool bottom = y >= bounds.bottom - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCAPTION;
        }
        case WM_EXITSIZEMOVE: {
            RECT bounds{};
            GetWindowRect(hwnd, &bounds);
            logMessage("End move/resize bounds=%ld,%ld,%ld,%ld", bounds.left, bounds.top,
                bounds.right, bounds.bottom);
            return 0;
        }
        case WM_SIZE:
            if (renderer && wp != SIZE_MINIMIZED) renderer->resize();
            return 0;
        case WM_DPICHANGED: {
            logMessage("WM_DPICHANGED dpi=%u,%u", LOWORD(wp), HIWORD(wp));
            auto rect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right-rect->left,
                rect->bottom-rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            try { if (renderer) renderer->render(); }
            catch (...) { EndPaint(hwnd, &paint); throw; }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            if (wp == VK_F1 && renderer) renderer->toggleAlphaDiagnostic();
            if (wp == VK_F2) {
                if (checkerWindow) {
                    DestroyWindow(checkerWindow);
                    checkerWindow = nullptr;
                } else {
                    WNDCLASSW checkerClass{};
                    checkerClass.hInstance = GetModuleHandleW(nullptr);
                    checkerClass.lpfnWndProc = checkerProcedure;
                    checkerClass.lpszClassName = L"DockAlphaChecker";
                    RegisterClassW(&checkerClass);
                    RECT bounds{};
                    GetWindowRect(hwnd, &bounds);
                    checkerWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        checkerClass.lpszClassName, L"Alpha checkerboard", WS_POPUP,
                        bounds.left - 60, bounds.top - 60, bounds.right - bounds.left + 120,
                        bounds.bottom - bounds.top + 120, nullptr, nullptr,
                        checkerClass.hInstance, nullptr);
                    if (checkerWindow) {
                        ShowWindow(checkerWindow, SW_SHOWNOACTIVATE);
                        SetWindowPos(checkerWindow, hwnd, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            }
            return 0;
        case WM_DESTROY:
            logMessage("WM_DESTROY");
            if (checkerWindow) { DestroyWindow(checkerWindow); checkerWindow = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& e) {
        logMessage("Window handler failure: %s", e.what());
        try {
            if (renderer) { renderer->initialize(); renderer->render(); }
        } catch (...) {
            MessageBoxW(hwnd, L"Graphics initialization or recovery failed.", L"Dock prototype", MB_ICONERROR);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    wchar_t logPath[32768]{};
    DWORD length = GetEnvironmentVariableW(L"DOCK_DIAGNOSTIC_LOG", logPath, 32768);
    if (length && length < 32768) _wfopen_s(&logFile, logPath, L"a");
    logMessage("Process started pid=%lu", GetCurrentProcessId());
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        logMessage("DPI awareness failed error=%lu", GetLastError());
    logMessage("Per-monitor V2 active=%d", AreDpiAwarenessContextsEqual(
        GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = procedure;
    wc.lpszClassName = L"DockPrototypeWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) { logMessage("RegisterClass failed error=%lu", GetLastError()); return 1; }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Dock prototype",
        WS_POPUP | WS_THICKFRAME, work.left + 40, work.top + 40,
        std::min<LONG>(640, work.right - work.left), std::min<LONG>(120, work.bottom - work.top),
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) { logMessage("CreateWindow failed error=%lu", GetLastError()); return 1; }
    logMessage("HWND created=%p", hwnd);
    try {
        Renderer graphics(hwnd);
        renderer = &graphics;
        ShowWindow(hwnd, SW_SHOW);
        RECT bounds{}; GetWindowRect(hwnd, &bounds);
        logMessage("Window visible=%d bounds=%ld,%ld,%ld,%ld dpi=%u", IsWindowVisible(hwnd),
            bounds.left, bounds.top, bounds.right, bounds.bottom, GetDpiForWindow(hwnd));
        graphics.render();
        MSG msg{};
        int result;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        logMessage("Message loop ended result=%d quit_code=%llu error=%lu", result,
            static_cast<unsigned long long>(msg.wParam), result == -1 ? GetLastError() : 0);
        renderer = nullptr;
        if (result == -1) { logMessage("Process exit=1"); return 1; }
    } catch (const std::exception& e) {
        logMessage("Fatal: %s", e.what());
        MessageBoxW(hwnd, L"Unable to initialize graphics.", L"Dock prototype", MB_ICONERROR);
        DestroyWindow(hwnd);
        logMessage("Process exit=1");
        return 1;
    }
    logMessage("Process exit=0");
    if (logFile) fclose(logFile);
    return 0;
}
