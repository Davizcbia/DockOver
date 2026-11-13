#include "dock_model.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
int main(){
 dock::DockModel m;m.SetTiles({false,false,false,true,false,true});
 m.SetWindowSize(m.DesiredWindowWidth(),m.DesiredWindowHeight());
 dock::DockFrame rest;m.Compute(rest,0);
 if(rest.items.size()!=6||std::fabs((rest.dockBottom-rest.dockTop)-76.f)>0.01f)return 1;
 for(const auto& g:rest.items)if(!g.separator&&(std::fabs(g.size-46.f)>0.01f||std::fabs(g.top+g.size-(rest.dockBottom-15.f))>0.01f))return 2;
 if(!rest.items[3].separator||!rest.items[5].separator||std::fabs(rest.items[3].lineBottom-rest.items[3].lineTop-56.f)>0.01f)return 3;
 const float x=rest.items[1].cx,y=(rest.dockTop+rest.dockBottom)/2;
 m.SetPointer(true,x,y);for(int i=1;i<=240;++i)m.Update(1.0/240.0,i/240.0);
 dock::DockFrame hover;m.Compute(hover,1);
 if(hover.items[1].size<=rest.items[1].size||hover.items[0].size<=rest.items[0].size||hover.items[3].size!=0)return 4;
 for(size_t i=1;i<hover.items.size();++i)if(hover.items[i].tileLeft<hover.items[i-1].tileRight-0.01f)return 5;
 if(m.HitTest(hover,hover.items[1].cx,y)!=1)return 6;
 m.SetPointer(false,0,0);for(int i=1;i<=480;++i)m.Update(1.0/240.0,1+i/240.0);
 dock::DockFrame end;m.Compute(end,3);if(std::fabs(end.items[1].size-rest.items[1].size)>0.01f)return 7;
 dock::DockModel initiallyReduced; initiallyReduced.SetReducedMotion(true); initiallyReduced.SetTiles({false,false,false,true,false,true});
 if(std::fabs(initiallyReduced.DesiredWindowWidth()-m.DesiredWindowWidth())>0.01f)return 10;
 m.SetReducedMotion(true); m.SetPointer(true,x,y); m.StartBounce(1,4.0); m.Update(1.0/240.0,4.2);
 dock::DockFrame reduced;m.Compute(reduced,4.2);
 if(!m.PointerActive() || std::fabs(reduced.items[1].size-rest.items[1].size)>0.01f || std::fabs(reduced.items[1].top-rest.items[1].top)>0.01f || m.IsAnimating(4.2))return 8;
 m.SetReducedMotion(false); m.StartBounce(1,5.0); if(!m.IsAnimating(5.1))return 9;
 // Interruption: enter, leave mid-ramp, re-enter. Magnification must stay continuous (no frame-to-frame
 // jump larger than the spring allows) and never leave 0..1, and the icon must end magnified again.
 {dock::DockModel q;q.SetTiles({false,false,false,true,false,true});q.SetWindowSize(q.DesiredWindowWidth(),q.DesiredWindowHeight());
  dock::DockFrame f0;q.Compute(f0,0);const float cx=f0.items[1].cx,cy=(f0.dockTop+f0.dockBottom)/2;
  float prev=0,maxJump=0;double t=0;
  auto run=[&](int frames){for(int i=0;i<frames;++i){t+=1.0/240.0;q.Update(1.0/240.0,t);const float h=q.Hover();
    if(h<-1e-4f||h>1.0f+1e-4f)return false;maxJump=std::max(maxJump,std::fabs(h-prev));prev=h;}return true;};
  q.SetPointer(true,cx,cy);if(!run(20))return 11;      // rising
  const float mid=q.Hover();if(mid<=0.05f||mid>=0.99f)return 12;
  q.SetPointer(false,0,0);if(!run(10))return 13;       // leave mid-ramp
  q.SetPointer(true,cx,cy);if(!run(10))return 14;      // re-enter before settling
  if(!run(240))return 15;
  if(q.Hover()<0.999f)return 16;
  if(maxJump>0.06f)return 17;                           // physics bound: peak slope w/e = 11.6 /s -> 0.048 per 1/240 s; a pop would be far larger
  // Launch bounce: height stays within [0, apex+eps], decays to rest, and is interruptible.
  q.StartBounce(1,t);float peak=0;bool ended=false;
  for(int i=0;i<480;++i){t+=1.0/240.0;q.Update(1.0/240.0,t);dock::DockFrame f;q.Compute(f,t);
   const float lift=f.items[1].restTop-f.items[1].top;if(lift<-1e-3f||lift>q.m.bounceHeight+0.5f)return 18;peak=std::max(peak,lift);if(!q.IsAnimating(t)&&i>20){ended=true;break;}}
  if(peak<q.m.bounceHeight*0.9f||!ended)return 19;
  q.StartBounce(1,t);for(int i=0;i<40;++i){t+=1.0/240.0;q.Update(1.0/240.0,t);}
  dock::DockFrame fa;q.Compute(fa,t);const float before=fa.items[1].restTop-fa.items[1].top;
  q.StartBounce(1,t);dock::DockFrame fb;q.Compute(fb,t);const float after=fb.items[1].restTop-fb.items[1].top;
  if(std::fabs(before-after)>1e-3f)return 20;           // relaunch mid-air: no positional jump
 }
 // Crossing a separator must not slide the Dock sideways (it read as the Dock shrinking): sweep the pointer over a
 // separator mid-dock at 600 DIP/s and require the left edge to stay within 9 DIP (22 DIP before edge anchoring; 20% of the gap is kept so the Dock never freezes).
 {std::vector<bool> tiles(17,false);tiles[8]=true;dock::DockModel s;s.SetTiles(tiles);s.SetWindowSize(s.DesiredWindowWidth(),s.DesiredWindowHeight());
  dock::DockFrame f0;s.Compute(f0,0);const float sy=(f0.dockTop+f0.dockBottom)/2,a=f0.items[5].cx,b=f0.items[11].cx;
  s.SetPointer(true,a,sy);double t=0;for(int i=0;i<480;++i){t+=1/240.0;s.Update(1/240.0,t);}
  float lo=1e9f,hi=-1e9f;for(float px=a;px<b;px+=2.5f){s.SetPointer(true,px,sy);t+=1/240.0;s.Update(1/240.0,t);dock::DockFrame g;s.Compute(g,t);lo=std::min(lo,g.dockLeft);hi=std::max(hi,g.dockLeft);}
  std::printf("separator sweep: left edge range %.2f DIP\n",hi-lo);if(hi-lo>9.0f)return 21;
  // ...and the Dock must keep responding while the pointer is over the separator itself (it used to freeze there).
  const float sl=f0.items[8].tileLeft,sr=f0.items[8].tileRight;dock::DockFrame g1,g2;
  s.SetPointer(true,sl+2,sy);for(int i=0;i<240;++i){t+=1/240.0;s.Update(1/240.0,t);}s.Compute(g1,t);
  s.SetPointer(true,sr-2,sy);for(int i=0;i<240;++i){t+=1/240.0;s.Update(1/240.0,t);}s.Compute(g2,t);
  if(std::fabs(g1.items[7].size-g2.items[7].size)<0.5f||std::fabs(g1.items[9].size-g2.items[9].size)<0.5f)return 22;}
 std::puts("PASS geometry, magnification, adjacency, restore, reduced motion, interrupted hover, bounce and separator crossing");return 0;
}
