#include "design_tokens.h"
#include "menu_surface.h"
#include "log.h"
namespace liquid::popup {
namespace { StaticBackdrop& Provider(){static StaticBackdrop p;return p;} }
void SetStaticBackdrop(StaticBackdrop provider){Provider()=std::move(provider);}
namespace { LiveBackdrop& Live(){static LiveBackdrop p;return p;} }
void SetLiveBackdrop(LiveBackdrop provider){Live()=std::move(provider);}
namespace { Microsoft::WRL::ComPtr<ID3D11Device>& RenderDevice(){static Microsoft::WRL::ComPtr<ID3D11Device> d;return d;} }
void SetRenderDevice(ID3D11Device* device){RenderDevice()=device;}
const LiveBackdrop& GetLiveBackdrop(){return Live();}
const StaticBackdrop& GetStaticBackdrop(){return Provider();}
bool UploadBackdrop(ID3D11Device* device,const RECT& screen,const std::vector<std::uint32_t>& bgra,Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,std::uint64_t& frameId,dock::BackdropFrame& out){
 const UINT w=static_cast<UINT>(screen.right-screen.left),h=static_cast<UINT>(screen.bottom-screen.top);
 if(!device||!w||!h||bgra.size()!=static_cast<size_t>(w)*h)return false;
 D3D11_TEXTURE2D_DESC have{};if(texture)texture->GetDesc(&have);
 if(!texture||have.Width!=w||have.Height!=h){
  texture.Reset();D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{bgra.data(),w*4,0};if(FAILED(device->CreateTexture2D(&d,&init,&texture)))return false;
 }else{Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);ctx->UpdateSubresource(texture.Get(),0,nullptr,bgra.data(),w*4,0);}
 out.texture=texture.Get();out.screenRect=screen;out.frameId=++frameId;out.captureTimeSeconds=0;return true;
}
MenuSurface::~MenuSurface(){backdrop_.SetDevice(nullptr);renderer_.Shutdown();}
bool MenuSurface::Init(HWND window,UINT width,UINT height,float dpi){
 window_=window;width_=width;height_=height;dpi_=dpi;
 if(RenderDevice())renderer_.UseDevice(RenderDevice().Get());
 const HRESULT hr=renderer_.Init(window,width,height,dpi);if(FAILED(hr)){DLOG(L"menu surface: D3D/DComp init failed 0x%08X",hr);return false;}
#ifdef LIQUID_MENU_DIAGNOSTICS
 wchar_t noCompare[4]{};if(GetEnvironmentVariableW(L"LIQUID_TEST_DISABLE_MENU_COMPARE",noCompare,ARRAYSIZE(noCompare))>0)return true;
#endif
 backdrop_.SetDevice(renderer_.Device());backdrop_.SetExcludedWindow(window);return true;
}
void MenuSurface::Start(){
#ifdef LIQUID_MENU_DIAGNOSTICS
 wchar_t disabled[4]{};if(GetEnvironmentVariableW(L"LIQUID_TEST_DISABLE_MENU_WGC",disabled,ARRAYSIZE(disabled))>0)return;
#endif
 backdropSeen_=false;staticValid_=false;
 if(GetStaticBackdrop())return;  // no capture at all
 backdrop_.Request(!captureDisabled_,MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST));
}
void MenuSurface::Suspend(){backdrop_.Request(false,nullptr);
 if(const auto& live=GetLiveBackdrop()){dock::BackdropFrame none;bool changed=false;live(renderer_.Device(),RECT{},RECT{},false,none,changed);}}
bool MenuSurface::PollLive(){
 const auto& live=GetLiveBackdrop();RECT rect{};if(!live||opaque_||!GetWindowRect(window_,&rect))return false;
 const LONG m=static_cast<LONG>(96.f*dpi_/96.f);const RECT want{rect.left-m,rect.top-m,rect.right+m,rect.bottom+m};
 dock::BackdropFrame frame;bool changed=false;return live(renderer_.Device(),want,rect,true,frame,changed)&&changed;
}
bool MenuSurface::Resize(UINT width,UINT height,float dpi){
 if(width==width_&&height==height_&&dpi==dpi_)return true;
 const HRESULT hr=renderer_.Resize(width,height,dpi);if(FAILED(hr)){DLOG(L"menu surface: resize failed 0x%08X",hr);return false;}
 width_=width;height_=height;dpi_=dpi;return true;
}
bool MenuSurface::Draw(const std::vector<dock::MenuRowVisual>& rows){
 RECT rect{};if(!GetWindowRect(window_,&rect))return false;
 dock::BackdropFrame frame;bool have=false;
 dock::BackdropFrame liveFrame;bool liveChanged=false;
 if(const auto& live=GetLiveBackdrop();live&&live(renderer_.Device(),RECT{rect.left-static_cast<LONG>(dpi_),rect.top-static_cast<LONG>(dpi_),rect.right+static_cast<LONG>(dpi_),rect.bottom+static_cast<LONG>(dpi_)},rect,false,liveFrame,liveChanged)){
  frame=liveFrame;have=true;  // live desktop duplication (follows video wallpapers)
 }else if(const auto& provider=GetStaticBackdrop()){  // capture-free backdrop: composed once per window position
  const LONG m=static_cast<LONG>(96.f*dpi_/96.f);RECT want{rect.left-m,rect.top-m,rect.right+m,rect.bottom+m};
  if(!staticValid_||!EqualRect(&want,&staticRect_)){std::vector<std::uint32_t> px;staticValid_=provider(want,px)&&UploadBackdrop(renderer_.Device(),want,px,staticTexture_,staticId_,staticFrame_);staticRect_=want;}
  have=staticValid_;if(have)frame=staticFrame_;
 }else have=backdrop_.Latest(rect,frame);
 dock::DockFrame geometry;const float scale=dpi_/96.0f;geometry.dockLeft=0;geometry.dockTop=0;geometry.dockRight=width_/scale;geometry.dockBottom=height_/scale;
 dock::DockMetrics metrics;dock::GlassParameters glass=dock::RoleGlass(dock::GlassRole::Menu);
 dock::Materialize(glass,materialize_);glass.opacity=1.f;  // coverage is the visual's opacity, which also carries the text
 dock::Scene scene;scene.frame=&geometry;scene.metrics=&metrics;scene.menuMode=true;scene.menuRows=rows;scene.windowW=geometry.dockRight;scene.windowH=geometry.dockBottom;scene.windowScreenRect=rect;scene.glass=&glass;scene.opaqueFallback=opaque_;scene.opaqueBackground=background_;scene.opaqueForeground=foreground_;scene.backdrop=have&&!opaque_?&frame:nullptr;
 const HRESULT hr=renderer_.Render(scene);if(FAILED(hr)){DLOG(L"menu surface: render failed 0x%08X",hr);return false;}
 if(have&&!opaque_&&renderer_.LastUsedGpu())backdropSeen_=true;
 DLOG_DEBUG(L"menu surface: rendered material=%ls backdrop=%d",renderer_.LastUsedGpu()?L"liquid":L"smoked",have);  // per frame: debug only
 return true;
}
bool MenuSurface::Capture(){RECT rect{};return GetWindowRect(window_,&rect)&&backdrop_.ProcessCapture(rect);}
bool MenuSurface::Complete(){RECT rect{};return GetWindowRect(window_,&rect)&&backdrop_.CompleteCompare(rect);}
bool MenuSurface::CheckHealth(){return backdrop_.CheckHealth();}
void MenuSurface::Opacity(float value){renderer_.SetVisualOpacity(value);}
void MenuSurface::Materialize(float t,bool material){materialize_=material?t:1.f;renderer_.SetVisualOpacity(dock::MaterializeAlpha(t));}
std::uint64_t MenuSurface::ContentVersion(){RECT rect{};dock::BackdropFrame frame;return GetWindowRect(window_,&rect)&&backdrop_.Latest(rect,frame)?frame.frameId:0;}
}
