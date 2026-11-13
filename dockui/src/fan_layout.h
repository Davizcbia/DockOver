#pragma once
// Device-independent geometry and animation for the Downloads Fan.
//
// V0.20 (user reference, macOS Downloads fan): rows about 58 DIP apart; the column leans steadily away from the Dock
// icon and leans more the higher it goes (x = 5 p + 0.5 p|p|, p = row), never folding back; each row is tilted along
// the column's tangent; an arrow button ("more in Explorer") sits one row above the top file.
#include <algorithm>
#include <cmath>
namespace dock::downloads {
inline constexpr int FanMax=10;
// Settings > Dock > Downloads: files shown (3..10), straight-up fan, file previews. Set by the Dock from settings.json.
inline int FanLimit=6;
inline bool FanVertical=false;
inline bool FanPreview=true;
inline void SetFanOptions(int count,bool vertical,bool preview){FanLimit=std::clamp(count,3,FanMax);FanVertical=vertical;FanPreview=preview;}
inline constexpr float FanStepMax=58.f,FanStepMin=40.f,FanBottom=70.f,FanTopRoom=40.f;
// Panel size (DIP): FanLimit rows plus the arrow row above the Dock; wide enough that the curve never reaches an edge.
inline int FanPanelHeight(){return static_cast<int>(FanBottom+FanTopRoom+FanLimit*FanStepMax+20.f);}
inline constexpr int FanPanelWidth=440,FanPanelAnchor=270;  // anchor = Dock icon centre from the panel's left edge
struct FanRect {float left=0,top=0,right=0,bottom=0;};
struct FanItemGeometry {FanRect icon,label;};
// Limit the visible subset on short work areas; the arrow button reaches the remainder.
inline int FanVisibleCount(int total,float height){
 if(total<=0||height<100.f)return 0;
 const int fit=(std::max)(1,static_cast<int>(std::floor((height-FanBottom-FanTopRoom)/FanStepMin)));
 return (std::min)({total,FanLimit,fit});
}
// Row spacing: as close to FanStepMax as the height allows with `count` rows plus the arrow row.
inline float FanStep(int count,float height){return std::clamp((height-FanBottom-FanTopRoom)/static_cast<float>((std::max)(1,count)),FanStepMin,FanStepMax);}
// Sideways offset of row `place` from the Dock icon, and the row tilt (degrees) along the column tangent.
inline float FanCurve(float place){return FanVertical?0.f:5.f*place+0.5f*place*std::fabs(place);}
inline float FanTilt(float place,bool leftLabel){
 if(FanVertical)return 0.f;
 const float slope=(5.f+std::fabs(place))/FanStepMax;
 return (leftLabel?1.f:-1.f)*std::atan(slope)*180.f/3.14159265f;
}
inline float FanEase(float t){t=std::clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);}
// Linear panel progress; per-item easing (stagger + overshoot) happens in FanLayout.
inline float FanProgress(float from,float to,float elapsedMs,float durationMs){
 return from+(to-from)*std::clamp(durationMs>0?elapsedMs/durationMs:1.f,0.f,1.f);
}
// Behaviour modelled on the user's Launcher instance fan (design only, no code reused): each row starts
// 8% of the run later than the one below, then settles with a light OutBack overshoot (c=1.4).
// Closing runs the same curve backwards, so the top row folds away first.
inline float FanItemLocal(float progress,int index,int count){return std::clamp(progress*(1.f+0.08f*count)-0.08f*index,0.f,1.f);}
inline float FanItemEase(float progress,int index,int count){
 const float local=FanItemLocal(progress,index,count);
 constexpr float c=1.4f;const float t=local-1.f;
 return 1.f+(c+1.f)*t*t*t+c*t*t;
}
// Final (fully open) centre of a row at a possibly fractional place, plus the closed start point.
struct FanPlacement {float finalX=0,finalY=0,startX=0,startY=0;bool labelLeft=true;};
inline FanPlacement FanPlace(float place,int count,float width,float height,float anchorX){
 FanPlacement r;count=std::clamp(count,1,FanMax);
 constexpr float half=16.f;
 anchorX=std::clamp(anchorX,half+4.f,width-half-4.f);
 const float step=FanStep(count,height);
 // Lean away from the nearer panel edge (the panel is placed so this is normally to the right).
 const float direction=anchorX+FanCurve(static_cast<float>(count))+30.f<=width?1.f:-1.f;
 r.finalX=std::clamp(anchorX+direction*FanCurve(place),half+4.f,width-half-4.f);
 r.finalY=height-FanBottom-place*step;
 r.startX=anchorX;r.startY=std::max(half,height-16.f);
 r.labelLeft=direction>0?anchorX>=150.f:anchorX>width-150.f;
 return r;
}
inline FanItemGeometry FanLayout(int index,int count,float width,float height,float anchorX,float progress){
 if(width<64.f||height<100.f||count<=0||index<0)return {};
 count=std::clamp(count,1,FanMax);index=std::clamp(index,0,count-1);progress=std::clamp(progress,0.f,1.f);
 constexpr float half=16.f;
 const FanPlacement pl=FanPlace(static_cast<float>(index),count,width,height,anchorX);
 const float e=FanItemEase(progress,index,count);
 const float x=pl.startX+(pl.finalX-pl.startX)*e,y=std::max(half,pl.startY+(pl.finalY-pl.startY)*e);
 FanItemGeometry g;g.icon={x-half,y-half,x+half,y+half};
 if(pl.labelLeft)g.label={std::max(4.f,x-half-190.f),y-14.f,std::max(4.f,x-half-6.f),y+14.f};
 else g.label={std::min(width-4.f,x+half+6.f),y-14.f,std::min(width-4.f,x+half+190.f),y+14.f};
 g.label.top=std::max(0.f,g.label.top);g.label.bottom=std::min(height,g.label.bottom);
 return g;
}
inline bool FanContains(const FanRect& r,float x,float y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
}
