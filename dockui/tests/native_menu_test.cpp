#include "native_menu.h"
#include <windows.h>
#include <cstdio>
#include <string>
using namespace liquid::menu;
static HANDLE notified=nullptr;
LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_COMMAND){if(LOWORD(w)==101){SetWindowTextW(h,L"New command received");if(notified)SetEvent(notified);return 0;}if(LOWORD(w)==102){DestroyWindow(h);return 0;}}
 if(m==WM_TIMER){DestroyWindow(h);return 0;}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l);
}
int wmain(int argc,wchar_t** argv){
 if(argc==3 && std::wstring(argv[1])==L"--child"){
  notified=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]);HINSTANCE h=GetModuleHandleW(nullptr);WNDCLASSW c{};c.hInstance=h;c.lpfnWndProc=WindowProc;c.lpszClassName=L"LiquidNativeMenuTest4bd7";RegisterClassW(&c);
  HMENU bar=CreateMenu(),file=CreatePopupMenu(),edit=CreatePopupMenu(),nested=CreatePopupMenu();AppendMenuW(file,MF_STRING,101,L"&New");AppendMenuW(file,MF_STRING|MF_GRAYED,103,L"Disabled");AppendMenuW(file,MF_STRING|MF_CHECKED,104,L"Checked");AppendMenuW(nested,MF_STRING,105,L"Nested action");AppendMenuW(file,MF_POPUP,(UINT_PTR)nested,L"Nested");AppendMenuW(file,MF_SEPARATOR,0,nullptr);AppendMenuW(file,MF_STRING,102,L"E&xit");AppendMenuW(edit,MF_STRING,106,L"&Copy");AppendMenuW(bar,MF_POPUP,(UINT_PTR)file,L"&File");AppendMenuW(bar,MF_POPUP,(UINT_PTR)edit,L"&Edit");
  HWND w=CreateWindowW(c.lpszClassName,L"Liquid native menu test",WS_OVERLAPPEDWINDOW,100,100,450,180,nullptr,bar,h,nullptr);if(!w)return 2;ShowWindow(w,SW_SHOW);SetTimer(w,1,120000,nullptr);MSG m;while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}if(notified)CloseHandle(notified);return 0;
 }
 std::wstring event=L"Local\\LiquidNativeMenuTest4bd7-"+std::to_wstring(GetCurrentProcessId());HANDLE signal=CreateEventW(nullptr,TRUE,FALSE,event.c_str());
 wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,ARRAYSIZE(path));std::wstring cmd=L"\""+std::wstring(path)+L"\" --child \""+event+L"\"";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};if(!CreateProcessW(path,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))return 3;CloseHandle(pi.hThread);
 struct Lookup {DWORD pid; HWND found;};Lookup lookup{pi.dwProcessId,nullptr};
 for(int i=0;i<40&&!lookup.found;++i){Sleep(100);EnumWindows([](HWND h,LPARAM value)->BOOL{auto* ctx=reinterpret_cast<Lookup*>(value);DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[80]{};GetClassNameW(h,cls,ARRAYSIZE(cls));if(pid==ctx->pid&&std::wstring(cls)==L"LiquidNativeMenuTest4bd7"){ctx->found=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&lookup));}
 HWND w=lookup.found;if(!w)return 4;
 auto menus=ReadNativeMenus(w);if(menus.size()!=2||menus[0].title!=L"File"||menus[1].title!=L"Edit"||menus[0].items.size()!=6){std::printf("FAIL read menus=%zu\n",menus.size());PostMessageW(w,WM_CLOSE,0,0);return 5;}
 const auto& f=menus[0].items;if(f[0].text!=L"New"||f[1].enabled||!f[2].checked||f[3].children.size()!=1||f[4].kind!=ItemKind::Separator||menus[1].items[0].text!=L"Copy"){puts("FAIL structure");PostMessageW(w,WM_CLOSE,0,0);return 6;}
 if(DispatchNativeCommand(menus[0],f[1])){puts("FAIL disabled");PostMessageW(w,WM_CLOSE,0,0);return 7;}
 auto fake=f[0];auto wrong=menus[0];wrong.targetPid++;if(DispatchNativeCommand(wrong,fake)){puts("FAIL pid");PostMessageW(w,WM_CLOSE,0,0);return 8;}
 if(!DispatchNativeCommand(menus[0],f[0])||WaitForSingleObject(signal,3000)!=WAIT_OBJECT_0){puts("FAIL new command");PostMessageW(w,WM_CLOSE,0,0);return 9;}
 if(!DispatchNativeCommand(menus[0],f[5])||WaitForSingleObject(pi.hProcess,3000)!=WAIT_OBJECT_0){puts("FAIL exit command");PostMessageW(w,WM_CLOSE,0,0);return 10;}
 if(DispatchNativeCommand(menus[0],f[0])){puts("FAIL stale");return 11;}
 puts("PASS cross-process File/Edit nested separator checked disabled PID New Exit stale HWND");CloseHandle(pi.hProcess);CloseHandle(signal);return 0;
}
