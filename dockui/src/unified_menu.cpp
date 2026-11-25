#include "unified_menu.h"
#include "ui_language.h"
#include "menu_surface.h"
#include "popup_dismiss_watch.h"
#include "menu_navigation.h"
#include "display_geometry.h"
#include "design_tokens.h"
#include "glass_params.h"
#include "motion.h"
#include "vblank.h"
#include <timeapi.h>
#include <climits>
#pragma comment(lib,"winmm.lib")
#include "log.h"
#include <windowsx.h>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace liquid::popup {
namespace {
constexpr wchar_t kClass[]=L"LiquidUnifiedPopup98d2";
struct Popup {
 HWND owner=nullptr,window=nullptr,previousForeground=nullptr;bool keyboardFocus=false;bool* destroyedDuringCreate=nullptr;UINT dpi=96;std::vector<menu::MenuItem> root;std::vector<size_t> path;
 Select select;Closed closed;int scroll=0,hover=-1;bool tracking=false,mouseDown=false,escapeDown=false,closing=false;
 docky::motion::Spring appear;LONGLONG lastQpc=0,openQpc=0;bool timerRes=false;  // 0..1 materialize amount; real-time dt per tick
 // Display-paced ticks (vblank.h): the open / close spring and the backdrop compare poll run once per display frame
 // (240 Hz on a 240 Hz monitor). They were WM_TIMER at 4 / 16 ms, which Windows clamps to ~10 / ~16 ms.
 docky::vblank::Ticker ticker;bool animTick=false,captureTick=false;
 bool hasSelection=false,callbacksEnabled=false,reducedMotion=false,selectedSecondary=false;menu::MenuItem selected;
 std::unique_ptr<MenuSurface> surface;
 DismissWatch dismissal;
 // Cascading submenus (macOS): a submenu row opens a child popup to its right after a short hover and it closes when the
 // pointer goes elsewhere; clicking the row pins it open. Keyboard activation keeps the in-place drill-down (path).
 Popup* parent=nullptr;HWND child=nullptr;int childRow=-1,awayTicks=0;bool childSticky=false,keyboardActivate=false;
 int aboveBottom=INT_MIN;  // ShowAbove: the popup's bottom edge stays here (screen px); INT_MIN = opens downwards
 int gridCols=0;std::wstring gridTitle;  // ShowGridAbove: items as icon cells, this many per row; footer when nothing is hovered
};
HWND current=nullptr,cached=nullptr;
bool visualCaptureTest=false;
bool lastSecondary=false;  // the selection being delivered was made with the right button
bool themeOpaque=false;COLORREF themeBackground=RGB(39,43,52),themeForeground=RGB(247,248,250);
bool IsMenuBarOwner(HWND owner){wchar_t cls[128]{};return owner&&GetClassNameW(owner,cls,128)&&wcscmp(cls,L"LiquidDesktopMenuBarV01")==0;}
int Px(const Popup* p,int x){return MulDiv(x,static_cast<int>(p->dpi),96);}
// Grid popups (dock::kMenuGrid* in DIP; the renderer uses the same numbers).
bool Grid(const Popup* p){return p->gridCols>0&&p->path.empty();}
int GridCount(const Popup* p);
int GridRows(const Popup* p){return (std::max)(1,(GridCount(p)+p->gridCols-1)/p->gridCols);}
int GridWidthDip(const Popup* p){return static_cast<int>((std::max)(dock::kMenuGridMinWidth,2*dock::kMenuGridPad+dock::kMenuGridCell*static_cast<float>(p->gridCols)));}
int GridHeightDip(const Popup* p){return static_cast<int>(dock::kMenuGridPad+dock::kMenuGridCell*static_cast<float>(GridRows(p))+dock::kMenuGridFooter);}
// The cell at client pixel (x, y), or -1.
int HitGrid(const Popup* p,int x,int y){
 const float dx=static_cast<float>(x)*96.f/static_cast<float>(p->dpi),dy=static_cast<float>(y)*96.f/static_cast<float>(p->dpi);
 const float left=(static_cast<float>(GridWidthDip(p))-dock::kMenuGridCell*static_cast<float>(p->gridCols))*.5f;
 if(dx<left||dy<dock::kMenuGridPad)return -1;
 const int col=static_cast<int>((dx-left)/dock::kMenuGridCell),row=static_cast<int>((dy-dock::kMenuGridPad)/dock::kMenuGridCell);
 if(col>=p->gridCols||row>=GridRows(p))return -1;
 const int index=row*p->gridCols+col;return index<GridCount(p)?index:-1;
}
// Client rectangle (px) of grid cell `index`.
RECT GridCell(const Popup* p,int index){
 const float left=(static_cast<float>(GridWidthDip(p))-dock::kMenuGridCell*static_cast<float>(p->gridCols))*.5f;
 const float x=left+dock::kMenuGridCell*static_cast<float>(index%p->gridCols),y=dock::kMenuGridPad+dock::kMenuGridCell*static_cast<float>(index/p->gridCols);
 const auto px=[p](float v){return static_cast<LONG>(std::lround(v*static_cast<float>(p->dpi)/96.f));};
 return RECT{px(x),px(y),px(x+dock::kMenuGridCell),px(y+dock::kMenuGridCell)};
}
const std::vector<menu::MenuItem>* Items(const Popup* p){auto* items=&p->root;for(size_t i:p->path){if(i>=items->size())return nullptr;items=&(*items)[i].children;}return items;}
int RowCount(const Popup* p){const auto* items=Items(p);return static_cast<int>(items?items->size():0)+(p->path.empty()?0:1);}
int GridCount(const Popup* p){const auto* items=Items(p);return static_cast<int>(items?items->size():0);}
int VisibleRows(const Popup* p){RECT r{};GetClientRect(p->window,&r);return (std::max)(1,(static_cast<int>(r.bottom)-Px(p,12))/Px(p,24));}
void StartClose(Popup* p);
void StopTick(Popup* p);
void OpenChild(Popup* p,int row);
Popup* PopupOf(HWND h){return h&&IsWindow(h)?reinterpret_cast<Popup*>(GetWindowLongPtrW(h,GWLP_USERDATA)):nullptr;}
Popup* RootOf(Popup* p){while(p&&p->parent)p=p->parent;return p;}
bool InWindow(HWND w,POINT pt){RECT r{};return w&&IsWindow(w)&&IsWindowVisible(w)&&GetWindowRect(w,&r)&&PtInRect(&r,pt);}
// The popup or any submenu it has open contains pt.
bool InChain(Popup* p,POINT pt){for(Popup* c=p;c;c=PopupOf(c->child))if(InWindow(c->window,pt))return true;return false;}
void CloseChild(Popup* p){
 if(!p||!p->child)return;HWND c=p->child;p->child=nullptr;p->childRow=-1;p->childSticky=false;p->awayTicks=0;
 if(Popup* cp=PopupOf(c)){CloseChild(cp);cp->callbacksEnabled=false;}
 if(IsWindow(c))DestroyWindow(c);
}
void Resize(Popup* p){
 RECT r{};GetWindowRect(p->window,&r);HMONITOR mon=MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST);MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(mon,&mi))return;
 const int wantH=Grid(p)?Px(p,GridHeightDip(p)):Px(p,12+24*(std::max)(1,RowCount(p)));
 const int wantW=Grid(p)?Px(p,GridWidthDip(p)):Px(p,244);
 const RECT bounds=docky::geometry::ClampPopup(mi.rcWork,r.left,p->aboveBottom!=INT_MIN?p->aboveBottom-wantH:r.top,wantW,wantH);
 const int x=bounds.left,y=bounds.top,width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
 SetWindowPos(p->window,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE);
 // A binary window region clips antialiased GPU pixels into a jagged edge.
 // The composition surface draws its own rounded alpha; keep a region only for GDI fallback.
 if(p->surface)SetWindowRgn(p->window,nullptr,TRUE);
 else {HRGN region=CreateRoundRectRgn(0,0,width+1,height+1,Px(p,24),Px(p,24));if(region&&!SetWindowRgn(p->window,region,TRUE))DeleteObject(region);}
 p->scroll=std::clamp(p->scroll,0,(std::max)(0,RowCount(p)-VisibleRows(p)));
 if(p->surface&&!p->surface->Resize(width,height,static_cast<float>(p->dpi))){DLOG(L"menu surface: resize failed; closing safely");StartClose(p);}
}
void FinishClose(Popup* p){
 if(!p)return;p->dismissal.Stop();CloseChild(p);HWND h=p->window;KillTimer(h,1);KillTimer(h,5);p->captureTick=false;StopTick(p);KillTimer(h,3);KillTimer(h,4);
 const bool restore=p->keyboardFocus&&GetForegroundWindow()==h;HWND previous=p->previousForeground;
 ShowWindow(h,SW_HIDE);if(restore&&previous&&IsWindow(previous))SetForegroundWindow(previous);
 if(p->surface)p->surface->Suspend();if(current==h)current=nullptr;
 auto closed=std::move(p->closed);auto select=std::move(p->select);auto selected=std::move(p->selected);
 bool execute=p->callbacksEnabled&&p->hasSelection&&IsWindow(p->owner);bool notify=p->callbacksEnabled;
 const bool secondary=p->selectedSecondary;p->callbacksEnabled=false;p->hasSelection=false;p->closing=false;p->selectedSecondary=false;
 if(notify&&closed)closed();if(execute&&select){lastSecondary=secondary;select(selected);lastSecondary=false;}
}
// Menu motion (springs, critically damped: a menu that just appeared must not overshoot). Response is the
// stiffness period; 95% settle is about 0.75 x response. Replaces the fixed 130 ms / 110 ms timed fades.
// Re-closing or re-opening mid-flight retargets the same spring, so value and velocity stay continuous.
const docky::motion::SpringSpec kOpenSpec=docky::motion::tokens::MenuOpen;
const docky::motion::SpringSpec kCloseSpec=docky::motion::tokens::MenuClose;
constexpr float kCloseBelow=0.05f;
constexpr double kGlassWaitMax=0.25;  // longest the reveal waits for the first captured backdrop frame
LONGLONG Qpc(){LARGE_INTEGER c{};QueryPerformanceCounter(&c);return c.QuadPart;}
double QpcSeconds(LONGLONG from,LONGLONG to){static LARGE_INTEGER f{};if(!f.QuadPart)QueryPerformanceFrequency(&f);return static_cast<double>(to-from)/static_cast<double>(f.QuadPart);}
void SyncTicker(Popup* p){if(p->animTick||p->captureTick)p->ticker.Start(p->window,docky::vblank::kMsgVBlank);else p->ticker.Stop();}
void StartTick(Popup* p){p->lastQpc=Qpc();p->animTick=true;SyncTicker(p);}
void StopTick(Popup* p){p->animTick=false;SyncTicker(p);}
void BeginAppear(Popup* p){p->appear.SetSpec(kOpenSpec);p->appear.SetRest(0.002,0.02);p->appear.SnapTo(0.0);p->appear.SetTarget(1.0);StartTick(p);p->openQpc=p->lastQpc;}
void StartClose(Popup* p){if(!p||p->closing)return;if(p->child)ShowWindow(p->child,SW_HIDE);p->closing=true;p->appear.SetSpec(kCloseSpec);p->appear.SetTarget(0.0);StartTick(p);}
int Hit(const Popup* p,int y){if(y<Px(p,6))return -1;int row=(y-Px(p,6))/Px(p,24)+p->scroll;return row<RowCount(p)?row:-1;}
const menu::MenuItem* ItemAt(const Popup* p,int row){const auto* items=Items(p);if(!items)return nullptr;if(!p->path.empty()){if(row==0)return nullptr;--row;}return row>=0&&static_cast<size_t>(row)<items->size()?&(*items)[row]:nullptr;}
int HitAt(const Popup* p,int x,int y){return Grid(p)?HitGrid(p,x,y):Hit(p,y);}
// Grid footer: the hovered item's name, otherwise the popup's title.
std::wstring GridFooter(const Popup* p){const auto* item=ItemAt(p,p->hover);return item&&!item->text.empty()?item->text:p->gridTitle;}
std::vector<dock::MenuRowVisual> VisualRows(const Popup* p){
 std::vector<dock::MenuRowVisual> rows;const int count=Grid(p)?GridCount(p):VisibleRows(p);
 for(int k=0;k<count;++k){int row=k+(Grid(p)?0:p->scroll);if(row>=RowCount(p))break;const auto* item=ItemAt(p,row);const bool back=!p->path.empty()&&row==0;
  dock::MenuRowVisual visual;visual.text=back?(docky::ui::chinese?L"‹  返回":L"‹  Back"):(item?item->text:L"");visual.enabled=back||(item&&item->enabled);visual.hover=row==p->hover;
  if(item){visual.checked=item->checked;visual.separator=item->kind==menu::ItemKind::Separator;visual.submenu=item->kind==menu::ItemKind::Submenu;visual.icon=item->icon;}rows.push_back(std::move(visual));}
 return rows;
}
// The menu fades in as fully formed liquid glass (no blur/refraction ramp: a half-formed material reads
// as a plain translucent sheet). Coverage stays 0 until the first captured backdrop frame exists.
bool ReducedMotion(){BOOL animations=TRUE;return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0)&&!animations;}
void SetAppear(Popup* p,float t){
 if(p->surface)p->surface->Materialize(t,false);
 else SetLayeredWindowAttributes(p->window,0,static_cast<BYTE>(dock::MaterializeAlpha(t)*255.f),LWA_ALPHA);
}
void CaptureTimer(Popup* p){if(p->surface&&p->surface->CompletionEvent()){p->captureTick=true;SyncTicker(p);}}
LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM w,LPARAM l){
 Popup* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(h,GWLP_USERDATA));
 if(msg==WM_NCCREATE){p=reinterpret_cast<Popup*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);p->window=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
 if(!p)return DefWindowProcW(h,msg,w,l);
 switch(msg){
 case WM_CREATE:SetTimer(h,1,40,nullptr);return 0;
 case WM_MOUSEACTIVATE:return p->keyboardFocus?MA_ACTIVATE:MA_NOACTIVATE;
 case DismissWatch::ClickMessage:
  if(!p->parent&&!InChain(p,POINT{static_cast<LONG>(w),static_cast<LONG>(l)}))StartClose(p);
  return 0;
 case DismissWatch::FocusMessage:
  if(!p->parent&&p->dismissal.LostFocus())StartClose(p);
  return 0;
 case WM_TIMER:
  if(w==1&&!p->closing){
   POINT pt{};GetCursorPos(&pt);
   if(!p->parent){
    if(!p->dismissal.HasMouseHook()){
     const bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)||(GetAsyncKeyState(VK_RBUTTON)&0x8000);
     if(down&&!p->mouseDown&&!InChain(p,pt))StartClose(p);p->mouseDown=down;
    }
    if(p->dismissal.LostFocus())StartClose(p);
    const bool esc=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;
    if(esc&&!p->escapeDown)StartClose(p);p->escapeDown=esc;
   }
   if(p->child&&!p->childSticky){
    RECT wr{};GetWindowRect(h,&wr);const bool onRow=PtInRect(&wr,pt)&&Hit(p,pt.y-wr.top)==p->childRow;
    if(onRow||InChain(PopupOf(p->child),pt))p->awayTicks=0;else if(++p->awayTicks>=3)CloseChild(p);  // ~120 ms grace for diagonal moves
   }}
  if(w==5){KillTimer(h,5);const auto* item=ItemAt(p,p->hover);if(!p->closing&&item&&item->kind==menu::ItemKind::Submenu&&item->enabled&&p->hover!=p->childRow){CloseChild(p);OpenChild(p,p->hover);}}
  if(w==2||(w==docky::vblank::kMsgVBlank&&p->animTick)){
   // Real elapsed time since the last tick (QPC), so the result is independent of timer jitter and the
   // display rate; the spring solve is exact for any dt. A very late tick is capped inside Spring::Step.
   const LONGLONG now=Qpc();const double dt=QpcSeconds(p->lastQpc,now);p->lastQpc=now;
   const bool waiting=!p->closing&&p->surface&&!p->surface->GlassReady()&&QpcSeconds(p->openQpc,now)<kGlassWaitMax;
   p->appear.SetTarget(p->closing?0.0:1.0);if(!waiting)p->appear.Step(dt);SetAppear(p,p->appear.F());
   if(waiting)return 0;
   if(p->closing){if(!p->appear.Animating()||p->appear.Value()<kCloseBelow){FinishClose(p);return 0;}}
   else if(!p->appear.Animating())StopTick(p);}
  if((w==3||w==docky::vblank::kMsgVBlank)&&p->captureTick&&p->surface){HANDLE event=p->surface->CompletionEvent();if(event&&WaitForSingleObject(event,0)==WAIT_OBJECT_0){if(p->surface->Complete())InvalidateRect(h,nullptr,FALSE);}if(!p->surface->CompletionEvent()){p->captureTick=false;SyncTicker(p);}}
  if(w==4&&p->surface&&p->surface->CheckHealth())InvalidateRect(h,nullptr,FALSE);
  return 0;
 case docky::vblank::kMsgVBlank:p->ticker.Ack();
  if(p->animTick||p->captureTick)return Proc(h,WM_TIMER,docky::vblank::kMsgVBlank,0);return 0;
 case dock::kMsgCaptureAvailable:if(p->surface){if(p->surface->Capture())InvalidateRect(h,nullptr,FALSE);CaptureTimer(p);}return 0;
 case WM_KEYDOWN:{
  if(w==VK_ESCAPE){StartClose(p);return 0;}
  if(Grid(p)){  // arrows move through the cells (Up / Down by a row), Return or Space chooses
   const int n=GridCount(p);
   if(n>0&&(w==VK_LEFT||w==VK_RIGHT||w==VK_UP||w==VK_DOWN||w==VK_TAB)){
    const bool back=w==VK_LEFT||(w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000));
    const int step=w==VK_UP?-p->gridCols:w==VK_DOWN?p->gridCols:back?-1:1;
    p->hover=p->hover<0?0:std::clamp(p->hover+step,0,n-1);InvalidateRect(h,nullptr,FALSE);return 0;}
   if((w==VK_RETURN||w==VK_SPACE)&&p->hover>=0){const auto* item=ItemAt(p,p->hover);if(item&&item->enabled){p->selected=*item;p->hasSelection=true;StartClose(p);}return 0;}
   break;}
  if(w==VK_TAB)w=(GetKeyState(VK_SHIFT)&0x8000)?VK_UP:VK_DOWN;  // V0.15: Tab / Shift+Tab navigate like Down / Up
  if(w==VK_UP||w==VK_DOWN){const auto* items=Items(p);if(!items)return 0;
   const int next=menu::NextSelectable(*items,!p->path.empty(),p->hover,w==VK_DOWN?1:-1);
   if(next>=0){p->hover=next;DLOG(L"menu: keyboard selected row=%d",next);const int rows=VisibleRows(p);if(next<p->scroll)p->scroll=next;else if(next>=p->scroll+rows)p->scroll=next-rows+1;InvalidateRect(h,nullptr,FALSE);}return 0;}
  if(w==VK_LEFT&&!p->path.empty()){p->path.pop_back();p->scroll=0;p->hover=-1;Resize(p);InvalidateRect(h,nullptr,FALSE);return 0;}
  if((w==VK_RETURN||w==VK_SPACE||w==VK_RIGHT)&&p->hover>=0){
   const auto* item=ItemAt(p,p->hover);if(w==VK_RIGHT&&(!item||item->kind!=menu::ItemKind::Submenu))return 0;
   const int y=Px(p,6+24*(p->hover-p->scroll)+12);p->keyboardActivate=true;SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(Px(p,16),y));if(PopupOf(h)==p)p->keyboardActivate=false;return 0;}
  break;}
 case WM_MOUSEMOVE:{int next=HitAt(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));auto* item=ItemAt(p,next);if(item&&(!item->enabled||item->kind==menu::ItemKind::Separator))next=-1;if(next!=p->hover){p->hover=next;InvalidateRect(h,nullptr,FALSE);if(item&&next>=0&&item->kind==menu::ItemKind::Submenu&&next!=p->childRow)SetTimer(h,5,120,nullptr);else KillTimer(h,5);}if(!p->tracking){TRACKMOUSEEVENT ev{sizeof(ev),TME_LEAVE,h,0};TrackMouseEvent(&ev);p->tracking=true;}return 0;}
 case WM_MOUSELEAVE:p->tracking=false;if(p->hover!=-1){p->hover=-1;InvalidateRect(h,nullptr,FALSE);}return 0;
 case WM_MOUSEWHEEL:{if(Grid(p))return 0;int d=GET_WHEEL_DELTA_WPARAM(w);p->scroll=std::clamp(p->scroll+(d<0?2:-2),0,(std::max)(0,RowCount(p)-VisibleRows(p)));p->hover=-1;InvalidateRect(h,nullptr,FALSE);return 0;}
 case WM_LBUTTONUP:{int row=HitAt(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));if(row<0)return 0;if(!p->path.empty()&&row==0){p->path.pop_back();p->scroll=0;p->hover=-1;Resize(p);InvalidateRect(h,nullptr,FALSE);return 0;}
  auto* item=ItemAt(p,row);if(!item||!item->enabled||item->kind==menu::ItemKind::Separator)return 0;
  if(item->kind==menu::ItemKind::Submenu){
   if(!p->keyboardActivate){KillTimer(h,5);if(p->child&&p->childRow==row)p->childSticky=true;else{CloseChild(p);OpenChild(p,row);if(p->child)p->childSticky=true;}return 0;}
   CloseChild(p);int index=row-(p->path.empty()?0:1);p->path.push_back(static_cast<size_t>(index));p->scroll=0;p->hover=-1;Resize(p);InvalidateRect(h,nullptr,FALSE);return 0;}
  if(p->parent){Popup* root=RootOf(p);root->selected=*item;root->hasSelection=true;ShowWindow(h,SW_HIDE);StartClose(root);return 0;}
  p->selected=*item;p->hasSelection=true;StartClose(p);return 0;}
 case WM_RBUTTONUP:{  // right click on a command row: same selection, flagged as secondary (tray icons: context menu)
  int row=HitAt(p,GET_X_LPARAM(l),GET_Y_LPARAM(l));if(row<0||(!p->path.empty()&&row==0))return 0;
  auto* item=ItemAt(p,row);if(!item||!item->enabled||item->kind!=menu::ItemKind::Command)return 0;
  Popup* root=p->parent?RootOf(p):p;root->selected=*item;root->hasSelection=true;root->selectedSecondary=true;if(p->parent)ShowWindow(h,SW_HIDE);StartClose(root);return 0;}
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
  if(p->surface){p->surface->SetTheme(themeOpaque,themeBackground,themeForeground);EndPaint(h,&ps);if(!p->surface->Draw(VisualRows(p),Grid(p)?p->gridCols:0,Grid(p)?GridFooter(p):std::wstring())){DLOG(L"menu surface: render failed; closing safely");StartClose(p);}return 0;}
  HBRUSH bg=CreateSolidBrush(themeBackground);FillRect(dc,&r,bg);DeleteObject(bg);
  SetBkMode(dc,TRANSPARENT);HFONT font=CreateFontW(-Px(p,static_cast<int>(liquid::design::type::Label)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,liquid::design::type::FamilyGdi);HGDIOBJ old=SelectObject(dc,font);
  if(Grid(p)){  // GDI fallback grid: first letters in the cells, the footer below
   for(int i=0;i<GridCount(p);++i){const auto* item=ItemAt(p,i);if(!item)continue;RECT cell=GridCell(p,i);
    if(i==p->hover&&item->enabled){HBRUSH highlight=CreateSolidBrush(RGB(0,105,249));FillRect(dc,&cell,highlight);DeleteObject(highlight);}
    SetTextColor(dc,i==p->hover?RGB(255,255,255):themeForeground);const std::wstring letter=item->text.empty()?L"?":item->text.substr(0,1);
    DrawTextW(dc,letter.c_str(),-1,&cell,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);}
   SetTextColor(dc,themeForeground);RECT foot{Px(p,10),r.bottom-Px(p,static_cast<int>(dock::kMenuGridFooter)),r.right-Px(p,10),r.bottom-Px(p,6)};const std::wstring footer=GridFooter(p);
   DrawTextW(dc,footer.c_str(),-1,&foot,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
   SelectObject(dc,old);DeleteObject(font);EndPaint(h,&ps);return 0;}
  int rows=VisibleRows(p);for(int k=0;k<rows;++k){int row=k+p->scroll;if(row>=RowCount(p))break;int top=Px(p,6+24*k);RECT box{Px(p,6),top,r.right-Px(p,6),top+Px(p,24)};
   const auto* item=ItemAt(p,row);const bool back=!p->path.empty()&&row==0;
   if(row==p->hover&&(back||(item&&item->enabled))){HBRUSH highlight=CreateSolidBrush(RGB(0,105,249));HGDIOBJ oldBrush=SelectObject(dc,highlight);HPEN pen=CreatePen(PS_NULL,0,0);HGDIOBJ oldPen=SelectObject(dc,pen);RoundRect(dc,box.left,box.top,box.right,box.bottom,Px(p,16),Px(p,16));SelectObject(dc,oldPen);DeleteObject(pen);SelectObject(dc,oldBrush);DeleteObject(highlight);}
   if(item&&item->kind==menu::ItemKind::Separator){HBRUSH line=CreateSolidBrush(themeForeground);RECT sep{Px(p,14),top+Px(p,11),r.right-Px(p,14),top+Px(p,12)};FillRect(dc,&sep,line);DeleteObject(line);continue;}
   const bool enabled=back||(item&&item->enabled);const COLORREF disabled=RGB((GetRValue(themeForeground)*2+GetRValue(themeBackground))/3,(GetGValue(themeForeground)*2+GetGValue(themeBackground))/3,(GetBValue(themeForeground)*2+GetBValue(themeBackground))/3);SetTextColor(dc,row==p->hover&&enabled?RGB(255,255,255):enabled?themeForeground:disabled);
   std::wstring label=back?(docky::ui::chinese?L"‹  返回":L"‹  Back"):(item?docky::ui::Text(item->text.c_str()):L"");if(item&&item->checked)label=L"✓  "+label;
   RECT text{Px(p,16),top,r.right-Px(p,26),top+Px(p,24)};DrawTextW(dc,label.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
   if(item&&item->kind==menu::ItemKind::Submenu){RECT arrow{r.right-Px(p,25),top,r.right-Px(p,10),top+Px(p,24)};DrawTextW(dc,L"›",-1,&arrow,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);}
  }
  SelectObject(dc,old);DeleteObject(font);EndPaint(h,&ps);return 0;}
 case WM_NCDESTROY:{KillTimer(h,1);StopTick(p);KillTimer(h,3);KillTimer(h,4);if(current==h)current=nullptr;if(cached==h)cached=nullptr;if(p->destroyedDuringCreate)*p->destroyedDuringCreate=true;auto closed=std::move(p->closed);bool notify=p->callbacksEnabled;SetWindowLongPtrW(h,GWLP_USERDATA,0);delete p;if(notify&&closed)closed();break;}
 }
 return DefWindowProcW(h,msg,w,l);
}
std::wstring Label(const wchar_t* input){std::wstring out;for(size_t i=0;input[i];++i){if(input[i]==L'&'){if(input[i+1]==L'&'){out+=L'&';++i;}continue;}out+=input[i];}return out;}
std::vector<menu::MenuItem> Convert(HMENU h,int depth,int& budget){std::vector<menu::MenuItem> result;if(!h||depth>8)return result;const int count=GetMenuItemCount(h);
 for(int i=0;i<count&&budget>0;++i){--budget;wchar_t text[512]{};MENUITEMINFOW info{sizeof(info)};info.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_ID|MIIM_SUBMENU|MIIM_STRING;info.dwTypeData=text;info.cch=ARRAYSIZE(text);if(!GetMenuItemInfoW(h,i,TRUE,&info))continue;
  menu::MenuItem item;item.text=Label(text);item.enabled=!(info.fState&(MFS_DISABLED|MFS_GRAYED))&&!(info.fType&(MFT_OWNERDRAW|MFT_BITMAP));item.checked=(info.fState&MFS_CHECKED)!=0;item.commandId=info.wID;
  if(info.fType&MFT_SEPARATOR){item.kind=menu::ItemKind::Separator;item.enabled=false;}
  else if(info.hSubMenu){item.kind=menu::ItemKind::Submenu;item.children=Convert(info.hSubMenu,depth+1,budget);}
  else item.kind=menu::ItemKind::Command;
  if(item.text.empty()&&item.kind!=menu::ItemKind::Separator){item.text=L"(unsupported)";item.enabled=false;}result.push_back(std::move(item));
 }return result;}
}
std::vector<menu::MenuItem> FromNative(HMENU h){int budget=200;return Convert(h,0,budget);}
int HighlightedRow(HWND window){if(!window||window!=current||!IsWindow(window))return -1;auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(window,GWLP_USERDATA));return p?p->hover:-1;}
bool UpdateItems(HWND window,std::vector<menu::MenuItem> items){
 if(!window||window!=current||!IsWindow(window))return false;auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(!p||p->closing||!p->path.empty())return false;
 bool same=p->root.size()==items.size();if(same)for(size_t i=0;i<items.size();++i){const auto& a=p->root[i];const auto& b=items[i];if(a.text!=b.text||a.kind!=b.kind||a.enabled!=b.enabled||a.checked!=b.checked||a.commandId!=b.commandId){same=false;break;}}
 if(same)return true;UINT selectedId=0;if(const auto* old=ItemAt(p,p->hover))selectedId=old->commandId;
 p->root=std::move(items);p->hover=-1;if(selectedId)for(size_t i=0;i<p->root.size();++i)if(p->root[i].commandId==selectedId&&p->root[i].enabled){p->hover=static_cast<int>(i);break;}
 Resize(p);InvalidateRect(window,nullptr,FALSE);DLOG(L"menu: status rows updated without reopening");return true;
}
void SetVisualCaptureTest(bool enabled){visualCaptureTest=enabled;}
bool LastSelectionSecondary(){return lastSecondary;}
void SetTheme(bool opaque,COLORREF background,COLORREF foreground){themeOpaque=opaque;themeBackground=background;themeForeground=foreground;if(current&&IsWindow(current))InvalidateRect(current,nullptr,FALSE);}
void Close(){if(current&&IsWindow(current)){auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(current,GWLP_USERDATA));if(p)FinishClose(p);}current=nullptr;}
bool RequestDiagnosticSnapshot(HWND window,const std::wstring& path){
 if(!window||window!=current||!IsWindow(window))return false;
 auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(!p||!p->surface)return false;
 HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;CloseHandle(f);
 p->surface->RequestSnapshot(path);InvalidateRect(window,nullptr,FALSE);return true;
}
#ifdef LIQUID_MENU_DIAGNOSTICS
std::uint64_t DiagnosticContentVersion(HWND window){
 if(!window||window!=current||!IsWindow(window))return 0;auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(window,GWLP_USERDATA));return p&&p->surface?p->surface->ContentVersion():0;
}
bool DiagnosticHasSurface(HWND window){if(!window||window!=current||!IsWindow(window))return false;auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(window,GWLP_USERDATA));return p&&p->surface!=nullptr;}
#endif
int g_aboveBottom=INT_MIN;  // set by ShowAbove for the popup being opened
int g_gridCols=0;std::wstring g_gridTitle;  // set by ShowGridAbove for the popup being opened
HWND Create(HWND owner,POINT anchor,std::vector<menu::MenuItem> items,Select select,Closed closed,bool gpu,Popup* parent=nullptr){
 HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=kClass;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
 if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;
 auto p=std::make_unique<Popup>();p->owner=owner;p->parent=parent;p->aboveBottom=parent?INT_MIN:g_aboveBottom;p->keyboardFocus=!parent&&IsMenuBarOwner(owner);p->previousForeground=p->keyboardFocus?GetForegroundWindow():nullptr;p->root=std::move(items);p->select=std::move(select);p->closed=std::move(closed);p->dpi=GetDpiForWindow(owner);if(!p->dpi)p->dpi=96;p->mouseDown=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;p->escapeDown=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;
 if(!parent){p->gridCols=g_gridCols;p->gridTitle=g_gridTitle;}
 // Transfer ownership only once WM_NCCREATE accepts it. The outer flag handles creation failure.
 bool destroyed=false;p->destroyedDuringCreate=&destroyed;Popup* raw=p.release();const DWORD ex=WS_EX_TOOLWINDOW|WS_EX_TOPMOST|(raw->keyboardFocus?0:WS_EX_NOACTIVATE)|(gpu?WS_EX_NOREDIRECTIONBITMAP:WS_EX_LAYERED);
 HWND h=CreateWindowExW(ex,kClass,L"DockOver menu",WS_POPUP,anchor.x,anchor.y,Px(raw,244),Px(raw,100),owner,nullptr,instance,raw);
 if(!h){if(!destroyed)delete raw;return nullptr;}raw->destroyedDuringCreate=nullptr;
 if(gpu){
   raw->surface=std::make_unique<MenuSurface>();raw->surface->SetVisualCaptureTest(visualCaptureTest);RECT r{};GetClientRect(h,&r);
   bool forceInitFailure=false;
#ifdef LIQUID_MENU_DIAGNOSTICS
   wchar_t fault[4]{};forceInitFailure=GetEnvironmentVariableW(L"LIQUID_TEST_MENU_DEVICE_FAIL",fault,ARRAYSIZE(fault))>0;
#endif
   if(forceInitFailure||!raw->surface->Init(h,r.right,r.bottom,static_cast<float>(raw->dpi))){
      auto retryItems=std::move(raw->root);auto retrySelect=std::move(raw->select);auto retryClosed=std::move(raw->closed);
      DestroyWindow(h);DLOG(L"menu surface: switching to GDI fallback");return Create(owner,anchor,std::move(retryItems),std::move(retrySelect),std::move(retryClosed),false,parent);
   }
 }
 if(!parent){current=h;cached=h;}raw->callbacksEnabled=!parent;SetWindowDisplayAffinity(h,visualCaptureTest?WDA_NONE:WDA_EXCLUDEFROMCAPTURE);Resize(raw);
 raw->reducedMotion=ReducedMotion();BeginAppear(raw);SetAppear(raw,0.f);
 ShowWindow(h,raw->keyboardFocus?SW_SHOW:SW_SHOWNOACTIVATE);if(raw->keyboardFocus)SetForegroundWindow(h);
 if(!parent)raw->dismissal.Start(h);
 if(raw->surface){raw->surface->Start();SetTimer(h,4,1000,nullptr);}InvalidateRect(h,nullptr,FALSE);return h;
}
namespace {
void OpenChild(Popup* p,int row){
 const auto* item=ItemAt(p,row);if(!p||!item||item->kind!=menu::ItemKind::Submenu||item->children.empty())return;
 RECT pr{};GetWindowRect(p->window,&pr);MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromRect(&pr,MONITOR_DEFAULTTONEAREST),&mi))return;
 const int width=Px(p,244),overlap=Px(p,4);
 POINT at{pr.right-overlap,pr.top+Px(p,24*(row-p->scroll))};
 if(at.x+width>mi.rcWork.right)at.x=pr.left-width+overlap;  // no room on the right: open to the left
 HWND c=Create(p->owner,at,item->children,{},{},true,p);
 if(!c)return;p->child=c;p->childRow=row;p->childSticky=false;p->awayTicks=0;
 DLOG(L"menu: submenu opened row=%d items=%zu",row,item->children.size());
}
}
HWND Submenu(HWND window){auto* p=PopupOf(window);return p&&p->child&&IsWindow(p->child)?p->child:nullptr;}
HWND ShowImpl(HWND owner,POINT anchor,std::vector<menu::MenuItem> items,Select select,Closed closed);
HWND Show(HWND owner,POINT anchor,std::vector<menu::MenuItem> items,Select select,Closed closed){
 g_aboveBottom=INT_MIN;g_gridCols=0;g_gridTitle.clear();return ShowImpl(owner,anchor,std::move(items),std::move(select),std::move(closed));
}
HWND ShowAbove(HWND owner,POINT bottomLeft,std::vector<menu::MenuItem> items,Select select,Closed closed){
 g_aboveBottom=bottomLeft.y;g_gridCols=0;g_gridTitle.clear();const HWND h=ShowImpl(owner,bottomLeft,std::move(items),std::move(select),std::move(closed));g_aboveBottom=INT_MIN;return h;
}
int GridColumns(size_t count){  // one row up to 6, two balanced rows up to 12, then rows of 6
 if(count<=6)return count<1?1:static_cast<int>(count);
 if(count<=12)return static_cast<int>((count+1)/2);
 return 6;
}
HWND ShowGridAbove(HWND owner,POINT bottomLeft,std::vector<menu::MenuItem> items,const std::wstring& title,Select select,Closed closed){
 g_aboveBottom=bottomLeft.y;g_gridCols=GridColumns(items.size());g_gridTitle=title;
 const HWND h=ShowImpl(owner,bottomLeft,std::move(items),std::move(select),std::move(closed));
 g_aboveBottom=INT_MIN;g_gridCols=0;g_gridTitle.clear();return h;
}
bool ItemScreenRect(HWND window,size_t index,RECT& out){
 auto* p=PopupOf(window);if(!p)return false;RECT w{};if(!GetWindowRect(window,&w))return false;
 RECT r{};
 if(Grid(p)){if(static_cast<int>(index)>=GridCount(p))return false;r=GridCell(p,static_cast<int>(index));}
 else{const int row=static_cast<int>(index)-p->scroll;r=RECT{Px(p,6),Px(p,6+24*row),w.right-w.left-Px(p,6),Px(p,6+24*row+24)};}
 OffsetRect(&r,w.left,w.top);out=r;return true;
}
HWND ShowImpl(HWND owner,POINT anchor,std::vector<menu::MenuItem> items,Select select,Closed closed){
 Close();
 if(cached&&IsWindow(cached)){
  auto* p=reinterpret_cast<Popup*>(GetWindowLongPtrW(cached,GWLP_USERDATA));if(!p)return nullptr;
  p->owner=owner;p->aboveBottom=g_aboveBottom;p->gridCols=g_gridCols;p->gridTitle=g_gridTitle;p->keyboardFocus=IsMenuBarOwner(owner);p->previousForeground=p->keyboardFocus?GetForegroundWindow():nullptr;p->root=std::move(items);p->path.clear();p->select=std::move(select);p->closed=std::move(closed);p->scroll=0;p->hover=-1;p->tracking=false;p->closing=false;p->hasSelection=false;p->callbacksEnabled=true;
  p->dpi=GetDpiForWindow(owner);if(!p->dpi)p->dpi=96;p->mouseDown=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;p->escapeDown=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;  SetWindowLongPtrW(cached,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));
  LONG_PTR ex=GetWindowLongPtrW(cached,GWL_EXSTYLE);SetWindowLongPtrW(cached,GWL_EXSTYLE,p->keyboardFocus?ex&~WS_EX_NOACTIVATE:ex|WS_EX_NOACTIVATE);
  SetWindowPos(cached,HWND_TOPMOST,anchor.x,anchor.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);current=cached;
  SetWindowDisplayAffinity(cached,visualCaptureTest?WDA_NONE:WDA_EXCLUDEFROMCAPTURE);Resize(p);SetTimer(cached,1,40,nullptr);
  p->reducedMotion=ReducedMotion();BeginAppear(p);SetAppear(p,0.f);
  ShowWindow(cached,p->keyboardFocus?SW_SHOW:SW_SHOWNOACTIVATE);if(p->keyboardFocus)SetForegroundWindow(cached);
  p->dismissal.Start(cached);
  if(p->surface){p->surface->Start();SetTimer(cached,4,1000,nullptr);}InvalidateRect(cached,nullptr,FALSE);return cached;
 }
 return Create(owner,anchor,std::move(items),std::move(select),std::move(closed),true);
}
}
