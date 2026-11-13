#pragma once
#include <windows.h>
#include <algorithm>
namespace docky::geometry {
// Input/output are physical screen pixels. DPI conversion belongs to the caller.
inline RECT DockOnMonitor(const RECT& monitor,int width,int height){
 const LONG x=monitor.left+(monitor.right-monitor.left-width)/2;
 const LONG y=monitor.bottom-height;return {x,y,x+width,y+height};
}
inline RECT ClampPopup(const RECT& work,int x,int y,int width,int height){
 const int availableW=(std::max)(0,static_cast<int>(work.right-work.left));
 const int availableH=(std::max)(0,static_cast<int>(work.bottom-work.top));
 const int w=std::clamp(width,0,availableW),h=std::clamp(height,0,availableH);
 const int left=std::clamp(x,static_cast<int>(work.left),static_cast<int>(work.right)-w);
 const int top=std::clamp(y,static_cast<int>(work.top),static_cast<int>(work.bottom)-h);
 return {left,top,left+w,top+h};
}
}
