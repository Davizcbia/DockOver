#pragma once

#include <dxgi1_2.h>
#include <wrl/client.h>

namespace docky {

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
