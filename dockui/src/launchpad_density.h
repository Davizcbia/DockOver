#pragma once
// Pure density planning only; Launchpad's persisted layout and runtime are unchanged until integrated.
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace dock::launchpad {
enum class DensityPreset { Compact, Default, Spacious, Custom };
struct DensityConfig { DensityPreset preset=DensityPreset::Default; float icon=96.f,gapX=80.f,gapY=48.f; };
struct DensityGrid { int columns=1,rows=1; float icon=1.f,pitchX=1.f,pitchY=1.f,width=1.f,height=1.f; size_t capacity()const{return static_cast<size_t>(columns)*rows;} };
inline float FiniteClamp(float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;}
inline DensityGrid PlanDensity(float width,float height,DensityConfig cfg){
 width=FiniteClamp(width,1.f,1.f,100000.f);height=FiniteClamp(height,1.f,1.f,100000.f);
 const float aw=(std::max)(1.f,width*.80f),ah=(std::max)(1.f,height*.90f);
 // Every preset derives from the user's existing 7x5 default proportions.
 const float base=std::clamp((std::min)(width*.040f,height*.071f),56.f,120.f);
 float icon=base,colsWanted=7,rowsWanted=5;
 if(cfg.preset==DensityPreset::Compact){icon=base*.85f;colsWanted=9;rowsWanted=6;}
 else if(cfg.preset==DensityPreset::Spacious){icon=base*1.1f;colsWanted=6;rowsWanted=4;}
 icon=(std::max)(1.f,(std::min)({icon,aw,ah}));
 float px=(std::max)(icon+24.f,width*.0983f*7.f/colsWanted),py=(std::max)(icon+60.f,height*(cfg.preset==DensityPreset::Compact?.14f:cfg.preset==DensityPreset::Spacious?.22f:.176f));
 int cols=static_cast<int>(colsWanted),rows=static_cast<int>(rowsWanted);
 if(cfg.preset==DensityPreset::Custom){icon=FiniteClamp(cfg.icon,96.f,32.f,160.f);icon=(std::min)({icon,aw,ah});px=icon+FiniteClamp(cfg.gapX,80.f,0.f,240.f)+24.f;py=icon+60.f+FiniteClamp(cfg.gapY,48.f,0.f,240.f);cols=std::clamp(static_cast<int>(aw/px),1,12);rows=std::clamp(static_cast<int>(ah/py),1,10);}
 // Keep columns/cells inside the display without changing saved logical order.
 cols=(std::min)(cols,(std::max)(1,static_cast<int>(aw/px)));rows=(std::min)(rows,(std::max)(1,static_cast<int>(ah/py)));
 px=(std::min)(px,aw);py=(std::min)(py,ah);icon=(std::max)(1.f,(std::min)({icon,px-24.f,py-60.f}));

 return {cols,rows,icon,px,py,px*cols,py*rows};
}
inline size_t DensityPageCount(size_t items,const DensityGrid& g){const auto cap=g.capacity();return cap?(items/cap+(items%cap!=0)):0;}
}
