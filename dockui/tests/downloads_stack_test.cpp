#include "downloads_stack.h"
#include "fan_layout.h"
#include <windows.h>
#include <cstdio>
#include <string>
using namespace dock::downloads;
bool SavePanel(HWND h,const std::wstring& path){
 RECT r{};GetClientRect(h,&r);int width=r.right,height=r.bottom;if(width<=0||height<=0)return false;
 HDC screen=GetDC(h),mem=CreateCompatibleDC(screen);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;void* bits=nullptr;
 HBITMAP bmp=CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0);if(!bmp||!bits){if(bmp)DeleteObject(bmp);DeleteDC(mem);ReleaseDC(h,screen);return false;}
 HGDIOBJ old=SelectObject(mem,bmp);bool ok=PrintWindow(h,mem,2)!=FALSE;GdiFlush();
 HANDLE file=ok?CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr):INVALID_HANDLE_VALUE;
 if(file!=INVALID_HANDLE_VALUE){BITMAPFILEHEADER fh{};fh.bfType=0x4D42;fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);fh.bfSize=fh.bfOffBits+width*height*4;bi.bmiHeader.biSizeImage=width*height*4;DWORD n=0;
  ok=WriteFile(file,&fh,sizeof(fh),&n,nullptr)&&n==sizeof(fh)&&WriteFile(file,&bi.bmiHeader,sizeof(bi.bmiHeader),&n,nullptr)&&n==sizeof(bi.bmiHeader)&&WriteFile(file,bits,width*height*4,&n,nullptr)&&n==static_cast<DWORD>(width*height*4);CloseHandle(file);
 }else ok=false;
 SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(h,screen);return ok;
}
int wmain(int argc,wchar_t** argv){
 if(argc==1){HANDLE signal=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\LiquidDownloadsOpenFixture98d2");if(signal){SetEvent(signal);CloseHandle(signal);return 0;}return 2;}
 if(argc==3 && std::wstring(argv[1])==L"--fan-open-test"){
  HANDLE signal=CreateEventW(nullptr,TRUE,FALSE,L"Local\\LiquidDownloadsOpenFixture98d2");if(!signal)return 70;
  HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Stack test owner",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 71;
  auto pump=[](int ticks){for(int t=0;t<ticks;++t){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}};
  RECT anchor{500,800,550,850};Settings settings;settings.view=View::Fan;
  for(int pass=0;pass<2;++pass){
   ResetEvent(signal);if(!Toggle(owner,anchor,argv[2],settings))return 72;pump(60);
   HWND panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);if(!panel)return 73;
   RECT client{},window{};GetClientRect(panel,&client);GetWindowRect(panel,&window);float factor=GetDpiForWindow(panel)/96.f;
   auto g=FanLayout(0,1,client.right/factor,client.bottom/factor,(525-window.left)/factor,1.f);
   float x=pass==0?(g.icon.left+g.icon.right)/2.f:(g.label.left+g.label.right)/2.f;
   float y=pass==0?(g.icon.top+g.icon.bottom)/2.f:(g.label.top+g.label.bottom)/2.f;
   SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(static_cast<int>(x*factor),static_cast<int>(y*factor)));
   bool opened=WaitForSingleObject(signal,5000)==WAIT_OBJECT_0;pump(15);
   if(!opened||IsWindow(panel))return pass==0?74:75;
  }
  if(!Toggle(owner,anchor,argv[2],settings))return 76;pump(20);
  HWND panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);if(!panel)return 77;
  // A repeated toggle requests a timed close instead of destroying immediately.
  if(!Toggle(owner,anchor,argv[2],settings))return 78;pump(15);if(IsWindow(panel))return 79;
  if(!Toggle(owner,anchor,argv[2],settings))return 80;pump(20);
  panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);if(!panel)return 81;
  SendMessageW(panel,WM_ACTIVATE,WA_INACTIVE,0);pump(15);if(IsWindow(panel))return 82;
  Close();DestroyWindow(owner);CloseHandle(signal);puts("PASS fan icon/label activation and animated toggle/outside close");return 0;
 }
 if(argc==3 && std::wstring(argv[1])==L"--open-test"){
  HANDLE signal=CreateEventW(nullptr,TRUE,FALSE,L"Local\\LiquidDownloadsOpenFixture98d2");if(!signal)return 40;
  HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Stack test owner",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 41;
  Settings settings;settings.view=View::List;RECT anchor{500,800,550,850};if(!Toggle(owner,anchor,argv[2],settings))return 42;
  for(int t=0;t<60;++t){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}
  HWND panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);if(!panel)return 43;
  UINT dpi=GetDpiForWindow(panel);SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(MulDiv(25,dpi,96),MulDiv(63,dpi,96)));
  bool opened=WaitForSingleObject(signal,5000)==WAIT_OBJECT_0;Close();DestroyWindow(owner);CloseHandle(signal);if(!opened)return 44;
  puts("PASS panel click opened isolated executable through ShellExecute");fflush(stdout);return 0;
 }
 if(argc==3 && std::wstring(argv[1])==L"--visual"){
  HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Stack test owner",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 30;
  RECT anchor{500,800,550,850};int index=0;for(View view:{View::Fan,View::Grid,View::List}){
   Settings settings;settings.view=view;if(!Toggle(owner,anchor,argv[2],settings))return 31;
   for(int t=0;t<45;++t){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}
   HWND panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);if(!panel)return 32;
   wchar_t tmp[MAX_PATH]{};GetTempPathW(ARRAYSIZE(tmp),tmp);std::wstring file=std::wstring(tmp)+L"liquid-stack-view-98d2-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(index++)+L".bmp";
   if(!SavePanel(panel,file))return 33;wprintf(L"snapshot %ls\n",file.c_str());Close();
  }DestroyWindow(owner);fflush(stdout);return 0;
 }
 if(argc==4 && std::wstring(argv[1])==L"--ui-background"){
  // V0.18: open a Grid panel after <delayMs>, during which the driving script gives the last real input to another
  // process, so Windows refuses this process the foreground. Prints granted/refused and whether the panel survives.
  HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Stack test owner",WS_POPUP|WS_VISIBLE,-300,-300,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 20;
  auto pump=[](int ms){for(int t=0;t<ms/25;++t){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}};
  pump(_wtoi(argv[3]));
  RECT anchor{300,700,350,750};Settings settings;settings.view=View::Grid;if(!Toggle(owner,anchor,argv[2],settings))return 21;
  const HWND created=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);const bool granted=created&&GetForegroundWindow()==created;
  pump(1500);const bool open=created&&IsWindow(created)&&IsWindowVisible(created);
  wprintf(L"background: foreground %ls; panel %ls after 1.5 s\n",granted?L"granted":L"refused",open?L"open":L"closed");fflush(stdout);
  if(!open)return 26;
  if(argc==4){wprintf(L"READY %p\n",created);fflush(stdout);pump(_wtoi(L"6000"));}  // window for the script's real outside click / Esc
  const bool stillOpen=IsWindow(created)&&IsWindowVisible(created);wprintf(L"after interaction: panel %ls\n",stillOpen?L"open":L"closed");fflush(stdout);
  Close();DestroyWindow(owner);return stillOpen?27:0;
 }
 if(argc==3 && std::wstring(argv[1])==L"--ui"){
  // V0.17: the owner must be a visible, activatable window like the Dock. With a hidden owner, destroying the active
  // panel made Windows hand the foreground to whichever app was active before the test (asynchronously); that app could
  // then take it back from the next panel. Kept off-screen so nothing appears on the user's desktop.
  HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Stack test owner",WS_POPUP|WS_VISIBLE,-300,-300,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 20;
  auto pump=[](int ms){for(int t=0;t<ms/25;++t){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}};
  RECT anchor{300,700,350,750};for(View view:{View::Fan,View::Grid,View::List}){
   Settings settings;settings.view=view;if(!Toggle(owner,anchor,argv[2],settings))return 21;
   const HWND created=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);
   pump(500);
   HWND panel=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);
   if(!panel||!IsWindowVisible(panel)){  // V0.17: say why, instead of a bare exit code
    wprintf(L"FAIL view %d: created=%d destroyed=%d visible=%d\n",static_cast<int>(view),created?1:0,created&&!IsWindow(created)?1:0,panel&&IsWindowVisible(panel)?1:0);return 22;}
   SendMessageW(panel,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-120)),0);Close();if(IsWindow(panel))return 23;
  }
  // V0.17: open a panel after this process lost the foreground (owner hidden, foreground moved to another app) and
  // check it stays open. Reports whether Windows granted or refused the foreground; the refused path is not forced here.
  ShowWindow(owner,SW_HIDE);
  for(int t=0;t<80;++t){pump(25);DWORD fgPid=0;HWND fg=GetForegroundWindow();if(fg)GetWindowThreadProcessId(fg,&fgPid);if(fg&&fgPid!=GetCurrentProcessId())break;}
  {Settings settings;settings.view=View::Grid;if(!Toggle(owner,anchor,argv[2],settings))return 24;
   const HWND created=FindWindowW(L"LiquidDownloadsStack98d2",nullptr);const bool gotForeground=created&&GetForegroundWindow()==created;pump(500);
   if(!created||!IsWindow(created)||!IsWindowVisible(created)){wprintf(L"FAIL background open: created=%d destroyed=%d\n",created?1:0,created&&!IsWindow(created)?1:0);return 25;}
   wprintf(L"background open: panel stayed open (foreground %ls)\n",gotForeground?L"granted":L"refused");Close();}
  DestroyWindow(owner);puts("PASS Fan Grid List panel creation, visibility, wheel and close; background open stays open");return 0;
 }
 if(argc!=2)return 2;std::wstring dir=argv[1],settings=dir+L"\\settings-test.txt",bad=dir+L"\\invalid-test.txt";
 if(KnownDownloadsPath().empty()){puts("FAIL Known Folder");return 3;}
 auto icon=MakeStackIcon(256);if(icon.size()!=256u*256u || (icon[0]>>24)!=0 || (icon[128u*256u+128u]>>24)==0)return 13;
 for(auto pixel:icon){unsigned a=pixel>>24;if(((pixel>>16)&255)>a||((pixel>>8)&255)>a||(pixel&255)>a)return 14;}
 DWORD err=0;auto list=Scan(dir+L"\\data",err);if(err||list.size()!=3){printf("FAIL scan count=%zu err=%lu\n",list.size(),err);return 4;}
 SortEntries(list,Sort::Name);if(list[0].name!=L"a.txt"||list[1].name!=L"b.png"||list[2].name!=L"c.txt")return 5;
 SortEntries(list,Sort::Type);if(list[0].name!=L"b.png")return 6;
 SortEntries(list,Sort::Date);if(CompareFileTime(&list[0].modified,&list[1].modified)<0)return 7;
 Settings v{View::Fan,Sort::Name,Display::Folder},r;std::wstring error;
 if(Settings{}.view!=View::Fan)return 50;
 const auto absent=dir+L"\\never-created-preference.txt";if(!LoadSettings(absent,r,error)||r.view!=View::Fan)return 51;
 for(auto view:{View::Fan,View::Grid,View::List}){v.view=view;
  if(!SaveSettings(settings,v,error)||!LoadSettings(settings,r,error)||r.view!=view||r.sort!=v.sort||r.display!=v.display)return 8;
 }
 // A saved Grid/List must survive each read without being replaced by the default.
 if(!LoadSettings(settings,r,error)||r.view!=View::List)return 52;
 HANDLE f=CreateFileW(bad.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return 9;const char payload[]="INVALID";DWORD n=0;WriteFile(f,payload,7,&n,nullptr);CloseHandle(f);
 if(LoadSettings(bad,r,error)||SaveSettings(bad,v,error))return 10;
 f=CreateFileW(bad.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);char bytes[8]{};ReadFile(f,bytes,7,&n,nullptr);CloseHandle(f);if(std::string(bytes)!="INVALID")return 11;
 auto missing=Scan(dir+L"\\missing",err);if(!missing.empty()||err==ERROR_SUCCESS)return 12;
 puts("PASS Known Folder, read-only scan, name/date/type sort, settings round-trip, corrupt settings preserved, missing folder");return 0;
}
