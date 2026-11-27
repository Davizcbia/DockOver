#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include "desktop_settings.h"
#include <wrl/client.h>

namespace docky {

inline std::wstring& RenderSettingsPath() {
    static std::wstring path = liquid::settings::DefaultPath();
    return path;
}

inline void SetRenderSettingsPath(const std::wstring& path) {
    RenderSettingsPath() = path;
}

inline bool PreferDiscreteRenderGpu() {
    static const bool discrete = liquid::settings::Load(RenderSettingsPath()).value.performance.useDiscreteGpu;
    return discrete;
}

inline bool UsesUnifiedMemory(ID3D11Device* device) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
    return device && SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options))) &&
           options.UnifiedMemoryArchitecture;
}

inline HRESULT CreateRenderDevice(UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                  ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) {
    using Microsoft::WRL::ComPtr;
    // Keep the same preference for every render device in this process until it restarts.
    const bool discrete = PreferDiscreteRenderGpu();
    ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        const auto preference = discrete ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_MINIMUM_POWER;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapterByGpuPreference(i, preference, IID_PPV_ARGS(&adapter)))) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                                 levelCount, D3D11_SDK_VERSION, device, featureLevel, context);
            if (SUCCEEDED(hr)) return hr;
        }
    }
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                         levelCount, D3D11_SDK_VERSION, device, featureLevel, context);
    if (SUCCEEDED(hr)) return hr;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, levelCount,
                             D3D11_SDK_VERSION, device, featureLevel, context);
}

// Desktop Duplication must create its capture device on the adapter that owns the output.
inline Microsoft::WRL::ComPtr<IDXGIAdapter1> DisplayAdapter(HMONITOR monitor) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, &adapter))) break;
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(j, &output))) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return adapter;
        }
    }
    return {};
}

}  // namespace docky
