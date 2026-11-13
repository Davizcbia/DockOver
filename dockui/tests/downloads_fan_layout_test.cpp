#include "fan_layout.h"
#include <cstdio>
#include <cmath>
using namespace dock::downloads;
int main(){
 if(FanVisibleCount(0,430)!=0||FanVisibleCount(20,430)!=6||FanVisibleCount(6,150)!=1||FanVisibleCount(6,206)!=2)return 9;
 for(int n=3;n<=FanMax;++n){SetFanOptions(n,false,true);const float H=static_cast<float>(FanPanelHeight());if(FanVisibleCount(50,H)!=n)return 12;
  // the column keeps leaning the same way all the way up (no fold-back) and stays inside the panel
  float px=-1;for(int i=0;i<=n;++i){const auto pl=FanPlace(static_cast<float>(i),n,static_cast<float>(FanPanelWidth),H,static_cast<float>(FanPanelAnchor));if(pl.finalX<=px||pl.finalX>FanPanelWidth-40.f||pl.finalY<FanTopRoom-20.f)return 13;px=pl.finalX;}}
 SetFanOptions(6,false,true);
 if(FanProgress(0,1,0,200)!=0||FanProgress(0,1,200,200)!=1||std::fabs(FanProgress(0,1,100,200)-0.5f)>0.001f)return 1;
 if(std::fabs(FanProgress(0.6f,0,0,180)-0.6f)>0.001f||FanProgress(0.6f,0,180,180)!=0)return 2;
 for(int count=1;count<=FanLimit;++count)for(float anchor:{30.f,175.f,320.f}){
  float priorY=10000.f;for(int i=0;i<count;++i){
   auto a=FanLayout(i,count,350,430,anchor,0),b=FanLayout(i,count,350,430,anchor,1);
   if(b.icon.left<0||b.icon.right>350||b.icon.top<40||b.icon.bottom>430||b.label.left<0||b.label.right>350||b.label.right<b.label.left)return 3;
   if(std::fabs((b.icon.right-b.icon.left)-32.f)>0.01f)return 4;
   float y=(b.icon.top+b.icon.bottom)*0.5f;if(i&&priorY-y<32.f)return 5;priorY=y;
   if(!FanContains(b.icon,(b.icon.left+b.icon.right)/2,y))return 6;
   if(std::fabs((a.icon.left+a.icon.right)*0.5f-std::clamp(anchor,20.f,330.f))>0.01f)return 7;
  }
 }
 for(float height:{150.f,206.f,430.f}){
  const int n=FanVisibleCount(20,height);float prior=height;
  for(int i=0;i<n;++i){const auto g=FanLayout(i,n,350,height,30,1);if(g.icon.top<48.f||g.icon.bottom>height-54.f)return 10;
   if(i&&prior-g.icon.bottom<4.f)return 11;prior=g.icon.top;}
 }
 auto low=FanLayout(0,6,350,430,175,1),high=FanLayout(5,6,350,430,175,1);
 if(std::fabs(high.icon.left-low.icon.left)<20.f)return 8;
 std::puts("PASS fan geometry: counts 1-6, screen-edge anchors, non-overlap, motion endpoints and hit test");return 0;
}
