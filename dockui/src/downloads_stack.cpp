#include "downloads_stack.h"
#include "vblank.h"
#include "fan_layout.h"
#include "fan_gpu.h"
#include "design_tokens.h"
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <mutex>
#include <memory>
#include <thread>
#include <map>
#include <deque>
#include <set>
#include <condition_variable>
#include <cstdint>
#include <objidl.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cwctype>
#include <commoncontrols.h>
namespace Gdiplus{using std::min;using std::max;}
#include <gdiplus.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace dock::downloads {
std::wstring KnownDownloadsPath(){PWSTR p=nullptr;std::wstring result;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads,KF_FLAG_DEFAULT,nullptr,&p))&&p)result=p;CoTaskMemFree(p);return result;}
std::vector<std::uint32_t> MakeStackIcon(unsigned size){
 if(size<16||size>1024)return {};std::vector<std::uint32_t> pixels(static_cast<size_t>(size)*size);
 const float scale=size/256.0f,center=size*0.5f,half=size*0.415f,radius=size*0.20f;
 auto segment=[](float px,float py,float ax,float ay,float bx,float by){const float dx=bx-ax,dy=by-ay;const float t=std::clamp(((px-ax)*dx+(py-ay)*dy)/(dx*dx+dy*dy),0.0f,1.0f);return std::hypot(px-(ax+t*dx),py-(ay+t*dy));};
 for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){float qx=std::fabs(x+0.5f-center)-(half-radius),qy=std::fabs(y+0.5f-center)-(half-radius);
  float sd=std::hypot((std::max)(qx,0.0f),(std::max)(qy,0.0f))+(std::min)((std::max)(qx,qy),0.0f)-radius;
  float a=std::clamp(0.5f-sd,0.0f,1.0f);float arrow=(std::min)(segment(static_cast<float>(x),static_cast<float>(y),76*scale,106*scale,128*scale,158*scale),segment(static_cast<float>(x),static_cast<float>(y),128*scale,158*scale,180*scale,106*scale));
  float ink=std::clamp(8.0f*scale+0.5f-arrow,0.0f,1.0f);float base=0.22f+0.13f*(1.0f-y/static_cast<float>(size));
  auto c=[&](float tint){return static_cast<unsigned>(std::clamp((base*tint*(1.0f-ink)+ink)*a,0.0f,1.0f)*255+0.5f);};
  unsigned A=static_cast<unsigned>(a*255+0.5f);pixels[static_cast<size_t>(y)*size+x]=(A<<24)|(c(0.96f)<<16)|(c(1.0f)<<8)|c(1.10f);
 }return pixels;
}
// Open-state Downloads tile (user reference, 2026-11-10): a translucent dark-grey squircle that fades from ~28 % grey at
// the top to ~11 % at the bottom (about 70 % opaque, the Dock glass shows through), a lighter rim, and a white chevron
// (38 % of the tile wide, point slightly below the centre, stroke ~5 %). No arrow shaft.
std::vector<std::uint32_t> MakeOpenStackIcon(unsigned size){
 if(size<16||size>1024)return {};std::vector<std::uint32_t> px(static_cast<size_t>(size)*size);
 const float S=static_cast<float>(size),c=S*0.5f,half=S*0.47f,rad=S*0.225f;  // near full tile, like masked icons
 auto seg=[](float x,float y,float ax,float ay,float bx,float by){const float dx=bx-ax,dy=by-ay;const float t=std::clamp(((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy),0.f,1.f);return std::hypot(x-(ax+t*dx),y-(ay+t*dy));};
 const float w=S*0.024f;  // chevron stroke half-width
 for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
  const float fx=x+0.5f,fy=y+0.5f,qx=std::fabs(fx-c)-(half-rad),qy=std::fabs(fy-c)-(half-rad);
  const float sd=std::hypot((std::max)(qx,0.f),(std::max)(qy,0.f))+(std::min)((std::max)(qx,qy),0.f)-rad;
  const float cover=std::clamp(0.5f-sd,0.f,1.f);if(cover<=0){px[static_cast<size_t>(y)*size+x]=0;continue;}
  const float t=std::clamp((fy-(c-half))/(2*half),0.f,1.f);                 // 0 top .. 1 bottom
  float g=0.28f+(0.11f-0.28f)*t;                                            // grey above, darker below
  float a=0.70f;                                                            // translucent body
  const float rim=std::clamp(1.f-std::fabs(sd+S*0.01f)/(S*0.01f),0.f,1.f)*(0.16f+0.30f*(1.f-t));  // lighter edge, stronger on top
  g=g+(0.95f-g)*rim;a=a+(1.f-a)*rim*0.5f;
  const float vy=c+S*0.08f,ey=c-S*0.055f,arm=S*0.19f;                        // chevron: point below centre
  const float d=(std::min)(seg(fx,fy,c-arm,ey,c,vy),seg(fx,fy,c+arm,ey,c,vy));
  const float ink=std::clamp(w+0.5f-d,0.f,1.f);
  g=g+(1.f-g)*ink;a=a+(1.f-a)*ink;
  const unsigned A=static_cast<unsigned>(std::clamp(a*cover,0.f,1.f)*255.f+0.5f),G=static_cast<unsigned>(std::clamp(g,0.f,1.f)*255.f+0.5f);
  const unsigned B=static_cast<unsigned>(std::clamp(g*1.04f,0.f,1.f)*255.f+0.5f);  // a touch cool
  px[static_cast<size_t>(y)*size+x]=(A<<24)|(G<<16)|(G<<8)|B;
 }
 return px;
}
static std::wstring Extension(const std::wstring& n){const auto i=n.find_last_of(L'.');return i==std::wstring::npos?L"":n.substr(i);}
void SortEntries(std::vector<Entry>& entries,Sort sort){
 std::stable_sort(entries.begin(),entries.end(),[sort](const Entry& a,const Entry& b){
  if(sort==Sort::Date){int c=CompareFileTime(&a.modified,&b.modified);if(c)return c>0;}
  if(sort==Sort::Type){int c=_wcsicmp(Extension(a.name).c_str(),Extension(b.name).c_str());if(c)return c<0;}
  return _wcsicmp(a.name.c_str(),b.name.c_str())<0;
 });
}
std::vector<Entry> Scan(const std::wstring& directory,DWORD& error,size_t limit){
 std::vector<Entry> out;error=ERROR_SUCCESS;if(directory.empty()){error=ERROR_PATH_NOT_FOUND;return out;}
 std::wstring pattern=directory;if(pattern.back()!=L'\\'&&pattern.back()!=L'/')pattern+=L'\\';pattern+=L'*';
 WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&fd,FindExSearchNameMatch,nullptr,0);
 if(f==INVALID_HANDLE_VALUE){error=GetLastError();return out;}
 do{if(wcscmp(fd.cFileName,L".")==0||wcscmp(fd.cFileName,L"..")==0)continue;
  if(fd.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM))continue;
  Entry e;e.name=fd.cFileName;e.path=directory;if(e.path.back()!=L'\\'&&e.path.back()!=L'/')e.path+=L'\\';e.path+=e.name;e.attributes=fd.dwFileAttributes;e.modified=fd.ftLastWriteTime;out.push_back(std::move(e));
 }while(out.size()<limit&&FindNextFileW(f,&fd));
 DWORD last=GetLastError();FindClose(f);if(out.size()<limit&&last!=ERROR_NO_MORE_FILES)error=last;return out;
}
bool LoadSettings(const std::wstring& path,Settings& result,std::wstring& error){
 HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(f==INVALID_HANDLE_VALUE){DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND){result={};return true;}error=L"Cannot read Downloads settings.";return false;}
 char bytes[128]{};DWORD n=0;LARGE_INTEGER size{};bool ok=GetFileSizeEx(f,&size)&&size.QuadPart>=0&&size.QuadPart<static_cast<LONGLONG>(sizeof(bytes))&&ReadFile(f,bytes,sizeof(bytes)-1,&n,nullptr)&&n==static_cast<DWORD>(size.QuadPart);CloseHandle(f);
 int view=-1,sort=-1,display=-1;char tail=0;
 if(!ok||sscanf_s(bytes,"LiquidDownloads 1\nview=%d\nsort=%d\ndisplay=%d\n%c",&view,&sort,&display,&tail,1)!=3||view<0||view>2||sort<0||sort>2||display<0||display>1){error=L"Downloads settings are invalid or unsupported; they were preserved.";return false;}
 result={static_cast<View>(view),static_cast<Sort>(sort),static_cast<Display>(display)};return true;
}
bool SaveSettings(const std::wstring& path,const Settings& settings,std::wstring& error){
 // Do not replace an unreadable or unsupported existing file.
 Settings old;if(!LoadSettings(path,old,error))return false;
 char data[128]{};int n=sprintf_s(data,"LiquidDownloads 1\nview=%d\nsort=%d\ndisplay=%d\n",static_cast<int>(settings.view),static_cast<int>(settings.sort),static_cast<int>(settings.display));
 if(n<=0){error=L"Cannot encode Downloads settings.";return false;}
 const std::wstring tmp=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".tmp";
 HANDLE f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE){error=L"Cannot create temporary Downloads settings.";return false;}
 DWORD written=0;bool ok=WriteFile(f,data,static_cast<DWORD>(n),&written,nullptr)&&written==static_cast<DWORD>(n)&&FlushFileBuffers(f);CloseHandle(f);
 if(ok)ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
 if(!ok){DeleteFileW(tmp.c_str());error=L"Cannot save Downloads settings.";return false;}return true;
}
namespace {
constexpr wchar_t kPanelClass[]=L"LiquidDownloadsStack98d2";constexpr UINT kScanDone=WM_APP+181,kTick=WM_APP+182,kSpriteReady=WM_APP+183;
double NowMs(){static LARGE_INTEGER f{};if(!f.QuadPart)QueryPerformanceFrequency(&f);LARGE_INTEGER c{};QueryPerformanceCounter(&c);return c.QuadPart*1000.0/f.QuadPart;}
// Frame pacing (V0.6). WM_TIMER(16) fires every 31.25 ms on the default 15.6 ms system tick, and GetTickCount64
// has the same granularity: that was the real stutter. A small helper thread waits for each DWM composition
// (DwmFlush) and posts one tick at a time to the panel; posted messages wake the Dock's message wait at once.
struct Ticker{std::atomic<bool> quit{false},active{false},pending{false};HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);HWND h=nullptr;~Ticker(){if(wake)CloseHandle(wake);}};
void StartTicker(const std::shared_ptr<Ticker>& t){std::thread([t]{double lastPost=0;while(!t->quit){if(!t->active){WaitForSingleObject(t->wake,250);continue;}
 docky::vblank::WaitFrame(t->h);const double now=docky::vblank::Seconds();
 if(now-lastPost<docky::vblank::CapInterval(docky::vblank::AnimationCap()))continue;  // Settings > General > Frame rate
 if(t->active&&!t->quit&&!t->pending.exchange(true)){lastPost=now;PostMessageW(t->h,kTick,0,0);}}}).detach();}
struct Shared {std::mutex mutex;HWND window=nullptr;bool busy=false;std::vector<Entry> result;DWORD error=0;};
struct FanHit{int entry=-1;float cx=0,cy=0,angle=0,scale=1;Gdiplus::RectF row;bool hasLabel=false;float lcx=0,lcy=0,lhw=0,lhh=0;};  // label centre / half size (window px), rotated by angle
struct RowSprite{std::unique_ptr<Gdiplus::Bitmap> bmp;float px=0,py=0;Gdiplus::RectF row;
 std::vector<std::uint32_t> pixels;int pw=0,ph=0;std::uint64_t id=0;float labelW=0;};  // pixels/id/labelW: GPU path (icon + shadow only)
std::atomic<std::uint64_t> g_spriteIds{0};
// V0.8: row sprites are built on a background STA thread (own icon cache and GDI+ objects) and handed to
// the panel thread through a mutex-protected list plus a posted message. Jobs carry a generation (bumped
// when the entry list changes) and are skipped when stale or far from the current scroll target.
struct SpriteJob{size_t e=0;int gen=0;std::wstring name,path;DWORD attributes=0;float k=1;bool left=true,iconOnly=false,preview=false;};
struct SpriteDone{size_t e=0;int gen=0;RowSprite sprite;double ms=0;};
struct SpriteWorker{std::mutex m;std::condition_variable cv;std::deque<SpriteJob> jobs;std::vector<SpriteDone> done;bool quit=false;HWND h=nullptr;
 std::atomic<int> gen{0},center{0};double buildMs=0,buildMax=0;int built=0,skipped=0;};
struct Panel {bool reducedMotion=false;bool* destroyedDuringCreate=nullptr;std::wstring directory;Settings settings;std::shared_ptr<Shared> shared=std::make_shared<Shared>();std::vector<Entry> entries;DWORD error=0;int scroll=0;UINT dpi=96;ULONGLONG opened=0,closing=0;float progress=0,closeFrom=1,anchorX=0;double openedMs=0,closingMs=0;float postedProgress=0;std::shared_ptr<Ticker> ticker=std::make_shared<Ticker>();
 float scrollPos=0,scrollFrom=0;double scrollStart=-1;struct Spring{float x=1,v=0;};std::map<size_t,Spring> springs;double springClock=0;
 double lastTick=0,intervalSum=0,intervalMax=0,ulwMs=0;int intervals=0,longFrames=0,prefetched=0,misses=0,over8=0,over16=0;
 std::shared_ptr<SpriteWorker> worker;std::set<size_t> pending;int generation=0;RowSprite placeholder;
 std::unique_ptr<IFanRenderer> gpu;double glassMs=0,glassMax=0;int glassFrames=0,snapIndex=0;bool snapOpenTaken=false,hitsLogged=false;double lastWheelMs=0;std::wstring snapPrefix;
 double frameMs=0,frameMax=0,buildMs=0;int frames=0;std::vector<std::uint32_t> chromePixels;
 BYTE alpha=0;int hover=-1,pressed=-1;bool tracking=false;RECT refreshRect{},explorerRect{};std::vector<FanHit> hits;std::map<int,std::unique_ptr<Gdiplus::Bitmap>> icons;std::map<size_t,RowSprite> sprites;std::unique_ptr<Gdiplus::Bitmap> chrome,canvas;int chromeKey=-1;
 HDC dc=nullptr;HBITMAP dib=nullptr;HGDIOBJ oldBmp=nullptr;void* bits=nullptr;int bw=0,bh=0,wheel=0;bool needsFrame=false,lastLeftLabel=true,haveLeftLabel=false;
 // V0.20: drag a file out, shell context menu, Ctrl+C/X. While a drag or menu runs the panel must not close (it is
 // still on the call stack); a dismissal is remembered and applied afterwards.
 int pressX=0,pressY=0;bool dragging=false,menuOpen=false,closeAfter=false,suppressClick=false;IContextMenu* menuHandler=nullptr;RowSprite arrow;};
HWND current=nullptr;
// V0.18: when Windows refuses the panel the foreground (focus-stealing protection), it never becomes active, so the
// usual "deactivated -> close" never fires and an outside click would leave it floating. While such a panel is open we
// observe (never swallow) button presses outside it and foreground changes, and post a dismiss to the panel.
constexpr UINT kDismiss=WM_APP+184;
HHOOK g_dismissMouse=nullptr;HWINEVENTHOOK g_dismissForeground=nullptr;
LRESULT CALLBACK DismissMouseProc(int code,WPARAM w,LPARAM l){
 if(code==HC_ACTION&&current&&(w==WM_LBUTTONDOWN||w==WM_RBUTTONDOWN||w==WM_MBUTTONDOWN)){
  RECT r{};GetWindowRect(current,&r);if(!PtInRect(&r,reinterpret_cast<MSLLHOOKSTRUCT*>(l)->pt))PostMessageW(current,kDismiss,1,0);}
 return CallNextHookEx(nullptr,code,w,l);}
void CALLBACK DismissForegroundProc(HWINEVENTHOOK,DWORD,HWND fg,LONG,LONG,DWORD,DWORD){if(current&&fg&&fg!=current)PostMessageW(current,kDismiss,2,0);}
void RemoveDismissWatch(){if(g_dismissMouse){UnhookWindowsHookEx(g_dismissMouse);g_dismissMouse=nullptr;}if(g_dismissForeground){UnhookWinEvent(g_dismissForeground);g_dismissForeground=nullptr;}}
void InstallDismissWatch(){RemoveDismissWatch();
 g_dismissMouse=SetWindowsHookExW(WH_MOUSE_LL,DismissMouseProc,GetModuleHandleW(nullptr),0);
 g_dismissForeground=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,DismissForegroundProc,0,0,WINEVENT_OUTOFCONTEXT);}
int Px(const Panel* p,int x){return MulDiv(x,static_cast<int>(p->dpi),96);}
void RequestScan(Panel* p){
 auto shared=p->shared;const auto directory=p->directory;
 {std::lock_guard<std::mutex> lock(shared->mutex);if(shared->busy||!shared->window)return;shared->busy=true;}
 std::thread([shared,directory]{DWORD error=0;auto found=Scan(directory,error);std::lock_guard<std::mutex> lock(shared->mutex);
  shared->result=std::move(found);shared->error=error;shared->busy=false;if(shared->window)PostMessageW(shared->window,kScanDone,0,0);
 }).detach();
}
void RenderFan(HWND h,Panel* p);
void ApplyAlpha(HWND h,Panel* p,BYTE alpha){if(p->settings.view==View::Fan){p->alpha=alpha;RenderFan(h,p);return;}SetLayeredWindowAttributes(h,RGB(1,0,1),alpha,LWA_ALPHA);}
void Animate(Panel* p){if(!p->ticker->active.exchange(true)){p->lastTick=0;SetEvent(p->ticker->wake);}}
void BeginClose(HWND,Panel* p){if(GetEnvironmentVariableW(L"LIQUID_STACK_TRACE",nullptr,0))fwprintf(stderr,L"stack trace: BeginClose closing=%llu\n",p->closing);if(!p->closing){p->closeFrom=p->progress;p->closing=GetTickCount64();p->closingMs=NowMs();Animate(p);}}
void OpenAsync(std::wstring path){std::thread([path=std::move(path)]{HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SHELLEXECUTEINFOW si{sizeof(si)};si.fMask=SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;si.lpVerb=L"open";si.lpFile=path.c_str();si.nShow=SW_SHOWNORMAL;if(!ShellExecuteExW(&si))MessageBoxW(nullptr,L"This item is unavailable or could not be opened.",L"Downloads Stack",MB_OK|MB_ICONWARNING);if(SUCCEEDED(co))CoUninitialize();}).detach();}
int IconIndex(const Entry& entry,HIMAGELIST& images,bool large){SHFILEINFOW info{};DWORD attrs=(entry.attributes&FILE_ATTRIBUTE_DIRECTORY)?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL;
 auto h=SHGetFileInfoW(entry.name.c_str(),attrs,&info,sizeof(info),SHGFI_SYSICONINDEX|(large?SHGFI_LARGEICON:SHGFI_SMALLICON)|SHGFI_USEFILEATTRIBUTES);images=reinterpret_cast<HIMAGELIST>(h);return info.iIcon;}
RECT EntryRect(const Panel* p,int index,int width,int /*height*/){
 if(p->settings.view==View::List)return {Px(p,12),Px(p,48+index*34),width-Px(p,12),Px(p,80+index*34)};
 if(p->settings.view==View::Grid){int cols=(std::max)(1,(width-Px(p,24))/Px(p,96));int col=index%cols,row=index/cols;return {Px(p,12+col*96),Px(p,48+row*84),Px(p,104+col*96),Px(p,128+row*84)};}
 return {}; // Fan has separate shared icon/label geometry.
}
int MaxScroll(const Panel* p,int width,int height){
 if(p->settings.view==View::Fan){const int total=static_cast<int>(p->entries.size());return (std::max)(0,total-FanVisibleCount(total,height/(p->dpi/96.f)));}int count=static_cast<int>(p->entries.size());
 int rows=p->settings.view==View::List?count:(count+(std::max)(1,(width-Px(p,24))/Px(p,96))-1)/(std::max)(1,(width-Px(p,24))/Px(p,96));
 int rowsVisible=(std::max)(1,(height-Px(p,95))/(p->settings.view==View::List?Px(p,34):Px(p,84)));return (std::max)(0,rows-rowsVisible);
}
// ---- Fan V0.4: per-pixel alpha rendering (GDI+ into a premultiplied DIB, UpdateLayeredWindow). ----
// Behaviour modelled on the user's Launcher instance fan (design only, GPL code not reused): staggered
// OutBack rows, scale 0.4->1 and fade-in per row, slight lean along the arc, soft shadow, frosted name
// capsules, hover 1.08 / press 0.92. Drawing and hit testing share the same cached row geometry.
void EnsureGdiplus(){static ULONG_PTR token=0;if(!token){Gdiplus::GdiplusStartupInput in;Gdiplus::GdiplusStartup(&token,&in,nullptr);}}
// V0.5 performance: every row (icon + shadow + frosted capsule + name) is pre-rendered once into a sprite at
// full size with high-quality filtering; the chrome capsules likewise. Each animation frame only clears a
// persistent DIB and draws one transformed sprite per row (bilinear, alpha via colour matrix only when
// fading). No Shell queries, text measuring, path building or buffer allocation happen per frame.
Gdiplus::Bitmap* FanIcon(std::map<int,std::unique_ptr<Gdiplus::Bitmap>>& icons,const Entry& entry){
 SHFILEINFOW info{};const DWORD attrs=(entry.attributes&FILE_ATTRIBUTE_DIRECTORY)?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL;
 SHGetFileInfoW(entry.name.c_str(),attrs,&info,sizeof(info),SHGFI_SYSICONINDEX|SHGFI_USEFILEATTRIBUTES);
 auto& slot=icons[info.iIcon];if(slot)return slot.get();
 IImageList* list=nullptr;if(FAILED(SHGetImageList(SHIL_EXTRALARGE,IID_IImageList,reinterpret_cast<void**>(&list)))||!list)return nullptr;
 HICON icon=nullptr;list->GetIcon(info.iIcon,ILD_TRANSPARENT,&icon);list->Release();if(!icon)return nullptr;
 constexpr int n=48;BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=n;bi.bmiHeader.biHeight=-n;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 void* bits=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP dib=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
 if(dib&&bits){HGDIOBJ old=SelectObject(dc,dib);std::memset(bits,0,n*n*4);DrawIconEx(dc,0,0,icon,n,n,0,nullptr,DI_NORMAL);GdiFlush();
  auto* px=static_cast<std::uint32_t*>(bits);bool anyAlpha=false;for(int i=0;i<n*n;++i)if(px[i]>>24){anyAlpha=true;break;}
  if(!anyAlpha)for(int i=0;i<n*n;++i)if(px[i]&0xFFFFFFu)px[i]|=0xFF000000u;  // legacy icon without alpha
  slot=std::make_unique<Gdiplus::Bitmap>(n,n,PixelFormat32bppPARGB);Gdiplus::BitmapData d{};Gdiplus::Rect rr(0,0,n,n);
  if(slot->LockBits(&rr,Gdiplus::ImageLockModeWrite,PixelFormat32bppPARGB,&d)==Gdiplus::Ok){for(int y=0;y<n;++y)std::memcpy(static_cast<BYTE*>(d.Scan0)+y*d.Stride,px+y*n,n*4);slot->UnlockBits(&d);}
  SelectObject(dc,old);}
 if(dib)DeleteObject(dib);DeleteDC(dc);DestroyIcon(icon);return slot.get();
}
void CapsulePath(Gdiplus::GraphicsPath& path,const Gdiplus::RectF& r){const float d=r.Height;path.AddArc(r.X,r.Y,d,d,90,180);path.AddArc(r.X+r.Width-d,r.Y,d,d,270,180);path.CloseFigure();}
void Capsule(Gdiplus::Graphics& g,const Gdiplus::RectF& r){
 Gdiplus::GraphicsPath shadow;Gdiplus::RectF s=r;s.Y+=1.5f;CapsulePath(shadow,s);
 Gdiplus::SolidBrush sb(Gdiplus::Color(41,0,0,0));g.FillPath(&sb,&shadow);
 Gdiplus::GraphicsPath path;CapsulePath(path,r);
 Gdiplus::SolidBrush fb(Gdiplus::Color(184,246,246,248));g.FillPath(&fb,&path);  // frosted, not opaque
 Gdiplus::Pen rim(Gdiplus::Color(217,255,255,255),1.f);g.DrawPath(&rim,&path);
}
void CapsuleText(Gdiplus::Graphics& g,const Gdiplus::Font& f,const std::wstring& text,const Gdiplus::RectF& r){
 Gdiplus::StringFormat sf;sf.SetAlignment(Gdiplus::StringAlignmentCenter);sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
 sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
 Gdiplus::SolidBrush ink(Gdiplus::Color(255,29,29,31));g.DrawString(text.c_str(),-1,&f,r,&sf,&ink);
}
void Quality(Gdiplus::Graphics& g){g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
 g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);}
struct BuildTimer{Panel* p;LARGE_INTEGER a{};BuildTimer(Panel* q):p(q){QueryPerformanceCounter(&a);}~BuildTimer(){LARGE_INTEGER b{},f{};QueryPerformanceCounter(&b);QueryPerformanceFrequency(&f);p->buildMs+=(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart;}};
// V0.20 file previews: the Shell thumbnail (images, PDFs, videos, documents) when the file has one. Folders and
// archives keep their type icon (their "thumbnails" are just icons or a first file).
bool PreviewEligible(const Entry& entry){
 if(entry.path.empty()||(entry.attributes&FILE_ATTRIBUTE_DIRECTORY))return false;
 const size_t dot=entry.name.find_last_of(L'.');if(dot==std::wstring::npos)return true;
 std::wstring ext=entry.name.substr(dot+1);for(auto& c:ext)c=static_cast<wchar_t>(towlower(c));
 for(const wchar_t* a:{L"zip",L"rar",L"7z",L"tar",L"gz",L"tgz",L"bz2",L"xz",L"zst",L"cab",L"iso",L"img",L"exe",L"msi",L"msix",L"appx",L"lnk",L"url"})if(ext==a)return false;
 return true;
}
std::unique_ptr<Gdiplus::Bitmap> Thumbnail(const std::wstring& path,int size,bool& transparent){
 transparent=false;
 Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
 if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&factory))))return nullptr;
 HBITMAP hb=nullptr;if(FAILED(factory->GetImage(SIZE{size,size},SIIGBF_THUMBNAILONLY|SIIGBF_BIGGERSIZEOK,&hb))||!hb)return nullptr;
 BITMAP bm{};GetObjectW(hb,sizeof(bm),&bm);const int w=bm.bmWidth,h=std::abs(bm.bmHeight);
 std::unique_ptr<Gdiplus::Bitmap> out;
 if(w>0&&h>0&&w<=4096&&h<=4096){
  std::vector<std::uint32_t> px(static_cast<size_t>(w)*h);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  HDC dc=GetDC(nullptr);const int lines=GetDIBits(dc,hb,0,h,px.data(),&bi,DIB_RGB_COLORS);ReleaseDC(nullptr,dc);
  if(lines==h){bool anyAlpha=false;for(auto v:px)if(v>>24){anyAlpha=true;break;}if(!anyAlpha)for(auto& v:px)v|=0xFF000000u;
   size_t clear=0;for(auto v:px)if((v>>24)<250)++clear;transparent=clear*50>px.size();  // cut-out images (icons, logos): no paper card
   out=std::make_unique<Gdiplus::Bitmap>(w,h,PixelFormat32bppPARGB);Gdiplus::BitmapData d{};Gdiplus::Rect rr(0,0,w,h);
   if(out->LockBits(&rr,Gdiplus::ImageLockModeWrite,PixelFormat32bppPARGB,&d)==Gdiplus::Ok){for(int y=0;y<h;++y)std::memcpy(static_cast<BYTE*>(d.Scan0)+y*d.Stride,px.data()+static_cast<size_t>(y)*w,static_cast<size_t>(w)*4);out->UnlockBits(&d);}else out.reset();}}
 DeleteObject(hb);return out;
}
// Thumbnail fitted into the icon square like a small print: white border, hairline edge (the row shadow adds depth).
void DrawPreview(Gdiplus::Graphics& g,Gdiplus::Bitmap& thumb,const Gdiplus::RectF& icon,float k,bool transparent){
 const float tw=static_cast<float>(thumb.GetWidth()),th=static_cast<float>(thumb.GetHeight()),fit=(std::min)(icon.Width/tw,icon.Height/th);
 const float w=tw*fit,h=th*fit;const Gdiplus::RectF dst(icon.X+(icon.Width-w)/2,icon.Y+(icon.Height-h)/2,w,h);
 if(transparent){g.DrawImage(&thumb,dst);return;}
 const float b=1.5f*k;Gdiplus::RectF frame(dst.X-b,dst.Y-b,dst.Width+2*b,dst.Height+2*b);
 Gdiplus::SolidBrush paper(Gdiplus::Color(255,255,255,255));g.FillRectangle(&paper,frame);
 g.DrawImage(&thumb,dst);
 Gdiplus::Pen edge(Gdiplus::Color(40,0,0,0),0.75f*k);g.DrawRectangle(&edge,frame);
}
// V0.20: the "more in Explorer" button above the top row: a graphite glass disc with a white forward arrow
// (original vector art after the user's macOS reference; no text).
RowSprite BuildArrow(float k){
 RowSprite sp;sp.id=++g_spriteIds;const float s=46.f*k,m=14.f*k;const int W=static_cast<int>(std::ceil(s+2*m)),H=W;
 sp.px=W/2.f;sp.py=H/2.f;sp.row=Gdiplus::RectF(-s/2,-s/2,s,s);sp.pw=W;sp.ph=H;sp.pixels.assign(static_cast<size_t>(W)*H,0u);
 {Gdiplus::Bitmap layer(W,H,W*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(sp.pixels.data()));Gdiplus::Graphics g(&layer);Quality(g);g.TranslateTransform(sp.px,sp.py);
  for(int i=6;i>=1;--i){const float r=s/2+i*1.2f*k;Gdiplus::SolidBrush sh(Gdiplus::Color(static_cast<BYTE>(10+3*(6-i)),0,0,0));g.FillEllipse(&sh,-r,-r+2.5f*k,2*r,2*r);}
  Gdiplus::RectF disc(-s/2,-s/2,s,s);
  Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(0,-s/2),Gdiplus::PointF(0,s/2),Gdiplus::Color(225,112,112,118),Gdiplus::Color(225,74,74,80));g.FillEllipse(&fill,disc);
  Gdiplus::Pen ring(Gdiplus::Color(200,28,28,30),1.6f*k);g.DrawEllipse(&ring,disc);
  Gdiplus::Pen inner(Gdiplus::Color(70,255,255,255),1.f*k);g.DrawEllipse(&inner,-s/2+2*k,-s/2+2*k,s-4*k,s-4*k);
  // Forward arrow: a thick curved tail rising from the lower left into a head pointing right.
  const float u=s/46.f;Gdiplus::GraphicsPath tail;
  tail.AddBezier(Gdiplus::PointF(-12*u,9*u),Gdiplus::PointF(-11*u,-2*u),Gdiplus::PointF(-4*u,-5*u),Gdiplus::PointF(3*u,-5*u));
  Gdiplus::Pen stroke(Gdiplus::Color(255,255,255,255),6.f*u);stroke.SetStartCap(Gdiplus::LineCapRound);stroke.SetEndCap(Gdiplus::LineCapFlat);g.DrawPath(&stroke,&tail);
  Gdiplus::PointF head[3]={Gdiplus::PointF(2*u,-14*u),Gdiplus::PointF(14*u,-5*u),Gdiplus::PointF(2*u,4*u)};
  Gdiplus::GraphicsPath hp;hp.AddPolygon(head,3);Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255));g.FillPath(&white,&hp);
  Gdiplus::Pen round(Gdiplus::Color(255,255,255,255),1.5f*u);round.SetLineJoin(Gdiplus::LineJoinRound);g.DrawPath(&round,&hp);}
 return sp;
}
// Builds one row sprite: icon + frosted capsule + name, plus a soft shadow of the whole row silhouette
// following the reference fan's MultiEffect (blur ~0.6, y offset 4, opacity 0.5 * shadowAmount 0.5).
// The shadow is a 3-pass box blur of the row alpha, so it has no square edges; the margin covers it.
RowSprite BuildRow(std::map<int,std::unique_ptr<Gdiplus::Bitmap>>& icons,const Entry& entry,float k,bool leftLabel,bool iconOnly=false,bool preview=false){
 RowSprite sp;sp.id=++g_spriteIds;const float s=48.f*k,gap=8.f*k,capH=28.f*k,m=18.f*k;
 Gdiplus::FontFamily family(liquid::design::type::FamilyGdi);Gdiplus::Font font(&family,liquid::design::type::Label*k,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
 Gdiplus::RectF measured;{Gdiplus::Bitmap one(1,1,PixelFormat32bppPARGB);Gdiplus::Graphics mg(&one);mg.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  if(!entry.name.empty())mg.MeasureString(entry.name.c_str(),-1,&font,Gdiplus::PointF(0,0),&measured);}
 const float capW=(std::min)((std::max)(measured.Width+24.f*k,56.f*k),200.f*k);sp.labelW=capW/k;
 const Gdiplus::RectF label(leftLabel?-s/2-gap-capW:s/2+gap,-capH/2,capW,capH),icon(-s/2,-s/2,s,s);
 Gdiplus::RectF row=icon;if(!iconOnly)Gdiplus::RectF::Union(row,label,icon);sp.row=row;sp.px=-row.X+m;sp.py=-row.Y+m;
 const int W=static_cast<int>(std::ceil(row.Width+2*m)),H=static_cast<int>(std::ceil(row.Height+2*m));
 std::vector<std::uint32_t> content(static_cast<size_t>(W)*H,0u);
 {Gdiplus::Bitmap layer(W,H,W*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(content.data()));Gdiplus::Graphics g(&layer);Quality(g);g.TranslateTransform(sp.px,sp.py);
  if(!iconOnly){Gdiplus::GraphicsPath path;CapsulePath(path,label);
  Gdiplus::SolidBrush fb(Gdiplus::Color(184,246,246,248));g.FillPath(&fb,&path);
  Gdiplus::Pen rim(Gdiplus::Color(217,255,255,255),1.f);g.DrawPath(&rim,&path);
  Gdiplus::RectF text=label;text.X+=10*k;text.Width-=20*k;if(!entry.name.empty())CapsuleText(g,font,entry.name,text);}
  bool cutout=false;std::unique_ptr<Gdiplus::Bitmap> thumb=preview&&PreviewEligible(entry)?Thumbnail(entry.path,static_cast<int>(std::lround(s*2)),cutout):nullptr;
  if(thumb)DrawPreview(g,*thumb,icon,k,cutout);
  else if(Gdiplus::Bitmap* bmp=entry.name.empty()?nullptr:FanIcon(icons,entry))g.DrawImage(bmp,icon);
  else{Gdiplus::SolidBrush ph(Gdiplus::Color(120,236,236,240));g.FillRectangle(&ph,Gdiplus::RectF(icon.X+6*k,icon.Y+3*k,s-12*k,s-6*k));}}
 // Shadow: blur the alpha, offset down 4 DIP, 25% black, composited under the content.
 std::vector<float> a(static_cast<size_t>(W)*H),t(a.size());for(size_t i=0;i<a.size();++i)a[i]=(content[i]>>24)/255.f;
 const int r=(std::max)(1,static_cast<int>(std::lround(5.f*k)));
 for(int pass=0;pass<3;++pass){
  for(int y=0;y<H;++y){float acc=0;for(int x=-r;x<=r;++x)acc+=a[static_cast<size_t>(y)*W+std::clamp(x,0,W-1)];
   for(int x=0;x<W;++x){t[static_cast<size_t>(y)*W+x]=acc/(2*r+1);acc+=a[static_cast<size_t>(y)*W+std::clamp(x+r+1,0,W-1)]-a[static_cast<size_t>(y)*W+std::clamp(x-r,0,W-1)];}}
  for(int x=0;x<W;++x){float acc=0;for(int y=-r;y<=r;++y)acc+=t[static_cast<size_t>(std::clamp(y,0,H-1))*W+x];
   for(int y=0;y<H;++y){a[static_cast<size_t>(y)*W+x]=acc/(2*r+1);acc+=t[static_cast<size_t>(std::clamp(y+r+1,0,H-1))*W+x]-t[static_cast<size_t>(std::clamp(y-r,0,H-1))*W+x];}}}
 const int oy=static_cast<int>(std::lround(4.f*k));
 sp.bmp=std::make_unique<Gdiplus::Bitmap>(W,H,PixelFormat32bppPARGB);Gdiplus::BitmapData d{};Gdiplus::Rect rr(0,0,W,H);
 if(sp.bmp->LockBits(&rr,Gdiplus::ImageLockModeWrite,PixelFormat32bppPARGB,&d)==Gdiplus::Ok){
  for(int y=0;y<H;++y){auto* out=reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(d.Scan0)+y*d.Stride);
   for(int x=0;x<W;++x){const std::uint32_t c=content[static_cast<size_t>(y)*W+x];const unsigned ca=c>>24;
    const float sh=(y-oy>=0?a[static_cast<size_t>(y-oy)*W+x]:0.f)*0.25f;const unsigned sa=static_cast<unsigned>(sh*255.f+0.5f)*(255u-ca)/255u;
    out[x]=((ca+sa)<<24)|(c&0xFFFFFFu);}
   if(iconOnly){if(sp.pixels.empty()){sp.pixels.resize(static_cast<size_t>(W)*H);sp.pw=W;sp.ph=H;}std::memcpy(sp.pixels.data()+static_cast<size_t>(y)*W,out,static_cast<size_t>(W)*4);}}  // shadow is black: adds alpha only (premultiplied)
  sp.bmp->UnlockBits(&d);}
 return sp;
}
void StartSpriteWorker(const std::shared_ptr<SpriteWorker>& w){
 std::thread([w]{const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  {std::map<int,std::unique_ptr<Gdiplus::Bitmap>> icons;  // owned by this thread only
   for(;;){SpriteJob job;{std::unique_lock<std::mutex> lock(w->m);w->cv.wait(lock,[&]{return w->quit||!w->jobs.empty();});if(w->quit)break;job=std::move(w->jobs.front());w->jobs.pop_front();}
    if(job.gen!=w->gen.load()||std::abs(static_cast<int>(job.e)-w->center.load())>24){std::lock_guard<std::mutex> lock(w->m);++w->skipped;continue;}
    const double t0=NowMs();Entry entry;entry.name=job.name;entry.path=job.path;entry.attributes=job.attributes;RowSprite sp=BuildRow(icons,entry,job.k,job.left,job.iconOnly,job.preview);const double ms=NowMs()-t0;
    HWND target=nullptr;{std::lock_guard<std::mutex> lock(w->m);if(w->quit)break;w->done.push_back({job.e,job.gen,std::move(sp),ms});w->buildMs+=ms;w->buildMax=(std::max)(w->buildMax,ms);++w->built;target=w->h;}
    if(target)PostMessageW(target,kSpriteReady,0,0);}
   std::lock_guard<std::mutex> lock(w->m);w->done.clear();w->jobs.clear();}
  if(SUCCEEDED(co))CoUninitialize();}).detach();
}
// Queue a row (front = needed now, back = prefetch). Never builds on the panel thread.
void RequestRow(Panel* p,size_t e,bool urgent){
 if(e>=p->entries.size()||p->sprites.count(e)||!p->pending.insert(e).second)return;
 SpriteJob job;job.e=e;job.gen=p->generation;job.name=p->entries[e].name;job.path=p->entries[e].path;job.preview=FanPreview;job.attributes=p->entries[e].attributes;job.k=p->dpi/96.f;job.left=p->lastLeftLabel;job.iconOnly=p->gpu!=nullptr;
 {std::lock_guard<std::mutex> lock(p->worker->m);if(urgent)p->worker->jobs.push_front(std::move(job));else p->worker->jobs.push_back(std::move(job));}
 p->worker->cv.notify_one();
}
void BuildChrome(Panel* p,int W,int H,int total,int visible){
 const float k=p->dpi/96.f;p->chromePixels.assign(static_cast<size_t>(W)*H,0u);p->chrome=std::make_unique<Gdiplus::Bitmap>(W,H,W*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(p->chromePixels.data()));p->chromeKey=p->scroll*100000+total*10+(p->error?1:0);
 Gdiplus::Graphics g(p->chrome.get());Quality(g);
 Gdiplus::FontFamily family(liquid::design::type::FamilyGdi);Gdiplus::Font font(&family,liquid::design::type::Label*k,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
 std::wstring title=L"Downloads";
 if(total>visible)title+=L"  "+std::to_wstring(p->scroll+1)+L"–"+std::to_wstring(p->scroll+visible)+L" of "+std::to_wstring(total);  // like the reference's "more" capsule
 Gdiplus::RectF measured;g.MeasureString(title.c_str(),-1,&font,Gdiplus::PointF(0,0),&measured);
 Gdiplus::RectF tr(8*k,8*k,(std::min)(measured.Width+24*k,static_cast<float>(W)-110*k),28*k);Capsule(g,tr);CapsuleText(g,font,title,tr);
 p->refreshRect={W-Px(p,92),Px(p,8),W-Px(p,8),Px(p,36)};
 Gdiplus::RectF rf(static_cast<float>(p->refreshRect.left),static_cast<float>(p->refreshRect.top),static_cast<float>(p->refreshRect.right-p->refreshRect.left),static_cast<float>(p->refreshRect.bottom-p->refreshRect.top));
 Capsule(g,rf);CapsuleText(g,font,L"Refresh",rf);
 p->explorerRect={W-Px(p,158),H-Px(p,38),W-Px(p,8),H-Px(p,10)};
 Gdiplus::RectF ex(static_cast<float>(p->explorerRect.left),static_cast<float>(p->explorerRect.top),static_cast<float>(p->explorerRect.right-p->explorerRect.left),static_cast<float>(p->explorerRect.bottom-p->explorerRect.top));
 Capsule(g,ex);CapsuleText(g,font,L"Open in Explorer  ›",ex);
 if(!total||p->error){Gdiplus::RectF msg(8*k,48*k,W-16*k,28*k);Capsule(g,msg);CapsuleText(g,font,p->error?L"Downloads is unavailable":L"No visible items (or loading…)",msg);}
}
Gdiplus::PointF ToRow(const FanHit& hit,float x,float y){
 const float a=-hit.angle*3.14159265f/180.f,dx=x-hit.cx,dy=y-hit.cy;
 return {(dx*std::cos(a)-dy*std::sin(a))/hit.scale,(dx*std::sin(a)+dy*std::cos(a))/hit.scale};
}
int FanHitTest(const Panel* p,int x,int y){
 for(const auto& hit:p->hits){
  if(hit.hasLabel){const float a=-hit.angle*3.14159265f/180.f,dx=x-hit.lcx,dy=y-hit.lcy;const float u=dx*std::cos(a)-dy*std::sin(a),v=dx*std::sin(a)+dy*std::cos(a);
   if(std::fabs(u)<=hit.lhw&&std::fabs(v)<=hit.lhh)return hit.entry;}
  const auto q=ToRow(hit,static_cast<float>(x),static_cast<float>(y));if(hit.row.Contains(q))return hit.entry;}return -1;
}
void RenderFanFrame(HWND h,Panel* p);
// Lightweight frame timing; written to %LIQUID_FAN_PROFILE% (if set) when the panel closes.
void RenderFan(HWND h,Panel* p){LARGE_INTEGER f{},a{},b{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);RenderFanFrame(h,p);QueryPerformanceCounter(&b);if(p->needsFrame)Animate(p);
 const double ms=(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart;p->frameMs+=ms;p->frameMax=(std::max)(p->frameMax,ms);++p->frames;}
// V0.9 GPU path: Liquid Glass capsules (real WGC backdrop through LiquidGlassMaterial) drawn first, then
// icons (pre-built icon + blurred shadow pixels, rotated with the arc) and white labels via D2D.
// Capsules follow the row along the arc and scale with it but stay axis-aligned (the glass shader draws
// axis-aligned continuous rectangles); icons keep the arc lean. Hit testing uses the same rectangles.
void RenderFanGpu(HWND h,Panel* p){
 RECT r{};GetClientRect(h,&r);const int W=r.right,H=r.bottom;if(W<=0||H<=0)return;p->needsFrame=false;
 const float k=p->dpi/96.f,WD=W/k,HD=H/k,A=p->alpha/255.f;p->hits.clear();
 const int total=static_cast<int>(p->entries.size()),count=(std::min)(FanVisibleCount(total,HD),total);
 struct Row{size_t e;const RowSprite* sp;float cx,cy,angle,scale,alpha;D2D1_RECT_F cap;bool hover;};
 std::vector<Row> rows;
 const int first=(std::max)(0,static_cast<int>(std::floor(p->scrollPos))-1),last=(std::min)(total-1,static_cast<int>(std::ceil(p->scrollPos))+count);
 for(int idx=first;idx<=last&&count>0;++idx){
  const size_t e=static_cast<size_t>(idx);const float slot=idx-p->scrollPos;
  const float past=(std::max)({-slot,slot-(count-1),0.f});const float edge=(std::max)(0.f,1.f-past/0.6f);if(edge<=0.f)continue;
  const float place=slot<0?slot/3.f:slot>count-1?count-1+(slot-count+1)/3.f:slot;
  const float local=std::clamp(p->progress*(1.f+0.08f*count)-0.08f*(std::max)(0.f,slot),0.f,1.f);if(local<=0.f)continue;
  const float ease=[&]{constexpr float c=1.4f;const float t=local-1.f;return 1.f+(c+1.f)*t*t*t+c*t*t;}();
  const auto pl=FanPlace(place,count,WD,HD,p->anchorX/k);
  const float ix=pl.startX+(pl.finalX-pl.startX)*ease,iy=pl.startY+(pl.finalY-pl.startY)*ease;
  auto sit=p->springs.find(e);const float lift=sit==p->springs.end()?1.f:sit->second.x;
  const bool leftLabel=pl.labelLeft;p->lastLeftLabel=leftLabel;p->haveLeftLabel=true;
  auto found=p->sprites.find(e);const bool ready=found!=p->sprites.end()&&!found->second.pixels.empty();
  if(!ready){RequestRow(p,e,true);++p->misses;}
  if(!p->placeholder.bmp){std::map<int,std::unique_ptr<Gdiplus::Bitmap>> none;p->placeholder=BuildRow(none,Entry{},k,leftLabel,true);}
  const RowSprite& sp=ready?found->second:p->placeholder;
  Row row;row.e=e;row.sp=&sp;row.cx=ix;row.cy=iy;row.scale=(0.4f+0.6f*ease)*(0.85f+0.15f*edge)*lift;row.alpha=(std::min)(1.f,local*2.f)*edge*A;
  row.angle=FanTilt(place,leftLabel);row.hover=static_cast<int>(e)==p->hover;
  // Label centre: the row-local label centre carried through the row's rotation and scale.
  const float capW=ready?sp.labelW:72.f,capH=28.f,lx=leftLabel?-24.f-8.f-capW*0.5f:24.f+8.f+capW*0.5f;
  const float rad=row.angle*3.14159265f/180.f,ccx=ix+std::cos(rad)*lx*row.scale,ccy=iy+std::sin(rad)*lx*row.scale;
  row.cap=D2D1::RectF(ccx-capW*0.5f*row.scale,ccy-capH*0.5f*row.scale,ccx+capW*0.5f*row.scale,ccy+capH*0.5f*row.scale);
  rows.push_back(row);
  if(edge>0.5f){FanHit hit;hit.entry=static_cast<int>(e);hit.cx=ix*k;hit.cy=iy*k;hit.angle=row.angle;hit.scale=row.scale;hit.row=Gdiplus::RectF(-24.f*k,-24.f*k,48.f*k,48.f*k);
   hit.hasLabel=true;hit.lcx=(row.cap.left+row.cap.right)*0.5f*k;hit.lcy=(row.cap.top+row.cap.bottom)*0.5f*k;hit.lhw=(row.cap.right-row.cap.left)*0.5f*k;hit.lhh=(row.cap.bottom-row.cap.top)*0.5f*k;p->hits.push_back(hit);}
 }
 // Chrome capsules (same glass): title, Refresh, Open in Explorer; hit rectangles stay in pixels.
 std::wstring title=L"Downloads";
 if(total>count&&count>0)title+=L"  "+std::to_wstring(p->scroll+1)+L"–"+std::to_wstring(p->scroll+count)+L" of "+std::to_wstring(total);
 const float titleW=(std::min)(26.f+7.2f*static_cast<float>(title.size()),WD-110.f);
 const D2D1_RECT_F titleR=D2D1::RectF(8,8,8+titleW,36),refreshR=D2D1::RectF(WD-92,8,WD-8,36),explorerR=D2D1::RectF(WD-158,HD-38,WD-8,HD-10);
 (void)titleR;(void)refreshR;(void)explorerR;p->refreshRect={};p->explorerRect={};  // fan: no capsules; the arrow button opens Explorer
 // Arrow button one row above the top file (V0.20, user reference); opens the folder in Explorer.
 struct Arrow{bool show=false;float cx=0,cy=0,scale=1,alpha=0;} arrow;
 if(count>0){const float local=std::clamp(p->progress*(1.f+0.08f*count)-0.08f*count,0.f,1.f);
  if(local>0.f){const float ease=[&]{constexpr float c=1.4f;const float t=local-1.f;return 1.f+(c+1.f)*t*t*t+c*t*t;}();
   const auto pl=FanPlace(static_cast<float>(count),count,WD,HD,p->anchorX/k);
   arrow.show=true;arrow.cx=pl.startX+(pl.finalX-pl.startX)*ease;arrow.cy=pl.startY+(pl.finalY-pl.startY)*ease;
   arrow.scale=(0.4f+0.6f*ease)*(p->hover==-2?(p->pressed==-2?0.92f:1.08f):1.f);arrow.alpha=(std::min)(1.f,local*2.f)*A;
   if(!p->arrow.id)p->arrow=BuildArrow(k);
   FanHit hit;hit.entry=-2;hit.cx=arrow.cx*k;hit.cy=arrow.cy*k;hit.scale=arrow.scale;hit.row=Gdiplus::RectF(-23.f*k,-23.f*k,46.f*k,46.f*k);p->hits.push_back(hit);}}
 const bool message=false;const D2D1_RECT_F messageR=D2D1::RectF(8,48,WD-8,76);(void)total;
 auto& g=*p->gpu;g.BeginFrame();
 if(message)g.Capsule(messageR,A,false,0,-4);
 for(const auto& row:rows)g.Capsule(row.cap,row.alpha,row.hover,row.angle,static_cast<int>(row.e));  // capsules lean with the arc like the reference rows
 g.BeginOverlay();
 for(const auto& row:rows){const RowSprite& sp=*row.sp;if(sp.pixels.empty())continue;
  const D2D1_MATRIX_3X2_F m=D2D1::Matrix3x2F::Scale(row.scale,row.scale)*D2D1::Matrix3x2F::Rotation(row.angle)*D2D1::Matrix3x2F::Translation(row.cx,row.cy);
  g.Image(sp.id,sp.pixels.data(),sp.pw,sp.ph,m,D2D1::RectF(-sp.px/k,-sp.py/k,(sp.pw-sp.px)/k,(sp.ph-sp.py)/k),row.alpha);}
 for(const auto& row:rows){if(row.e>=p->entries.size()||row.scale<0.55f)continue;const auto& c=row.cap;g.Text(p->entries[row.e].name,D2D1::RectF(c.left+10*row.scale,c.top,c.right-10*row.scale,c.bottom),row.alpha,row.angle,static_cast<int>(row.e));}
 if(arrow.show){const RowSprite& a=p->arrow;const D2D1_MATRIX_3X2_F m=D2D1::Matrix3x2F::Scale(arrow.scale,arrow.scale)*D2D1::Matrix3x2F::Translation(arrow.cx,arrow.cy);
  g.Image(a.id,a.pixels.data(),a.pw,a.ph,m,D2D1::RectF(-a.px/k,-a.py/k,(a.pw-a.px)/k,(a.ph-a.py)/k),arrow.alpha);}
 if(message)g.Text(p->error?L"Downloads is unavailable":L"No visible items (or loading…)",messageR,A,0,-4);
 g.EndFrame();
 if(!p->hitsLogged&&p->progress>=1.f&&p->scrollStart<0&&!rows.empty()){p->hitsLogged=true;RECT wr{};GetWindowRect(h,&wr);std::wstring line=L"fan hits:";
  for(const auto& hit:p->hits)line+=L" "+std::to_wstring(hit.entry)+L"@"+std::to_wstring(wr.left+static_cast<LONG>(hit.lcx))+L","+std::to_wstring(wr.top+static_cast<LONG>(hit.lcy))+L"/icon"+std::to_wstring(wr.left+static_cast<LONG>(hit.cx))+L","+std::to_wstring(wr.top+static_cast<LONG>(hit.cy));
  g.Note(line);}
 const double gm=g.LastGlassMs();p->glassMs+=gm;p->glassMax=(std::max)(p->glassMax,gm);++p->glassFrames;
}
void RenderFanFrame(HWND h,Panel* p){
 if(p->gpu){RenderFanGpu(h,p);return;}
 EnsureGdiplus();RECT r{};GetClientRect(h,&r);const int W=r.right,H=r.bottom;if(W<=0||H<=0)return;p->needsFrame=false;
 if(!p->dc||p->bw!=W||p->bh!=H){  // persistent premultiplied DIB, recreated only on resize
  if(p->dc){SelectObject(p->dc,p->oldBmp);DeleteObject(p->dib);DeleteDC(p->dc);p->dc=nullptr;}
  BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=W;bi.bmiHeader.biHeight=-H;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  p->dc=CreateCompatibleDC(nullptr);p->dib=p->dc?CreateDIBSection(p->dc,&bi,DIB_RGB_COLORS,&p->bits,nullptr,0):nullptr;
  if(!p->dib||!p->bits){if(p->dib)DeleteObject(p->dib);if(p->dc)DeleteDC(p->dc);p->dc=nullptr;return;}
  p->oldBmp=SelectObject(p->dc,p->dib);p->bw=W;p->bh=H;p->canvas=std::make_unique<Gdiplus::Bitmap>(W,H,W*4,PixelFormat32bppPARGB,static_cast<BYTE*>(p->bits));p->chrome.reset();}
 const float k=p->dpi/96.f;const BYTE A=p->alpha;p->hits.clear();
 const int total=static_cast<int>(p->entries.size()),count=(std::min)(FanVisibleCount(total,H/k),total);
 if(!p->chrome||p->chromeKey!=p->scroll*100000+total*10+(p->error?1:0)){BuildTimer timer(p);BuildChrome(p,W,H,total,count);}
 std::memcpy(p->bits,p->chromePixels.data(),static_cast<size_t>(W)*H*4);  // static chrome: raw copy, no blending
 {Gdiplus::Graphics g(p->canvas.get());g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);g.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
  auto draw=[&](Gdiplus::Bitmap* bmp,const Gdiplus::RectF& dst,BYTE a){
   if(a>=255){g.DrawImage(bmp,dst);return;}
   Gdiplus::ColorMatrix cm={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,a/255.f,0},{0,0,0,0,1}}};Gdiplus::ImageAttributes ia;ia.SetColorMatrix(&cm);
   g.DrawImage(bmp,dst,0,0,static_cast<Gdiplus::REAL>(bmp->GetWidth()),static_cast<Gdiplus::REAL>(bmp->GetHeight()),Gdiplus::UnitPixel,&ia);};
  // Launcher instance-fan math: slot = index - scroll; rows scrolled past either end travel a third of the
  // distance and fade out within 0.6 slot; staggered start uses max(0,slot); OutBack c=1.4.
  const int first=(std::max)(0,static_cast<int>(std::floor(p->scrollPos))-1),last=(std::min)(total-1,static_cast<int>(std::ceil(p->scrollPos))+count);
  for(int idx=first;idx<=last&&count>0;++idx){
   const size_t e=static_cast<size_t>(idx);const float slot=idx-p->scrollPos;
   const float past=(std::max)({-slot,slot-(count-1),0.f});const float edge=(std::max)(0.f,1.f-past/0.6f);if(edge<=0.f)continue;
   const float place=slot<0?slot/3.f:slot>count-1?count-1+(slot-count+1)/3.f:slot;
   const float local=std::clamp(p->progress*(1.f+0.08f*count)-0.08f*(std::max)(0.f,slot),0.f,1.f);if(local<=0.f)continue;
   const float ease=[&]{constexpr float c=1.4f;const float t=local-1.f;return 1.f+(c+1.f)*t*t*t+c*t*t;}();
   const auto pl=FanPlace(place,count,W/k,H/k,p->anchorX/k);
   const float ix=pl.startX+(pl.finalX-pl.startX)*ease,iy=pl.startY+(pl.finalY-pl.startY)*ease;
   const int ei=static_cast<int>(e);auto sit=p->springs.find(e);const float lift=sit==p->springs.end()?1.f:sit->second.x;
   const bool leftLabel=pl.labelLeft;p->lastLeftLabel=leftLabel;p->haveLeftLabel=true;const int i=static_cast<int>(std::lround(std::fabs(place)));(void)i;
   auto found=p->sprites.find(e);const bool ready=found!=p->sprites.end()&&found->second.bmp;
   if(!ready){RequestRow(p,e,true);++p->misses;if(!p->placeholder.bmp){std::map<int,std::unique_ptr<Gdiplus::Bitmap>> none;p->placeholder=BuildRow(none,Entry{},k,leftLabel);}}
   const RowSprite& sp=ready?found->second:p->placeholder;
   FanHit hit;hit.entry=ei;hit.cx=ix*k;hit.cy=iy*k;hit.scale=(0.4f+0.6f*ease)*(0.85f+0.15f*edge)*lift;hit.row=sp.row;
   hit.angle=FanTilt(place,leftLabel);  // lean with the arc tangent
   g.ResetTransform();g.TranslateTransform(hit.cx,hit.cy);g.RotateTransform(hit.angle);g.ScaleTransform(hit.scale,hit.scale);
   draw(sp.bmp.get(),Gdiplus::RectF(-sp.px,-sp.py,static_cast<float>(sp.bmp->GetWidth()),static_cast<float>(sp.bmp->GetHeight())),static_cast<BYTE>(255.f*(std::min)(1.f,local*2.f)*edge));
   if(edge>0.5f)p->hits.push_back(hit);
  }}
 HDC screen=GetDC(nullptr);RECT wr{};GetWindowRect(h,&wr);POINT dst{wr.left,wr.top},src{0,0};SIZE size{W,H};BLENDFUNCTION blend{AC_SRC_OVER,0,A,AC_SRC_ALPHA};  // global fade: free in DWM
 const double u0=NowMs();UpdateLayeredWindow(h,screen,&dst,&size,p->dc,&src,0,&blend,ULW_ALPHA);p->ulwMs+=NowMs()-u0;ReleaseDC(nullptr,screen);
}
void Redraw(HWND h,Panel* p){if(p->settings.view==View::Fan)RenderFan(h,p);else InvalidateRect(h,nullptr,FALSE);}
// The panel thread needs OLE for drag and drop and the Shell's clipboard verbs.
void EnsureOle(){static thread_local bool done=false;if(!done){done=true;OleInitialize(nullptr);}}
Microsoft::WRL::ComPtr<IContextMenu> ShellMenuFor(HWND h,const std::wstring& path){
 Microsoft::WRL::ComPtr<IShellItem> item;Microsoft::WRL::ComPtr<IContextMenu> cm;
 if(SUCCEEDED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item))))item->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&cm));
 (void)h;return cm;
}
// Ctrl+C / Ctrl+X: the Shell's own copy / cut, so Explorer and other apps can paste the file.
bool InvokeVerb(HWND h,const std::wstring& path,const char* verb){
 EnsureOle();auto cm=ShellMenuFor(h,path);if(!cm)return false;
 HMENU menu=CreatePopupMenu();if(!menu)return false;cm->QueryContextMenu(menu,0,1,0x7FFF,CMF_NORMAL);
 CMINVOKECOMMANDINFO ici{sizeof(ici)};ici.hwnd=h;ici.lpVerb=verb;ici.nShow=SW_SHOWNORMAL;
 const bool ok=SUCCEEDED(cm->InvokeCommand(&ici));DestroyMenu(menu);return ok;
}
// Right click: the file's Shell context menu (Open with, Copy, Cut, Share, Delete, Properties, ...). Commands are
// invoked for the Dock window so their dialogs outlive the panel; the panel closes afterwards.
void ShowShellMenu(HWND h,Panel* p,const std::wstring& path,POINT screen){
 EnsureOle();auto cm=ShellMenuFor(h,path);if(!cm)return;
 HMENU menu=CreatePopupMenu();if(!menu)return;
 if(FAILED(cm->QueryContextMenu(menu,0,1,0x7FFF,CMF_NORMAL|CMF_EXPLORE))){DestroyMenu(menu);return;}
 p->menuOpen=true;p->menuHandler=cm.Get();p->hover=p->pressed=-1;Animate(p);
 SetForegroundWindow(h);
 const UINT cmd=static_cast<UINT>(TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,h,nullptr));
 p->menuHandler=nullptr;
 if(cmd>0){HWND owner=GetWindow(h,GW_OWNER);CMINVOKECOMMANDINFOEX ici{sizeof(ici)};ici.fMask=CMIC_MASK_UNICODE|CMIC_MASK_PTINVOKE;ici.hwnd=owner?owner:h;
  ici.lpVerb=MAKEINTRESOURCEA(cmd-1);ici.lpVerbW=MAKEINTRESOURCEW(cmd-1);ici.nShow=SW_SHOWNORMAL;ici.ptInvoke=screen;
  cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));}
 DestroyMenu(menu);p->menuOpen=false;
 if(cmd>0||p->closeAfter){p->closeAfter=false;BeginClose(h,p);}else RequestScan(p);
}
// Dragging a row out: the Shell's data object for the file (Explorer, mail, chat and editors accept it) with the
// Shell drag image. The panel closes after a drop, as on macOS.
void StartDrag(HWND h,Panel* p,int index){
 if(index<0||index>=static_cast<int>(p->entries.size()))return;
 EnsureOle();Microsoft::WRL::ComPtr<IShellItem> item;Microsoft::WRL::ComPtr<IDataObject> data;
 if(FAILED(SHCreateItemFromParsingName(p->entries[index].path.c_str(),nullptr,IID_PPV_ARGS(&item)))||FAILED(item->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(&data))))return;
 p->dragging=true;p->pressed=p->hover=-1;Animate(p);
 DWORD effect=DROPEFFECT_NONE;const HRESULT hr=SHDoDragDrop(h,data.Get(),nullptr,DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK,&effect);
 p->dragging=false;p->suppressClick=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;  // DoDragDrop normally consumes the release
 if(p->gpu)p->gpu->Note(L"fan drag out: hr="+std::to_wstring(static_cast<long>(hr))+L" effect="+std::to_wstring(effect));
 if(hr==DRAGDROP_S_DROP||p->closeAfter){p->closeAfter=false;BeginClose(h,p);}else RequestScan(p);
}
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
 Panel* p=reinterpret_cast<Panel*>(GetWindowLongPtrW(h,GWLP_USERDATA));
 if(m==WM_NCCREATE){p=reinterpret_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
 if(!p)return DefWindowProcW(h,m,w,l);
 switch(m){
 case WM_CREATE:{BOOL animations=TRUE;if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0))p->reducedMotion=!animations;p->dpi=GetDpiForWindow(h);if(!p->dpi)p->dpi=96;p->opened=GetTickCount64();p->openedMs=NowMs();{std::lock_guard<std::mutex> lock(p->shared->mutex);p->shared->window=h;}RequestScan(p);SetTimer(h,1,4000,nullptr);
  p->ticker->h=h;StartTicker(p->ticker);p->worker=std::make_shared<SpriteWorker>();p->worker->h=h;StartSpriteWorker(p->worker);Animate(p);return 0;}
 case WM_SETTINGCHANGE:{BOOL animations=TRUE;if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0)&&p->reducedMotion!=!animations){p->reducedMotion=!animations;Animate(p);}break;}
 case kSpriteReady:{std::vector<SpriteDone> done;{std::lock_guard<std::mutex> lock(p->worker->m);done.swap(p->worker->done);}
  bool any=false;for(auto& d:done){if(d.gen!=p->generation)continue;p->pending.erase(d.e);p->sprites[d.e]=std::move(d.sprite);any=true;}
  if(any&&p->settings.view==View::Fan){p->needsFrame=true;Animate(p);}return 0;}
 case kTick:{p->ticker->pending=false;const double now=NowMs();
  if(p->lastTick>0){const double d=now-p->lastTick;p->intervalSum+=d;++p->intervals;p->intervalMax=(std::max)(p->intervalMax,d);if(d>25.0)++p->longFrames;if(d>8.33)++p->over8;if(d>16.67)++p->over16;}p->lastTick=now;
  bool busy=false;
  if(p->closing){const double el=now-p->closingMs;p->progress=p->reducedMotion?0.f:FanProgress(p->closeFrom,0.f,static_cast<float>(el),180.f);if(p->reducedMotion||el>=180){DestroyWindow(h);return 0;}busy=true;}
  else{const double el=now-p->openedMs;p->progress=p->reducedMotion?1.f:FanProgress(0.f,1.f,static_cast<float>(el),360.f);busy=!p->reducedMotion&&el<360;}
  if(p->scrollStart>=0){const float t=p->reducedMotion?1.f:static_cast<float>(std::clamp((now-p->scrollStart)/220.0,0.0,1.0));const float e=1.f-(1.f-t)*(1.f-t)*(1.f-t);  // OutCubic 220 ms
   p->scrollPos=p->scrollFrom+(p->scroll-p->scrollFrom)*e;if(t>=1.f){p->scrollPos=static_cast<float>(p->scroll);p->scrollStart=-1;}else busy=true;}
  // Hover spring like Qt SpringAnimation{spring:4.5;damping:0.32}, integrated in fixed 16 ms steps.
  if(p->reducedMotion){p->springs.clear();p->springClock=now;}
  if(p->springClock<=0)p->springClock=now;while(!p->reducedMotion&&p->springClock+16.0<=now){p->springClock+=16.0;
   for(auto it=p->springs.begin();it!=p->springs.end();){const int e=static_cast<int>(it->first);const float target=(e==p->pressed&&e==p->hover)?0.92f:e==p->hover?1.08f:1.f;
    auto& s=it->second;const float diff=target-s.x;s.v+=4.5f*diff-0.32f*s.v;s.x+=s.v*0.016f;
    if(std::fabs(target-s.x)<0.002f&&std::fabs(s.v)<0.01f){s.x=target;s.v=0;if(target==1.f){it=p->springs.erase(it);continue;}}++it;}}
  for(const auto& [e,s]:p->springs){const int ei=static_cast<int>(e);const float target=(ei==p->pressed&&ei==p->hover)?0.92f:ei==p->hover?1.08f:1.f;if(s.x!=target||s.v!=0)busy=true;}
  // Idle only: pre-build one missing row sprite per tick within +-6 rows of the scroll target and drop
  // sprites farther than 18 rows away, so fast scrolling reuses ready rows without slowing animation.
  if(!busy&&!p->needsFrame&&p->settings.view==View::Fan&&p->haveLeftLabel&&!p->entries.empty()){
   const int total=static_cast<int>(p->entries.size());RECT cr{};GetClientRect(h,&cr);const int vis=FanVisibleCount(total,cr.bottom/(p->dpi/96.f));
   // V0.7: +-16 rows (fast 15-notch scrolls missed a +-6 window); keep within 24 rows (~3.5 MB max).
   for(auto it=p->sprites.begin();it!=p->sprites.end();)if(std::abs(static_cast<int>(it->first)-p->scroll)>24)it=p->sprites.erase(it);else ++it;
   for(int e=(std::max)(0,p->scroll-16);e<(std::min)(total,p->scroll+vis+16);++e){auto f=p->sprites.find(static_cast<size_t>(e));
    if(f==p->sprites.end()||!f->second.bmp){if(!p->pending.count(static_cast<size_t>(e)))++p->prefetched;RequestRow(p,static_cast<size_t>(e),false);}}}
  if(p->gpu&&p->gpu->PollCapture())p->needsFrame=true;
  // Test snapshots (LIQUID_FAN_SNAPSHOT): 1.5 s after opening and 0.6 s after the last wheel step.
  if(p->gpu&&!p->snapPrefix.empty()&&!p->closing){const bool open=!p->snapOpenTaken&&now-p->openedMs>1500;const bool wheel=p->lastWheelMs>0&&now-p->lastWheelMs>600;
   if(open||wheel){p->snapOpenTaken=true;p->lastWheelMs=0;p->gpu->Snapshot(p->snapPrefix+L"-"+std::to_wstring(++p->snapIndex)+L".bmp");p->needsFrame=true;}
   if(!p->snapOpenTaken||p->lastWheelMs>0)busy=true;}
  if(!busy&&!p->needsFrame&&!(p->gpu&&p->gpu->ComparePending()))p->ticker->active=false;  // a pending compare is polled every frame
  if(std::fabs(p->progress-p->postedProgress)>0.001f){p->postedProgress=p->progress;if(HWND o=GetWindow(h,GW_OWNER))PostMessageW(o,kProgressMessage,0,0);}
  ApplyAlpha(h,p,static_cast<BYTE>(std::clamp(p->progress,0.f,1.f)*255.f));return 0;}
 case WM_TIMER:if(w==1)RequestScan(p);else if(w==3&&p->gpu&&p->gpu->PollCapture()){p->needsFrame=true;Animate(p);}return 0;
 case WM_APP+71:  // dock::kMsgCaptureAvailable from the WGC backdrop source
  if(p->gpu){if(p->gpu->OnCaptureMessage())p->needsFrame=true;if(p->needsFrame||p->gpu->ComparePending())Animate(p);}return 0;
 case kScanDone:{std::vector<Entry> next;DWORD error=0;{std::lock_guard<std::mutex> lock(p->shared->mutex);next=std::move(p->shared->result);error=p->shared->error;}SortEntries(next,p->settings.sort);bool changed=error!=p->error||next.size()!=p->entries.size();if(!changed)for(size_t i=0;i<next.size();++i){const auto& a=next[i];const auto& b=p->entries[i];if(a.name!=b.name||a.path!=b.path||a.attributes!=b.attributes||CompareFileTime(&a.modified,&b.modified)!=0){changed=true;break;}}if(changed){p->entries=std::move(next);p->error=error;p->sprites.clear();p->pending.clear();++p->generation;if(p->worker){p->worker->gen=p->generation;std::lock_guard<std::mutex> lock(p->worker->m);p->worker->jobs.clear();}p->hover=p->pressed=-1;RECT bounds{};GetClientRect(h,&bounds);p->scroll=std::clamp(p->scroll,0,MaxScroll(p,bounds.right,bounds.bottom));p->scrollPos=static_cast<float>(p->scroll);p->scrollStart=-1;p->springs.clear();Redraw(h,p);}return 0;}
 case WM_MOUSEWHEEL:{RECT r{};GetClientRect(h,&r);const int delta=GET_WHEEL_DELTA_WPARAM(w);
  if(p->settings.view!=View::Fan){p->scroll=std::clamp(p->scroll+(delta<0?1:-1),0,MaxScroll(p,r.right,r.bottom));Redraw(h,p);return 0;}
  p->wheel+=delta;int steps=0;while(p->wheel>=WHEEL_DELTA){p->wheel-=WHEEL_DELTA;--steps;}while(p->wheel<=-WHEEL_DELTA){p->wheel+=WHEEL_DELTA;++steps;}
  const int next=std::clamp(p->scroll+steps,0,MaxScroll(p,r.right,r.bottom));
  if(next!=p->scroll){if(p->worker)p->worker->center=next;if(!p->snapPrefix.empty())p->lastWheelMs=NowMs();p->scrollFrom=p->scrollPos;p->scroll=next;p->scrollStart=NowMs();p->pressed=-1;Animate(p);}return 0;}
 case WM_MOUSEMOVE:if(p->settings.view==View::Fan){if(!p->tracking){TRACKMOUSEEVENT ev{sizeof(ev),TME_LEAVE,h,0};TrackMouseEvent(&ev);p->tracking=true;}
  if(p->pressed>=0&&(w&MK_LBUTTON)&&!p->dragging&&(std::abs(GET_X_LPARAM(l)-p->pressX)>GetSystemMetrics(SM_CXDRAG)||std::abs(GET_Y_LPARAM(l)-p->pressY)>GetSystemMetrics(SM_CYDRAG))){StartDrag(h,p,p->pressed);return 0;}
  const int next=FanHitTest(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));if(next!=p->hover){if(p->gpu)p->gpu->Note(L"fan hover "+std::to_wstring(next));p->hover=next;if(next==-1)p->pressed=-1;if(next>=0)p->springs.try_emplace(static_cast<size_t>(next));Animate(p);}}return 0;
 case WM_MOUSELEAVE:p->tracking=false;if(p->hover>=0||p->pressed>=0){p->hover=p->pressed=-1;if(p->settings.view==View::Fan)Animate(p);}return 0;
 case WM_LBUTTONDOWN:if(p->settings.view==View::Fan){p->pressX=GET_X_LPARAM(l);p->pressY=GET_Y_LPARAM(l);p->pressed=FanHitTest(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));if(p->pressed>=0){p->springs.try_emplace(static_cast<size_t>(p->pressed));Animate(p);}}return 0;
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:{PAINTSTRUCT ps{};HDC screen=BeginPaint(h,&ps);if(p->settings.view==View::Fan){EndPaint(h,&ps);return 0;}RECT r{};GetClientRect(h,&r);
  HDC buffer=CreateCompatibleDC(screen);HBITMAP image=buffer?CreateCompatibleBitmap(screen,(std::max)(1L,r.right),(std::max)(1L,r.bottom)):nullptr;
  HGDIOBJ previous=image?SelectObject(buffer,image):nullptr;HDC dc=image?buffer:screen;HBRUSH bg=CreateSolidBrush(p->settings.view==View::Fan?RGB(1,0,1):RGB(37,42,51));FillRect(dc,&r,bg);DeleteObject(bg);
  if(p->settings.view==View::Fan){HBRUSH chip=CreateSolidBrush(RGB(37,42,51));HGDIOBJ oldBrush=SelectObject(dc,chip);HPEN pen=CreatePen(PS_NULL,0,0);HGDIOBJ oldPen=SelectObject(dc,pen);RoundRect(dc,Px(p,8),Px(p,5),r.right-Px(p,8),Px(p,40),Px(p,14),Px(p,14));RoundRect(dc,Px(p,110),r.bottom-Px(p,43),r.right-Px(p,8),r.bottom-Px(p,5),Px(p,14),Px(p,14));SelectObject(dc,oldPen);DeleteObject(pen);SelectObject(dc,oldBrush);DeleteObject(chip);}
  SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(245,247,250));HFONT f=CreateFontW(-Px(p,static_cast<int>(liquid::design::type::Label)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,liquid::design::type::FamilyGdi);HGDIOBJ old=SelectObject(dc,f);
  HFONT medium=CreateFontW(-Px(p,static_cast<int>(liquid::design::type::Label)),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,liquid::design::type::FamilyGdi);if(!medium)medium=f;
  RECT header{Px(p,16),Px(p,8),r.right-Px(p,110),Px(p,38)};DrawTextW(dc,L"Downloads",-1,&header,DT_SINGLELINE|DT_VCENTER);
  RECT refresh{r.right-Px(p,105),Px(p,8),r.right-Px(p,12),Px(p,38)};DrawTextW(dc,L"Refresh",-1,&refresh,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
  if(p->entries.empty()&&!p->error){RECT empty{Px(p,16),Px(p,52),r.right-Px(p,16),Px(p,100)};DrawTextW(dc,L"No visible items (or loading…)",-1,&empty,DT_WORDBREAK);}
  if(p->error){RECT e{Px(p,16),Px(p,52),r.right-Px(p,16),Px(p,100)};DrawTextW(dc,L"Downloads is unavailable. Open in Explorer to check access.",-1,&e,DT_WORDBREAK);}
  const int total=static_cast<int>(p->entries.size());const int count=p->settings.view==View::Fan?FanVisibleCount(total,r.bottom/(p->dpi/96.f)):total;
  const int cols=(std::max)(1,(static_cast<int>(r.right)-Px(p,24))/Px(p,96));const int start=p->settings.view==View::Grid?p->scroll*cols:p->scroll;
  for(int i=start;i<count;++i){int visual=i-start;RECT item=EntryRect(p,visual,r.right,r.bottom);if(p->settings.view!=View::Fan&&item.top>=r.bottom-Px(p,45))break;
   HIMAGELIST images=nullptr;const bool large=p->settings.view!=View::List;const int icon=IconIndex(p->entries[i],images,large);
   if(p->settings.view==View::Grid){if(images)ImageList_Draw(images,icon,dc,(item.left+item.right)/2-Px(p,16),item.top+Px(p,3),ILD_TRANSPARENT);
    RECT text{item.left+Px(p,2),item.top+Px(p,37),item.right-Px(p,2),item.bottom-Px(p,2)};
    DrawTextW(dc,p->entries[i].name.c_str(),-1,&text,DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS|DT_NOPREFIX);
   }else if(p->settings.view==View::Fan){
    const float factor=p->dpi/96.f;
    const auto g=FanLayout(visual,count,r.right/factor,r.bottom/factor,p->anchorX/factor,p->progress);
    RECT iconRect{static_cast<LONG>(g.icon.left*factor),static_cast<LONG>(g.icon.top*factor),static_cast<LONG>(g.icon.right*factor),static_cast<LONG>(g.icon.bottom*factor)};
    RECT label{static_cast<LONG>(g.label.left*factor),static_cast<LONG>(g.label.top*factor),static_cast<LONG>(g.label.right*factor),static_cast<LONG>(g.label.bottom*factor)};
    SIZE measured{};GetTextExtentPoint32W(dc,p->entries[i].name.c_str(),static_cast<int>(p->entries[i].name.size()),&measured);
    const int desired=(std::min)(static_cast<int>(label.right-label.left),static_cast<int>(measured.cx)+Px(p,20));
    if(g.label.right<=g.icon.left)label.left=label.right-desired;else label.right=label.left+desired;
    // Light frosted pill with dark ink instead of the dark chip (reference: glass name capsule, fully round).
    HBRUSH chip=CreateSolidBrush(RGB(236,237,241));HGDIOBJ oldBrush=SelectObject(dc,chip);HPEN pen=CreatePen(PS_SOLID,1,RGB(204,207,214));HGDIOBJ oldPen=SelectObject(dc,pen);
    const int pill=static_cast<int>(label.bottom-label.top);
    if(label.right>label.left)RoundRect(dc,label.left,label.top,label.right,label.bottom,pill,pill);
    SelectObject(dc,oldPen);DeleteObject(pen);SelectObject(dc,oldBrush);DeleteObject(chip);
    if(images)ImageList_DrawEx(images,icon,dc,iconRect.left,iconRect.top,Px(p,32),Px(p,32),CLR_NONE,CLR_NONE,ILD_TRANSPARENT);
    label.left+=Px(p,10);label.right-=Px(p,10);
    const COLORREF ink=SetTextColor(dc,RGB(28,28,30));HGDIOBJ prior=SelectObject(dc,medium);
    if(label.right>label.left)DrawTextW(dc,p->entries[i].name.c_str(),-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(dc,prior);SetTextColor(dc,ink);
   }else{if(images)ImageList_Draw(images,icon,dc,item.left+Px(p,4),item.top+Px(p,5),ILD_TRANSPARENT);
    RECT text=item;text.left+=Px(p,28);DrawTextW(dc,p->entries[i].name.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);}
  }
  RECT bottom{Px(p,16),r.bottom-Px(p,40),r.right-Px(p,16),r.bottom-Px(p,8)};DrawTextW(dc,L"Open in Explorer  ›",-1,&bottom,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
  SelectObject(dc,old);if(medium!=f)DeleteObject(medium);DeleteObject(f);
  if(image){BitBlt(screen,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(buffer,previous);DeleteObject(image);}
  if(buffer)DeleteDC(buffer);EndPaint(h,&ps);return 0;}
 case WM_LBUTTONUP:{if(p->suppressClick){p->suppressClick=false;p->pressed=-1;return 0;}if(p->closing||p->progress<0.95f)return 0;
  int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);RECT r{};GetClientRect(h,&r);
  if(p->settings.view==View::Fan){POINT click{x,y};p->pressed=-1;
   if(PtInRect(&p->refreshRect,click)){if(p->gpu)p->gpu->Note(L"fan refresh");RequestScan(p);return 0;}
   if(PtInRect(&p->explorerRect,click)){OpenAsync(p->directory);BeginClose(h,p);return 0;}
   const int i=FanHitTest(p,x,y);if(i==-2){OpenAsync(p->directory);BeginClose(h,p);return 0;}
   if(i>=0&&i<static_cast<int>(p->entries.size())){auto path=p->entries[i].path;OpenAsync(std::move(path));BeginClose(h,p);}
   else if(p->gpu){p->gpu->Note(L"fan empty-area click: close");BeginClose(h,p);}return 0;}
  if(y<Px(p,40)&&x>r.right-Px(p,110)){RequestScan(p);return 0;}
  if(y>=r.bottom-Px(p,45)){OpenAsync(p->directory);BeginClose(h,p);return 0;}
  int cols=(std::max)(1,(static_cast<int>(r.right)-Px(p,24))/Px(p,96));int start=p->settings.view==View::Grid?p->scroll*cols:p->scroll;int count=static_cast<int>(p->entries.size());if(p->settings.view==View::Fan)count=FanVisibleCount(count,r.bottom/(p->dpi/96.f));
  POINT pt{x,y};for(int i=start;i<count;++i){
   bool hit=false;
   if(p->settings.view==View::Fan){
    const float factor=p->dpi/96.f;const auto g=FanLayout(i-start,count,r.right/factor,r.bottom/factor,p->anchorX/factor,p->progress);
    hit=FanContains(g.icon,x/factor,y/factor)||FanContains(g.label,x/factor,y/factor);
   }else{RECT rect=EntryRect(p,i-start,r.right,r.bottom);if(rect.top>=r.bottom-Px(p,45))break;hit=PtInRect(&rect,pt)!=FALSE;}
   if(hit){auto path=p->entries[i].path;OpenAsync(std::move(path));BeginClose(h,p);break;}
  }return 0;}
 case WM_ACTIVATE:
  if(GetEnvironmentVariableW(L"LIQUID_STACK_TRACE",nullptr,0)){wchar_t cls[64]{};if(l)GetClassNameW(reinterpret_cast<HWND>(l),cls,64);DWORD op=0;if(l)GetWindowThreadProcessId(reinterpret_cast<HWND>(l),&op);
   HWND fg=GetForegroundWindow();wchar_t fcls[64]{};DWORD fp=0;if(fg){GetClassNameW(fg,fcls,64);GetWindowThreadProcessId(fg,&fp);}
   fwprintf(stderr,L"stack trace: WM_ACTIVATE state=%u fg_is_panel=%d other=%ls pid=%lu fg=%ls fgpid=%lu self=%lu t=%.0f\n",LOWORD(w),fg==h?1:0,cls,op,fcls,fp,GetCurrentProcessId(),NowMs());}  // V0.17 opt-in trace (LIQUID_STACK_TRACE)
  if(LOWORD(w)==WA_INACTIVE){if(p->dragging||p->menuOpen){p->closeAfter=true;return 0;}BeginClose(h,p);return 0;}
  if(GetForegroundWindow()==h)RemoveDismissWatch();  // V0.18: really active now; the normal deactivate/Esc path applies
  break;
 case kDismiss:  // V0.18: outside press (1) or app switch (2) while the panel never had the foreground
  if(GetEnvironmentVariableW(L"LIQUID_STACK_TRACE",nullptr,0))fwprintf(stderr,L"stack trace: dismiss reason=%llu\n",static_cast<unsigned long long>(w));
  if(p->dragging||p->menuOpen){p->closeAfter=true;return 0;}
  RemoveDismissWatch();BeginClose(h,p);return 0;
 case WM_KEYDOWN:if(w==VK_ESCAPE){BeginClose(h,p);return 0;}
  if((w=='C'||w=='X')&&(GetKeyState(VK_CONTROL)&0x8000)&&p->hover>=0&&p->hover<static_cast<int>(p->entries.size())){
   if(InvokeVerb(h,p->entries[p->hover].path,w=='C'?"copy":"cut"))BeginClose(h,p);return 0;}
  break;
 case WM_RBUTTONUP:if(p->settings.view==View::Fan&&!p->closing&&p->progress>=0.95f){
   const int i=FanHitTest(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&pt);
   if(i>=0&&i<static_cast<int>(p->entries.size())){ShowShellMenu(h,p,p->entries[i].path,pt);return 0;}}
  return 0;
 case WM_INITMENUPOPUP:case WM_DRAWITEM:case WM_MEASUREITEM:
  if(p->menuHandler){Microsoft::WRL::ComPtr<IContextMenu2> cm2;if(SUCCEEDED(p->menuHandler->QueryInterface(IID_PPV_ARGS(&cm2)))&&SUCCEEDED(cm2->HandleMenuMsg(m,w,l)))return m==WM_INITMENUPOPUP?0:TRUE;}
  break;
 case WM_MENUCHAR:
  if(p->menuHandler){Microsoft::WRL::ComPtr<IContextMenu3> cm3;LRESULT result=0;if(SUCCEEDED(p->menuHandler->QueryInterface(IID_PPV_ARGS(&cm3)))&&SUCCEEDED(cm3->HandleMenuMsg2(m,w,l,&result)))return result;}
  break;
 case WM_NCDESTROY:{if(p->dc){p->canvas.reset();SelectObject(p->dc,p->oldBmp);DeleteObject(p->dib);DeleteDC(p->dc);p->dc=nullptr;}
  if(p->frames){wchar_t out[MAX_PATH]{};if(GetEnvironmentVariableW(L"LIQUID_FAN_PROFILE",out,MAX_PATH)){FILE* f=nullptr;if(_wfopen_s(&f,out,L"a")==0&&f){fprintf(f,"fan frames=%d render_avg=%.2fms (excl. one-time sprite builds %.2fms) render_max=%.2fms ulw_avg=%.2fms | tick_intervals=%d avg=%.2fms max=%.2fms over8.33=%d over16.67=%d long(>25ms)=%d placeholder_rows=%d prefetch_requests=%d | worker built=%d avg=%.2fms max=%.2fms skipped=%d | glass frames=%d avg=%.2fms max=%.2fms gpu=%d entries=%zu\n",p->frames,(p->frameMs-p->buildMs)/p->frames,p->buildMs,p->frameMax,p->ulwMs/p->frames,p->intervals,p->intervals?p->intervalSum/p->intervals:0.0,p->intervalMax,p->over8,p->over16,p->longFrames,p->misses,p->prefetched,p->worker?p->worker->built:0,p->worker&&p->worker->built?p->worker->buildMs/p->worker->built:0.0,p->worker?p->worker->buildMax:0.0,p->worker?p->worker->skipped:0,p->glassFrames,p->glassFrames?p->glassMs/p->glassFrames:0.0,p->glassMax,p->gpu?1:0,p->entries.size());fclose(f);}}}
  if(current==h)RemoveDismissWatch();
  if(HWND o=GetWindow(h,GW_OWNER))PostMessageW(o,kProgressMessage,0,0);  // closed: Dock icon back to normal
  KillTimer(h,3);p->gpu.reset();p->ticker->quit=true;p->ticker->active=false;SetEvent(p->ticker->wake);if(p->worker){std::lock_guard<std::mutex> lock(p->worker->m);p->worker->quit=true;p->worker->h=nullptr;}if(p->worker)p->worker->cv.notify_all();KillTimer(h,1);{std::lock_guard<std::mutex> lock(p->shared->mutex);p->shared->window=nullptr;}if(current==h)current=nullptr;if(p->destroyedDuringCreate)*p->destroyedDuringCreate=true;SetWindowLongPtrW(h,GWLP_USERDATA,0);delete p;break;}
 }return DefWindowProcW(h,m,w,l);
}
}
float OpenProgress(){if(!current||!IsWindow(current))return 0.f;auto* p=reinterpret_cast<Panel*>(GetWindowLongPtrW(current,GWLP_USERDATA));return p?std::clamp(p->progress,0.f,1.f):0.f;}
void Close(){if(current&&IsWindow(current))DestroyWindow(current);current=nullptr;}
FanRendererFactory g_fanFactory;
bool ToggleImpl(HWND owner,const RECT& iconScreen,const std::wstring& directory,const Settings& settings,bool allowGpu);
bool Toggle(HWND owner,const RECT& iconScreen,const std::wstring& directory,const Settings& settings){return ToggleImpl(owner,iconScreen,directory,settings,true);}
void SetFanRendererFactory(FanRendererFactory factory){g_fanFactory=std::move(factory);}
bool ToggleImpl(HWND owner,const RECT& iconScreen,const std::wstring& directory,const Settings& settings,bool allowGpu){
 if(current&&IsWindow(current)){auto* panel=reinterpret_cast<Panel*>(GetWindowLongPtrW(current,GWLP_USERDATA));if(panel)BeginClose(current,panel);return true;}HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=kPanelClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;
 if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;EnsureGdiplus();
 auto p=std::make_unique<Panel>();p->directory=directory;p->settings=settings;p->dpi=GetDpiForWindow(owner);if(!p->dpi)p->dpi=96;
 HMONITOR mon=MonitorFromRect(&iconScreen,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(mon,&mi))return false;
 int width=MulDiv(settings.view==View::Fan?FanPanelWidth:430,p->dpi,96);int height=MulDiv(settings.view==View::Fan?FanPanelHeight():480,p->dpi,96);
 width=(std::min)(width,static_cast<int>(mi.rcWork.right-mi.rcWork.left));height=(std::min)(height,static_cast<int>(mi.rcWork.bottom-mi.rcWork.top));
 const int center=(iconScreen.left+iconScreen.right)/2;
 int x=std::clamp(settings.view==View::Fan?center-MulDiv(FanPanelAnchor,p->dpi,96):center-width/2,static_cast<int>(mi.rcWork.left),static_cast<int>(mi.rcWork.right)-width);
 p->anchorX=static_cast<float>(center-x);
 int y=std::clamp(static_cast<int>(iconScreen.top-height-8),static_cast<int>(mi.rcWork.top),static_cast<int>(mi.rcWork.bottom)-height);
 bool destroyed=false;p->destroyedDuringCreate=&destroyed;Panel* raw=p.release();
 const bool gpu=allowGpu&&settings.view==View::Fan&&g_fanFactory;
 HWND h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|(gpu?WS_EX_NOREDIRECTIONBITMAP:WS_EX_LAYERED),kPanelClass,L"Downloads Stack",WS_POPUP,x,y,width,height,owner,nullptr,instance,raw);
 if(!h){if(!destroyed)delete raw;return false;}raw->destroyedDuringCreate=nullptr;current=h;
 if(gpu){raw->gpu=g_fanFactory();
  if(!raw->gpu||!raw->gpu->Init(h,static_cast<UINT>(width),static_cast<UINT>(height),raw->dpi/96.f)){raw->gpu.reset();DestroyWindow(h);return ToggleImpl(owner,iconScreen,directory,settings,false);}
  wchar_t snap[MAX_PATH]{};if(GetEnvironmentVariableW(L"LIQUID_FAN_SNAPSHOT",snap,MAX_PATH))raw->snapPrefix=snap;
  SetTimer(h,3,30,nullptr);}  // backdrop compare polling (non-blocking) while the Fan is open
 if(settings.view!=View::Fan){HRGN corners=CreateRoundRectRgn(0,0,width+1,height+1,MulDiv(22,raw->dpi,96),MulDiv(22,raw->dpi,96));if(corners&&!SetWindowRgn(h,corners,FALSE))DeleteObject(corners);}
 ApplyAlpha(h,raw,0);ShowWindow(h,SW_SHOWNORMAL);SetForegroundWindow(h);
 if(GetForegroundWindow()!=h&&IsWindow(h))InstallDismissWatch();  // V0.18: foreground refused; watch for dismissal instead
 return true;
}
}
