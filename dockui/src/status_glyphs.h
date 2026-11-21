#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <memory>
namespace docky::statusart {
inline void Wifi(Gdiplus::Graphics& g,float cx,float cy,float k,Gdiplus::Color color){
 using namespace Gdiplus;g.SetSmoothingMode(SmoothingModeAntiAlias);Pen pen(color,1.7f*k);
 pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
 const float by=cy+5.f*k;for(float rad:{4.2f,7.6f,11.f}){const float r=rad*k;g.DrawArc(&pen,cx-r,by-r,2*r,2*r,225.f,90.f);}
 SolidBrush brush(color);g.FillEllipse(&brush,cx-1.5f*k,by-1.5f*k,3.f*k,3.f*k);
}
// Bluetooth rune: a vertical stem with two arrowheads on the right, crossed by the two diagonals (15 x 7.6 DIP);
// round caps and joins, 1.65 DIP stroke.
inline void Bluetooth(Gdiplus::Graphics& g,float cx,float cy,float k,Gdiplus::Color color){
 using namespace Gdiplus;g.SetSmoothingMode(SmoothingModeAntiAlias);Pen pen(color,1.65f*k);
 pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
 const float x=cx-0.5f*k,t=cy-7.f*k,b=cy+7.f*k,w=3.8f*k;
 const PointF p[]={{x-w,cy-3.7f*k},{x+w,cy+3.5f*k},{x,b},{x,t},{x+w,cy-3.5f*k},{x-w,cy+3.7f*k}};g.DrawLines(&pen,p,6);
}
inline void Speaker(Gdiplus::Graphics& g,float cx,float cy,float k,Gdiplus::Color color,bool available,bool muted,int volume){
 using namespace Gdiplus;g.SetSmoothingMode(SmoothingModeAntiAlias);
 const float x=cx-3.8f*k,y=cy;PointF p[]={{x-5.f*k,y-2.6f*k},{x-2.2f*k,y-2.6f*k},{x+2.2f*k,y-7.f*k},{x+2.2f*k,y+7.f*k},{x-2.2f*k,y+2.6f*k},{x-5.f*k,y+2.6f*k}};
 GraphicsPath body;for(int i=0;i<6;++i){auto q=p[i],prev=p[(i+5)%6],next=p[(i+1)%6];PointF a(q.X+(prev.X-q.X)*.16f,q.Y+(prev.Y-q.Y)*.16f),b(q.X+(next.X-q.X)*.16f,q.Y+(next.Y-q.Y)*.16f);body.AddBezier(a,q,q,b);}body.CloseFigure();SolidBrush brush(color);g.FillPath(&brush,&body);
 Pen pen(color,1.8f*k);pen.SetStartCap(LineCapRound);pen.SetEndCap(LineCapRound);pen.SetLineJoin(LineJoinRound);
 if(available&&!muted){const int waves=volume<=0?0:volume<34?1:volume<67?2:3;for(int i=0;i<waves;++i){const float r=(5.f+3.4f*i)*k;g.DrawArc(&pen,x+1.f*k-r,y-r,2*r,2*r,-48.f,96.f);}}
 if(available&&muted){Pen cut(Color(255,0,0,0),3.2f*k);cut.SetStartCap(LineCapRound);cut.SetEndCap(LineCapRound);g.DrawLine(&cut,x-6.f*k,y-6.5f*k,x+8.f*k,y+6.5f*k);g.DrawLine(&pen,x-6.f*k,y-6.5f*k,x+8.f*k,y+6.5f*k);}
}
}
