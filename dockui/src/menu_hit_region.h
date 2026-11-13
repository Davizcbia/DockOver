#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>
namespace docky::menuhit {
inline int Px(int dip,UINT dpi){return MulDiv(dip,static_cast<int>(dpi),96);}
// Shared with rendering: subpixel coverage of the visual pill at a physical pixel.
inline float Coverage(const RECT& rc,int height,UINT dpi,int x,int y){
 const float l=static_cast<float>(rc.left+Px(2,dpi)),r=static_cast<float>(rc.right-Px(2,dpi));
 const float t=static_cast<float>(Px(2,dpi)),b=static_cast<float>(height-Px(2,dpi));
 const float radius=(std::min)((b-t)*.5f,(r-l)*.5f);if(radius<=0.f)return 0.f;
 const float fx=x+.5f,fy=y+.5f,qx=(std::max)({l+radius-fx,fx-(r-radius),0.f}),qy=(std::max)({t+radius-fy,fy-(b-radius),0.f});
 return std::clamp(.5f-(std::hypot(qx,qy)-radius),0.f,1.f);
}
// Drawing-only selection capsule: macOS reference (Apple-menu screenshot at 2x: 84 x 48 px) is a 42 x 24 pt
// capsule whose ends are full half-circles (radius = half height), vertically centred. The rect includes
// the 2 px side inset. Hit testing keeps the full-height Coverage() above so the whole bar stays clickable.
inline float PillCoverage(const RECT& rc,int height,UINT dpi,int x,int y){
 const float l=static_cast<float>(rc.left+Px(2,dpi)),r=static_cast<float>(rc.right-Px(2,dpi));
 const float pillH=(std::min)(static_cast<float>(height-2*Px(2,dpi)),static_cast<float>(Px(24,dpi)));
 const float t=(static_cast<float>(height)-pillH)*.5f,b=t+pillH;
 const float radius=(std::min)((b-t)*.5f,(r-l)*.5f);if(radius<=0.f)return 0.f;
 const float fx=x+.5f,fy=y+.5f,qx=(std::max)({l+radius-fx,fx-(r-radius),0.f}),qy=(std::max)({t+radius-fy,fy-(b-radius),0.f});
 return std::clamp(.5f-(std::hypot(qx,qy)-radius),0.f,1.f);
}
// Union of exactly the pixels where the antialiased pill has nonzero coverage.
// Caller owns returned region; SetWindowRgn transfers ownership on success.
inline HRGN CreateRegion(const std::vector<RECT>& slots,int width,int height,UINT dpi){
 HRGN result=CreateRectRgn(0,0,0,0);if(!result)return nullptr;
 for(const auto& rc:slots){int first=(std::max)(0,static_cast<int>(rc.left)),last=(std::min)(width,static_cast<int>(rc.right));
  for(int y=0;y<height;++y){int x=first;while(x<last&&Coverage(rc,height,dpi,x,y)<=0.f)++x;if(x>=last)continue;
   int end=last;while(end>x&&Coverage(rc,height,dpi,end-1,y)<=0.f)--end;
   HRGN row=CreateRectRgn(x,y,end,y+1);if(!row||CombineRgn(result,result,row,RGN_OR)==ERROR){if(row)DeleteObject(row);DeleteObject(result);return nullptr;}DeleteObject(row);
  }}
 return result;
}
}
