#include "display_geometry.h"
#include <cstdio>
using namespace docky::geometry;
int main(){
 RECT monitor{-1920,-120,0,960};
 for(int dpi:{96,144,192}){const int w=MulDiv(600,dpi,96),h=MulDiv(196,dpi,96);auto r=DockOnMonitor(monitor,w,h);
  if(r.bottom!=monitor.bottom||r.right-r.left!=w||r.bottom-r.top!=h||r.left!=(monitor.left+(monitor.right-monitor.left-w)/2))return 1;}
 RECT work{-1920,-80,0,900};auto r=ClampPopup(work,-30,890,390,600);
 if(r.left!=-390||r.right!=0||r.bottom!=900||r.top!=300)return 2;
 r=ClampPopup(work,-4000,-500,4000,4000);if(!EqualRect(&r,&work))return 3;
 RECT second{0,0,2560,1440};auto a=DockOnMonitor(second,900,196);RECT workWithTaskbar{0,0,2560,1392};
 (void)workWithTaskbar;auto b=DockOnMonitor(second,900,196);if(!EqualRect(&a,&b)||a.bottom!=1440)return 4;
 std::puts("PASS monitor-relative Dock and clipped popup physical coordinates at 100/150/200% DPI");return 0;
}
