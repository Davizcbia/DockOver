#include "desktop_settings.h"
#include "mydockfinder_import.h"
#include "settings_search.h"
#include "design_tokens.h"
#include "theme_state.h"
#include "launchpad_density.h"
#include "renderer.h"
#include "backdrop_wgc.h"
#include "log.h"
#include "vblank.h"
#include "workshop.h"
#include "json_lite.h"
#include "taskbar_guardian.h"
#include "taskbar_hotkey.h"
#include "update_client.h"
#include <atomic>
#include <thread>
#include <shobjidl.h>
#include <shlobj.h>
#include <wincodec.h>
#include <vector>
#include <roapi.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <memory>
#include <algorithm>
#include "liquid_build_id.h"
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <string>
#include <cwctype>
#include <cmath>
using Microsoft::WRL::ComPtr;
static volatile const char buildMarker[]=LIQUID_BUILD_MARKER;
namespace {
constexpr wchar_t ClassName[]=L"LiquidDesktopSettingsV01";
constexpr const wchar_t* titles[]={L"General",L"Appearance",L"Dock",L"Menu Bar",L"Launchpad",L"Control Center",L"Windows Taskbar",L"Workshop",L"Migration",L"About"};
ComPtr<ID2D1Factory> factory; ComPtr<ID2D1HwndRenderTarget> fallbackTarget;
ID2D1RenderTarget* target=nullptr; ComPtr<ID2D1SolidColorBrush> brush;
std::unique_ptr<dock::Renderer> renderer;
std::unique_ptr<dock::WgcBackdropSource> backdrop;
std::wstring snapshotPath;
bool gpuReady=false; bool previousGlass=false; bool haveLoggedGlass=false;
winrt::Windows::UI::ViewManagement::UISettings systemUi{nullptr};
bool allowTransparency=false;
int appearanceMode=0,appearanceKeyboard=0; bool systemDark=false;
bool RefreshSystemAppearance(){
 bool dark=false; try { if(systemUi){ auto c=systemUi.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background); dark=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000; }} catch(...) {}
 bool changed=dark!=systemDark;systemDark=dark;return changed;
}
liquid::design::Palette SettingsPalette(){
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 auto palette=liquid::theme::Palette(appearanceMode,systemDark,contrast);
 if(!contrast&&liquid::theme::IsDark(appearanceMode,systemDark)){palette.background=0x1C1C1E;palette.navigation=0x232323;palette.surface=0x29292B;}
 return palette;
}
unsigned SearchSurface(){HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);return contrast?SettingsPalette().surface:liquid::theme::IsDark(appearanceMode,systemDark)?0x48484A:0xDFDFE2;}
bool RefreshAccessibility(){
 bool allow=false;HIGHCONTRASTW hc{sizeof(hc)};
 const bool contrastKnown=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)!=FALSE;
 try {allow=systemUi && systemUi.AdvancedEffectsEnabled() && contrastKnown && !(hc.dwFlags&HCF_HIGHCONTRASTON);} catch(...) {allow=false;}
 const bool changed=allowTransparency!=allow;if(changed)DLOG(L"settings: system transparency allowed=%d",allow);allowTransparency=allow;return changed;
}

ComPtr<IDWriteFactory> writeFactory; ComPtr<IDWriteTextFormat> normal,fontSmall,heading,caption,categoryIcons,sidebarLabel,centerSmall,trailingSmall;
int selected=0, dockKeyboardRow=0, densityKeyboardControl=0, workshopKeyboardControl=0;
dock::LocalGlassOverlay* drawingLocalGlass=nullptr;
float switchPointerX=0.f,switchPointerY=0.f;
struct SwitchMotion {bool initialized=false,on=false;float from=0,to=0;ULONGLONG start=0;};
SwitchMotion switchMotion[18];int visualSwitch=-1;float pressFrom=0,pressTo=0;ULONGLONG pressStart=0;
bool switchDragging=false;float switchDragStartX=0,switchDragStartPosition=0,switchDragPosition=0;
float localGlassWhite=0.f;
float EaseOutBack(float p){const float x=p-1.f;return 1.f+2.65f*x*x*x+.65f*x*x;}
float PressAmount(ULONGLONG now){const float t=std::clamp((now-pressStart)/150.f,0.f,1.f);const float e=1.f-std::pow(1.f-t,3.f);return pressFrom+(pressTo-pressFrom)*e;}
docky::vblank::Ticker switchTicker;  // switch animation frames
liquid::settings::Performance perf;  // General > Frame rate
// About > Software Update. One worker thread at a time checks or downloads; it posts kMsgUpdate when it finishes
// (and while downloading, for progress). Switches 14 and 15: check automatically, install automatically.
liquid::settings::Updates updatePref;
constexpr UINT kMsgUpdate = WM_APP + 310;
enum class UpdUi { Idle, Checking, UpToDate, NoRelease, Available, Downloading, Installing, Error };
UpdUi updUi = UpdUi::Idle;
docky::update::Release updRelease;
std::wstring updError;
std::atomic<uint64_t> updDone{0}, updTotal{0};
std::atomic<bool> updCancel{false}, updBusy{false};
docky::update::State updState;
D2D1_RECT_F UpdateCheckButton(float left, float top) { return D2D1::RectF(left + 16, top + 104, left + 204, top + 136); }
D2D1_RECT_F UpdateInstallButton(float left, float top) { return D2D1::RectF(left + 216, top + 104, left + 404, top + 136); }
float UpdateCardTop() { return liquid::design::settings::CardTop + 166.f; }
float UpdateSwitchCardTop(int row) { return UpdateCardTop() + 162.f + row * (liquid::design::settings::CardHeight + 12.f); }
D2D1_RECT_F UpdateSwitch(float right, int row) {
 namespace lay = liquid::design::settings;
 const float top = UpdateSwitchCardTop(row) + (lay::CardHeight - lay::SwitchHeight) * .5f;
 return D2D1::RectF(right - lay::SwitchRight - lay::SwitchWidth, top, right - lay::SwitchRight, top + lay::SwitchHeight);
}
std::wstring UpdateWhen(int64_t t) {
 if (!t) return L"never";
 const int64_t ago = docky::update::UnixNow() - t;
 if (ago < 90) return L"just now";
 if (ago < 3600) return std::to_wstring(ago / 60) + L" minutes ago";
 if (ago < 2 * 86400) return std::to_wstring(ago / 3600) + L" hours ago";
 return std::to_wstring(ago / 86400) + L" days ago";
}
void StartUpdateCheck(HWND h) {
 if (updBusy.exchange(true)) return;
 updUi = UpdUi::Checking; updError.clear();
 std::thread([h] {
  auto* r = new docky::update::CheckResult(docky::update::Check());
  if (r->status != docky::update::CheckStatus::Failed) {
   auto st = docky::update::LoadState();
   st.lastCheck = docky::update::UnixNow();
   st.latest = r->status == docky::update::CheckStatus::Available ? r->release.version : std::string();
   if (r->status == docky::update::CheckStatus::Available) st.notified = r->release.version;  // seen here: no notification
   docky::update::SaveState(st);
  }
  updBusy = false;
  if (!PostMessageW(h, kMsgUpdate, 1, reinterpret_cast<LPARAM>(r))) delete r;
 }).detach();
}
void StartUpdateDownload(HWND h) {
 if (updRelease.assetUrl.empty() || updBusy.exchange(true)) return;
 updUi = UpdUi::Downloading; updError.clear(); updDone = 0; updTotal = updRelease.size; updCancel = false;
 const auto rel = updRelease;
 std::thread([h, rel] {
  const std::wstring zip = docky::update::WorkDir() + L"\\" + dock::json::FromUtf8(rel.assetName);
  auto* err = new std::wstring;
  ULONGLONG lastPost = 0;
  const bool ok = docky::update::Download(rel, zip, [&](uint64_t done, uint64_t total) {
   updDone = done; if (total) updTotal = total;
   const ULONGLONG now = GetTickCount64();
   if (now - lastPost > 100) { lastPost = now; PostMessageW(h, kMsgUpdate, 3, 0); }
  }, &updCancel, *err);
  if (ok) {
   err->clear();
   docky::update::StartInstall(zip, rel.version, true, true, *err);  // fills err on failure
  }
  updBusy = false;
  if (!PostMessageW(h, kMsgUpdate, 2, reinterpret_cast<LPARAM>(err))) delete err;
 }).detach();
}
// General page, "Frame rate" card: two rows of five choices (30, 60, 120, 240, Unlimited).
constexpr int kFrameCaps[5] = {30, 60, 120, 240, 0};
D2D1_RECT_F FrameCapButton(int row, int i, float left, float right) {
 namespace lay = liquid::design::settings; const float top = lay::CardTop + 178.f, x0 = right - 16.f - 5 * 74.f - 4 * 6.f;
 const float y = top + 44.f + row * 40.f, x = x0 + i * 80.f; return D2D1::RectF(x, y, x + 74.f, y + 30.f);
}
void BeginSwitchPress(HWND h,int id,bool down){const auto now=GetTickCount64();const float current=visualSwitch==id?PressAmount(now):0.f;visualSwitch=id;pressFrom=down?current:std::max(current,.55f);pressTo=down?1.f:0.f;pressStart=now;SetTimer(h,4,4,nullptr);}

int chromeHover=-1,chromePressed=-1;bool chromeTracking=false;bool keyboardFocusVisible=false;int switchPressed=-1; float scale=1; bool diagnostics=false, iconMask=true, autoHide=false, hideSnap=false, genie=false, dlVertical=false, dlPreview=true, launchDesktop=false, loginItem=false; liquid::settings::Launchpad density; int ccBlur=60, dlCount=6; std::wstring settingsPath;
liquid::settings::Revision revision;
struct ShellTaskbarState { bool known=false, autoHide=false; int windows=0, visible=0; bool operator==(const ShellTaskbarState&) const = default; } taskbarState;
BOOL CALLBACK CountShellTaskbar(HWND window,LPARAM value){
 auto* data=reinterpret_cast<std::pair<DWORD,ShellTaskbarState*>*>(value);DWORD owner=0;GetWindowThreadProcessId(window,&owner);if(owner!=data->first)return TRUE;
 wchar_t name[64]{};if(!GetClassNameW(window,name,64))return TRUE;
 if(wcscmp(name,L"Shell_TrayWnd")&&wcscmp(name,L"Shell_SecondaryTrayWnd"))return TRUE;
 ++data->second->windows;if(IsWindowVisible(window))++data->second->visible;return TRUE;
}
bool RefreshTaskbarState(){
 ShellTaskbarState next;DWORD shellPid=0;if(HWND shell=GetShellWindow())GetWindowThreadProcessId(shell,&shellPid);
 if(shellPid){std::pair<DWORD,ShellTaskbarState*> data{shellPid,&next};const bool enumerated=EnumWindows(CountShellTaskbar,reinterpret_cast<LPARAM>(&data))!=FALSE;
  if(enumerated&&next.windows){APPBARDATA appbar{};appbar.cbSize=sizeof(appbar);next.autoHide=(SHAppBarMessage(ABM_GETSTATE,&appbar)&ABS_AUTOHIDE)!=0;next.known=true;}}
 const bool changed=!(taskbarState==next);taskbarState=next;return changed;
}
HWND searchBox=nullptr; WNDPROC originalSearchProc=nullptr;
HBRUSH searchBrush=nullptr;HFONT searchFont=nullptr; COLORREF searchText=RGB(0,0,0),searchBackground=RGB(255,255,255);
COLORREF ColorRef(unsigned rgb){return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);}
void ApplyNativeAppearance(HWND h){
 const auto colors=SettingsPalette();const COLORREF bg=ColorRef(SearchSurface()),fg=ColorRef(colors.primary);
 HBRUSH next=CreateSolidBrush(bg);if(next){HBRUSH previous=searchBrush;searchBrush=next;searchBackground=bg;searchText=fg;if(previous)DeleteObject(previous);}
 const unsigned c=colors.background;const BOOL dark=(((c>>16)&255)*299+((c>>8)&255)*587+(c&255)*114)<128000;
 DwmSetWindowAttribute(h,20,&dark,sizeof(dark));
 if(searchBox)InvalidateRect(searchBox,nullptr,TRUE);
} std::vector<int> visibleCategories{0,1,2,3,4,5,6,7,8,9}; int sidebarFirst=0;
std::vector<int> categoryHistory{0};size_t categoryHistoryIndex=0;
void StopRecording(HWND h,const wchar_t* feedback);
std::map<std::string,std::wstring> settingsPackIcons;
std::wstring workshopRoot;std::vector<docky::workshop::Pack> packs;int packSelected=-1,packOffset=0;std::string activePack,activeSettingsPack;
liquid::settings::Revision workshopRevision,settingsWorkshopRevision;
bool SettingsOnly(const docky::workshop::Pack& p){return !p.icons.empty()&&std::all_of(p.icons.begin(),p.icons.end(),[](const auto& item){return item.first.rfind("settings.",0)==0;});}
void RefreshWorkshop(){std::wstring error;std::vector<docky::workshop::Pack> next;if(!docky::workshop::List(workshopRoot,next,error))next.clear();
 std::string previous=packSelected>=0&&packSelected<static_cast<int>(packs.size())?packs[packSelected].id:std::string{};packs=std::move(next);packSelected=-1;
 for(size_t i=0;i<packs.size();++i)if(packs[i].id==previous){packSelected=static_cast<int>(i);break;}if(packSelected<0&&!packs.empty())packSelected=0;
 packOffset=std::clamp(packOffset,0,std::max(0,static_cast<int>(packs.size())-3));docky::workshop::Pack active;bool enabled=false;
 activePack=docky::workshop::Active(workshopRoot,active,enabled,error)&&enabled?active.id:std::string{};settingsPackIcons=activePack.empty()?std::map<std::string,std::wstring>{}:active.icons;
 docky::workshop::Pack settings;bool settingsEnabled=false;activeSettingsPack=docky::workshop::ActiveSettings(workshopRoot,settings,settingsEnabled,error)&&settingsEnabled?settings.id:std::string{};
 if(!activeSettingsPack.empty())for(const auto& item:settings.icons)if(item.first.rfind("settings.",0)==0)settingsPackIcons[item.first]=item.second;
 workshopRevision=liquid::settings::ReadRevision(workshopRoot+L"\\active.txt");settingsWorkshopRevision=liquid::settings::ReadRevision(workshopRoot+L"\\settings-active.txt");}
std::wstring PickPackFile(HWND owner,bool save){ComPtr<IFileDialog> dialog;HRESULT hr=save?CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&dialog)):CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&dialog));
 if(FAILED(hr))return {};COMDLG_FILTERSPEC filter[]{ {L"DockOver resource packages (*.dockypack)",L"*.dockypack"} };dialog->SetFileTypes(1,filter);dialog->SetDefaultExtension(L"dockypack");FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);
 dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR|(save?0:FOS_FILEMUSTEXIST));if(FAILED(dialog->Show(owner)))return {};ComPtr<IShellItem> item;PWSTR path=nullptr;std::wstring result;
 if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))&&path)result=path;if(path)CoTaskMemFree(path);return result;}
std::wstring PickLegacyFile(HWND owner){ComPtr<IFileOpenDialog> dialog;
 if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&dialog))))return {};
 FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR|FOS_FILEMUSTEXIST);
 dialog->SetTitle(L"Choose a configuration file to back up");if(FAILED(dialog->Show(owner)))return {};
 ComPtr<IShellItem> item;PWSTR path=nullptr;std::wstring result;
 if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))&&path)result=path;
 if(path)CoTaskMemFree(path);return result;
}
std::map<std::wstring,ComPtr<ID2D1Bitmap>> previewCache;
ID2D1RenderTarget* previewCacheTarget=nullptr;
bool DrawWorkshopPreview(const std::wstring& path,D2D1_RECT_F box){
 if(path.empty()||!target)return false;
 if(target!=previewCacheTarget){previewCache.clear();previewCacheTarget=target;}
 auto found=previewCache.find(path);
 if(found==previewCache.end()){
  ComPtr<IWICImagingFactory> f;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> conv;ComPtr<ID2D1Bitmap> image;
  HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f));if(SUCCEEDED(hr))hr=f->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
  if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);if(SUCCEEDED(hr))hr=f->CreateFormatConverter(&conv);if(SUCCEEDED(hr))hr=conv->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
  if(SUCCEEDED(hr))hr=target->CreateBitmapFromWicBitmap(conv.Get(),nullptr,&image);if(FAILED(hr)||!image)return false;
  found=previewCache.emplace(path,std::move(image)).first;
 }
 const auto size=found->second->GetSize();if(size.width<=0||size.height<=0)return false;
 const float factor=std::min((box.right-box.left)/size.width,(box.bottom-box.top)/size.height);const float w=size.width*factor,h=size.height*factor;
 const float x=box.left+((box.right-box.left)-w)*.5f,y=box.top+((box.bottom-box.top)-h)*.5f;
 target->DrawBitmap(found->second.Get(),D2D1::RectF(x,y,x+w,y+h),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);return true;
}

RECT ClientScreen(HWND h){RECT r{};GetClientRect(h,&r);POINT p{0,0};ClientToScreen(h,&p);OffsetRect(&r,p.x,p.y);return r;}
RECT SidebarScreen(HWND h){RECT r=ClientScreen(h);r.right=(std::min)(r.right,r.left+static_cast<LONG>(liquid::design::settings::SidebarWidth*scale));return r;}

float ContentLeftForWidth(float width){namespace lay=liquid::design::settings;return lay::ContentLeft+(std::max)(0.f,(width-lay::ContentRight-lay::ContentLeft-900.f)*.5f);}
float ContentRightForWidth(float width,float left){return (std::min)(width-liquid::design::settings::ContentRight,left+900.f);}
D2D1_COLOR_F C(unsigned rgb,float a=1.f){return D2D1::ColorF(((rgb>>16)&255)/255.f,((rgb>>8)&255)/255.f,(rgb&255)/255.f,a);}
void Fill(D2D1_RECT_F r,unsigned color){brush->SetColor(C(color));target->FillRectangle(r,brush.Get());}
void Round(D2D1_RECT_F r,float rad,unsigned color){brush->SetColor(C(color));target->FillRoundedRectangle(D2D1::RoundedRect(r,rad,rad),brush.Get());}
// Compact switch: the active thumb samples this opaque window, not the desktop.
void DrawSettingsSwitch(HWND hwnd,int index,D2D1_RECT_F hit,bool on,bool pressed,bool focused,const liquid::design::Palette& colors){
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 BOOL motion=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0);
 const float cx=(hit.left+hit.right)*.5f,cy=(hit.top+hit.bottom)*.5f,w=46.4f,h=20.f;
 const auto track=D2D1::RectF(cx-w*.5f,cy-h*.5f,cx+w*.5f,cy+h*.5f);
 Round(track,h*.5f,contrast?(on?colors.selected:colors.background):(on?0x007AFF:liquid::theme::IsDark(appearanceMode,systemDark)?0x3A3B40:0xC8C9CE));
 if(focused||contrast){brush->SetColor(C(focused?colors.focused:colors.primary));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left-2,track.top-2,track.right+2,track.bottom+2),12,12),brush.Get(),focused?2.f:1.f);}
 const ULONGLONG now=GetTickCount64();auto& anim=switchMotion[index];
 if(!anim.initialized){anim.initialized=true;anim.on=on;anim.from=anim.to=on?1.f:0.f;anim.start=now;}
 const auto positionAt=[&](ULONGLONG t){const float u=std::clamp((t-anim.start)/420.f,0.f,1.f);return std::clamp(anim.from+(anim.to-anim.from)*EaseOutBack(u),0.f,1.f);};
 if(anim.on!=on){anim.from=positionAt(now);anim.to=on?1.f:0.f;anim.on=on;anim.start=now;}
 float pos=motion?positionAt(now):(on?1.f:0.f);if(pressed&&switchDragging)pos=switchDragPosition;
 const float press=visualSwitch==index?(motion?PressAmount(now):(pressed?1.f:0.f)):0.f;
 if(motion&&((now-anim.start<420&&anim.from!=anim.to)||(visualSwitch==index&&now-pressStart<150&&pressFrom!=pressTo)))switchTicker.Start(hwnd,docky::vblank::kMsgVBlank);  // next frame at the display rate (a 4 ms WM_TIMER ran at ~64-100 Hz)
 const float knobCenter=track.left+16.f+pos*(w-32.f);
 const float expanded=(motion&&!contrast)?press:0.f;
 const dock::GlassParameters lens=dock::ControlLensGlass(17.6f);  // Glass Studio "Sliders & switches"
 const float kw=29.6f*(1.f+(lens.switchPressWidth-1.f)*expanded),kh=17.6f*(1.f+(lens.switchPressHeight-1.f)*expanded);
 const auto knob=D2D1::RectF(knobCenter-kw*.5f,cy-kh*.5f,knobCenter+kw*.5f,cy+kh*.5f);
 if(press>.001f&&!contrast&&allowTransparency&&drawingLocalGlass){
  drawingLocalGlass->active=true;drawingLocalGlass->plate=knob;drawingLocalGlass->sampleBounds=track;drawingLocalGlass->params=dock::ControlLensGlass(kh);
  drawingLocalGlass->params.dark=liquid::theme::IsDark(appearanceMode,systemDark);drawingLocalGlass->controlScale=1.f;
  drawingLocalGlass->pointerX=switchPointerX*scale;drawingLocalGlass->pointerY=switchPointerY*scale;localGlassWhite=1.f-press;
  return;
 }
 brush->SetColor(C(contrast?colors.primary:0xFFFFFF,pressed&&!contrast?.82f:1.f));target->FillRoundedRectangle(D2D1::RoundedRect(knob,kh*.5f,kh*.5f),brush.Get());
 if(!contrast){brush->SetColor(C(0x000000,.12f));target->DrawRoundedRectangle(D2D1::RoundedRect(knob,kh*.5f,kh*.5f),brush.Get(),.7f);}
}
void Text(const wchar_t* text,IDWriteTextFormat* fmt,D2D1_RECT_F r,unsigned color){brush->SetColor(C(color));target->DrawTextW(text,static_cast<UINT32>(wcslen(text)),fmt,r,brush.Get());}
int SidebarCapacity(HWND h){RECT rc{};GetClientRect(h,&rc);return std::max(1,static_cast<int>((rc.bottom/scale-liquid::design::settings::SidebarTop-8)/liquid::design::settings::SidebarStep));}
void SelectCategory(HWND h,int next,bool record=true){
 if(next<0||next>=static_cast<int>(std::size(titles))||next==selected)return;
 if(selected==6)StopRecording(h,nullptr);
 if(record){categoryHistory.erase(categoryHistory.begin()+categoryHistoryIndex+1,categoryHistory.end());categoryHistory.push_back(next);if(categoryHistory.size()>64)categoryHistory.erase(categoryHistory.begin());categoryHistoryIndex=categoryHistory.size()-1;}
 selected=next;if(selected==7)RefreshWorkshop();InvalidateRect(h,nullptr,FALSE);
}
void StepHistory(HWND h,int delta){
 const int next=static_cast<int>(categoryHistoryIndex)+delta;if(next<0||next>=static_cast<int>(categoryHistory.size()))return;
 if(searchBox&&GetWindowTextLengthW(searchBox)>0)SetWindowTextW(searchBox,L"");
 categoryHistoryIndex=static_cast<size_t>(next);SelectCategory(h,categoryHistory[categoryHistoryIndex],false);
 auto it=std::find(visibleCategories.begin(),visibleCategories.end(),selected);if(it!=visibleCategories.end()){const int index=static_cast<int>(it-visibleCategories.begin()),capacity=SidebarCapacity(h);if(index<sidebarFirst)sidebarFirst=index;else if(index>=sidebarFirst+capacity)sidebarFirst=index-capacity+1;}
 InvalidateRect(h,nullptr,FALSE);
}
void UpdateSearch(HWND h){
 wchar_t query[128]{}; if(searchBox)GetWindowTextW(searchBox,query,128);
 visibleCategories=liquid::settings::SearchCategories(query); sidebarFirst=0;
 if(!visibleCategories.empty() && std::find(visibleCategories.begin(),visibleCategories.end(),selected)==visibleCategories.end()){
  SelectCategory(h,visibleCategories.front());
 }
 const auto it=std::find(visibleCategories.begin(),visibleCategories.end(),selected);
 if(it!=visibleCategories.end())sidebarFirst=std::max(0,static_cast<int>(it-visibleCategories.begin())-SidebarCapacity(h)+1);
 InvalidateRect(h,nullptr,FALSE);
}
void MoveCategory(HWND h,int delta){
 if(visibleCategories.empty())return;
 auto it=std::find(visibleCategories.begin(),visibleCategories.end(),selected);
 int index=it==visibleCategories.end()?0:static_cast<int>(it-visibleCategories.begin());
 index=(index+delta+static_cast<int>(visibleCategories.size()))%static_cast<int>(visibleCategories.size());
 SelectCategory(h,visibleCategories[index]);const int capacity=SidebarCapacity(h);if(index<sidebarFirst)sidebarFirst=index;else if(index>=sidebarFirst+capacity)sidebarFirst=index-capacity+1;InvalidateRect(h,nullptr,FALSE);
}
LRESULT CALLBACK SearchProc(HWND h,UINT message,WPARAM w,LPARAM l){
 if(message==WM_PAINT && GetWindowTextLengthW(h)==0 && GetFocus()!=h){
  const LRESULT result=CallWindowProcW(originalSearchProc,h,message,w,l);
  HDC dc=GetDC(h);if(dc){const int old=SetBkMode(dc,TRANSPARENT);const COLORREF color=SetTextColor(dc,searchText);
   HFONT font=reinterpret_cast<HFONT>(SendMessageW(h,WM_GETFONT,0,0));HGDIOBJ prior=font?SelectObject(dc,font):nullptr;
   RECT r{};GetClientRect(h,&r);DrawTextW(dc,L"Search",-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
   if(prior)SelectObject(dc,prior);SetTextColor(dc,color);SetBkMode(dc,old);ReleaseDC(h,dc);}return result;
 }
 if(message==WM_SYSKEYDOWN&&(w==VK_LEFT||w==VK_RIGHT)){StepHistory(GetParent(h),w==VK_LEFT?-1:1);return 0;}
 if(message==WM_KEYDOWN){
  if(w==VK_DOWN||w==VK_UP){MoveCategory(GetParent(h),w==VK_DOWN?1:-1);return 0;}
  if(w==VK_ESCAPE){SetWindowTextW(h,L"");return 0;}
  if(w==VK_TAB){SetFocus(GetParent(h));return 0;}
 }
 return CallWindowProcW(originalSearchProc,h,message,w,l);
}
void UpdateSearchFont(){if(!searchBox)return;HFONT next=CreateFontW(-static_cast<int>(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,liquid::design::type::FamilyGdi);if(next){SendMessageW(searchBox,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);HFONT old=searchFont;searchFont=next;if(old)DeleteObject(old);}}
void PositionSearch(){if(searchBox)SetWindowPos(searchBox,nullptr,static_cast<int>(45*scale),static_cast<int>(80*scale),static_cast<int>((liquid::design::settings::SidebarWidth-69)*scale),static_cast<int>(20*scale),SWP_NOZORDER|SWP_NOACTIVATE);}
// Approximation from the supplied reference, in DIPs; Apple does not publish a
// macOS window radius or its private continuous-corner path for Windows.
void UpdateWindowContour(HWND h, UINT sizeKind){
 // Corners come from DWM's own rounding (antialiased, with the system shadow): a window region can only cut whole pixels,
 // which left a visible staircase with grey pixels around the curve (reported twice, measured over white 2026-11-10).
 // The D2D content mask uses the same 8 DIP radius, so content and frame agree.
 if(sizeKind==SIZE_MINIMIZED)return;
 if(gpuReady&&renderer)renderer->SetRoundedVisualClip(0.f);
 SetWindowRgn(h,nullptr,TRUE);
}
// A subtle antialiased inner outline follows the same continuous contour as
// the window region. Geometry is cached across redraws and recreated on resize.
ComPtr<ID2D1PathGeometry> windowOutline;ComPtr<ID2D1Factory> outlineFactory;float outlineWidth=-1,outlineHeight=-1;
void DrawWindowOutline(HWND h,bool draw=true){
 if(!target||!brush||IsZoomed(h))return;
 ComPtr<ID2D1Factory> currentFactory;target->GetFactory(&currentFactory);if(!currentFactory)return;
 if(currentFactory.Get()!=outlineFactory.Get()){windowOutline.Reset();outlineFactory=currentFactory;}
 RECT rc{};if(!GetClientRect(h,&rc)||rc.right<=0||rc.bottom<=0)return;
 const float width=rc.right/scale,height=rc.bottom/scale;
 if(!windowOutline||width!=outlineWidth||height!=outlineHeight){
  ComPtr<ID2D1PathGeometry> geometry;ComPtr<ID2D1GeometrySink> sink;
  if(FAILED(currentFactory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return;
  const float inset=0.0f,left=inset,top=inset,right=width-inset,bottom=height-inset;
  const float r=(std::min)(8.f,(std::min)((right-left)*.5f,(bottom-top)*.5f));  // = DWM's rounded-corner radius
  constexpr int segments=20;constexpr float halfPi=1.57079632679f;
  sink->BeginFigure(D2D1::Point2F(left+r,top),D2D1_FIGURE_BEGIN_FILLED);
  for(int corner=0;corner<4;++corner){
   for(int i=0;i<=segments;++i){const float t=halfPi*i/segments;
    const float a=r*(1.f-std::pow((std::max)(0.f,std::sin(t)),2.f/3.f)),b=r*(1.f-std::pow((std::max)(0.f,std::cos(t)),2.f/3.f));
    D2D1_POINT_2F point{};
    if(corner==0)point=D2D1::Point2F(right-a,top+b);
    else if(corner==1)point=D2D1::Point2F(right-b,bottom-a);
    else if(corner==2)point=D2D1::Point2F(left+a,bottom-b);
    else point=D2D1::Point2F(left+b,top+a);
    sink->AddLine(point);
   }
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);if(FAILED(sink->Close()))return;
  windowOutline=geometry;outlineWidth=width;outlineHeight=height;
 }
 if(!draw)return;
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 const bool dark=liquid::theme::IsDark(appearanceMode,systemDark);
 brush->SetColor(C(contrast?SettingsPalette().primary:dark?0xFFFFFF:0x000000,contrast?1.f:dark?.11f:.12f));
 target->DrawGeometry(windowOutline.Get(),brush.Get(),1.f);
}
// Original, simple glyphs drawn locally; these are not Apple or SF Symbols assets.
void DrawCategoryGlyph(int kind,D2D1_RECT_F box,unsigned color){
 const float x=box.left,y=box.top;brush->SetColor(C(color));
 auto line=[&](float x1,float y1,float x2,float y2){target->DrawLine(D2D1::Point2F(x+x1,y+y1),D2D1::Point2F(x+x2,y+y2),brush.Get(),1.5f);};
 auto rect=[&](float l,float t,float r,float b,float rad){target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x+l,y+t,x+r,y+b),rad,rad),brush.Get(),1.5f);};
 auto circle=[&](float cx,float cy,float rad){target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x+cx,y+cy),rad,rad),brush.Get(),1.5f);};
 switch(kind){
 case 0: circle(12,12,6);circle(12,12,2);for(int k=0;k<8;++k){float a=k*3.14159265f/4;line(12+7*std::cos(a),12+7*std::sin(a),12+9*std::cos(a),12+9*std::sin(a));}break;
 case 1: circle(12,12,7);line(12,5,12,19);target->FillRectangle(D2D1::RectF(x+12,y+6,x+18,y+18),brush.Get());break;
 case 2: rect(4,7,20,17,2);line(6,14,18,14);break;
 case 3: rect(4.5f,5.f,19.5f,11.f,3.f);rect(4.5f,13.f,19.5f,19.f,3.f);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+8.f,y+8.f),1.8f,1.8f),brush.Get());target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+16.f,y+16.f),1.8f,1.8f),brush.Get());break;
 case 4: for(int a=0;a<2;++a)for(int b=0;b<2;++b)rect(5+a*8,5+b*8,10+a*8,10+b*8,1);break;
 case 5: rect(5,6,19,11,2.5f);rect(5,13,19,18,2.5f);circle(9,8.5f,1);circle(15,15.5f,1);break;
 case 6: rect(4,6,20,18,1);line(5,15,19,15);line(12,8,12,13);break;
 case 7: rect(5,6,19,19,2);line(9,6,9,4);line(15,6,15,4);line(9,4,15,4);break;
 case 8: line(5,9,18,9);line(15,6,18,9);line(15,12,18,9);line(19,15,6,15);line(9,12,6,15);line(9,18,6,15);break;
 default: circle(12,12,7);line(12,11,12,16);line(12,7.5f,12,8.5f);break;
 }
}
int ChromeButtonAt(float x,float y){if(y<11.f||y>39.f)return -1;for(int i=0;i<3;++i)if(std::fabs(x-(25.f+23.f*i))<=11.5f)return i;return -1;}
// ---- Windows Taskbar page: every control drives the Desktop Manager / guardian (no placeholder state) ----
liquid::settings::Taskbar taskbarPref;
docky::taskbar::Status tbStatus;bool tbLive=false;bool recordingHotkey=false;int taskbarKeyboardControl=0;std::wstring taskbarFeedback;
bool RefreshTaskbarStatus(){docky::taskbar::Status next;const bool read=docky::taskbar::ReadStatus(docky::taskbar::DefaultStatusPath(),next);const bool live=read&&docky::taskbar::StatusIsLive(next);
 const bool changed=live!=tbLive||next.state!=tbStatus.state||next.guardian!=tbStatus.guardian||next.hotkey!=tbStatus.hotkey||next.hotkeyText!=tbStatus.hotkeyText||next.message!=tbStatus.message;
 tbStatus=next;tbLive=live;return changed;}
bool LoginItemEnabled();
void Refresh(){const auto r=liquid::settings::Load(settingsPath);diagnostics=r.value.dock.diagnostics;iconMask=r.value.dock.useMacIconMask;autoHide=r.value.dock.autoHide;hideSnap=r.value.dock.hideSnapLayouts;genie=r.value.dock.genieMinimize;dlCount=r.value.dock.downloadsCount;dlVertical=r.value.dock.downloadsVertical;dlPreview=r.value.dock.downloadsPreview;launchDesktop=r.value.dock.launchDesktop;loginItem=LoginItemEnabled();density=r.value.launchpad;ccBlur=r.value.controlCenter.backdropBlur;perf=r.value.performance;updatePref=r.value.updates;appearanceMode=r.value.appearance.mode;taskbarPref=r.value.taskbar;revision=liquid::settings::ReadRevision(settingsPath);}
void OpenWindowsTaskbarSettings(HWND h){
 const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",L"ms-settings:taskbar",nullptr,nullptr,SW_SHOWNORMAL));
 if(result<=32)MessageBoxW(h,L"Windows Taskbar Settings could not be opened. Open Personalization > Taskbar in Windows Settings.",L"DockOver Settings",MB_OK|MB_ICONWARNING);
}
void OpenVisualEffects(HWND h){
 const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",L"ms-settings:easeofaccess-visualeffects",nullptr,nullptr,SW_SHOWNORMAL));
 if(result<=32)MessageBoxW(h,L"Windows Settings could not be opened. Open Accessibility > Visual effects in Windows Settings.",L"DockOver Settings",MB_OK|MB_ICONWARNING);
}
void ToggleDockSetting(HWND h,int row){
 if(row<0||row>=5)return;const char* keys[]={"useMacIconMask","autoHide","diagnostics","hideSnapLayouts","genieMinimize"};const bool now[]={iconMask,autoHide,diagnostics,hideSnap,genie};
 const auto status=liquid::settings::WriteBool(settingsPath,"dock",keys[row],!now[row]);
 if(status!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetAppearance(HWND h,int mode){
 if(liquid::settings::WriteAppearanceMode(settingsPath,mode)!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save appearance. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);
}
void SetDownloadsCount(HWND h,int value){
 if(liquid::settings::WriteDockInt(settingsPath,"downloadsCount",std::clamp(value,3,10))!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetCcBlur(HWND h,int value){
 if(liquid::settings::WriteControlCenterInt(settingsPath,"backdropBlur",std::clamp(value,0,100))!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetDensity(HWND h,const char* key,int value){
 const auto status=liquid::settings::WriteDensityInt(settingsPath,key,value);
 if(status!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void ActivateDensity(HWND h,int control){
 const int modes[]={1,0,2,3};
 if(control>=0&&control<4){SetDensity(h,"densityPreset",modes[control]);return;}
 if(density.densityPreset!=3||control<4||control>=10)return;
 const int row=(control-4)/2,delta=((control-4)%2)?8:-8;
 const char* keys[]={"iconSize","gapX","gapY"};const int values[]={density.iconSize,density.gapX,density.gapY};
 SetDensity(h,keys[row],std::clamp(values[row]+delta,row==0?32:0,row==0?160:240));
}
bool SignalManager(const wchar_t* name){HANDLE e=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);if(!e)return false;const bool ok=SetEvent(e)!=FALSE;CloseHandle(e);return ok;}
void TaskbarToggle(HWND h){
 if(!tbLive){taskbarFeedback=L"DockOver is not running, so it cannot hide or show the taskbar.";InvalidateRect(h,nullptr,FALSE);return;}
 const bool hidden=tbStatus.state==L"hidden";
 if(!hidden&&tbStatus.guardian!=L"ready"){taskbarFeedback=L"Hiding is unavailable until the recovery guardian is ready.";InvalidateRect(h,nullptr,FALSE);return;}
 taskbarFeedback=SignalManager(hidden?docky::taskbar::kShowEvent:docky::taskbar::kHideEvent)?(hidden?L"Showing the Windows taskbar…":L"Hiding the Windows taskbar…"):L"DockOver did not accept the request.";
 SetTimer(h,2,400,nullptr);InvalidateRect(h,nullptr,FALSE);
}
void TaskbarRestoreNow(HWND h){
 if(tbLive){  // ask the independent guardian; it works even if the Manager is unresponsive
  taskbarFeedback=SignalManager(docky::taskbar::kRestoreNowEvent)?L"Restore requested from the recovery guardian.":L"The recovery guardian is not running; DockOver has not hidden the taskbar.";
  SetTimer(h,2,600,nullptr);InvalidateRect(h,nullptr,FALSE);return;}
 // docky is not running: run the standalone recovery tool from this build.
 wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);std::wstring exe(self);exe=exe.substr(0,exe.find_last_of(L"\\/")+1)+L"docky-taskbar-restore.exe";
 std::wstring cmd=L"\""+exe+L"\"";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};DWORD code=99;
 if(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
  if(WaitForSingleObject(pi.hProcess,8000)==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}
 taskbarFeedback=code==0?L"Recovery finished: nothing DockOver owned is hidden.":code==99?L"The recovery tool could not be run.":L"Recovery stopped: an ambiguous record was preserved. See docky-taskbar-restore for details.";
 RefreshTaskbarState();InvalidateRect(h,nullptr,FALSE);
}
void SaveTaskbarBool(HWND h,const char* key,bool value){
 if(liquid::settings::WriteBool(settingsPath,"taskbar",key,value)!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
bool SaveHotkey(UINT mods,UINT vk){
 using namespace liquid::settings;
 return WriteTaskbarInt(settingsPath,"hotkeyModifiers",static_cast<int>(mods))==SaveStatus::Saved&&WriteTaskbarInt(settingsPath,"hotkeyKey",static_cast<int>(vk))==SaveStatus::Saved&&
        WriteBool(settingsPath,"taskbar","hotkeyEnabled",vk!=0)==SaveStatus::Saved;
}
// Recording owns the lease: while it is held the Manager has unregistered the global shortcut, so pressing the
// current shortcut is recorded instead of toggling the taskbar. Every exit path goes through StopRecording.
docky::taskbar::RecordingLease recordingLease;
constexpr UINT_PTR kRecordTimeout=3;
void StopRecording(HWND h,const wchar_t* feedback){
 if(!recordingHotkey&&!recordingLease.Held())return;
 recordingHotkey=false;KillTimer(h,kRecordTimeout);recordingLease.Release();  // after saving: the Manager reloads on release
 if(feedback)taskbarFeedback=feedback;InvalidateRect(h,nullptr,FALSE);
}
void StartRecording(HWND h){
 if(recordingHotkey)return;
 bool acked=false;std::wstring why;
 if(!recordingLease.Acquire(600,acked,why)){taskbarFeedback=why;InvalidateRect(h,nullptr,FALSE);return;}
 recordingHotkey=true;SetTimer(h,kRecordTimeout,20000,nullptr);
 taskbarFeedback=tbLive&&!acked?L"Press the new shortcut (DockOver did not confirm the pause yet). Esc cancels, Backspace clears.":L"Press the new shortcut. Esc cancels, Backspace clears.";
 SetFocus(h);InvalidateRect(h,nullptr,FALSE);
}
void ClearHotkey(HWND h){const bool ok=SaveHotkey(static_cast<UINT>(taskbarPref.hotkeyModifiers),0);Refresh();
 StopRecording(h,ok?L"Shortcut cleared.":L"Could not save settings. Your previous settings were preserved.");}
// Recording: the next non-modifier key with its held modifiers. Checked against reserved combinations and, with a
// trial RegisterHotKey, against shortcuts other apps already own. No keyboard hook is used.
void RecordHotkeyKey(HWND h,UINT vk){
 using namespace docky::hotkey;
 UINT mods=0;if(GetKeyState(VK_CONTROL)&0x8000)mods|=MOD_CONTROL;if(GetKeyState(VK_MENU)&0x8000)mods|=MOD_ALT;if(GetKeyState(VK_SHIFT)&0x8000)mods|=MOD_SHIFT;
 if((GetKeyState(VK_LWIN)&0x8000)||(GetKeyState(VK_RWIN)&0x8000))mods|=MOD_WIN;
 if(vk==VK_ESCAPE&&!mods){StopRecording(h,L"Recording cancelled; the previous shortcut is kept.");return;}
 if((vk==VK_BACK||vk==VK_DELETE)&&!mods){ClearHotkey(h);return;}
 if(IsModifierKey(vk)){InvalidateRect(h,nullptr,FALSE);return;}
 const auto validity=Validate(mods,vk);
 if(validity!=Validity::Ok){taskbarFeedback=docky::hotkey::Text(mods,vk)+L": "+ValidityText(validity)+L" Still recording.";InvalidateRect(h,nullptr,FALSE);return;}
 // The Manager's own registration is paused, so a failure here means another program owns the combination.
 if(!RegisterHotKey(h,0x7001,mods|MOD_NOREPEAT,vk)){taskbarFeedback=docky::hotkey::Text(mods,vk)+L" is already used by another app. Still recording.";InvalidateRect(h,nullptr,FALSE);return;}
 UnregisterHotKey(h,0x7001);
 const bool saved=SaveHotkey(mods,vk);Refresh();
 StopRecording(h,saved?(L"Shortcut set to "+docky::hotkey::Text(mods,vk)+L".").c_str():L"Could not save settings. The previous shortcut is kept.");
 SetTimer(h,2,1500,nullptr);
}
void ActivateTaskbarControl(HWND h,int control){
 switch(control){
 case 0:TaskbarToggle(h);break;
 case 1:TaskbarRestoreNow(h);break;
 case 2:OpenWindowsTaskbarSettings(h);break;
 case 3:SaveTaskbarBool(h,"hideOnStartup",!taskbarPref.hideOnStartup);break;
 case 4:  // explicit opt-in: changes the Windows work area (not persisted) while docky hides the taskbar
  if(!taskbarPref.freeWorkArea&&MessageBoxW(h,L"While DockOver hides the Windows taskbar, maximized windows will also use the strip the taskbar normally reserves.\n\n"
       L"DockOver changes the work area only temporarily (it is not saved to your Windows profile) and restores it when the taskbar is shown, "
       L"when DockOver exits, or through its recovery guardian. Changes made by Windows or other programs are never overwritten.\n\nEnable?",
       L"DockOver Settings",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK)break;
  SaveTaskbarBool(h,"freeWorkArea",!taskbarPref.freeWorkArea);break;
 case 5:if(!taskbarPref.hotkeyKey)StartRecording(h);
        else SaveTaskbarBool(h,"hotkeyEnabled",!taskbarPref.hotkeyEnabled);break;
 case 6:if(recordingHotkey)StopRecording(h,L"Recording cancelled; the previous shortcut is kept.");else StartRecording(h);break;
 case 7:ClearHotkey(h);break;
 default:break;}
}
// Hit rectangles (DIP) of the 8 Windows Taskbar controls, shared by drawing, mouse and keyboard focus.
constexpr int kTaskbarControls=8;
D2D1_RECT_F TaskbarControlRect(int control,float left,float right){
 namespace lay=liquid::design::settings;const float top=lay::CardTop;
 switch(control){
 case 0:return D2D1::RectF(left,top+128,left+150,top+160);
 case 1:return D2D1::RectF(left+158,top+128,left+366,top+160);
 case 2:return D2D1::RectF(left+374,top+128,std::min(right,left+590),top+160);
 case 3:case 4:case 5:{const float rowTop=top+176+(control-3)*56.f,sr=right-lay::SwitchRight,sl=sr-lay::SwitchWidth,st=rowTop+(56-lay::SwitchHeight)/2;return D2D1::RectF(sl,st,sr,st+lay::SwitchHeight);}
 case 6:return D2D1::RectF(right-196,top+344+12,right-100,top+344+44);
 case 7:return D2D1::RectF(right-92,top+344+12,right-16,top+344+44);
 }return D2D1::RectF(0,0,0,0);
}
bool CreateResources(HWND h){
 if(!fallbackTarget){RECT rc{};GetClientRect(h,&rc);
  if(FAILED(factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(h,D2D1::SizeU(rc.right,rc.bottom)),&fallbackTarget)))return false;
  fallbackTarget->SetDpi(96*scale,96*scale);}
 target=fallbackTarget.Get();
 if(!brush && FAILED(target->CreateSolidColorBrush(C(0xffffff),&brush))){fallbackTarget.Reset();target=nullptr;return false;}return true;
}
bool SettingsSwitchValue(int index){switch(index){case 0:return iconMask;case 1:return autoHide;case 2:return diagnostics;
 case 3:return taskbarPref.hideOnStartup;case 4:return taskbarPref.freeWorkArea;case 5:return taskbarPref.hotkeyEnabled&&taskbarPref.hotkeyKey!=0;case 8:return hideSnap;case 9:return density.hideLabels;case 10:return genie;case 11:return dlVertical;case 12:return dlPreview;case 13:return density.showWindows;case 14:return updatePref.autoCheck;case 15:return updatePref.autoInstall;case 16:return launchDesktop;case 17:return loginItem;default:return false;}}
// General page: "Start DockOver Desktop with DockOver" (index 16), in its own card below the frame-rate card.
float DesktopLaunchCardTop(){return liquid::design::settings::CardTop+350.f;}
D2D1_RECT_F DesktopLaunchSwitch(float right){namespace lay=liquid::design::settings;const float top=DesktopLaunchCardTop()+(lay::CardHeight-lay::SwitchHeight)*.5f;
 return D2D1::RectF(right-lay::SwitchRight-lay::SwitchWidth,top,right-lay::SwitchRight,top+lay::SwitchHeight);}
// General page: "Open DockOver at login" (index 17), below the Desktop card.
float LoginItemCardTop(){return DesktopLaunchCardTop()+liquid::design::settings::CardHeight+12.f;}
D2D1_RECT_F LoginItemSwitch(float right){namespace lay=liquid::design::settings;const float top=LoginItemCardTop()+(lay::CardHeight-lay::SwitchHeight)*.5f;
 return D2D1::RectF(right-lay::SwitchRight-lay::SwitchWidth,top,right-lay::SwitchRight,top+lay::SwitchHeight);}
// Login item: the per-user Run key (HKCU, no administrator rights), the same entry Task Manager > Startup apps lists.
// The state is read from Windows every time, not stored in settings.json, so turning it off in Task Manager shows here.
constexpr wchar_t kRunKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kLoginValue[]=L"DockOver";
// DockOver.exe starts the Dock and Menu Bar. Release folders: beside runtime\ (this program's folder); build folders: here.
std::wstring DockOverLauncher(){
 std::wstring self(32768,L'\0');const DWORD n=GetModuleFileNameW(nullptr,self.data(),static_cast<DWORD>(self.size()));
 if(!n||n>=self.size())return {};self.resize(n);const std::wstring dir=self.substr(0,self.find_last_of(L"\\/"));
 for(const std::wstring& candidate:{dir+L"\\..\\DockOver.exe",dir+L"\\DockOver.exe"}){
  std::wstring full(32768,L'\0');const DWORD m=GetFullPathNameW(candidate.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
  if(m&&m<full.size()){full.resize(m);if(GetFileAttributesW(full.c_str())!=INVALID_FILE_ATTRIBUTES)return full;}}
 return {};
}
bool LoginItemEnabled(){
 wchar_t data[2048]{};DWORD size=sizeof(data);
 if(RegGetValueW(HKEY_CURRENT_USER,kRunKey,kLoginValue,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,data,&size)!=ERROR_SUCCESS)return false;
 BYTE approved[12]{};DWORD approvedSize=sizeof(approved);  // Task Manager "Disabled": first byte odd (03); enabled: 02
 if(RegGetValueW(HKEY_CURRENT_USER,kStartupApprovedKey,kLoginValue,RRF_RT_REG_BINARY,nullptr,approved,&approvedSize)==ERROR_SUCCESS&&approvedSize>0&&(approved[0]&1))return false;
 return true;
}
bool SetLoginItem(bool on,std::wstring& error){
 if(!on){const LSTATUS s=RegDeleteKeyValueW(HKEY_CURRENT_USER,kRunKey,kLoginValue);
  if(s!=ERROR_SUCCESS&&s!=ERROR_FILE_NOT_FOUND){error=L"Windows did not allow removing the login item (error "+std::to_wstring(s)+L").";return false;}
  DLOG(L"settings: login item removed");return true;}
 const std::wstring exe=DockOverLauncher();
 if(exe.empty()){error=L"DockOver.exe was not found next to DockOver Settings.";return false;}
 const std::wstring command=L"\""+exe+L"\"";
 const LSTATUS s=RegSetKeyValueW(HKEY_CURRENT_USER,kRunKey,kLoginValue,REG_SZ,command.c_str(),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
 if(s!=ERROR_SUCCESS){error=L"Windows did not allow adding the login item (error "+std::to_wstring(s)+L").";return false;}
 RegDeleteKeyValueW(HKEY_CURRENT_USER,kStartupApprovedKey,kLoginValue);  // undo an earlier "Disabled" in Task Manager
 DLOG(L"settings: login item set to %ls",command.c_str());return true;
}
// Dock page: the Downloads card below the five Dock rows (row 0: files shown, row 1: straight-up switch, index 11).
float DownloadsCardTop(){namespace lay=liquid::design::settings;return lay::CardTop+5*lay::CardHeight+16.f;}
D2D1_RECT_F DownloadsVerticalSwitch(float right,int row=1){namespace lay=liquid::design::settings;const float st=DownloadsCardTop()+row*lay::CardHeight+(lay::SwitchTop-lay::CardTop);
 return D2D1::RectF(right-lay::SwitchRight-lay::SwitchWidth,st,right-lay::SwitchRight,st+lay::SwitchHeight);}
// Launchpad page: the "Hide app names" switch (index 9), in its own card below the density card.
D2D1_RECT_F LaunchpadNamesSwitch(float right){namespace lay=liquid::design::settings;const float top=lay::CardTop+236.f+(lay::CardHeight-lay::SwitchHeight)*.5f;
 return D2D1::RectF(right-lay::SwitchRight-lay::SwitchWidth,top,right-lay::SwitchRight,top+lay::SwitchHeight);}
// Launchpad page: "Show windows behind Launchpad" (index 13), the card under "Hide app names".
D2D1_RECT_F LaunchpadWindowsSwitch(float right){namespace lay=liquid::design::settings;const auto r=LaunchpadNamesSwitch(right);const float d=lay::CardHeight+12.f;
 return D2D1::RectF(r.left,r.top+d,r.right,r.bottom+d);}
int SettingsSwitchAt(HWND h,float x,float y){
 namespace lay=liquid::design::settings;RECT rc{};GetClientRect(h,&rc);
 const float left=ContentLeftForWidth(rc.right/scale),right=ContentRightForWidth(rc.right/scale,left);
 if(selected==2){for(int i=0;i<5;++i){const float cy=lay::SwitchTop+i*lay::CardHeight+lay::SwitchHeight*.5f;
  if(x>=right-lay::SwitchRight-lay::SwitchWidth-8&&x<right-lay::SwitchRight+8&&y>=cy-20&&y<cy+20)return i==3?8:i==4?10:i;}
  const auto d=DownloadsVerticalSwitch(right);if(x>=d.left-8&&x<d.right+8&&y>=d.top-8&&y<d.bottom+8)return 11;
  const auto q=DownloadsVerticalSwitch(right,2);if(x>=q.left-8&&x<q.right+8&&y>=q.top-8&&y<q.bottom+8)return 12;}
 if(selected==0){const auto r=DesktopLaunchSwitch(right);if(x>=r.left-8&&x<r.right+8&&y>=r.top-8&&y<r.bottom+8)return 16;
  const auto g=LoginItemSwitch(right);if(x>=g.left-8&&x<g.right+8&&y>=g.top-8&&y<g.bottom+8)return 17;}
 if(selected==4){const auto r=LaunchpadNamesSwitch(right);if(x>=r.left-8&&x<r.right+8&&y>=r.top-8&&y<r.bottom+8)return 9;
  const auto v=LaunchpadWindowsSwitch(right);if(x>=v.left-8&&x<v.right+8&&y>=v.top-8&&y<v.bottom+8)return 13;}
 if(selected==9){for(int row=0;row<2;++row){const auto r=UpdateSwitch(right,row);if(x>=r.left-8&&x<r.right+8&&y>=r.top-8&&y<r.bottom+8)return 14+row;}}
 if(selected==6){for(int i=0;i<3;++i){const auto r=TaskbarControlRect(3+i,left,right);if(x>=r.left-4&&x<r.right+4&&y>=r.top-4&&y<r.bottom+4)return i+3;}}
 return -1;
}
// Soft top and bottom highlights along the traffic-light contour, matching
// the Launcher control's reference geometry without importing QML code.
void DrawChromeHighlight(float cx,float cy,float radius,bool top,float alpha){
 ComPtr<ID2D1Factory> f;target->GetFactory(&f);if(!f)return;
 ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
 if(FAILED(f->CreatePathGeometry(&path))||FAILED(path->Open(&sink)))return;
 constexpr float pi=3.14159265359f;const float a=top?1.12f*pi:.12f*pi,b=top?1.88f*pi:.88f*pi;
 const auto start=D2D1::Point2F(cx+radius*std::cos(a),cy+radius*std::sin(a));
 const auto end=D2D1::Point2F(cx+radius*std::cos(b),cy+radius*std::sin(b));
 sink->BeginFigure(start,D2D1_FIGURE_BEGIN_HOLLOW);sink->AddArc(D2D1::ArcSegment(end,D2D1::SizeF(radius,radius),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));sink->EndFigure(D2D1_FIGURE_END_OPEN);if(FAILED(sink->Close()))return;
 D2D1_GRADIENT_STOP stops[]={{0.f,C(0xFFFFFF,0.f)},{.5f,C(0xFFFFFF,alpha)},{1.f,C(0xFFFFFF,0.f)}};
 ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> gradient;
 if(SUCCEEDED(target->CreateGradientStopCollection(stops,3,&collection))&&SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx-radius,cy),D2D1::Point2F(cx+radius,cy)),collection.Get(),&gradient)))target->DrawGeometry(path.Get(),gradient.Get(),.9f);
}
void DrawContent(HWND h,bool glass){
 namespace ds=liquid::design; namespace sp=liquid::design::space; namespace lay=liquid::design::settings;
 RECT rc{};GetClientRect(h,&rc);float w=rc.right/scale,ht=rc.bottom/scale;
 const auto colors=SettingsPalette();
 const float left=ContentLeftForWidth(w),right=ContentRightForWidth(w,left);
 if(!glass){Fill(D2D1::RectF(0,0,w,ht),colors.background);Fill(D2D1::RectF(0,0,lay::SidebarWidth,ht),colors.navigation);}
 else {
  Fill(D2D1::RectF(lay::SidebarWidth,0,w,ht),colors.background);
  const auto nav=ds::Material(ds::MaterialRole::Navigation);
  brush->SetColor(C(colors.navigation,nav.tintOpacity));
  target->FillRectangle(D2D1::RectF(0,0,lay::SidebarWidth,ht),brush.Get());
  brush->SetColor(C(colors.surface,0.35f));target->DrawLine(D2D1::Point2F(lay::SidebarWidth-1,0),D2D1::Point2F(lay::SidebarWidth-1,ht),brush.Get(),1.f);
 }
 // Native window operations are represented by the three small reference-style controls.
 HIGHCONTRASTW chromeHc{sizeof(chromeHc)};const bool chromeContrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(chromeHc),&chromeHc,0)&&(chromeHc.dwFlags&HCF_HIGHCONTRASTON);
 const unsigned chromeColors[]={0xFF5F57,0xFEBC2E,0x28C840};
 const unsigned chromeGlyphs[]={0x4D0000,0x995700,0x006500};
 const bool chromeLit=GetForegroundWindow()==h||chromeHover>=0||chromePressed>=0;
 const bool showGlyphs=chromeHover>=0||chromePressed>=0||chromeContrast;
 const unsigned inactiveChrome=liquid::theme::IsDark(appearanceMode,systemDark)?0x4D4D50:0xD1D1D4;
 for(int i=0;i<3;++i){const float x=25.f+23.f*i,y=25.f,radius=7.f;const auto circle=D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius);
  brush->SetColor(C(chromeContrast?colors.background:chromeLit?chromeColors[i]:inactiveChrome));target->FillEllipse(circle,brush.Get());
  if(chromePressed==i&&!chromeContrast){brush->SetColor(C(0x000000,0.16f));target->FillEllipse(circle,brush.Get());}
  if(chromeContrast){brush->SetColor(C(colors.primary));target->DrawEllipse(circle,brush.Get(),1.f);}
  else {DrawChromeHighlight(x,y,6.25f,true,chromeLit?.75f:.4f);DrawChromeHighlight(x,y,6.25f,false,chromeLit?.4f:.22f);}
  if(showGlyphs){brush->SetColor(C(chromeContrast?colors.primary:chromeGlyphs[i],0.9f));const float arm=7.f*.486f;
   if(i==0){const float v=arm*.72f;target->DrawLine(D2D1::Point2F(x-v,y-v),D2D1::Point2F(x+v,y+v),brush.Get(),1.4f);target->DrawLine(D2D1::Point2F(x+v,y-v),D2D1::Point2F(x-v,y+v),brush.Get(),1.4f);}
   else {target->DrawLine(D2D1::Point2F(x-arm,y),D2D1::Point2F(x+arm,y),brush.Get(),1.4f);if(i==2)target->DrawLine(D2D1::Point2F(x,y-arm),D2D1::Point2F(x,y+arm),brush.Get(),1.4f);}}
 }


 Round(D2D1::RectF(12.f,74.f,lay::SidebarWidth-12.f,106.f),16.f,SearchSurface());
 brush->SetColor(C(colors.secondary));target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(29,89),5.f,5.f),brush.Get(),1.4f);target->DrawLine(D2D1::Point2F(32.5f,92.5f),D2D1::Point2F(36.5f,96.5f),brush.Get(),1.4f);
 brush->SetColor(C(chromeContrast?colors.primary:0x5576D8));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(35,141),21.f,21.f),brush.Get());
 Text(L"D",heading.Get(),D2D1::RectF(28,125,52,158),chromeContrast?colors.background:0xFFFFFF);
 Text(L"DockOver",normal.Get(),D2D1::RectF(68,125,lay::SidebarWidth-16,145),colors.primary);
 Text(L"Desktop settings",fontSmall.Get(),D2D1::RectF(68,145,lay::SidebarWidth-16,164),colors.secondary);
 if(visibleCategories.empty()) Text(L"No matching categories",fontSmall.Get(),D2D1::RectF(sp::L,lay::SidebarTop,lay::SidebarWidth-sp::S,lay::SidebarTop+40),colors.secondary);
 target->PushAxisAlignedClip(D2D1::RectF(0,lay::SidebarTop,lay::SidebarWidth,ht),D2D1_ANTIALIAS_MODE_ALIASED);
 for(int row=sidebarFirst;row<static_cast<int>(visibleCategories.size());++row){int i=visibleCategories[row];float y=lay::SidebarTop+static_cast<float>(row-sidebarFirst)*lay::SidebarStep;
  HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
  if(i==selected){const auto selection=D2D1::RoundedRect(D2D1::RectF(sp::M,y,lay::SidebarWidth-sp::M,y+lay::SidebarRow),ds::radius::Button,ds::radius::Button);Round(selection.rect,ds::radius::Button,contrast?colors.selected:0x087AFA);if(contrast||(GetFocus()==h&&keyboardFocusVisible)){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(selection,brush.Get(),1.f);}}
  const auto iconRect=D2D1::RectF(sp::Xl,y+6.f,sp::Xl+24.f,y+30.f);
  const unsigned badges[]={0x85858B,0x34343A,0x34343A,0x85858B,0x1688F8,0x85858B,0x62666F,0x1688F8,0x8D68C5,0x85858B};
  const char* keys[]={"settings.general","settings.appearance","settings.dock","settings.menu-bar","settings.launchpad","settings.control-center","settings.taskbar","settings.workshop","settings.migration","settings.about"};
  const auto custom=settingsPackIcons.find(keys[i]);
  const bool screenshotMenuIcon=i==3&&(activeSettingsPack=="settings-reference-local-01"||activeSettingsPack=="settings-reference-local-02"||(activeSettingsPack.empty()&&activePack=="apple-combined-2026"));
  if(contrast||screenshotMenuIcon||custom==settingsPackIcons.end()||!DrawWorkshopPreview(custom->second,iconRect)){Round(iconRect,ds::radius::Button/2,contrast?colors.background:badges[i]);DrawCategoryGlyph(i,iconRect,contrast?colors.primary:colors.onAccent);}
  Text(titles[i],sidebarLabel.Get(),D2D1::RectF(sp::Xl*2+sp::Xs,y,lay::SidebarWidth-sp::L,y+lay::SidebarRow),(i==selected&&!contrast)?0xFFFFFF:colors.primary);}
 target->PopAxisAlignedClip();
 if(visibleCategories.empty()){
  Text(L"No Results",heading.Get(),D2D1::RectF(left,lay::ContentTop,right,lay::ContentTop+sp::Xxl+sp::S),colors.primary);
  Text(L"Try another search term.",fontSmall.Get(),D2D1::RectF(left,lay::CardTop,right,lay::CardTop+sp::Xxl),colors.secondary);return;
 }
 // The navigation buttons operate on actual visited categories, including keyboard navigation.
 const float nav=lay::SidebarWidth+10.f;
 Round(D2D1::RectF(nav,10,nav+88,52),21.f,colors.surface);
 brush->SetColor(C(colors.secondary,0.4f));target->DrawLine(D2D1::Point2F(nav+44,16),D2D1::Point2F(nav+44,46),brush.Get(),1.f);
 for(int i=0;i<2;++i){const bool enabled=i==0?categoryHistoryIndex>0:categoryHistoryIndex+1<categoryHistory.size();const float cx=nav+22.f+i*44.f,sign=i==0?-1.f:1.f;brush->SetColor(C(enabled?colors.primary:colors.secondary,enabled?1.f:0.45f));target->DrawLine(D2D1::Point2F(cx-sign*3.5f,24),D2D1::Point2F(cx+sign*3.5f,31),brush.Get(),2.f);target->DrawLine(D2D1::Point2F(cx+sign*3.5f,31),D2D1::Point2F(cx-sign*3.5f,38),brush.Get(),2.f);}
 Text(titles[selected],heading.Get(),D2D1::RectF(nav+102,lay::ContentTop,right,lay::ContentTop+sp::Xxl+sp::S),colors.primary);
 if(selected==0){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+70),ds::radius::Card,colors.surface);
  Round(D2D1::RectF(left+14,top+18,left+48,top+52),8.f,chromeContrast?colors.background:0x7D8088);
  if(categoryIcons)Text(L"\uE713",categoryIcons.Get(),D2D1::RectF(left+14,top+18,left+48,top+52),chromeContrast?colors.primary:0xFFFFFF);
  Text(L"Windows visual effects",normal.Get(),D2D1::RectF(left+58,top+12,right-170,top+36),colors.primary);
  Text(L"Transparency and animation",fontSmall.Get(),D2D1::RectF(left+58,top+37,right-170,top+59),colors.secondary);
  const auto button=D2D1::RectF(right-158,top+20,right-12,top+50);Round(button,ds::radius::Button,colors.background);
  if(GetFocus()==h){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(button.left-2,button.top-2,button.right+2,button.bottom+2),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
  Text(L"Windows Settings…",centerSmall.Get(),button,colors.primary);
  Round(D2D1::RectF(left,top+82,right,top+166),ds::radius::Card,colors.surface);
  brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+16,top+124),D2D1::Point2F(right-16,top+124),brush.Get(),1.f);
  Text(L"System transparency",normal.Get(),D2D1::RectF(left+16,top+90,right-150,top+120),colors.primary);
  Text(allowTransparency?L"Allowed":L"Reduced",trailingSmall.Get(),D2D1::RectF(right-150,top+82,right-16,top+124),colors.secondary);
  BOOL motion=TRUE;const bool motionKnown=SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0)!=FALSE;
  Text(L"Animations",normal.Get(),D2D1::RectF(left+16,top+132,right-150,top+160),colors.primary);
  Text(!motionKnown?L"Unavailable":motion?L"On":L"Reduced",trailingSmall.Get(),D2D1::RectF(right-150,top+124,right-16,top+166),colors.secondary);
  {const float c=top+178.f;Round(D2D1::RectF(left,c,right,c+160.f),ds::radius::Card,colors.surface);
   Text(L"Frame rate",normal.Get(),D2D1::RectF(left+16,c+10,right-16,c+36),colors.primary);
   const wchar_t* rows[]={L"Animations",L"Capture"};const int values[]={perf.animationFps,perf.captureFps};
   const wchar_t* names[]={L"30",L"60",L"120",L"240",L"Unlimited"};
   for(int row=0;row<2;++row){const auto first=FrameCapButton(row,0,left,right);
    Text(rows[row],fontSmall.Get(),D2D1::RectF(left+16,first.top+5,first.left-8,first.bottom),colors.primary);
    for(int i=0;i<5;++i){const auto b=FrameCapButton(row,i,left,right);Round(b,ds::radius::Button,values[row]==kFrameCaps[i]?colors.selected:colors.background);
     Text(names[i],centerSmall.Get(),b,colors.primary);}}
   Text(L"Unlimited follows the display's refresh rate. Lower values save power; capture sets how often live glass backgrounds update.",fontSmall.Get(),D2D1::RectF(left+16,c+124,right-16,c+156),colors.secondary);}
  {const float c=DesktopLaunchCardTop();Round(D2D1::RectF(left,c,right,c+lay::CardHeight),ds::radius::Card,colors.surface);
   Text(L"Start DockOver Desktop with DockOver",normal.Get(),D2D1::RectF(left+sp::L,c+sp::L,right-sp::Xxl*3,c+sp::L+sp::Xxl),colors.primary);
   Text(L"Starting the Dock also starts widgets and Stacks. Starting DockOver Desktop alone does not start the Dock.",fontSmall.Get(),D2D1::RectF(left+sp::L,c+sp::Xl+sp::L,right-sp::Xxl*3,c+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,16,DesktopLaunchSwitch(right),launchDesktop,switchPressed==16,false,colors);}
  {const float c=LoginItemCardTop();Round(D2D1::RectF(left,c,right,c+lay::CardHeight),ds::radius::Card,colors.surface);
   Text(L"Open DockOver at login",normal.Get(),D2D1::RectF(left+sp::L,c+sp::L,right-sp::Xxl*3,c+sp::L+sp::Xxl),colors.primary);
   Text(L"Starts the Dock and Menu Bar when you sign in to Windows. Also listed in Task Manager > Startup apps.",fontSmall.Get(),D2D1::RectF(left+sp::L,c+sp::Xl+sp::L,right-sp::Xxl*3,c+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,17,LoginItemSwitch(right),loginItem,switchPressed==17,false,colors);}
 }else if(selected==1){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+150),ds::radius::Card,colors.surface);
  Text(L"Settings appearance",normal.Get(),D2D1::RectF(left+sp::L,top+12,right-sp::L,top+40),colors.primary);
  const wchar_t* modes[]={L"System",L"Light",L"Dark"};const float gap=sp::S,bw=(right-left-2*sp::L-2*gap)/3;
  for(int i=0;i<3;++i){const float x=left+sp::L+i*(bw+gap);Round(D2D1::RectF(x,top+48,x+bw,top+80),ds::radius::Button,appearanceMode==i?colors.selected:colors.background);
   Text(modes[i],fontSmall.Get(),D2D1::RectF(x+8,top+55,x+bw-4,top+78),colors.primary);
   if(GetFocus()==h&&appearanceKeyboard==i){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-2,top+46,x+bw+2,top+82),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}}
  Text(L"Choose how DockOver uses light and dark appearance.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+96,right-sp::L,top+138),colors.secondary);
 }else if(selected==6){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+118),ds::radius::Card,colors.surface);
  Text(L"Windows taskbar",normal.Get(),D2D1::RectF(left+sp::L,top+14,right-sp::L,top+40),colors.primary);
  const bool docHidden=tbLive&&tbStatus.state==L"hidden";
  std::wstring current=!taskbarState.known?L"Current: unavailable":taskbarState.visible==taskbarState.windows?L"Current: visible":taskbarState.visible==0?(docHidden?L"Current: hidden by DockOver":L"Current: hidden (not by DockOver)"):L"Current: partly hidden";
  if(tbLive&&tbStatus.state==L"conflict")current+=L"  •  record needs review (nothing forced)";
  Text(current.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+42,right-sp::L,top+64),colors.secondary);
  const std::wstring guard=!tbLive?L"Recovery guardian: DockOver is not running":tbStatus.guardian==L"ready"?L"Recovery guardian: ready":tbStatus.guardian==L"starting"?L"Recovery guardian: starting…":tbStatus.guardian==L"failed"?L"Recovery guardian: unavailable — hiding disabled":L"Recovery guardian: not started";
  Text(guard.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+64,right-sp::L,top+86),colors.secondary);
  const std::wstring winAuto=!taskbarState.known?L"Windows auto-hide: unavailable":taskbarState.autoHide?L"Windows auto-hide: on (a Windows setting, separate from DockOver hiding)":L"Windows auto-hide: off (a Windows setting, separate from DockOver hiding)";
  Text(winAuto.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+86,right-sp::L,top+110),colors.secondary);
  const bool canToggle=tbLive&&(docHidden||tbStatus.guardian==L"ready");
  const wchar_t* labels[]={docHidden?L"Show taskbar":L"Hide taskbar",L"Restore Windows taskbar now",L"Windows Taskbar Settings…"};
  for(int i=0;i<3;++i){const auto r=TaskbarControlRect(i,left,right);const bool enabled=i!=0||canToggle;
   Round(r,ds::radius::Button,enabled?colors.surface:colors.background);
   if(GetFocus()==h&&taskbarKeyboardControl==i){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left-2,r.top-2,r.right+2,r.bottom+2),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
   Text(labels[i],fontSmall.Get(),D2D1::RectF(r.left+10,r.top+7,r.right-4,r.bottom-2),enabled?colors.primary:colors.secondary);}
  const float card2=top+176;Round(D2D1::RectF(left,card2,right,card2+224),ds::radius::Card,colors.surface);
  const std::wstring hotkeyLine=!taskbarPref.hotkeyKey?L"No shortcut set.":!taskbarPref.hotkeyEnabled?L"Off":!tbLive?L"Active while DockOver runs.":tbStatus.hotkey==L"registered"?L"Active: press it anywhere to hide or show the taskbar.":
                                tbStatus.hotkey==L"suspended"?L"Paused while a new shortcut is recorded.":tbStatus.hotkey==L"conflict"?L"In use by another app — record a different shortcut.":tbStatus.hotkey==L"invalid"?L"Not usable — record a different shortcut.":L"Applying…";
  struct Row{const wchar_t* title;std::wstring detail;bool on;};
  const Row rows[]={{L"Hide Windows taskbar on startup",L"Applies at startup; restore it manually.",taskbarPref.hideOnStartup},
                    {L"Free work area when taskbar is hidden",L"Temporarily reclaims the taskbar area.",taskbarPref.freeWorkArea},
                    {L"Toggle taskbar hotkey",hotkeyLine,taskbarPref.hotkeyEnabled&&taskbarPref.hotkeyKey!=0}};
  for(int i=0;i<3;++i){const float y=card2+i*56.f;
   if(i){brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+sp::L,y),D2D1::Point2F(right-sp::L,y),brush.Get(),1.f);}
   Text(rows[i].title,normal.Get(),D2D1::RectF(left+sp::L,y+8,right-sp::Xxl*3,y+30),colors.primary);
   Text(rows[i].detail.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,y+30,right-sp::Xxl*3,y+54),colors.secondary);
   const auto s=TaskbarControlRect(3+i,left,right);
   DrawSettingsSwitch(h,i+3,s,rows[i].on,switchPressed==i+3,GetFocus()==h&&keyboardFocusVisible&&taskbarKeyboardControl==3+i,colors);}

  {const float y=card2+168;brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+sp::L,y),D2D1::Point2F(right-sp::L,y),brush.Get(),1.f);
   Text(L"Shortcut",normal.Get(),D2D1::RectF(left+sp::L,y+8,left+120,y+30),colors.primary);
   const std::wstring shortcut=recordingHotkey?L"Press keys…":docky::hotkey::Text(static_cast<UINT>(taskbarPref.hotkeyModifiers),static_cast<UINT>(taskbarPref.hotkeyKey));
   Round(D2D1::RectF(left+120,y+12,right-208,y+44),ds::radius::Button,recordingHotkey?colors.selected:colors.background);
   Text(shortcut.c_str(),fontSmall.Get(),D2D1::RectF(left+132,y+19,right-212,y+42),colors.primary);
   const wchar_t* b[]={recordingHotkey?L"Cancel":L"Record",L"Clear"};
   for(int i=0;i<2;++i){const auto r=TaskbarControlRect(6+i,left,right);Round(r,ds::radius::Button,colors.background);
    if(GetFocus()==h&&taskbarKeyboardControl==6+i){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left-2,r.top-2,r.right+2,r.bottom+2),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
    Text(b[i],fontSmall.Get(),D2D1::RectF(r.left+12,r.top+7,r.right-4,r.bottom-2),colors.primary);}}
  const std::wstring feedback=!taskbarFeedback.empty()?taskbarFeedback:tbLive?tbStatus.message:L"";
  if(!feedback.empty())Text(feedback.c_str(),fontSmall.Get(),D2D1::RectF(left,card2+232,right,card2+256),colors.secondary);
 }else if(selected==8 || selected==9){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+150),ds::radius::Card,colors.surface);
  if(selected==8){
   // Import from MyDockFinder: Dock items and Launchpad order (backups first).
   {const float t2=top+166.f;Round(D2D1::RectF(left,t2,right,t2+150),ds::radius::Card,colors.surface);
    Text(L"Import from MyDockFinder",normal.Get(),D2D1::RectF(left+sp::L,t2+sp::L,right-sp::L,t2+44),colors.primary);
    Text(L"Import: MyDockFinder's Dock items, and its Launchpad order first. Replace everything: back up all of DockOver's items to one folder, remove them, and keep only MyDockFinder's.",fontSmall.Get(),D2D1::RectF(left+sp::L,t2+48,right-sp::L,t2+100),colors.secondary);
    Round(D2D1::RectF(left+sp::L,t2+104,left+204,t2+136),ds::radius::Button,colors.background);
    Text(L"Import…",fontSmall.Get(),D2D1::RectF(left+sp::Xl,t2+111,left+196,t2+132),colors.primary);
    Round(D2D1::RectF(left+216,t2+104,left+404,t2+136),ds::radius::Button,colors.background);
    Text(L"Replace everything…",fontSmall.Get(),D2D1::RectF(left+228,t2+111,left+400,t2+132),colors.primary);}
   Text(L"Back up an existing configuration",normal.Get(),D2D1::RectF(left+sp::L,top+sp::L,right-sp::L,top+44),colors.primary);
   Text(L"Choose a file to create a separate backup. This does not import or change the original.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+48,right-sp::L,top+96),colors.secondary);
   Round(D2D1::RectF(left+sp::L,top+104,left+204,top+136),ds::radius::Button,colors.background);
   Text(L"Choose file…",fontSmall.Get(),D2D1::RectF(left+sp::Xl,top+111,left+196,top+132),colors.primary);
  }else{
   Text(L"DockOver",normal.Get(),D2D1::RectF(left+sp::L,top+sp::L,right-sp::L,top+44),colors.primary);
   Text(L"A Windows desktop Dock and menu bar.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+48,right-sp::L,top+72),colors.secondary);
   const std::wstring ver=L"Version "+dock::json::FromUtf8(docky::update::CurrentVersion());
   Text(ver.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+76,right-sp::L,top+100),colors.primary);
   const std::wstring src=L"Updates from github.com/"+docky::update::Repository();
   Text(src.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+104,right-sp::L,top+128),colors.secondary);
   // Software Update card.
   {const float t2=UpdateCardTop();Round(D2D1::RectF(left,t2,right,t2+150),ds::radius::Card,colors.surface);
    Text(L"Software Update",normal.Get(),D2D1::RectF(left+sp::L,t2+sp::L,right-sp::L,t2+44),colors.primary);
    std::wstring status,detail;const std::wstring next=dock::json::FromUtf8(updRelease.version);
    switch(updUi){
     case UpdUi::Idle:status=!updState.lastCheck?std::wstring(L"Not checked yet."):!updState.latest.empty()&&docky::update::CompareVersions(updState.latest,docky::update::CurrentVersion())>0?L"DockOver "+dock::json::FromUtf8(updState.latest)+L" is available. Check for Updates to install it.":L"DockOver is up to date as of the last check.";detail=L"Last checked: "+UpdateWhen(updState.lastCheck)+L".";break;
     case UpdUi::Checking:status=L"Checking for updates…";break;
     case UpdUi::UpToDate:status=L"DockOver is up to date.";detail=L"Version "+dock::json::FromUtf8(docky::update::CurrentVersion())+L" is the newest release.";break;
     case UpdUi::NoRelease:status=L"No releases have been published yet.";detail=L"Nothing to install.";break;
     case UpdUi::Available:status=L"DockOver "+next+L" is available.";detail=L"Install downloads and verifies it, then restarts docky. Your settings are kept.";break;
     case UpdUi::Downloading:{const uint64_t d=updDone,t=updTotal;wchar_t b[160];if(t)_snwprintf_s(b,_TRUNCATE,L"Downloading docky %ls… %.1f of %.1f MB",next.c_str(),d/1048576.0,t/1048576.0);else _snwprintf_s(b,_TRUNCATE,L"Downloading docky %ls… %.1f MB",next.c_str(),d/1048576.0);status=b;
      if(t){const float w=(right-left-2*sp::L),f=static_cast<float>((std::min)(1.0,static_cast<double>(d)/t));Round(D2D1::RectF(left+sp::L,t2+80,left+sp::L+w,t2+86),3.f,colors.background);Round(D2D1::RectF(left+sp::L,t2+80,left+sp::L+(std::max)(6.f,w*f),t2+86),3.f,colors.selected);}break;}
     case UpdUi::Installing:status=L"Installing DockOver "+next+L"…";detail=L"DockOver closes and starts again; Settings reopens when it is done.";break;
     case UpdUi::Error:status=L"The update did not complete.";detail=updError;break;}
    Text(status.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,t2+48,right-sp::L,t2+72),colors.primary);
    if(!detail.empty())Text(detail.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,t2+72,right-sp::L,t2+102),colors.secondary);
    const bool busy=updUi==UpdUi::Checking||updUi==UpdUi::Installing;
    const auto cb=UpdateCheckButton(left,t2);Round(cb,ds::radius::Button,colors.background);
    Text(updUi==UpdUi::Downloading?L"Cancel":L"Check for Updates",centerSmall.Get(),cb,busy?colors.secondary:colors.primary);
    if(updUi==UpdUi::Available){const auto ib=UpdateInstallButton(left,t2);Round(ib,ds::radius::Button,colors.selected);Text((L"Install "+next).c_str(),centerSmall.Get(),ib,colors.primary);}}
   const wchar_t* titles2[]={L"Check for updates automatically",L"Install updates automatically"};
   const wchar_t* details2[]={L"Once a day while DockOver runs, with a notification for each new version.",L"Download and install new versions without asking; DockOver restarts by itself."};
   for(int row=0;row<2;++row){const float c=UpdateSwitchCardTop(row);Round(D2D1::RectF(left,c,right,c+lay::CardHeight),ds::radius::Card,colors.surface);
    Text(titles2[row],normal.Get(),D2D1::RectF(left+sp::L,c+sp::L,right-sp::Xxl*3,c+sp::L+sp::Xxl),colors.primary);
    Text(details2[row],fontSmall.Get(),D2D1::RectF(left+sp::L,c+sp::Xl+sp::L,right-sp::Xxl*3,c+lay::CardHeight-sp::Xs),colors.secondary);
    DrawSettingsSwitch(h,14+row,UpdateSwitch(right,row),row?updatePref.autoInstall:updatePref.autoCheck,switchPressed==14+row,false,colors);}
  }
 }else if(selected==7){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+160),ds::radius::Card,colors.surface);
  if(packs.empty())Text(L"No installed packs. Import a .dockypack to begin.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+18,right-sp::L,top+54),colors.secondary);
  for(int i=0;i<3&&packOffset+i<static_cast<int>(packs.size());++i){const int index=packOffset+i;const float y=top+4+i*48.f;
   if(index==packSelected)Round(D2D1::RectF(left+4,y,right-4,y+44),ds::radius::Button,colors.selected);
   const auto& pack=packs[index];const auto name=dock::json::FromUtf8(pack.name);const auto detail=dock::json::FromUtf8(pack.version)+(!pack.valid?L"  •  Damaged":(pack.id==activePack||pack.id==activeSettingsPack)?L"  •  Enabled":L"  •  Disabled")+(pack.valid&&SettingsOnly(pack)?L"  •  Settings icons":L"");
   Text(name.c_str(),normal.Get(),D2D1::RectF(left+sp::L,y+3,right-sp::L,y+24),colors.primary);Text(detail.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,y+25,right-sp::L,y+43),colors.secondary);}
  Round(D2D1::RectF(left,top+168,left+48,top+196),ds::radius::Button,colors.surface);Round(D2D1::RectF(left+56,top+168,left+104,top+196),ds::radius::Button,colors.surface);
  if(GetFocus()==h&&workshopKeyboardControl<2){const float bx=left+workshopKeyboardControl*56.f;brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx-2,top+166,bx+50,top+198),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
  Text(L"‹",normal.Get(),D2D1::RectF(left+19,top+169,left+46,top+193),colors.primary);Text(L"›",normal.Get(),D2D1::RectF(left+75,top+169,left+102,top+193),colors.primary);
  if(packSelected>=0&&packSelected<static_cast<int>(packs.size()))DrawWorkshopPreview(packs[packSelected].preview,D2D1::RectF(left+116,top+168,right,top+202));
  const wchar_t* buttons[]={L"Import…",L"Export…",L"Delete",L"Enable / Disable",L"Reset Selected"};
  const float gap=8,bw=(right-left-2*gap)/3.f;
  for(int i=0;i<5;++i){const int row=i<3?0:1,col=i<3?i:i-3;const float x=left+col*(bw+gap),y=top+212+row*40.f;
   const bool selectedPack=packSelected>=0&&packSelected<static_cast<int>(packs.size());const bool available=i==0||i==4||(selectedPack&&(i==2||packs[packSelected].valid));
   Round(D2D1::RectF(x,y,x+bw,y+32),ds::radius::Button,available?colors.surface:colors.background);
   if(GetFocus()==h&&workshopKeyboardControl==i+2){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-2,y-2,x+bw+2,y+34),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
   Text(buttons[i],centerSmall.Get(),D2D1::RectF(x,y,x+bw,y+32),available?colors.primary:colors.secondary);}
 }else if(selected==4){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+220.f),ds::radius::Card,colors.surface);
  const wchar_t* names[]={L"Compact",L"Default",L"Spacious",L"Custom"};const int values[]={1,0,2,3};
  const float gap=sp::S,button=(right-left-2*sp::L-3*gap)/4.f;
  for(int i=0;i<4;++i){const float x=left+sp::L+i*(button+gap);Round(D2D1::RectF(x,top+sp::L,x+button,top+sp::L+32),ds::radius::Button,density.densityPreset==values[i]?colors.selected:colors.background);
   if(GetFocus()==h&&densityKeyboardControl==i){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-2,top+sp::L-2,x+button+2,top+sp::L+34),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
   Text(names[i],fontSmall.Get(),D2D1::RectF(x+4,top+sp::L+7,x+button-2,top+sp::L+30),colors.primary);}
  if(density.densityPreset==3){const wchar_t* names2[]={L"Icon size",L"Horizontal spacing",L"Vertical spacing"};const int vals[]={density.iconSize,density.gapX,density.gapY};
   for(int i=0;i<3;++i){const float y=top+60.f+i*36.f;Text(names2[i],normal.Get(),D2D1::RectF(left+sp::L,y+3,right-140,y+30),colors.primary);
    Round(D2D1::RectF(right-126,y,right-98,y+28),ds::radius::Button,colors.background);Round(D2D1::RectF(right-48,y,right-20,y+28),ds::radius::Button,colors.background);
    if(GetFocus()==h&&(densityKeyboardControl==4+2*i||densityKeyboardControl==5+2*i)){const float bx=densityKeyboardControl==4+2*i?right-126:right-48;brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx-2,y-2,bx+30,y+30),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
    Text(L"−",normal.Get(),D2D1::RectF(right-117,y+2,right-100,y+26),colors.primary);Text(L"+",normal.Get(),D2D1::RectF(right-39,y+2,right-21,y+26),colors.primary);
    wchar_t number[32]{};_snwprintf_s(number,_TRUNCATE,L"%d",vals[i]);Text(number,fontSmall.Get(),D2D1::RectF(right-89,y+5,right-49,y+27),colors.primary);}
  }else Text(L"Select Custom to adjust size and spacing.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+70,right-sp::L,top+96),colors.secondary);
  dock::launchpad::DensityConfig cfg;cfg.preset=density.densityPreset==1?dock::launchpad::DensityPreset::Compact:density.densityPreset==2?dock::launchpad::DensityPreset::Spacious:density.densityPreset==3?dock::launchpad::DensityPreset::Custom:dock::launchpad::DensityPreset::Default;
  cfg.icon=static_cast<float>(density.iconSize);cfg.gapX=static_cast<float>(density.gapX);cfg.gapY=static_cast<float>(density.gapY);
  MONITORINFO mi{sizeof(mi)};if(GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&mi)){auto grid=dock::launchpad::PlanDensity((mi.rcMonitor.right-mi.rcMonitor.left)/scale,(mi.rcMonitor.bottom-mi.rcMonitor.top)/scale,cfg);
   wchar_t preview[96]{};_snwprintf_s(preview,_TRUNCATE,L"Estimated grid: %d columns × %d rows",grid.columns,grid.rows);Text(preview,fontSmall.Get(),D2D1::RectF(left+sp::L,top+184,right-sp::L,top+211),colors.secondary);}
  {const float c2=top+236.f;Round(D2D1::RectF(left,c2,right,c2+lay::CardHeight),ds::radius::Card,colors.surface);
   Text(L"Hide app names",normal.Get(),D2D1::RectF(left+sp::L,c2+sp::L,right-sp::Xxl*3,c2+sp::L+sp::Xxl),colors.primary);
   Text(L"Show only the icons in Launchpad. Applies immediately.",fontSmall.Get(),D2D1::RectF(left+sp::L,c2+sp::Xl+sp::L,right-sp::Xxl*3,c2+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,9,LaunchpadNamesSwitch(right),density.hideLabels,switchPressed==9,false,colors);}
  {const float c3=top+236.f+lay::CardHeight+12.f;Round(D2D1::RectF(left,c3,right,c3+lay::CardHeight),ds::radius::Card,colors.surface);
   Text(L"Show windows behind Launchpad",normal.Get(),D2D1::RectF(left+sp::L,c3+sp::L,right-sp::Xxl*3,c3+sp::L+sp::Xxl),colors.primary);
   Text(L"Off: only the wallpaper (live, dynamic wallpapers too). On: the screen with its open windows.",fontSmall.Get(),D2D1::RectF(left+sp::L,c3+sp::Xl+sp::L,right-sp::Xxl*3,c3+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,13,LaunchpadWindowsSwitch(right),density.showWindows,switchPressed==13,false,colors);}
 }else if(selected==2){
  // V0.15 Dock page: every row maps to a setting the Dock already applies (mask/auto-hide live, diagnostics on restart).
  struct RowSpec{const wchar_t* title;const wchar_t* detail;bool on;};
  const RowSpec rows[]={{L"macOS icon mask",L"Rounded-square tiles for app icons. Applies immediately.",iconMask},
                        {L"Automatically hide and show the Dock",L"Slides away when the pointer leaves; move to the screen edge to reveal.",autoHide},
                        {L"Show Dock diagnostics",L"Applies when the Dock restarts.",diagnostics},
                        {L"Hide snap layouts while dragging windows",L"Hides the Windows layout bar at the top of the screen during a drag.",hideSnap},
                        {L"Genie minimize animation",L"Minimized windows pour into the Dock. Pauses Windows' own minimize animation while DockOver runs.",genie}};
  Round(D2D1::RectF(left,lay::CardTop,right,lay::CardTop+5*lay::CardHeight),ds::radius::Card,colors.surface);
  for(int i=0;i<5;++i){const float dy=i*lay::CardHeight;
   if(i){brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+sp::L,lay::CardTop+dy),D2D1::Point2F(right-sp::L,lay::CardTop+dy),brush.Get(),1.f);}
   Text(rows[i].title,normal.Get(),D2D1::RectF(left+sp::L,lay::CardTop+dy+sp::L,right-sp::Xxl*3,lay::CardTop+dy+sp::L+sp::Xxl),colors.primary);
   Text(rows[i].detail,fontSmall.Get(),D2D1::RectF(left+sp::L,lay::CardTop+dy+sp::Xl+sp::L,right-sp::Xxl*3,lay::CardTop+dy+lay::CardHeight-sp::Xs),colors.secondary);
   const float switchRight=right-lay::SwitchRight,switchLeft=switchRight-lay::SwitchWidth,st=lay::SwitchTop+dy;
   const int sw=i==3?8:i==4?10:i;DrawSettingsSwitch(h,sw,D2D1::RectF(switchLeft,st,switchRight,st+lay::SwitchHeight),rows[i].on,switchPressed==sw,GetFocus()==h&&keyboardFocusVisible&&i==dockKeyboardRow,colors);}
  // Downloads stack: how many files the fan shows, and whether it rises straight up.
  {const float c2=DownloadsCardTop();Round(D2D1::RectF(left,c2,right,c2+3*lay::CardHeight),ds::radius::Card,colors.surface);
   Text(L"Downloads: files shown",normal.Get(),D2D1::RectF(left+sp::L,c2+sp::L,right-140,c2+sp::L+sp::Xxl),colors.primary);
   Text(L"How many recent files the Downloads fan shows (3–10). The rest stay one click away in Explorer.",fontSmall.Get(),D2D1::RectF(left+sp::L,c2+sp::Xl+sp::L,right-140,c2+lay::CardHeight-sp::Xs),colors.secondary);
   const float y=c2+sp::L;
   Round(D2D1::RectF(right-126,y,right-98,y+28),ds::radius::Button,colors.background);Round(D2D1::RectF(right-48,y,right-20,y+28),ds::radius::Button,colors.background);
   Text(L"−",normal.Get(),D2D1::RectF(right-117,y+2,right-100,y+26),colors.primary);Text(L"+",normal.Get(),D2D1::RectF(right-39,y+2,right-21,y+26),colors.primary);
   wchar_t number[16]{};_snwprintf_s(number,_TRUNCATE,L"%d",dlCount);Text(number,fontSmall.Get(),D2D1::RectF(right-80,y+5,right-49,y+27),colors.primary);
   const float d2=c2+lay::CardHeight;brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+sp::L,d2),D2D1::Point2F(right-sp::L,d2),brush.Get(),1.f);
   Text(L"Downloads: straight fan",normal.Get(),D2D1::RectF(left+sp::L,d2+sp::L,right-sp::Xxl*3,d2+sp::L+sp::Xxl),colors.primary);
   Text(L"The fan rises straight up from the Dock instead of curving.",fontSmall.Get(),D2D1::RectF(left+sp::L,d2+sp::Xl+sp::L,right-sp::Xxl*3,d2+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,11,DownloadsVerticalSwitch(right),dlVertical,switchPressed==11,false,colors);
   const float d3=c2+2*lay::CardHeight;brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+sp::L,d3),D2D1::Point2F(right-sp::L,d3),brush.Get(),1.f);
   Text(L"Downloads: file previews",normal.Get(),D2D1::RectF(left+sp::L,d3+sp::L,right-sp::Xxl*3,d3+sp::L+sp::Xxl),colors.primary);
   Text(L"Show pictures, PDFs and videos as thumbnails. Folders and archives keep their icons.",fontSmall.Get(),D2D1::RectF(left+sp::L,d3+sp::Xl+sp::L,right-sp::Xxl*3,d3+lay::CardHeight-sp::Xs),colors.secondary);
   DrawSettingsSwitch(h,12,DownloadsVerticalSwitch(right,2),dlPreview,switchPressed==12,false,colors);}

 }else if(selected==5){
  // Control Center: strength of the blur behind the whole panel (applies the next time it opens).
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+96.f),ds::radius::Card,colors.surface);
  Text(L"Background blur",normal.Get(),D2D1::RectF(left+sp::L,top+sp::L,right-140,top+sp::L+sp::Xxl),colors.primary);
  Text(L"How strongly the desktop behind the Control Center is blurred. 0% turns it off.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+sp::Xl+sp::L,right-140,top+90),colors.secondary);
  const float y=top+sp::L;
  Round(D2D1::RectF(right-126,y,right-98,y+28),ds::radius::Button,colors.background);Round(D2D1::RectF(right-48,y,right-20,y+28),ds::radius::Button,colors.background);
  Text(L"\u2212",normal.Get(),D2D1::RectF(right-117,y+2,right-100,y+26),colors.primary);Text(L"+",normal.Get(),D2D1::RectF(right-39,y+2,right-21,y+26),colors.primary);
  wchar_t number[32]{};_snwprintf_s(number,_TRUNCATE,L"%d%%",ccBlur);Text(number,fontSmall.Get(),D2D1::RectF(right-92,y+5,right-49,y+27),colors.primary);
 }else{
 const float cardBottom=lay::CardTop+(selected==2?lay::CardHeight:lay::CardHeight+sp::S);
 Round(D2D1::RectF(left,lay::CardTop,right,cardBottom),ds::radius::Card,colors.surface);
 Text(selected==2?L"Show Dock diagnostics":L"More settings coming soon",normal.Get(),
      D2D1::RectF(left+sp::L,lay::CardTop+sp::L,right-(selected==2?sp::Xxl*3:sp::L),lay::CardTop+sp::L+sp::Xxl),colors.primary);
 Text(selected==2?L"Applies when the Dock restarts.":L"Additional controls will appear here when available.",fontSmall.Get(),
      D2D1::RectF(left+sp::L,lay::CardTop+sp::Xl+sp::L,right-(selected==2?sp::Xxl*3:sp::L),cardBottom-sp::Xs),colors.secondary);

 }

}
void DisableGpu(){
 previewCache.clear();previewCacheTarget=nullptr;
 if(backdrop){backdrop->Request(false,nullptr);backdrop->SetDevice(nullptr);backdrop.reset();}
 if(renderer){renderer->Shutdown();renderer.reset();}
 gpuReady=false;target=nullptr;brush.Reset();
}
void Draw(HWND h){
 if(gpuReady&&renderer){
  RECT client{};GetClientRect(h,&client);if(client.right<=0||client.bottom<=0)return;
  dock::BackdropFrame frame;RECT roi=SidebarScreen(h);
  const bool have=allowTransparency&&backdrop&&backdrop->Latest(roi,frame);
  dock::DockFrame geometry;geometry.dockLeft=0;geometry.dockTop=0;
  geometry.dockRight=liquid::design::settings::SidebarWidth;geometry.dockBottom=client.bottom/scale;
  dock::LocalGlassOverlay localGlass;
  dock::Scene scene;scene.frame=&geometry;scene.glass=nullptr;scene.backdrop=nullptr;scene.windowScreenRect=ClientScreen(h);
  scene.localGlass=&localGlass;
  scene.customDraw=[h,&localGlass](ID2D1DeviceContext* dc,bool){
   target=dc;brush.Reset();if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xffffff),&brush))){
    DrawWindowOutline(h,false); RECT bounds{};GetClientRect(h,&bounds);
    BOOL contains=FALSE;const bool candidate=!IsZoomed(h)&&windowOutline;
    const bool mask=candidate&&SUCCEEDED(windowOutline->FillContainsPoint(D2D1::Point2F(bounds.right/(2*scale),bounds.bottom/(2*scale)),nullptr,&contains))&&contains;
    if(mask)dc->PushLayer(D2D1::LayerParameters(D2D1::RectF(0,0,bounds.right/scale,bounds.bottom/scale),windowOutline.Get(),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),nullptr);
    drawingLocalGlass=renderer->LiquidAvailable()?&localGlass:nullptr;DrawContent(h,false);drawingLocalGlass=nullptr;DrawWindowOutline(h);
    if(mask)dc->PopLayer();}
   brush.Reset();target=nullptr;
  };
  localGlass.fallback=[&localGlass](ID2D1DeviceContext* dc){ComPtr<ID2D1SolidColorBrush> b;
   if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xFFFFFF),&b))){const float rad=(localGlass.plate.bottom-localGlass.plate.top)*.5f;dc->FillRoundedRectangle(D2D1::RoundedRect(localGlass.plate,rad,rad),b.Get());}};
  localGlass.after=[&localGlass](ID2D1DeviceContext* dc){if(localGlassWhite<=.001f)return;ComPtr<ID2D1SolidColorBrush> b;
   if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xFFFFFF,localGlassWhite),&b))){const float rad=(localGlass.plate.bottom-localGlass.plate.top)*.5f;dc->FillRoundedRectangle(D2D1::RoundedRect(localGlass.plate,rad,rad),b.Get());}};
  if(have&&!snapshotPath.empty()){renderer->RequestSnapshot(snapshotPath);snapshotPath.clear();}
  const HRESULT hr=renderer->Render(scene);
  const bool used=SUCCEEDED(hr)&&renderer->LastUsedGpu();
  if(!haveLoggedGlass||used!=previousGlass){DLOG(L"settings: glass=%d backdrop=%d hr=0x%08X",used,have,hr);haveLoggedGlass=true;previousGlass=used;}
  if(SUCCEEDED(hr))return;
  DLOG(L"settings: GPU render failed; switching to opaque fallback");DisableGpu();
 }
 if(!CreateResources(h))return;target->BeginDraw();DrawContent(h,false);DrawWindowOutline(h);
 HRESULT hr=target->EndDraw();if(hr==D2DERR_RECREATE_TARGET){target=nullptr;previewCache.clear();previewCacheTarget=nullptr;fallbackTarget.Reset();brush.Reset();}
}
// Settings > Migration > Import from MyDockFinder.
void ImportMyDockFinder(HWND h,bool replace){
 std::wstring dir=docky::mdf::FindInstall();
 if(dir.empty()){
  // Not in the Steam default folder: let the user point at its ico.ini.
  wchar_t file[MAX_PATH]{};OPENFILENAMEW of{sizeof(of)};of.hwndOwner=h;of.lpstrFilter=L"MyDockFinder ico.ini\0ico.ini\0";of.lpstrFile=file;of.nMaxFile=MAX_PATH;
  of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;of.lpstrTitle=L"Select MyDockFinder's ico.ini";
  if(!GetOpenFileNameW(&of))return;dir=file;dir=dir.substr(0,dir.find_last_of(L"\\/"));
 }
 const wchar_t* ask=replace
  ?L"Replace everything with MyDockFinder?\n\nDockOver's Dock items, Launchpad order and folders, and the shortcuts in Launchpad Apps are first copied to a new folder under LiquidDock\\Backups, then removed. Afterwards the Dock and Launchpad hold only MyDockFinder's items (apps installed later still appear in Launchpad).\n\nYour settings, glass and custom icons are backed up too but stay as they are."
  :L"Import MyDockFinder's Dock items and Launchpad order into DockOver?\n\nThe current pinned.txt and launchpad.json are copied aside first (\".before-mydockfinder-<time>\"), so nothing is lost. Apps MyDockFinder does not list stay in Launchpad after its items.";
 if(MessageBoxW(h,ask,L"DockOver Settings",MB_OKCANCEL|(replace?MB_ICONWARNING|MB_DEFBUTTON2:MB_ICONQUESTION))!=IDOK)return;
 wchar_t local[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))return;
 HCURSOR old=SetCursor(LoadCursorW(nullptr,IDC_WAIT));
 docky::mdf::ImportResult r;const bool ok=docky::mdf::Import(dir,std::wstring(local)+L"\\LiquidDock",replace,r);
 SetCursor(old);
 if(HWND dock=FindWindowW(L"LiquidDockV01",nullptr))PostMessageW(dock,WM_APP+80,0,0);  // the Dock reloads pinned.txt
 std::wstring msg=ok?std::wstring(L"Imported.\n\n"):L"The import stopped: "+r.error+L"\n\n";
 msg+=L"Dock items: "+std::to_wstring(r.dockItems)+L"\nLaunchpad apps placed in MyDockFinder's order: "+std::to_wstring(r.launchpadMatched);
 if(!r.dockSkipped.empty()){msg+=L"\n\nNot available in DockOver (skipped):";for(const auto& n:r.dockSkipped)msg+=L"\n  • "+n;}
 if(!r.launchpadUnmatched.empty()){msg+=L"\n\nLaunchpad shortcuts DockOver does not list as applications ("+std::to_wstring(r.launchpadUnmatched.size())+L"):";
  for(size_t i=0;i<r.launchpadUnmatched.size()&&i<12;++i)msg+=L"\n  • "+r.launchpadUnmatched[i];if(r.launchpadUnmatched.size()>12)msg+=L"\n  …";}
 if(replace&&r.hidden>0)msg+=L"\nOther apps removed from Launchpad: "+std::to_wstring(r.hidden);
 if(!r.backupFolder.empty())msg+=L"\n\nEverything before the import: "+r.backupFolder;
 if(!r.dockBackup.empty())msg+=L"\n\nPrevious Dock list: "+r.dockBackup;
 if(!r.launchpadBackup.empty())msg+=L"\nPrevious Launchpad order: "+r.launchpadBackup;
 MessageBoxW(h,msg.c_str(),L"DockOver Settings",MB_OK|(ok?MB_ICONINFORMATION:MB_ICONWARNING));
}
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==docky::vblank::kMsgVBlank){switchTicker.Ack();switchTicker.Stop();InvalidateRect(h,nullptr,FALSE);return 0;}  // the painter re-arms it while switches animate
 switch(m){
 case WM_NCCALCSIZE:
  if(w){if(IsZoomed(h)){auto* p=reinterpret_cast<NCCALCSIZE_PARAMS*>(l);const UINT dpi=GetDpiForWindow(h);const int x=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi),y=GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);InflateRect(&p->rgrc[0],-x,-y);}return 0;}break;
 case WM_NCHITTEST:{
  RECT r{};GetWindowRect(h,&r);const int x=GET_X_LPARAM(l)-r.left,y=GET_Y_LPARAM(l)-r.top,wid=r.right-r.left,hei=r.bottom-r.top;const float k=GetDpiForWindow(h)/96.f;const int border=(std::max)(4,static_cast<int>(7*k));
  if(!IsZoomed(h)){const bool left=x<border,right=x>=wid-border,top=y<border,bottom=y>=hei-border;if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;}
  const float dx=x/k,dy=y/k,nav=liquid::design::settings::SidebarWidth+10.f;if(dy<60.f){if(ChromeButtonAt(dx,dy)>=0||(dx>=nav&&dx<nav+88&&dy>=10&&dy<52))return HTCLIENT;return HTCAPTION;}return HTCLIENT;}
 case WM_MOUSEMOVE:{switchPointerX=GET_X_LPARAM(l)/scale;switchPointerY=GET_Y_LPARAM(l)/scale;
  if(switchPressed>=0){const float delta=switchPointerX-switchDragStartX;const float threshold=GetSystemMetricsForDpi(SM_CXDRAG,GetDpiForWindow(h))/scale;
   if(!switchDragging&&std::fabs(delta)>=threshold)switchDragging=true;
   if(switchDragging)switchDragPosition=std::clamp(switchDragStartPosition+delta/14.4f,0.f,1.f);InvalidateRect(h,nullptr,FALSE);}const int next=ChromeButtonAt(switchPointerX,switchPointerY);if(next!=chromeHover){chromeHover=next;InvalidateRect(h,nullptr,FALSE);}if(!chromeTracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);chromeTracking=true;}break;}
 case WM_MOUSELEAVE:chromeTracking=false;if(chromeHover!=-1){chromeHover=-1;InvalidateRect(h,nullptr,FALSE);}return 0;
 case WM_LBUTTONDOWN:{keyboardFocusVisible=false;const float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;switchPointerX=x;switchPointerY=y;const int button=ChromeButtonAt(x,y);if(button>=0){chromePressed=button;SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}
  const int sw=SettingsSwitchAt(h,x,y);if(sw>=0){SetFocus(h);switchPressed=sw;switchDragging=false;switchDragStartX=x;switchDragStartPosition=SettingsSwitchValue(sw)?1.f:0.f;switchDragPosition=switchDragStartPosition;BeginSwitchPress(h,sw,true);SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}break;}
 case WM_CAPTURECHANGED:if(chromePressed>=0||switchPressed>=0){if(switchPressed>=0){if(switchDragging){auto& anim=switchMotion[switchPressed];const bool value=SettingsSwitchValue(switchPressed);anim.initialized=true;anim.on=value;anim.from=switchDragPosition;anim.to=value?1.f:0.f;anim.start=GetTickCount64();}BeginSwitchPress(h,switchPressed,false);}chromePressed=-1;switchPressed=-1;switchDragging=false;InvalidateRect(h,nullptr,FALSE);}break;
 case WM_CREATE: scale=GetDpiForWindow(h)/96.f;Refresh();RefreshWorkshop();RefreshTaskbarState();RefreshTaskbarStatus();
  try {systemUi=winrt::Windows::UI::ViewManagement::UISettings();} catch(...) {systemUi=nullptr;} RefreshAccessibility();RefreshSystemAppearance();
  searchBox=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(1001),GetModuleHandleW(nullptr),nullptr);
  if(searchBox){originalSearchProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(searchBox,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(SearchProc)));UpdateSearchFont();SendMessageW(searchBox,0x1501,TRUE,reinterpret_cast<LPARAM>(L"Search Settings"));PositionSearch();}
  ApplyNativeAppearance(h);SetTimer(h,1,1000,nullptr);return 0;
 case WM_CTLCOLOREDIT: if(reinterpret_cast<HWND>(l)==searchBox&&searchBrush){SetTextColor(reinterpret_cast<HDC>(w),searchText);SetBkColor(reinterpret_cast<HDC>(w),searchBackground);return reinterpret_cast<LRESULT>(searchBrush);}break;
 case WM_SYSCOLORCHANGE: ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);break;
 case WM_COMMAND: if(LOWORD(w)==1001&&HIWORD(w)==EN_CHANGE){UpdateSearch(h);return 0;}break;
 case WM_SYSKEYDOWN:  // Alt combinations while recording a shortcut; otherwise navigation history
  if(recordingHotkey&&selected==6){RecordHotkeyKey(h,static_cast<UINT>(w));return 0;}
  if(w==VK_LEFT||w==VK_RIGHT){StepHistory(h,w==VK_LEFT?-1:1);return 0;}break;
 case WM_SYSCHAR: if(recordingHotkey)return 0;break;  // no menu beep while recording
 case WM_CHAR: if(recordingHotkey)return 0;break;
 case WM_KEYDOWN:
  keyboardFocusVisible=true;
  if(recordingHotkey&&selected==6){RecordHotkeyKey(h,static_cast<UINT>(w));return 0;}
  if(w=='F'&&(GetKeyState(VK_CONTROL)&0x8000)){if(searchBox)SetFocus(searchBox);return 0;}
  if(visibleCategories.empty())return 0;
  if(w==VK_DOWN||w==VK_UP){MoveCategory(h,w==VK_DOWN?1:-1);return 0;}
  if(w==VK_TAB&&selected==1){const bool back=(GetKeyState(VK_SHIFT)&0x8000)!=0;if((back&&appearanceKeyboard==0)||(!back&&appearanceKeyboard==2)){if(searchBox)SetFocus(searchBox);}else appearanceKeyboard+=back?-1:1;InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==1){SetAppearance(h,appearanceKeyboard);return 0;}
  if(w==VK_TAB && selected==2){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;if((backwards&&dockKeyboardRow==0)||(!backwards&&dockKeyboardRow==4)){if(searchBox)SetFocus(searchBox);}else dockKeyboardRow+=backwards?-1:1;InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==6){ActivateTaskbarControl(h,taskbarKeyboardControl);return 0;}
  if(w==VK_TAB&&selected==6){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;
   if((backwards&&taskbarKeyboardControl==0)||(!backwards&&taskbarKeyboardControl==kTaskbarControls-1)){if(searchBox)SetFocus(searchBox);}else taskbarKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==0){OpenVisualEffects(h);return 0;}
  if(w==VK_TAB&&selected==0){if(searchBox)SetFocus(searchBox);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==2){ToggleDockSetting(h,dockKeyboardRow);return 0;}
  if(w==VK_TAB&&selected==4){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;const int count=density.densityPreset==3?10:4;
   densityKeyboardControl=std::clamp(densityKeyboardControl,0,count-1);
   if((backwards&&densityKeyboardControl==0)||(!backwards&&densityKeyboardControl==count-1)){if(searchBox)SetFocus(searchBox);}else densityKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==4){ActivateDensity(h,densityKeyboardControl);return 0;}
  if(selected==7&&(w==VK_LEFT||w==VK_RIGHT)&&!packs.empty()){
   packSelected=std::clamp(packSelected+(w==VK_LEFT?-1:1),0,static_cast<int>(packs.size())-1);
   if(packSelected<packOffset)packOffset=packSelected;else if(packSelected>=packOffset+3)packOffset=packSelected-2;
   InvalidateRect(h,nullptr,FALSE);return 0;
  }
  if(w==VK_TAB&&selected==7){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;
   if((backwards&&workshopKeyboardControl==0)||(!backwards&&workshopKeyboardControl==6)){if(searchBox)SetFocus(searchBox);}else workshopKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==7){
   namespace lay=liquid::design::settings;RECT client{};GetClientRect(h,&client);const float left=ContentLeftForWidth(client.right/scale),right=ContentRightForWidth(client.right/scale,left),top=lay::CardTop;
   float x=left+24,y=top+182;
   if(workshopKeyboardControl==1)x=left+80;
   if(workshopKeyboardControl>=2){const int index=workshopKeyboardControl-2,row=index<3?0:1,col=index<3?index:index-3;const float bw=(right-left-16)/3.f;x=left+col*(bw+8)+bw*.5f;y=top+228+row*40.f;}
   SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(static_cast<int>(x*scale),static_cast<int>(y*scale)));return 0;
  }
  break;
 case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h,nullptr,FALSE);break;
 case WM_PAINT:{PAINTSTRUCT p{};BeginPaint(h,&p);Draw(h);EndPaint(h,&p);return 0;}
 case WM_ERASEBKGND:return 1;
 case WM_SIZE:
  sidebarFirst=std::clamp(sidebarFirst,0,std::max(0,static_cast<int>(visibleCategories.size())-SidebarCapacity(h)));
  if(gpuReady&&renderer&&w!=SIZE_MINIMIZED&&LOWORD(l)&&HIWORD(l)){
   if(FAILED(renderer->Resize(LOWORD(l),HIWORD(l),96*scale))){DLOG(L"settings: resize failed; opaque fallback");DisableGpu();}
   InvalidateRect(h,nullptr,FALSE);
  }else if(fallbackTarget){fallbackTarget->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));InvalidateRect(h,nullptr,FALSE);}
  UpdateWindowContour(h,static_cast<UINT>(w));
  return 0;
 case WM_MOVE:if(backdrop){backdrop->Request(allowTransparency,MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST));if(allowTransparency)backdrop->ProcessCapture(SidebarScreen(h));InvalidateRect(h,nullptr,FALSE);}return 0;
 case dock::kMsgCaptureAvailable:if(allowTransparency&&backdrop&&backdrop->ProcessCapture(SidebarScreen(h)))InvalidateRect(h,nullptr,FALSE);return 0;
 case WM_GETMINMAXINFO:{auto* p=reinterpret_cast<MINMAXINFO*>(l);p->ptMinTrackSize.x=static_cast<LONG>(liquid::design::settings::MinWidth*scale);p->ptMinTrackSize.y=static_cast<LONG>(liquid::design::settings::MinHeight*scale);
  const HMONITOR monitor=MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};HWND bar=FindWindowW(L"LiquidDesktopMenuBarV01",nullptr);RECT br{};
  if(bar&&IsWindowVisible(bar)&&GetWindowRect(bar,&br)&&GetMonitorInfoW(monitor,&mi)&&MonitorFromRect(&br,MONITOR_DEFAULTTONEAREST)==monitor&&br.top<=mi.rcWork.top&&br.bottom>mi.rcWork.top&&br.bottom<mi.rcWork.bottom){
   const UINT dpi=GetDpiForWindow(h);const int fx=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi),fy=GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
   p->ptMaxPosition.x=mi.rcWork.left-mi.rcMonitor.left-fx;p->ptMaxPosition.y=br.bottom-mi.rcMonitor.top-fy;
   p->ptMaxSize.x=mi.rcWork.right-mi.rcWork.left+2*fx;p->ptMaxSize.y=mi.rcWork.bottom-br.bottom+2*fy;
  }return 0;}
 case WM_DPICHANGED:{scale=HIWORD(w)/96.f;UpdateSearchFont();PositionSearch();auto* r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);if(fallbackTarget)fallbackTarget->SetDpi(96*scale,96*scale);InvalidateRect(h,nullptr,FALSE);return 0;}
 case kMsgUpdate:{
  if(w==1){std::unique_ptr<docky::update::CheckResult> r(reinterpret_cast<docky::update::CheckResult*>(l));
   updState=docky::update::LoadState();updRelease=r->release;updError=r->error;
   updUi=r->status==docky::update::CheckStatus::Available?UpdUi::Available:r->status==docky::update::CheckStatus::UpToDate?UpdUi::UpToDate:r->status==docky::update::CheckStatus::NoRelease?UpdUi::NoRelease:UpdUi::Error;
   DLOG(L"settings: update check status=%d version=%hs",static_cast<int>(r->status),r->release.version.c_str());}
  else if(w==2){std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(l));
   if(err->empty()){updUi=UpdUi::Installing;InvalidateRect(h,nullptr,FALSE);UpdateWindow(h);DLOG(L"settings: update %hs downloaded; installer started, closing Settings",updRelease.version.c_str());PostMessageW(h,WM_CLOSE,0,0);}
   else{updUi=updCancel.load()?UpdUi::Available:UpdUi::Error;updError=*err;DLOG(L"settings: update failed: %ls",err->c_str());}}
  InvalidateRect(h,nullptr,FALSE);return 0;}
 case WM_TIMER:{if(w==4){KillTimer(h,4);InvalidateRect(h,nullptr,FALSE);return 0;}if(RefreshSystemAppearance()&&appearanceMode==0){ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);}if(RefreshAccessibility()){ApplyNativeAppearance(h);if(backdrop)backdrop->Request(allowTransparency,MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST));InvalidateRect(h,nullptr,FALSE);}
  if(w==kRecordTimeout){StopRecording(h,L"Recording timed out; the previous shortcut is kept.");return 0;}
  if(recordingHotkey&&selected!=6)StopRecording(h,nullptr);  // left the page (e.g. via search)
  if(w==2){KillTimer(h,2);taskbarFeedback.clear();}  // short follow-up refresh after a taskbar command
  if(selected==6){const bool a=RefreshTaskbarState(),b=RefreshTaskbarStatus();if(a||b||w==2)InvalidateRect(h,nullptr,FALSE);}
  if(w==2)return 0;
   auto r=liquid::settings::ReadRevision(settingsPath);if(r!=revision){Refresh();ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);}
  {auto wr=liquid::settings::ReadRevision(workshopRoot+L"\\active.txt"),sr=liquid::settings::ReadRevision(workshopRoot+L"\\settings-active.txt");if(wr!=workshopRevision||sr!=settingsWorkshopRevision){RefreshWorkshop();InvalidateRect(h,nullptr,FALSE);}}
  if(backdrop&&backdrop->CheckHealth())InvalidateRect(h,nullptr,FALSE);return 0;}
 case WM_MOUSEWHEEL:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&point);
  if(point.x/scale<liquid::design::settings::SidebarWidth){const int maximum=std::max(0,static_cast<int>(visibleCategories.size())-SidebarCapacity(h));
   sidebarFirst=std::clamp(sidebarFirst+(GET_WHEEL_DELTA_WPARAM(w)<0?1:-1),0,maximum);InvalidateRect(h,nullptr,FALSE);return 0;}
  if(selected==7&&!packs.empty()){const int maximum=std::max(0,static_cast<int>(packs.size())-3);
   packOffset=std::clamp(packOffset+(GET_WHEEL_DELTA_WPARAM(w)<0?1:-1),0,maximum);InvalidateRect(h,nullptr,FALSE);return 0;}break;}
 case WM_LBUTTONUP:{namespace ds=liquid::design;namespace lay=liquid::design::settings;namespace sp=liquid::design::space;
  float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;RECT rc{};GetClientRect(h,&rc);const float contentLeft=ContentLeftForWidth(rc.right/scale),right=ContentRightForWidth(rc.right/scale,contentLeft);
  if(switchPressed>=0){const int pressed=switchPressed;const bool oldValue=SettingsSwitchValue(pressed);const bool change=!switchDragging||((switchDragPosition>=.5f)!=oldValue);if(switchDragging){auto& anim=switchMotion[pressed];anim.initialized=true;anim.on=oldValue;anim.from=switchDragPosition;anim.to=oldValue?1.f:0.f;anim.start=GetTickCount64();}switchPressed=-1;BeginSwitchPress(h,pressed,false);switchDragging=false;if(GetCapture()==h)ReleaseCapture();
   if(change&&SettingsSwitchAt(h,x,y)==pressed){if(pressed==14||pressed==15){if(liquid::settings::WriteBool(settingsPath,"updates",pressed==14?"autoCheck":"autoInstall",!SettingsSwitchValue(pressed))!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==17){std::wstring error;if(!SetLoginItem(!loginItem,error))MessageBoxW(h,error.c_str(),L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==16){if(liquid::settings::WriteBool(settingsPath,"dock","launchDesktop",!launchDesktop)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==13){if(liquid::settings::WriteBool(settingsPath,"launchpad","showWindows",!density.showWindows)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==9){if(liquid::settings::WriteBool(settingsPath,"launchpad","hideLabels",!density.hideLabels)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==12){if(liquid::settings::WriteBool(settingsPath,"dock","downloadsPreview",!dlPreview)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==11){if(liquid::settings::WriteBool(settingsPath,"dock","downloadsVertical",!dlVertical)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed<3||pressed==8||pressed==10){const int row=pressed==8?3:pressed==10?4:pressed;dockKeyboardRow=row;ToggleDockSetting(h,row);}else{taskbarKeyboardControl=pressed;ActivateTaskbarControl(h,pressed);}}InvalidateRect(h,nullptr,FALSE);return 0;}
  if(chromePressed>=0){const int pressed=chromePressed;chromePressed=-1;if(GetCapture()==h)ReleaseCapture();if(ChromeButtonAt(x,y)==pressed){if(pressed==0)PostMessageW(h,WM_CLOSE,0,0);else if(pressed==1)ShowWindow(h,SW_MINIMIZE);else ShowWindow(h,IsZoomed(h)?SW_RESTORE:SW_MAXIMIZE);}InvalidateRect(h,nullptr,FALSE);return 0;}
  const float nav=lay::SidebarWidth+10.f;if(x>=nav&&x<nav+88&&y>=10&&y<52){StepHistory(h,x<nav+44?-1:1);return 0;}
  if(visibleCategories.empty())return 0;
  if(x>=sp::M&&x<lay::SidebarWidth-sp::M&&y>=lay::SidebarTop&&y<lay::SidebarTop+SidebarCapacity(h)*lay::SidebarStep){
   int visualRow=static_cast<int>((y-lay::SidebarTop)/lay::SidebarStep);int row=sidebarFirst+visualRow;
   if(row>=0&&row<static_cast<int>(visibleCategories.size())&&y<lay::SidebarTop+visualRow*lay::SidebarStep+lay::SidebarRow){SelectCategory(h,visibleCategories[row]);InvalidateRect(h,nullptr,FALSE);}}
  else if(selected==1){const float left=contentLeft,top=lay::CardTop,gap=sp::S,bw=(right-left-2*sp::L-2*gap)/3;
   if(y>=top+48&&y<top+80)for(int i=0;i<3;++i){float bx=left+sp::L+i*(bw+gap);if(x>=bx&&x<bx+bw){appearanceKeyboard=i;SetAppearance(h,i);break;}}return 0;
  }
  else if(selected==6){
   for(int i=0;i<kTaskbarControls;++i){const auto r=TaskbarControlRect(i,contentLeft,right);
    if(x>=r.left-4&&x<r.right+4&&y>=r.top-4&&y<r.bottom+4){taskbarKeyboardControl=i;ActivateTaskbarControl(h,i);return 0;}}
   return 0;
  }
  else if(selected==0&&x>=right-158&&x<right-12&&y>=lay::CardTop+20&&y<lay::CardTop+50){
   OpenVisualEffects(h);return 0;
  }
  else if(selected==0&&y>=lay::CardTop+178.f){
   for(int row=0;row<2;++row)for(int i=0;i<5;++i){const auto b=FrameCapButton(row,i,contentLeft,right);
    if(x>=b.left&&x<b.right&&y>=b.top&&y<b.bottom){
     if(liquid::settings::WritePerformanceInt(settingsPath,row?"captureFps":"animationFps",kFrameCaps[i])!=liquid::settings::SaveStatus::Saved)
      MessageBoxW(h,L"Could not save settings. Your previous settings were preserved.",L"DockOver Settings",MB_OK|MB_ICONWARNING);
     Refresh();InvalidateRect(h,nullptr,FALSE);return 0;}}
  }
  else if(selected==9&&y>=UpdateCardTop()+104&&y<UpdateCardTop()+136){
   const auto cb=UpdateCheckButton(contentLeft,UpdateCardTop()),ib=UpdateInstallButton(contentLeft,UpdateCardTop());
   if(x>=cb.left&&x<cb.right){if(updUi==UpdUi::Downloading)updCancel=true;else if(updUi!=UpdUi::Checking&&updUi!=UpdUi::Installing)StartUpdateCheck(h);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(updUi==UpdUi::Available&&x>=ib.left&&x<ib.right){StartUpdateDownload(h);InvalidateRect(h,nullptr,FALSE);return 0;}
   return 0;
  }
  else if(selected==8&&x>=contentLeft+sp::L&&x<contentLeft+204&&y>=lay::CardTop+166+104&&y<lay::CardTop+166+136){
   ImportMyDockFinder(h,false);return 0;
  }
  else if(selected==8&&x>=contentLeft+216&&x<contentLeft+404&&y>=lay::CardTop+166+104&&y<lay::CardTop+166+136){
   ImportMyDockFinder(h,true);return 0;
  }
  else if(selected==8&&x>=contentLeft+sp::L&&x<contentLeft+204&&y>=lay::CardTop+104&&y<lay::CardTop+136){
   const auto file=PickLegacyFile(h);if(!file.empty()){
    const auto result=liquid::settings::BackupLegacy(file);
    MessageBoxW(h,result==liquid::settings::BackupStatus::Created?L"Backup created beside the selected file. The original was not changed.":result==liquid::settings::BackupStatus::AlreadyExists?L"A backup already exists and was not replaced.":L"Could not create a backup. The original was not changed.",L"DockOver Migration",MB_OK|(result==liquid::settings::BackupStatus::Unavailable?MB_ICONWARNING:MB_ICONINFORMATION));
   }return 0;
  }
  else if(selected==7){
   const float left=contentLeft,top=lay::CardTop,gap=8,bw=(right-left-2*gap)/3.f;std::wstring error;bool action=false,ok=true;
   if(x>=left&&x<right&&y>=top+4&&y<top+148){const int index=packOffset+static_cast<int>((y-top-4)/48);if(index>=0&&index<static_cast<int>(packs.size()))packSelected=index;}
   else if(y>=top+168&&y<top+196){if(x>=left&&x<left+48)packOffset=std::max(0,packOffset-3);else if(x>=left+56&&x<left+104)packOffset=std::min(std::max(0,static_cast<int>(packs.size())-3),packOffset+3);}
   else if(y>=top+212&&y<top+284){const int row=y>=top+252?1:0;const float by=top+212+row*40.f;if(y<by+32){int col=static_cast<int>((x-left)/(bw+gap));const float bx=left+col*(bw+gap);if(col>=0&&col<3&&x>=bx&&x<bx+bw){const int index=row?col+3:col;
    if(index==0){auto file=PickPackFile(h,false);if(!file.empty()){docky::workshop::Pack pack;action=true;ok=docky::workshop::Install(workshopRoot,file,pack,error);}}
    else if(index==4){action=true;const bool settings=packSelected>=0&&packSelected<static_cast<int>(packs.size())&&SettingsOnly(packs[packSelected]);ok=settings?docky::workshop::SelectSettings(workshopRoot,"",error):docky::workshop::Select(workshopRoot,"",error);}
    else if(packSelected>=0&&packSelected<static_cast<int>(packs.size())){const auto id=packs[packSelected].id;
     if(index==1&&packs[packSelected].valid){auto file=PickPackFile(h,true);if(!file.empty()){action=true;ok=docky::workshop::Export(workshopRoot,id,file,error);}}
     else if(index==2){if(MessageBoxW(h,L"Delete the selected installed resource pack?",L"DockOver Workshop",MB_YESNO|MB_ICONQUESTION)==IDYES){action=true;ok=docky::workshop::Remove(workshopRoot,id,error);}}
     else if(index==3&&packs[packSelected].valid){action=true;const bool settings=SettingsOnly(packs[packSelected]);ok=settings?docky::workshop::SelectSettings(workshopRoot,id==activeSettingsPack?std::string{}:id,error):docky::workshop::Select(workshopRoot,id==activePack?std::string{}:id,error);}
    }
   }}}
   if(action){if(!ok)MessageBoxW(h,error.empty()?L"Workshop operation failed.":error.c_str(),L"DockOver Workshop",MB_OK|MB_ICONWARNING);RefreshWorkshop();}InvalidateRect(h,nullptr,FALSE);
  }
  else if(selected==2){const float y0=DownloadsCardTop()+sp::L;
   if(y>=y0&&y<y0+28){if(x>=right-126&&x<right-98)SetDownloadsCount(h,dlCount-1);else if(x>=right-48&&x<right-20)SetDownloadsCount(h,dlCount+1);}
  }
  else if(selected==5){const float y0=lay::CardTop+sp::L;
   if(y>=y0&&y<y0+28){if(x>=right-126&&x<right-98)SetCcBlur(h,ccBlur-10);else if(x>=right-48&&x<right-20)SetCcBlur(h,ccBlur+10);}
  }
  else if(selected==4){
   const float left=contentLeft,top=lay::CardTop,gap=sp::S,button=(right-left-2*sp::L-3*gap)/4.f;
   const int modes[]={1,0,2,3};const char* key=nullptr;int value=0;
   if(y>=top+sp::L&&y<top+sp::L+32){for(int i=0;i<4;++i){float bx=left+sp::L+i*(button+gap);if(x>=bx&&x<bx+button){key="densityPreset";value=modes[i];break;}}}
   if(!key&&density.densityPreset==3){const char* keys[]={"iconSize","gapX","gapY"};const int vals[]={density.iconSize,density.gapX,density.gapY};
    for(int i=0;i<3;++i){float by=top+60.f+i*36.f;if(y>=by&&y<by+28){int delta=x>=right-126&&x<right-98?-8:x>=right-48&&x<right-20?8:0;
      if(delta){key=keys[i];value=std::clamp(vals[i]+delta,i==0?32:0,i==0?160:240);}break;}}}
   if(key)SetDensity(h,key,value);
  }
  else if(selected==2&&x>=right-lay::SwitchRight-lay::SwitchWidth-sp::S&&x<=right-lay::SwitchRight+sp::S
          &&y>=lay::SwitchTop-sp::S&&y<=lay::SwitchTop+4*lay::CardHeight+lay::SwitchHeight+sp::S
          &&std::fmod(y-(lay::SwitchTop-sp::S),lay::CardHeight)<=lay::SwitchHeight+2*sp::S){
   const int row=static_cast<int>((y-(lay::SwitchTop-sp::S))/lay::CardHeight);
   dockKeyboardRow=row;ToggleDockSetting(h,row);
  }return 0;}
 case WM_ACTIVATE: if(LOWORD(w)==WA_INACTIVE&&recordingHotkey)StopRecording(h,L"Recording stopped (window inactive); the previous shortcut is kept.");InvalidateRect(h,nullptr,FALSE);break;
 case WM_DESTROY:StopRecording(h,nullptr);KillTimer(h,1);systemUi=nullptr;if(searchFont){DeleteObject(searchFont);searchFont=nullptr;}if(searchBrush){DeleteObject(searchBrush);searchBrush=nullptr;}
  if(renderer)DLOG(L"settings: present calls=%llu",static_cast<unsigned long long>(renderer->PresentCalls()));
  DisableGpu();fallbackTarget.Reset();PostQuitMessage(0);return 0;
 }return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
 (void)buildMarker[0];SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 const HRESULT ro=RoInitialize(RO_INIT_SINGLETHREADED);
 settingsPath=liquid::settings::DefaultPath();workshopRoot=docky::workshop::DefaultRoot();bool devWorkshop=false,devOpaque=false,explicitWorkshopRoot=false,explicitSettingsPath=false,devPress=false;int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 if(argv){for(int i=1;i<argc;++i){const std::wstring a=argv[i];if(a==L"--settings-path"&&i+1<argc){settingsPath=argv[++i];explicitSettingsPath=true;}
  else if(a==L"--workshop-root"&&i+1<argc){workshopRoot=argv[++i];explicitWorkshopRoot=true;}else if(a==L"--dev-workshop")devWorkshop=true;else if(a==L"--dev-press-switch")devPress=true;else if(a==L"--dev-opaque")devOpaque=true;else if(a==L"--snapshot"&&i+1<argc)snapshotPath=argv[++i];else if(a==L"--log"&&i+1<argc)dock::Log::Get().Open(argv[++i]);else if(a==L"--page"&&i+1<argc&&std::wstring(argv[++i])==L"about"){selected=9;categoryHistory={9};categoryHistoryIndex=0;}}LocalFree(argv);}

 updState=docky::update::LoadState();
 {wchar_t local[MAX_PATH]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(local)+L"\\LiquidDock\\glass.json");}  // Sliders & switches lens
 if(devOpaque&&!explicitWorkshopRoot)return 2;
 if(devPress&&(!explicitSettingsPath||!explicitWorkshopRoot||devOpaque))return 2;
 if(devPress){selected=2;switchPressed=1;visualSwitch=1;pressFrom=pressTo=1.f;pressStart=GetTickCount64();switchPointerX=824.f;switchPointerY=190.f;}
 if(devWorkshop){selected=7;categoryHistory={7};categoryHistoryIndex=0;RefreshWorkshop();}
 if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))||FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()))))return 1;
 auto font=[&](float size,DWRITE_FONT_WEIGHT weight,ComPtr<IDWriteTextFormat>& out){return writeFactory->CreateTextFormat(liquid::design::type::Family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",&out);};
 if(FAILED(font(16.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,normal))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,fontSmall))||FAILED(font(22.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,heading))||FAILED(font(liquid::design::type::Caption,DWRITE_FONT_WEIGHT_SEMI_BOLD,caption)))return 1;
 if(FAILED(font(16.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,sidebarLabel))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,centerSmall))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,trailingSmall)))return 1;
 sidebarLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
 centerSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);centerSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
 trailingSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);trailingSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
 if(SUCCEEDED(writeFactory->CreateTextFormat(L"Segoe Fluent Icons",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,15.f,L"en-us",&categoryIcons))){categoryIcons->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);categoryIcons->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}
 WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=ClassName;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 if(!RegisterClassW(&wc))return 1;
 HWND h=CreateWindowExW(0,ClassName,L"DockOver Settings",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,900,760,nullptr,nullptr,instance,nullptr);
 if(!h)return 1;
 SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
 RECT client{};GetClientRect(h,&client);
 // The window remains opaque and capturable. GPU composition is used only to
 // sample the window's own pixels for a pressed switch; never start WGC here.
 if(!devOpaque){renderer=std::make_unique<dock::Renderer>();if(SUCCEEDED(renderer->Init(h,client.right,client.bottom,96*scale,false))){gpuReady=true;}
  else{renderer->Shutdown();renderer.reset();DLOG(L"settings: GPU unavailable; opaque control fallback");}}
 UpdateWindowContour(h,SIZE_RESTORED);
 DLOG(L"settings: opaque themed window; desktop capture disabled; local GPU=%d",gpuReady);
 // DWM rounds the corners (antialiased); its 1 px border colour is turned off so only the rounded shape remains.
 DWORD corner=2;DwmSetWindowAttribute(h,33,&corner,sizeof(corner));  // DWMWCP_ROUND: antialiased system corners
 const COLORREF noBorder=0xFFFFFFFE;DwmSetWindowAttribute(h,34,&noBorder,sizeof(noBorder));
 ShowWindow(h,SW_SHOWNORMAL);UpdateWindow(h);
 MSG msg{};
 for(;;){
  HANDLE ev=backdrop?backdrop->CompletionEvent():nullptr;
  DWORD wait=MsgWaitForMultipleObjectsEx(ev?1:0,ev?&ev:nullptr,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  if(ev&&wait==WAIT_OBJECT_0){if(backdrop&&IsWindow(h)&&backdrop->CompleteCompare(SidebarScreen(h)))InvalidateRect(h,nullptr,FALSE);continue;}
  bool quit=false;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}
  if(quit)break;
 }
 if(SUCCEEDED(ro))RoUninitialize();return 0;
}
