#pragma once
// SimulatedBackdropSource: a stand-in IBackdropSource until P0.2's WGC capture is integrated.
// It is NOT a live capture. Modes: the desktop wallpaper of the Dock's monitor rendered statically
// (IDesktopWallpaper + WIC, honouring Fill/Fit/Stretch/Center/Tile), or generated test images.
// Images are built on a worker thread; the render thread only uploads finished pixels.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "backdrop.h"
#include "glass_params.h"

namespace dock {

class SimulatedBackdropSource final : public IBackdropSource {
public:
    ~SimulatedBackdropSource() override;
    void SetDevice(ID3D11Device* device);              // call after (re)creating the D3D device
    void Request(BackdropKind kind, HMONITOR monitor);  // no-op if unchanged; async otherwise
    bool Latest(const RECT& wantedScreenRect, BackdropFrame& out) override;
    void SetExcludedWindow(HWND) override {}
    const wchar_t* Name() const override;
    int State() const { return state_.load(); }        // 0 none, 1 loading, 2 ready, 3 failed
    BackdropKind Kind() const { return kind_; }

private:
    struct Pending {
        std::mutex mutex;
        bool ready = false;
        unsigned generation = 0;
        std::vector<uint32_t> pixels;
        UINT width = 0, height = 0;
        RECT rect{};
        bool ok = false;
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    RECT rect_{};
    unsigned long long frameId_ = 0;
    BackdropKind kind_ = BackdropKind::None;
    HMONITOR monitor_ = nullptr;
    unsigned generation_ = 0;
    std::shared_ptr<Pending> pending_ = std::make_shared<Pending>();
    std::vector<uint32_t> lastPixels_;  // kept so a device reset can re-upload without decoding again
    UINT lastW_ = 0, lastH_ = 0;
    std::atomic<int> state_{0};
};

}  // namespace dock
