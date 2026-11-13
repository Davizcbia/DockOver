#include "launchpad_density.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace dock::launchpad;
int main(){
 DensityConfig c;auto d=PlanDensity(2560,1440,c);if(d.columns!=7||d.rows!=5||d.capacity()!=35)return 1;
 if(DensityPageCount(0,d)!=0||DensityPageCount(35,d)!=1||DensityPageCount(36,d)!=2)return 2;
 c.preset=DensityPreset::Compact;auto compact=PlanDensity(2560,1440,c);
 c.preset=DensityPreset::Spacious;auto spacious=PlanDensity(2560,1440,c);
 if(compact.capacity()<=spacious.capacity())return 3;
 c.preset=DensityPreset::Custom;c.icon=std::numeric_limits<float>::quiet_NaN();c.gapX=-100;c.gapY=1e9f;
 auto custom=PlanDensity(1280,720,c);if(!std::isfinite(custom.icon)||custom.columns<1||custom.rows<1||custom.width>1280*.8f+.01f||custom.height>720*.64f+.01f)return 4;
 for(auto w:{1.f,200.f,640.f,1920.f})for(auto h:{1.f,100.f,480.f,1080.f}){auto g=PlanDensity(w,h,c);if(g.columns<1||g.rows<1||g.icon<=0||g.width>w*.8f+1.01f||g.height>h*.64f+1.01f)return 5;}
 // Every stable logical index must map to exactly one in-range cell for each preset and page size.
 for(auto preset:{DensityPreset::Compact,DensityPreset::Default,DensityPreset::Spacious,DensityPreset::Custom}){
  c.preset=preset;c.icon=104;c.gapX=64;c.gapY=48;auto g=PlanDensity(1920,1080,c);const size_t cap=g.capacity();
  for(size_t n:{size_t(1),cap,cap+1,cap*3+7}){size_t pages=DensityPageCount(n,g);if(pages!=(n/cap+(n%cap!=0)))return 6;
   for(size_t i=0;i<n;++i){size_t page=i/cap,within=i%cap,row=within/static_cast<size_t>(g.columns),col=within%static_cast<size_t>(g.columns);
    if(page>=pages||row>=static_cast<size_t>(g.rows)||col>=static_cast<size_t>(g.columns)||page*cap+row*g.columns+col!=i)return 7;}}
 }
 std::puts("PASS density defaults, presets, custom clamps, stable pagination and tiny screens");return 0;
}
