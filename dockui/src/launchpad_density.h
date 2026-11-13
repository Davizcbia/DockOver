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
 const float aw=(std::max)(1.f,width*.80f),ah=(std::max)(1.f,height*.64f);
 float base=std::clamp((std::min)(width*.042f,height*.075f),56.f,104.f),icon=base,gx=base*1.1f,gy=(std::max)(0.f,ah/5.f-base-26.f);
 if(cfg.preset==DensityPreset::Compact){icon=base*.85f;gx=base*.65f;gy=base*.30f;}
 else if(cfg.preset==DensityPreset::Spacious){icon=base*1.1f;gx=base*1.4f;gy=base*.75f;}
 else if(cfg.preset==DensityPreset::Custom){icon=FiniteClamp(cfg.icon,96.f,32.f,160.f);gx=FiniteClamp(cfg.gapX,80.f,0.f,240.f);gy=FiniteClamp(cfg.gapY,48.f,0.f,240.f);}
 // Fit a single icon even in a very small available area; never report negative sizes.
 icon=(std::max)(1.f,(std::min)({icon,aw,ah}));
 float px=icon+gx,py=icon+26.f+gy;
 if(cfg.preset==DensityPreset::Default){px=(std::max)(icon,aw/7.f);py=(std::max)(icon+26.f,ah/5.f);}
 px=(std::max)(1.f,px);py=(std::max)(1.f,py);
 int cols=std::clamp(static_cast<int>((aw+0.001f)/px),1,12),rows=std::clamp(static_cast<int>((ah+0.001f)/py),1,10);
 // For pathological small screens, fit the pitch to the viewport rather than overflow.
 px=(std::min)(px,aw);py=(std::min)(py,ah);icon=(std::min)({icon,px,py});
 return {cols,rows,icon,px,py,px*cols,py*rows};
}
inline size_t DensityPageCount(size_t items,const DensityGrid& g){const auto cap=g.capacity();return cap?(items/cap+(items%cap!=0)):0;}
}
