#include "dock_model.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
int main(){
 // The maximum resting width follows the monitor and the current tile count.
 for (int count : {2, 8, 30, 100}) for (float width : {640.f, 1280.f, 1920.f, 3840.f}) {
  dock::DockModel q;q.SetTiles(std::vector<bool>(count,false));const int maxSize=q.MaxIconSizeForWidth(width);
  q.SetIconSize(static_cast<float>(maxSize));if(q.BaseWidth()>width+0.01f)return 24;
  q.SetIconSize(static_cast<float>(maxSize+1));if(q.BaseWidth()<=width)return 25;
 }
 // When magnification makes the strip wider than the screen, it shifts towards the
 // focused end while keeping the full magnification and the focused icon visible.
 for (float width : {640.f, 1920.f, 3840.f}) for (int count : {5, 15, 30}) {
  for(int target : {0, count/2, count-1}) {
   dock::DockModel q;q.SetTiles(std::vector<bool>(count,false));
   q.SetIconSize(static_cast<float>(q.MaxIconSizeForWidth(width)));q.SetWindowSize(width,500);
   dock::DockFrame rest;q.Compute(rest,0);const float x=rest.items[target].cx,y=(rest.dockTop+rest.dockBottom)*0.5f;
   q.SetPointer(true,x,y);for(int i=1;i<=480;++i)q.Update(1.0/240.0,i/240.0);
   dock::DockFrame f;q.Compute(f,2);const auto& icon=f.items[target];
   if(icon.size < rest.items[target].size*2.2f || icon.left < -0.02f || icon.left+icon.size > width+0.02f)return 26;
   if(target==0 && f.dockLeft < -0.02f)return 27;
   if(target==count-1 && f.dockRight > width+0.02f)return 28;
  }
 }
 // Fast end-to-end moves and reversals must translate continuously, then settle at the edge.
 {dock::DockModel q;q.SetTiles(std::vector<bool>(15,false));q.SetIconSize(static_cast<float>(q.MaxIconSizeForWidth(1200)));q.SetWindowSize(1200,500);
  dock::DockFrame rest;q.Compute(rest,0);double t=0;float previous=rest.dockLeft;
  for(int target : {14,0,14,0}) {
   q.SetPointer(true,rest.items[target].cx,(rest.dockTop+rest.dockBottom)*0.5f);
   dock::DockFrame immediate;q.Compute(immediate,t);if(std::fabs(immediate.dockLeft-previous)>0.02f)return 29;
   for(int k=0;k<240;++k){t+=1.0/240.0;q.Update(1.0/240.0,t);dock::DockFrame f;q.Compute(f,t);
    if(std::fabs(f.dockLeft-previous)>35.f)return 30;previous=f.dockLeft;}
  }
 }
 // The settled edge translation changes continuously throughout a slow centre-to-end sweep.
 {dock::DockModel q;q.SetTiles(std::vector<bool>(15,false));q.SetIconSize(static_cast<float>(q.MaxIconSizeForWidth(1200)));q.SetWindowSize(1200,500);
  dock::DockFrame rest;q.Compute(rest,0);double t=0;float previous=0;bool havePrevious=false;
  for(int step=0;step<=100;++step){const float x=rest.items[7].cx+(rest.items[14].cx-rest.items[7].cx)*step/100.f;
   q.SetPointer(true,x,(rest.dockTop+rest.dockBottom)*0.5f);
   for(int k=0;k<240;++k){t+=1.0/240.0;q.Update(1.0/240.0,t);}
   dock::DockFrame f;q.Compute(f,t);if(havePrevious && std::fabs(f.dockLeft-previous)>15.f)return 31;
   previous=f.dockLeft;havePrevious=true;
  }
 }
 // Resizing uses a fixed canvas and must match a freshly laid-out model at every size.
 {dock::DockModel live;live.SetTiles({false,false,true,false,true});live.SetWindowSize(2000,500);
  for(float size : {80.f,24.f,46.f,73.f,32.f}) {
   live.SetIconSize(size);dock::DockModel fresh;fresh.m.SetIconSize(size);fresh.SetTiles({false,false,true,false,true});fresh.SetWindowSize(2000,500);
   dock::DockFrame a,b;live.Compute(a,0);fresh.Compute(b,0);
   if(std::fabs(live.MaxExtra()-fresh.MaxExtra())>0.02f || std::fabs(a.dockLeft-b.dockLeft)>0.02f ||
      std::fabs((a.dockLeft+a.dockRight)*0.5f-1000.f)>0.02f || std::fabs(a.dockBottom-494.5f)>0.02f)return 22;
   for(size_t j=0;j<a.items.size();++j)if(std::fabs(a.items[j].cx-b.items[j].cx)>0.02f)return 23;
  }
 }
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
 // Width factors (the status group): all 1 must reproduce the square-icon layout exactly, at rest and while magnified.
 {std::vector<bool> tiles(12,false);tiles[6]=true;dock::DockModel a,b;a.SetTiles(tiles);b.SetTiles(tiles,std::vector<float>(12,1.0f));
  for(auto* q:{&a,&b})q->SetWindowSize(q->DesiredWindowWidth(),q->DesiredWindowHeight());
  dock::DockFrame fa,fb;a.Compute(fa,0);b.Compute(fb,0);const float y=(fa.dockTop+fa.dockBottom)/2;
  for(auto* q:{&a,&b}){q->SetPointer(true,fa.items[3].cx,y);double t=0;for(int i=0;i<240;++i){t+=1/240.0;q->Update(1/240.0,t);}}
  dock::DockFrame ma,mb;a.Compute(ma,1);b.Compute(mb,1);
  for(size_t i=0;i<12;++i){const auto &p=fa.items[i],&q=fb.items[i],&r=ma.items[i],&s=mb.items[i];
   if(p.left!=q.left||p.size!=q.size||p.tileLeft!=q.tileLeft||r.left!=s.left||r.size!=s.size||r.tileRight!=s.tileRight)return 30;
   if(!p.separator&&p.width!=p.size)return 31;}}
 // A 0.5 and a 1.5 wide tile: bodies are factor x icon wide and icon tall, the gaps between bodies stay the icon gap,
 // and a wide tile magnifies as a whole about its centre.
 {std::vector<bool> tiles(6,false);std::vector<float> w{1,1,0.5f,1,1.5f,1};dock::DockModel q;q.SetTiles(tiles,w);q.SetWindowSize(q.DesiredWindowWidth(),q.DesiredWindowHeight());
  dock::DockFrame f;q.Compute(f,0);const float icon=q.m.icon,gap=q.m.Gap();
  if(std::fabs(f.items[2].width-0.5f*icon)>0.01f||std::fabs(f.items[4].width-1.5f*icon)>0.01f||std::fabs(f.items[4].size-icon)>0.01f)return 32;
  for(int i=0;i<5;++i){const float between=f.items[i+1].left-(f.items[i].left+f.items[i].width);if(std::fabs(between-gap)>0.01f)return 33;}
  if(std::fabs(q.BaseWidth()-(2*q.m.padX+5*q.m.pitch+(0.5f*icon+gap)+(1.5f*icon+gap)-q.m.pitch))>0.01f)return 34;
  q.SetPointer(true,f.items[4].cx,(f.dockTop+f.dockBottom)/2);double t=0;for(int i=0;i<480;++i){t+=1/240.0;q.Update(1/240.0,t);}
  dock::DockFrame m;q.Compute(m,t);const auto& g=m.items[4];
  if(g.size<icon*2.0f||std::fabs(g.width-1.5f*g.size)>0.01f||std::fabs(g.cx-(g.left+g.width/2))>0.01f)return 35;
  for(int i=0;i<5;++i)if(m.items[i+1].left<m.items[i].left+m.items[i].width-0.01f)return 36;}  // no overlap while magnified
 std::puts("PASS geometry, magnification, adjacency, restore, reduced motion, interrupted hover, bounce, separator crossing and width factors");return 0;
}
