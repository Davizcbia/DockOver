#include "desktop_settings.h"
#include "ui_language.h"
#include "mydockfinder_import.h"
#include "settings_search.h"
#include "settings_motion.h"
#include "lab_slider.h"
#include "design_tokens.h"
#include "theme_state.h"
#include "launchpad_density.h"
#include "renderer.h"
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
#include "game_compat.h"
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
constexpr wchar_t MusicClassName[]=L"DockOverMusicSettings";
bool musicOnly=false;
constexpr const wchar_t* titles[]={L"General",L"Appearance",L"Dock",L"Menu Bar",L"Launchpad",L"Control Center",L"Windows Taskbar",L"Workshop",L"Migration",L"About",L"Music"};
ComPtr<ID2D1Factory> factory; ComPtr<ID2D1HwndRenderTarget> fallbackTarget;
ID2D1RenderTarget* target=nullptr; ComPtr<ID2D1SolidColorBrush> brush;
std::unique_ptr<dock::Renderer> renderer;
std::wstring snapshotPath,referenceCaptureDir,glassConfigPath;int referenceCaptureFrame=0;bool referenceInteractive=false,referenceLayoutOnly=false;int referenceWidth=900,referenceHeight=560;
HWND settingsWindowHandle=nullptr;
bool gpuReady=false;
winrt::Windows::UI::ViewManagement::UISettings systemUi{nullptr};
bool allowTransparency=false;
int appearanceMode=0,appearanceKeyboard=0,applicationsSort=0; bool systemDark=false;
bool RefreshSystemAppearance(){
 bool dark=false; try { if(systemUi){ auto c=systemUi.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background); dark=(int(c.R)*299+int(c.G)*587+int(c.B)*114)<128000; }} catch(...) {}
 bool changed=dark!=systemDark;systemDark=dark;return changed;
}
liquid::design::Palette SettingsPalette(){
 HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
 auto palette=liquid::theme::Palette(appearanceMode,systemDark,contrast);
 if(!contrast&&liquid::theme::IsDark(appearanceMode,systemDark)){palette.background=0x262524;
  palette.navigation=0x242323;
  palette.surface=0x2D2C2B;
  palette.primary=0xF2F1F0;
  palette.secondary=0xBAB8B7;
  palette.selected=0x43413F;
  palette.accent=0x5299FF;}
 if(!contrast&&!liquid::theme::IsDark(appearanceMode,systemDark)){palette.background=0xFFFFFF;palette.navigation=0xEEEEEF;palette.surface=0xF7F7F8;palette.primary=0x202022;palette.secondary=0x626265;}
 return palette;
}
unsigned SearchSurface(){HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);return contrast?SettingsPalette().surface:liquid::theme::IsDark(appearanceMode,systemDark)?0x48484A:0xDFDFE2;}
bool RefreshAccessibility(){
 bool allow=false;HIGHCONTRASTW hc{sizeof(hc)};
 const bool contrastKnown=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)!=FALSE;
 try {allow=systemUi && systemUi.AdvancedEffectsEnabled() && contrastKnown && !(hc.dwFlags&HCF_HIGHCONTRASTON);} catch(...) {allow=false;}
 const bool changed=allowTransparency!=allow;if(changed)DLOG(L"settings: system transparency allowed=%d",allow);allowTransparency=allow;return changed;
}

ComPtr<IDWriteFactory> writeFactory; ComPtr<IDWriteTextFormat> normal,fontSmall,heading,caption,categoryIcons,sidebarLabel,sidebarSelected,centerSmall,trailingSmall;
constexpr float SettingsWindowRadius=24.f,SettingsCardRadius=12.f;
int menuScalePercent=100,settingsUiPercent=90,applicationsUiPercent=80,activeSizeSlider=0,menuKeyboardControl=0,menuDragStart=100;bool menuSliderDragging=false;float menuSliderGrab=0;
int selected=0, dockKeyboardRow=0, densityKeyboardControl=0, workshopKeyboardControl=0;
dock::LocalGlassOverlay* drawingLocalGlass=nullptr;
float switchPointerX=0.f,switchPointerY=0.f;
struct SwitchMotion {bool initialized=false,on=false;float from=0,to=0;ULONGLONG start=0;};
SwitchMotion switchMotion[64];  // indexed by switch id (Control Center uses up to 33; sliders 40+)
int visualSwitch=-1;float pressFrom=0,pressTo=0;ULONGLONG pressStart=0;
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
D2D1_RECT_F UpdateCheckButton(float right, float top) { return D2D1::RectF(right-188,top+22,right-16,top+54); }
D2D1_RECT_F UpdateInstallButton(float right, float top) { return D2D1::RectF(right-188,top+70,right-16,top+102); }
float UpdateCardTop() { return liquid::design::settings::CardTop + 166.f; }
float UpdateSwitchCardTop(int row);
D2D1_RECT_F UpdateSwitch(float right,int row);
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
D2D1_RECT_F FrameCapButton(int row,int i,float left,float right){const float width=(right-left-32-4*8)/5,top=liquid::design::settings::CardTop+178+62+row*64;const float x=left+16+i*(width+8);return D2D1::RectF(x,top,x+width,top+28);}
void BeginSwitchPress(HWND h,int id,bool down){const auto now=GetTickCount64();const float current=visualSwitch==id?PressAmount(now):0.f;visualSwitch=id;pressFrom=down?current:std::max(current,.55f);pressTo=down?1.f:0.f;pressStart=now;SetTimer(h,4,4,nullptr);}

int recordingShortcut=-1;int shortcutValues[4]={0x34C,0x34D,0x344,0x34E};int chromeHover=-1,chromePressed=-1;bool chromeTracking=false;bool keyboardFocusVisible=false;int switchPressed=-1; float scale=1; bool diagnostics=false, iconMask=true, autoHide=false, hideSnap=false, genie=false, gameCompatibility=false, lowLatencyGlass=false, menuBarOn=true, menuBarThemeGradient=true, dlVertical=false, dlPreview=true, launchDesktop=false, loginItem=false; liquid::settings::Launchpad density; bool mediaArtworkBackground=false, musicWallpaper=false, wallpaperAvoidBars=true, wallpaperLyrics=true, lyricsFullArtwork=true, lyricsAutoReturn=true, lyricsBeat=true, shortcutsLoaded=false; int lyricsOffsetMs=300; bool animatedArtwork=true, lyricsDoubleClickSeek=true, keepArtworkWhenPaused=false, localMusicTags=true, hideIconsWithWidgets=true, captureNotifications=false, hideSystemNotifications=true; int ncBlur=20, ncFade=100, ncShade=30, gradientOpacity=90, gradientLength=100; int ccBlur=60, dlCount=6, dockMagnification=126; std::wstring settingsPath;
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
HWND searchBox=nullptr,searchHost=nullptr; WNDPROC originalSearchProc=nullptr;
void PositionSearch();
void FocusSearch(){if(searchBox&&searchHost){PositionSearch();ShowWindow(searchHost,SW_SHOWNOACTIVATE);ShowWindow(searchBox,SW_SHOW);SetFocus(searchBox);InvalidateRect(GetWindow(searchHost,GW_OWNER),nullptr,FALSE);}}
HBRUSH searchBrush=nullptr;HFONT searchFont=nullptr; COLORREF searchText=RGB(0,0,0),searchBackground=RGB(255,255,255);
COLORREF ColorRef(unsigned rgb){return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);}
void ApplyNativeAppearance(HWND h){
 const auto colors=SettingsPalette();const COLORREF bg=ColorRef(colors.navigation),fg=ColorRef(colors.primary);
 HBRUSH next=CreateSolidBrush(bg);if(next){HBRUSH previous=searchBrush;searchBrush=next;searchBackground=bg;searchText=fg;if(previous)DeleteObject(previous);}
 const unsigned c=colors.background;const BOOL dark=(((c>>16)&255)*299+((c>>8)&255)*587+(c&255)*114)<128000;
 DwmSetWindowAttribute(h,20,&dark,sizeof(dark));
 if(searchBox)InvalidateRect(searchBox,nullptr,TRUE);
} std::vector<int> visibleCategories=liquid::settings::SearchCategories(L""); int sidebarFirst=0;
std::vector<int> categoryHistory{0};size_t categoryHistoryIndex=0;
void StopRecording(HWND h,const wchar_t* feedback);
std::map<std::string,std::wstring> settingsPackIcons;
liquid::settings::ScrollMotion sidebarScroll,contentScroll[11];
std::vector<dock::LocalGlassOverlay>* drawingChromeGlass=nullptr;
ULONGLONG lastScrollTick=0,scrollActiveUntil=0;
float bodyShift=0.f;
constexpr UINT kScrollTimer=37;
float BodyShift(){return -contentScroll[selected].Position();}
bool ReducedMotion(){BOOL animations=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);return !animations;}
float SettingsContentBottom();
void UpdateScrollBounds(HWND h){RECT r{};GetClientRect(h,&r);const float height=r.bottom/scale;
 sidebarScroll.Bounds(liquid::design::settings::SidebarTop+visibleCategories.size()*liquid::design::settings::SidebarStep+12-height);
 contentScroll[selected].Bounds(SettingsContentBottom()+16-height);
}
void StartScroll(HWND h){const auto now=GetTickCount64();if(!lastScrollTick||now-lastScrollTick>100)lastScrollTick=now;scrollActiveUntil=now+1000;SetTimer(h,kScrollTimer,16,nullptr);switchTicker.Start(h,docky::vblank::kMsgVBlank);InvalidateRect(h,nullptr,FALSE);}
void EnsureSidebarSelection(HWND h){UpdateScrollBounds(h);auto it=std::find(visibleCategories.begin(),visibleCategories.end(),selected);if(it!=visibleCategories.end()){
 const float y=liquid::design::settings::SidebarTop+(it-visibleCategories.begin())*liquid::design::settings::SidebarStep;
 RECT r{};GetClientRect(h,&r);sidebarScroll.Ensure(y,y+liquid::design::settings::SidebarRow,112,r.bottom/scale-12);StartScroll(h);}}


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


float ContentLeftForWidth(float width){namespace lay=liquid::design::settings;return lay::SidebarWidth+24.f+(std::max)(0.f,(width-lay::SidebarWidth-48.f-760.f)*.5f);}
float ContentRightForWidth(float width,float left){return (std::min)(width-24.f,left+760.f);}
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
 const ULONGLONG now=GetTickCount64();static SwitchMotion spare;auto& anim=index>=0&&index<int(std::size(switchMotion))?switchMotion[index]:spare;
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
  drawingLocalGlass->active=true;drawingLocalGlass->plate=D2D1::RectF(knob.left,knob.top+bodyShift,knob.right,knob.bottom+bodyShift);drawingLocalGlass->sampleBounds=D2D1::RectF(track.left,track.top+bodyShift,track.right,track.bottom+bodyShift);drawingLocalGlass->params=dock::ControlLensGlass(kh);
  drawingLocalGlass->params.dark=liquid::theme::IsDark(appearanceMode,systemDark);drawingLocalGlass->controlScale=1.f;
  drawingLocalGlass->pointerX=switchPointerX*scale;drawingLocalGlass->pointerY=switchPointerY*scale;localGlassWhite=1.f-press;
  return;
 }
 brush->SetColor(C(contrast?colors.primary:0xFFFFFF,pressed&&!contrast?.82f:1.f));target->FillRoundedRectangle(D2D1::RoundedRect(knob,kh*.5f,kh*.5f),brush.Get());
 if(!contrast){brush->SetColor(C(0x000000,.12f));target->DrawRoundedRectangle(D2D1::RoundedRect(knob,kh*.5f,kh*.5f),brush.Get(),.7f);}
}
void Text(const wchar_t* text,IDWriteTextFormat* fmt,D2D1_RECT_F r,unsigned color){text=docky::ui::Text(text);brush->SetColor(C(color));target->DrawTextW(text,static_cast<UINT32>(wcslen(text)),fmt,r,brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);}
struct FormRow {float top=0,bottom=0;D2D1_RECT_F title{},detail{},control{};};
float DetailHeight(const wchar_t* text,float width){
 if(!text||!*text)return 0;
 static std::map<std::pair<std::wstring,int>,float> cache;
 width=std::max(60.f,width);if(cache.size()>2048)cache.clear();const auto key=std::make_pair(std::wstring(text),static_cast<int>(std::lround(width*100)));
 auto found=cache.find(key);if(found!=cache.end())return found->second;
 ComPtr<IDWriteTextLayout> layout;DWRITE_TEXT_METRICS metrics{};
 if(!writeFactory||!fontSmall||FAILED(writeFactory->CreateTextLayout(text,static_cast<UINT32>(wcslen(text)),fontSmall.Get(),width,4096,&layout))||FAILED(layout->GetMetrics(&metrics)))return 36;
 return cache.emplace(key,std::ceil(metrics.height)).first->second;
}
FormRow MeasureFormRow(float top,float left,float right,const wchar_t* detail,float controlWidth=48){
 const float textRight=right-20-controlWidth-20,detailHeight=DetailHeight(detail,textRight-left-16);
 const float contentHeight=22+(detailHeight>0?3+detailHeight:0),height=std::max(64.f,contentHeight+24);
 const float textTop=top+(height-contentHeight)*.5f,cy=top+height*.5f;
 return {top,top+height,D2D1::RectF(left+16,textTop,textRight,textTop+22),D2D1::RectF(left+16,textTop+25,textRight,textTop+25+detailHeight),D2D1::RectF(right-20-controlWidth,cy-14,right-20,cy+14)};
}
void DrawFormText(const FormRow& row,const wchar_t* title,const wchar_t* detail,const liquid::design::Palette& colors){Text(title,normal.Get(),row.title,colors.primary);Text(detail,fontSmall.Get(),row.detail,colors.secondary);}
struct DockRowSpec{const wchar_t* title;const wchar_t* detail;int id;};
const DockRowSpec dockRows[]={
 {L"macOS icon mask",L"Use rounded-square tiles for app icons.",0},
 {L"Automatically hide and show the Dock",L"Reveal the Dock by moving the pointer to the screen edge.",1},
 {L"Show Dock diagnostics",L"Applies when the Dock restarts.",2},
 {L"Hide snap layouts while dragging windows",L"Hide Windows snap layouts while dragging a window.",8},
 {L"Genie minimize animation",L"Animate windows into and out of the Dock when minimizing or restoring.",10},
 {L"Files shown",L"Show 3–10 recent files. Open Downloads to see everything.",-1},
 {L"Straight fan",L"Arrange the Downloads fan in a straight line.",11},
 {L"File previews",L"Show thumbnails for supported files instead of file-type icons.",12}};
struct DockForm {std::array<FormRow,8> rows;};
DockForm MeasureDock(float left,float right){DockForm result;float top=liquid::design::settings::CardTop+146+12;  // below the Magnification card
 for(int i=0;i<8;++i){if(i==5)top+=40;result.rows[i]=MeasureFormRow(top,left,right,dockRows[i].detail,i==5?106:48);top=result.rows[i].bottom;}return result;}
std::pair<float,float> CurrentContentEdges(){RECT rc{};GetClientRect(settingsWindowHandle,&rc);const float left=ContentLeftForWidth(rc.right/scale);return {left,ContentRightForWidth(rc.right/scale,left)};}
DockForm CurrentDockForm(){const auto [left,right]=CurrentContentEdges();return MeasureDock(left,right);}
struct FormSpec{const wchar_t* title;const wchar_t* detail;};
const wchar_t* TaskbarHotkeyDetail();
FormSpec SpecFor(int id){switch(id){
 case 3:return {L"Hide Windows taskbar on startup",L"Applies when you start DockOver."};
 case 4:return {L"Free work area when taskbar is hidden",L"Let maximized windows use the hidden taskbar's space."};
 case 5:return {L"Toggle taskbar hotkey",TaskbarHotkeyDetail()};
 case 9:return {L"Hide app names",L"Show icons without app names."};
 case 13:return {L"Show windows behind Launchpad",L"Show open windows behind Launchpad. Off uses the live wallpaper only."};
 case 14:return {L"Check for updates automatically",L"Check once a day while DockOver is running."};
 case 15:return {L"Install updates automatically",L"Download updates and restart DockOver automatically."};
 case 16:return {L"Start DockOver Desktop with DockOver",L"Start widgets and Stacks together with the Dock."};
 case 17:return {L"Open DockOver at login",L"Launch the Dock and Menu Bar when you sign in."};
 case 18:return {L"Game compatibility",L"Hide the Dock and Menu Bar and stop desktop capture while a full-screen game, or a program from the games list, is running."};
 case 26:return {L"Low latency glass",L"The Dock's glass shows the desktop one display frame sooner (DXGI capture). Uses a little more CPU while the background moves. Off by default."};
 case 27:return {L"Games list",L"Programs that pause DockOver while they run, one .exe name per line. Applies within 2 seconds."};
 case 46:return {L"Menu bar gradient",L"Use a black or white gradient and match icon colour to Appearance. Turn off to follow the wallpaper."};
 case 25:return {L"Hide Windows notification banners",L"While Capture notifications is on, only DockOver's banners appear. Notifications still go to the Windows notification list."};
 case 24:return {L"Capture notifications",L"Show Windows notifications in Notification Center (click the clock in the menu bar) and as banners at the top right. Windows may ask you to allow notification access."};
 case 20:return {L"Show menu bar",L"Off closes the menu bar completely. Control Center, Wi-Fi, Bluetooth and other status items stay in the Dock's status group."};
 case 28:return {L"Music wallpaper",L"While music plays in Apple Music, Spotify, NetEase Cloud Music, QQ Music, YouTube Music or another music app, the desktop background shows the album artwork, moving when the album has motion artwork. Your wallpaper returns when the music stops."};
 case 30:return {L"Custom lyrics",L"Put .lrc or .txt files named \"Artist - Title\" (or just \"Title\") in this folder. They are used before online lyrics. In full-screen Now Playing, press L or click Lyrics to show them."};
 case 31:return {L"Lyrics on the desktop",L"The music wallpaper also shows the lyrics of the playing song, from your files or the online lyrics services."};
 case 32:return {L"Keep the borderless artwork with lyrics",L"With lyrics, the full-screen artwork moves to the left and the lyrics appear on the right. Off: the artwork is shown as a card on a blurred background."};
 case 34:return {L"Shortcut: lyrics on the desktop",L"Turns the music wallpaper's lyrics on or off."};
 case 35:return {L"Shortcut: music wallpaper",L"Turns the album artwork wallpaper on or off."};
 case 36:return {L"Shortcut: hide DockOver Desktop",L"Hides the widgets and Stacks, or shows them again."};
 case 37:return {L"Shortcut: full-screen Now Playing",L"Opens or closes the full-screen view. Click a shortcut to record a new one (Esc cancels, Backspace removes it)."};
 case 44:return {L"Animated album covers",L"Play motion artwork when available in Now Playing and the music wallpaper."};
 case 45:return {L"Double-click lyrics to seek",L"Double-click a timed lyric to jump to that line in the song."};
 case 50:return {L"Keep artwork when paused",L"Paused music keeps its album cover and lyrics on screen. When off, they fade out a few seconds after pausing."};
 case 51:return {L"Match local music files",L"For a song file such as an MP3, read its title and artist (tags or file name) to find the cover and lyrics. Uses the file's own cover and an .lrc file next to it when there is one."};
 case 52:return {L"Hide desktop icons too",L"The Hide DockOver Desktop shortcut also hides the Windows desktop icons, and shows them again with the widgets."};
 case 38:return {L"Flow with the music",L"Behind the lyrics, the album artwork's colours flow and move with the music, faster when it is loud and swelling on the beat. The music is read from what your speakers play; nothing is recorded or sent."};
 case 39:return {L"Lyrics timing",L"Shows the lyrics earlier (+) or later (-) when they run behind or ahead of the music. In full-screen Now Playing, [ and ] adjust it while you listen."};
 case 53:return {L"Music response",L"Controls how quickly the visuals react to music."};
 case 40:return {L"Frame rate",L"Limits how often the music wallpaper and full-screen Now Playing draw while the background flows. Lower uses less GPU."};
 case 33:return {L"Return to the current line after scrolling",L"In full-screen Now Playing, lyrics you scroll with the mouse wheel or by dragging go back to the line being sung after a second. Off: they stay where you left them."};
 case 29:return {L"Keep artwork clear of the Dock and menu bar",L"The album artwork is fitted between the menu bar and the Dock. The blurred artwork still fills the area behind them."};
 default:return {L"Album artwork background",L"Use album artwork behind the music card."};}}
float TaskbarPreferencesTop();
float UpdateCardBottom();
float DensityCardHeight(){return density.densityPreset==3?220.f:138.f;}
FormRow FormFor(int id,float left,float right);
// Menu Bar page: Notification Center blur strength and fade width, one card with two stepper rows below the notification switches.
constexpr const wchar_t* kNcBlurDetail=L"How strongly the desktop behind Notification Center is blurred. 0% turns it off.";
constexpr const wchar_t* kNcFadeDetail=L"How far the blur fades out toward the left of Notification Center.";
constexpr const wchar_t* kNcShadeDetail=L"How much the blurred background behind Notification Center is darkened. 0% keeps its colours.";
struct SizeSliderSpec {int lo,hi;float top;const wchar_t* title;const wchar_t* detail;const wchar_t* unit=L"%";};
SizeSliderSpec SizeSpec(int index);
constexpr float MenuSizeRowHeight=72.f;
FormRow CcBlurRow(float left,float right){return MeasureFormRow(80,left,right,L"Adjust how strongly the desktop behind Control Center is blurred. 0% turns it off.",106);}
FormRow FormFor(int id,float left,float right){float top=80;switch(id){
 case 3:case 4:case 5:top=TaskbarPreferencesTop();for(int i=3;i<id;++i)top=MeasureFormRow(top,left,right,SpecFor(i).detail).bottom;break;
 case 9:top=80+DensityCardHeight()+12;break;
 case 13:top=FormFor(9,left,right).bottom+12;break;
 case 14:top=UpdateCardBottom()+12;break;
 case 15:top=FormFor(14,left,right).bottom;break;
 case 16:top=80+178+230+12;break;
 case 17:top=FormFor(16,left,right).bottom+12;break;
 case 46:top=80+MenuSizeRowHeight+12;break;
 case 18:top=FormFor(46,left,right).bottom+(menuBarThemeGradient?328:12);break;
 case 27:top=FormFor(18,left,right).bottom;break;
 case 20:top=FormFor(27,left,right).bottom+12;break;
 case 26:top=FormFor(17,left,right).bottom+12;break;
 case 24:top=FormFor(20,left,right).bottom+12;break;
 case 25:top=FormFor(24,left,right).bottom;break;
 case 19:top=CcBlurRow(left,right).bottom+12;break;
 case 44:top=80;break;
 case 28:top=FormFor(44,left,right).bottom;break;
 case 52:top=FormFor(37,left,right).bottom;break;
 case 40:top=FormFor(52,left,right).bottom+12;break;
 case 53:top=FormFor(40,left,right).bottom+12;break;
 case 29:top=FormFor(28,left,right).bottom;break;
 case 31:top=FormFor(29,left,right).bottom;break;
 case 50:top=FormFor(31,left,right).bottom;break;
 case 51:top=FormFor(50,left,right).bottom;break;
 case 32:top=FormFor(51,left,right).bottom+12;break;
 case 33:top=FormFor(32,left,right).bottom;break;
 case 45:top=FormFor(33,left,right).bottom;break;
 case 38:top=FormFor(45,left,right).bottom;break;
 case 39:top=FormFor(38,left,right).bottom;break;
 case 30:top=FormFor(39,left,right).bottom;break;
 case 34:top=FormFor(30,left,right).bottom+12;break;
 case 35:case 36:case 37:top=FormFor(id-1,left,right).bottom;break;
 }return MeasureFormRow(top,left,right,SpecFor(id).detail,id==27||id==30?124.f:id>=34&&id<=37?170.f:id==39?136.f:id==40?330.f:id==53?254.f:48.f);}
// Menu Bar page, Games list row: opens game-compat.txt (created with instructions when missing) in the default editor.
// Control Center page, shortcut rows (34..37): a button shows the combination; clicking it records the next one.
// Stored in controlCenter as (MOD_* << 8) | virtual key; the Manager registers them.
void Refresh();
// Music page, Frame rate row (40): 30 / 60 / 120 / 240 / Unlimited (0).
const int kMusicFps[5]={30,60,120,240,0};int musicFps=60, musicResponse=1, musicSensitivity=50;
const wchar_t* const kMusicResponses[]={L"Instant",L"Balanced",L"Smooth"};
D2D1_RECT_F ResponseButton(int i,float left,float right){
 const auto row=FormFor(53,left,right);const float cy=(row.top+row.bottom)*.5f;
 const float x=right-20-(3-i)*78.f;return D2D1::RectF(x,cy-14,x+72,cy+14);
}
D2D1_RECT_F FpsButton(int i,float left,float right){const auto row=FormFor(40,left,right);const float cy=(row.top+row.bottom)*.5f;
 const float widths[5]={46,46,52,52,86},gap=6;float x=right-20;for(int k=4;k>=i;--k){x-=widths[k];if(k>i)x-=gap;}
 return D2D1::RectF(x,cy-14,x+widths[i],cy+14);}
const char* const kShortcutKeys[4]={"shortcutLyrics","shortcutArtwork","shortcutDesktop","shortcutNowPlaying"};
D2D1_RECT_F ShortcutButton(int i,float left,float right){const auto row=FormFor(34+i,left,right);const float cy=(row.top+row.bottom)*.5f;return D2D1::RectF(right-20-170,cy-15,right-20,cy+15);}
void StartShortcut(HWND h,int i){recordingShortcut=i;SetFocus(h);InvalidateRect(h,nullptr,FALSE);}
void SaveShortcut(HWND h,int value){const int i=recordingShortcut;recordingShortcut=-1;
 if(i>=0&&liquid::settings::WriteControlCenterInt(settingsPath,kShortcutKeys[i],value)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
 Refresh();InvalidateRect(h,nullptr,FALSE);}
// Next key with Ctrl/Alt/Shift held is the new shortcut; Esc cancels; Backspace/Delete removes it.
void RecordShortcutKey(HWND h,UINT vk){
 if(vk==VK_CONTROL||vk==VK_MENU||vk==VK_SHIFT||vk==VK_LWIN||vk==VK_RWIN||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_LMENU||vk==VK_RMENU||vk==VK_LSHIFT||vk==VK_RSHIFT)return;
 UINT mods=0;if(GetKeyState(VK_CONTROL)&0x8000)mods|=MOD_CONTROL;if(GetKeyState(VK_MENU)&0x8000)mods|=MOD_ALT;if(GetKeyState(VK_SHIFT)&0x8000)mods|=MOD_SHIFT;
 if(!mods&&vk==VK_ESCAPE){recordingShortcut=-1;InvalidateRect(h,nullptr,FALSE);return;}
 if(!mods&&(vk==VK_BACK||vk==VK_DELETE)){SaveShortcut(h,0);return;}
 if(!mods||vk>254){MessageBeep(MB_ICONWARNING);return;}  // a shortcut needs Ctrl, Alt or Shift
 SaveShortcut(h,static_cast<int>((mods<<8)|vk));}
// Control Center page, Custom lyrics row: opens %LOCALAPPDATA%\LiquidDesktop\lyrics (created when missing).
D2D1_RECT_F LyricsFolderButton(float left,float right){const auto row=FormFor(30,left,right);const float cy=(row.top+row.bottom)*.5f;return D2D1::RectF(right-20-124,cy-15,right-20,cy+15);}
void OpenLyricsFolder(HWND h){wchar_t base[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return;
 const std::wstring root=std::wstring(base)+L"\\LiquidDesktop",dir=root+L"\\lyrics";CreateDirectoryW(root.c_str(),nullptr);CreateDirectoryW(dir.c_str(),nullptr);
 if(reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",dir.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)MessageBoxW(h,(L"Put lyrics files in this folder:\n"+dir).c_str(),L"DockOver Settings",MB_OK|MB_ICONINFORMATION);}
D2D1_RECT_F GamesListButton(float left,float right){const auto row=FormFor(27,left,right);const float cy=(row.top+row.bottom)*.5f;return D2D1::RectF(right-20-124,cy-15,right-20,cy+15);}
void OpenGamesList(HWND h){const std::wstring path=docky::game::ListPath(settingsPath);
 if(!docky::game::EnsureList(path)){MessageBoxW(h,docky::ui::Text(L"The games list could not be created next to the settings file."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);return;}
 if(reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)MessageBoxW(h,(L"Open this file to edit the games list:\n"+path).c_str(),L"DockOver Settings",MB_OK|MB_ICONINFORMATION);}
FormRow CurrentForm(int id){const auto [left,right]=CurrentContentEdges();return FormFor(id,left,right);}
float UpdateSwitchCardTop(int row){return CurrentForm(14+row).top;}
D2D1_RECT_F UpdateSwitch(float,int row){return CurrentForm(14+row).control;}
struct UpdateText {std::wstring status,detail;};
UpdateText CurrentUpdateText(){UpdateText text;const auto next=dock::json::FromUtf8(updRelease.version);
 switch(updUi){
 case UpdUi::Idle:text.status=!updState.lastCheck?L"Not checked yet.":!updState.latest.empty()&&docky::update::CompareVersions(updState.latest,docky::update::CurrentVersion())>0?L"DockOver "+dock::json::FromUtf8(updState.latest)+L" is available.":L"DockOver is up to date.";text.detail=L"Last checked: "+UpdateWhen(updState.lastCheck)+L".";break;
 case UpdUi::Checking:text.status=L"Checking for updates…";break;
 case UpdUi::UpToDate:text.status=L"DockOver is up to date.";text.detail=L"Version "+dock::json::FromUtf8(docky::update::CurrentVersion())+L" is the newest release.";break;
 case UpdUi::NoRelease:text.status=L"No releases have been published yet.";text.detail=L"Nothing to install.";break;
 case UpdUi::Available:text.status=L"DockOver "+next+L" is available.";text.detail=L"Install downloads and verifies the update, then restarts DockOver. Your settings are kept.";break;
 case UpdUi::Downloading:{const uint64_t d=updDone,t=updTotal;wchar_t b[160];if(t)_snwprintf_s(b,_TRUNCATE,L"Downloading… %.1f of %.1f MB",d/1048576.0,t/1048576.0);else _snwprintf_s(b,_TRUNCATE,L"Downloading… %.1f MB",d/1048576.0);text.status=b;break;}
 case UpdUi::Installing:text.status=L"Installing DockOver "+next+L"…";text.detail=L"DockOver restarts after installation.";break;
 case UpdUi::Error:text.status=L"The update did not complete.";text.detail=updError;break;
 }return text;
}
struct UpdateGeometry {D2D1_RECT_F card,status,detail;float progressTop=0;};
UpdateGeometry MeasureUpdateCard(float left,float right){const auto text=CurrentUpdateText();const float top=UpdateCardTop(),width=right-left-220,statusHeight=DetailHeight(text.status.c_str(),width),detailHeight=DetailHeight(text.detail.c_str(),width);
 const auto status=D2D1::RectF(left+16,top+44,right-204,top+44+statusHeight),detail=D2D1::RectF(left+16,status.bottom+8,right-204,status.bottom+8+detailHeight);
 const float contentBottom=std::max(detail.bottom,top+(updUi==UpdUi::Available?102.f:54.f));const float progress=contentBottom+12;
 return {D2D1::RectF(left,top,right,std::max(top+110,updUi==UpdUi::Downloading?progress+22:contentBottom+16)),status,detail,progress};
}
float UpdateCardBottom(){const auto [left,right]=CurrentContentEdges();return MeasureUpdateCard(left,right).card.bottom;}
struct ActionCard {D2D1_RECT_F bounds,title,detail;std::vector<D2D1_RECT_F> buttons;};
ActionCard MeasureActionCard(float top,float left,float right,const wchar_t* detail,int buttons){
 const float detailTop=top+44,detailBottom=detailTop+DetailHeight(detail,right-left-32),buttonTop=detailBottom+16;
 ActionCard card{D2D1::RectF(left,top,right,buttonTop+32+16),D2D1::RectF(left+16,top+12,right-16,top+38),D2D1::RectF(left+16,detailTop,right-16,detailBottom),{}};
 const float width=std::min(188.f,(right-left-32-12*(buttons-1))/buttons);
 for(int i=0;i<buttons;++i)card.buttons.push_back(D2D1::RectF(left+16+i*(width+12),buttonTop,left+16+i*(width+12)+width,buttonTop+32));return card;
}
const wchar_t* migrationBackupDetail=L"Choose a configuration file to create a separate backup. The original file stays unchanged.";
const wchar_t* migrationImportDetail=L"Import Dock items and Launchpad order. Replace everything first backs up your current items, then keeps only the imported items.";
std::array<ActionCard,2> MeasureMigration(float left,float right){auto backup=MeasureActionCard(80,left,right,migrationBackupDetail,1);return {backup,MeasureActionCard(backup.bounds.bottom+16,left,right,migrationImportDetail,2)};}
bool Contains(D2D1_RECT_F r,float x,float y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

int SidebarCapacity(HWND h){RECT rc{};GetClientRect(h,&rc);return std::max(1,static_cast<int>((rc.bottom/scale-liquid::design::settings::SidebarTop-8)/liquid::design::settings::SidebarStep));}
void SelectCategory(HWND h,int next,bool record=true){
 if(next<0||next>=static_cast<int>(std::size(titles))||next==selected)return;
 if(selected==6)StopRecording(h,nullptr);
 if(record){categoryHistory.erase(categoryHistory.begin()+categoryHistoryIndex+1,categoryHistory.end());categoryHistory.push_back(next);if(categoryHistory.size()>64)categoryHistory.erase(categoryHistory.begin());categoryHistoryIndex=categoryHistory.size()-1;}
 selected=next;UpdateScrollBounds(h);if(selected==7)RefreshWorkshop();InvalidateRect(h,nullptr,FALSE);
}
void StepHistory(HWND h,int delta){
 const int next=static_cast<int>(categoryHistoryIndex)+delta;if(next<0||next>=static_cast<int>(categoryHistory.size()))return;
 if(searchBox&&GetWindowTextLengthW(searchBox)>0)SetWindowTextW(searchBox,L"");
 categoryHistoryIndex=static_cast<size_t>(next);SelectCategory(h,categoryHistory[categoryHistoryIndex],false);EnsureSidebarSelection(h);
 auto it=std::find(visibleCategories.begin(),visibleCategories.end(),selected);if(it!=visibleCategories.end()){const int index=static_cast<int>(it-visibleCategories.begin()),capacity=SidebarCapacity(h);if(index<sidebarFirst)sidebarFirst=index;else if(index>=sidebarFirst+capacity)sidebarFirst=index-capacity+1;}
 InvalidateRect(h,nullptr,FALSE);
}
void UpdateSearch(HWND h){
 wchar_t query[128]{}; if(searchBox)GetWindowTextW(searchBox,query,128);
 visibleCategories=liquid::settings::SearchCategories(query);
 if(docky::ui::chinese&&*query){for(int i=0;i<static_cast<int>(std::size(titles));++i)
 if(std::wstring_view(docky::ui::Text(titles[i])).find(query)!=std::wstring_view::npos&&
 std::find(visibleCategories.begin(),visibleCategories.end(),i)==visibleCategories.end())visibleCategories.push_back(i);}
 if(musicOnly)std::erase_if(visibleCategories,[](int i){return i!=1&&i!=10;});
 sidebarFirst=0;sidebarScroll.Reset();
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
 SelectCategory(h,visibleCategories[index]);EnsureSidebarSelection(h);const int capacity=SidebarCapacity(h);if(index<sidebarFirst)sidebarFirst=index;else if(index>=sidebarFirst+capacity)sidebarFirst=index-capacity+1;InvalidateRect(h,nullptr,FALSE);
}
LRESULT CALLBACK SearchProc(HWND h,UINT message,WPARAM w,LPARAM l){
 if(message==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
 if(message==WM_KILLFOCUS){const auto result=CallWindowProcW(originalSearchProc,h,message,w,l);if(searchHost)ShowWindow(searchHost,SW_HIDE);InvalidateRect(settingsWindowHandle,nullptr,FALSE);return result;}

 if(message==WM_PAINT && GetWindowTextLengthW(h)==0 && GetFocus()!=h){
  const LRESULT result=CallWindowProcW(originalSearchProc,h,message,w,l);
  HDC dc=GetDC(h);if(dc){const int old=SetBkMode(dc,TRANSPARENT);const COLORREF color=SetTextColor(dc,searchText);
   HFONT font=reinterpret_cast<HFONT>(SendMessageW(h,WM_GETFONT,0,0));HGDIOBJ prior=font?SelectObject(dc,font):nullptr;
   RECT r{};GetClientRect(h,&r);DrawTextW(dc,docky::ui::Text(L"Search"),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
   if(prior)SelectObject(dc,prior);SetTextColor(dc,color);SetBkMode(dc,old);ReleaseDC(h,dc);}return result;
 }
 if(message==WM_SYSKEYDOWN&&(w==VK_LEFT||w==VK_RIGHT)){StepHistory(settingsWindowHandle,w==VK_LEFT?-1:1);return 0;}
 if(message==WM_KEYDOWN){
  if(w==VK_DOWN||w==VK_UP){MoveCategory(settingsWindowHandle,w==VK_DOWN?1:-1);return 0;}
  if(w==VK_ESCAPE){SetWindowTextW(h,L"");return 0;}
  if(w==VK_TAB){SetFocus(settingsWindowHandle);return 0;}
 }
 const auto result=CallWindowProcW(originalSearchProc,h,message,w,l);
 if(message==WM_KEYDOWN||message==WM_KEYUP||message==WM_CHAR||message==WM_LBUTTONUP||message==WM_IME_COMPOSITION)
  InvalidateRect(settingsWindowHandle,nullptr,FALSE);
 return result;
}
void UpdateSearchFont(){if(!searchBox)return;HFONT next=CreateFontW(-static_cast<int>(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,liquid::design::type::FamilyGdi);if(next){SendMessageW(searchBox,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);HFONT old=searchFont;searchFont=next;if(old)DeleteObject(old);}}
// The transparent owned host receives native text input. The glass pass
// draws the search text and caret with the same material in both focus states.
LRESULT CALLBACK SearchHostProc(HWND h,UINT message,WPARAM w,LPARAM l){
 if(message==WM_ERASEBKGND){RECT rc{};GetClientRect(h,&rc);FillRect(reinterpret_cast<HDC>(w),&rc,searchBrush?searchBrush:GetSysColorBrush(COLOR_WINDOW));return 1;}
 if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);FillRect(dc,&ps.rcPaint,searchBrush?searchBrush:GetSysColorBrush(COLOR_WINDOW));EndPaint(h,&ps);return 0;}
 if(message==WM_CTLCOLOREDIT||message==WM_COMMAND)return SendMessageW(settingsWindowHandle,message,w,l);
 if(message==WM_NCHITTEST)return HTCLIENT;
 if(message==WM_CLOSE){PostMessageW(settingsWindowHandle,WM_CLOSE,0,0);return 0;}
 return DefWindowProcW(h,message,w,l);
}
void PositionSearch(){if(!searchBox||!searchHost)return;POINT origin{static_cast<LONG>(45*scale),static_cast<LONG>(80*scale)};ClientToScreen(settingsWindowHandle,&origin);
 const int width=static_cast<int>((liquid::design::settings::SidebarWidth-69)*scale),height=static_cast<int>(20*scale);
 SetWindowPos(searchHost,HWND_TOP,origin.x,origin.y,width,height,SWP_NOACTIVATE);
 SetWindowPos(searchBox,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}
// Approximation from the supplied reference, in DIPs; Apple does not publish a
// macOS window radius or its private continuous-corner path for Windows.
void UpdateWindowContour(HWND h, UINT sizeKind){
 if(sizeKind==SIZE_MINIMIZED)return;
 if(gpuReady&&renderer)renderer->SetRoundedVisualClip(0.f);
 if(IsZoomed(h)){SetWindowRgn(h,nullptr,TRUE);return;}
 RECT rc{};GetClientRect(h,&rc);const float w=static_cast<float>(rc.right),ht=static_cast<float>(rc.bottom);
 const float radius=std::min(SettingsWindowRadius*scale,std::min(w,ht)*.5f);
 // Native hit contour includes the antialiased edge pixels of the D2D mask.
 // The same continuous corner is used by the visible outline below.
 std::vector<POINT> points;
 for(int corner=0;corner<4;++corner)for(int i=0;i<=40;++i){const float t=1.57079632679f*i/40;
  const float a=radius*(1-std::pow(std::max(0.f,std::sin(t)),2.f/3.f)),b=radius*(1-std::pow(std::max(0.f,std::cos(t)),2.f/3.f));float x,y;
  if(corner==0){x=w-a+1;y=b-1;}else if(corner==1){x=w-b+1;y=ht-a+1;}else if(corner==2){x=a-1;y=ht-b+1;}else{x=b-1;y=a-1;}
  points.push_back({static_cast<LONG>(std::lround(x)),static_cast<LONG>(std::lround(y))});
 }
 HRGN region=CreatePolygonRgn(points.data(),static_cast<int>(points.size()),WINDING);
 if(region&&!SetWindowRgn(h,region,TRUE))DeleteObject(region);
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
  const float r=(std::min)(SettingsWindowRadius,(std::min)((right-left)*.5f,(bottom-top)*.5f));  // reference-matched continuous outer corner
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
 if(!contrast)return;
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
 case 10: {circle(9,17,2.6f);circle(17,15,2.6f);line(11.6f,17,11.6f,6.5f);line(19.6f,15,19.6f,5);line(11.6f,6.5f,19.6f,5);break;}  // music notes
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
void Refresh(){const auto r=liquid::settings::Load(settingsPath);docky::ui::SetLanguage(r.value.appearance.language);gameCompatibility=r.value.dock.gameCompatibility;lowLatencyGlass=r.value.dock.lowLatencyGlass;menuBarOn=r.value.menuBar.enabled;menuBarThemeGradient=r.value.menuBar.themeGradient;diagnostics=r.value.dock.diagnostics;iconMask=r.value.dock.useMacIconMask;autoHide=r.value.dock.autoHide;hideSnap=r.value.dock.hideSnapLayouts;genie=r.value.dock.genieMinimize;dlCount=r.value.dock.downloadsCount;if(!menuSliderDragging)dockMagnification=r.value.dock.magnification;dlVertical=r.value.dock.downloadsVertical;dlPreview=r.value.dock.downloadsPreview;launchDesktop=r.value.dock.launchDesktop;loginItem=LoginItemEnabled();density=r.value.launchpad;ccBlur=r.value.controlCenter.backdropBlur;mediaArtworkBackground=r.value.controlCenter.mediaArtworkBackground;musicWallpaper=r.value.controlCenter.musicWallpaper;wallpaperAvoidBars=r.value.controlCenter.wallpaperAvoidBars;wallpaperLyrics=r.value.controlCenter.wallpaperLyrics;lyricsFullArtwork=r.value.controlCenter.lyricsFullArtwork;lyricsAutoReturn=r.value.controlCenter.lyricsAutoReturn;animatedArtwork=r.value.controlCenter.animatedArtwork;lyricsDoubleClickSeek=r.value.controlCenter.lyricsDoubleClickSeek;keepArtworkWhenPaused=r.value.controlCenter.keepArtworkWhenPaused;localMusicTags=r.value.controlCenter.localMusicTags;hideIconsWithWidgets=r.value.controlCenter.hideDesktopIconsWithWidgets;lyricsBeat=r.value.controlCenter.lyricsBeat;musicFps=r.value.controlCenter.musicFps;musicResponse=r.value.controlCenter.musicResponse;if(!menuSliderDragging||activeSizeSlider!=9)musicSensitivity=r.value.controlCenter.musicSensitivity;lyricsOffsetMs=r.value.controlCenter.lyricsOffsetMs;shortcutValues[0]=r.value.controlCenter.shortcutLyrics;shortcutValues[1]=r.value.controlCenter.shortcutArtwork;shortcutValues[2]=r.value.controlCenter.shortcutDesktop;shortcutValues[3]=r.value.controlCenter.shortcutNowPlaying;shortcutsLoaded=true;captureNotifications=r.value.menuBar.captureNotifications;hideSystemNotifications=r.value.menuBar.hideSystemNotifications;ncBlur=r.value.menuBar.notificationBlur;ncFade=r.value.menuBar.notificationBlurFade;ncShade=r.value.menuBar.notificationShade;if(!menuSliderDragging){gradientOpacity=r.value.menuBar.gradientOpacity;gradientLength=r.value.menuBar.gradientLength;menuScalePercent=r.value.menuBar.scalePercent;settingsUiPercent=r.value.appearance.settingsScalePercent;applicationsUiPercent=r.value.appearance.applicationsScalePercent;}perf=r.value.performance;updatePref=r.value.updates;appearanceMode=r.value.appearance.mode;applicationsSort=r.value.appearance.applicationsSort;taskbarPref=r.value.taskbar;revision=liquid::settings::ReadRevision(settingsPath);}
void OpenWindowsTaskbarSettings(HWND h){
 const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",L"ms-settings:taskbar",nullptr,nullptr,SW_SHOWNORMAL));
 if(result<=32)MessageBoxW(h,docky::ui::Text(L"Windows Taskbar Settings could not be opened. Open Personalization > Taskbar in Windows Settings."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
}
void OpenVisualEffects(HWND h){
 const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(h,L"open",L"ms-settings:easeofaccess-visualeffects",nullptr,nullptr,SW_SHOWNORMAL));
 if(result<=32)MessageBoxW(h,docky::ui::Text(L"Windows Settings could not be opened. Open Accessibility > Visual effects in Windows Settings."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
}
void ToggleDockSetting(HWND h,int row){
 if(row<0||row>=5)return;const char* keys[]={"useMacIconMask","autoHide","diagnostics","hideSnapLayouts","genieMinimize"};const bool now[]={iconMask,autoHide,diagnostics,hideSnap,genie};
 const auto status=liquid::settings::WriteBool(settingsPath,"dock",keys[row],!now[row]);
 if(status!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetAppearance(HWND h,int mode){
 if(liquid::settings::WriteAppearanceMode(settingsPath,mode)!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save appearance. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
 ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);
}
void SetDownloadsCount(HWND h,int value){
 if(liquid::settings::WriteDockInt(settingsPath,"downloadsCount",std::clamp(value,3,10))!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetCcBlur(HWND h,int value){
 if(liquid::settings::WriteControlCenterInt(settingsPath,"backdropBlur",std::clamp(value,0,100))!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
 InvalidateRect(h,nullptr,FALSE);
}
void SetDensity(HWND h,const char* key,int value){
 const auto status=liquid::settings::WriteDensityInt(settingsPath,key,value);
 if(status!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
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
 taskbarFeedback=code==0?L"Recovery finished: nothing DockOver owned is hidden.":code==99?L"The recovery tool could not be run.":L"Recovery stopped: an ambiguous record was preserved. See the DockOver taskbar restore log (docky-taskbar-restore) for details.";
 RefreshTaskbarState();InvalidateRect(h,nullptr,FALSE);
}
void SaveTaskbarBool(HWND h,const char* key,bool value){
 if(liquid::settings::WriteBool(settingsPath,"taskbar",key,value)!=liquid::settings::SaveStatus::Saved){Refresh();MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);}else Refresh();
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
const wchar_t* TaskbarHotkeyDetail(){static std::wstring text;
 text=!taskbarPref.hotkeyKey?L"No shortcut set.":!taskbarPref.hotkeyEnabled?L"Off":!tbLive?L"Available while DockOver is running.":tbStatus.hotkey==L"registered"?L"Use the shortcut anywhere to hide or show the taskbar.":tbStatus.hotkey==L"suspended"?L"Paused while recording a new shortcut.":tbStatus.hotkey==L"conflict"?L"Used by another app. Record a different shortcut.":tbStatus.hotkey==L"invalid"?L"Record a different shortcut.":L"Applying…";return text.c_str();}
std::array<std::wstring,3> TaskbarStatusLines(){const bool hidden=tbLive&&tbStatus.state==L"hidden";
 std::wstring current=!taskbarState.known?L"Current: unavailable":taskbarState.visible==taskbarState.windows?L"Current: visible":taskbarState.visible==0?(hidden?L"Current: hidden by DockOver":L"Current: hidden outside DockOver"):L"Current: partly hidden";
 if(tbLive&&tbStatus.state==L"conflict")current+=L". Recovery record needs review.";
 std::wstring recovery=!tbLive?L"DockOver is not running.":tbStatus.guardian==L"ready"?L"Crash recovery: ready":tbStatus.guardian==L"starting"?L"Crash recovery: starting…":tbStatus.guardian==L"failed"?L"Crash recovery unavailable. Hiding is disabled.":L"Crash recovery: not started";
 return {current,recovery,!taskbarState.known?L"Windows auto-hide: unavailable":taskbarState.autoHide?L"Windows auto-hide: on":L"Windows auto-hide: off"};}
float TaskbarStatusBottom(){const auto [left,right]=CurrentContentEdges();float y=80+44;for(const auto& line:TaskbarStatusLines())y+=DetailHeight(line.c_str(),right-left-32)+6;return y+10;}
float TaskbarPreferencesTop(){return TaskbarStatusBottom()+12+32+16;}
D2D1_RECT_F TaskbarControlRect(int control,float left,float right){
 if(control<3){const float width=(right-left-16)/3,y=TaskbarStatusBottom()+12,x=left+control*(width+8);return D2D1::RectF(x,y,x+width,y+32);}
 if(control<6)return FormFor(control,left,right).control;
 const float top=FormFor(5,left,right).bottom;
 return control==6?D2D1::RectF(right-178,top+40,right-92,top+72):D2D1::RectF(right-84,top+40,right-16,top+72);
}
bool CreateResources(HWND h){
 if(!fallbackTarget){RECT rc{};GetClientRect(h,&rc);
  if(FAILED(factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(h,D2D1::SizeU(rc.right,rc.bottom)),&fallbackTarget)))return false;
  fallbackTarget->SetDpi(96*scale,96*scale);}
 target=fallbackTarget.Get();
 if(!brush && FAILED(target->CreateSolidColorBrush(C(0xffffff),&brush))){fallbackTarget.Reset();target=nullptr;return false;}return true;
}
bool SettingsSwitchValue(int index){switch(index){case 0:return iconMask;case 1:return autoHide;case 2:return diagnostics;
 case 3:return taskbarPref.hideOnStartup;case 4:return taskbarPref.freeWorkArea;case 5:return taskbarPref.hotkeyEnabled&&taskbarPref.hotkeyKey!=0;case 8:return hideSnap;case 9:return density.hideLabels;case 10:return genie;case 11:return dlVertical;case 12:return dlPreview;case 13:return density.showWindows;case 14:return updatePref.autoCheck;case 15:return updatePref.autoInstall;case 16:return launchDesktop;case 17:return loginItem;case 18:return gameCompatibility;case 26:return lowLatencyGlass;case 20:return menuBarOn;case 46:return menuBarThemeGradient;case 24:return captureNotifications;case 25:return hideSystemNotifications;case 19:return mediaArtworkBackground;case 28:return musicWallpaper;case 29:return wallpaperAvoidBars;case 31:return wallpaperLyrics;case 32:return lyricsFullArtwork;case 33:return lyricsAutoReturn;case 38:return lyricsBeat;case 44:return animatedArtwork;case 45:return lyricsDoubleClickSeek;case 50:return keepArtworkWhenPaused;case 51:return localMusicTags;case 52:return hideIconsWithWidgets;default:return false;}}
// General page: "Start DockOver Desktop with DockOver" (index 16), in its own card below the frame-rate card.
float DesktopLaunchCardTop(){return CurrentForm(16).top;}
D2D1_RECT_F DesktopLaunchSwitch(float){return CurrentForm(16).control;}
float LoginItemCardTop(){return CurrentForm(17).top;}
D2D1_RECT_F LoginItemSwitch(float){return CurrentForm(17).control;}
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
float DownloadsCardTop(){return CurrentDockForm().rows[5].top;}
D2D1_RECT_F DownloadsVerticalSwitch(float,int row=1){return CurrentDockForm().rows[5+row].control;}
// Launchpad page: the "Hide app names" switch (index 9), in its own card below the density card.
D2D1_RECT_F LaunchpadNamesSwitch(float){return CurrentForm(9).control;}
D2D1_RECT_F LaunchpadWindowsSwitch(float){return CurrentForm(13).control;}
int SettingsSwitchAt(HWND h,float x,float y){
 if(x<liquid::design::settings::SidebarWidth||y<60)return -1;
 y=liquid::settings::SettingsBodyY(y,contentScroll[selected].Position());
 namespace lay=liquid::design::settings;RECT rc{};GetClientRect(h,&rc);
 const float left=ContentLeftForWidth(rc.right/scale),right=ContentRightForWidth(rc.right/scale,left);
 if(selected==2){const auto form=MeasureDock(left,right);for(int i=0;i<8;++i){if(i==5)continue;auto r=form.rows[i].control;r.left-=8;r.right+=8;r.top-=8;r.bottom+=8;if(Contains(r,x,y))return dockRows[i].id;}}
 if(selected==5||selected==10){for(int id:{19,44,28,29,31,50,51,32,33,45,38,52}){if((id==19)!=(selected==5))continue;auto r=FormFor(id,left,right).control;r.left-=8;r.right+=8;r.top-=8;r.bottom+=8;if(Contains(r,x,y))return id;}}
 if(selected==3){for(int id:{46,18,20,24,25}){auto r=FormFor(id,left,right).control;r.left-=8;r.right+=8;r.top-=8;r.bottom+=8;if(Contains(r,x,y))return id;}}
 if(selected==0){const auto r=DesktopLaunchSwitch(right);if(x>=r.left-8&&x<r.right+8&&y>=r.top-8&&y<r.bottom+8)return 16;
  const auto g=LoginItemSwitch(right);if(x>=g.left-8&&x<g.right+8&&y>=g.top-8&&y<g.bottom+8)return 17;
  {auto l=FormFor(26,left,right).control;l.left-=8;l.right+=8;l.top-=8;l.bottom+=8;if(Contains(l,x,y))return 26;}}
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
float SettingsContentBottom(){namespace lay=liquid::design::settings;
 switch(selected){case 0:return CurrentForm(26).bottom;
 case 1:return 822;case 2:return CurrentDockForm().rows.back().bottom;
 case 3:return SizeSpec(5).top+146;case 4:return CurrentForm(13).bottom;
 case 5:return CurrentForm(19).bottom;case 10:return SizeSpec(9).top+146;case 6:{const auto [left,right]=CurrentContentEdges();const std::wstring feedback=!taskbarFeedback.empty()?taskbarFeedback:tbLive?tbStatus.message:L"";return CurrentForm(5).bottom+88+(feedback.empty()?0:12+DetailHeight(feedback.c_str(),right-left-32));}
 case 7:return lay::CardTop+318;case 8:{const auto [left,right]=CurrentContentEdges();return MeasureMigration(left,right)[1].bounds.bottom;}
 case 9:return CurrentForm(15).bottom;default:return lay::CardTop+lay::CardHeight;}
}
void DrawSwitchRow(HWND h,int id,float left,float right,const liquid::design::Palette& colors,bool card=true){const auto row=FormFor(id,left,right);if(card)Round(D2D1::RectF(left,row.top,right,row.bottom),SettingsCardRadius,colors.surface);const auto spec=SpecFor(id);DrawFormText(row,spec.title,spec.detail,colors);DrawSettingsSwitch(h,id,row.control,SettingsSwitchValue(id),switchPressed==id,GetFocus()==h&&keyboardFocusVisible&&((selected==6&&taskbarKeyboardControl==id)||(selected==3&&((id==46&&menuKeyboardControl==1)||(id==18&&menuKeyboardControl==2)))||selected==5),colors);}
dock::GlassParameters SettingsGlass(dock::GlassSurface surface,dock::SettingsGlassShape shape){
 auto params=dock::GlassSettingsStore::Get().Surface(surface);
 params.settingsShape=shape;
 params.dark=liquid::theme::IsDark(appearanceMode,systemDark);
 params.tintR=params.tintStrength;
 params.tintStrength=0; // The settings shader applies tint after refraction and diffusion.
 params.interactionStrength=0;
 params.blurSpread=shape==dock::SettingsGlassShape::Sidebar?1.f:std::max(.54f,params.blurSpread);
 return params;
}
void ChromeGlass(D2D1_RECT_F rect,dock::GlassSurface surface,std::function<void()> labels){
 const auto colors=SettingsPalette();auto fallback=[rect,colors,labels](){Round(rect,(rect.bottom-rect.top)*.5f,colors.surface);brush->SetColor(C(colors.secondary,.18f));target->DrawRoundedRectangle(D2D1::RoundedRect(rect,(rect.bottom-rect.top)*.5f,(rect.bottom-rect.top)*.5f),brush.Get(),.7f);labels();};
 if(!drawingChromeGlass||!allowTransparency){fallback();return;}
 dock::LocalGlassOverlay overlay;overlay.cacheKey=surface==dock::GlassSurface::SettingsSearch?1:2;overlay.active=true;overlay.plate=overlay.sampleBounds=rect;
 overlay.params=SettingsGlass(surface,dock::SettingsGlassShape::Capsule);overlay.controlScale=1.f;overlay.press=0;
 overlay.pointerX=switchPointerX*scale;overlay.pointerY=switchPointerY*scale;
 auto finish=[](std::function<void()> paint){return [paint](ID2D1DeviceContext* dc){target=dc;brush.Reset();if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xffffff),&brush)))paint();brush.Reset();target=nullptr;};};
 overlay.after=finish(labels);overlay.fallback=finish(fallback);drawingChromeGlass->push_back(std::move(overlay));
}
std::pair<float,float> CurrentContentEdges();
// Menu Bar page, below the notification switches: the Notification Center background (indices 3-5), one card each.
float NcSliderTop(int index){const auto [left,right]=CurrentContentEdges();return FormFor(25,left,right).bottom+12+(index-3)*158.f;}
SizeSliderSpec SizeSpec(int index){switch(index){
 case 6:case 7:{const auto [left,right]=CurrentContentEdges();const float top=FormFor(46,left,right).bottom+12+(index-6)*158.f;
  return index==6?SizeSliderSpec{0,100,top,L"Gradient opacity",L"Set how strong the black or white shade is at the top edge."}
                 :SizeSliderSpec{40,100,top,L"Gradient length",L"Set how far down the menu bar the gradient reaches before becoming transparent."};}
case 9:{const auto [left,right]=CurrentContentEdges();
 return {0,100,FormFor(53,left,right).bottom+12,L"Detection sensitivity",L"Higher values pick up softer rhythmic hits. Lower values filter out more weak hits."};}
case 8:return {0,150,liquid::design::settings::CardTop,L"Magnification",L"How much Dock icons grow under the pointer. 0% turns magnification off."};
case 3:return {0,100,NcSliderTop(3),L"Notification Center blur",kNcBlurDetail};case 4:return {40,400,NcSliderTop(4),L"Blur fade width",kNcFadeDetail,L""};case 5:return {0,100,NcSliderTop(5),L"Background shade",kNcShadeDetail};case 1:return {75,125,242,L"Settings interface size",L"Scale the whole Settings window. Applies when you release the slider."};case 2:return {60,120,400,L"Applications window size",L"Scale the window opened with the Windows key."};default:return {80,160,80,L"Menu bar size",L"Scale text, icons and the height of the top menu bar."};}}
int& SizeValue(int index){return index==9?musicSensitivity:index==8?dockMagnification:index==1?settingsUiPercent:index==2?applicationsUiPercent:index==3?ncBlur:index==4?ncFade:index==5?ncShade:index==6?gradientOpacity:index==7?gradientLength:menuScalePercent;}
// Appearance > Applications window size: "Reset Size" returns the Win-key window to its default size (it saves the size
// the user dragged to in applications-window.json next to the shared settings; the window notices the file is gone).
std::wstring ApplicationsWindowSizePath(){const auto slash=settingsPath.find_last_of(L"\\/");return (slash==std::wstring::npos?std::wstring():settingsPath.substr(0,slash+1))+L"applications-window.json";}
bool ApplicationsWindowResized(){return GetFileAttributesW(ApplicationsWindowSizePath().c_str())!=INVALID_FILE_ATTRIBUTES;}
D2D1_RECT_F ApplicationsResetButton(float right);
D2D1_RECT_F SizeSliderTrack(int index,float left,float right){const auto spec=SizeSpec(index);
 if(index==0)return D2D1::RectF(right-175,spec.top+32,right-72,spec.top+38);
 return D2D1::RectF(left+36,spec.top+93,right-36,spec.top+99);}
void ResizeSettingsUi(HWND h){
 const float next=GetDpiForWindow(h)/96.f*settingsUiPercent/100.f;if(std::fabs(next-scale)<.001f)return;
 RECT rc{};GetWindowRect(h,&rc);const float ratio=next/scale;scale=next;UpdateSearchFont();PositionSearch();
 if(IsZoomed(h)){RECT client{};GetClientRect(h,&client);if(renderer)renderer->Resize(client.right,client.bottom,96*scale);UpdateWindowContour(h,SIZE_MAXIMIZED);}
 else SetWindowPos(h,nullptr,0,0,static_cast<int>(std::lround((rc.right-rc.left)*ratio)),static_cast<int>(std::lround((rc.bottom-rc.top)*ratio)),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
 if(fallbackTarget)fallbackTarget->SetDpi(96*scale,96*scale);InvalidateRect(h,nullptr,FALSE);
}
void PreviewMenuScale(){
 if(activeSizeSlider!=0||settingsPath!=liquid::settings::DefaultPath())return;
 if(HWND bar=FindWindowW(L"LiquidDesktopMenuBarV01",nullptr))PostMessageW(bar,WM_APP+91,menuScalePercent,0);
}
void UpdateMenuSlider(HWND h,float x){const auto [left,right]=CurrentContentEdges();const auto track=SizeSliderTrack(activeSizeSlider,left,right);const auto spec=SizeSpec(activeSizeSlider);
 const int value=std::clamp(static_cast<int>(std::lround(spec.lo+(spec.hi-spec.lo)*(x-menuSliderGrab-track.left)/(track.right-track.left))),spec.lo,spec.hi);
 if(value!=SizeValue(activeSizeSlider)){SizeValue(activeSizeSlider)=value;PreviewMenuScale();InvalidateRect(h,nullptr,FALSE);}
}
void SaveMenuScale(HWND h){
 const int index=activeSizeSlider;
 const auto saved=index==0?liquid::settings::WriteMenuBarScale(settingsPath,menuScalePercent)
  :index==9?liquid::settings::WriteControlCenterInt(settingsPath,"musicSensitivity",musicSensitivity)
  :index==8?liquid::settings::WriteDockInt(settingsPath,"magnification",dockMagnification)
  :index>=3?liquid::settings::WriteMenuBarInt(settingsPath,index==3?"notificationBlur":index==4?"notificationBlurFade":index==5?"notificationShade":index==6?"gradientOpacity":"gradientLength",SizeValue(index))
  :liquid::settings::WriteUiScale(settingsPath,index==1?"settingsScalePercent":"applicationsScalePercent",SizeValue(index));
 Refresh();PreviewMenuScale();if(index==1)ResizeSettingsUi(h);
 if(saved!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous setting was preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
 InvalidateRect(h,nullptr,FALSE);
}
D2D1_RECT_F ApplicationsResetButton(float right){const float top=SizeSpec(2).top;return D2D1::RectF(right-224,top+12,right-100,top+40);}
void DrawSizeSlider(HWND h,int index,float left,float right,const liquid::design::Palette& colors){
 const auto spec=SizeSpec(index);const float top=spec.top;
 Round(D2D1::RectF(left,top,right,top+(index==0?MenuSizeRowHeight:146.f)),SettingsCardRadius,colors.surface);
 Text(spec.title,normal.Get(),D2D1::RectF(left+16,top+14,right-100,top+39),colors.primary);
 Text((std::to_wstring(SizeValue(index))+spec.unit).c_str(),trailingSmall.Get(),D2D1::RectF(right-64,top+(index==0?21:14),right-16,top+(index==0?50:39)),colors.secondary);
 Text(spec.detail,fontSmall.Get(),D2D1::RectF(left+16,top+(index==0?36:45),right-(index==0?202:16),top+(index==0?71:83)),colors.secondary);
 const auto track=SizeSliderTrack(index,left,right);const auto now=GetTickCount64();
 const float press=ReducedMotion()?0.f:(visualSwitch==40+index?PressAmount(now):0.f);
 if(visualSwitch==40+index&&now-pressStart<150&&!ReducedMotion())switchTicker.Start(h,docky::vblank::kMsgVBlank);
 const auto shape=dock::LabSlider(track.left,track.right,(track.top+track.bottom)*.5f,(SizeValue(index)-spec.lo)/float(spec.hi-spec.lo),.8f,press);
 const bool gpu=allowTransparency&&drawingChromeGlass;
 dock::DrawLabSlider(target,brush.Get(),shape,liquid::theme::IsDark(appearanceMode,systemDark),gpu);
 const auto knob=shape.thumb;
 const bool focused=keyboardFocusVisible&&GetFocus()==h&&(index==9?selected==10:index==0?menuKeyboardControl==0:appearanceKeyboard==index+2);
 if(focused){brush->SetColor(C(colors.focused));const float r=(knob.bottom-knob.top)*.5f+3;target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(knob.left-3,knob.top-3,knob.right+3,knob.bottom+3),r,r),brush.Get(),2);}
 if(index!=0){Text((std::to_wstring(spec.lo)+spec.unit).c_str(),fontSmall.Get(),D2D1::RectF(left+16,top+118,left+90,top+138),colors.secondary);
 Text((std::to_wstring(spec.hi)+spec.unit).c_str(),trailingSmall.Get(),D2D1::RectF(right-90,top+114,right-16,top+138),colors.secondary);}
 if(press<=.001f||!gpu)return;
 dock::LocalGlassOverlay overlay;overlay.cacheKey=3+index;overlay.active=true;overlay.plate=D2D1::RectF(knob.left,knob.top+bodyShift,knob.right,knob.bottom+bodyShift);overlay.sampleBounds=D2D1::RectF(track.left,track.top+bodyShift,track.right,track.bottom+bodyShift);
 overlay.params=dock::ControlLensGlass(knob.bottom-knob.top);overlay.params.dark=liquid::theme::IsDark(appearanceMode,systemDark);overlay.controlScale=1;overlay.pointerX=switchPointerX*scale;overlay.pointerY=switchPointerY*scale;
 const auto plate=overlay.plate;auto pill=[plate](ID2D1DeviceContext* dc,float alpha){if(alpha<=.001f)return;ComPtr<ID2D1SolidColorBrush>b;const float r=(plate.bottom-plate.top)*.5f;if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xFFFFFF,alpha),&b)))dc->FillRoundedRectangle(D2D1::RoundedRect(plate,r,r),b.Get());};
 overlay.after=[pill,press](ID2D1DeviceContext* dc){pill(dc,1-press);};overlay.fallback=[pill](ID2D1DeviceContext* dc){pill(dc,1);};drawingChromeGlass->push_back(std::move(overlay));
}
void SetLanguage(HWND h,int option){if(liquid::settings::WriteLanguage(settingsPath,option==0?"en":"zh-CN")!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save the language preference."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();SetWindowTextW(h,docky::ui::Text(musicOnly?L"Music settings":L"DockOver Settings"));
 if(searchBox)SendMessageW(searchBox,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(docky::ui::Text(L"Search Settings")));
 UpdateSearch(h);}
void DrawLanguageCard(HWND h,float left,float right,const liquid::design::Palette& colors){
 Round(D2D1::RectF(left,558,right,698),SettingsCardRadius,colors.surface);Text(L"Language",normal.Get(),D2D1::RectF(left+16,570,right-16,596),colors.primary);
 const float width=(right-left-44)*.5f;const auto english=D2D1::RectF(left+16,604,left+16+width,636),chinese=D2D1::RectF(left+28+width,604,right-16,636);
 Round(english,8,docky::ui::chinese?colors.background:colors.selected);Text(L"English",centerSmall.Get(),english,colors.primary);
 Round(chinese,8,docky::ui::chinese?colors.selected:colors.background);Text(L"简体中文",centerSmall.Get(),chinese,colors.primary);
 if(keyboardFocusVisible&&GetFocus()==h&&(appearanceKeyboard==5||appearanceKeyboard==6)){const auto box=appearanceKeyboard==5?english:chinese;brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left-2,602,box.right+2,638),10,10),brush.Get(),2);}
 Text(L"Change the language used in DockOver.",fontSmall.Get(),D2D1::RectF(left+16,650,right-16,686),colors.secondary);
}
D2D1_RECT_F ApplicationsSortOption(float left,float right,int index){
 const float width=(right-left-48)/3.f,x=left+16+index*(width+8);
 return D2D1::RectF(x,752,x+width,786);
}
void SetApplicationsOrder(HWND h,int index){
 if(liquid::settings::WriteApplicationsSort(settingsPath,index)!=liquid::settings::SaveStatus::Saved)
  MessageBoxW(h,docky::ui::Text(L"Could not save the applications sort order."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
 Refresh();InvalidateRect(h,nullptr,FALSE);
}
void DrawApplicationsOrder(HWND h,float left,float right,const liquid::design::Palette& colors){
 Round(D2D1::RectF(left,710,right,822),SettingsCardRadius,colors.surface);
 Text(L"Windows key app order",normal.Get(),D2D1::RectF(left+16,721,right-16,745),colors.primary);
 const wchar_t* labels[]={L"Name (A–Z)",L"Most used",L"Recently opened"};
 for(int i=0;i<3;++i){const auto box=ApplicationsSortOption(left,right,i);
  Round(box,8,i==applicationsSort?colors.selected:colors.background);Text(labels[i],centerSmall.Get(),box,colors.primary);
  if(keyboardFocusVisible&&GetFocus()==h&&appearanceKeyboard==7+i){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(box,8,8),brush.Get(),2);}
 }
}
D2D1_RECT_F AppearanceOption(float left,float right,int index){
 const float width=std::min(94.f,(right-left-128.f)/3.f);
 const float x=right-16.f-(3-index)*width-(2-index)*10.f;
 return D2D1::RectF(x,liquid::design::settings::CardTop+23,x+width,liquid::design::settings::CardTop+107);
}

void DrawAppearanceOptions(HWND h,float left,float right,const liquid::design::Palette& colors){
 const wchar_t* labels[]={L"System",L"Light",L"Dark"};
 for(int i=0;i<3;++i){
  const auto option=AppearanceOption(left,right,i);
  const auto preview=D2D1::RectF(option.left,option.top,option.right,option.top+54);
  const float width=preview.right-preview.left;
  Round(preview,8.f,i==1?0xCBDCF0:0x243C66);
  const auto window=D2D1::RectF(preview.left+width*.10f,preview.top+9,preview.right-width*.10f,preview.bottom-8);
  auto drawWindow=[&](bool dark){
   Round(window,5.f,dark?0x26282D:0xFAFAFA);
   const float divider=window.left+width*.24f;
   Fill(D2D1::RectF(window.left+4,window.top+8,divider,window.bottom-4),dark?0x373B43:0xE8ECF2);
   Round(D2D1::RectF(window.left+5,window.top+16,divider-2,window.top+21),2.5f,0x377CE7);
   Round(D2D1::RectF(divider+4,window.top+10,window.right-5,window.top+17),3.f,dark?0x434650:0xE0E7F2);
   Round(D2D1::RectF(divider+4,window.top+22,window.right-5,window.top+29),3.f,dark?0x434650:0xE0E7F2);
  };
  drawWindow(i==2);
  if(i==0){
   target->PushAxisAlignedClip(D2D1::RectF(preview.left+width*.5f,preview.top,preview.right,preview.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
   drawWindow(true);
   target->PopAxisAlignedClip();
  }
  if(i==appearanceMode){
   brush->SetColor(C(colors.accent));
   target->DrawRoundedRectangle(D2D1::RoundedRect(preview,8,8),brush.Get(),2.f);
  }
  if(GetFocus()==h&&keyboardFocusVisible&&appearanceKeyboard==i){
   brush->SetColor(C(colors.focused));
   target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(option.left-3,option.top-3,option.right+3,option.bottom+3),10,10),brush.Get(),1.f);
  }
  Text(labels[i],centerSmall.Get(),D2D1::RectF(option.left,preview.bottom+4,option.right,option.bottom),i==appearanceMode?colors.primary:colors.secondary);
 }
}

void DrawContent(HWND h){
 namespace ds=liquid::design; namespace sp=liquid::design::space; namespace lay=liquid::design::settings;
 RECT rc{};GetClientRect(h,&rc);float w=rc.right/scale,ht=rc.bottom/scale;
 const auto colors=SettingsPalette();
 const float left=ContentLeftForWidth(w),right=ContentRightForWidth(w,left);
 Fill(D2D1::RectF(0,0,w,ht),colors.background);
 Fill(D2D1::RectF(0,0,lay::SidebarWidth,ht),colors.navigation);
 HIGHCONTRASTW chromeHc{sizeof(chromeHc)};const bool chromeContrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(chromeHc),&chromeHc,0)&&(chromeHc.dwFlags&HCF_HIGHCONTRASTON);
  auto scrollbar=[colors](float x,float top,float height,float total,float offset){if(total<=height)return;const float thumb=std::max(28.f,height*height/total),y=top+(height-thumb)*offset/(total-height);brush->SetColor(C(colors.secondary,.45f));target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+4,y+thumb),2,2),brush.Get());};
 auto sidebarLabels=[h,ht,colors,chromeContrast,scrollbar](){
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


 target->PushAxisAlignedClip(D2D1::RectF(0,64,lay::SidebarWidth,ht),D2D1_ANTIALIAS_MODE_ALIASED);
 target->SetTransform(D2D1::Matrix3x2F::Translation(0,-sidebarScroll.Position()));
 brush->SetColor(C(chromeContrast?colors.primary:0x5576D8));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(35,141),21.f,21.f),brush.Get());
 Text(L"D",heading.Get(),D2D1::RectF(28,125,52,158),chromeContrast?colors.background:0xFFFFFF);
 Text(L"DockOver",normal.Get(),D2D1::RectF(68,125,lay::SidebarWidth-16,145),colors.primary);
 Text(L"Desktop settings",fontSmall.Get(),D2D1::RectF(68,145,lay::SidebarWidth-16,164),colors.secondary);
 if(visibleCategories.empty()) Text(L"No matching categories",fontSmall.Get(),D2D1::RectF(sp::L,lay::SidebarTop,lay::SidebarWidth-sp::S,lay::SidebarTop+40),colors.secondary);

 for(int row=0;row<static_cast<int>(visibleCategories.size());++row){int i=visibleCategories[row];float y=lay::SidebarTop+static_cast<float>(row)*lay::SidebarStep;
  HIGHCONTRASTW hc{sizeof(hc)};const bool contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
  if(i==selected){const auto selection=D2D1::RoundedRect(D2D1::RectF(sp::M,y,lay::SidebarWidth-sp::M,y+lay::SidebarRow),ds::radius::Button,ds::radius::Button);brush->SetColor(contrast?C(colors.focused):C(0x326BEA));target->FillRoundedRectangle(selection,brush.Get());if(contrast||(GetFocus()==h&&keyboardFocusVisible)){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(selection,brush.Get(),1.f);}}
  const auto iconRect=D2D1::RectF(sp::Xl,y+6.f,sp::Xl+24.f,y+30.f);
  const unsigned badges[]={0x85858B,0x34343A,0x34343A,0x85858B,0x1688F8,0x85858B,0x62666F,0x1688F8,0x8D68C5,0x85858B,0xFA2D48};
  const char* keys[]={"settings.general","settings.appearance","settings.dock","settings.menu-bar","settings.launchpad","settings.control-center","settings.taskbar","settings.workshop","settings.migration","settings.about","settings.music"};
  const auto custom=settingsPackIcons.find(keys[i]);
  const bool screenshotMenuIcon=i==3&&(activeSettingsPack=="settings-reference-local-01"||activeSettingsPack=="settings-reference-local-02"||(activeSettingsPack.empty()&&activePack=="apple-combined-2026"));
  if(contrast||screenshotMenuIcon||custom==settingsPackIcons.end()||!DrawWorkshopPreview(custom->second,iconRect)){Round(iconRect,ds::radius::Button/2,contrast?colors.background:badges[i]);DrawCategoryGlyph(i,iconRect,contrast?colors.primary:colors.onAccent);}
  Text(titles[i],i==selected?sidebarSelected.Get():sidebarLabel.Get(),D2D1::RectF(sp::Xl*2+sp::Xs,y,lay::SidebarWidth-sp::L,y+lay::SidebarRow),i==selected?colors.onAccent:colors.primary);}
 target->SetTransform(D2D1::Matrix3x2F::Identity());target->PopAxisAlignedClip();
  if(GetTickCount64()<scrollActiveUntil)scrollbar(lay::SidebarWidth-7,110,ht-118,lay::SidebarTop+visibleCategories.size()*lay::SidebarStep+12-110,sidebarScroll.Position());
 };
 if(drawingChromeGlass&&allowTransparency){
  dock::LocalGlassOverlay sidebar;
  sidebar.active=true;
  sidebar.cacheKey=3;
  sidebar.sampleBounds=D2D1::RectF(0,0,lay::SidebarWidth,ht);
  sidebar.plate=D2D1::RectF(-16,-16,lay::SidebarWidth,ht+16);
  sidebar.params=SettingsGlass(dock::GlassSurface::SettingsNavigation,dock::SettingsGlassShape::Sidebar);
  sidebar.params.radius=0;
  sidebar.press=0;
  auto finish=[sidebarLabels](ID2D1DeviceContext* dc){
   target=dc;
   if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xffffff),&brush)))sidebarLabels();
   brush.Reset();target=nullptr;
  };
  sidebar.after=finish;
  sidebar.fallback=finish;
  drawingChromeGlass->push_back(std::move(sidebar));
 }else sidebarLabels();
 auto searchLabels=[h,colors](){
  brush->SetColor(C(colors.secondary));target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(29,89),5.f,5.f),brush.Get(),1.4f);target->DrawLine(D2D1::Point2F(32.5f,92.5f),D2D1::Point2F(36.5f,96.5f),brush.Get(),1.4f);
  wchar_t query[128]{};GetWindowTextW(searchBox,query,128);
  const auto field=D2D1::RectF(45,80,liquid::design::settings::SidebarWidth-24,101);
  if(GetFocus()!=searchBox){Text(query[0]?query:L"Search",fontSmall.Get(),field,query[0]?colors.primary:colors.secondary);return;}
  ComPtr<IDWriteTextLayout> layout;const auto length=static_cast<UINT32>(wcslen(query));
  if(FAILED(writeFactory->CreateTextLayout(query,length,fontSmall.Get(),4096,21,&layout)))return;
  DWORD first=0,last=0;SendMessageW(searchBox,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
  DWRITE_HIT_TEST_METRICS hit{};float caret=0,unused=0,selection=0;
  layout->HitTestTextPosition(std::min<DWORD>(last,length),FALSE,&caret,&unused,&hit);
  layout->HitTestTextPosition(std::min<DWORD>(first,length),FALSE,&selection,&unused,&hit);
  const float x=field.left-std::max(0.f,caret-(field.right-field.left-2));
  target->PushAxisAlignedClip(field,D2D1_ANTIALIAS_MODE_ALIASED);
  if(first!=last)Fill(D2D1::RectF(x+std::min(selection,caret),field.top,x+std::max(selection,caret),field.bottom-2),colors.selected);
  if(!length)Text(L"Search",fontSmall.Get(),D2D1::RectF(field.left+3,field.top,field.right,field.bottom),colors.secondary);
  brush->SetColor(C(colors.primary));target->DrawTextLayout(D2D1::Point2F(x,field.top),layout.Get(),brush.Get());
  if(first==last)target->DrawLine(D2D1::Point2F(x+caret,field.top+1),D2D1::Point2F(x+caret,field.bottom-3),brush.Get(),1);
  target->PopAxisAlignedClip();
 };
 ChromeGlass(D2D1::RectF(12,74,lay::SidebarWidth-12,106),dock::GlassSurface::SettingsSearch,searchLabels);
 if(visibleCategories.empty()){
  Text(L"No Results",heading.Get(),D2D1::RectF(left,lay::ContentTop,right,lay::ContentTop+sp::Xxl+sp::S),colors.primary);
  Text(L"Try another search term.",fontSmall.Get(),D2D1::RectF(left,lay::CardTop,right,lay::CardTop+sp::Xxl),colors.secondary);return;
 }
 // The navigation buttons operate on actual visited categories, including keyboard navigation.
 const float nav=lay::SidebarWidth+10.f;
 auto navLabels=[nav,colors](){
 brush->SetColor(C(colors.secondary,.3f));target->DrawLine(D2D1::Point2F(nav+44,19),D2D1::Point2F(nav+44,43),brush.Get(),.7f);
 for(int i=0;i<2;++i){const bool enabled=i==0?categoryHistoryIndex>0:categoryHistoryIndex+1<categoryHistory.size();const float cx=nav+22.f+i*44.f,sign=i==0?-1.f:1.f;brush->SetColor(C(enabled?colors.primary:colors.secondary,enabled?1.f:.4f));target->DrawLine(D2D1::Point2F(cx-sign*3.5f,24),D2D1::Point2F(cx+sign*3.5f,31),brush.Get(),2.f);target->DrawLine(D2D1::Point2F(cx+sign*3.5f,31),D2D1::Point2F(cx-sign*3.5f,38),brush.Get(),2.f);}
 };
 ChromeGlass(D2D1::RectF(nav,10,nav+88,52),dock::GlassSurface::SettingsNavigation,navLabels);
 Text(titles[selected],normal.Get(),D2D1::RectF(nav+102,18,right,47),colors.primary);
 target->PushAxisAlignedClip(D2D1::RectF(lay::SidebarWidth,60,w,ht),D2D1_ANTIALIAS_MODE_ALIASED);
 bodyShift=BodyShift();target->SetTransform(D2D1::Matrix3x2F::Translation(0,bodyShift));
 if(selected==0){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+166),SettingsCardRadius,colors.surface);
  Round(D2D1::RectF(left+14,top+18,left+48,top+52),8.f,chromeContrast?colors.background:0x7D8088);
  if(categoryIcons)Text(L"\uE713",categoryIcons.Get(),D2D1::RectF(left+14,top+18,left+48,top+52),chromeContrast?colors.primary:0xFFFFFF);
  Text(L"Windows visual effects",normal.Get(),D2D1::RectF(left+58,top+12,right-170,top+36),colors.primary);
  Text(L"Transparency and animation",fontSmall.Get(),D2D1::RectF(left+58,top+37,right-170,top+59),colors.secondary);
  const auto button=D2D1::RectF(right-158,top+20,right-12,top+50);Round(button,ds::radius::Button,colors.background);
  if(GetFocus()==h){brush->SetColor(C(colors.focused));target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(button.left-2,button.top-2,button.right+2,button.bottom+2),ds::radius::Button,ds::radius::Button),brush.Get(),2.f);}
  Text(L"Windows Settings…",centerSmall.Get(),button,colors.primary);
  brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,top+76),D2D1::Point2F(right-16,top+76),brush.Get(),1);
  brush->SetColor(C(colors.secondary,0.18f));target->DrawLine(D2D1::Point2F(left+16,top+124),D2D1::Point2F(right-16,top+124),brush.Get(),1.f);
  Text(L"System transparency",normal.Get(),D2D1::RectF(left+16,top+90,right-150,top+120),colors.primary);
  Text(allowTransparency?L"Allowed":L"Reduced",trailingSmall.Get(),D2D1::RectF(right-150,top+82,right-16,top+124),colors.secondary);
  BOOL motion=TRUE;const bool motionKnown=SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&motion,0)!=FALSE;
  Text(L"Animations",normal.Get(),D2D1::RectF(left+16,top+132,right-150,top+160),colors.primary);
  Text(!motionKnown?L"Unavailable":motion?L"On":L"Reduced",trailingSmall.Get(),D2D1::RectF(right-150,top+124,right-16,top+166),colors.secondary);
  {const float c=top+178.f;Round(D2D1::RectF(left,c,right,c+230.f),SettingsCardRadius,colors.surface);
   Text(L"Frame rate",normal.Get(),D2D1::RectF(left+16,c+10,right-16,c+36),colors.primary);
   const wchar_t* rows[]={L"Animations",L"Capture"};const int values[]={perf.animationFps,perf.captureFps};
   const wchar_t* names[]={L"30",L"60",L"120",L"240",L"Unlimited"};
   for(int row=0;row<2;++row){const auto first=FrameCapButton(row,0,left,right);
    Text(rows[row],fontSmall.Get(),D2D1::RectF(left+16,first.top-24,right-16,first.top-3),colors.primary);
    for(int i=0;i<5;++i){const auto b=FrameCapButton(row,i,left,right);Round(b,ds::radius::Button,values[row]==kFrameCaps[i]?colors.selected:colors.background);
     Text(names[i],centerSmall.Get(),b,colors.primary);}}
   Text(L"Unlimited follows the display's refresh rate. Lower values save power; capture sets how often live glass backgrounds update.",fontSmall.Get(),D2D1::RectF(left+16,c+174,right-16,c+214),colors.secondary);}
  DrawSwitchRow(h,16,left,right,colors);DrawSwitchRow(h,17,left,right,colors);DrawSwitchRow(h,26,left,right,colors);
 }else if(selected==1){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+150),SettingsCardRadius,colors.surface);
  Text(L"Appearance",normal.Get(),D2D1::RectF(left+sp::L,top+26,AppearanceOption(left,right,0).left-8,top+63),colors.primary);
  DrawAppearanceOptions(h,left,right,colors);
  DrawSizeSlider(h,1,left,right,colors);DrawSizeSlider(h,2,left,right,colors);
  {const auto b=ApplicationsResetButton(right);const bool on=ApplicationsWindowResized();Round(b,ds::radius::Button,colors.background);
   Text(on?L"Reset Size":L"Default Size",centerSmall.Get(),b,on?colors.primary:colors.secondary);}DrawLanguageCard(h,left,right,colors);DrawApplicationsOrder(h,left,right,colors);
 }else if(selected==6){
  const float top=80,statusBottom=TaskbarStatusBottom();Round(D2D1::RectF(left,top,right,statusBottom),SettingsCardRadius,colors.surface);
  Text(L"Windows taskbar",normal.Get(),D2D1::RectF(left+16,top+12,right-16,top+38),colors.primary);
  float y=top+44;for(const auto& line:TaskbarStatusLines()){const float height=DetailHeight(line.c_str(),right-left-32);Text(line.c_str(),fontSmall.Get(),D2D1::RectF(left+16,y,right-16,y+height),colors.secondary);y+=height+6;}
  const bool docHidden=tbLive&&tbStatus.state==L"hidden",canToggle=tbLive&&(docHidden||tbStatus.guardian==L"ready");
  const wchar_t* labels[]={docHidden?L"Show taskbar":L"Hide taskbar",L"Restore taskbar",L"Windows settings…"};
  for(int i=0;i<3;++i){const auto r=TaskbarControlRect(i,left,right);const bool enabled=i!=0||canToggle;Round(r,ds::radius::Button,enabled?colors.surface:colors.background);Text(labels[i],centerSmall.Get(),r,enabled?colors.primary:colors.secondary);}
  const float groupTop=TaskbarPreferencesTop(),shortcutTop=FormFor(5,left,right).bottom,groupBottom=shortcutTop+88;
  Round(D2D1::RectF(left,groupTop,right,groupBottom),SettingsCardRadius,colors.surface);
  for(int id=3;id<=5;++id){const auto row=FormFor(id,left,right);if(id>3){brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,row.top),D2D1::Point2F(right-16,row.top),brush.Get(),1);}DrawSwitchRow(h,id,left,right,colors,false);}
  brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,shortcutTop),D2D1::Point2F(right-16,shortcutTop),brush.Get(),1);
  Text(L"Shortcut",normal.Get(),D2D1::RectF(left+16,shortcutTop+10,right-16,shortcutTop+34),colors.primary);
  const std::wstring shortcut=recordingHotkey?L"Press keys…":docky::hotkey::Text(static_cast<UINT>(taskbarPref.hotkeyModifiers),static_cast<UINT>(taskbarPref.hotkeyKey));
  const auto field=D2D1::RectF(left+16,shortcutTop+40,right-190,shortcutTop+72);Round(field,ds::radius::Button,recordingHotkey?colors.selected:colors.background);Text(shortcut.c_str(),centerSmall.Get(),field,colors.primary);
  for(int i=0;i<2;++i){const auto r=TaskbarControlRect(6+i,left,right);Round(r,ds::radius::Button,colors.background);Text(i==0?(recordingHotkey?L"Cancel":L"Record"):L"Clear",centerSmall.Get(),r,colors.primary);}
  const std::wstring feedback=!taskbarFeedback.empty()?taskbarFeedback:tbLive?tbStatus.message:L"";
  if(!feedback.empty())Text(feedback.c_str(),fontSmall.Get(),D2D1::RectF(left+16,groupBottom+12,right-16,groupBottom+12+DetailHeight(feedback.c_str(),right-left-32)),colors.secondary);
 }else if(selected==8 || selected==9){
  const float top=lay::CardTop;
  if(selected==8){const auto cards=MeasureMigration(left,right);
   for(int i=0;i<2;++i){const auto& card=cards[i];Round(card.bounds,SettingsCardRadius,colors.surface);
    Text(i==0?L"Back up a configuration":L"Import from MyDockFinder",normal.Get(),card.title,colors.primary);
    Text(i==0?migrationBackupDetail:migrationImportDetail,fontSmall.Get(),card.detail,colors.secondary);
    for(size_t j=0;j<card.buttons.size();++j){Round(card.buttons[j],ds::radius::Button,colors.background);Text(i==0?L"Choose file…":j==0?L"Import…":L"Replace everything…",centerSmall.Get(),card.buttons[j],colors.primary);}
   }
  }else{
   Round(D2D1::RectF(left,top,right,top+150),SettingsCardRadius,colors.surface);
   Text(L"DockOver",normal.Get(),D2D1::RectF(left+sp::L,top+sp::L,right-sp::L,top+44),colors.primary);
   Text(L"A Windows desktop Dock and menu bar.",fontSmall.Get(),D2D1::RectF(left+sp::L,top+48,right-sp::L,top+72),colors.secondary);
   const std::wstring ver=L"Version "+dock::json::FromUtf8(docky::update::CurrentVersion());
   Text(ver.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+76,right-sp::L,top+100),colors.primary);
   const std::wstring src=L"Updates from github.com/"+docky::update::Repository();
   Text(src.c_str(),fontSmall.Get(),D2D1::RectF(left+sp::L,top+104,right-sp::L,top+128),colors.secondary);
   // Software Update card.
   {const float t2=UpdateCardTop();const auto geometry=MeasureUpdateCard(left,right);const auto text=CurrentUpdateText();Round(geometry.card,SettingsCardRadius,colors.surface);
    Text(L"Software Update",normal.Get(),D2D1::RectF(left+16,t2+12,right-204,t2+38),colors.primary);Text(text.status.c_str(),fontSmall.Get(),geometry.status,colors.primary);if(!text.detail.empty())Text(text.detail.c_str(),fontSmall.Get(),geometry.detail,colors.secondary);
    if(updUi==UpdUi::Downloading&&updTotal.load()){const float ratio=std::min(1.f,float(updDone.load())/updTotal.load()),width=right-left-32,y=geometry.progressTop;Round(D2D1::RectF(left+16,y,right-16,y+6),3,colors.background);Round(D2D1::RectF(left+16,y,left+16+std::max(6.f,width*ratio),y+6),3,colors.selected);}
    const bool busy=updUi==UpdUi::Checking||updUi==UpdUi::Installing;const auto cb=UpdateCheckButton(right,t2);Round(cb,ds::radius::Button,colors.background);Text(updUi==UpdUi::Downloading?L"Cancel":L"Check for Updates",centerSmall.Get(),cb,busy?colors.secondary:colors.primary);
    if(updUi==UpdUi::Available){const auto ib=UpdateInstallButton(right,t2);Round(ib,ds::radius::Button,colors.selected);Text((L"Install "+dock::json::FromUtf8(updRelease.version)).c_str(),centerSmall.Get(),ib,colors.primary);}}
   Round(D2D1::RectF(left,FormFor(14,left,right).top,right,FormFor(15,left,right).bottom),SettingsCardRadius,colors.surface);
   DrawSwitchRow(h,14,left,right,colors,false);DrawSwitchRow(h,15,left,right,colors,false);
   brush->SetColor(C(colors.secondary,.14f));const float divider=FormFor(15,left,right).top;target->DrawLine(D2D1::Point2F(left+16,divider),D2D1::Point2F(right-16,divider),brush.Get(),1);

  }
 }else if(selected==7){
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+160),SettingsCardRadius,colors.surface);
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
  const float top=lay::CardTop;Round(D2D1::RectF(left,top,right,top+DensityCardHeight()),SettingsCardRadius,colors.surface);
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
   wchar_t preview[96]{};_snwprintf_s(preview,_TRUNCATE,L"Estimated grid: %d columns × %d rows",grid.columns,grid.rows);Text(preview,fontSmall.Get(),D2D1::RectF(left+sp::L,top+DensityCardHeight()-36,right-sp::L,top+DensityCardHeight()-10),colors.secondary);}
  DrawSwitchRow(h,9,left,right,colors);DrawSwitchRow(h,13,left,right,colors);
 }else if(selected==2){
  const auto form=MeasureDock(left,right);
  DrawSizeSlider(h,8,left,right,colors);
  Round(D2D1::RectF(left,form.rows[0].top,right,form.rows[4].bottom),SettingsCardRadius,colors.surface);
  Text(L"Downloads",fontSmall.Get(),D2D1::RectF(left+16,form.rows[5].top-28,right-16,form.rows[5].top-8),colors.secondary);
  Round(D2D1::RectF(left,form.rows[5].top,right,form.rows[7].bottom),SettingsCardRadius,colors.surface);
  for(int i=0;i<8;++i){const auto& row=form.rows[i];if(i!=0&&i!=5){brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,row.top),D2D1::Point2F(right-16,row.top),brush.Get(),1);}
   DrawFormText(row,dockRows[i].title,dockRows[i].detail,colors);
   if(i==5){const float y=row.control.top;
    const auto minus=D2D1::RectF(right-126,y,right-98,y+28),plus=D2D1::RectF(right-48,y,right-20,y+28);
    Round(minus,8,colors.background);Round(plus,8,colors.background);Text(L"−",centerSmall.Get(),minus,colors.primary);Text(L"+",centerSmall.Get(),plus,colors.primary);
    Text(std::to_wstring(dlCount).c_str(),centerSmall.Get(),D2D1::RectF(right-98,y,right-48,y+28),colors.primary);
   } else DrawSettingsSwitch(h,dockRows[i].id,row.control,SettingsSwitchValue(dockRows[i].id),switchPressed==dockRows[i].id,GetFocus()==h&&keyboardFocusVisible&&i==dockKeyboardRow,colors);
  }

 }else if(selected==3){
  DrawSizeSlider(h,0,left,right,colors);
  DrawSwitchRow(h,46,left,right,colors);
  if(menuBarThemeGradient){DrawSizeSlider(h,6,left,right,colors);DrawSizeSlider(h,7,left,right,colors);}
  {const auto row=FormFor(27,left,right);Round(D2D1::RectF(left,FormFor(18,left,right).top,right,row.bottom),SettingsCardRadius,colors.surface);
   DrawSwitchRow(h,18,left,right,colors,false);
   brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+12,row.top),D2D1::Point2F(right-12,row.top),brush.Get(),1);const auto spec=SpecFor(27);DrawFormText(row,spec.title,spec.detail,colors);
   const auto b=GamesListButton(left,right);Round(b,ds::radius::Button,colors.background);Text(L"Edit list...",centerSmall.Get(),b,colors.primary);}
  DrawSwitchRow(h,20,left,right,colors);
  {const auto a=FormFor(24,left,right),b=FormFor(25,left,right);Round(D2D1::RectF(left,a.top,right,b.bottom),SettingsCardRadius,colors.surface);
   DrawSwitchRow(h,24,left,right,colors,false);DrawSwitchRow(h,25,left,right,colors,false);
   brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,b.top),D2D1::Point2F(right-16,b.top),brush.Get(),1);}
  DrawSizeSlider(h,3,left,right,colors);DrawSizeSlider(h,4,left,right,colors);DrawSizeSlider(h,5,left,right,colors);
 }else if(selected==5){
  const auto row=CcBlurRow(left,right);Round(D2D1::RectF(left,row.top,right,row.bottom),SettingsCardRadius,colors.surface);
  DrawFormText(row,L"Background blur",L"Adjust how strongly the desktop behind Control Center is blurred. 0% turns it off.",colors);
  const float y=row.control.top;const auto minus=D2D1::RectF(right-126,y,right-98,y+28),plus=D2D1::RectF(right-48,y,right-20,y+28);Round(minus,8,colors.background);Round(plus,8,colors.background);Text(L"−",centerSmall.Get(),minus,colors.primary);Text(L"+",centerSmall.Get(),plus,colors.primary);Text((std::to_wstring(ccBlur)+L"%").c_str(),centerSmall.Get(),D2D1::RectF(right-98,y,right-48,y+28),colors.primary);
  DrawSwitchRow(h,19,left,right,colors);
 }else if(selected==10){
  {const auto a=FormFor(44,left,right),b=FormFor(29,left,right),c=FormFor(31,left,right),last=FormFor(51,left,right);Round(D2D1::RectF(left,a.top,right,last.bottom),SettingsCardRadius,colors.surface);
   DrawSwitchRow(h,44,left,right,colors,false);DrawSwitchRow(h,28,left,right,colors,false);DrawSwitchRow(h,29,left,right,colors,false);DrawSwitchRow(h,31,left,right,colors,false);DrawSwitchRow(h,50,left,right,colors,false);DrawSwitchRow(h,51,left,right,colors,false);
   brush->SetColor(C(colors.secondary,.14f));for(int id:{50,51}){const float ly=FormFor(id,left,right).top;target->DrawLine(D2D1::Point2F(left+16,ly),D2D1::Point2F(right-16,ly),brush.Get(),1);}
   brush->SetColor(C(colors.secondary,.14f));const float wallTop=FormFor(28,left,right).top;target->DrawLine(D2D1::Point2F(left+16,wallTop),D2D1::Point2F(right-16,wallTop),brush.Get(),1);target->DrawLine(D2D1::Point2F(left+16,b.top),D2D1::Point2F(right-16,b.top),brush.Get(),1);target->DrawLine(D2D1::Point2F(left+16,c.top),D2D1::Point2F(right-16,c.top),brush.Get(),1);}
  {const auto a=FormFor(32,left,right),ret=FormFor(33,left,right),row=FormFor(30,left,right);Round(D2D1::RectF(left,a.top,right,row.bottom),SettingsCardRadius,colors.surface);DrawSwitchRow(h,32,left,right,colors,false);DrawSwitchRow(h,33,left,right,colors,false);DrawSwitchRow(h,45,left,right,colors,false);DrawSwitchRow(h,38,left,right,colors,false);
   {const auto t=FormFor(39,left,right);const auto spec=SpecFor(39);DrawFormText(t,spec.title,spec.detail,colors);const float y=(t.top+t.bottom)*.5f-14;
    const auto minus=D2D1::RectF(right-136,y,right-108,y+28),plus=D2D1::RectF(right-48,y,right-20,y+28);Round(minus,8,colors.background);Round(plus,8,colors.background);
    Text(L"\u2212",centerSmall.Get(),minus,colors.primary);Text(L"+",centerSmall.Get(),plus,colors.primary);
    wchar_t value[24];swprintf_s(value,L"%+.1f s",lyricsOffsetMs/1000.0);Text(value,centerSmall.Get(),D2D1::RectF(right-108,y,right-48,y+28),colors.primary);
    brush->SetColor(C(colors.secondary,.14f));for(int id:{45,38,39}){const float ly=FormFor(id,left,right).top;target->DrawLine(D2D1::Point2F(left+16,ly),D2D1::Point2F(right-16,ly),brush.Get(),1);}}
   brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,ret.top),D2D1::Point2F(right-16,ret.top),brush.Get(),1);
   brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,row.top),D2D1::Point2F(right-16,row.top),brush.Get(),1);const auto spec=SpecFor(30);DrawFormText(row,spec.title,spec.detail,colors);
   const auto b=LyricsFolderButton(left,right);Round(b,ds::radius::Button,colors.background);Text(L"Open folder...",centerSmall.Get(),b,colors.primary);}
  {const auto first=FormFor(34,left,right),last=FormFor(52,left,right);Round(D2D1::RectF(left,first.top,right,last.bottom),SettingsCardRadius,colors.surface);
   DrawSwitchRow(h,52,left,right,colors,false);brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,last.top),D2D1::Point2F(right-16,last.top),brush.Get(),1);
   for(int i=0;i<4;++i){const auto row=FormFor(34+i,left,right);const auto spec=SpecFor(34+i);DrawFormText(row,spec.title,spec.detail,colors);
    if(i){brush->SetColor(C(colors.secondary,.14f));target->DrawLine(D2D1::Point2F(left+16,row.top),D2D1::Point2F(right-16,row.top),brush.Get(),1);}
    const auto b=ShortcutButton(i,left,right);const int v=shortcutValues[i];
    Round(b,ds::radius::Button,recordingShortcut==i?colors.selected:colors.background);
    Text(recordingShortcut==i?L"Type a shortcut...":v?docky::hotkey::Text(UINT(v>>8),UINT(v&0xFF)).c_str():L"None",centerSmall.Get(),b,colors.primary);}}
  {const auto row=FormFor(40,left,right);Round(D2D1::RectF(left,row.top,right,row.bottom),SettingsCardRadius,colors.surface);const auto spec=SpecFor(40);DrawFormText(row,spec.title,spec.detail,colors);
   for(int i=0;i<5;++i){const auto b=FpsButton(i,left,right);const bool on=musicFps==kMusicFps[i];Round(b,8,on?0x007AFF:colors.background);
    Text(i==4?L"Unlimited":std::to_wstring(kMusicFps[i]).c_str(),centerSmall.Get(),b,on?0xFFFFFF:colors.primary);}}
  {const auto row=FormFor(53,left,right);Round(D2D1::RectF(left,row.top,right,row.bottom),SettingsCardRadius,colors.surface);
   const auto spec=SpecFor(53);DrawFormText(row,spec.title,spec.detail,colors);
   for(int i=0;i<3;++i){const auto b=ResponseButton(i,left,right);const bool active=musicResponse==i;
    Round(b,8,active?0x007AFF:colors.background);
    Text(kMusicResponses[i],centerSmall.Get(),b,active?0xFFFFFF:colors.primary);}}
  DrawSizeSlider(h,9,left,right,colors);
 }else{
 const float cardBottom=lay::CardTop+(selected==2?lay::CardHeight:lay::CardHeight+sp::S);
 Round(D2D1::RectF(left,lay::CardTop,right,cardBottom),SettingsCardRadius,colors.surface);
 Text(selected==2?L"Show Dock diagnostics":L"More settings coming soon",normal.Get(),
      D2D1::RectF(left+sp::L,lay::CardTop+sp::L,right-(selected==2?sp::Xxl*3:sp::L),lay::CardTop+sp::L+sp::Xxl),colors.primary);
 Text(selected==2?L"Applies when the Dock restarts.":L"Additional controls will appear here when available.",fontSmall.Get(),
      D2D1::RectF(left+sp::L,lay::CardTop+sp::Xl+sp::L,right-(selected==2?sp::Xxl*3:sp::L),cardBottom-sp::Xs),colors.secondary);

 }
 target->SetTransform(D2D1::Matrix3x2F::Identity());target->PopAxisAlignedClip();bodyShift=0;
 if(GetTickCount64()<scrollActiveUntil){
  scrollbar(w-7,64,ht-72,SettingsContentBottom()+16-64,contentScroll[selected].Position());
 }
}
void DisableGpu(){
 if(settingsWindowHandle){const auto ex=GetWindowLongPtrW(settingsWindowHandle,GWL_EXSTYLE);SetWindowLongPtrW(settingsWindowHandle,GWL_EXSTYLE,ex&~WS_EX_NOREDIRECTIONBITMAP);SetWindowPos(settingsWindowHandle,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);}
 previewCache.clear();previewCacheTarget=nullptr;
 if(renderer){renderer->Shutdown();renderer.reset();}
 gpuReady=false;target=nullptr;brush.Reset();
}
void EnsureKeyboardControl(HWND h){
 if(!keyboardFocusVisible||GetFocus()!=h)return;
 namespace lay=liquid::design::settings;
 float top=lay::CardTop,bottom=top+60;
 if(selected==2){const auto row=CurrentDockForm().rows[dockKeyboardRow];top=row.top;bottom=row.bottom;}
 else if(selected==1&&appearanceKeyboard>=3){top=appearanceKeyboard>=7?710:appearanceKeyboard>=5?558:SizeSpec(appearanceKeyboard-2).top;bottom=top+(appearanceKeyboard>=7?112:146);}
 else if(selected==3){const int id=menuKeyboardControl==1?46:18;top=menuKeyboardControl==0?80:CurrentForm(id).top;bottom=menuKeyboardControl==0?80+MenuSizeRowHeight:CurrentForm(id).bottom;}
 else if(selected==6){const auto [left,right]=CurrentContentEdges();const auto r=TaskbarControlRect(taskbarKeyboardControl,left,right);top=r.top;bottom=r.bottom;}
 else if(selected==4){top=lay::CardTop+(densityKeyboardControl>=4?60+((densityKeyboardControl-4)/2)*36:16);bottom=top+40;}
 else if(selected==5){top=CurrentForm(19).top;bottom=CurrentForm(19).bottom;}
 else if(selected==10&&activeSizeSlider==9){top=SizeSpec(9).top;bottom=top+146;}
 else if(selected==7){top=lay::CardTop+(workshopKeyboardControl>=2?228+((workshopKeyboardControl-2)/3)*40:168);bottom=top+40;}
 else if(selected==9){top=UpdateCardTop()+22;bottom=top+32;}
 RECT rc{};GetClientRect(h,&rc);const float old=contentScroll[selected].Target();
 contentScroll[selected].Ensure(top,bottom,64,rc.bottom/scale-16);
 if(old!=contentScroll[selected].Target())StartScroll(h);
}
void Draw(HWND h){
 UpdateScrollBounds(h);EnsureKeyboardControl(h);dock::GlassSettingsStore::Get().ReloadIfChanged();
 if(gpuReady&&renderer){
  RECT client{};GetClientRect(h,&client);if(client.right<=0||client.bottom<=0)return;
  dock::DockFrame geometry;geometry.dockLeft=0;geometry.dockTop=0;
  geometry.dockRight=liquid::design::settings::SidebarWidth;geometry.dockBottom=client.bottom/scale;
  dock::LocalGlassOverlay localGlass;std::vector<dock::LocalGlassOverlay> chromeGlass;
  dock::Scene scene;scene.frame=&geometry;
  scene.localGlass=&localGlass;scene.chromeGlass=&chromeGlass;
  scene.customDraw=[h,&localGlass,&chromeGlass](ID2D1DeviceContext* dc,bool){
   target=dc;brush.Reset();if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xffffff),&brush))){
    DrawWindowOutline(h,false); RECT bounds{};GetClientRect(h,&bounds);
    BOOL contains=FALSE;const bool candidate=!IsZoomed(h)&&windowOutline;
    const bool mask=candidate&&SUCCEEDED(windowOutline->FillContainsPoint(D2D1::Point2F(bounds.right/(2*scale),bounds.bottom/(2*scale)),nullptr,&contains))&&contains;
    if(mask)dc->PushLayer(D2D1::LayerParameters(D2D1::RectF(0,0,bounds.right/scale,bounds.bottom/scale),windowOutline.Get(),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),nullptr);
    drawingLocalGlass=renderer->LiquidAvailable()?&localGlass:nullptr;drawingChromeGlass=renderer->LiquidAvailable()?&chromeGlass:nullptr;DrawContent(h);drawingLocalGlass=nullptr;drawingChromeGlass=nullptr;DrawWindowOutline(h);
    if(mask)dc->PopLayer();}
   brush.Reset();target=nullptr;
  };
  localGlass.fallback=[&localGlass](ID2D1DeviceContext* dc){ComPtr<ID2D1SolidColorBrush> b;
   if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xFFFFFF),&b))){const float rad=(localGlass.plate.bottom-localGlass.plate.top)*.5f;dc->FillRoundedRectangle(D2D1::RoundedRect(localGlass.plate,rad,rad),b.Get());}};
  localGlass.after=[&localGlass](ID2D1DeviceContext* dc){if(localGlassWhite<=.001f)return;ComPtr<ID2D1SolidColorBrush> b;
   if(SUCCEEDED(dc->CreateSolidColorBrush(C(0xFFFFFF,localGlassWhite),&b))){const float rad=(localGlass.plate.bottom-localGlass.plate.top)*.5f;dc->FillRoundedRectangle(D2D1::RoundedRect(localGlass.plate,rad,rad),b.Get());}};
  if(!snapshotPath.empty()){renderer->RequestSnapshot(snapshotPath);snapshotPath.clear();}
  const HRESULT hr=renderer->Render(scene);
  if(SUCCEEDED(hr))return;
  DLOG(L"settings: GPU render failed; switching to opaque fallback");DisableGpu();
 }
 if(!CreateResources(h))return;target->BeginDraw();DrawContent(h);DrawWindowOutline(h);
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
 if(m==docky::vblank::kMsgVBlank){switchTicker.Ack();switchTicker.Stop();const auto now=GetTickCount64();const double dt=std::min(.05,(now-lastScrollTick)/1000.0);lastScrollTick=now;
  bool moving=sidebarScroll.Step(dt,ReducedMotion());for(auto& scroll:contentScroll)moving=scroll.Step(dt,ReducedMotion())||moving;
  if(moving)switchTicker.Start(h,docky::vblank::kMsgVBlank);InvalidateRect(h,nullptr,FALSE);return 0;}  // the painter re-arms it while switches animate
 switch(m){
 case WM_NCCALCSIZE:
  if(w){if(IsZoomed(h)){auto* p=reinterpret_cast<NCCALCSIZE_PARAMS*>(l);const UINT dpi=GetDpiForWindow(h);const int x=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi),y=GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);InflateRect(&p->rgrc[0],-x,-y);}return 0;}break;
 case WM_NCHITTEST:{
  RECT r{};GetWindowRect(h,&r);const int x=GET_X_LPARAM(l)-r.left,y=GET_Y_LPARAM(l)-r.top,wid=r.right-r.left,hei=r.bottom-r.top;const float k=GetDpiForWindow(h)/96.f;const int border=(std::max)(4,static_cast<int>(7*k));
  if(!IsZoomed(h)){const bool left=x<border,right=x>=wid-border,top=y<border,bottom=y>=hei-border;if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;}
  const float dx=x/scale,dy=y/scale,nav=liquid::design::settings::SidebarWidth+10.f;if(dy<60.f){if(ChromeButtonAt(dx,dy)>=0||(dx>=nav&&dx<nav+88&&dy>=10&&dy<52))return HTCLIENT;return HTCAPTION;}return HTCLIENT;}
 case WM_MOUSEMOVE:{switchPointerX=GET_X_LPARAM(l)/scale;switchPointerY=GET_Y_LPARAM(l)/scale;
  if(menuSliderDragging){UpdateMenuSlider(h,switchPointerX);return 0;}
  if(switchPressed>=0){const float delta=switchPointerX-switchDragStartX;const float threshold=GetSystemMetricsForDpi(SM_CXDRAG,GetDpiForWindow(h))/scale;
   if(!switchDragging&&std::fabs(delta)>=threshold)switchDragging=true;
   if(switchDragging)switchDragPosition=std::clamp(switchDragStartPosition+delta/14.4f,0.f,1.f);InvalidateRect(h,nullptr,FALSE);}const int next=ChromeButtonAt(switchPointerX,switchPointerY);if(next!=chromeHover){chromeHover=next;InvalidateRect(h,nullptr,FALSE);}if(!chromeTracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);chromeTracking=true;}break;}
 case WM_MOUSELEAVE:chromeTracking=false;if(chromeHover!=-1){chromeHover=-1;InvalidateRect(h,nullptr,FALSE);}return 0;
 case WM_LBUTTONDOWN:{keyboardFocusVisible=false;const float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;switchPointerX=x;switchPointerY=y;const int button=ChromeButtonAt(x,y);if(button>=0){chromePressed=button;SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}
  if(x>=12&&x<liquid::design::settings::SidebarWidth-12&&y>=74&&y<106){FocusSearch();return 0;}
  if(GetFocus()==searchBox)SetFocus(h);
  if(selected==1){const auto [left,right]=CurrentContentEdges();const float by=y-BodyShift();const auto b=ApplicationsResetButton(right);
   if(x>=b.left&&x<b.right&&by>=b.top&&by<b.bottom){if(ApplicationsWindowResized()&&!DeleteFileW(ApplicationsWindowSizePath().c_str()))MessageBoxW(h,docky::ui::Text(L"Could not reset the Applications window size."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);InvalidateRect(h,nullptr,FALSE);return 0;}}
  if(selected==3||selected==1||selected==2||selected==10){const auto [left,right]=CurrentContentEdges();const float by=y-BodyShift();
   for(int index:selected==10?std::vector<int>{9}:selected==2?std::vector<int>{8}:selected==3?(menuBarThemeGradient?std::vector<int>{0,6,7,3,4,5}:std::vector<int>{0,3,4,5}):std::vector<int>{1,2}){const auto track=SizeSliderTrack(index,left,right);const auto spec=SizeSpec(index);const float cx=track.left+(track.right-track.left)*(SizeValue(index)-spec.lo)/float(spec.hi-spec.lo);
    if(x>=track.left-22&&x<=track.right+22&&by>=track.top-18&&by<=track.bottom+18){SetFocus(h);activeSizeSlider=index;if(index==0)menuKeyboardControl=0;else if(index<=2)appearanceKeyboard=index+2;menuDragStart=SizeValue(index);menuSliderDragging=true;BeginSwitchPress(h,40+index,true);menuSliderGrab=std::fabs(x-cx)<=20?x-cx:0;SetCapture(h);UpdateMenuSlider(h,x);InvalidateRect(h,nullptr,FALSE);return 0;}}}

  const int sw=SettingsSwitchAt(h,x,y);if(sw>=0){SetFocus(h);if(sw==46)menuKeyboardControl=1;else if(sw==18)menuKeyboardControl=2;switchPressed=sw;switchDragging=false;switchDragStartX=x;switchDragStartPosition=SettingsSwitchValue(sw)?1.f:0.f;switchDragPosition=switchDragStartPosition;BeginSwitchPress(h,sw,true);SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}break;}
 case WM_CAPTURECHANGED:if(menuSliderDragging){menuSliderDragging=false;BeginSwitchPress(h,40+activeSizeSlider,false);SizeValue(activeSizeSlider)=menuDragStart;PreviewMenuScale();InvalidateRect(h,nullptr,FALSE);}if(chromePressed>=0||switchPressed>=0){if(switchPressed>=0){if(switchDragging){auto& anim=switchMotion[switchPressed];const bool value=SettingsSwitchValue(switchPressed);anim.initialized=true;anim.on=value;anim.from=switchDragPosition;anim.to=value?1.f:0.f;anim.start=GetTickCount64();}BeginSwitchPress(h,switchPressed,false);}chromePressed=-1;switchPressed=-1;switchDragging=false;InvalidateRect(h,nullptr,FALSE);}break;
 case WM_CREATE: settingsWindowHandle=h;Refresh();scale=GetDpiForWindow(h)/96.f*settingsUiPercent/100.f;RefreshWorkshop();RefreshTaskbarState();RefreshTaskbarStatus();
  try {systemUi=winrt::Windows::UI::ViewManagement::UISettings();} catch(...) {systemUi=nullptr;} RefreshAccessibility();RefreshSystemAppearance();
  {WNDCLASSW host{};host.hInstance=GetModuleHandleW(nullptr);host.lpfnWndProc=SearchHostProc;host.lpszClassName=L"DockOverSettingsSearchHost";host.hCursor=LoadCursorW(nullptr,IDC_IBEAM);RegisterClassW(&host);
   searchHost=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT,host.lpszClassName,L"Settings search",WS_POPUP|WS_CLIPCHILDREN,0,0,0,0,h,nullptr,host.hInstance,nullptr);}
  if(searchHost)SetLayeredWindowAttributes(searchHost,0,0,LWA_ALPHA);
  searchBox=CreateWindowExW(0,L"EDIT",L"",WS_VISIBLE|WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,searchHost,reinterpret_cast<HMENU>(1001),GetModuleHandleW(nullptr),nullptr);
  if(searchBox){originalSearchProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(searchBox,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(SearchProc)));UpdateSearchFont();SendMessageW(searchBox,EM_SETLIMITTEXT,127,0);SendMessageW(searchBox,0x1501,TRUE,reinterpret_cast<LPARAM>(docky::ui::Text(L"Search Settings")));PositionSearch();}
  ApplyNativeAppearance(h);SetTimer(h,1,1000,nullptr);return 0;
 case WM_CTLCOLOREDIT: if(reinterpret_cast<HWND>(l)==searchBox&&searchBrush){SetTextColor(reinterpret_cast<HDC>(w),searchText);SetBkColor(reinterpret_cast<HDC>(w),searchBackground);return reinterpret_cast<LRESULT>(searchBrush);}break;
 case WM_SYSCOLORCHANGE: ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);break;
 case WM_COMMAND: if(LOWORD(w)==1001&&HIWORD(w)==EN_CHANGE){UpdateSearch(h);return 0;}break;
 case WM_SYSKEYDOWN:  // Alt combinations while recording a shortcut; otherwise navigation history
  if(recordingShortcut>=0&&selected==10){RecordShortcutKey(h,static_cast<UINT>(w));return 0;}
  if(recordingHotkey&&selected==6){RecordHotkeyKey(h,static_cast<UINT>(w));return 0;}
  if(w==VK_LEFT||w==VK_RIGHT){StepHistory(h,w==VK_LEFT?-1:1);return 0;}break;
 case WM_SYSCHAR: if(recordingHotkey||recordingShortcut>=0)return 0;break;  // no menu beep while recording
 case WM_CHAR: if(recordingHotkey||recordingShortcut>=0)return 0;break;
 case WM_KEYDOWN:
  keyboardFocusVisible=true;
  if(recordingShortcut>=0&&selected==10){RecordShortcutKey(h,static_cast<UINT>(w));return 0;}
  if(recordingHotkey&&selected==6){RecordHotkeyKey(h,static_cast<UINT>(w));return 0;}
  if(w=='F'&&(GetKeyState(VK_CONTROL)&0x8000)){if(searchBox)FocusSearch();return 0;}
  if(visibleCategories.empty())return 0;
  if(w==VK_DOWN||w==VK_UP){MoveCategory(h,w==VK_DOWN?1:-1);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==5){if(liquid::settings::WriteBool(settingsPath,"controlCenter","mediaArtworkBackground",!mediaArtworkBackground)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();InvalidateRect(h,nullptr,FALSE);return 0;}
  if((selected==10||(selected==3&&menuKeyboardControl==0)||(selected==1&&appearanceKeyboard>=3&&appearanceKeyboard<=4))&&(w==VK_LEFT||w==VK_RIGHT||w==VK_HOME||w==VK_END)){
   activeSizeSlider=selected==10?9:selected==3?0:appearanceKeyboard-2;const auto spec=SizeSpec(activeSizeSlider);auto& value=SizeValue(activeSizeSlider);value=w==VK_HOME?spec.lo:w==VK_END?spec.hi:std::clamp(value+(w==VK_LEFT?-1:1)*((GetKeyState(VK_SHIFT)&0x8000)?10:1),spec.lo,spec.hi);PreviewMenuScale();SaveMenuScale(h);return 0;}

  if(w==VK_ESCAPE&&menuSliderDragging){menuSliderDragging=false;BeginSwitchPress(h,40+activeSizeSlider,false);SizeValue(activeSizeSlider)=menuDragStart;PreviewMenuScale();ReleaseCapture();InvalidateRect(h,nullptr,FALSE);return 0;}
  if(w==VK_TAB&&selected==3){const bool back=(GetKeyState(VK_SHIFT)&0x8000)!=0;if((back&&menuKeyboardControl==0)||(!back&&menuKeyboardControl==2)){if(searchBox)FocusSearch();}else menuKeyboardControl+=back?-1:1;InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==3&&menuKeyboardControl>0){const bool gradient=menuKeyboardControl==1;if(liquid::settings::WriteBool(settingsPath,gradient?"menuBar":"dock",gradient?"themeGradient":"gameCompatibility",gradient?!menuBarThemeGradient:!gameCompatibility)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();InvalidateRect(h,nullptr,FALSE);return 0;}
  if(w==VK_TAB&&selected==1){const bool back=(GetKeyState(VK_SHIFT)&0x8000)!=0;if((back&&appearanceKeyboard==0)||(!back&&appearanceKeyboard==9)){if(searchBox)FocusSearch();}else appearanceKeyboard+=back?-1:1;InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==1&&appearanceKeyboard>=7){SetApplicationsOrder(h,appearanceKeyboard-7);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==1&&appearanceKeyboard>=5){SetLanguage(h,appearanceKeyboard-5);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==1&&appearanceKeyboard<3){SetAppearance(h,appearanceKeyboard);return 0;}
  if(w==VK_TAB && selected==2){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;if((backwards&&dockKeyboardRow==0)||(!backwards&&dockKeyboardRow==4)){if(searchBox)FocusSearch();}else dockKeyboardRow+=backwards?-1:1;InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==6){ActivateTaskbarControl(h,taskbarKeyboardControl);return 0;}
  if(w==VK_TAB&&selected==6){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;
   if((backwards&&taskbarKeyboardControl==0)||(!backwards&&taskbarKeyboardControl==kTaskbarControls-1)){if(searchBox)FocusSearch();}else taskbarKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==0){OpenVisualEffects(h);return 0;}
  if(w==VK_TAB&&selected==0){if(searchBox)FocusSearch();return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==2){ToggleDockSetting(h,dockKeyboardRow);return 0;}
  if(w==VK_TAB&&selected==4){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;const int count=density.densityPreset==3?10:4;
   densityKeyboardControl=std::clamp(densityKeyboardControl,0,count-1);
   if((backwards&&densityKeyboardControl==0)||(!backwards&&densityKeyboardControl==count-1)){if(searchBox)FocusSearch();}else densityKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==4){ActivateDensity(h,densityKeyboardControl);return 0;}
  if(selected==7&&(w==VK_LEFT||w==VK_RIGHT)&&!packs.empty()){
   packSelected=std::clamp(packSelected+(w==VK_LEFT?-1:1),0,static_cast<int>(packs.size())-1);
   if(packSelected<packOffset)packOffset=packSelected;else if(packSelected>=packOffset+3)packOffset=packSelected-2;
   InvalidateRect(h,nullptr,FALSE);return 0;
  }
  if(w==VK_TAB&&selected==7){const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;
   if((backwards&&workshopKeyboardControl==0)||(!backwards&&workshopKeyboardControl==6)){if(searchBox)FocusSearch();}else workshopKeyboardControl+=backwards?-1:1;
   InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_SPACE||w==VK_RETURN)&&selected==7){
   namespace lay=liquid::design::settings;RECT client{};GetClientRect(h,&client);const float left=ContentLeftForWidth(client.right/scale),right=ContentRightForWidth(client.right/scale,left),top=lay::CardTop;
   float x=left+24,y=top+182;
   if(workshopKeyboardControl==1)x=left+80;
   if(workshopKeyboardControl>=2){const int index=workshopKeyboardControl-2,row=index<3?0:1,col=index<3?index:index-3;const float bw=(right-left-16)/3.f;x=left+col*(bw+8)+bw*.5f;y=top+228+row*40.f;}
   SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(static_cast<int>(x*scale),static_cast<int>((y+BodyShift())*scale)));return 0;
  }
  break;
 case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h,nullptr,FALSE);break;
 case WM_PAINT:{PAINTSTRUCT p{};BeginPaint(h,&p);Draw(h);EndPaint(h,&p);return 0;}
 case WM_ERASEBKGND:return 1;
 case WM_SIZE:
  PositionSearch();UpdateScrollBounds(h);
  sidebarFirst=std::clamp(sidebarFirst,0,std::max(0,static_cast<int>(visibleCategories.size())-SidebarCapacity(h)));
  if(gpuReady&&renderer&&w!=SIZE_MINIMIZED&&LOWORD(l)&&HIWORD(l)){
   if(FAILED(renderer->Resize(LOWORD(l),HIWORD(l),96*scale))){DLOG(L"settings: resize failed; opaque fallback");DisableGpu();}
   InvalidateRect(h,nullptr,FALSE);
  }else if(fallbackTarget){fallbackTarget->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));InvalidateRect(h,nullptr,FALSE);}
  UpdateWindowContour(h,static_cast<UINT>(w));
  return 0;
 case WM_MOVE:PositionSearch();return 0;
 case WM_GETMINMAXINFO:{auto* p=reinterpret_cast<MINMAXINFO*>(l);p->ptMinTrackSize.x=static_cast<LONG>(liquid::design::settings::MinWidth*scale);p->ptMinTrackSize.y=static_cast<LONG>(liquid::design::settings::MinHeight*scale);
  const HMONITOR monitor=MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};HWND bar=FindWindowW(L"LiquidDesktopMenuBarV01",nullptr);RECT br{};
  if(bar&&IsWindowVisible(bar)&&GetWindowRect(bar,&br)&&GetMonitorInfoW(monitor,&mi)&&MonitorFromRect(&br,MONITOR_DEFAULTTONEAREST)==monitor&&br.top<=mi.rcWork.top&&br.bottom>mi.rcWork.top&&br.bottom<mi.rcWork.bottom){
   const UINT dpi=GetDpiForWindow(h);const int fx=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi),fy=GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
   p->ptMaxPosition.x=mi.rcWork.left-mi.rcMonitor.left-fx;p->ptMaxPosition.y=br.bottom-mi.rcMonitor.top-fy;
   p->ptMaxSize.x=mi.rcWork.right-mi.rcWork.left+2*fx;p->ptMaxSize.y=mi.rcWork.bottom-br.bottom+2*fy;
  }return 0;}
 case WM_DPICHANGED:{scale=HIWORD(w)/96.f*settingsUiPercent/100.f;UpdateSearchFont();PositionSearch();auto* r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);if(fallbackTarget)fallbackTarget->SetDpi(96*scale,96*scale);InvalidateRect(h,nullptr,FALSE);return 0;}
 case kMsgUpdate:{
  if(w==1){std::unique_ptr<docky::update::CheckResult> r(reinterpret_cast<docky::update::CheckResult*>(l));
   updState=docky::update::LoadState();updRelease=r->release;updError=r->error;
   updUi=r->status==docky::update::CheckStatus::Available?UpdUi::Available:r->status==docky::update::CheckStatus::UpToDate?UpdUi::UpToDate:r->status==docky::update::CheckStatus::NoRelease?UpdUi::NoRelease:UpdUi::Error;
   DLOG(L"settings: update check status=%d version=%hs",static_cast<int>(r->status),r->release.version.c_str());}
  else if(w==2){std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(l));
   if(err->empty()){updUi=UpdUi::Installing;InvalidateRect(h,nullptr,FALSE);UpdateWindow(h);DLOG(L"settings: update %hs downloaded; installer started, closing Settings",updRelease.version.c_str());PostMessageW(h,WM_CLOSE,0,0);}
   else{updUi=updCancel.load()?UpdUi::Available:UpdUi::Error;updError=*err;DLOG(L"settings: update failed: %ls",err->c_str());}}
  InvalidateRect(h,nullptr,FALSE);return 0;}
 case WM_TIMER:{if(w==38&&!referenceCaptureDir.empty()){
  ++referenceCaptureFrame;
  if(referenceLayoutOnly&&referenceCaptureFrame>=3){KillTimer(h,38);PostMessageW(h,WM_CLOSE,0,0);return 0;}
  if(referenceCaptureFrame==20){contentScroll[selected].By(1000);StartScroll(h);}
  if(referenceCaptureFrame==80){sidebarScroll.By(1000);StartScroll(h);}
  if(referenceCaptureFrame==140){contentScroll[selected].By(-1000);sidebarScroll.By(-1000);StartScroll(h);}
  for(int f:{1,21,24,30,50,82,90,110,142,150,175})if(referenceCaptureFrame==f)snapshotPath=referenceCaptureDir+L"\\settings-"+std::to_wstring(f)+L".bmp";
  if(referenceCaptureFrame>=190){KillTimer(h,38);PostMessageW(h,WM_CLOSE,0,0);}
  InvalidateRect(h,nullptr,FALSE);return 0;
 }if(w==kScrollTimer){if(GetTickCount64()>scrollActiveUntil&&!sidebarScroll.Animating()&&!contentScroll[selected].Animating())KillTimer(h,kScrollTimer);else switchTicker.Start(h,docky::vblank::kMsgVBlank);InvalidateRect(h,nullptr,FALSE);return 0;}if(w==4){KillTimer(h,4);InvalidateRect(h,nullptr,FALSE);return 0;}if(RefreshSystemAppearance()&&appearanceMode==0){ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);}if(RefreshAccessibility()){ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);}
  if(w==kRecordTimeout){StopRecording(h,L"Recording timed out; the previous shortcut is kept.");return 0;}
  if(recordingHotkey&&selected!=6)StopRecording(h,nullptr);  // left the page (e.g. via search)
  if(w==2){KillTimer(h,2);taskbarFeedback.clear();}  // short follow-up refresh after a taskbar command
  if(selected==6){const bool a=RefreshTaskbarState(),b=RefreshTaskbarStatus();if(a||b||w==2)InvalidateRect(h,nullptr,FALSE);}
  if(w==2)return 0;
   {auto& store=dock::GlassSettingsStore::Get();const auto before=store.Version();store.ReloadIfChanged();if(store.Version()!=before)InvalidateRect(h,nullptr,FALSE);}
   auto r=liquid::settings::ReadRevision(settingsPath);if(r!=revision){Refresh();ResizeSettingsUi(h);ApplyNativeAppearance(h);InvalidateRect(h,nullptr,FALSE);}
  {auto wr=liquid::settings::ReadRevision(workshopRoot+L"\\active.txt"),sr=liquid::settings::ReadRevision(workshopRoot+L"\\settings-active.txt");if(wr!=workshopRevision||sr!=settingsWorkshopRevision){RefreshWorkshop();InvalidateRect(h,nullptr,FALSE);}}
  return 0;}
 case WM_MOUSEWHEEL:{if(menuSliderDragging)return 0;POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&point);keyboardFocusVisible=false;
  if(switchPressed>=0)return 0;
  UpdateScrollBounds(h);const float distance=-GET_WHEEL_DELTA_WPARAM(w)/120.f*72.f;
  if(point.x/scale<liquid::design::settings::SidebarWidth)sidebarScroll.By(distance);else contentScroll[selected].By(distance);
  StartScroll(h);return 0;}
 case WM_LBUTTONUP:{if(menuSliderDragging){UpdateMenuSlider(h,GET_X_LPARAM(l)/scale);menuSliderDragging=false;BeginSwitchPress(h,40+activeSizeSlider,false);ReleaseCapture();SaveMenuScale(h);return 0;}namespace ds=liquid::design;namespace lay=liquid::design::settings;namespace sp=liquid::design::space;
  float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;RECT rc{};GetClientRect(h,&rc);const float contentLeft=ContentLeftForWidth(rc.right/scale),right=ContentRightForWidth(rc.right/scale,contentLeft);
  if(switchPressed>=0){const int pressed=switchPressed;const bool oldValue=SettingsSwitchValue(pressed);const bool change=!switchDragging||((switchDragPosition>=.5f)!=oldValue);if(switchDragging){auto& anim=switchMotion[pressed];anim.initialized=true;anim.on=oldValue;anim.from=switchDragPosition;anim.to=oldValue?1.f:0.f;anim.start=GetTickCount64();}switchPressed=-1;BeginSwitchPress(h,pressed,false);switchDragging=false;if(GetCapture()==h)ReleaseCapture();
   if(change&&SettingsSwitchAt(h,x,y)==pressed&&pressed==20){if(liquid::settings::WriteBool(settingsPath,"menuBar","enabled",!menuBarOn)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(change&&SettingsSwitchAt(h,x,y)==pressed){if(pressed==44||pressed==45||pressed==50||pressed==51||pressed==52){
     const char* key=pressed==44?"animatedArtwork":pressed==45?"lyricsDoubleClickSeek":pressed==50?"keepArtworkWhenPaused":pressed==51?"localMusicTags":"hideDesktopIconsWithWidgets";
     if(liquid::settings::WriteBool(settingsPath,"controlCenter",key,!oldValue)!=liquid::settings::SaveStatus::Saved)
      MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
     Refresh();
   }else if(pressed==31||pressed==32||pressed==33||pressed==38){if(liquid::settings::WriteBool(settingsPath,"controlCenter",pressed==31?"wallpaperLyrics":pressed==32?"lyricsFullArtwork":pressed==33?"lyricsAutoReturn":"lyricsBeat",!(pressed==31?wallpaperLyrics:pressed==32?lyricsFullArtwork:pressed==33?lyricsAutoReturn:lyricsBeat))!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==29){if(liquid::settings::WriteBool(settingsPath,"controlCenter","wallpaperAvoidBars",!wallpaperAvoidBars)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==28){if(liquid::settings::WriteBool(settingsPath,"controlCenter","musicWallpaper",!musicWallpaper)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==19){if(liquid::settings::WriteBool(settingsPath,"controlCenter","mediaArtworkBackground",!mediaArtworkBackground)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==25){if(liquid::settings::WriteBool(settingsPath,"menuBar","hideSystemNotifications",!hideSystemNotifications)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==46){if(liquid::settings::WriteBool(settingsPath,"menuBar","themeGradient",!menuBarThemeGradient)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==24){if(liquid::settings::WriteBool(settingsPath,"menuBar","captureNotifications",!captureNotifications)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==26){if(liquid::settings::WriteBool(settingsPath,"dock","lowLatencyGlass",!lowLatencyGlass)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==18){if(liquid::settings::WriteBool(settingsPath,"dock","gameCompatibility",!gameCompatibility)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==14||pressed==15){if(liquid::settings::WriteBool(settingsPath,"updates",pressed==14?"autoCheck":"autoInstall",!SettingsSwitchValue(pressed))!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==17){std::wstring error;if(!SetLoginItem(!loginItem,error))MessageBoxW(h,error.c_str(),L"DockOver Settings",MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==16){if(liquid::settings::WriteBool(settingsPath,"dock","launchDesktop",!launchDesktop)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==13){if(liquid::settings::WriteBool(settingsPath,"launchpad","showWindows",!density.showWindows)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==9){if(liquid::settings::WriteBool(settingsPath,"launchpad","hideLabels",!density.hideLabels)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==12){if(liquid::settings::WriteBool(settingsPath,"dock","downloadsPreview",!dlPreview)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed==11){if(liquid::settings::WriteBool(settingsPath,"dock","downloadsVertical",!dlVertical)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);Refresh();}
   else if(pressed<3||pressed==8||pressed==10){const int row=pressed==8?3:pressed==10?4:pressed;dockKeyboardRow=row;ToggleDockSetting(h,row);}else{taskbarKeyboardControl=pressed;ActivateTaskbarControl(h,pressed);}}InvalidateRect(h,nullptr,FALSE);return 0;}
  if(chromePressed>=0){const int pressed=chromePressed;chromePressed=-1;if(GetCapture()==h)ReleaseCapture();if(ChromeButtonAt(x,y)==pressed){if(pressed==0)PostMessageW(h,WM_CLOSE,0,0);else if(pressed==1)ShowWindow(h,SW_MINIMIZE);else ShowWindow(h,IsZoomed(h)?SW_RESTORE:SW_MAXIMIZE);}InvalidateRect(h,nullptr,FALSE);return 0;}
  const float nav=lay::SidebarWidth+10.f;if(x>=nav&&x<nav+88&&y>=10&&y<52){StepHistory(h,x<nav+44?-1:1);return 0;}
  if(visibleCategories.empty())return 0;
  if(x<lay::SidebarWidth){
   if(y>=108){const float localY=y+sidebarScroll.Position();const int row=static_cast<int>(std::floor((localY-lay::SidebarTop)/lay::SidebarStep));
    if(row>=0&&row<static_cast<int>(visibleCategories.size())&&localY<lay::SidebarTop+row*lay::SidebarStep+lay::SidebarRow){SelectCategory(h,visibleCategories[row]);InvalidateRect(h,nullptr,FALSE);}}
   return 0;
  }
  if(y<60)return 0;
  y=liquid::settings::SettingsBodyY(y,contentScroll[selected].Position());
  if(selected==1){if(y>=710&&y<822){for(int i=0;i<3;++i)if(Contains(ApplicationsSortOption(contentLeft,right,i),x,y)){appearanceKeyboard=7+i;SetApplicationsOrder(h,i);break;}return 0;}if(y>=558&&y<698){const float width=(right-contentLeft-44)*.5f;if(y>=604&&y<636){if(x>=contentLeft+16&&x<contentLeft+16+width){appearanceKeyboard=5;SetLanguage(h,0);}else if(x>=contentLeft+28+width&&x<right-16){appearanceKeyboard=6;SetLanguage(h,1);}}return 0;}for(int i=0;i<3;++i)if(Contains(AppearanceOption(contentLeft,right,i),x,y)){appearanceKeyboard=i;SetAppearance(h,i);break;}return 0;
  }
  else if(selected==3&&Contains(GamesListButton(contentLeft,right),x,y)){OpenGamesList(h);return 0;}
  else if(selected==10&&Contains(LyricsFolderButton(contentLeft,right),x,y)){OpenLyricsFolder(h);return 0;}
  else if(selected==10&&[&]{for(int i=0;i<3;++i)if(Contains(ResponseButton(i,contentLeft,right),x,y)){
   if(liquid::settings::WriteControlCenterInt(settingsPath,"musicResponse",i)!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
   Refresh();InvalidateRect(h,nullptr,FALSE);return true;}return false;}())return 0;
  else if(selected==10&&[&]{for(int i=0;i<5;++i)if(Contains(FpsButton(i,contentLeft,right),x,y)){
   if(liquid::settings::WriteControlCenterInt(settingsPath,"musicFps",kMusicFps[i])!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
   Refresh();InvalidateRect(h,nullptr,FALSE);return true;}return false;}())return 0;
  else if(selected==10&&[&]{const auto t=FormFor(39,contentLeft,right);const float yy=(t.top+t.bottom)*.5f-14;if(y<yy||y>=yy+28)return false;
   int next=lyricsOffsetMs;if(x>=right-136&&x<right-108)next-=100;else if(x>=right-48&&x<right-20)next+=100;else return false;
   if(liquid::settings::WriteControlCenterInt(settingsPath,"lyricsOffsetMs",std::clamp(next,-3000,3000))!=liquid::settings::SaveStatus::Saved)MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
   Refresh();InvalidateRect(h,nullptr,FALSE);return true;}())return 0;
  else if(selected==10&&[&]{for(int i=0;i<4;++i)if(Contains(ShortcutButton(i,contentLeft,right),x,y)){StartShortcut(h,i);return true;}return false;}())return 0;
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
      MessageBoxW(h,docky::ui::Text(L"Could not save settings. Your previous settings were preserved."),docky::ui::Text(L"DockOver Settings"),MB_OK|MB_ICONWARNING);
     Refresh();InvalidateRect(h,nullptr,FALSE);return 0;}}
  }
  else if(selected==9&&y>=UpdateCardTop()+22&&y<UpdateCardTop()+102){
   const auto cb=UpdateCheckButton(right,UpdateCardTop()),ib=UpdateInstallButton(right,UpdateCardTop());
   if(x>=cb.left&&x<cb.right&&y>=cb.top&&y<cb.bottom){if(updUi==UpdUi::Downloading)updCancel=true;else if(updUi!=UpdUi::Checking&&updUi!=UpdUi::Installing)StartUpdateCheck(h);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(updUi==UpdUi::Available&&x>=ib.left&&x<ib.right&&y>=ib.top&&y<ib.bottom){StartUpdateDownload(h);InvalidateRect(h,nullptr,FALSE);return 0;}
   return 0;
  }
  else if(selected==8){const auto cards=MeasureMigration(contentLeft,right);
   if(Contains(cards[1].buttons[0],x,y)){ImportMyDockFinder(h,false);return 0;}
   if(Contains(cards[1].buttons[1],x,y)){ImportMyDockFinder(h,true);return 0;}
   if(Contains(cards[0].buttons[0],x,y)){
   const auto file=PickLegacyFile(h);if(!file.empty()){
    const auto result=liquid::settings::BackupLegacy(file);
    MessageBoxW(h,result==liquid::settings::BackupStatus::Created?L"Backup created beside the selected file. The original was not changed.":result==liquid::settings::BackupStatus::AlreadyExists?L"A backup already exists and was not replaced.":L"Could not create a backup. The original was not changed.",L"DockOver Migration",MB_OK|(result==liquid::settings::BackupStatus::Unavailable?MB_ICONWARNING:MB_ICONINFORMATION));
   }return 0;
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
     else if(index==2){if(MessageBoxW(h,docky::ui::Text(L"Delete the selected installed resource pack?"),docky::ui::Text(L"DockOver Workshop"),MB_YESNO|MB_ICONQUESTION)==IDYES){action=true;ok=docky::workshop::Remove(workshopRoot,id,error);}}
     else if(index==3&&packs[packSelected].valid){action=true;const bool settings=SettingsOnly(packs[packSelected]);ok=settings?docky::workshop::SelectSettings(workshopRoot,id==activeSettingsPack?std::string{}:id,error):docky::workshop::Select(workshopRoot,id==activePack?std::string{}:id,error);}
    }
   }}}
   if(action){if(!ok)MessageBoxW(h,error.empty()?L"Workshop operation failed.":error.c_str(),L"DockOver Workshop",MB_OK|MB_ICONWARNING);RefreshWorkshop();}InvalidateRect(h,nullptr,FALSE);
  }
  else if(selected==2){const float y0=CurrentDockForm().rows[5].control.top;
   if(y>=y0&&y<y0+28){if(x>=right-126&&x<right-98)SetDownloadsCount(h,dlCount-1);else if(x>=right-48&&x<right-20)SetDownloadsCount(h,dlCount+1);}
  }
  else if(selected==5){const float y0=CcBlurRow(contentLeft,right).control.top;
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
  return 0;}
 case WM_ACTIVATE: if(LOWORD(w)==WA_INACTIVE&&recordingHotkey)StopRecording(h,L"Recording stopped (window inactive); the previous shortcut is kept.");InvalidateRect(h,nullptr,FALSE);break;
 case WM_DESTROY:if(searchHost){DestroyWindow(searchHost);searchHost=nullptr;searchBox=nullptr;}StopRecording(h,nullptr);KillTimer(h,1);systemUi=nullptr;if(searchFont){DeleteObject(searchFont);searchFont=nullptr;}if(searchBrush){DeleteObject(searchBrush);searchBrush=nullptr;}
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
  else if(a==L"--workshop-root"&&i+1<argc){workshopRoot=argv[++i];explicitWorkshopRoot=true;}else if(a==L"--music-only")musicOnly=true;else if(a==L"--dev-workshop")devWorkshop=true;else if(a==L"--dev-press-switch")devPress=true;else if(a==L"--dev-opaque")devOpaque=true;else if(a==L"--snapshot"&&i+1<argc)snapshotPath=argv[++i];else if(a==L"--log"&&i+1<argc)dock::Log::Get().Open(argv[++i]);else if(a==L"--glass-config"&&i+1<argc)glassConfigPath=argv[++i];else if(a==L"--dev-layout-only")referenceLayoutOnly=true;else if(a==L"--dev-size"&&i+2<argc){referenceWidth=std::max(760,_wtoi(argv[++i]));referenceHeight=std::max(560,_wtoi(argv[++i]));}else if(a==L"--dev-hold")referenceInteractive=true;else if(a==L"--dev-reference"&&i+1<argc)referenceCaptureDir=argv[++i];else if(a==L"--page"&&i+1<argc){const std::wstring page=argv[++i];const wchar_t* keys[]={L"general",L"appearance",L"dock",L"menu-bar",L"launchpad",L"control-center",L"taskbar",L"workshop",L"migration",L"about",L"music"};for(int i=0;i<11;++i)if(page==keys[i]){selected=i;categoryHistory={0,selected};categoryHistoryIndex=1;}}}LocalFree(argv);}

 if(musicOnly){
  if(snapshotPath.empty())if(HWND existing=FindWindowW(MusicClassName,nullptr)){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);return 0;}
  selected=10;visibleCategories={1,10};categoryHistory={10};categoryHistoryIndex=0;
 }
 updState=docky::update::LoadState();
 if(!glassConfigPath.empty())dock::GlassSettingsStore::Get().LoadReadOnly(glassConfigPath);else {wchar_t local[MAX_PATH]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))dock::GlassSettingsStore::Get().LoadReadOnly(std::wstring(local)+L"\\LiquidDock\\glass.json");}  // Sliders & switches lens
 if((!referenceCaptureDir.empty()||referenceInteractive||referenceLayoutOnly)&&(!explicitWorkshopRoot||!explicitSettingsPath||referenceCaptureDir.empty()))return 2;
 if(devOpaque&&!explicitWorkshopRoot)return 2;
 if(devPress&&(!explicitSettingsPath||!explicitWorkshopRoot||devOpaque))return 2;
 if(devPress){selected=2;switchPressed=1;visualSwitch=1;pressFrom=pressTo=1.f;pressStart=GetTickCount64();switchPointerX=824.f;switchPointerY=190.f;}
 if(devWorkshop){selected=7;categoryHistory={7};categoryHistoryIndex=0;RefreshWorkshop();}
 if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))||FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()))))return 1;
 auto font=[&](float size,DWRITE_FONT_WEIGHT weight,ComPtr<IDWriteTextFormat>& out){return writeFactory->CreateTextFormat(liquid::design::type::Family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",&out);};
 if(FAILED(font(16.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,normal))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,fontSmall))||FAILED(font(22.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,heading))||FAILED(font(liquid::design::type::Caption,DWRITE_FONT_WEIGHT_SEMI_BOLD,caption)))return 1;
 if(FAILED(font(16.f,DWRITE_FONT_WEIGHT_NORMAL,sidebarLabel))||FAILED(font(16.f,DWRITE_FONT_WEIGHT_SEMI_BOLD,sidebarSelected))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,centerSmall))||FAILED(font(liquid::design::type::Label,DWRITE_FONT_WEIGHT_NORMAL,trailingSmall)))return 1;
 for(auto* format:{normal.Get(),sidebarLabel.Get(),sidebarSelected.Get(),heading.Get()}){
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);ComPtr<IDWriteInlineObject> ellipsis;
  if(SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(format,&ellipsis))){DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};format->SetTrimming(&trim,ellipsis.Get());}
 }
 sidebarSelected->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
 sidebarLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
 centerSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);centerSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
 trailingSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);trailingSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
 if(SUCCEEDED(writeFactory->CreateTextFormat(L"Segoe Fluent Icons",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,15.f,L"en-us",&categoryIcons))){categoryIcons->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);categoryIcons->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}
 WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=musicOnly?MusicClassName:ClassName;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
 if(!RegisterClassW(&wc))return 1;
 const auto initialAppearance=liquid::settings::Load(settingsPath).value.appearance;
 docky::ui::SetLanguage(initialAppearance.language);
 const int initialSize=initialAppearance.settingsScalePercent;
 settingsUiPercent=initialSize;scale=GetDpiForSystem()/96.f*initialSize/100.f;
 HWND h=CreateWindowExW(devOpaque?0:WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,docky::ui::Text(musicOnly?L"Music settings":L"DockOver Settings"),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,static_cast<int>(std::lround(900*scale)),static_cast<int>(std::lround(760*scale)),nullptr,nullptr,instance,nullptr);
 if(!h)return 1;
 SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
 RECT client{};GetClientRect(h,&client);
 // The sidebar and controls sample only the composed client area.
 if(!devOpaque){renderer=std::make_unique<dock::Renderer>();if(SUCCEEDED(renderer->Init(h,client.right,client.bottom,96*scale,false))){gpuReady=true;}
  else{DisableGpu();DLOG(L"settings: GPU unavailable; opaque control fallback");}}
 RECT startupBounds{};GetWindowRect(h,&startupBounds);DLOG(L"settings: startup size=%ldx%ld ui=%d%% applications=%d%%",startupBounds.right-startupBounds.left,startupBounds.bottom-startupBounds.top,settingsUiPercent,applicationsUiPercent);
 UpdateWindowContour(h,SIZE_RESTORED);
 DLOG(L"settings: opaque themed window; desktop capture disabled; local GPU=%d",gpuReady);
 // The custom continuous silhouette owns the outer corners; suppress the unrelated DWM border.
 DWORD corner=1;DwmSetWindowAttribute(h,33,&corner,sizeof(corner));  // Custom continuous contour, rather than the system 8 DIP corner.
 const COLORREF noBorder=0xFFFFFFFE;DwmSetWindowAttribute(h,34,&noBorder,sizeof(noBorder));
 if(!referenceCaptureDir.empty()){SetWindowPos(h,nullptr,0,0,referenceWidth,referenceHeight,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);if(!referenceInteractive)SetTimer(h,38,16,nullptr);}
 ShowWindow(h,SW_SHOWNORMAL);if(!referenceCaptureDir.empty())ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);
 MSG msg{};
 while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 if(SUCCEEDED(ro))RoUninitialize();return 0;
}
