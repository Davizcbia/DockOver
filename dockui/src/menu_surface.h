#pragma once
#include "renderer.h"
#include "backdrop_wgc.h"
#include <vector>
#include <functional>
#include <cstdint>
namespace liquid::popup {
// Optional capture-free backdrop for processes that cannot (or must not) use screen capture: the callback composes
// what lies under `screen` (physical px) as straight BGRA, top-down, exactly covering `screen`.
// DockOver Desktop uses it as a fallback until a live capture frame is available.
using StaticBackdrop = std::function<bool(const RECT& screen, std::vector<std::uint32_t>& bgra)>;
void SetStaticBackdrop(StaticBackdrop provider);
const StaticBackdrop& GetStaticBackdrop();
// Optional device every menu surface renders on (docky Desktop: its shared UI device); null = each creates its own.
void SetRenderDevice(ID3D11Device* device);
// Uploads `bgra` covering `screen` into a cached texture and fills a BackdropFrame (frameId changes with the content).
bool UploadBackdrop(ID3D11Device* device, const RECT& screen, const std::vector<std::uint32_t>& bgra,
                    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, std::uint64_t& frameId, dock::BackdropFrame& out);
class MenuSurface {
public:
 ~MenuSurface();
 bool Init(HWND window,UINT width,UINT height,float dpi);
 void SetVisualCaptureTest(bool enabled){captureDisabled_=enabled;}
 void SetTheme(bool opaque,COLORREF background,COLORREF foreground){opaque_=opaque;background_=background;foreground_=foreground;}
 void Start();
 void Suspend();
 bool Resize(UINT width,UINT height,float dpi);
 // gridColumns > 0 draws `rows` as a grid with `footer` below (dock::kMenuGrid* layout).
 bool Draw(const std::vector<dock::MenuRowVisual>& rows,int gridColumns=0,const std::wstring& footer={});
 // The same menu glass (Glass Studio "Menu" category) with caller-drawn content in DIPs instead of rows, and the
 // plate's corner radius in DIPs (the window preview: a larger panel than a menu, its cards drawn concentric to it).
 bool DrawCustom(float radius,const std::function<void(ID2D1DeviceContext*,bool)>& content);
 bool Capture();
 bool Complete();
 bool CheckHealth();
 HANDLE CompletionEvent() const {return backdrop_.CompletionEvent();}
 void Opacity(float value);
 // t in 0..1. Sets the surface coverage; with `material` the glass also forms (blur and refraction
 // grow with t) and the caller redraws. Without it the formed surface only cross-fades.
 void Materialize(float t,bool material);
 // True once a frame was drawn with a captured backdrop (or glass is not wanted: opaque theme or
 // capture disabled). The popup keeps coverage at 0 until then so no fallback frame is ever shown.
 bool GlassReady() const {return backdropSeen_||opaque_||captureDisabled_;}
 void RequestSnapshot(const std::wstring& path){renderer_.RequestSnapshot(path);}
 bool UsedGpu() const{return renderer_.LastUsedGpu();}
 std::uint64_t ContentVersion();
private:
 bool Render(const std::vector<dock::MenuRowVisual>& rows,int gridColumns,const std::wstring& footer,float radius,
             const std::function<void(ID2D1DeviceContext*,bool)>* content);
 HWND window_=nullptr;UINT width_=0,height_=0;float dpi_=96;
 bool captureDisabled_=false;float materialize_=1.f;bool backdropSeen_=false;
 ULONGLONG startedAt_=0;  // Start(): the static (wallpaper-only) fallback waits a moment for the live capture
 bool opaque_=false;COLORREF background_=RGB(39,43,52),foreground_=RGB(247,248,250);
 dock::Renderer renderer_;dock::WgcBackdropSource backdrop_;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> staticTexture_;RECT staticRect_{};std::uint64_t staticId_=0;dock::BackdropFrame staticFrame_{};bool staticValid_=false;
};
}
