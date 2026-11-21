#include "unified_menu.h"
#include "log.h"
#include "native_menu.h"
#include "renderer.h"
#include <wincodec.h>
#include <windows.h>
#include <roapi.h>
#include <d3d11.h>
#include <dwrite.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <map>
#include <vector>
using namespace liquid;
void Pump(int ms){DWORD end=GetTickCount()+ms;while(static_cast<int>(end-GetTickCount())>0){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}}
bool checkerChanged=false;
LRESULT CALLBACK Checker(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_APP+10){checkerChanged=!checkerChanged;InvalidateRect(h,nullptr,FALSE);UpdateWindow(h);return 0;}
 if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);for(int y=0;y<r.bottom;y+=40)for(int x=0;x<r.right;x+=40){HBRUSH b=CreateSolidBrush((((x/40+y/40)&1)^checkerChanged)?RGB(240,90,50):RGB(25,180,220));RECT tile{x,y,x+40,y+40};FillRect(dc,&tile,b);DeleteObject(b);}EndPaint(h,&ps);return 0;}
 return DefWindowProcW(h,m,w,l);
}
int VisualTest(){
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Checker;wc.lpszClassName=L"LiquidMenuChecker98d2";RegisterClassW(&wc);
 HWND background=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Menu glass test",WS_POPUP,100,100,800,500,nullptr,nullptr,wc.hInstance,nullptr);if(!background)return 30;ShowWindow(background,SW_SHOWNOACTIVATE);UpdateWindow(background);
 menu::MenuItem first;first.text=L"Sample menu item";first.enabled=true;menu::MenuItem second;second.text=L"Checked item";second.enabled=true;second.checked=true;
 HWND h=popup::Show(background,POINT{260,220},{first,second},{},{});if(!h)return 31;Pump(1500);
 DWORD affinity=0;if(!GetWindowDisplayAffinity(h,&affinity)||affinity!=WDA_EXCLUDEFROMCAPTURE)return 34;
 auto baseline=popup::DiagnosticContentVersion(h);if(!baseline)return 35;
 SendMessageW(h,WM_MOUSEMOVE,0,MAKELPARAM(20,15));Pump(900);auto unchanged=popup::DiagnosticContentVersion(h);
 if(unchanged!=baseline){printf("FAIL self-exclusion baseline=%llu after=%llu\n",static_cast<unsigned long long>(baseline),static_cast<unsigned long long>(unchanged));return 36;}
 SendMessageW(background,WM_APP+10,0,0);for(int tries=0;tries<30&&popup::DiagnosticContentVersion(h)==baseline;++tries)Pump(100);
 auto changed=popup::DiagnosticContentVersion(h);if(changed<=baseline){puts("FAIL dynamic content update");return 37;}
 printf("PASS exclusion static_version=%llu changed_version=%llu\n",static_cast<unsigned long long>(baseline),static_cast<unsigned long long>(changed));
 wchar_t temp[MAX_PATH]{};GetTempPathW(ARRAYSIZE(temp),temp);std::wstring file=std::wstring(temp)+L"liquid-menu-glass-98d2-"+std::to_wstring(GetCurrentProcessId())+L".bmp";
 if(!popup::RequestDiagnosticSnapshot(h,file))return 32;Pump(600);WIN32_FILE_ATTRIBUTE_DATA info{};bool saved=GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&info)&&info.nFileSizeLow>54;
 popup::Close();DestroyWindow(background);if(!saved){puts("FAIL no GPU snapshot");return 33;}wprintf(L"PASS GPU diagnostic snapshot %ls\n",file.c_str());fflush(stdout);return 0;
}
int IntegrationTest(){
 std::wstring event=L"Local\\LiquidUnifiedIntegration98d2-"+std::to_wstring(GetCurrentProcessId());HANDLE signal=CreateEventW(nullptr,TRUE,FALSE,event.c_str());if(!signal)return 40;
 wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));std::wstring path=module;auto slash=path.find_last_of(L"\\/");path=path.substr(0,slash+1)+L"LiquidNativeMenuTest.exe";
 std::wstring cmd=L"\""+path+L"\" --child \""+event+L"\"";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};if(!CreateProcessW(path.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){CloseHandle(signal);return 41;}CloseHandle(pi.hThread);
 struct Find{DWORD pid;HWND hwnd;};Find found{pi.dwProcessId,nullptr};for(int i=0;i<40&&!found.hwnd;++i){Sleep(100);EnumWindows([](HWND h,LPARAM value)->BOOL{auto* f=reinterpret_cast<Find*>(value);DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t c[80]{};GetClassNameW(h,c,ARRAYSIZE(c));if(pid==f->pid&&std::wstring(c)==L"LiquidNativeMenuTest4bd7"){f->hwnd=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&found));}
 if(!found.hwnd){CloseHandle(pi.hProcess);CloseHandle(signal);return 42;}
 auto menus=menu::ReadNativeMenus(found.hwnd);if(menus.size()<2||menus[0].items.size()<6){PostMessageW(found.hwnd,WM_CLOSE,0,0);CloseHandle(pi.hProcess);CloseHandle(signal);return 43;}
 bool queued=false;auto selectedMenu=menus[0];HWND pop=popup::Show(found.hwnd,POINT{300,200},selectedMenu.items,[&](const menu::MenuItem& item){queued=menu::DispatchNativeCommand(selectedMenu,item);});
 if(!pop){PostMessageW(found.hwnd,WM_CLOSE,0,0);CloseHandle(pi.hProcess);CloseHandle(signal);return 44;}Pump(500);UINT dpi=GetDpiForWindow(found.hwnd);int pad=MulDiv(6,dpi,96),row=MulDiv(28,dpi,96);
 SendMessageW(pop,WM_LBUTTONUP,0,MAKELPARAM(20,pad+row/2));Pump(300);
 if(!queued||WaitForSingleObject(signal,3000)!=WAIT_OBJECT_0){PostMessageW(found.hwnd,WM_CLOSE,0,0);CloseHandle(pi.hProcess);CloseHandle(signal);return 45;}
 queued=false;pop=popup::Show(found.hwnd,POINT{300,200},selectedMenu.items,[&](const menu::MenuItem& item){queued=menu::DispatchNativeCommand(selectedMenu,item);});if(!pop){PostMessageW(found.hwnd,WM_CLOSE,0,0);CloseHandle(pi.hProcess);CloseHandle(signal);return 46;}
 Pump(300);SendMessageW(pop,WM_LBUTTONUP,0,MAKELPARAM(20,pad+5*row+row/2));Pump(300);bool exited=WaitForSingleObject(pi.hProcess,3000)==WAIT_OBJECT_0;
 if(!exited)PostMessageW(found.hwnd,WM_CLOSE,0,0);CloseHandle(pi.hProcess);CloseHandle(signal);if(!queued||!exited)return 47;
 puts("PASS unified popup -> real Win32 New event and Exit process");fflush(stdout);return 0;
}
int FallbackTest(){
 SetEnvironmentVariableW(L"LIQUID_TEST_MENU_DEVICE_FAIL",L"1");
 HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Fallback test",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 50;
 menu::MenuItem item;item.text=L"Fallback command";item.enabled=true;item.commandId=77;UINT selected=0;int closed=0;
 HWND h=popup::Show(owner,POINT{100,100},{item},[&](const menu::MenuItem& e){selected=e.commandId;},[&]{++closed;});
 SetEnvironmentVariableW(L"LIQUID_TEST_MENU_DEVICE_FAIL",nullptr);if(!h||popup::DiagnosticHasSurface(h))return 51;Pump(200);
 UINT dpi=GetDpiForWindow(owner);SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(20,MulDiv(20,dpi,96)));Pump(250);
 if(IsWindowVisible(h)||selected!=77||closed!=1)return 52;DestroyWindow(owner);puts("PASS GDI fallback creation, selection and close");fflush(stdout);return 0;
}
int DismissalTest() {
 popup::SetVisualCaptureTest(true);
 const auto instance=GetModuleHandleW(nullptr);
 const HWND owner=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"STATIC",L"Menu dismissal owner",WS_POPUP,80,80,600,340,nullptr,nullptr,instance,nullptr);
 const HWND other=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"STATIC",L"Menu dismissal focus target",WS_POPUP,440,80,200,100,nullptr,nullptr,instance,nullptr);
 int closed=0;
 const int result=[&]{
  if(!owner||!other)return 80;
  ShowWindow(owner,SW_SHOW);
  SetForegroundWindow(owner);
  Pump(120);
  if(GetForegroundWindow()!=owner)return 81;
  menu::MenuItem item;
  item.text=L"Dismissal test";item.enabled=true;
  auto open=[&]{return popup::Show(owner,POINT{270,195},{item},{},[&]{++closed;});};
  HWND menu=open();
  if(!menu)return 82;
  Pump(350);
  if(!IsWindowVisible(menu))return 83;
  ShowWindow(other,SW_SHOW);
  SetForegroundWindow(other);
  Pump(400);
  if(GetForegroundWindow()!=other||IsWindowVisible(menu)||closed!=1)return 87;
  puts("PASS popup focus-loss dismissal");
  ShowWindow(other,SW_HIDE);
  SetForegroundWindow(owner);
  Pump(120);
  if(GetForegroundWindow()!=owner)return 81;
  menu=open();
  if(!menu)return 86;
  Pump(350);
  POINT original{};
  const POINT outside{120,130};
  if(WindowFromPoint(outside)!=owner||!GetCursorPos(&original)||!SetCursorPos(outside.x,outside.y))return 84;
  INPUT click[2]{};
  click[0].type=click[1].type=INPUT_MOUSE;
  click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
  const UINT sent=SendInput(ARRAYSIZE(click),click,sizeof(INPUT));
  Pump(400);
  SetCursorPos(original.x,original.y);
  if(sent!=ARRAYSIZE(click)||IsWindowVisible(menu)||closed!=2||GetForegroundWindow()!=owner)return 85;
  puts("PASS brief outside click with unchanged foreground");return 0;
 }();
 popup::Close();
 if(other)DestroyWindow(other);
 if(owner)DestroyWindow(owner);
 return result;
}
struct HandleEntry {void* object;ULONG_PTR pid;ULONG_PTR value;ULONG access;USHORT creator;USHORT type;ULONG attributes;ULONG reserved;};
struct HandleInfo {ULONG_PTR count;ULONG_PTR reserved;HandleEntry entries[1];};
struct TypeName {USHORT length,maximum;wchar_t* buffer;};
std::map<std::wstring,int> HandleTypes(){
 std::map<std::wstring,int> out;using QuerySystem=LONG (WINAPI*)(ULONG,void*,ULONG,ULONG*);using QueryObject=LONG (WINAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
 HMODULE n=GetModuleHandleW(L"ntdll.dll");auto qs=reinterpret_cast<QuerySystem>(GetProcAddress(n,"NtQuerySystemInformation"));auto qo=reinterpret_cast<QueryObject>(GetProcAddress(n,"NtQueryObject"));if(!qs||!qo)return out;
 ULONG needed=0,size=1<<20;std::vector<BYTE> data;LONG status=0;for(int i=0;i<10;++i){data.resize(size);status=qs(64,data.data(),size,&needed);if(status>=0)break;size=(std::max)(size*2,needed+65536);if(size>128u*1024u*1024u)return out;}if(status<0)return out;
 auto* info=reinterpret_cast<HandleInfo*>(data.data());DWORD pid=GetCurrentProcessId();for(ULONG_PTR i=0;i<info->count;++i){auto& e=info->entries[i];if(e.pid!=pid)continue;BYTE typeBuffer[2048]{};ULONG length=0;if(qo(reinterpret_cast<HANDLE>(e.value),2,typeBuffer,sizeof(typeBuffer),&length)<0)continue;auto* name=reinterpret_cast<TypeName*>(typeBuffer);if(name->buffer&&name->length)++out[std::wstring(name->buffer,name->length/sizeof(wchar_t))];}return out;
}
void PrintTypeDiff(const std::map<std::wstring,int>& before,const std::map<std::wstring,int>& after){for(const auto& [name,count]:after){auto it=before.find(name);int delta=count-(it==before.end()?0:it->second);if(delta)printf("handle type %ls delta %+d\n",name.c_str(),delta);}fflush(stdout);}  // V0.17: no wchar_t->char narrowing (C4244)
int IsolateTest(){
 using Microsoft::WRL::ComPtr;auto run=[](const char* name,auto fn){fn();Pump(500);DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);auto types1=HandleTypes();for(int i=0;i<12;++i)fn();Pump(1200);GetProcessHandleCount(GetCurrentProcess(),&after);auto types2=HandleTypes();printf("%s handles %lu -> %lu\n",name,before,after);PrintTypeDiff(types1,types2);};
 run("d3d",[]{ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level{};D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c);});
 run("dxgi factory",[]{ComPtr<IDXGIFactory2> f;CreateDXGIFactory2(0,IID_PPV_ARGS(&f));});
 run("dwrite",[]{ComPtr<IDWriteFactory> f;DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(f.GetAddressOf()));});
 run("d2d factory",[]{ComPtr<ID2D1Factory1> f;D2D1_FACTORY_OPTIONS o{};D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&o,reinterpret_cast<void**>(f.GetAddressOf()));});
 run("dcomp",[]{ComPtr<IDCompositionDevice> d;DCompositionCreateDevice(nullptr,IID_PPV_ARGS(&d));});
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);ComPtr<IDXGIDevice> dxgi;if(device)device.As(&dxgi);
 run("composition swapchain",[&]{if(!dxgi)return;ComPtr<IDXGIFactory2> factory;if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return;DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=200;desc.Height=100;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;ComPtr<IDXGISwapChain1> swap;if(SUCCEEDED(factory->CreateSwapChainForComposition(dxgi.Get(),&desc,nullptr,&swap))){ComPtr<IDXGISwapChain2> s2;if(SUCCEEDED(swap.As(&s2))){HANDLE wait=s2->GetFrameLatencyWaitableObject();if(wait)CloseHandle(wait);}}});
 run("swapchain without waitable",[&]{if(!dxgi)return;ComPtr<IDXGIFactory2> factory;if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return;DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=200;desc.Height=100;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;desc.Flags=0;ComPtr<IDXGISwapChain1> swap;if(SUCCEEDED(factory->CreateSwapChainForComposition(dxgi.Get(),&desc,nullptr,&swap))){}});
 return 0;
}
int StressTest(){
 using Microsoft::WRL::ComPtr;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
 HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
 ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter3> adapter3;
 if(SUCCEEDED(hr))hr=device.As(&dxgi);if(SUCCEEDED(hr))hr=dxgi->GetAdapter(&adapter);if(SUCCEEDED(hr))adapter.As(&adapter3);
 HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Resource test",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 60;
 menu::MenuItem item;item.text=L"Resource test";item.enabled=true;
 int cycleNumber=0;auto cycle=[&](){std::vector<menu::MenuItem> varied((++cycleNumber%2)?1:6,item);HWND h=popup::Show(owner,POINT{400,250},std::move(varied),{},{});if(!h)return false;Pump(250);popup::Close();Pump(100);return !IsWindowVisible(h);};
 if(!cycle())return 61;Pump(500);DWORD h0=0,h1=0;GetProcessHandleCount(GetCurrentProcess(),&h0);DWORD g0=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);DXGI_QUERY_VIDEO_MEMORY_INFO a{},b{};bool gpu=adapter3&&SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&a));
 for(int i=0;i<12;++i)if(!cycle())return 62;Pump(6000);GetProcessHandleCount(GetCurrentProcess(),&h1);DWORD g1=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);if(gpu)gpu=SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&b));auto types1=HandleTypes();
 printf("resource stress first 12: handles %lu -> %lu, GDI %lu -> %lu, GPU known=%d %llu -> %llu bytes\n",h0,h1,g0,g1,gpu,static_cast<unsigned long long>(a.CurrentUsage),static_cast<unsigned long long>(b.CurrentUsage));fflush(stdout);
 for(int i=0;i<12;++i)if(!cycle())return 64;Pump(6000);DWORD h2=0;GetProcessHandleCount(GetCurrentProcess(),&h2);DWORD g2=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);auto types2=HandleTypes();PrintTypeDiff(types1,types2);
 printf("resource stress next 12: handles %lu -> %lu, GDI %lu -> %lu\n",h1,h2,g1,g2);fflush(stdout);
 DestroyWindow(owner);if(h2>h1+8||g2>g1+4)return 63;puts("PASS bounded handles and GDI resources");fflush(stdout);return 0;
}
int OpaqueFallbackTest(bool light){
 wchar_t dir[MAX_PATH]{};if(!GetEnvironmentVariableW(L"LIQUID_DOCK_SNAPSHOT_DIR",dir,MAX_PATH))return 70;
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"DockOpaqueFallbackTest";RegisterClassW(&wc);
 HWND h=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"opaque test",WS_POPUP,0,0,200,200,nullptr,nullptr,wc.hInstance,nullptr);if(!h)return 71;
 dock::Renderer renderer;if(FAILED(renderer.Init(h,200,200,96))){DestroyWindow(h);return 72;}
 dock::DockFrame frame;frame.dockLeft=20;frame.dockTop=20;frame.dockRight=180;frame.dockBottom=180;dock::ItemGeom item;item.cx=100;frame.items.push_back(item);
 dock::DockMetrics metrics;dock::Scene scene;scene.frame=&frame;scene.metrics=&metrics;scene.windowW=200;scene.windowH=200;scene.opaqueFallback=true;scene.opaqueBackground=light?RGB(255,255,255):RGB(36,36,40);scene.opaqueForeground=light?RGB(0,0,0):RGB(255,255,255);scene.icons.resize(1);scene.icons[0].running=true;
 renderer.RequestPngSnapshot();const HRESULT render=renderer.Render(scene);renderer.Shutdown();DestroyWindow(h);if(FAILED(render))return 73;
 WIN32_FIND_DATAW found{};const std::wstring pattern=std::wstring(dir)+L"\\dock-*.png";HANDLE find=FindFirstFileW(pattern.c_str(),&found);if(find==INVALID_HANDLE_VALUE)return 74;FindClose(find);
 const auto path=std::wstring(dir)+L"\\"+found.cFileName;
 Microsoft::WRL::ComPtr<IWICImagingFactory> factory;Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> decoded;Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
 if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&decoded);if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
 if(SUCCEEDED(hr))hr=converter->Initialize(decoded.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<BYTE> pixels(200*200*4);if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,800,static_cast<UINT>(pixels.size()),pixels.data());if(FAILED(hr))return 75;
 const size_t center=(100*200+100)*4,dot=(174*200+100)*4;
 const BYTE blue=light?255:40,redGreen=light?255:36,foreground=light?0:255;
 if(pixels[center]!=blue||pixels[center+1]!=redGreen||pixels[center+2]!=redGreen||pixels[center+3]!=255||pixels[3]!=0)return 76;
 if(pixels[dot]!=foreground||pixels[dot+1]!=foreground||pixels[dot+2]!=foreground||pixels[dot+3]!=255)return 77;
 puts("PASS opaque fallback rendered opaque plate and transparent exterior (offscreen frame only)");return 0;
}
int main(int argc,char** argv){
 HRESULT ro=RoInitialize(RO_INIT_SINGLETHREADED);
 wchar_t tmp[MAX_PATH]{};GetTempPathW(ARRAYSIZE(tmp),tmp);dock::Log::Get().Open(std::wstring(tmp)+L"liquid-unified-menu-test-98d2.log");
 if(argc>1&&(std::string(argv[1])=="--opaque"||std::string(argv[1])=="--opaque-light")){int code=OpaqueFallbackTest(std::string(argv[1])=="--opaque-light");if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--visual"){int code=VisualTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--integration"){int code=IntegrationTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--fallback"){int code=FallbackTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--dismiss"){int code=DismissalTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--stress"){int code=StressTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 if(argc>1&&std::string(argv[1])=="--isolate"){int code=IsolateTest();if(SUCCEEDED(ro))RoUninitialize();return code;}
 puts("start");fflush(stdout);
 HMENU root=CreatePopupMenu(),sub=CreatePopupMenu();AppendMenuW(root,MF_STRING,17,L"&First");AppendMenuW(root,MF_STRING|MF_GRAYED,18,L"Disabled");AppendMenuW(sub,MF_STRING|MF_CHECKED,19,L"&Nested");AppendMenuW(root,MF_POPUP,(UINT_PTR)sub,L"Submenu");AppendMenuW(root,MF_SEPARATOR,0,nullptr);
 puts("native created");fflush(stdout);auto items=popup::FromNative(root);DestroyMenu(root);if(items.size()!=4||items[0].text!=L"First"||items[1].enabled||items[2].children.size()!=1||!items[2].children[0].checked||items[3].kind!=menu::ItemKind::Separator)return 1;
 HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Popup test owner",WS_POPUP,10,10,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!owner)return 2;
 UINT selected=0;int closed=0;POINT anchor{100000,100000};HWND win=popup::Show(owner,anchor,items,[&](const menu::MenuItem& item){selected=item.commandId;},[&]{++closed;});if(!win)return 3;puts("popup created");fflush(stdout);Pump(1500);
 RECT wr{};GetWindowRect(win,&wr);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&wr,MONITOR_DEFAULTTONEAREST),&mi);if(wr.left<mi.rcWork.left||wr.right>mi.rcWork.right||wr.top<mi.rcWork.top||wr.bottom>mi.rcWork.bottom)return 4;
 const UINT dpi=GetDpiForWindow(owner);const int pad=MulDiv(6,dpi,96),row=MulDiv(28,dpi,96);
 SendMessageW(win,WM_LBUTTONUP,0,MAKELPARAM(20,pad+row+row/2));if(!IsWindow(win)||selected)return 5;
 SendMessageW(win,WM_LBUTTONUP,0,MAKELPARAM(20,pad+2*row+row/2));puts("submenu entered");fflush(stdout);Pump(50);
 // A clicked submenu row cascades: its items open (pinned) in a child popup to the right; choosing one selects through the root.
 HWND child=popup::Submenu(win);if(!child||!IsWindowVisible(child))return 7;
 SendMessageW(child,WM_LBUTTONUP,0,MAKELPARAM(20,pad+row/2));Pump(250);if(IsWindow(child)&&IsWindowVisible(child))return 8;puts("after close pump");fflush(stdout);
 printf("state window=%d selected=%u closed=%d\n",IsWindow(win),selected,closed);fflush(stdout);if(IsWindowVisible(win)||selected!=19||closed!=1)return 6;
 popup::Close();puts("after popup close");fflush(stdout);printf("owner=%p valid=%d\n",owner,IsWindow(owner));fflush(stdout);BOOL gone=DestroyWindow(owner);printf("destroyed=%d error=%lu\n",gone,GetLastError());fflush(stdout);puts("PASS menu conversion, disabled, checked, submenu, callback, close and screen bounds");fflush(stdout);if(SUCCEEDED(ro))RoUninitialize();return 0;
}
