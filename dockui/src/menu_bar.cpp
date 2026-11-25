#include "game_compat.h"
#include "ui_language.h"
#include "capture_visible.h"
#include "status_glyphs.h"
#include "status_popups.h"
#include "status_hosts.h"
#include "battery_indicator.h"
#include "desktop_settings.h"
#include "theme_state.h"
#include <winrt/Windows.UI.ViewManagement.h>
#include "liquid_build_id.h"
static volatile const char liquidBuildMarker[] = LIQUID_BUILD_MARKER;
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <roapi.h>
#include <windowsx.h>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>
#include <objidl.h>
namespace Gdiplus{using std::min;using std::max;}
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <fstream>
#include "log.h"
#include "menu_model.h"
#include "native_menu.h"
#include "system_status.h"
#include "unified_menu.h"
#include "control_center.h"
#include "notification_center.h"
#include "design_tokens.h"
#include "glass_params.h"
#include "workshop.h"
#include "custom_icons.h"
#include "user_sound_icons.h"
#include "backdrop_wgc.h"
#include "vblank.h"
#include <map>
#include "menu_hit_region.h"
#include "menubar_extras.h"
#include "tray_host.h"
#include "ext_protocol.h"

namespace {
constexpr wchar_t kClass[] = L"LiquidDesktopMenuBarV01";
std::wstring appName, clockText;
liquid::settings::Revision settingsRevision;
std::wstring themeSettingsPath;
bool SamePath(const std::wstring& a,const std::wstring& b){
 wchar_t x[32768]{},y[32768]{};const DWORD nx=GetFullPathNameW(a.c_str(),static_cast<DWORD>(std::size(x)),x,nullptr),ny=GetFullPathNameW(b.c_str(),static_cast<DWORD>(std::size(y)),y,nullptr);
 if(!nx||!ny||nx>=std::size(x)||ny>=std::size(y))return true;return _wcsicmp(x,y)==0;
}
winrt::Windows::UI::ViewManagement::UISettings themeUi{nullptr};
int themeMode=0;bool themeDark=true,themeContrast=false;COLORREF themeInk=RGB(255,255,255);
// Adaptive bar ink (macOS): over a light backdrop the bar's text and icons turn black (same shapes, no halo or shadow) and
// the selection pill turns grey. Only the bar itself; Control Center and the popups keep the appearance setting.
bool barLight=false, themeGradient=true;int gradientOpacity=90, gradientLength=100;int barStyleForce=-1;  // dev: --dev-bar-style light|dark
constexpr UINT_PTR kBackdropTimer=17;
bool UseThemeGradient(){return themeGradient && !themeContrast;}
bool LightBar(){
    if (themeContrast) return false;
    if (barStyleForce >= 0) return barStyleForce == 1;
    return UseThemeGradient() ? !themeDark : barLight;
}
COLORREF BarInk(){return themeContrast ? themeInk : LightBar() ? RGB(0,0,0) : RGB(255,255,255);}
// Median luminance of what is under the bar. The bar is a top AppBar, so that is the wallpaper (or Wallpaper Engine).
// BitBlt without CAPTUREBLT leaves layered windows out; if the compositor still returns the bar, its sparse glyphs do not
// move the median. Hysteresis keeps the ink from flickering on mid-grey wallpapers.
bool SampleBarBackdrop(HWND h){
 bool next=barLight;
 if(UseThemeGradient() || barStyleForce>=0)return false;
 if(themeContrast)next=false;
 else{
  RECT r{};if(!IsWindowVisible(h)||!GetWindowRect(h,&r))return false;
  // Use the status side where the densely packed small glyphs need contrast.
  r.left += (r.right-r.left)/2;
  const int w=r.right-r.left,hh=r.bottom-r.top;if(w<=0||hh<=0)return false;
  HDC screen=GetDC(nullptr);if(!screen)return false;HDC mem=CreateCompatibleDC(screen);
  BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-hh;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  void* bits=nullptr;HBITMAP bmp=mem?CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;bool got=false;unsigned hist[256]{};size_t n=0;
  if(bmp&&bits){HGDIOBJ old=SelectObject(mem,bmp);got=BitBlt(mem,0,0,w,hh,screen,r.left,r.top,SRCCOPY)!=FALSE;SelectObject(mem,old);
   if(got){const auto* px=static_cast<const std::uint32_t*>(bits);
    for(int y=0;y<hh;y+=2)for(int x=0;x<w;x+=2){const auto v=px[static_cast<size_t>(y)*w+x];
     const unsigned l=(((v>>16)&255u)*54u+((v>>8)&255u)*183u+(v&255u)*19u)>>8;++hist[(std::min)(255u,l)];++n;}}}
  if(bmp)DeleteObject(bmp);if(mem)DeleteDC(mem);ReleaseDC(nullptr,screen);
  if(!got||!n)return false;
  size_t acc=0;int median=0;for(int i=0;i<256;++i){acc+=hist[i];if(acc*2>=n){median=i;break;}}
  if(!barLight&&median>=184)next=true;else if(barLight&&median<=166)next=false;
  static int logged=-1000;if(std::abs(median-logged)>=12||next!=barLight){logged=median;DLOG(L"menu: backdrop median luminance=%d light=%d",median,next?1:0);}
 }
 if(next==barLight)return false;barLight=next;return true;
}
bool RefreshTheme(){
 const auto settings=liquid::settings::Load(themeSettingsPath).value;
 const int mode=settings.appearance.mode;
 const bool gradientChanged=themeGradient!=settings.menuBar.themeGradient ||
     gradientOpacity!=settings.menuBar.gradientOpacity || gradientLength!=settings.menuBar.gradientLength;
 gradientOpacity=settings.menuBar.gradientOpacity;
 gradientLength=settings.menuBar.gradientLength;
 themeGradient=settings.menuBar.themeGradient;
 bool systemDark=true;try{if(themeUi){auto c=themeUi.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);systemDark=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000;}}catch(...){}
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 const bool dark=liquid::theme::IsDark(mode,systemDark);const auto palette=liquid::theme::Palette(mode,systemDark,contrast);
 const COLORREF ink=liquid::theme::ColorRef(palette.primary);const bool changed=themeMode!=mode||themeDark!=dark||themeContrast!=contrast||themeInk!=ink;
 themeMode=mode;themeDark=dark;themeContrast=contrast;themeInk=ink;
 if(changed)liquid::popup::SetTheme(contrast||!dark,liquid::theme::ColorRef(palette.surface),ink);return changed||gradientChanged;
}
UINT dpi=96;
int menuScalePercent=100;
liquid::menu::MenuBar menus;
HWND popup=nullptr,barWindow=nullptr,lastForeground=nullptr,inputWindow=nullptr;
bool tracking=false,inputTracking=false;
std::vector<RECT> inputSlots;int inputWidth=0,inputHeight=0;UINT inputDpi=0;
int hoverIndex=-1,activeMenu=-1;
std::vector<RECT> headings;
// Status slots: 0 Wi-Fi, 1 sound, 2 battery, 3 Control Center, 4 clock, 5 Bluetooth, 6 input menu (keyboard layout).
constexpr int kStatusSlots=7;
RECT statusRects[kStatusSlots]{}, appRect{};int statusHover=-1;
enum class ExtraKind{Chevron,Tray,Ext};
struct Extra{ExtraKind kind=ExtraKind::Chevron;std::wstring id,title;std::uint64_t serial=0;size_t ext=0;RECT rect{};int contentW=0;bool templ=false;bool hasIcon=false;};
std::vector<Extra> extras;
bool appHover=false;
std::vector<float> headingOpacity;
float statusOpacity[kStatusSlots]{},appOpacity=0.f;
ULONGLONG hoverTime=0;
// Optional development trace: only an explicitly supplied, newly created local file.
HANDLE devTrace=INVALID_HANDLE_VALUE;
int devInitialPopup=-1, devInitialKey=0, devNextPopup=-1, devRoute=-1;
int panelAnchor=-1, pendingPanel=-1;
std::string devLast;
std::wstring devPngPath, devPopupBmpPath;
bool devSnapshotRequested=false;
// Same straight-alpha PNG strategy as the Dock's DComp snapshot; this captures only
// our own layered-window pixels, never the desktop or a composited screen image.
bool SaveDevPng(const std::uint32_t* pixels,UINT width,UINT height,const std::wstring& path){
 using Microsoft::WRL::ComPtr;
 if(path.empty()||!pixels||!width||!height||static_cast<unsigned long long>(width)*height>100000000ull)return false;
 std::vector<BYTE> straight(static_cast<size_t>(width)*height*4);
 for(size_t i=0;i<static_cast<size_t>(width)*height;++i){const auto v=pixels[i];const unsigned a=v>>24;straight[i*4+3]=static_cast<BYTE>(a);
  for(int c=0;c<3;++c){const unsigned premul=(v>>(c*8))&255u;straight[i*4+c]=static_cast<BYTE>(a?std::min(255u,(premul*255u+a/2)/a):0u);}}
 ComPtr<IWICImagingFactory> f;ComPtr<IWICBitmapEncoder> e;ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IStream> stream;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f));
 if(SUCCEEDED(hr))hr=CreateStreamOnHGlobal(nullptr,TRUE,&stream);if(SUCCEEDED(hr))hr=f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&e);
 if(SUCCEEDED(hr))hr=e->Initialize(stream.Get(),WICBitmapEncoderNoCache);if(SUCCEEDED(hr))hr=e->CreateNewFrame(&frame,nullptr);
 if(SUCCEEDED(hr))hr=frame->Initialize(nullptr);if(SUCCEEDED(hr))hr=frame->SetSize(width,height);WICPixelFormatGUID fmt=GUID_WICPixelFormat32bppBGRA;
 if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&fmt);if(SUCCEEDED(hr)&&!IsEqualGUID(fmt,GUID_WICPixelFormat32bppBGRA))hr=E_FAIL;
 if(SUCCEEDED(hr))hr=frame->WritePixels(height,width*4,static_cast<UINT>(straight.size()),straight.data());
 if(SUCCEEDED(hr))hr=frame->Commit();if(SUCCEEDED(hr))hr=e->Commit();STATSTG stat{};HGLOBAL memory=nullptr;
 if(SUCCEEDED(hr))hr=stream->Stat(&stat,STATFLAG_NONAME);if(SUCCEEDED(hr))hr=GetHGlobalFromStream(stream.Get(),&memory);
 if(FAILED(hr)||stat.cbSize.HighPart)return false;HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;void* data=GlobalLock(memory);DWORD written=0;const bool ok=data&&WriteFile(file,data,stat.cbSize.LowPart,&written,nullptr)&&written==stat.cbSize.LowPart;
 if(data)GlobalUnlock(memory);CloseHandle(file);return ok;
}
void TraceState(){
 if(devTrace==INVALID_HANDLE_VALUE)return;
 char buf[1024]{};int n=std::snprintf(buf,sizeof(buf),
  "hover_heading=%d hover_status=%d hover_app=%d active_menu=%d popup=0x%llx popup_row=%d foreground=0x%llx cc_open=%d panel_anchor=%d logo=%ld,%ld,%ld,%ld app=%ld,%ld,%ld,%ld wifi=%ld,%ld,%ld,%ld sound=%ld,%ld,%ld,%ld battery=%ld,%ld,%ld,%ld control=%ld,%ld,%ld,%ld clock=%ld,%ld,%ld,%ld\n",
  hoverIndex,statusHover,appHover?1:0,activeMenu,static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(popup)),liquid::popup::HighlightedRow(popup),static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(GetForegroundWindow())),liquid::cc::IsOpen()?1:0,panelAnchor,
  headings.empty()?0:headings[0].left,headings.empty()?0:headings[0].top,headings.empty()?0:headings[0].right,headings.empty()?0:headings[0].bottom,
  appRect.left,appRect.top,appRect.right,appRect.bottom,
  statusRects[0].left,statusRects[0].top,statusRects[0].right,statusRects[0].bottom,
  statusRects[1].left,statusRects[1].top,statusRects[1].right,statusRects[1].bottom,
  statusRects[2].left,statusRects[2].top,statusRects[2].right,statusRects[2].bottom,
  statusRects[3].left,statusRects[3].top,statusRects[3].right,statusRects[3].bottom,
  statusRects[4].left,statusRects[4].top,statusRects[4].right,statusRects[4].bottom);
 if(n<=0||static_cast<size_t>(n)>=sizeof(buf))return;std::string line(buf,static_cast<size_t>(n));if(line==devLast)return;devLast=line;
 DWORD written=0;WriteFile(devTrace,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);FlushFileBuffers(devTrace);
}
constexpr UINT_PTR kHoverTimer=5;
constexpr wchar_t kInputClass[]=L"dockyMenuBarInputV13";
LRESULT CALLBACK InputProc(HWND h,UINT msg,WPARAM w,LPARAM l){
 if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
 if(msg==WM_ERASEBKGND)return 1;
 if(msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP||msg==WM_LBUTTONDOWN||msg==WM_RBUTTONUP||msg==WM_RBUTTONDOWN){
  if(msg==WM_MOUSEMOVE&&!inputTracking){TRACKMOUSEEVENT ev{sizeof(ev),TME_LEAVE,h,0};TrackMouseEvent(&ev);inputTracking=true;}
  if(barWindow&&IsWindow(barWindow)){POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&pt);ScreenToClient(barWindow,&pt);return SendMessageW(barWindow,msg,w,MAKELPARAM(pt.x,pt.y));}
  return 0;
 }
 if(msg==WM_MOUSELEAVE){inputTracking=false;if(barWindow&&IsWindow(barWindow))SendMessageW(barWindow,WM_MOUSELEAVE,0,0);return 0;}
 return DefWindowProcW(h,msg,w,l);
}
void SyncInputWindow(HWND bar,int width,int height){
 if(!inputWindow)return;
 std::vector<RECT> slots=headings;slots.push_back(appRect);for(const auto& r:statusRects)if(r.right>r.left)slots.push_back(r);for(const auto& e:extras)slots.push_back(e.rect);
 bool changed=inputWidth!=width||inputHeight!=height||inputDpi!=dpi||slots.size()!=inputSlots.size();
 if(!changed)for(size_t i=0;i<slots.size();++i)if(!EqualRect(&slots[i],&inputSlots[i])){changed=true;break;}
 if(changed){HRGN region=docky::menuhit::CreateRegion(slots,width,height,dpi);
  if(!region||!SetWindowRgn(inputWindow,region,FALSE)){
   if(region)DeleteObject(region);DLOG(L"menu input: region failed, reverting to layered hit testing");HWND old=inputWindow;inputWindow=nullptr;DestroyWindow(old);
   const LONG_PTR ex=GetWindowLongPtrW(bar,GWL_EXSTYLE);SetWindowLongPtrW(bar,GWL_EXSTYLE,ex&~WS_EX_TRANSPARENT);return;
  }
  inputSlots=std::move(slots);inputWidth=width;inputHeight=height;inputDpi=dpi;
 }
 RECT wr{},ir{};if(GetWindowRect(bar,&wr)){
  const bool moved=!GetWindowRect(inputWindow,&ir)||ir.left!=wr.left||ir.top!=wr.top||ir.right-ir.left!=width||ir.bottom-ir.top!=height;
  if(moved||!IsWindowVisible(inputWindow))SetWindowPos(inputWindow,HWND_TOPMOST,wr.left,wr.top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
 }
}

struct StatusShared {std::mutex mutex;HWND window=nullptr;bool busy=false;liquid::status::Snapshot result;};
std::shared_ptr<StatusShared> statusShared=std::make_shared<StatusShared>();
liquid::status::Snapshot systemStatus;
struct WorkshopImage {UINT w=0,h=0;std::vector<std::uint32_t> pixels;
 mutable int mono=-1;  // 1 = grey-scale template art (follows the bar ink), 0 = coloured, -1 = not checked yet
 mutable int cacheSide=0,cacheW=0,cacheH=0;mutable float opticalX=0,opticalY=0;mutable std::vector<std::uint32_t> cache;
 bool operator==(const WorkshopImage& x)const{return w==x.w&&h==x.h&&pixels==x.pixels;}};
using WorkshopImages=std::map<std::string,WorkshopImage>;
struct WorkshopShared {std::mutex mutex;HWND window=nullptr;bool busy=false,retry=false;WorkshopImages images;};
std::shared_ptr<WorkshopShared> workshopShared=std::make_shared<WorkshopShared>();
WorkshopImages workshopImages;std::wstring workshopRoot;liquid::settings::Revision workshopRevision;
constexpr UINT kWorkshopReady=WM_APP+61;
bool HasWorkshop(const char* key){return workshopImages.find(key)!=workshopImages.end();}
void RequestWorkshop(){auto shared=workshopShared;const auto root=workshopRoot;{std::lock_guard<std::mutex> l(shared->mutex);if(!shared->window)return;if(shared->busy){shared->retry=true;return;}shared->busy=true;}
 std::thread([shared,root]{const HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);WorkshopImages images;docky::workshop::Pack pack;bool enabled=false;std::wstring error;
  if(docky::workshop::Active(root,pack,enabled,error)&&enabled)for(const auto& [key,path]:pack.icons){WorkshopImage image;std::wstring why;if(dock::LoadCustomIconPixels(path,image.pixels,image.w,image.h,why))images.emplace(key,std::move(image));}
  for(const auto& entry:docky::usersound::entries){WorkshopImage image;if(docky::usersound::Load(entry.file,image.pixels,image.w,image.h))images[entry.key]=std::move(image);}
  {std::lock_guard<std::mutex> l(shared->mutex);shared->images=std::move(images);shared->busy=false;const bool retry=shared->retry;shared->retry=false;if(shared->window)PostMessageW(shared->window,kWorkshopReady,retry?1:0,0);}if(SUCCEEDED(co))CoUninitialize();}).detach();
}
const char* WifiKey(){return systemStatus.wifi==liquid::status::Wifi::Connected?"wifi.connected":systemStatus.wifi==liquid::status::Wifi::Disconnected?"wifi.disconnected":"wifi.unavailable";}
const char* WifiArtKey(){const auto key=WifiKey();return HasWorkshop(key)?key:HasWorkshop("wifi.connected")?"wifi.connected":key;}
const char* SoundKey(){return !systemStatus.audioAvailable?"sound.unavailable":systemStatus.muted?"sound.muted":systemStatus.volumePercent<34?"sound.low":systemStatus.volumePercent<67?"sound.medium":"sound.high";}
// Crop only transparent padding and cache a supersampled, premultiplied icon at its
// actual display size. Different packs keep their original colours and aspect ratios.
void CompositeImage(std::uint32_t* dest,int width,int height,const WorkshopImage& src,float cx,float cy,int side,const COLORREF* tint=nullptr){
 if(side<=0)return;
 if(!src.w||!src.h||src.pixels.size()!=static_cast<size_t>(src.w)*src.h)return;
 if(src.cacheSide!=side){
  int x0=static_cast<int>(src.w),y0=static_cast<int>(src.h),x1=-1,y1=-1;unsigned peakAlpha=0;double total=0,weightedX=0,weightedY=0;
  for(UINT y=0;y<src.h;++y)for(UINT x=0;x<src.w;++x){const unsigned a=src.pixels[static_cast<size_t>(y)*src.w+x]>>24;peakAlpha=(std::max)(peakAlpha,a);if(a>8){total+=a;weightedX+=(x+.5)*a;weightedY+=(y+.5)*a;x0=(std::min)(x0,static_cast<int>(x));y0=(std::min)(y0,static_cast<int>(y));x1=(std::max)(x1,static_cast<int>(x));y1=(std::max)(y1,static_cast<int>(y));}}
  src.cacheSide=side;src.cache.clear();src.cacheW=src.cacheH=0;if(x1<x0||y1<y0)return;
  const int sw=x1-x0+1,sh=y1-y0+1;float scale=side/static_cast<float>(sh);if(sw*scale>side*2.f)scale=side*2.f/sw;
  const int tw=(std::max)(1,static_cast<int>(std::lround(sw*scale))),th=(std::max)(1,static_cast<int>(std::lround(sh*scale)));
  // Centre the outer contour exactly (the selection pill is centred on the same point). The former
  // ink-weight "optical" shift moved glyphs up to ~1.25 px off the pill centre.
  (void)total;(void)weightedX;(void)weightedY;
  src.opticalX=0;src.opticalY=0;
  src.cacheW=tw;src.cacheH=th;src.cache.assign(static_cast<size_t>(tw)*th,0);
  // Exact area-weighted (box) downsampling: every source pixel contributes by the fraction of it that
  // falls inside the target pixel. The former fixed grid of at most 6x6 point samples skipped source
  // pixels at large reductions, which showed as jagged steps on small glyphs such as the Apple logo.
  const double fx=static_cast<double>(sw)/tw,fy=static_cast<double>(sh)/th;
  for(int y=0;y<th;++y){const double sy0=y*fy,sy1=sy0+fy;
   for(int x=0;x<tw;++x){const double sx0=x*fx,sx1=sx0+fx;double sum[4]{},area=0;
    for(int iy=static_cast<int>(sy0);iy<(std::min)(sh,static_cast<int>(std::ceil(sy1)));++iy){const double wy=(std::min)(sy1,iy+1.0)-(std::max)(sy0,static_cast<double>(iy));if(wy<=0)continue;
     for(int ix=static_cast<int>(sx0);ix<(std::min)(sw,static_cast<int>(std::ceil(sx1)));++ix){const double wx=(std::min)(sx1,ix+1.0)-(std::max)(sx0,static_cast<double>(ix));if(wx<=0)continue;
      const double wgt=wx*wy;const auto p=src.pixels[static_cast<size_t>(y0+iy)*src.w+(x0+ix)];for(int c=0;c<4;++c)sum[c]+=((p>>(8*c))&255u)*wgt;area+=wgt;}}
    const unsigned boost=(peakAlpha>=192&&peakAlpha<255)?peakAlpha:255;std::uint32_t result=0;
    for(int c=0;c<4;++c){const unsigned value=area>0?static_cast<unsigned>(sum[c]/area+.5):0u;result|=static_cast<std::uint32_t>((std::min)(255u,(value*255u+boost/2)/boost))<<(8*c);}
    src.cache[static_cast<size_t>(y)*tw+x]=result;}}
 }
 const int left=static_cast<int>(std::lround(cx-src.cacheW*.5f+src.opticalX)),top=static_cast<int>(std::lround(cy-src.cacheH*.5f+src.opticalY));
 for(int y=0;y<src.cacheH;++y){const int dy=top+y;if(dy<0||dy>=height)continue;for(int x=0;x<src.cacheW;++x){const int dx=left+x;if(dx<0||dx>=width)continue;auto p=src.cache[static_cast<size_t>(y)*src.cacheW+x];
   if(tint){const unsigned ta=p>>24;p=(ta<<24)|((GetRValue(*tint)*ta/255u)<<16)|((GetGValue(*tint)*ta/255u)<<8)|(GetBValue(*tint)*ta/255u);}auto& d=dest[static_cast<size_t>(dy)*width+dx];const unsigned inv=255-(p>>24);unsigned o[4]{};for(int c=0;c<4;++c)o[c]=(std::min)(255u,((p>>(8*c))&255u)+((((d>>(8*c))&255u)*inv+127u)/255u));d=o[0]|(o[1]<<8)|(o[2]<<16)|(o[3]<<24);}}
}
bool IsMonochrome(const WorkshopImage& img){
 if(img.mono<0){img.mono=1;for(const auto v:img.pixels){const int a=static_cast<int>(v>>24);if(a<16)continue;const int r=(v>>16)&255,g=(v>>8)&255,b=v&255;
   if((std::max)({r,g,b})-(std::min)({r,g,b})>24){img.mono=0;break;}}}
 return img.mono==1;
}
void CompositeWorkshop(std::uint32_t* dest,int width,int height,const char* key,float cx,float cy,int side){
 auto it=workshopImages.find(key);if(it==workshopImages.end())return;
 const COLORREF ink=BarInk();CompositeImage(dest,width,height,it->second,cx,cy,side,(UseThemeGradient()||LightBar())&&IsMonochrome(it->second)?&ink:nullptr);
}
liquid::menu::Menu openMenu;
// ---- Menu Bar extras V0.1 (menubar_extras.h, tray_host.h) ------------------------------------------------
// Right side, right to left: clock, the built-in items the user keeps (Control Center, battery, Wi-Fi, sound),
// extension status items, promoted tray icons, then the Hidden Icons chevron (only when something is hidden).
docky::extras::Config extrasConfig;std::wstring extrasConfigPath;
std::vector<docky::extras::StatusItem> extItems;     // pushed (protocol) and manifest items
std::vector<docky::tray::Icon> trayIcons;            // latest snapshot from the tray host
HWND extHost=nullptr;bool trayStarted=false;
constexpr UINT kTrayChanged=WM_APP+63,kExtOpenMenu=WM_APP+64;constexpr UINT_PTR kTrayTimer=16;
std::vector<std::pair<std::uint64_t,RECT>> popupAnchors;  // tray rows of the open Hidden Icons popup (screen px)
std::map<std::wstring,WorkshopImage> extraImages;   // keyed by full id (tray keys may repeat: serial appended)
std::wstring ExtraImageKey(const Extra& e){return e.kind==ExtraKind::Tray?e.id+L"@"+std::to_wstring(e.serial):e.id;}
bool ExtShown(size_t i){return docky::extras::IsShown(extrasConfig,L"ext:"+extItems[i].id);}
bool TrayShown(const docky::tray::Icon& icon){return !icon.hidden&&docky::extras::IsShown(extrasConfig,icon.key);}
bool AnythingHidden(){
 for(const auto& t:trayIcons)if(!t.hidden&&!TrayShown(t))return true;
 for(size_t i=0;i<extItems.size();++i)if(!ExtShown(i))return true;
 return false;
}
void SaveExtrasConfig(){if(!docky::extras::SaveConfig(extrasConfigPath,extrasConfig))DLOG(L"menu extras: config not saved");}
void LoadExtImage(docky::extras::StatusItem& item){
 item.icon.reset();if(item.iconPath.empty())return;auto image=std::make_shared<docky::extras::Image>();std::wstring why;
 if(dock::LoadCustomIconPixels(item.iconPath,image->pixels,image->w,image->h,why))item.icon=image;else DLOG(L"menu extras: icon for %ls not loaded (%ls)",item.id.c_str(),why.c_str());
}

constexpr UINT kDismiss=WM_APP+41,kStatusReady=WM_APP+42,kControlCenterChanged=WM_APP+43,kNotificationCenterChanged=WM_APP+46;
constexpr UINT_PTR kDevNotifyTimer=41;
bool devNotification=false,devNotificationCenter=false;  // dev: --dev-notification (sample banner), --dev-notification-center
void RequestStatus(){auto shared=statusShared;{std::lock_guard<std::mutex> lock(shared->mutex);if(shared->busy||!shared->window)return;shared->busy=true;}
 std::thread([shared]{auto value=liquid::status::Query();std::lock_guard<std::mutex> lock(shared->mutex);shared->result=value;shared->busy=false;if(shared->window)PostMessageW(shared->window,kStatusReady,0,0);}).detach();
}

int Px(int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); }
// Executable name of a window's process without ".exe"; DockOver's own processes are all reported as "DockOver".
// Empty when the process cannot be opened.
std::wstring WindowProcessName(HWND w) {
    DWORD pid=0; if(w)GetWindowThreadProcessId(w,&pid);
    HANDLE p=pid?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid):nullptr;
    if(!p)return L"";
    wchar_t path[32768];DWORD n=ARRAYSIZE(path);std::wstring name;
    if(QueryFullProcessImageNameW(p,0,path,&n)) {
        name.assign(path,n);const auto at=name.find_last_of(L"\\/");if(at!=std::wstring::npos)name.erase(0,at+1);
        if(name.size()>4 && _wcsicmp(name.c_str()+name.size()-4,L".exe")==0)name.resize(name.size()-4);
    }
    CloseHandle(p);
    if (_wcsicmp(name.c_str(),L"LiquidDock")==0 || _wcsicmp(name.c_str(),L"LiquidMenuBar")==0 ||
        _wcsicmp(name.c_str(),L"LiquidDesktopManager")==0 || _wcsicmp(name.c_str(),L"LiquidDesktopSettings")==0 ||
        _wcsicmp(name.c_str(),L"DockyDesktop")==0 || _wcsicmp(name.c_str(),L"DockOverDesktop")==0 ||
        _wcsicmp(name.c_str(),L"DockOver")==0) name=L"DockOver";
    return name;
}
std::wstring ForegroundName() {
    std::wstring name=WindowProcessName(GetForegroundWindow());
    if(name.empty())name=L"Desktop";
    return name;
}
// The battery shows the bolt whenever the power adapter is connected (charging or already full), as requested.
bool BatteryBolt(){return systemStatus.charging||systemStatus.powerSource==liquid::status::PowerSource::AC;}
void Render(HWND h);
void ClosePopup();
void Update(HWND h) {
    SYSTEMTIME now{};GetLocalTime(&now);wchar_t date[128]{},time[128]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,&now,L"ddd MMM d",date,ARRAYSIZE(date),nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,0,&now,L"h:mm tt",time,ARRAYSIZE(time));
    HWND foreground=GetForegroundWindow();
    // A keyboard-active popup is ours; keep the originating app and its native menus.
    if(popup && foreground==popup)foreground=lastForeground;
    std::wstring next=std::wstring(date)+L"  "+time, app=ForegroundName();
    if(next!=clockText||app!=appName||foreground!=lastForeground){if(popup&&foreground!=lastForeground)ClosePopup();lastForeground=foreground;clockText=next;appName=app;menus=liquid::menu::Build(foreground);auto native=liquid::menu::ReadNativeMenus(foreground);for(auto& entry:native)menus.menus.push_back(std::move(entry));Render(h);DLOG(L"menu: foreground=%ls time=%ls",appName.c_str(),clockText.c_str());}
}
// macOS-style window placement: the bar is a standard top-edge AppBar, so Windows itself shrinks the work area
// (maximized and snapped windows stay below the bar) and tells us when a full-screen app takes the monitor.
// Explorer owns the reserved space; ABM_REMOVE on exit gives it back. Only the bar's own edge is reserved, so
// the optional "free taskbar work area" feature (which edits only the taskbar's edge) is unaffected.
constexpr UINT kAppBarMsg=WM_APP+77;
bool appBarRegistered=false,fullscreenApp=false;RECT appBarRect{};HWINEVENTHOOK moveHook=nullptr,moveStartHook=nullptr,locationHook=nullptr;
bool shellHooksEnabled=false;  // not in dev/visual-capture runs
void ApplyAppBar(HWND h,const RECT& monitor){
    if(!appBarRegistered){APPBARDATA a{sizeof(a)};a.hWnd=h;a.uCallbackMessage=kAppBarMsg;appBarRegistered=SHAppBarMessage(ABM_NEW,&a)!=FALSE;
        DLOG(L"menu: appbar register=%d",appBarRegistered?1:0);}
    if(!appBarRegistered)return;
    const RECT want{monitor.left,monitor.top,monitor.right,monitor.top+Px(30)};
    if(EqualRect(&want,&appBarRect))return;  // ABM_SETPOS changes the work area, which re-enters Place(): only on real change
    APPBARDATA a{sizeof(a)};a.hWnd=h;a.uEdge=ABE_TOP;a.rc=want;SHAppBarMessage(ABM_QUERYPOS,&a);a.rc.bottom=a.rc.top+Px(30);SHAppBarMessage(ABM_SETPOS,&a);
    appBarRect=want;DLOG(L"menu: appbar reserved %ld,%ld-%ld,%ld",a.rc.left,a.rc.top,a.rc.right,a.rc.bottom);
}
void RemoveAppBar(HWND h){if(!appBarRegistered)return;APPBARDATA a{sizeof(a)};a.hWnd=h;SHAppBarMessage(ABM_REMOVE,&a);appBarRegistered=false;appBarRect={};}
// Live window-drag behaviour (macOS):
//  * top: a title bar never goes above the menu bar. Holding a window there for a moment (or pushing on against the bar)
//    shows a fill preview; releasing fills the window between the menu bar and the Dock (maximize), like double-clicking
//    the title bar;
//  * Dock: when the window's bottom edge crosses the Dock's top edge (either direction) it holds there briefly; the user
//    has to keep dragging down (or up) for it to move on.
// The move loop runs inside the other application, so instead of injecting into it the cursor is confined (ClipCursor)
// while that window is moved: with the grab offset known, a cursor limit is a window-edge limit. Raw mouse input still
// reports motion against the clip, which measures the push. The clip is re-asserted on every raw input (the move loop
// may reset it) and released on move end, on button release (polled), and when this window is destroyed.
// A drag that starts on a maximized window (Windows restores it under the cursor) gets its limits once it has restored.
// Apps that move their windows themselves (no system move loop) are corrected when the button is released.
// Optionally (Settings > Dock) the Windows snap-layout bar that appears at the top during a drag is hidden.
constexpr UINT_PTR kDragClipTimer=13,kReleaseCheckTimer=15;
constexpr UINT kDockFullscreenMsg=WM_APP+79;  // to the Dock: wParam 1 = a full-screen app owns the monitor
bool hideSnapLayouts=false,rawMouseRegistered=false;
bool gameCompatibility=false,gameSuppressed=false;
// Games list (game_compat.h): a listed program running counts as a detected full-screen game (bar hidden, popups closed,
// hooks and capture paused) until it exits. Process names only; nothing is opened.
docky::game::Watcher menuGameWatch;bool listedGameMenu=false;
struct DragClip{bool active=false,hasDock=false,fill=false,snap=false,calibrated=false,pending=false,dwellFill=false;HWND target=nullptr;
    RECT clip{},virt{},startVisible{};LONG topLimit=0,line=0,workTop=0,workBottom=0;POINT lastPt{};ULONGLONG dwellStart=0;
    int side=-1;LONG push=0,pushUp=0;int resets=0;DWORD shellPid=0;std::vector<HWND> before,hidden;}dragClip;
LONG DragDetent(){return Px(72);}        // raw counts of extra push that release the Dock edge
LONG DragFill(){return Px(64);}          // raw counts of push against the menu bar that arm "fill"
constexpr ULONGLONG kDragDwellMs=450;    // resting against the menu bar this long arms "fill" (macOS waits briefly too)
void EnsureRawMouse(){
    if(rawMouseRegistered||!barWindow)return;
    RAWINPUTDEVICE rid{0x01,0x02,RIDEV_INPUTSINK,barWindow};rawMouseRegistered=RegisterRawInputDevices(&rid,1,sizeof(rid))!=FALSE;
}
// Game mode: every raw mouse packet (gaming mice send 1000-8000 per second) woke this thread. While a full-screen app owns
// the monitor raw input is only needed when the pointer is at the top edge (the push that reveals the bar).
void DropRawMouse(){
    if(!rawMouseRegistered)return;
    RAWINPUTDEVICE rid{0x01,0x02,RIDEV_REMOVE,nullptr};if(RegisterRawInputDevices(&rid,1,sizeof(rid)))rawMouseRegistered=false;
}
// Fill preview: a translucent rounded rectangle over the area the window will fill, just behind the dragged window.
HWND fillPreview=nullptr;
void ShowFillPreview(bool show){
    if(!show){if(fillPreview)ShowWindow(fillPreview,SW_HIDE);return;}
    if(!fillPreview){
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DockyFillPreview";
        static HBRUSH white=CreateSolidBrush(RGB(255,255,255));wc.hbrBackground=white;RegisterClassW(&wc);
        fillPreview=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
        if(!fillPreview)return;
        SetLayeredWindowAttributes(fillPreview,0,52,LWA_ALPHA);
        const DWORD round=2;DwmSetWindowAttribute(fillPreview,DWMWA_WINDOW_CORNER_PREFERENCE,&round,sizeof(round));
        SetWindowDisplayAffinity(fillPreview,WDA_EXCLUDEFROMCAPTURE);  // never part of a glass backdrop
    }
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    RECT r=mi.rcWork;InflateRect(&r,-Px(4),-Px(4));
    SetWindowPos(fillPreview,dragClip.target&&IsWindow(dragClip.target)?dragClip.target:HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
void UpdateFill(const wchar_t* why){
    DragClip& c=dragClip;const bool fill=c.pushUp>DragFill()||c.dwellFill;
    if(fill==c.fill)return;
    c.fill=fill;ShowFillPreview(fill);DLOG(L"menu: fill %ls (%ls)",fill?L"armed":L"cancelled",why);
}
void ApplyDragClip(){
    DragClip& c=dragClip;c.clip=c.virt;c.clip.top=c.topLimit;
    if(c.hasDock){if(c.side<0)c.clip.bottom=(std::min)(c.virt.bottom,c.line+1);else c.clip.top=(std::max)(c.topLimit,c.line);}
    if(c.clip.bottom<=c.clip.top+1)c.clip.bottom=c.clip.top+2;
    ClipCursor(&c.clip);
}
// Limits from the grab offset (cursor relative to the window). A window already partly under the menu bar is pulled
// out at once by moving the cursor to the limit.
void SetDragLimits(DragClip& c,POINT pt,const RECT& visible){
    c.topLimit=c.workTop+(pt.y-visible.top);                         // window top >= below the menu bar
    c.hasDock=false;
    MONITORINFO mi{sizeof(mi)};
    if(GetMonitorInfoW(MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY),&mi)&&mi.rcWork.bottom<mi.rcMonitor.bottom){
        c.hasDock=true;c.line=c.workBottom-(visible.bottom-pt.y);      // cursor y where window bottom == Dock top
        c.side=pt.y<=c.line?-1:1;
        if(c.line<=c.topLimit+1)c.hasDock=false;
    }
    if(pt.y<c.topLimit){SetCursorPos(pt.x,c.topLimit);DLOG(L"menu: window pulled out from under the menu bar (%ld px)",c.topLimit-pt.y);}
}
// The move-start event arrives after the drag threshold, before the window has followed the cursor, so the first grab
// offset can be a few pixels short (measured: 6 px). Once the window has moved and the cursor rests for one tick, the
// cursor and window are in step: measure the offset again and correct the limits. A restore-drag (maximized window)
// gets its first limits here once Windows has restored it.
void CalibrateDragClip(){
    DragClip& c=dragClip;if(!c.active||c.calibrated||!IsWindow(c.target))return;
    POINT pt{};if(!GetCursorPos(&pt))return;
    const bool still=pt.x==c.lastPt.x&&pt.y==c.lastPt.y;c.lastPt=pt;if(!still)return;
    RECT frame{},visible{};if(!GetWindowRect(c.target,&frame))return;
    if(FAILED(DwmGetWindowAttribute(c.target,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))))visible=frame;
    if(c.pending){
        if(IsZoomed(c.target))return;
        c.pending=false;c.startVisible=visible;SetDragLimits(c,pt,visible);ApplyDragClip();
        DLOG(L"menu: restored window limits on (top %ld, dock line %ld)",c.topLimit,c.hasDock?c.line:-1L);return;
    }
    if(visible.top==c.startVisible.top&&visible.left==c.startVisible.left)return;  // not following yet
    c.calibrated=true;
    const LONG oldTop=c.topLimit,oldLine=c.line;
    SetDragLimits(c,pt,visible);
    if(oldTop==c.topLimit&&(!c.hasDock||oldLine==c.line))return;
    DLOG(L"menu: drag limits calibrated (top %ld->%ld, dock line %ld->%ld)",oldTop,c.topLimit,oldLine,c.line);
    ApplyDragClip();
}
// Resting against the menu bar arms "fill"; moving away from it cancels.
void DragDwell(){
    DragClip& c=dragClip;if(!c.active||c.pending||(c.hasDock&&c.side>0))return;
    POINT pt{};if(!GetCursorPos(&pt))return;
    const ULONGLONG now=GetTickCount64();
    if(pt.y<=c.topLimit+1){if(!c.dwellStart)c.dwellStart=now;else if(!c.dwellFill&&now-c.dwellStart>=kDragDwellMs){c.dwellFill=true;UpdateFill(L"rested at the top");}}
    else{c.dwellStart=0;if(c.dwellFill&&pt.y>c.topLimit+Px(8)){c.dwellFill=false;UpdateFill(L"moved away");}}
}
// Explorer's snap-layout bar: a new visible top-level window of the shell process near the top of the monitor.
struct SnapScan{DragClip* c;RECT mon;};
BOOL CALLBACK SnapScanWindow(HWND w,LPARAM l){
    auto& x=*reinterpret_cast<SnapScan*>(l);DWORD pid=0;GetWindowThreadProcessId(w,&pid);
    if(pid!=x.c->shellPid||!IsWindowVisible(w))return TRUE;
    if(std::find(x.c->before.begin(),x.c->before.end(),w)!=x.c->before.end()||std::find(x.c->hidden.begin(),x.c->hidden.end(),w)!=x.c->hidden.end())return TRUE;
    RECT r{};if(!GetWindowRect(w,&r))return TRUE;const LONG mh=x.mon.bottom-x.mon.top;
    if(r.top>x.mon.top+mh/4||r.bottom<=x.mon.top||r.bottom-r.top>mh/3||r.right<=x.mon.left||r.left>=x.mon.right)return TRUE;
    wchar_t cls[96]{},title[96]{};GetClassNameW(w,cls,96);GetWindowTextW(w,title,96);
    if(!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd")||!wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW"))return TRUE;
    ShowWindowAsync(w,SW_HIDE);x.c->hidden.push_back(w);
    DLOG(L"menu: snap UI hidden class=%ls title=%ls rect=%ld,%ld-%ld,%ld",cls,title,r.left,r.top,r.right,r.bottom);return TRUE;
}
void HideSnapUi(){
    DragClip& c=dragClip;if(!c.active||!c.snap||!c.shellPid)return;
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    SnapScan scan{&c,mi.rcMonitor};EnumWindows(SnapScanWindow,reinterpret_cast<LPARAM>(&scan));
}
BOOL CALLBACK SnapBaseline(HWND x,LPARAM l){auto& d=*reinterpret_cast<DragClip*>(l);DWORD pid=0;GetWindowThreadProcessId(x,&pid);if(pid==d.shellPid&&IsWindowVisible(x))d.before.push_back(x);return TRUE;}
void ReleaseDragClip(){
    if(!dragClip.active)return;
    ClipCursor(nullptr);ShowFillPreview(false);
    if(dragClip.resets)DLOG(L"menu: drag clip was reset %d time(s) by the system and re-asserted",dragClip.resets);
    dragClip=DragClip{};
    if(barWindow)KillTimer(barWindow,kDragClipTimer);
}
// An ordinary app window (caption or sizing frame) of another process on the menu bar's monitor.
bool OrdinaryWindow(HWND w){
    if(!w||!barWindow||GetAncestor(w,GA_ROOT)!=w||!IsWindowVisible(w)||IsIconic(w))return false;
    DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==GetCurrentProcessId())return false;
    if(GetWindowLongW(w,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return false;
    if(!(GetWindowLongW(w,GWL_STYLE)&(WS_CAPTION|WS_THICKFRAME)))return false;
    BOOL cloaked=FALSE;DwmGetWindowAttribute(w,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)return false;
    return MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST)==MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY);
}
// A window left with its top edge under the bar is moved down to sit just below it.
bool CoversMonitor(HWND w);
bool IsButtonFullscreen(HWND w);
void ClampBelowBar(HWND w,const wchar_t* why){
    if(!OrdinaryWindow(w)||fullscreenApp||IsZoomed(w)||IsButtonFullscreen(w)||CoversMonitor(w))return;  // full screen covers the bar on purpose
    RECT bar{},visible{},frame{};if(!GetWindowRect(barWindow,&bar)||!GetWindowRect(w,&frame))return;
    if(FAILED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))))visible=frame;
    if(visible.top>=bar.bottom||visible.bottom<=bar.top)return;
    const int dy=bar.bottom-visible.top;
    SetWindowPos(w,nullptr,frame.left,frame.top+dy,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_ASYNCWINDOWPOS);
    DLOG(L"menu: window moved below the bar by %d px (%ls)",dy,why);
}
void CALLBACK OnMoveSizeStart(HWINEVENTHOOK,DWORD,HWND w,LONG object,LONG,DWORD,DWORD eventTime){
    ReleaseDragClip();
    if(object!=OBJID_WINDOW||!w||!barWindow||GetAncestor(w,GA_ROOT)!=w||!IsWindowVisible(w)||IsIconic(w))return;
    if(GetWindowLongW(w,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return;
    POINT pt{};if(!GetCursorPos(&pt)||!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return;  // keyboard moves are left alone
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    if(MonitorFromPoint(pt,MONITOR_DEFAULTTONULL)!=MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY))return;
    RECT frame{},visible{};if(!GetWindowRect(w,&frame))return;
    if(FAILED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))))visible=frame;
    const bool zoomed=IsZoomed(w)!=FALSE;  // a maximized window only moves (Windows restores it under the cursor)
    // Moves only; resizing is never constrained. WM_NCHITTEST is not usable here (measured 2026-11-10: modern title bars
    // live in child windows and the top-level answers HTNOWHERE / times out inside the move loop), so a size drag is
    // recognised by where it starts: on the invisible resize border or within a few pixels of the visible edge.
    const LONG edge=Px(5);
    // This out-of-context callback runs a little after the press, so the cursor may already have moved: test both the
    // current point (window following it) and the press point from the mouse history (window not yet moved).
    POINT press=pt;{MOUSEMOVEPOINT now{};now.x=pt.x&0xFFFF;now.y=pt.y&0xFFFF;MOUSEMOVEPOINT hist[64]{};
        const int n=GetMouseMovePointsEx(sizeof(MOUSEMOVEPOINT),&now,hist,64,GMMP_USE_DISPLAY_POINTS);
        for(int i=0;i<n;++i)if(hist[i].time<=eventTime){press={hist[i].x>32767?hist[i].x-65536:hist[i].x,hist[i].y>32767?hist[i].y-65536:hist[i].y};break;}}
    auto onBorder=[&](POINT p){return p.x<visible.left+edge||p.x>=visible.right-edge||p.y<visible.top+Px(3)||p.y>=visible.bottom-edge;};
    {wchar_t cls[96]{};GetClassNameW(w,cls,96);DLOG(L"menu: move start class=%ls zoomed=%d pt=%ld,%ld press=%ld,%ld visible=%ld,%ld-%ld,%ld",cls,zoomed?1:0,pt.x,pt.y,press.x,press.y,visible.left,visible.top,visible.right,visible.bottom);}
    if(!zoomed&&onBorder(pt)&&onBorder(press)){DLOG(L"menu: drag not limited (resize border)");return;}
    DragClip c;c.active=true;c.target=w;c.startVisible=visible;c.workTop=mi.rcWork.top;c.workBottom=mi.rcWork.bottom;c.lastPt=pt;
    c.virt={GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    c.clip=c.virt;c.pending=zoomed;
    if(!zoomed)SetDragLimits(c,pt,visible);
    c.snap=hideSnapLayouts;
    if(c.snap){GetWindowThreadProcessId(GetShellWindow(),&c.shellPid);EnumWindows(SnapBaseline,reinterpret_cast<LPARAM>(&c));}
    dragClip=std::move(c);
    if(!dragClip.pending)ApplyDragClip();
    EnsureRawMouse();
    SetTimer(barWindow,kDragClipTimer,30,nullptr);
    DLOG(L"menu: drag limits %ls (top %ld, dock line %ld side %d, hide snap %d)",dragClip.pending?L"pending restore":L"on",dragClip.topLimit,dragClip.hasDock?dragClip.line:-1L,dragClip.side,dragClip.snap?1:0);
}
// macOS full screen from the maximize button (the middle caption button, macOS's green button): when a window becomes
// maximized right after a press on its maximize button it is enlarged to cover the whole monitor, which hides the menu
// bar and the Dock (FullscreenForeground). Double-click / drag-to-top / Win+Up keep the ordinary maximize between them.
// The press is classified when it happens: WM_NCHITTEST == HTMAXBUTTON, or (for apps with custom title bars) the
// position of the standard second caption button (46 DIP wide, title band 48 DIP).
struct CaptionPress{HWND root=nullptr;ULONGLONG time=0;bool maxButton=false;}captionPress;
void OnLeftDown(){
    captionPress={};
    POINT pt{};if(!GetCursorPos(&pt))return;
    HWND root=GetAncestor(WindowFromPoint(pt),GA_ROOT);
    if(!OrdinaryWindow(root)||IsZoomed(root))return;
    RECT frame{},visible{};if(!GetWindowRect(root,&frame))return;
    if(FAILED(DwmGetWindowAttribute(root,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))))visible=frame;
    const float k=GetDpiForWindow(root)/96.f;
    if(pt.y<visible.top||pt.y>=visible.top+static_cast<LONG>(48*k)||!(GetWindowLongW(root,GWL_STYLE)&WS_MAXIMIZEBOX))return;
    DWORD_PTR ht=HTNOWHERE;SendMessageTimeoutW(root,WM_NCHITTEST,0,MAKELPARAM(pt.x,pt.y),SMTO_ABORTIFHUNG,30,&ht);
    const LONG bw=static_cast<LONG>(46*k),slack=static_cast<LONG>(3*k);
    const bool geometric=pt.x>=visible.right-2*bw-slack&&pt.x<visible.right-bw+slack;
    captionPress={root,GetTickCount64(),ht==HTMAXBUTTON||(geometric&&ht!=HTCLOSE&&ht!=HTMINBUTTON&&ht!=HTCAPTION)};
    if(captionPress.maxButton)DLOG(L"menu: press on a maximize button (hit %d)",static_cast<int>(ht));
}
// Windows keeps a maximized window inside the work area (measured: resizing it to the monitor does not stick), so full
// screen is a normal window covering the whole monitor; its own size before maximizing is kept and restored when the
// maximize button (or a title-bar double-click) maximizes it again, which is how full screen is left.
struct FullscreenWindow{HWND w=nullptr;RECT normal{};};
std::vector<FullscreenWindow> buttonFullscreen;
bool fullscreenBusy=false;
POINT WorkspaceOffset(){  // SetWindowPlacement uses workspace coordinates: origin at the primary monitor's work area
    MONITORINFO mi{sizeof(mi)};const POINT origin{0,0};GetMonitorInfoW(MonitorFromPoint(origin,MONITOR_DEFAULTTOPRIMARY),&mi);
    return {mi.rcWork.left-mi.rcMonitor.left,mi.rcWork.top-mi.rcMonitor.top};
}
bool CoversMonitor(HWND w){
    MONITORINFO mi{sizeof(mi)};RECT r{};if(!GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi)||!GetWindowRect(w,&r))return false;
    return r.left<=mi.rcMonitor.left&&r.top<=mi.rcMonitor.top&&r.right>=mi.rcMonitor.right&&r.bottom>=mi.rcMonitor.bottom;
}
void EnterFullscreen(HWND w){
    WINDOWPLACEMENT wp{sizeof(wp)};if(!GetWindowPlacement(w,&wp))return;
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi))return;
    fullscreenBusy=true;
    const RECT normal=wp.rcNormalPosition;const POINT o=WorkspaceOffset();const RECT m=mi.rcMonitor;
    wp.showCmd=SW_SHOWNORMAL;wp.rcNormalPosition={m.left-o.x,m.top-o.y,m.right-o.x,m.bottom-o.y};wp.flags=0;
    SetWindowPlacement(w,&wp);
    RECT frame{},visible{};
    if(GetWindowRect(w,&frame)&&SUCCEEDED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))))  // hide the invisible borders
        SetWindowPos(w,nullptr,m.left-(visible.left-frame.left),m.top-(visible.top-frame.top),(m.right-m.left)+(visible.left-frame.left)+(frame.right-visible.right),
                     (m.bottom-m.top)+(visible.top-frame.top)+(frame.bottom-visible.bottom),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    fullscreenBusy=false;
    buttonFullscreen.push_back({w,normal});
    wchar_t cls[96]{};GetClassNameW(w,cls,96);DLOG(L"menu: maximize button -> full screen class=%ls",cls);
}
bool IsButtonFullscreen(HWND w){for(const auto& f:buttonFullscreen)if(f.w==w)return true;return false;}
void LeaveFullscreen(size_t i){
    const FullscreenWindow f=buttonFullscreen[i];buttonFullscreen.erase(buttonFullscreen.begin()+static_cast<std::ptrdiff_t>(i));
    WINDOWPLACEMENT wp{sizeof(wp)};if(!IsWindow(f.w)||!GetWindowPlacement(f.w,&wp))return;
    fullscreenBusy=true;wp.showCmd=SW_SHOWNORMAL;wp.rcNormalPosition=f.normal;wp.flags=0;SetWindowPlacement(f.w,&wp);fullscreenBusy=false;
    DLOG(L"menu: full screen left (maximize button / double-click); previous size restored");
}
void CALLBACK OnLocationChange(HWINEVENTHOOK,DWORD,HWND w,LONG object,LONG child,DWORD,DWORD){
    if(object!=OBJID_WINDOW||child!=CHILDID_SELF||!w||fullscreenBusy)return;
    for(size_t i=0;i<buttonFullscreen.size();++i)if(buttonFullscreen[i].w==w){if(IsZoomed(w))LeaveFullscreen(i);return;}
    if(w!=captionPress.root||!captionPress.maxButton)return;
    if(GetTickCount64()-captionPress.time>2500){captionPress={};return;}
    if(!IsZoomed(w))return;
    captionPress={};
    EnterFullscreen(w);
}
// Forgets full-screen windows that were closed, minimized or moved/resized off the monitor by the user.
void MaintainButtonFullscreen(){
    for(size_t i=0;i<buttonFullscreen.size();){
        HWND w=buttonFullscreen[i].w;
        if(!IsWindow(w)||IsIconic(w)||(!IsZoomed(w)&&!CoversMonitor(w))){buttonFullscreen.erase(buttonFullscreen.begin()+static_cast<std::ptrdiff_t>(i));DLOG(L"menu: full screen ended (window closed or moved)");continue;}
        ++i;
    }
}
void OnLeftUp(){
    if(dragClip.active||!barWindow)return;  // system moves end in OnMoveSizeEnd
    SetTimer(barWindow,kReleaseCheckTimer,150,nullptr);
}
void FullscreenEdge(HWND h,LONG dy);
void OnDragRawInput(HRAWINPUT input){
    RAWINPUT ri{};UINT size=sizeof(ri);
    if(GetRawInputData(input,RID_INPUT,&ri,&size,sizeof(RAWINPUTHEADER))==static_cast<UINT>(-1)||ri.header.dwType!=RIM_TYPEMOUSE)return;
    const USHORT buttons=ri.data.mouse.usButtonFlags;
    if(buttons&RI_MOUSE_LEFT_BUTTON_DOWN)OnLeftDown();
    if(buttons&RI_MOUSE_LEFT_BUTTON_UP)OnLeftUp();
    if(fullscreenApp&&!(ri.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)&&barWindow)FullscreenEdge(barWindow,ri.data.mouse.lLastY);
    DragClip& c=dragClip;if(!c.active||c.pending)return;
    RECT now{};if(GetClipCursor(&now)&&!EqualRect(&now,&c.clip)){++c.resets;ClipCursor(&c.clip);}
    if(ri.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)return;
    POINT pt{};GetCursorPos(&pt);const LONG dy=ri.data.mouse.lLastY;if(!dy)return;
    const LONG mag=dy>0?dy:-dy;
    if(c.hasDock){  // two-way detent at the Dock's top edge
        const bool atLine=c.side<0?pt.y>=c.line-1:pt.y<=c.line+1;const bool into=c.side<0?dy>0:dy<0;
        if(atLine&&into)c.push+=mag;else if(!into)c.push=(std::max)(0L,c.push-2*mag);
        if(c.push>DragDetent()){c.side=-c.side;c.push=0;ApplyDragClip();DLOG(L"menu: dock edge passed (%ls)",c.side>0?L"down":L"up");}
    }
    if(!c.hasDock||c.side<0){  // pushing against the menu bar arms "fill"
        if(pt.y<=c.topLimit+1&&dy<0)c.pushUp=(std::min)(c.pushUp+mag,2*DragFill());else if(dy>0)c.pushUp=(std::max)(0L,c.pushUp-2*mag);
        if(pt.y>c.topLimit+Px(8))c.pushUp=0;  // the window left the top: no fill
        UpdateFill(L"push");
    }
}
void CALLBACK OnMoveSizeEnd(HWINEVENTHOOK,DWORD,HWND w,LONG object,LONG,DWORD,DWORD){
    const bool fill=dragClip.active&&dragClip.fill&&dragClip.target==w;
    ReleaseDragClip();
    if(object!=OBJID_WINDOW||!OrdinaryWindow(w)||fullscreenApp||IsZoomed(w))return;
    if(fill&&(GetWindowLongW(w,GWL_STYLE)&WS_MAXIMIZEBOX)){ShowWindowAsync(w,SW_MAXIMIZE);DLOG(L"menu: window filled (held against the menu bar)");return;}
    ClampBelowBar(w,L"move end");
}
// macOS: true full screen (the app covers the whole monitor: F11, a full-screen button, games) hides the menu bar and the
// Dock; a maximized window (double-click the title bar, or drag against the menu bar) keeps both and sits between them.
// The shell's ABN_FULLSCREENAPP is not sent for every such window (measured 2026-11-10), so the foreground window is
// also checked every 250 ms: visible, not cloaked, not the desktop or a docky window, and covering the whole monitor.
void Place(HWND h);
constexpr UINT_PTR kFullscreenTimer=14;
// The full-screen window found last: while it stays (visible, not minimized, covering the monitor), opening another,
// ordinary window on top keeps the bars hidden (macOS: the full-screen app keeps its Space).
HWND fullscreenWindow=nullptr;
bool CoversMonitor(HWND w);
bool FullscreenCandidate(HWND f){
    if(!f||!IsWindow(f)||!IsWindowVisible(f)||IsIconic(f))return false;
    DWORD pid=0,shellPid=0;GetWindowThreadProcessId(f,&pid);GetWindowThreadProcessId(GetShellWindow(),&shellPid);
    if(pid==GetCurrentProcessId()||pid==shellPid)return false;  // Explorer: desktop, taskbar, the desktop-switch animation
    wchar_t cls[64]{};GetClassNameW(f,cls,64);
    if(!wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW")||!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd")||!wcscmp(cls,L"XamlWindow")||!wcscmp(cls,L"MultitaskingViewFrame"))return false;
    if(GetWindowLongW(f,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return false;
    BOOL cloaked=FALSE;DwmGetWindowAttribute(f,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)return false;
    if(MonitorFromWindow(f,MONITOR_DEFAULTTONULL)!=MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY))return false;
    return CoversMonitor(f);
}
// The window actually in front on the bar's monitor, by Z order, when it covers the whole monitor. Used when the
// foreground is missing, Explorer or DockOver: a game started from Steam can cover the monitor without ever getting
// the foreground (PUBG, log of 2026-11-14: foreground stayed "explorer" while the shell reported full screen). The
// bar and Dock then stayed up, the topmost Dock sat over the game, a click on it took the focus and the game minimized.
// Passed over: hidden, minimized, cloaked, empty, tool, click-through and no-activate windows, DockOver's own windows,
// and small always-on-top windows (game / GPU overlays). Any other window in front ends the search: not full screen.
HWND FrontCoveringWindow(){
    const HMONITOR bar=MonitorFromWindow(barWindow,MONITOR_DEFAULTTOPRIMARY);
    int budget=48;  // windows on the monitor examined at most
    for(HWND w=GetTopWindow(nullptr);w&&budget>0;w=GetWindow(w,GW_HWNDNEXT)){
        if(!IsWindowVisible(w)||IsIconic(w))continue;
        const LONG_PTR ex=GetWindowLongPtrW(w,GWL_EXSTYLE);
        if(ex&(WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE))continue;
        RECT r{};if(!GetWindowRect(w,&r)||IsRectEmpty(&r))continue;
        BOOL cloaked=FALSE;DwmGetWindowAttribute(w,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)continue;
        if(MonitorFromWindow(w,MONITOR_DEFAULTTONULL)!=bar)continue;
        --budget;
        DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==GetCurrentProcessId())continue;
        const bool candidate=FullscreenCandidate(w);
        if(!candidate&&(ex&WS_EX_TOPMOST))continue;  // overlay
        if(WindowProcessName(w)==L"DockOver")continue;  // Launchpad, docky Desktop, Settings
        return candidate?w:nullptr;
    }
    return nullptr;
}
bool FullscreenForeground(){
    if(!barWindow)return false;
    // No foreground while windows change hands, or a docky window in front: the state follows the full-screen window itself.
    // (It used to be kept as is, so closing a full-screen Chrome left the bar and Dock hidden until another window got
    // focus, 3.5-5.5 s in the log of 2026-11-12.)
    const auto keep=[]{if(!fullscreenApp)return false;if(fullscreenWindow&&FullscreenCandidate(fullscreenWindow))return true;fullscreenWindow=nullptr;return false;};
    const auto front=[]{HWND z=FrontCoveringWindow();if(!z)return false;
        if(z!=fullscreenWindow){wchar_t cls[64]{};GetClassNameW(z,cls,64);DLOG(L"menu: full-screen window in front without the foreground: %ls (%ls)",cls,WindowProcessName(z).c_str());}
        fullscreenWindow=z;return true;};
    HWND f=GetForegroundWindow();if(!f)return front()||keep();
    {DWORD pid=0,shellPid=0;GetWindowThreadProcessId(f,&pid);GetWindowThreadProcessId(GetShellWindow(),&shellPid);wchar_t cls[64]{};GetClassNameW(f,cls,64);
     if(pid==GetCurrentProcessId()||!wcscmp(cls,L"LiquidDockV01")||!wcscmp(cls,L"LiquidControlCenter")||!wcscmp(cls,L"LiquidUnifiedPopup98d2"))return front()||keep();
     if(pid==shellPid&&front())return true;}
    if(FullscreenCandidate(f)){fullscreenWindow=f;return true;}
    if(fullscreenWindow&&FullscreenCandidate(fullscreenWindow))return true;  // another window opened over a full-screen app
    fullscreenWindow=nullptr;
    return false;
}
// In full screen the bar is hidden; pushing into (or resting at) the top edge reveals it over the app (macOS) without
// touching the work area or the app's size. It hides again once the pointer leaves it and no menu is open.
bool barRevealed=false;ULONGLONG topEdgeSince=0,topPushSince=0,topPushLast=0;LONG topPush=0;
// macOS shows the full-screen menu bar on a solid black strip: an opaque black window directly behind the bar.
HWND barBlack=nullptr;
void ShowBarBlack(bool show,const RECT& r){
    if(!show){if(barBlack)ShowWindow(barBlack,SW_HIDE);return;}
    if(!barBlack){WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DockyMenuBarBlack";
        wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));RegisterClassW(&wc);
        barBlack=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,wc.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
        if(!barBlack)return;const DWORD square=1;DwmSetWindowAttribute(barBlack,DWMWA_WINDOW_CORNER_PREFERENCE,&square,sizeof(square));}
    SetWindowPos(barBlack,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
void RevealBar(HWND h,bool show){
    if(show==barRevealed||!fullscreenApp)return;
    barRevealed=show;topPush=0;topEdgeSince=topPushSince=topPushLast=0;
    if(show){MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTOPRIMARY),&mi))return;
        const RECT r{mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right,mi.rcMonitor.top+Px(30)};
        ShowBarBlack(true,r);  // first, so the bar and its input window end up above it
        SetWindowPos(h,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        if(inputWindow)SetWindowPos(inputWindow,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);Render(h);}
    else{ClosePopup();if(inputWindow)ShowWindow(inputWindow,SW_HIDE);ShowWindow(h,SW_HIDE);ShowBarBlack(false,RECT{});}
    DLOG(L"menu: full screen, bar %ls",show?L"revealed":L"hidden again");
}
// Raw mouse motion while in full screen: the top edge reveals the bar only after a sustained push (user request: not too
// eager): upward motion against the edge for at least 0.65 s and ~60 counts, without pausing more than 0.3 s; resting at
// the edge without pushing reveals it after 1.5 s.
constexpr ULONGLONG kRevealPushMs=650,kRevealRestMs=1500;
// A game owns the pointer: the cursor is hidden or clipped to less than the whole desktop. Looking up in a shooter then
// pins the hidden cursor at the top edge while raw input keeps reporting upward motion, which revealed the bar; the
// topmost bar over an exclusive full-screen game (PUBG, log of 2026-11-13) made it drop out of full screen and minimize.
bool GameOwnsPointer(){
    CURSORINFO ci{sizeof(ci)};if(GetCursorInfo(&ci)&&ci.flags==0)return true;  // hidden (CURSOR_SUPPRESSED is touch/pen)
    RECT clip{};if(!GetClipCursor(&clip))return false;
    const RECT desk{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return clip.left>desk.left||clip.top>desk.top||clip.right<desk.right||clip.bottom<desk.bottom;
}
void FullscreenEdge(HWND h,LONG dy){
    if(!fullscreenApp||barRevealed)return;
    if(GameOwnsPointer()){topPush=0;topEdgeSince=topPushSince=topPushLast=0;return;}
    POINT pt{};MONITORINFO mi{sizeof(mi)};if(!GetCursorPos(&pt)||!GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    if(pt.y>mi.rcMonitor.top+1||pt.x<mi.rcMonitor.left||pt.x>=mi.rcMonitor.right){topPush=0;topEdgeSince=topPushSince=topPushLast=0;return;}
    const ULONGLONG now=GetTickCount64();
    if(!topEdgeSince)topEdgeSince=now;
    if(dy<0){
        if(!topPushSince||now-topPushLast>300){topPushSince=now;topPush=0;}  // a pause restarts the push
        topPush-=dy;topPushLast=now;
        if(now-topPushSince>=kRevealPushMs&&topPush>Px(60))RevealBar(h,true);
    }
}
void FullscreenRevealTick(HWND h){
    if(!fullscreenApp)return;
    POINT pt{};MONITORINFO mi{sizeof(mi)};if(!GetCursorPos(&pt)||!GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    if(!barRevealed){if(topEdgeSince&&!GameOwnsPointer()&&pt.y<=mi.rcMonitor.top+1&&GetTickCount64()-topEdgeSince>=kRevealRestMs)RevealBar(h,true);return;}
    static ULONGLONG awaySince=0;
    const bool busy=popup||liquid::cc::IsOpen()||activeMenu!=-1;
    if(busy||pt.y<=mi.rcMonitor.top+Px(30)+Px(6)){awaySince=0;return;}
    if(!awaySince)awaySince=GetTickCount64();else if(GetTickCount64()-awaySince>=400){awaySince=0;RevealBar(h,false);}
}
void SetFullscreenApp(HWND h,bool full){
    if(full==fullscreenApp)return;
    fullscreenApp=full;barRevealed=false;topPush=0;topEdgeSince=topPushSince=topPushLast=0;ShowBarBlack(false,RECT{});

    {wchar_t cls[64]{};if(full)GetClassNameW(fullscreenWindow?fullscreenWindow:GetForegroundWindow(),cls,64);DLOG(L"menu: full-screen app %ls %ls",full?L"on: bar and Dock hidden; window":L"off: bar and Dock shown",cls);}
    if(full){ReleaseDragClip();ClosePopup();if(inputWindow)ShowWindow(inputWindow,SW_HIDE);ShowWindow(h,SW_HIDE);
        // Game mode: EVENT_OBJECT_LOCATIONCHANGE also fires for every pointer move, and raw input for every mouse packet.
        if(locationHook){UnhookWinEvent(locationHook);locationHook=nullptr;}DropRawMouse();}
    else{Place(h);ShowWindow(h,SW_SHOWNOACTIVATE);if(inputWindow)ShowWindow(inputWindow,SW_SHOWNOACTIVATE);Render(h);
        if(!locationHook&&shellHooksEnabled)locationHook=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,OnLocationChange,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        if(shellHooksEnabled)EnsureRawMouse();}
}
// Game compatibility follows the detected fullscreen window and Windows' D3D fullscreen
// state or a game-owned pointer. A cursor test is a heuristic, not game identification.
void SyncGameCompatibility(HWND h) {
    const auto revision=liquid::settings::ReadRevision(themeSettingsPath);
    if(revision!=settingsRevision){
        settingsRevision=revision;
        const auto settings=liquid::settings::Load(themeSettingsPath);
        gameCompatibility=settings.value.dock.gameCompatibility;
        hideSnapLayouts=settings.value.dock.hideSnapLayouts;
        if(menuScalePercent!=settings.value.menuBar.scalePercent){menuScalePercent=settings.value.menuBar.scalePercent;ClosePopup();Place(h);Render(h);}
        docky::vblank::SetCaps(settings.value.performance.animationFps,settings.value.performance.captureFps);
    }
    QUERY_USER_NOTIFICATION_STATE state=QUNS_NOT_PRESENT;
    const bool exclusive=SUCCEEDED(SHQueryUserNotificationState(&state))&&state==QUNS_RUNNING_D3D_FULL_SCREEN;
    // Keep the decision for the same fullscreen window: games often show and release the
    // cursor in their inventory or menus, which must not bring the Dock back over them.
    static HWND detectedGame=nullptr;
    if(!fullscreenApp||!gameCompatibility||detectedGame!=fullscreenWindow)detectedGame=nullptr;
    if(gameCompatibility&&fullscreenApp&&fullscreenWindow&&(exclusive||GameOwnsPointer()))detectedGame=fullscreenWindow;
    const bool suppress=(gameCompatibility&&fullscreenApp&&detectedGame&&detectedGame==fullscreenWindow)||listedGameMenu;
    if(suppress!=gameSuppressed){
        gameSuppressed=suppress;
        if(suppress){if(barRevealed)RevealBar(h,false);DropRawMouse();}
        DLOG(L"menu: game compatibility %ls",suppress?L"on":L"off");
    }
    static HWND sentWindow=nullptr;static bool sentFull=false,sentSuppressed=false;
    HWND dockWindow=FindWindowW(L"LiquidDockV01",nullptr);
    if(dockWindow&&(dockWindow!=sentWindow||sentFull!=fullscreenApp||sentSuppressed!=gameSuppressed)){
        if(PostMessageW(dockWindow,kDockFullscreenMsg,fullscreenApp?1:0,gameSuppressed?1:0)){
            sentWindow=dockWindow;sentFull=fullscreenApp;sentSuppressed=gameSuppressed;
        }
    } else if(!dockWindow) sentWindow=nullptr;
}
void Place(HWND h) {
    MONITORINFO mi{sizeof(mi)};
    if(!GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTOPRIMARY),&mi))return;
    const UINT monitorDpi=GetDpiForWindow(h);
    dpi=MulDiv(monitorDpi?monitorDpi:96,menuScalePercent,100);
    ApplyAppBar(h,mi.rcMonitor);
    if(fullscreenApp)return;  // hidden while a full-screen app owns the monitor
    SetWindowPos(h,HWND_TOPMOST,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,Px(30),SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
// Status icons V0.2: one vector style (GDI+ paths, 1.5 DIP strokes, shared centre line) instead of mixed
// font glyphs. White = opaque, grey = dimmed, black = cut-out (the bar converts luminance to alpha).
// Original shapes in a simple monochrome macOS-like language; no SF Symbols assets are used.
// ---- Menu Bar left section V0.5 -------------------------------------------------------------
// Text: DirectWrite through a D2D DC render target into the same DIB as before (white on black, the
// luminance becomes alpha). Segoe UI Variable Text (fallback Segoe UI), one size, Regular for menus and
// the clock, SemiBold only for the active application name (macOS). Grayscale antialiasing.
struct TextKit {
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> regular, semibold;
    std::wstring family;
    float size = 0;
};
TextKit& Kit() { static TextKit k; return k; }
bool EnsureKit(float px) {
    TextKit& k = Kit();
    if (!k.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, k.d2d.GetAddressOf()))) return false;
    if (!k.dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(k.dw.GetAddressOf())))) return false;
    if (!k.rt) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(k.d2d->CreateDCRenderTarget(&props, k.rt.GetAddressOf()))) return false;
        k.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        k.white.Reset();
    }
    if (k.family.empty()) {
        Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts; UINT32 idx = 0; BOOL exists = FALSE;
        k.dw->GetSystemFontCollection(fonts.GetAddressOf());
        k.family = fonts && SUCCEEDED(fonts->FindFamilyName(liquid::design::type::Family, &idx, &exists)) && exists ? liquid::design::type::Family : liquid::design::type::FamilyGdi;
        DLOG(L"menu: text family %ls", k.family.c_str());
    }
    if (k.size != px || !k.regular) {
        k.size = px;
        k.regular.Reset(); k.semibold.Reset();
        IDWriteTextFormat** outs[2] = {k.regular.ReleaseAndGetAddressOf(), k.semibold.ReleaseAndGetAddressOf()};
        const DWRITE_FONT_WEIGHT weights[2] = {DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_WEIGHT_SEMI_BOLD};
        for (int i = 0; i < 2; ++i) {
            if (FAILED(k.dw->CreateTextFormat(k.family.c_str(), nullptr, weights[i], DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", outs[i]))) return false;
            (*outs[i])->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*outs[i])->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);  // one shared vertical centre / baseline
        }
    }
    return true;
}
float TextWidth(const std::wstring& t, IDWriteTextFormat* f) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (t.empty() || FAILED(Kit().dw->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, 4096.0f, 100.0f, layout.GetAddressOf()))) return 0;
    DWRITE_TEXT_METRICS m{}; layout->GetMetrics(&m); return m.widthIncludingTrailingWhitespace;
}

// Logo provider (V0.5). Default: built-in vector Windows-11-style four-pane mark. A custom image (PNG or
// ICO; SVG reserved for later) can be named by LIQUID_MENU_LOGO or the first line of
// %LOCALAPPDATA%\LiquidDesktop\menu-logo.txt. Custom images are drawn as a menu-bar template: their alpha
// silhouette in the bar's text colour. Missing or unreadable files fall back to the default mark.
class LogoProvider {
public:
    void Init() {
        if (initialized_) return;
        initialized_ = true;
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LIQUID_MENU_LOGO", env, MAX_PATH)) path_ = env;
        else {
            wchar_t local[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
                std::wifstream f(std::wstring(local) + L"\\LiquidDesktop\\menu-logo.txt");
                std::wstring line; if (f && std::getline(f, line)) path_ = line;
            }
        }
        while (!path_.empty() && (path_.back() == L'\r' || path_.back() == L' ')) path_.pop_back();
        if (path_.empty()) { DLOG(L"menu: logo default (built-in vector)"); return; }
        auto img = std::make_unique<Gdiplus::Bitmap>(path_.c_str());
        if (img->GetLastStatus() == Gdiplus::Ok && img->GetWidth() > 0 && img->GetHeight() > 0) { image_ = std::move(img); DLOG(L"menu: logo custom image loaded"); }
        else DLOG(L"menu: logo custom image unavailable; default used");
    }
    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& box) {
        if (image_) {  // template rendering: keep alpha, force white
            Gdiplus::ColorMatrix cm = {{{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,1,0},{1,1,1,0,1}}};
            Gdiplus::ImageAttributes ia; ia.SetColorMatrix(&cm);
            const float s = (std::min)(box.Width, box.Height), iw = static_cast<float>(image_->GetWidth()), ih = static_cast<float>(image_->GetHeight());
            const float k = s / (std::max)(iw, ih);
            g.DrawImage(image_.get(), Gdiplus::RectF(box.X + (box.Width - iw * k) / 2, box.Y + (box.Height - ih * k) / 2, iw * k, ih * k), 0, 0, iw, ih, Gdiplus::UnitPixel, &ia);
            return;
        }
        // Four slightly rounded panes, 2x2, small gap (original vector art; not a system or vendor asset).
        const float s = (std::min)(box.Width, box.Height), gap = s * 0.09f, pane = (s - gap) / 2, r = pane * 0.16f;
        const float x0 = box.X + (box.Width - s) / 2, y0 = box.Y + (box.Height - s) / 2;
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
        for (int i = 0; i < 4; ++i) {
            const float x = x0 + (i % 2) * (pane + gap), y = y0 + (i / 2) * (pane + gap);
            Gdiplus::GraphicsPath p;
            p.AddArc(x, y, 2 * r, 2 * r, 180, 90); p.AddArc(x + pane - 2 * r, y, 2 * r, 2 * r, 270, 90);
            p.AddArc(x + pane - 2 * r, y + pane - 2 * r, 2 * r, 2 * r, 0, 90); p.AddArc(x, y + pane - 2 * r, 2 * r, 2 * r, 90, 90);
            p.CloseFigure(); g.FillPath(&brush, &p);
        }
    }
private:
    bool initialized_ = false;
    std::wstring path_;
    std::unique_ptr<Gdiplus::Bitmap> image_;
};
LogoProvider& Logo() { static LogoProvider l; return l; }

// ---- Input menu (keyboard layout of the foreground app) -------------------------------------------------------
// The bar never takes focus, so the foreground window is the app being typed into: its thread's keyboard layout is the
// one shown, and choosing another posts WM_INPUTLANGCHANGEREQUEST to that window (the documented way to ask an app's
// thread to switch). Polled every 300 ms (kInputTimer); a change re-renders the bar.
constexpr UINT_PTR kInputTimer=31;
HKL inputLayout=nullptr;HWND inputTarget=nullptr;
HKL ForegroundLayout(){return docky::statuspopup::ForegroundLayout(inputLayout,inputTarget);}
using docky::statuspopup::InputBadge;
void StatusIcons(HDC dc){
    static ULONG_PTR token=0;if(!token){Gdiplus::GdiplusStartupInput in;Gdiplus::GdiplusStartup(&token,&in,nullptr);}
    Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const float k=Px(100)/100.f;
    auto mid=[](const RECT& r){return Gdiplus::PointF((r.left+r.right)*0.5f,(r.top+r.bottom)*0.5f);};
    auto grey=[](BYTE v){return Gdiplus::Color(255,v,v,v);};
    auto pen=[&](BYTE v,float w){auto p=std::make_unique<Gdiplus::Pen>(grey(v),w*k);p->SetStartCap(Gdiplus::LineCapRound);p->SetEndCap(Gdiplus::LineCapRound);p->SetLineJoin(Gdiplus::LineJoinRound);return p;};
    auto shown=[](int i){return statusRects[i].right>statusRects[i].left;};
    for(const auto& e:extras)if(e.kind==ExtraKind::Chevron){  // Hidden Icons: a small downward chevron, same 1.4 DIP stroke as the Control Center glyph
        const auto c=mid(e.rect);auto p=pen(255,1.5f);const Gdiplus::PointF pts[3]{{c.X-4.f*k,c.Y-2.f*k},{c.X,c.Y+2.f*k},{c.X+4.f*k,c.Y-2.f*k}};g.DrawLines(p.get(),pts,3);}
    if(shown(0)&&!HasWorkshop(WifiArtKey())) {
        const auto c=mid(statusRects[0]);const BYTE v=systemStatus.wifi==liquid::status::Wifi::Connected?255:systemStatus.wifi==liquid::status::Wifi::Disconnected?120:80;
        docky::statusart::Wifi(g,c.X,c.Y-1.f*k,k,grey(v));
    }
    if(shown(1)&&!HasWorkshop(SoundKey())) {
        const auto c=mid(statusRects[1]);const bool ok=systemStatus.audioAvailable;
        docky::statusart::Speaker(g,c.X,c.Y,k,grey(ok?255:110),ok,systemStatus.muted,systemStatus.volumePercent);
    }
    if(shown(5)&&!HasWorkshop("bluetooth")){const auto c=mid(statusRects[5]);docky::statusart::Bluetooth(g,c.X,c.Y,k,grey(255));}
    if(shown(6)){  // input menu: the layout's badge in a rounded outline (macOS: "拼" for Pinyin), or its language code
        const auto c=mid(statusRects[6]);const std::wstring badge=InputBadge(inputLayout);const bool wide=badge.size()>1;
        const float bw=(wide?20.f:16.f)*k,bh=15.f*k,r=3.5f*k;auto p=pen(255,1.2f);Gdiplus::GraphicsPath box;
        box.AddArc(c.X-bw/2,c.Y-bh/2,2*r,2*r,180,90);box.AddArc(c.X+bw/2-2*r,c.Y-bh/2,2*r,2*r,270,90);box.AddArc(c.X+bw/2-2*r,c.Y+bh/2-2*r,2*r,2*r,0,90);box.AddArc(c.X-bw/2,c.Y+bh/2-2*r,2*r,2*r,90,90);box.CloseFigure();
        g.DrawPath(p.get(),&box);
        Gdiplus::FontFamily family(wide?L"Segoe UI":L"Microsoft YaHei UI");Gdiplus::Font font(&family,(wide?8.f:10.f)*k,wide?Gdiplus::FontStyleBold:Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::StringFormat fmt;fmt.SetAlignment(Gdiplus::StringAlignmentCenter);fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);Gdiplus::SolidBrush ink(grey(255));
        g.DrawString(badge.c_str(),static_cast<INT>(badge.size()),&font,Gdiplus::RectF(c.X-bw/2,c.Y-bh/2+0.5f*k,bw,bh),&fmt,&ink);}
    if(shown(3)&&!HasWorkshop("control-center")) {   // Control Center: two stacked switches with knobs on opposite ends (no gear).
        const auto c=mid(statusRects[3]);const float w=15.f*k,h=6.6f*k;auto p=pen(255,1.4f);Gdiplus::SolidBrush knob(grey(255));
        for(int i=0;i<2;++i){const float y=c.Y+(i?3.9f:-3.9f)*k;Gdiplus::GraphicsPath cap;cap.AddArc(c.X-w/2,y-h/2,h,h,90,180);cap.AddArc(c.X+w/2-h,y-h/2,h,h,270,180);cap.CloseFigure();
            g.DrawPath(p.get(),&cap);const float kx=i?c.X+w/2-h/2:c.X-w/2+h/2;g.FillEllipse(&knob,kx-2.4f*k,y-2.4f*k,4.8f*k,4.8f*k);}
    }
}
// The percentage is real system state. Draw into the premultiplied output after the
// monochrome mask pass so dark digits stay opaque even over a light desktop.
void DrawBatteryPill(std::uint32_t* pixels,int width,int height){
 if(!systemStatus.batteryPresent||statusRects[2].right<=statusRects[2].left||HasWorkshop(BatteryBolt()?"battery.charging":"battery.discharging"))return;
 // The body plus terminal (0.8 gap + 1.7 wide) is centred, not the body alone: shift left by half the terminal.
 const RECT& slot=statusRects[2];const float k=Px(100)/100.f,cx=(slot.left+slot.right)*.5f-1.25f*k,cy=(slot.top+slot.bottom)*.5f;
 docky::battery::DrawSmooth(pixels,width,height,cx,cy,k,systemStatus.batteryPercent,BatteryBolt(),LightBar());
}
// GDI rasterizes text into a black DIB; luminance becomes premultiplied white alpha.
// Empty pixels are exactly zero and the normal layered-window hit testing stays enabled.
void Render(HWND h) {
    RECT r{};GetClientRect(h,&r);const int width=r.right, height=r.bottom;const int widthPx=width;
    if(width<=0||height<=0)return;
    HDC screen=GetDC(nullptr);if(!screen)return;
    HDC dc=CreateCompatibleDC(screen);if(!dc){ReleaseDC(nullptr,screen);return;}
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;
    bi.bmiHeader.biHeight=-height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP bmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bmp||!bits){if(bmp)DeleteObject(bmp);DeleteDC(dc);ReleaseDC(nullptr,screen);return;}
    HGDIOBJ oldBmp=SelectObject(dc,bmp);std::memset(bits,0,static_cast<size_t>(width)*height*4);
    // Layout (pixels). Left: logo (system menu), active app (SemiBold), app menus. Right: status, clock.
    const bool kit=EnsureKit(13.0f*static_cast<float>(Px(1000))/1000.0f);
    TextKit& tk=Kit();
    auto textW=[&](const std::wstring& t,bool bold){return kit?static_cast<int>(std::ceil(TextWidth(t,bold?tk.semibold.Get():tk.regular.Get()))):static_cast<int>(t.size())*Px(7);};
    const int clockW=textW(clockText,false);
    // Clock pill (macOS reference 2026-11-19, the open Notification Center's clock): the text is centred with even
    // padding of 0.55 x the pill height on each side (was 2-3 px). PillCoverage insets the slot by 2 px per side.
    const int clockPillH=(std::min)(height-2*Px(2),Px(24)),clockPad=static_cast<int>(std::lround(0.55f*static_cast<float>(clockPillH)));
    int edge=widthPx-Px(2);RECT right{edge-clockW-2*clockPad-2*Px(2),0,edge,height};statusRects[4]=right;edge=right.left-Px(4);
    // macOS reference (menu bar screenshot 2026-11-10, measured at 2x): item centres sit at fixed distances
    // left of the clock text's left edge: Control Center 29.75 pt, battery 73.25, Wi-Fi 111.75, next item
    // (sound here, Bluetooth in the reference) 147. Slots are symmetric about those centres (icons are drawn
    // at the slot middle) and reach halfway to the nearer neighbour.
    // Extras V0.1: items the user switched off are skipped; the next visible item keeps its own distance to its
    // right-hand neighbour (43.5 / 38.5 / 35.25 pt) and the first visible one sits 29.75 pt from the clock.
    const auto pt=[&](float v){return static_cast<int>(std::lround(v*static_cast<float>(Px(1000))/1000.0f));};
    const int clockLeft=right.left+Px(2)+clockPad;  // the clock text's left edge
    const auto slot=[&](int c,int half){return RECT{c-half,0,c+half,height};};
    {
        const bool batterySlot=systemStatus.batteryPresent||systemStatus.batteryKnown;
        // Right to left: Control Center, battery, Wi-Fi, Bluetooth (macOS order), sound, then the input menu.
        constexpr int kOrdered=6;
        const int order[kOrdered]={3,2,0,5,1,6};const float lead[kOrdered]={29.75f,43.5f,38.5f,35.25f,35.25f,35.25f};
        const bool visible[kOrdered]={extrasConfig.control,extrasConfig.battery&&batterySlot,extrasConfig.wifi,extrasConfig.bluetooth,extrasConfig.sound,extrasConfig.input};
        int centers[kOrdered]{};int prev=clockLeft;bool first=true;
        for(int k=0;k<kOrdered;++k){if(!visible[k]){statusRects[order[k]]={0,0,0,0};continue;}centers[k]=prev-pt(first?29.75f:lead[k]);prev=centers[k];first=false;}
        for(int k=0;k<kOrdered;++k){if(!visible[k])continue;int next=-1;for(int j=k+1;j<kOrdered;++j)if(visible[j]){next=j;break;}
            int half=next>=0?(centers[k]-centers[next])/2:pt(35.25f)/2;
            if(order[k]==2){if(systemStatus.batteryPresent){wchar_t pct[32]{};if(systemStatus.batteryPercent>=0)_snwprintf_s(pct,_TRUNCATE,L"%d%%",systemStatus.batteryPercent);else wcscpy_s(pct,L"?");
                    const bool customBattery=HasWorkshop(BatteryBolt()?"battery.charging":"battery.discharging");const int natural=customBattery?textW(pct,false)+Px(38):Px(48);half=(std::max)(half,natural/2);}
                else half=(std::max)(half,textW(L"Power",false)/2+Px(8));}
            statusRects[order[k]]=slot(centers[k],half);edge=(std::min)(edge,static_cast<int>(statusRects[order[k]].left));}
        {static RECT logged{};RECT now=statusRects[3];if(!EqualRect(&now,&logged)){logged=now;RECT wr{};GetWindowRect(h,&wr);DLOG(L"menu: control center icon at %ld,%ld",wr.left+(now.left+now.right)/2,wr.top+(now.top+now.bottom)/2);}}
    }
    { static RECT previous[4]{}; bool changed=false;for(int i=0;i<4;++i)if(!EqualRect(&previous[i],&statusRects[i]))changed=true;
      if(changed){for(int i=0;i<4;++i)previous[i]=statusRects[i];DLOG(L"menu: status centers wifi=%ld sound=%ld battery=%ld control=%ld",(statusRects[0].left+statusRects[0].right)/2,(statusRects[1].left+statusRects[1].right)/2,(statusRects[2].left+statusRects[2].right)/2,(statusRects[3].left+statusRects[3].right)/2);}}
    // Extras, right to left after the built-ins: extension items (registration order), promoted tray icons (the order
    // they were promoted in), then the Hidden Icons chevron. An icon-only slot is one standard item step (35.25 pt);
    // a titled item grows to its text plus 8 DIP padding per side so its pill hugs the content.
    {
        extras.clear();const int step=pt(35.25f),iconSide=Px(16),gap=Px(4);
        for(size_t i=0;i<extItems.size();++i){if(!ExtShown(i))continue;Extra e;e.kind=ExtraKind::Ext;e.id=L"ext:"+extItems[i].id;e.ext=i;e.title=extItems[i].title;
            e.hasIcon=extItems[i].icon!=nullptr;e.templ=extItems[i].templateIcon;e.contentW=(e.hasIcon?iconSide:0)+(e.title.empty()?0:textW(e.title,false)+(e.hasIcon?gap:0));extras.push_back(std::move(e));}
        for(const auto& shownId:extrasConfig.shown)for(const auto& t:trayIcons){if(t.key!=shownId||t.hidden)continue;
            Extra e;e.kind=ExtraKind::Tray;e.id=t.key;e.serial=t.serial;e.hasIcon=!t.pixels.empty();e.contentW=iconSide;extras.push_back(std::move(e));}
        if(extrasConfig.hiddenIcons&&AnythingHidden()){Extra e;e.kind=ExtraKind::Chevron;e.id=L"chevron";e.contentW=Px(10);extras.push_back(std::move(e));}
        int at=edge;
        for(auto& e:extras){const int w=(std::max)(step,e.contentW+Px(16));e.rect={at-w,0,at,height};at-=w;}
        if(!extras.empty())edge=at;
    }
    // Logo slot = the Liquid Desktop system menu heading (index 0), like the macOS top-left menu.
    const RECT logoSlot{Px(8),0,Px(44),height};
    const int appX=Px(52);const int statusLeft=(std::max)(appX,edge-Px(6));
    const int appW=textW(appName,true);const int appRight=(std::min)(statusLeft,appX+appW+Px(20));
    appRect={appX-Px(8),0,(std::max)(appX-Px(8),appRight-Px(12)),height};
    headings.clear();headings.push_back(logoSlot);int hx=(std::max)(appX,appRight);
    struct Label{std::wstring text;RECT r;bool bold;bool alignRight;};std::vector<Label> labels;
    labels.push_back({appName,RECT{appX,0,(std::max)(appX,appRight),height},true,false});
    for(size_t i=1;i<menus.menus.size();++i){const int tw=textW(menus.menus[i].title,false);
        int end=(std::min)(statusLeft,hx+tw+Px(20));if(end<=hx)break;
        headings.push_back(RECT{hx,0,end,height});labels.push_back({menus.menus[i].title,RECT{hx+Px(10),0,end-Px(4),height},false,false});hx=end;}
    RECT clockLabel=right;clockLabel.left+=Px(2)+clockPad;clockLabel.right-=Px(2)+clockPad;labels.push_back({clockText,clockLabel,false,true});
    const bool batteryShown=statusRects[2].right>statusRects[2].left;
    for(const auto& e:extras){if(e.kind!=ExtraKind::Ext||e.title.empty())continue;
        const int left=static_cast<int>(e.rect.left)+(static_cast<int>(e.rect.right-e.rect.left)-e.contentW)/2+(e.hasIcon?Px(16)+Px(4):0);
        labels.push_back({e.title,RECT{left,0,static_cast<int>(e.rect.right),height},false,false});}
    if(batteryShown&&systemStatus.batteryPresent&&HasWorkshop(BatteryBolt()?"battery.charging":"battery.discharging")){wchar_t battery[32]{};if(systemStatus.batteryPercent>=0)_snwprintf_s(battery,_TRUNCATE,L"%d%%",systemStatus.batteryPercent);else wcscpy_s(battery,L"?%");RECT text=statusRects[2];text.right-=Px(34);labels.push_back({battery,text,false,true});}
    else if(batteryShown&&!systemStatus.batteryPresent&&systemStatus.batteryKnown){RECT text=statusRects[2];text.left+=Px(8);text.right-=Px(8);labels.push_back({L"Power",text,false,true});}
    if(kit){RECT all{0,0,widthPx,height};
        if(SUCCEEDED(tk.rt->BindDC(dc,&all))){
            if(!tk.white)tk.rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),tk.white.GetAddressOf());
            tk.rt->BeginDraw();
            for(const auto& lb:labels){IDWriteTextFormat* f=lb.bold?tk.semibold.Get():tk.regular.Get();
                f->SetTextAlignment(lb.alignRight?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
                tk.rt->DrawTextW(lb.text.c_str(),static_cast<UINT32>(lb.text.size()),f,D2D1::RectF(static_cast<float>(lb.r.left),static_cast<float>(lb.r.top),static_cast<float>(lb.r.right),static_cast<float>(lb.r.bottom)),tk.white.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);}
            tk.regular->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if(tk.rt->EndDraw()==D2DERR_RECREATE_TARGET)tk.rt.Reset();}}
    {   // Logo: drawn after the text so it is part of the same luminance -> alpha mask.
        Logo().Init();Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const float s=static_cast<float>(Px(14));const float cx=(logoSlot.left+logoSlot.right)*0.5f,cy=height*0.5f;
        if(!HasWorkshop("system.logo")) Logo().Draw(g,Gdiplus::RectF(cx-s/2,cy-s/2,s,s));}
    StatusIcons(dc);
    GdiFlush();
    // Animate only active pills; a quiet bar keeps no animation timer or full-width hit pixels.
    const ULONGLONG now=GetTickCount64();
    const float step=hoverTime?std::clamp(static_cast<float>(now-hoverTime)/150.f,0.f,1.f):0.f;
    hoverTime=now; headingOpacity.resize(headings.size(),0.f); bool transitioning=false;
    auto advance=[&](float& value,bool on){const float goal=on?1.f:0.f;
        value=on?(std::min)(goal,value+step):(std::max)(goal,value-step);
        if(value!=goal)transitioning=true;};
    // macOS (reference recording 2026-11-10): no highlight on hover; the pill appears the moment a menu
    // or panel opens and disappears the moment it closes, with no fade.
    for(size_t i=0;i<headings.size();++i){headingOpacity[i]=activeMenu==static_cast<int>(i)?1.f:0.f;}
    for(int i=0;i<kStatusSlots;++i){statusOpacity[i]=(activeMenu==(-10-i)||(i==panelAnchor&&liquid::cc::IsOpen())||(i==4&&docky::notify::IsCenterOpen()))?1.f:0.f;}
    appOpacity=0.f;
    (void)step;(void)advance;
    // System status pills (Wi-Fi, sound, battery, Control Center) share one size so the selected box is
    // identical whichever is open; the clock (and future third-party items) keep their own width.
    // Reference pill: 42 x 20 pt capsule (Wi-Fi and Apple logo measured identical). Coverage() insets the
    // rect by 2 px per side, so the rect is 46 pt wide. A wider custom battery label keeps its own width.
    RECT statusPill[kStatusSlots];RECT logoPill{};
    {   const int full=static_cast<int>(std::lround(42.f*static_cast<float>(Px(1000))/1000.0f))+2*Px(2);
        auto around=[&](int c,int w){RECT r{c-w/2,0,c-w/2+w,height};return r;};
        for(int i=0;i<kStatusSlots;++i){statusPill[i]=statusRects[i];if(i!=4&&statusRects[i].right>statusRects[i].left){
            const int c=(statusRects[i].left+statusRects[i].right)/2;  // icon centre (slots are symmetric)
            const int w=static_cast<int>(statusRects[i].right-statusRects[i].left);
            statusPill[i]=around(c,(std::max)(full,i==2?w-Px(10):full));}}
        if(!headings.empty())logoPill=around((headings[0].left+headings[0].right)/2,full);}
    std::vector<RECT> extraPill(extras.size());
    {const int full=static_cast<int>(std::lround(42.f*static_cast<float>(Px(1000))/1000.0f))+2*Px(2);
     for(size_t i=0;i<extras.size();++i){const int c=(extras[i].rect.left+extras[i].rect.right)/2;const int w=(std::max)(full,extras[i].contentW+Px(20));extraPill[i]=RECT{c-w/2,0,c-w/2+w,height};}}
    const int activeExtra=activeMenu<=-100?-100-activeMenu:-1;
    if(transitioning)SetTimer(h,kHoverTimer,16,nullptr);else KillTimer(h,kHoverTimer);
    auto pixels=static_cast<std::uint32_t*>(bits);size_t nonzero=0;
    const size_t pixelCount=static_cast<size_t>(width)*height;
    std::vector<unsigned char> glyph(pixelCount);
    for(size_t i=0;i<pixelCount;++i){const auto v=pixels[i];glyph[i]=static_cast<unsigned char>((std::max)({v&255u,(v>>8)&255u,(v>>16)&255u}));}
    // Separable max filter (radius ~5 DIP): pixels near any glyph become clickable (see 'hit' below).
    std::vector<unsigned char> halo(pixelCount),rowNear(pixelCount);const int rad=Px(5);
    for(int y=0;y<height;++y){int last=-100000;for(int x=0;x<width;++x)if(glyph[static_cast<size_t>(y)*width+x])last=x,rowNear[static_cast<size_t>(y)*width+x]=1;else if(x-last<=rad)rowNear[static_cast<size_t>(y)*width+x]=1;
        last=100000;for(int x=width-1;x>=0;--x)if(glyph[static_cast<size_t>(y)*width+x])last=x;else if(last-x<=rad)rowNear[static_cast<size_t>(y)*width+x]=1;}
    for(int x=0;x<width;++x){int last=-100000;for(int y=0;y<height;++y)if(rowNear[static_cast<size_t>(y)*width+x])last=y,halo[static_cast<size_t>(y)*width+x]=1;else if(y-last<=rad)halo[static_cast<size_t>(y)*width+x]=1;
        last=100000;for(int y=height-1;y>=0;--y)if(rowNear[static_cast<size_t>(y)*width+x])last=y;else if(last-y<=rad)halo[static_cast<size_t>(y)*width+x]=1;}
    const bool lightBar=LightBar();const COLORREF ink=BarInk();  // light backdrop: black ink at 85 %, grey pill, no shadow or halo
    for(size_t i=0;i<pixelCount;++i){
        const auto a=lightBar?static_cast<unsigned>(glyph[i])*217u/255u:static_cast<unsigned>(glyph[i]);
        const int x=static_cast<int>(i%width),y=static_cast<int>(i/width);
        unsigned nearCoverage=0;
        // Soft 1px drop shadow (was a 3x3 max-dilated outline at 70%, which visibly thickened strokes).
        if(y>0){unsigned sum=0;for(int dx=-1;dx<=1;++dx){const int nx=x+dx;if(nx>=0&&nx<width)sum+=glyph[static_cast<size_t>(y-1)*width+nx]*(dx==0?2u:1u);}nearCoverage=sum/4u;}
        // Alpha 1 makes the title's whole rectangle hit-testable without an opaque strip.
        int title=-1;for(size_t j=0;j<headings.size();++j)if(x>=headings[j].left&&x<headings[j].right){title=static_cast<int>(j);break;}
        int status=-1;for(int j=0;j<kStatusSlots;++j)if(x>=statusRects[j].left&&x<statusRects[j].right){status=j;break;}
        if(status<0)for(const auto& e:extras)if(x>=e.rect.left&&x<e.rect.right){status=99;break;}
        // Highlight: a rounded pill inset from the bar edges (macOS-like), antialiased; was a full-height
        // hard-edged 40/255 rectangle over the whole slot, which read as a grey box (e.g. while Control Center is open).
        auto pill=[&](const RECT& rc){return docky::menuhit::PillCoverage(rc,height,dpi,x,y);};
        const bool app=x>=appRect.left&&x<appRect.right;
        // Open highlight only (white, rounded). Hit testing no longer paints the whole slot: an alpha-1
        // WHITE fill over every slot pixel was the faint rectangle under the status icons. Now only a ~5 px halo
        // around the glyphs (inside title/status slots) gets alpha 1, in BLACK, so it adds no light at all.
        // Status pills may be wider than their slot, so they are evaluated by pill, not by slot.
        float cover=title>=0&&headingOpacity[title]>0.f?pill(title==0?logoPill:headings[title])*headingOpacity[title]:0.f;
        if(headingOpacity.size()>0&&headingOpacity[0]>0.f)cover=(std::max)(cover,pill(logoPill)*headingOpacity[0]);
        for(int j=0;j<kStatusSlots;++j)if(statusOpacity[j]>0.f)cover=(std::max)(cover,pill(statusPill[j])*statusOpacity[j]);
        if(activeExtra>=0&&static_cast<size_t>(activeExtra)<extraPill.size())cover=(std::max)(cover,pill(extraPill[static_cast<size_t>(activeExtra)]));
        const unsigned highlight=static_cast<unsigned>(cover*(lightBar?0.15f:0.15f)*255.f);  // dark bar: 15 % white (reference pill), was 20 %
        const unsigned hit=(title>=0||status>=0||app)&&halo[i]?1u:0u;
        // A small dark outline follows glyph coverage; the rest of the bar remains transparent.
        unsigned outline=nearCoverage;
        if(!themeDark&&!lightBar){for(int yy=-1;yy<=1;++yy)for(int xx=-1;xx<=1;++xx){const int nx=x+xx,ny=y+yy;if(nx>=0&&nx<width&&ny>=0&&ny<height)outline=(std::max)(outline,static_cast<unsigned>(glyph[static_cast<size_t>(ny)*width+nx]));}}
        const unsigned shadow=lightBar?0u:themeDark?nearCoverage*110u/255u:outline*235u/255u;
        const unsigned background=(std::max)(highlight,hit);
        const unsigned opacity=a+(255u-a)*(background+(255u-background)*shadow/255u)/255u;
        // Tint the glyph mask with the semantic foreground; retain the alpha hit area.
        const unsigned inv=255u-a,base=themeDark&&!lightBar?highlight:0u;
        const unsigned haloLight=themeDark||lightBar?0u:(inv*(255u-background)*shadow)/(255u*255u);
        const unsigned red=(a*GetRValue(ink)+inv*base)/255u+haloLight;
        const unsigned g=(a*GetGValue(ink)+inv*base)/255u+haloLight;
        const unsigned b=(a*GetBValue(ink)+inv*base)/255u+haloLight;
        pixels[i]=(opacity<<24)|(red<<16)|(g<<8)|b;if(opacity)++nonzero;
    }
    DrawBatteryPill(pixels,width,height);
    const auto center=[](const RECT& r){return (r.left+r.right)*.5f;};
    // Every icon's outer contour is centred on its slot centre, which is also the selection pill centre.
    CompositeWorkshop(pixels,width,height,"system.logo",center(logoSlot),height*.5f,Px(15));
    const auto shownSlot=[](int i){return statusRects[i].right>statusRects[i].left;};
    if(shownSlot(0))CompositeWorkshop(pixels,width,height,WifiArtKey(),center(statusRects[0]),height*.5f,Px(14));
    if(shownSlot(1))CompositeWorkshop(pixels,width,height,SoundKey(),center(statusRects[1]),height*.5f,Px(14));
    if(shownSlot(2)&&systemStatus.batteryPresent)CompositeWorkshop(pixels,width,height,BatteryBolt()?"battery.charging":"battery.discharging",center(statusRects[2]),height*.5f,Px(14));
    if(shownSlot(3))CompositeWorkshop(pixels,width,height,"control-center",center(statusRects[3]),height*.5f,Px(14));
    if(shownSlot(5))CompositeWorkshop(pixels,width,height,"bluetooth",center(statusRects[5]),height*.5f,Px(14));
    // Extras: tray icons keep their colours (16 DIP like the Windows notification area); extension template icons take
    // the bar's ink. Images are area-resampled and cached at the display size (CompositeImage).
    {std::vector<std::pair<std::uint64_t,RECT>> anchors;RECT bar{};GetWindowRect(h,&bar);
     for(const auto& e:extras){if(!e.hasIcon)continue;auto key=ExtraImageKey(e);auto it=extraImages.find(key);
        if(it==extraImages.end()){WorkshopImage image;
            if(e.kind==ExtraKind::Tray){for(const auto& t:trayIcons)if(t.serial==e.serial){image.w=t.w;image.h=t.h;image.pixels=t.pixels;break;}}
            else if(e.kind==ExtraKind::Ext&&e.ext<extItems.size()&&extItems[e.ext].icon){image.w=extItems[e.ext].icon->w;image.h=extItems[e.ext].icon->h;image.pixels=extItems[e.ext].icon->pixels;}
            it=extraImages.emplace(key,std::move(image)).first;}
        const float cx=e.kind==ExtraKind::Ext&&!e.title.empty()?e.rect.left+(e.rect.right-e.rect.left-e.contentW)*.5f+Px(8):center(e.rect);
        CompositeImage(pixels,width,height,it->second,cx,height*.5f,Px(16),e.kind==ExtraKind::Ext&&e.templ?&ink:nullptr);
        if(e.kind==ExtraKind::Tray){RECT slotRect=e.rect;OffsetRect(&slotRect,bar.left,bar.top);anchors.push_back({e.serial,slotRect});}}
     anchors.insert(anchors.end(),popupAnchors.begin(),popupAnchors.end());
     if(trayStarted)docky::tray::Get().SetAnchors(anchors);}
    SyncInputWindow(h,width,height);
    // Only paint over empty pixels when the visual window can pass clicks to the separate input region.
    if (UseThemeGradient() && gradientOpacity > 0 && inputWindow && (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT)) {
        const float fadePixels = (std::max)(1, height - 1) * (gradientLength / 100.f);
        for (int y = 0; y < height && y < fadePixels; ++y) {
            const float t = y / fadePixels;
            const unsigned shade = static_cast<unsigned>(std::lround(255.f * (gradientOpacity / 100.f) * std::pow(1.f - t * t, 1.25f)));
            auto* row = pixels + static_cast<size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                const auto pixel = row[x];
                const unsigned a = pixel >> 24;
                const unsigned behind = ((255u - a) * shade + 127u) / 255u;
                if (!behind) continue;
                const unsigned gray = lightBar ? behind : 0u;
                row[x] = pixel + (behind << 24) + gray * 0x010101u;
                if (!a) ++nonzero;
            }
        }
    }
    if(devSnapshotRequested){devSnapshotRequested=false;const bool saved=SaveDevPng(pixels,static_cast<UINT>(width),static_cast<UINT>(height),devPngPath);DLOG(L"dev: menu PNG saved=%d",saved);}
    if(liquid::cc::IsOpen() && panelAnchor>=0 && panelAnchor<kStatusSlots && panelAnchor!=4){RECT bar{};GetWindowRect(h,&bar);RECT anchor=statusRects[panelAnchor];OffsetRect(&anchor,bar.left,bar.top);liquid::cc::UpdateAnchor(anchor);}
    // Asynchronous status discovery can change slot widths after a popup opens (for
    // example when the battery first appears). Keep the popup under its live slot.
    if(popup && IsWindow(popup) && ((activeMenu<=-10 && activeMenu>=-12) || activeMenu==-16)){
        const int selected=-10-activeMenu;RECT bar{},pr{};MONITORINFO mi{sizeof(mi)};
        if(GetWindowRect(h,&bar)&&GetWindowRect(popup,&pr)&&GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&mi)){
            const int pw=pr.right-pr.left,ph=pr.bottom-pr.top;
            const int x=std::clamp(static_cast<int>(bar.left+statusRects[selected].right-pw),static_cast<int>(mi.rcWork.left),std::max(static_cast<int>(mi.rcWork.left),static_cast<int>(mi.rcWork.right-pw)));
            const int y=std::clamp(static_cast<int>(bar.bottom),static_cast<int>(mi.rcWork.top),std::max(static_cast<int>(mi.rcWork.top),static_cast<int>(mi.rcWork.bottom-ph)));
            if(x!=pr.left||y!=pr.top)SetWindowPos(popup,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        }
    }
    TraceState();
    RECT wr{};GetWindowRect(h,&wr);POINT dst{wr.left,wr.top},src{0,0};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    const BOOL ok=UpdateLayeredWindow(h,screen,&dst,&size,dc,&src,0,&blend,ULW_ALPHA);
    DLOG_DEBUG(L"menu: layered render=%d pixels=%zu transparent=%zu",ok,nonzero,static_cast<size_t>(width)*height-nonzero);
    SelectObject(dc,oldBmp);DeleteObject(bmp);DeleteDC(dc);ReleaseDC(nullptr,screen);
}
void ClosePopup() {
    liquid::popup::Close();popup=nullptr;activeMenu=-1;hoverTime=GetTickCount64();
    if(barWindow)Render(barWindow);
}
// Unified Quit: signal the Desktop Manager (named event it owns); Manager closes the components it launched.
// This Menu Bar then always closes itself: before, an orphaned or independently started Menu Bar (no
// Manager, or not owned by it) ignored Quit entirely. Only our own window is closed; nothing is killed.
void RequestUnifiedQuit(){
    HANDLE ev=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\LiquidDesktopManagerQuitV1");
    if(ev){const BOOL ok=SetEvent(ev);CloseHandle(ev);DLOG(L"menu: quit request sent to Desktop Manager ok=%d",ok);}
    else DLOG(L"menu: quit requested; Desktop Manager is not running (error=%lu)",GetLastError());
    if(barWindow){DLOG(L"menu: closing this Menu Bar");PostMessageW(barWindow,WM_CLOSE,0,0);}
}
// Settings: the Desktop Manager opens it (one instance, same build). Without a Manager (a Menu Bar started on its own)
// the Settings app next to this exe is started directly, or an open one is brought forward, with this bar's settings file.
void RequestSettings(){
    if(HANDLE ev=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\LiquidDesktopSettingsOpenV1")){SetEvent(ev);CloseHandle(ev);return;}
    if(HWND open=FindWindowW(L"LiquidDesktopSettingsV01",nullptr)){ShowWindow(open,SW_RESTORE);SetForegroundWindow(open);return;}
    wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);std::wstring dir=self;dir=dir.substr(0,dir.find_last_of(L'\\'));
    const std::wstring exe=dir+L"\\LiquidDesktopSettings.exe";
    std::wstring args;if(!themeSettingsPath.empty()&&!SamePath(themeSettingsPath,liquid::settings::DefaultPath()))args=L"--settings-path \""+themeSettingsPath+L"\"";
    const auto r=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",exe.c_str(),args.empty()?nullptr:args.c_str(),dir.c_str(),SW_SHOWNORMAL));
    DLOG(L"menu: Settings started directly (no Desktop Manager) result=%lld",static_cast<long long>(r));
    if(r<=32)MessageBoxW(nullptr,L"Could not open DockOver Settings.",L"DockOver",MB_OK|MB_ICONWARNING);
}
std::vector<liquid::menu::MenuItem> CustomizeItems();bool HandleCustomize(UINT id);void TogglePanel(HWND h,int selected);constexpr UINT kCmdWidgetsId=0xE6F1;
void TogglePopup(HWND h,int selected) {
    if(popup){const bool same=activeMenu==selected;ClosePopup();if(same)return;}
    if(selected<0||static_cast<size_t>(selected)>=menus.menus.size()||static_cast<size_t>(selected)>=headings.size())return;
    openMenu=menus.menus[selected];activeMenu=selected;hoverTime=GetTickCount64();RECT r{};GetWindowRect(h,&r);
    if(openMenu.source==liquid::menu::ProviderKind::Internal){  // before the Quit separator
        using liquid::menu::ItemKind;auto at=openMenu.items.end();for(auto i=openMenu.items.begin();i!=openMenu.items.end();++i)if(i->kind==ItemKind::Separator)at=i;
        std::vector<liquid::menu::MenuItem> extra;extra.push_back({L"Menu Bar Items",ItemKind::Submenu,true,false,CustomizeItems()});
        if(docky::ext::FindDesktopHost())extra.push_back({L"Edit Desktop Widgets…",ItemKind::Command,true,false,{},kCmdWidgetsId});
        openMenu.items.insert(at,extra.begin(),extra.end());}
    POINT anchor{r.left+headings[selected].left,r.bottom};const auto target=openMenu;
    popup=liquid::popup::Show(h,anchor,openMenu.items,[target](const liquid::menu::MenuItem& item){
        if(target.source==liquid::menu::ProviderKind::Internal){if(item.commandId==0xF0F1)RequestUnifiedQuit();else if(item.commandId==0xF0F2)RequestSettings();else HandleCustomize(item.commandId);return;}
        const bool queued=liquid::menu::DispatchNativeCommand(target,item);DLOG(L"menu: native command queued=%d id=%u",queued,item.commandId);
    },[]{popup=nullptr;activeMenu=-1;hoverTime=GetTickCount64();if(barWindow)Render(barWindow);});
    if(!popup)activeMenu=-1;Render(h);
}
void TogglePanel(HWND h,int selected){
    if(popup)ClosePopup();
    if(liquid::cc::IsOpen()){
        pendingPanel=(panelAnchor==selected)?-1:selected;
        RECT unused{};liquid::cc::Toggle(unused,h,kControlCenterChanged); // closes the existing shared panel
    } else {
        pendingPanel=-1;panelAnchor=selected;RECT r{};GetWindowRect(h,&r);RECT anchor=statusRects[selected];OffsetRect(&anchor,r.left,r.top);
        if(selected==1)liquid::cc::ToggleSound(anchor,h,kControlCenterChanged);
        else if(selected==0)liquid::cc::ToggleWifi(anchor,h,kControlCenterChanged);
        else if(selected==5)liquid::cc::ToggleBluetooth(anchor,h,kControlCenterChanged);
        else liquid::cc::Toggle(anchor,h,kControlCenterChanged);
    }
    hoverTime=GetTickCount64();Render(h);
}
void ToggleControlCenter(HWND h){TogglePanel(h,3);}
// The clock opens the Notification Center (macOS). Other menus and panels close first; the clock gets the pill.
void ToggleNotificationCenter(HWND h){
    if(popup)ClosePopup();
    if(liquid::cc::IsOpen()&&!docky::notify::IsCenterOpen())TogglePanel(h,panelAnchor);
    RECT r{};GetWindowRect(h,&r);RECT anchor=statusRects[4];OffsetRect(&anchor,r.left,r.top);
    docky::notify::ToggleCenter(anchor,h,kNotificationCenterChanged);
}
std::vector<liquid::menu::MenuItem> BuildStatusItems(int selected){
    using liquid::menu::ItemKind;
    std::vector<liquid::menu::MenuItem> items;
    if(selected==0){
        const wchar_t* status=systemStatus.wifi==liquid::status::Wifi::Connected?L"Wi-Fi: Connected":systemStatus.wifi==liquid::status::Wifi::Disconnected?L"Wi-Fi: Disconnected":L"Wi-Fi: Unavailable";
        items.push_back({status,ItemKind::Command,false,false,{}});
        if(systemStatus.wifi==liquid::status::Wifi::Connected && !systemStatus.ssid.empty())items.push_back({L"Network: "+systemStatus.ssid,ItemKind::Command,false,false,{}});
        if(systemStatus.wifiListAvailable){items.push_back({L"Available networks",ItemKind::Command,false,false,{}});
            for(const auto& name:systemStatus.wifiNetworks)items.push_back({name,ItemKind::Command,false,false,{}});
            if(systemStatus.wifiNetworks.empty())items.push_back({L"No networks reported",ItemKind::Command,false,false,{}});}
        else items.push_back({L"Network list unavailable",ItemKind::Command,false,false,{}});
        items.push_back({L"Refresh network list",ItemKind::Command,true,false,{},0xE103});
    }else if(selected==1){
        std::wstring status=!systemStatus.audioAvailable?L"Audio: Unavailable":systemStatus.muted?L"Volume: Muted":L"Volume: "+std::to_wstring(systemStatus.volumePercent)+L"%";
        items.push_back({status,ItemKind::Command,false,false,{}});
        if(!systemStatus.outputDevice.empty())items.push_back({L"Output: "+systemStatus.outputDevice,ItemKind::Command,false,false,{}});
    }else{
        std::wstring level=!systemStatus.batteryKnown?L"Battery: Unknown":!systemStatus.batteryPresent?L"Battery: Not present":systemStatus.batteryPercent<0?L"Battery: Unknown":L"Battery: "+std::to_wstring(systemStatus.batteryPercent)+L"%";
        items.push_back({level,ItemKind::Command,false,false,{}});
        if(systemStatus.batteryPresent)items.push_back({systemStatus.charging?L"Charging":systemStatus.powerSource==liquid::status::PowerSource::AC?L"Power Source: Power Adapter":L"Power Source: Battery",ItemKind::Command,false,false,{}});
        const wchar_t* source=systemStatus.powerSource==liquid::status::PowerSource::AC?L"Power source: AC":systemStatus.powerSource==liquid::status::PowerSource::Battery?L"Power source: Battery":L"Power source: Unknown";
        items.push_back({source,ItemKind::Command,false,false,{}});
    }
    items.push_back({L"",ItemKind::Separator,false,false,{}});
    items.push_back({L"Open Control Center…",ItemKind::Command,true,false,{},0xE101});
    items.push_back({selected==0?L"Network Settings…":selected==1?L"Sound Settings…":L"Battery Settings…",ItemKind::Command,true,false,{},0xE102});
    return items;
}
void ToggleInputPopup(HWND h);
void ToggleStatusPopup(HWND h,int selected){
    if(selected==6){ToggleInputPopup(h);return;}
    if(selected<0 || selected>2)return;
    if(popup){const bool same=activeMenu==(-10-selected);ClosePopup();if(same)return;}
    if(liquid::cc::IsOpen())TogglePanel(h,panelAnchor); // close our panel before switching surfaces
    auto items=BuildStatusItems(selected);
    const wchar_t* uri=selected==0?L"ms-settings:network-wifi":selected==1?L"ms-settings:sound":L"ms-settings:powersleep";
    RECT r{};GetWindowRect(h,&r);POINT anchor{r.left+statusRects[selected].right-Px(260),r.bottom};
    activeMenu=-10-selected;hoverTime=GetTickCount64();
    DLOG(L"menu: opening status popup %d",selected);
    popup=liquid::popup::Show(h,anchor,std::move(items),[h,uri](const liquid::menu::MenuItem& item){
        if(item.commandId==0xE101)ToggleControlCenter(h);
        else if(item.commandId==0xE102)ShellExecuteW(nullptr,L"open",uri,nullptr,nullptr,SW_SHOWNORMAL);
        else if(item.commandId==0xE103)RequestStatus(); // reload OS-reported list asynchronously; reopen to view

    },[]{popup=nullptr;activeMenu=-1;hoverTime=GetTickCount64();if(barWindow)Render(barWindow);});
    if(!popup)activeMenu=-1;Render(h);
}
// Input menu popup: the installed keyboard layouts (the current one ticked) and Windows' keyboard settings.
void ToggleInputPopup(HWND h){
    if(popup){const bool same=activeMenu==-16;ClosePopup();if(same)return;}
    if(liquid::cc::IsOpen())TogglePanel(h,panelAnchor);
    inputLayout=ForegroundLayout();
    RECT r{};GetWindowRect(h,&r);POINT anchor{r.left+statusRects[6].right-Px(260),r.bottom};
    activeMenu=-16;hoverTime=GetTickCount64();
    popup=docky::statuspopup::ShowInputMenu(h,anchor,false,inputLayout,inputTarget,[]{popup=nullptr;activeMenu=-1;hoverTime=GetTickCount64();if(barWindow)Render(barWindow);});
    if(!popup)activeMenu=-1;Render(h);
}
// ---- Extras interaction ------------------------------------------------------------------------------------
std::shared_ptr<const liquid::menu::Icon> RowIcon(const std::vector<std::uint32_t>& px,UINT w,UINT h,bool templ){
    if(px.empty()||!w||!h||px.size()!=static_cast<size_t>(w)*h)return nullptr;
    auto icon=std::make_shared<liquid::menu::Icon>();icon->w=w;icon->h=h;icon->pixels=px;icon->templ=templ;return icon;
}
std::wstring ExtLabel(const docky::extras::StatusItem& item){return !item.tooltip.empty()?item.tooltip:!item.title.empty()?item.title:item.id;}
constexpr UINT kCmdBuiltin=0xE200,kCmdTray=0xE300,kCmdExt=0xE500,kCmdHiddenToggle=0xE6F0;
constexpr UINT kRowTray=0x1000,kRowExt=0x3000,kRowExtMenu=0x5000;
constexpr UINT kBuiltinCount=6;
const wchar_t* const kBuiltinIds[kBuiltinCount]={L"builtin:wifi",L"builtin:sound",L"builtin:battery",L"builtin:control",L"builtin:bluetooth",L"builtin:input"};
constexpr int kBuiltinSlot[kBuiltinCount]={0,1,2,3,5,6};
std::vector<std::wstring> customizeTrayKeys;
std::vector<liquid::menu::MenuItem> CustomizeItems(){
    using liquid::menu::ItemKind;std::vector<liquid::menu::MenuItem> items;
    const wchar_t* names[kBuiltinCount]={L"Wi-Fi",L"Sound",L"Battery",L"Control Center",L"Bluetooth",L"Input Menu"};
    for(UINT i=0;i<kBuiltinCount;++i)items.push_back({names[i],ItemKind::Command,true,docky::extras::IsShown(extrasConfig,kBuiltinIds[i]),{},kCmdBuiltin+i});
    customizeTrayKeys.clear();
    if(!extItems.empty()||!trayIcons.empty())items.push_back({L"",ItemKind::Separator,false,false,{}});
    for(size_t i=0;i<extItems.size()&&i<200;++i){liquid::menu::MenuItem m{ExtLabel(extItems[i]),ItemKind::Command,true,ExtShown(i),{},kCmdExt+static_cast<UINT>(i)};
        if(extItems[i].icon)m.icon=RowIcon(extItems[i].icon->pixels,extItems[i].icon->w,extItems[i].icon->h,extItems[i].templateIcon);items.push_back(std::move(m));}
    for(const auto& t:trayIcons){if(t.hidden||customizeTrayKeys.size()>=200||std::find(customizeTrayKeys.begin(),customizeTrayKeys.end(),t.key)!=customizeTrayKeys.end())continue;
        liquid::menu::MenuItem m{docky::tray::DisplayName(t),ItemKind::Command,true,TrayShown(t),{},kCmdTray+static_cast<UINT>(customizeTrayKeys.size())};
        m.icon=RowIcon(t.pixels,t.w,t.h,false);customizeTrayKeys.push_back(t.key);items.push_back(std::move(m));}
    items.push_back({L"",ItemKind::Separator,false,false,{}});
    items.push_back({L"Hidden Icons Button",ItemKind::Command,true,extrasConfig.hiddenIcons,{},kCmdHiddenToggle});
    return items;
}
void ToggleShown(const std::wstring& id){docky::extras::SetShown(extrasConfig,id,!docky::extras::IsShown(extrasConfig,id));DLOG(L"menu extras: %ls %ls",id.c_str(),docky::extras::IsShown(extrasConfig,id)?L"shown":L"hidden");}
bool HandleCustomize(UINT id){
    if(id>=kCmdBuiltin&&id<kCmdBuiltin+kBuiltinCount){
        const UINT index=id-kCmdBuiltin;ToggleShown(kBuiltinIds[index]);
        const int status=kBuiltinSlot[index];  // close a panel whose item just disappeared
        if(liquid::cc::IsOpen()&&panelAnchor==status&&!docky::extras::IsShown(extrasConfig,kBuiltinIds[index])&&barWindow)TogglePanel(barWindow,panelAnchor);
    }
    else if(id>=kCmdTray&&id<kCmdTray+customizeTrayKeys.size())ToggleShown(customizeTrayKeys[id-kCmdTray]);
    else if(id>=kCmdExt&&id<kCmdExt+extItems.size())ToggleShown(L"ext:"+extItems[id-kCmdExt].id);
    else if(id==kCmdHiddenToggle)extrasConfig.hiddenIcons=!extrasConfig.hiddenIcons;
    else if(id==kCmdWidgetsId){HWND desktop=docky::ext::FindDesktopHost();const bool sent=docky::ext::SendJson(desktop,extHost,R"({"v":1,"type":"widget.gallery"})");DLOG(L"menu: widget gallery request sent=%d",sent?1:0);return true;}
    else return false;
    SaveExtrasConfig();extraImages.clear();if(barWindow)Render(barWindow);return true;
}
void NotifyExtension(const docky::extras::StatusItem& item,const std::string& json){
    if(!item.notify||!IsWindow(item.notify))return;DWORD pid=0;GetWindowThreadProcessId(item.notify,&pid);if(pid)AllowSetForegroundWindow(pid);
    const bool ok=docky::ext::SendJson(item.notify,extHost,json,300);DLOG(L"menu extras: event to %ls ok=%d",item.id.c_str(),ok?1:0);
}
void RunExtension(const std::wstring& run,const std::wstring& args,const std::wstring& folder){
    if(run.empty())return;const auto r=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",run.c_str(),args.empty()?nullptr:args.c_str(),folder.empty()?nullptr:folder.c_str(),SW_SHOWNORMAL));
    DLOG(L"menu extras: run %ls result=%lld",run.c_str(),static_cast<long long>(r));
}
// Extension item activated (in the bar or from the Hidden Icons popup): its menu, else a click event / run command.
void ActivateExt(HWND h,size_t index,bool right,POINT anchor,int code){
    if(index>=extItems.size())return;const auto item=extItems[index];
    if(item.menu.empty()){RunExtension(item.run,item.args,item.folder);NotifyExtension(item,docky::extras::ClickedEvent(item.id,right));return;}
    using liquid::menu::ItemKind;std::vector<liquid::menu::MenuItem> rows;
    for(size_t k=0;k<item.menu.size();++k){const auto& e=item.menu[k];
        if(e.separator)rows.push_back({L"",ItemKind::Separator,false,false,{}});
        else rows.push_back({e.title,ItemKind::Command,e.enabled,e.checked,{},kRowExtMenu+static_cast<UINT>(k)});}
    activeMenu=code;hoverTime=GetTickCount64();
    popup=liquid::popup::Show(h,anchor,std::move(rows),[item](const liquid::menu::MenuItem& row){
        if(row.commandId<kRowExtMenu||row.commandId>=kRowExtMenu+item.menu.size())return;const auto& e=item.menu[row.commandId-kRowExtMenu];
        if(!e.run.empty())RunExtension(e.run,e.args,item.folder);NotifyExtension(item,docky::extras::MenuEvent(item.id,e.id));
    },[]{popup=nullptr;activeMenu=-1;hoverTime=GetTickCount64();if(barWindow)Render(barWindow);});
    if(!popup)activeMenu=-1;Render(h);
}
std::vector<std::uint64_t> popupTray;std::vector<size_t> popupExt;
POINT ExtraAnchor(HWND h,size_t i){RECT r{};GetWindowRect(h,&r);return POINT{r.left+extras[i].rect.left,r.bottom};}
void ToggleHiddenPopup(HWND h,size_t extraIndex){
    const int code=-100-static_cast<int>(extraIndex);
    if(popup){const bool same=activeMenu==code;ClosePopup();if(same)return;}
    if(liquid::cc::IsOpen())TogglePanel(h,panelAnchor);
    using liquid::menu::ItemKind;std::vector<liquid::menu::MenuItem> items;popupTray.clear();popupExt.clear();
    for(size_t i=0;i<extItems.size();++i){if(ExtShown(i))continue;
        liquid::menu::MenuItem m{ExtLabel(extItems[i]),ItemKind::Command,true,false,{},kRowExt+static_cast<UINT>(popupExt.size())};
        if(extItems[i].icon)m.icon=RowIcon(extItems[i].icon->pixels,extItems[i].icon->w,extItems[i].icon->h,extItems[i].templateIcon);popupExt.push_back(i);items.push_back(std::move(m));}
    for(const auto& t:trayIcons){if(t.hidden||TrayShown(t))continue;
        liquid::menu::MenuItem m{docky::tray::DisplayName(t),ItemKind::Command,true,false,{},kRowTray+static_cast<UINT>(popupTray.size())};
        m.icon=RowIcon(t.pixels,t.w,t.h,false);popupTray.push_back(t.serial);items.push_back(std::move(m));}
    const size_t firstTrayRow=popupExt.size();
    if(items.empty())items.push_back({L"No hidden icons",ItemKind::Command,false,false,{}});
    items.push_back({L"",ItemKind::Separator,false,false,{}});
    items.push_back({L"Menu Bar Items",ItemKind::Submenu,true,false,CustomizeItems()});
    activeMenu=code;hoverTime=GetTickCount64();const POINT anchor=ExtraAnchor(h,extraIndex);
    DLOG(L"menu: hidden icons popup (%zu tray, %zu extension)",popupTray.size(),popupExt.size());
    popup=liquid::popup::Show(h,anchor,std::move(items),[h,code](const liquid::menu::MenuItem& item){
        const bool right=liquid::popup::LastSelectionSecondary();
        if(item.commandId>=kRowTray&&item.commandId<kRowTray+popupTray.size()){POINT pt{};GetCursorPos(&pt);docky::tray::Get().Click(popupTray[item.commandId-kRowTray],right,pt);}
        else if(item.commandId>=kRowExt&&item.commandId<kRowExt+popupExt.size())PostMessageW(h,kExtOpenMenu,popupExt[item.commandId-kRowExt],MAKELPARAM(right?1:0,static_cast<WORD>(-code)));
        else HandleCustomize(item.commandId);
    },[]{popup=nullptr;activeMenu=-1;popupAnchors.clear();hoverTime=GetTickCount64();if(barWindow)Render(barWindow);});
    if(!popup){activeMenu=-1;Render(h);return;}
    // Shell_NotifyIconGetRect answers with the row, so apps that place their own flyout next to "their icon" use the row.
    RECT pr{};GetWindowRect(popup,&pr);popupAnchors.clear();
    for(size_t j=0;j<popupTray.size();++j){const int row=static_cast<int>(firstTrayRow+j);RECT rr{pr.left+Px(6),pr.top+Px(6+24*row),pr.right-Px(6),pr.top+Px(6+24*row+24)};popupAnchors.push_back({popupTray[j],rr});}
    Render(h);
}
void ClickExtra(HWND h,size_t i,bool right){
    if(i>=extras.size())return;const Extra e=extras[i];
    if(e.kind==ExtraKind::Chevron){ToggleHiddenPopup(h,i);return;}
    const int code=-100-static_cast<int>(i);
    if(popup){const bool same=activeMenu==code;ClosePopup();if(same)return;}
    if(e.kind==ExtraKind::Tray){RECT r{};GetWindowRect(h,&r);docky::tray::Get().Click(e.serial,right,POINT{r.left+(e.rect.left+e.rect.right)/2,r.bottom});return;}
    if(liquid::cc::IsOpen())TogglePanel(h,panelAnchor);
    ActivateExt(h,e.ext,right,ExtraAnchor(h,i),code);
}
bool SameItems(const std::vector<docky::extras::StatusItem>& a,const std::vector<docky::extras::StatusItem>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];if(x.id!=y.id||x.title!=y.title||x.tooltip!=y.tooltip||x.iconPath!=y.iconPath||x.templateIcon!=y.templateIcon||x.menu.size()!=y.menu.size()||x.run!=y.run)return false;
        for(size_t k=0;k<x.menu.size();++k)if(x.menu[k].title!=y.menu[k].title||x.menu[k].enabled!=y.menu[k].enabled||x.menu[k].checked!=y.menu[k].checked)return false;}
    return true;
}
// Manifest items are re-read every few seconds (cheap: a handful of small files); pushed items stay as they are.
void ReloadManifests(HWND h,bool force){
    auto next=docky::extras::LoadManifests(docky::ext::ExtensionsRoot());
    std::vector<docky::extras::StatusItem> current;for(const auto& i:extItems)if(i.fromManifest)current.push_back(i);
    if(!force&&SameItems(current,next))return;
    for(auto& i:next)LoadExtImage(i);
    std::vector<docky::extras::StatusItem> merged=std::move(next);
    for(auto& i:extItems)if(!i.fromManifest&&std::none_of(merged.begin(),merged.end(),[&](const docky::extras::StatusItem& m){return m.id==i.id;}))merged.push_back(std::move(i));
    extItems=std::move(merged);extraImages.clear();DLOG(L"menu extras: %zu status item(s) after manifest reload",extItems.size());if(h)Render(h);
}
void PruneExtensions(HWND h){
    const auto before=extItems.size();
    extItems.erase(std::remove_if(extItems.begin(),extItems.end(),[](const docky::extras::StatusItem& i){return !i.fromManifest&&i.notify&&!IsWindow(i.notify);}),extItems.end());
    if(extItems.size()!=before){extraImages.clear();DLOG(L"menu extras: removed %zu item(s) of closed apps",before-extItems.size());Render(h);}
}
LRESULT CALLBACK ExtHostProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COPYDATA){
        std::string json;if(!docky::ext::ReadJson(l,json))return 0;
        docky::extras::Message msg;std::string error;
        if(!docky::extras::ParseMessage(json,msg,error)){DLOG(L"menu extras: message rejected (%hs)",error.c_str());return 0;}
        if(!msg.item.notify&&w)msg.item.notify=reinterpret_cast<HWND>(w);
        auto it=std::find_if(extItems.begin(),extItems.end(),[&](const docky::extras::StatusItem& i){return i.id==msg.item.id;});
        if(msg.action==docky::extras::Action::Remove){if(it==extItems.end())return 0;extItems.erase(it);}
        else{if(it!=extItems.end()&&it->fromManifest){DLOG(L"menu extras: %ls belongs to a manifest; push ignored",msg.item.id.c_str());return 0;}
            LoadExtImage(msg.item);if(it!=extItems.end())*it=std::move(msg.item);else{if(extItems.size()>=64)return 0;extItems.push_back(std::move(msg.item));}}
        extraImages.clear();if(barWindow)Render(barWindow);return 1;
    }
    return DefWindowProcW(h,m,w,l);
}
// The tray and extension hosts exist once per session (status_hosts.h): the menu bar holds kHostsMutex while it hosts
// them. While the menu bar was turned off the Dock had them; it releases them when the menu bar is turned on again.
constexpr UINT_PTR kHostsRetryTimer=32;bool hostsDev=false;HANDLE hostsMutex=nullptr;
bool StartHosts(HWND h){
    if(!hostsMutex)hostsMutex=CreateMutexW(nullptr,FALSE,docky::statushost::kHostsMutex);
    const DWORD r=hostsMutex?WaitForSingleObject(hostsMutex,0):WAIT_FAILED;
    if(r!=WAIT_OBJECT_0&&r!=WAIT_ABANDONED){DLOG(L"menu: tray / extension hosts busy (Dock); retrying");return false;}
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    {WNDCLASSW hostClass{};hostClass.hInstance=instance;hostClass.lpfnWndProc=ExtHostProc;hostClass.lpszClassName=docky::ext::kMenuBarHostClass;
     if(RegisterClassW(&hostClass)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS){
        extHost=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,docky::ext::kMenuBarHostClass,L"DockOver Menu Bar Extras",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
        if(extHost){ChangeWindowMessageFilterEx(extHost,WM_COPYDATA,MSGFLT_ALLOW,nullptr);PostMessageW(HWND_BROADCAST,RegisterWindowMessageW(L"dockyHostStarted"),0,0);}}}
    ReloadManifests(nullptr,true);
    if(hostsDev&&extrasConfig.trayHost){trayStarted=docky::tray::Get().Start(h,kTrayChanged);DLOG(L"menu: tray host %ls",trayStarted?L"started":L"unavailable");}
    if(h)Render(h);
    return true;
}
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE: barWindow=h;{std::lock_guard<std::mutex> lock(statusShared->mutex);statusShared->window=h;}{std::lock_guard<std::mutex> lock(workshopShared->mutex);workshopShared->window=h;}Place(h);Update(h);RequestStatus();RequestWorkshop();SetTimer(h,1,1000,nullptr);SetTimer(h,4,5000,nullptr);SetTimer(h,kFullscreenTimer,250,nullptr);SetTimer(h,kBackdropTimer,500,nullptr);inputLayout=ForegroundLayout();SetTimer(h,kInputTimer,300,nullptr);return 0;
    case WM_TIMER: if(w==kDragClipTimer){  // safety: never leave the cursor confined; re-assert if the move loop reset it
            // Button up: OnMoveSizeEnd normally follows at once (and needs the fill state); only clean up if it does not come.
            static ULONGLONG upSince=0;const bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
            if(!dragClip.target||!IsWindow(dragClip.target)){upSince=0;ReleaseDragClip();}
            else if(!down){if(!upSince)upSince=GetTickCount64();else if(GetTickCount64()-upSince>400){upSince=0;ReleaseDragClip();}}
            else if(dragClip.active){upSince=0;if(!dragClip.pending){RECT now{};if(GetClipCursor(&now)&&!EqualRect(&now,&dragClip.clip)){++dragClip.resets;ClipCursor(&dragClip.clip);}}CalibrateDragClip();DragDwell();HideSnapUi();}
            return 0;}
        if(w==kBackdropTimer){if(SampleBarBackdrop(h))Render(h);return 0;}
        if(w==kHostsRetryTimer){if(StartHosts(h))KillTimer(h,kHostsRetryTimer);return 0;}
        if(w==kInputTimer){const HKL now=ForegroundLayout();if(now!=inputLayout){inputLayout=now;if(extrasConfig.input)Render(h);}return 0;}
        if(w==kTrayTimer){KillTimer(h,kTrayTimer);trayIcons=docky::tray::Get().Snapshot();extraImages.clear();Render(h);return 0;}
        if(w==kFullscreenTimer){MaintainButtonFullscreen();
            {const bool listed=!menuGameWatch.Poll(themeSettingsPath,gameCompatibility).empty();if(listed!=listedGameMenu){listedGameMenu=listed;DLOG(L"menu: games list %ls",listed?L"program running: bar hidden":L"no listed program running");}}
            SetFullscreenApp(h,FullscreenForeground()||listedGameMenu);SyncGameCompatibility(h);
            if(gameSuppressed){DropRawMouse();return 0;}
            if(fullscreenApp){POINT pt{};MONITORINFO mi{sizeof(mi)};
                const bool top=!GameOwnsPointer()&&GetCursorPos(&pt)&&GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTOPRIMARY),&mi)&&pt.y<=mi.rcMonitor.top+1&&pt.x>=mi.rcMonitor.left&&pt.x<mi.rcMonitor.right;
                if(top||barRevealed){EnsureRawMouse();if(top&&!topEdgeSince)topEdgeSince=GetTickCount64();}else{DropRawMouse();topEdgeSince=0;}}
            FullscreenRevealTick(h);return 0;}
        if((w==1||w==4)&&fullscreenApp&&!barRevealed)return 0;  // game mode: nothing of the bar is visible; no polling
        if(w==kDevNotifyTimer){KillTimer(h,kDevNotifyTimer);  // dev: sample notifications (no Windows notifications involved)
            if(devNotification){  // the reference screenshot's first notifications (two from one app: a stack)
                docky::notify::DebugBanner(L"Amazon",L"A Black Friday Week Deal \u26A1",L"Limited-time offer: Save 15% on ESR for iPad Pro in your cart");
                docky::notify::DebugBanner(L"WeChat",L"Aaa.\u5154\u5154\uff01",L"\u8981\u70b9\u5317\u4eac\u697c\u5417");
                docky::notify::DebugBanner(L"Weee!",L"\u26A1 $129.99\u62a2SK-II\u5927\u7ea2\u74f6\u9762\u971c\uff01\u4ec5\u6b64\u4e00\u5929",L"$3.99 \u808c\u7f8e\u7cbe\u8865\u6c34\u9762\u819c\u7c89\u76d2 $1.99AGF\u62ff\u94c1/8\u888b");
                docky::notify::DebugBanner(L"Amazon",L"4+ star savings for you!",L"Get 21% off BONTEC Desk Keyboard Tray for Black Friday Week");}
            if(devNotificationCenter)ToggleNotificationCenter(h);return 0;}
        if(w==kReleaseCheckTimer){KillTimer(h,kReleaseCheckTimer);if(!dragClip.active)ClampBelowBar(GetForegroundWindow(),L"button released");return 0;}
        if(w==1){Update(h);if(RefreshTheme())Render(h);const auto revision=liquid::settings::ReadRevision(themeSettingsPath);if(revision!=settingsRevision){settingsRevision=revision;const auto current=liquid::settings::Load(themeSettingsPath);docky::ui::SetLanguage(current.value.appearance.language);hideSnapLayouts=current.value.dock.hideSnapLayouts;gameCompatibility=current.value.dock.gameCompatibility;docky::notify::SetCapture(current.value.menuBar.captureNotifications);docky::notify::SetHideSystemBanners(current.value.menuBar.hideSystemNotifications);if(menuScalePercent!=current.value.menuBar.scalePercent){menuScalePercent=current.value.menuBar.scalePercent;ClosePopup();Place(h);Render(h);}docky::vblank::SetCaps(current.value.performance.animationFps,current.value.performance.captureFps);DLOG(L"shared settings changed; status=%d hideSnap=%d",static_cast<int>(current.status),hideSnapLayouts?1:0);}const auto wr=liquid::settings::ReadRevision(workshopRoot+L"\\active.txt");if(wr!=workshopRevision){workshopRevision=wr;RequestWorkshop();}}else if(w==2)DestroyWindow(h);else if(w==4){RequestStatus();RequestWorkshop();ReloadManifests(h,false);PruneExtensions(h);}else if(w==kHoverTimer)Render(h);else if(w==6&&devTrace!=INVALID_HANDLE_VALUE&&devInitialPopup>=0){KillTimer(h,6);const int selected=devInitialPopup;devInitialPopup=-1;if(selected==9){for(size_t i=0;i<extras.size();++i)if(extras[i].kind==ExtraKind::Chevron){ToggleHiddenPopup(h,i);break;}}else if(selected==3||selected==1||selected==0||selected==5)TogglePanel(h,selected);else ToggleStatusPopup(h,selected);}else if(w==7&&devTrace!=INVALID_HANDLE_VALUE&&devInitialKey){
        KillTimer(h,7);HWND target=popup;if(!target&&liquid::cc::IsOpen())target=FindWindowW(L"LiquidControlCenter",L"DockOver Control Center");DWORD pid=0;if(target)GetWindowThreadProcessId(target,&pid);
        bool sent=false;if(target&&pid==GetCurrentProcessId()){if(devInitialKey==0x10000){const UINT testDpi=GetDpiForWindow(target);const LPARAM pt=MAKELPARAM(MulDiv(120,testDpi,96),MulDiv(126,testDpi,96));sent=PostMessageW(target,WM_LBUTTONDOWN,MK_LBUTTON,pt)&&PostMessageW(target,WM_LBUTTONUP,0,pt);}else sent=PostMessageW(target,WM_KEYDOWN,static_cast<WPARAM>(devInitialKey),0)!=FALSE;}
        DLOG(L"dev: own-window key=%d sent=%d",devInitialKey,sent);devInitialKey=0;
    }else if(w==8&&devTrace!=INVALID_HANDLE_VALUE)TraceState();else if(w==9&&devTrace!=INVALID_HANDLE_VALUE){KillTimer(h,9);devSnapshotRequested=true;Render(h);}else if(w==10&&devTrace!=INVALID_HANDLE_VALUE&&devNextPopup>=0){KillTimer(h,10);const int selected=devNextPopup;devNextPopup=-1;if(selected==1||selected==3||selected==0||selected==5)TogglePanel(h,selected);else ToggleStatusPopup(h,selected);}else if(w==11&&devTrace!=INVALID_HANDLE_VALUE){KillTimer(h,11);const bool accepted=liquid::popup::RequestDiagnosticSnapshot(popup,devPopupBmpPath);DLOG(L"dev: popup snapshot requested=%d",accepted);}else if(w==12&&devTrace!=INVALID_HANDLE_VALUE&&devRoute>=0){
        KillTimer(h,12);const int index=devRoute;devRoute=-1;
        if(inputWindow&&statusRects[index].right>statusRects[index].left){const RECT r=statusRects[index];const int x=r.left+Px(3),y=(r.top+r.bottom)/2;
          SendMessageW(inputWindow,WM_MOUSEMOVE,0,MAKELPARAM(x,y));SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));SendMessageW(inputWindow,WM_LBUTTONUP,0,MAKELPARAM(x,y));DLOG(L"dev: routed own-window input status=%d",index);}
    }return 0;
    case kWorkshopReady:{WorkshopImages next;{std::lock_guard<std::mutex> lock(workshopShared->mutex);next=workshopShared->images;}if(next!=workshopImages){workshopImages=std::move(next);DLOG(L"workshop: menu icons updated count=%zu",workshopImages.size());Render(h);}if(w)RequestWorkshop();return 0;}
    case kStatusReady:{liquid::status::Snapshot value;{std::lock_guard<std::mutex> lock(statusShared->mutex);value=statusShared->result;}if(!(value==systemStatus)){systemStatus=value;DLOG(L"status: wifi=%d audio=%d muted=%d volume=%d battery=%d percent=%d charging=%d",static_cast<int>(value.wifi),value.audioAvailable,value.muted,value.volumePercent,value.batteryPresent,value.batteryPercent,value.charging);
        if(popup&&activeMenu<=-10&&activeMenu>=-12)liquid::popup::UpdateItems(popup,BuildStatusItems(-10-activeMenu));Render(h);}return 0;}
    case WM_POWERBROADCAST:RequestStatus();return TRUE;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_NCHITTEST:if(inputWindow)return HTTRANSPARENT;break;
    case WM_MOUSEMOVE: {
        int next=-1,st=-1;POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};for(size_t i=0;i<headings.size();++i)if(PtInRect(&headings[i],pt)){next=static_cast<int>(i);break;}
        for(int i=0;i<kStatusSlots;++i)if(PtInRect(&statusRects[i],pt)){st=i;break;}
        const bool app=PtInRect(&appRect,pt)!=FALSE;
        if(next!=hoverIndex||st!=statusHover||app!=appHover){hoverIndex=next;statusHover=st;appHover=app;hoverTime=GetTickCount64();Render(h);}
        if(!inputWindow&&!tracking){TRACKMOUSEEVENT e{sizeof(e),TME_LEAVE,h,0};TrackMouseEvent(&e);tracking=true;}
        return 0;
    }
    case WM_MOUSELEAVE:tracking=false;if(hoverIndex!=-1||statusHover!=-1||appHover){hoverIndex=-1;statusHover=-1;appHover=false;hoverTime=GetTickCount64();Render(h);}return 0;
    case WM_RBUTTONUP:{POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        for(size_t i=0;i<extras.size();++i)if(PtInRect(&extras[i].rect,pt)){ClickExtra(h,i,true);return 0;}return 0;}
    case WM_LBUTTONUP:{POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        for(size_t i=0;i<extras.size();++i)if(PtInRect(&extras[i].rect,pt)){ClickExtra(h,i,false);return 0;}
        if(PtInRect(&statusRects[4],pt)){ToggleNotificationCenter(h);return 0;}
        for(int i=0;i<kStatusSlots;++i)if(i!=4&&PtInRect(&statusRects[i],pt)){if(i==3||i==1||i==0||i==5)TogglePanel(h,i);else ToggleStatusPopup(h,i);return 0;}
        for(size_t i=0;i<headings.size();++i)if(PtInRect(&headings[i],pt)){TogglePopup(h,static_cast<int>(i));break;}return 0;}
    case kDismiss:ClosePopup();return 0;
    case kTrayChanged:SetTimer(h,kTrayTimer,120,nullptr);return 0;  // animated icons modify often: coalesce
    case kExtOpenMenu:{const size_t index=static_cast<size_t>(w);const bool right=LOWORD(l)!=0;const int code=-static_cast<int>(HIWORD(l));
        const size_t chevron=static_cast<size_t>(-100-code);POINT anchor{};if(chevron<extras.size())anchor=ExtraAnchor(h,chevron);else{RECT r{};GetWindowRect(h,&r);anchor={r.right-Px(260),r.bottom};}
        ActivateExt(h,index,right,anchor,code);return 0;}
    case kControlCenterChanged:
        if(w==1){panelAnchor=-1;const int next=pendingPanel;pendingPanel=-1;if(next>=0)TogglePanel(h,next);}
        RequestStatus();hoverTime=GetTickCount64();Render(h);return 0;
    case WM_APP+91:if(w>=80&&w<=160&&menuScalePercent!=static_cast<int>(w)){menuScalePercent=static_cast<int>(w);ClosePopup();Place(h);Render(h);}return 0;
    case WM_APP+44:ToggleControlCenter(h);return 0;
    case kNotificationCenterChanged:if(w==2){HandleCustomize(kCmdWidgetsId);return 0;}  // the panel's "Edit Widgets"
        // The open panel blurs the desktop under the bar too: keep the bar's own glyphs out of that capture (else the
        // blur shows soft copies of them under the sharp ones). Restored when the panel closes.
        SetWindowDisplayAffinity(h,w==1?WDA_EXCLUDEFROMCAPTURE:WDA_NONE);
        hoverTime=GetTickCount64();Render(h);return 0;
    case kAppBarMsg:
        if(w==ABN_FULLSCREENAPP){DLOG(L"menu: shell reports full-screen %d",l!=0?1:0);SetFullscreenApp(h,FullscreenForeground());}
        else if(w==ABN_POSCHANGED){static ULONGLONG last=0;const ULONGLONG t=GetTickCount64();if(t-last>500){last=t;appBarRect={};Place(h);}}  // bounded: never a notify loop
        return 0;
    case WM_DISPLAYCHANGE: Place(h);Render(h);return 0;
    case WM_SETTINGCHANGE: if(w==SPI_SETWORKAREA){Place(h);Render(h);}return 0;
    case WM_DPICHANGED: Place(h);Render(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: { PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0; }
    case WM_INPUT:OnDragRawInput(reinterpret_cast<HRAWINPUT>(l));return DefWindowProcW(h,m,w,l);
    case WM_DESTROY:if(trayStarted){docky::tray::Get().Stop();trayStarted=false;}if(extHost){DestroyWindow(extHost);extHost=nullptr;}ReleaseDragClip();if(locationHook){UnhookWinEvent(locationHook);locationHook=nullptr;}if(fillPreview){DestroyWindow(fillPreview);fillPreview=nullptr;}if(barBlack){DestroyWindow(barBlack);barBlack=nullptr;}if(moveStartHook){UnhookWinEvent(moveStartHook);moveStartHook=nullptr;}if(moveHook){UnhookWinEvent(moveHook);moveHook=nullptr;}RemoveAppBar(h);if(inputWindow){HWND old=inputWindow;inputWindow=nullptr;DestroyWindow(old);}ClosePopup();liquid::cc::Shutdown();docky::notify::Shutdown();barWindow=nullptr;{std::lock_guard<std::mutex> lock(statusShared->mutex);statusShared->window=nullptr;}{std::lock_guard<std::mutex> lock(workshopShared->mutex);workshopShared->window=nullptr;}KillTimer(h,1);KillTimer(h,2);KillTimer(h,4);KillTimer(h,kHoverTimer);PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    (void)liquidBuildMarker[0];
    docky::capture_visible::Watch();  // screenshots show the menu bar's glass windows (capture_visible.h)
    const auto sharedSettings=liquid::settings::Load(liquid::settings::DefaultPath());
    docky::ui::SetLanguage(sharedSettings.value.appearance.language);
    docky::vblank::SetCaps(sharedSettings.value.performance.animationFps,sharedSettings.value.performance.captureFps);
    hideSnapLayouts=sharedSettings.value.dock.hideSnapLayouts;
    settingsRevision=liquid::settings::ReadRevision(liquid::settings::DefaultPath());
    struct Apartment { HRESULT hr=RoInitialize(RO_INIT_SINGLETHREADED);~Apartment(){themeUi=nullptr;if(SUCCEEDED(hr))RoUninitialize();} } apartment;
    try{themeUi=winrt::Windows::UI::ViewManagement::UISettings();}catch(...){themeUi=nullptr;}
    dock::PrewarmBorderlessCapture();  // answer ready before the first popup capture (no yellow border flash)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // --dev-isolated (tests only): its own instance lock, so a test copy runs beside the user's Menu Bar.
    HANDLE single=CreateMutexW(nullptr,FALSE,wcsstr(GetCommandLineW(),L"--dev-isolated")?L"Local\\LiquidDesktopMenuBarDevIsolated":L"Local\\LiquidDesktopMenuBarV01SingleInstance");
    if(!single)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(single);return 3;}
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);unsigned exitAfter=0;bool openControlCenter=false,visualCaptureTest=false,devTray=false,explicitSettings=false,explicitWorkshop=false;
    std::wstring devPath,devHover,devPopup,devKey,devNext,devRouteName;workshopRoot=docky::workshop::DefaultRoot();themeSettingsPath=liquid::settings::DefaultPath();
    // Glass Studio's categories (Menus, Menu bar popups, Control Center) live in the Dock's glass.json; read only.
    {wchar_t local[MAX_PATH]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH)){const bool ok=dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(local)+L"\\LiquidDock\\glass.json");DLOG(L"menu: glass surfaces %ls",ok?L"loaded":L"defaults");}}
    if(argv){for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"--exit-after"&&i+1<argc)exitAfter=static_cast<unsigned>(_wtoi(argv[++i]));else if(a==L"--log"&&i+1<argc)dock::Log::Get().Open(argv[++i]);else if(a==L"--control-center")openControlCenter=true;
      else if(a==L"--dev-diag"&&i+1<argc)devPath=argv[++i];else if(a==L"--dev-hover"&&i+1<argc)devHover=argv[++i];else if(a==L"--dev-popup"&&i+1<argc)devPopup=argv[++i];else if(a==L"--dev-key"&&i+1<argc)devKey=argv[++i];else if(a==L"--dev-png"&&i+1<argc)devPngPath=argv[++i];else if(a==L"--dev-next"&&i+1<argc)devNext=argv[++i];else if(a==L"--dev-popup-bmp"&&i+1<argc)devPopupBmpPath=argv[++i];else if(a==L"--dev-route"&&i+1<argc)devRouteName=argv[++i];else if(a==L"--workshop-root"&&i+1<argc){workshopRoot=argv[++i];explicitWorkshop=true;}else if(a==L"--settings-path"&&i+1<argc){themeSettingsPath=argv[++i];explicitSettings=true;}else if(a==L"--dev-visual-capture")visualCaptureTest=true;else if(a==L"--dev-tray")devTray=true;else if(a==L"--dev-notification")devNotification=true;else if(a==L"--dev-notification-center")devNotificationCenter=true;else if(a==L"--dev-bar-style"&&i+1<argc){const std::wstring v=argv[++i];barStyleForce=v==L"light"?1:v==L"dark"?0:-1;}}LocalFree(argv);}
    if(visualCaptureTest&&(!explicitSettings||!explicitWorkshop||devPath.empty()||exitAfter==0||exitAfter>120||SamePath(themeSettingsPath,liquid::settings::DefaultPath())||SamePath(workshopRoot,docky::workshop::DefaultRoot()))){CloseHandle(single);return 2;}
    liquid::cc::SetSettingsPath(themeSettingsPath);liquid::cc::SetWorkshopRoot(workshopRoot);settingsRevision=liquid::settings::ReadRevision(themeSettingsPath);{const auto current=liquid::settings::Load(themeSettingsPath);gameCompatibility=current.value.dock.gameCompatibility;hideSnapLayouts=current.value.dock.hideSnapLayouts;menuScalePercent=current.value.menuBar.scalePercent;docky::notify::Start(themeSettingsPath);docky::notify::SetCapture(current.value.menuBar.captureNotifications);docky::notify::SetHideSystemBanners(current.value.menuBar.hideSystemNotifications);}RefreshTheme();
    if((!devHover.empty()||!devPopup.empty()||!devKey.empty()||!devPngPath.empty()||!devNext.empty()||!devPopupBmpPath.empty()||!devRouteName.empty())&&devPath.empty()){CloseHandle(single);return 2;}
    auto statusIndex=[](const std::wstring& s)->int {if(s==L"wifi")return 0;if(s==L"sound")return 1;if(s==L"battery")return 2;if(s==L"control")return 3;if(s==L"clock")return 4;if(s==L"bluetooth")return 5;if(s==L"input")return 6;if(s==L"hidden")return 9;return -1;};
    if((!devHover.empty()&&devHover!=L"logo"&&devHover!=L"app"&&statusIndex(devHover)<0)||(!devPopup.empty()&&(statusIndex(devPopup)<0||statusIndex(devPopup)==4))){CloseHandle(single);return 2;}
    if(!devRouteName.empty()){
        if(!devPopup.empty()||statusIndex(devRouteName)<0||statusIndex(devRouteName)>3){CloseHandle(single);return 2;}
        devRoute=statusIndex(devRouteName);
    }
    if(!devPopupBmpPath.empty()&&(devPopup!=L"wifi"&&devPopup!=L"battery"&&devPopup!=L"hidden")){CloseHandle(single);return 2;}
    if(!devNext.empty()){
        if((devPopup.empty()&&devRouteName.empty())||statusIndex(devNext)<0||statusIndex(devNext)>3){CloseHandle(single);return 2;}
        devNextPopup=statusIndex(devNext);
    }
    if(!devKey.empty()){
        if(devPopup.empty()&&devRouteName.empty()){CloseHandle(single);return 2;}
        if(devKey==L"escape")devInitialKey=VK_ESCAPE;else if(devKey==L"down")devInitialKey=VK_DOWN;
        else if(devKey==L"left")devInitialKey=VK_LEFT;else if(devKey==L"space")devInitialKey=VK_SPACE;else if(devKey==L"bluetooth"&&visualCaptureTest&&devPopup==L"control")devInitialKey=0x10000;else{CloseHandle(single);return 2;}
    }
    if(!devPath.empty()){devTrace=CreateFileW(devPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(devTrace==INVALID_HANDLE_VALUE){CloseHandle(single);return 2;}}
    if(visualCaptureTest){liquid::cc::SetVisualCaptureTest(true);liquid::popup::SetVisualCaptureTest(true);DLOG(L"visual test: WGC capture disabled; window exclusion disabled");}
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=kClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc)){CloseHandle(single);return 1;}
    HWND h=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,kClass,L"DockOver Menu Bar",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);
    if(!h){if(devTrace!=INVALID_HANDLE_VALUE)CloseHandle(devTrace);CloseHandle(single);return 1;}liquid::cc::Prewarm();
    // Diagnostic/visual-test runs never move other applications' windows.
    if(devPath.empty()&&!visualCaptureTest){
        moveHook=SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND,EVENT_SYSTEM_MOVESIZEEND,nullptr,OnMoveSizeEnd,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        moveStartHook=SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART,EVENT_SYSTEM_MOVESIZESTART,nullptr,OnMoveSizeStart,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        locationHook=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,OnLocationChange,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        EnsureRawMouse();  // button presses: maximize-button full screen and the release check
        shellHooksEnabled=true;
    }
    extrasConfigPath=docky::extras::ConfigPath();extrasConfig=docky::extras::LoadConfig(extrasConfigPath);
    hostsDev=(devPath.empty()||devTray)&&!visualCaptureTest;
    if(!StartHosts(h))SetTimer(h,kHostsRetryTimer,1000,nullptr);  // the Dock still hosts them (menu bar was off): take over when it lets go
    if(devHover==L"logo")hoverIndex=0;else if(devHover==L"app")appHover=true;else if(!devHover.empty())statusHover=statusIndex(devHover);
    hoverTime=GetTickCount64();Render(h);ShowWindow(h,SW_SHOWNOACTIVATE);
    WNDCLASSW inputClass{};inputClass.hInstance=instance;inputClass.lpfnWndProc=InputProc;inputClass.lpszClassName=kInputClass;inputClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(RegisterClassW(&inputClass)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS){
        inputWindow=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,kInputClass,L"",WS_POPUP,0,0,1,1,h,nullptr,instance,nullptr);
        if(inputWindow){Render(h);if(inputWindow){const LONG_PTR ex=GetWindowLongPtrW(h,GWL_EXSTYLE);SetLastError(0);
            if(!SetWindowLongPtrW(h,GWL_EXSTYLE,ex|WS_EX_TRANSPARENT)&&GetLastError()!=0){DLOG(L"menu input: visual pass-through failed; retaining layered hit testing");HWND old=inputWindow;inputWindow=nullptr;DestroyWindow(old);}else Render(h);}}
        else DLOG(L"menu input: create failed; retaining layered hit testing");
    }else DLOG(L"menu input: class registration failed; retaining layered hit testing");
    if(devTrace!=INVALID_HANDLE_VALUE)SetTimer(h,8,100,nullptr);if(!devPngPath.empty())SetTimer(h,9,1500,nullptr);if(devRoute>=0){SetTimer(h,12,1500,nullptr);if(devInitialKey)SetTimer(h,7,3000,nullptr);if(devNextPopup>=0)SetTimer(h,10,3500,nullptr);}if(exitAfter)SetTimer(h,2,exitAfter*1000,nullptr);if(openControlCenter)PostMessageW(h,WM_APP+44,0,0);if(devNotification||devNotificationCenter)SetTimer(h,kDevNotifyTimer,1200,nullptr);
    if(!devPopup.empty()&&statusIndex(devPopup)==9){devInitialPopup=9;SetTimer(h,6,3000,nullptr);if(!devPopupBmpPath.empty())SetTimer(h,11,4500,nullptr);}
    else if(!devPopup.empty()){devInitialPopup=statusIndex(devPopup);SetTimer(h,6,20,nullptr);if(devInitialKey)SetTimer(h,7,2000,nullptr);if(devNextPopup>=0)SetTimer(h,10,3000,nullptr);if(!devPopupBmpPath.empty())SetTimer(h,11,1500,nullptr);}
    DLOG(L"menu: visible hwnd=%p dpi=%u",h,dpi);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}DLOG(L"menu: exit");if(devTrace!=INVALID_HANDLE_VALUE)CloseHandle(devTrace);CloseHandle(single);return 0;
}
