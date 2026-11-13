#include "menu_hit_region.h"
#include <windows.h>
#include <cstdio>
#include <vector>
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int wmain(){
 for(UINT dpi:{96u,144u,192u}){const int height=MulDiv(30,dpi,96),width=MulDiv(180,dpi,96);RECT a{MulDiv(10,dpi,96),0,MulDiv(60,dpi,96),height},b{MulDiv(95,dpi,96),0,MulDiv(128,dpi,96),height};std::vector<RECT> slots{a,b};
  HRGN region=docky::menuhit::CreateRegion(slots,width,height,dpi);if(!region)return 1;
  for(int y=0;y<height;++y)for(int x=0;x<width;++x){bool wanted=false;for(auto rc:slots)wanted|=docky::menuhit::Coverage(rc,height,dpi,x,y)>0.f;if((PtInRegion(region,x,y)!=FALSE)!=wanted){DeleteObject(region);return 2;}}
  DeleteObject(region);
 }
 WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"docky-test-transparent-input-v13";if(!RegisterClassW(&wc))return 3;
 HWND base=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"docky isolated input test",WS_POPUP,100,100,240,100,nullptr,nullptr,wc.hInstance,nullptr);if(!base)return 4;
 HWND overlay=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,100,100,240,100,nullptr,nullptr,wc.hInstance,nullptr);if(!overlay){DestroyWindow(base);return 5;}
 RECT area{40,0,120,30};HRGN r=docky::menuhit::CreateRegion({area},240,100,96);if(!r||!SetWindowRgn(overlay,r,FALSE)){if(r)DeleteObject(r);DestroyWindow(overlay);DestroyWindow(base);return 6;}
 ShowWindow(base,SW_SHOWNOACTIVATE);ShowWindow(overlay,SW_SHOWNOACTIVATE);SetWindowPos(overlay,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);RECT wr{};GetWindowRect(overlay,&wr);
 POINT inside{wr.left+80,wr.top+15},outside{wr.left+180,wr.top+15};HWND a=WindowFromPoint(inside),b=WindowFromPoint(outside);
 bool ok=a==overlay&&b==base;std::printf("transparent input host inside=%d outside_fixture=%d\n",a==overlay,b==base);DestroyWindow(overlay);DestroyWindow(base);
 if(!ok)return 7;std::puts("PASS 96/144/192 pill pixel parity and invisible non-layered input window routing");return 0;
}
