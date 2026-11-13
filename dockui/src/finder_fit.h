#pragma once
#include "custom_icons.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace dock {
// Every Dock icon gets the same visible size: the opaque body (alpha >= 50%) is centred and scaled so its
// longer side fills the canvas. Shell icons were already trimmed (alpha > 12) and Workshop / Finder icons
// carried their own canvas padding, so the same icon could look larger or smaller than its neighbours.
// Returns the input unchanged when it already fits within 1.5%. User icon edits are applied afterwards.
inline std::vector<std::uint32_t> NormalizeIconBody(const std::vector<std::uint32_t>& pixels,UINT w,UINT h){
 if(!w||!h||pixels.size()!=static_cast<size_t>(w)*h)return pixels;
 UINT x0=w,y0=h,x1=0,y1=0;bool found=false;
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)if((pixels[static_cast<size_t>(y)*w+x]>>24)>=128){found=true;x0=(std::min)(x0,x);y0=(std::min)(y0,y);x1=(std::max)(x1,x);y1=(std::max)(y1,y);}
 if(!found)return pixels;
 const double side=static_cast<double>((std::max)(w,h));const double body=static_cast<double>((std::max)(x1-x0+1,y1-y0+1));
 const double scale=(std::min)(2.0,side/body);
 const double cx=(x0+x1+1)*.5-w*.5,cy=(y0+y1+1)*.5-h*.5;
 if(scale<=1.015&&std::abs(cx)<=side*0.01&&std::abs(cy)<=side*0.01)return pixels;
 IconEdit e;e.scale=scale;e.offsetX=-cx*scale/side;e.offsetY=-cy*scale/side;e.Clamp();return ApplyIconEdit(pixels,w,h,e);
}
// Kept for callers/tests written against the earlier Finder-only normalisation.
inline std::vector<std::uint32_t> FitFinderBody(const std::vector<std::uint32_t>& pixels,UINT w,UINT h){
 if(!w||!h||pixels.size()!=static_cast<size_t>(w)*h)return pixels;
 UINT x0=w,y0=h,x1=0,y1=0;bool found=false;
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)if((pixels[static_cast<size_t>(y)*w+x]>>24)>=128){found=true;x0=(std::min)(x0,x);y0=(std::min)(y0,y);x1=(std::max)(x1,x);y1=(std::max)(y1,y);}
 if(!found)return pixels;const double side=static_cast<double>((std::max)(w,h));const double body=static_cast<double>((std::max)(x1-x0+1,y1-y0+1));
 const double scale=(std::min)(1.3,0.97*side/body);if(scale<=1.015)return pixels;
 IconEdit e;e.scale=scale;e.offsetX=-(((x0+x1+1)*.5)-w*.5)*scale/side;e.offsetY=-(((y0+y1+1)*.5)-h*.5)*scale/side;e.Clamp();return ApplyIconEdit(pixels,w,h,e);
}
}
